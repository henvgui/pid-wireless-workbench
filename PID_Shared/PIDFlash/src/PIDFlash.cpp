#include "PIDFlash.h"
#include "slave_target_pins.h"
#include "PIDLink.h"
#include "stm32_rom.h"
#include "swd_programmer.h"
#include "echo_target.h"
#include "motor_bridge.h"
#include "serial_log.h"
#include "serial_bridge.h"
#include "serial_tx.h"
#include "bulk_core.h"
#include <Arduino.h>
#include <HardwareSerial.h>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <new>
#include "mbedtls/sha256.h"
#include "esp_heap_caps.h"
#ifndef PID_FLASH_TARGET_ENABLE
#define PID_FLASH_TARGET_ENABLE 0
#endif
#ifndef PID_FLASH_SWD_HALF_PERIOD_US
#define PID_FLASH_SWD_HALF_PERIOD_US 5
#endif
static_assert(PID_FLASH_SWD_HALF_PERIOD_US >= 1 && PID_FLASH_SWD_HALF_PERIOD_US <= 20,
              "SWD half period must be 1..20 microseconds");

namespace pidflash {
namespace {
Packet outgoing,lastRequest,lastReply;
bool pending=false,replyReady=false,locked=false;
uint32_t nextSequence=0,lastSend=0,retries=0,replyRetries=0;
uint8_t attempts=0;
Packet hostReply;
bulk::Sender bulkSender;
bulk::UsbDecoder bulkDecoder;
bulk::UsbFrame bulkUsbFrame;
bool bulkHostActive=false,bulkNotice=false,bulkSlaveActive=false,bulkAckPending=false;
bool bulkUsbQuarantine=false;
uint32_t bulkHostToken=0,bulkHostSession=0,bulkPeerSession=0,bulkLastReport=0;
uint32_t bulkSlaveToken=0,bulkSlaveOwner=0,bulkSlaveBitmap=0,bulkLastAck=0,bulkPackets=0;
Error bulkSlaveError=Error::Ok;
uint32_t bulkDataAttempts=0,bulkDataDrops=0,bulkAckAttempts=0,bulkAckDrops=0,bulkAckDropsSeen=0;
uint8_t bulkDropEvery=0,bulkSlaveDropEvery=0;
Target hostTarget=Target::G431;
Interface hostInterface=Interface::Uart;
uint8_t* image=nullptr;
uint32_t imageCapacity=0;
Staging staging;
uint32_t cachedHost=0,cachedSequence=0;
TaskHandle_t worker=nullptr;
std::atomic<bool> workerDone{false},targetEnabled{false};
std::atomic<uint32_t> progressBytes{0};
std::atomic<uint8_t> progressStage{0};
Error workerError=Error::Ok,lastJobError=Error::Ok;
std::atomic<uint16_t> workerDevice{0};
std::atomic<uint16_t> workerFlashKb{0};
bool runAfterProgram=false;
bool legacySwdProgram=false;
std::atomic<bool> workerUsedRam{false};
std::atomic<bool> goAcknowledged{false};
std::atomic<uint32_t> swdDpidr{0},swdTrace{0};
std::atomic<uint32_t> swdCapacityWord{0},swdOptionBytes{0};
std::atomic<uint32_t> swdOptionBytesAfter{0};
std::atomic<uint8_t> swdRecoveryState{0};
uint32_t swdHoldUntil=0;
bool echoLocked=false,echoActive=false,echoWaiting=false,echoCached=false;
bool uartLocked=false,uartActive=false,uartCached=false;
Packet uartRequest,uartReply;
uint32_t uartStarted=0;
uint32_t uartHostSession=0,uartHighestSequence=0;
Packet echoRequest,echoCachedRequest,echoCachedReply;
HardwareSerial echoUart{1};
EchoParser echoParser={};
uint32_t echoLastActivity=0,echoSentAt=0,echoHighestSequence=0;
uint32_t echoRadioDropEvery=0,echoRadioSends=0,echoRadioDrops=0;
uint32_t echoUartCorruptEvery=0,echoDataSends=0,echoUartRetries=0,echoUartCorruptions=0;
uint8_t echoUartAttempts=0;
LogBuffer<32768>* logBuffer=nullptr;
LogConfig logConfig;
bool logActive=false,logCached=false;
uint32_t logId=0,logOwner=0,logHighest=0;
Packet logRequest,logReply;
std::atomic<uint32_t> logErrors{0};
bool echoOp(Op op){return op==Op::EchoOpen||op==Op::EchoData||op==Op::EchoClose;}

bool sha256(const uint8_t* data,size_t n,uint8_t* hash) { return mbedtls_sha256_ret(data,n,hash,0)==0; }
void progress(uint8_t stage,uint32_t done) {
  progressBytes.store(done);progressStage.store(stage);
  // Bit-banged SWD can otherwise starve the core's idle task throughout a long
  // readback. Yield at each completed chunk, outside all SWD transactions.
  vTaskDelay(1);
}
class TargetUart:public RomIo {
 public:
  HardwareSerial uart{1};
  bool write(const uint8_t* bytes,size_t n) override { return uart.write(bytes,n)==n; }
  int read(uint32_t timeout) override {
    const uint32_t started=millis();
    do { if(uart.available())return uart.read();vTaskDelay(1); } while(uint32_t(millis()-started)<timeout);
    return -1;
  }
};
class TargetSwd:public SwdPins {
 public:
  explicit TargetSwd(uint32_t halfPeriod=PID_FLASH_SWD_HALF_PERIOD_US):halfPeriod_(halfPeriod) {}
  void output(bool toTarget) override {
    if(toTarget){digitalWrite(21,HIGH);pinMode(slavepins::kSwdData,OUTPUT);}
    else {pinMode(slavepins::kSwdData,INPUT);digitalWrite(21,LOW);}
  }
  void data(bool high) override { digitalWrite(slavepins::kSwdData,high?HIGH:LOW); }
  bool data() override { return digitalRead(slavepins::kSwdData)==HIGH; }
  void clock(bool high) override { digitalWrite(slavepins::kSwdClock,high?HIGH:LOW); }
  void halfPeriod() override { delayMicroseconds(halfPeriod_); }
  void reset(bool asserted) override { digitalWrite(15,asserted?LOW:HIGH); }
  uint32_t now() override { return millis(); }
  void wait(uint32_t ms) override { vTaskDelay(pdMS_TO_TICKS(ms?ms:1)); }
 private:
  uint32_t halfPeriod_;
};
void programWorker(void*) {
  for(;;) {
    ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    // Slave H2: UART_TX17, UART_RX18, BOOT16, NRST15, shared OE#47.
    // DIR4=LOW makes target SWDIO an input while the shared translators are on.
    digitalWrite(47,HIGH);pinMode(47,OUTPUT);
    digitalWrite(21,LOW);pinMode(21,OUTPUT);
    pinMode(slavepins::kSwdClock,INPUT);pinMode(slavepins::kSwdData,INPUT);pinMode(slavepins::kSwo,INPUT);
    digitalWrite(15,LOW);pinMode(15,OUTPUT);
    if(staging.interface==Interface::Swd) {
      // BOOT is intentionally high impedance: SWD needs only reset, clock/data,
      // VTREF and GND. Shared translator DIR4 tracks every SWDIO turnaround.
      pinMode(16,INPUT);pinMode(17,INPUT_PULLUP);pinMode(18,INPUT_PULLUP);
      digitalWrite(slavepins::kSwdClock,LOW);pinMode(slavepins::kSwdClock,OUTPUT);
      targetEnabled.store(true);digitalWrite(47,LOW);
      TargetSwd pins;ArmSwd arm(pins);F407Swd programmer(arm,staging.target);
      const bool recovery=swdRecoveryState.load()==1;
      workerError=recovery?programmer.removeReadProtection():programmer.program(image,staging.size,progress,legacySwdProgram);
      workerUsedRam.store(programmer.usedRam);
      workerDevice=programmer.deviceId;workerFlashKb=programmer.flashKb;
      swdCapacityWord.store(programmer.capacityWord);swdOptionBytes.store(programmer.optionBytes);
      swdOptionBytesAfter.store(programmer.optionBytesAfter);
      if(!recovery&&workerError==Error::Ok&&runAfterProgram) {
        workerError=programmer.start();goAcknowledged.store(workerError==Error::Ok);
        if(workerError==Error::Ok)progress(5,staging.size);
      }
      swdDpidr.store(arm.debugPortId);
      swdTrace.store(uint32_t(arm.lastAck)|(uint32_t(arm.lastRequest)<<8)|
          (uint32_t(arm.connectStep)<<16)|(uint32_t(workerError)<<24));
      digitalWrite(47,HIGH);targetEnabled.store(false);
      pinMode(slavepins::kSwdClock,INPUT);pinMode(slavepins::kSwdData,INPUT);pinMode(15,INPUT);pinMode(21,INPUT_PULLUP);
      if(recovery)swdRecoveryState.store(workerError==Error::Ok?2:3);
      workerDone.store(true);continue;
    }
    digitalWrite(16,HIGH);pinMode(16,OUTPUT);
    TargetUart transport;
    transport.uart.begin(115200,SERIAL_8E1,18,17);
    while(transport.uart.available())transport.uart.read();
    targetEnabled.store(true);digitalWrite(47,LOW);
    vTaskDelay(pdMS_TO_TICKS(50));digitalWrite(15,HIGH);
    vTaskDelay(pdMS_TO_TICKS(250));
    Stm32Rom rom(transport);
    workerError=rom.program(image,staging.size,staging.flashKb,progress,staging.target);
    workerDevice=rom.deviceId;
    workerFlashKb=rom.flashKb;
    if(workerError==Error::Ok&&runAfterProgram) {
      digitalWrite(16,LOW);
      workerError=rom.go();goAcknowledged.store(workerError==Error::Ok);
      if(workerError==Error::Ok)progress(5,staging.size);
    }
    // Default stays in ROM. ERASE RUN explicitly starts only after full readback.
    digitalWrite(47,HIGH);targetEnabled.store(false);
    transport.uart.end();
    pinMode(17,INPUT_PULLUP);pinMode(18,INPUT_PULLUP);
    pinMode(15,INPUT);pinMode(16,INPUT);pinMode(21,INPUT_PULLUP);
    workerDone.store(true);
  }
}
void localError(Error error,uint32_t transfer=0) {
  if(outgoing.op==Op::UartRead)uartLocked=false;
  if((outgoing.op==Op::EchoOpen&&error!=Error::Busy)||outgoing.op==Op::EchoClose)echoLocked=false;
  hostReply=Packet();hostReply.op=Op::Reply;hostReply.transfer=transfer;
  hostReply.length=32;hostReply.data[0]=uint8_t(State::Idle);hostReply.data[1]=uint8_t(error);
  put32(hostReply.data+20,uint32_t(hostTarget));
  put32(hostReply.data+28,uint32_t(hostInterface));
  replyRetries=attempts?attempts-1:0;pending=false;replyReady=true;
}
void makeReply(const Packet& request,Error error,Packet& response) {
  const auto& e=pidlink::radio.engine();response=Packet();response.op=Op::Reply;
  response.sender=e.session();response.peer=request.sender;response.transfer=request.transfer;
  response.sequence=request.sequence;response.offset=staging.received;response.length=32;
  response.data[0]=uint8_t(staging.state);response.data[1]=uint8_t(error);
  response.data[2]=progressStage.load();response.data[3]=(PID_FLASH_TARGET_ENABLE?1:0)|(goAcknowledged.load()?2:0)|4|(serialbridge::waitingTx()?8:0)|16|32|(workerUsedRam.load()?64:0);
  put32(response.data+4,staging.size);put32(response.data+8,progressBytes.load());
  put32(response.data+12,workerDevice.load());put32(response.data+16,uint32_t(lastJobError));
  put32(response.data+20,uint32_t(staging.target));
  put32(response.data+24,workerFlashKb.load());
  put32(response.data+28,uint32_t(staging.interface));
}
bool send(const Packet& p) {
  if((echoOp(p.op)||p.op==Op::EchoReply)&&echoRadioDropEvery&&++echoRadioSends%echoRadioDropEvery==0){++echoRadioDrops;return true;}
#if defined(PID_FLASH_TEST_DROP_EVERY)
  static uint32_t sends=0;
  if(++sends%PID_FLASH_TEST_DROP_EVERY==0)return true;
#endif
  uint8_t bytes[kMaxPacket];const size_t n=encode(p,bytes);return n&&pidlink::radio.sendAux(bytes,n,p.op!=Op::BulkData);
}
bool sendBulkAck() {
  if(bulkSlaveDropEvery&&(bulkAckAttempts+1)%bulkSlaveDropEvery==0){++bulkAckAttempts;++bulkAckDrops;return true;}
  Packet p;p.op=Op::BulkAck;p.sender=pidlink::radio.engine().session();p.peer=bulkSlaveOwner;
  p.transfer=staging.transfer;p.sequence=bulkSlaveToken;p.offset=staging.received;p.length=12;
  p.data[0]=uint8_t(bulkSlaveError);p.data[1]=1;p.data[2]=bulk::kWindow;put32(p.data+4,bulkSlaveBitmap);
  put32(p.data+8,bulkAckDrops);
  if(!send(p))return false;++bulkAckAttempts;return true;
}
bool sameRequest(const Packet& a,const Packet& b) {
  return a.op==b.op&&a.sender==b.sender&&a.peer==b.peer&&a.transfer==b.transfer&&
    a.sequence==b.sequence&&a.offset==b.offset&&a.length==b.length&&!std::memcmp(a.data,b.data,a.length);
}
void closeSerialLog() {
  if(!logActive)return;
  Serial0.end();Serial0.setRxBufferSize(256);
  Serial0.begin(115200,SERIAL_8N1,44,43);
  logActive=false;
}
void pollSerialLog() {
  if(!logActive)return;
  for(unsigned i=0;i<2048&&Serial0.available();++i)logBuffer->append(uint8_t(Serial0.read()));
}
bool receiveSerialLog(const Packet& p) {
  if(logCached&&p.sender==logRequest.sender&&p.sequence==logRequest.sequence) {
    if(!sameRequest(p,logRequest))return false;
    send(logReply);return true;
  }
  if(p.sender==logOwner&&logHighest&&int32_t(p.sequence-logHighest)<=0)return false;
  Packet reply;reply.op=Op::LogReply;reply.sender=p.peer;reply.peer=p.sender;
  reply.transfer=p.transfer;reply.sequence=p.sequence;reply.offset=p.offset;
  Error error=Error::Ok;
  if(p.op==Op::LogOpen) {
    LogConfig config;
    if(p.length!=8||p.offset||p.data[7]!=1)error=Error::Invalid;
    else {
      config.baud=get32(p.data);config.bits=p.data[4];config.parity=p.data[5];config.stop=p.data[6];
      if(!config.valid())error=Error::Invalid;
      else if(logId==p.transfer&&logOwner==p.sender&&logActive) {
        if(!(config==logConfig))error=Error::Conflict;
      } else {
        if(!logBuffer)logBuffer=new(std::nothrow) LogBuffer<32768>();
        if(!logBuffer)error=Error::NoMemory;
        else {
          closeSerialLog();Serial0.end();Serial0.setRxBufferSize(4096);
          const uint32_t format=0x08000010u|((config.bits-5u)<<2)|
              (config.parity==1?3u:config.parity==2?2u:0u)|(config.stop==2?0x20u:0u);
          Serial0.begin(config.baud,format,44,43);Serial0.setDebugOutput(false);
          // Receive-only: do not drive STM32 RX or any attached USB-TTL TX.
          pinMode(43,INPUT);
          logErrors.store(0);
          Serial0.onReceiveError([](hardwareSerial_error_t){logErrors.fetch_add(1);});
          logBuffer->reset();logConfig=config;logId=p.transfer;logOwner=p.sender;logActive=true;
        }
      }
    }
  } else if(p.transfer!=logId||p.sender!=logOwner)error=Error::WrongTransfer;
  else if(p.length)error=Error::Invalid;
  else if(p.op==Op::LogClose)closeSerialLog();
  else if(!logActive)error=Error::Invalid;
  else {
    pollSerialLog();size_t size=0;uint32_t actual=0;
    error=logBuffer->read(p.offset,reply.data+20,kLegacyChunk-20,actual,size);
    reply.offset=actual;reply.length=uint16_t(20+size);
  }
  if(error!=Error::Ok)makeReply(p,error,reply);
  else {
    if(reply.length<20)reply.length=20;
    reply.data[0]=uint8_t(p.op);reply.data[1]=logActive;reply.data[2]=logConfig.bits;
    reply.data[3]=logConfig.parity;reply.data[4]=logConfig.stop;reply.data[5]=1;
    put32(reply.data+8,logBuffer?logBuffer->dropped():0);
    put32(reply.data+12,logErrors.load());put32(reply.data+16,logConfig.baud);
  }
  logHighest=p.sequence;logRequest=p;logReply=reply;logCached=true;send(reply);return true;
}
void releaseEcho() {
  if(!echoActive)return;
  digitalWrite(47,HIGH);echoUart.end();
  pinMode(17,INPUT_PULLUP);pinMode(18,INPUT_PULLUP);
  pinMode(15,INPUT);pinMode(16,INPUT);pinMode(21,INPUT_PULLUP);
  echoActive=false;echoWaiting=false;echoParser.size=0;
}
void echoFailure(const Packet& p,Error error) {
  Packet reply;makeReply(p,error,reply);send(reply);
}
void echoWrite(uint32_t now) {
  uint8_t bytes[kMaxPacket];size_t n=encode(echoRequest,bytes);
  if(echoUartAttempts==0&&echoRequest.op==Op::EchoData&&echoUartCorruptEvery&&++echoDataSends%echoUartCorruptEvery==0){bytes[n-1]^=1;++echoUartCorruptions;}
  if(echoUartAttempts)++echoUartRetries;
  ++echoUartAttempts;echoSentAt=now;
  echoUart.write(bytes,n);
}
bool receiveEcho(const Packet& p,uint32_t now) {
  if(echoCached&&sameRequest(p,echoCachedRequest)){send(echoCachedReply);echoLastActivity=now;return true;}
  if(echoWaiting) {
    if(!sameRequest(p,echoRequest))echoFailure(p,Error::Busy);
    return true;
  }
  if(echoHighestSequence&&p.sender==echoRequest.sender&&int32_t(p.sequence-echoHighestSequence)<=0)return false;
  if(p.op==Op::EchoOpen) {
    if(p.length!=8||p.offset||get32(p.data)>100000||get32(p.data+4)>100000){echoFailure(p,Error::Invalid);return true;}
    if(echoActive||uartActive||serialbridge::ownsPort()||staging.busy()||pidlink::radio.engine().mirror().run){echoFailure(p,Error::Busy);return true;}
    if(!PID_FLASH_TARGET_ENABLE){echoFailure(p,Error::Disabled);return true;}
    motorbridge::pause(now);
    echoRadioDropEvery=get32(p.data);echoUartCorruptEvery=get32(p.data+4);
    echoRadioSends=echoRadioDrops=echoDataSends=echoUartRetries=echoUartCorruptions=0;
    digitalWrite(47,HIGH);pinMode(47,OUTPUT);
    digitalWrite(21,LOW);pinMode(21,OUTPUT);
    pinMode(slavepins::kSwdClock,INPUT);pinMode(slavepins::kSwdData,INPUT);pinMode(slavepins::kSwo,INPUT);
    digitalWrite(16,LOW);pinMode(16,OUTPUT);
    digitalWrite(15,HIGH);pinMode(15,OUTPUT);
    echoUart.setRxBufferSize(512);echoUart.begin(115200,SERIAL_8N1,18,17);
    while(echoUart.available())echoUart.read();
    digitalWrite(47,LOW);echoActive=true;echoParser.size=0;
  } else {
    if(!echoActive||p.sender!=echoRequest.sender||p.peer!=echoRequest.peer||p.transfer!=echoRequest.transfer){echoFailure(p,Error::WrongTransfer);return true;}
    if((p.op==Op::EchoData&&!p.length)||(p.op==Op::EchoClose&&p.length)){echoFailure(p,Error::Invalid);return true;}
  }
  echoCached=false;echoRequest=p;echoHighestSequence=p.sequence;
  echoWaiting=true;echoUartAttempts=0;echoLastActivity=now;echoWrite(now);return true;
}
void pollEcho(uint32_t now) {
  if(!echoActive)return;
  const auto& e=pidlink::radio.engine();
  if(!e.online(now)||echoRequest.sender!=e.peerSession()||echoRequest.peer!=e.session()||uint32_t(now-echoLastActivity)>10000){releaseEcho();echoCached=false;return;}
  for(unsigned i=0;i<320&&echoUart.available();++i) {
    uint8_t bytes[kMaxPacket];const size_t n=echo_parser_feed(&echoParser,uint8_t(echoUart.read()),bytes);
    if(!n||!echoWaiting)continue;
    Packet p;if(!decode(bytes,n,p)||p.op!=Op::EchoReply||p.sender!=echoRequest.peer||p.peer!=echoRequest.sender||
      p.transfer!=echoRequest.transfer||p.sequence!=echoRequest.sequence||p.offset!=echoRequest.offset)continue;
    if(echoRequest.op==Op::EchoData&&(p.length!=echoRequest.length||std::memcmp(p.data,echoRequest.data,p.length)))continue;
    if(echoRequest.op==Op::EchoOpen&&!echo_marker_valid(p.data,p.length))continue;
    if(echoRequest.op==Op::EchoClose&&p.length!=12)continue;
    if(echoRequest.op==Op::EchoClose){put32(p.data+12,echoUartRetries);put32(p.data+16,echoRadioDrops);put32(p.data+20,echoUartCorruptions);p.length=24;}
    echoWaiting=false;echoCached=true;echoCachedRequest=echoRequest;echoCachedReply=p;
    echoLastActivity=now;send(p);
    if(echoRequest.op==Op::EchoClose){releaseEcho();return;}
  }
  if(echoWaiting&&uint32_t(now-echoSentAt)>=180) {
    if(echoUartAttempts<4)echoWrite(now);
    else {echoFailure(echoRequest,Error::Timeout);releaseEcho();echoCached=false;}
  }
}
// Bounded 115200/8N1 capture. Keep reset released; never pulse target reset.
void releaseUartRead() {
  if(!uartActive)return;
  digitalWrite(47,HIGH);echoUart.end();
  pinMode(17,INPUT_PULLUP);pinMode(18,INPUT_PULLUP);
  pinMode(15,INPUT);pinMode(16,INPUT);pinMode(21,INPUT_PULLUP);
  uartActive=false;
}
bool receiveUartRead(const Packet& p,uint32_t now) {
  if(p.length||p.offset<50||p.offset>1000){echoFailure(p,Error::Invalid);return true;}
  if(uartHostSession!=p.sender){uartHostSession=p.sender;uartHighestSequence=0;}
  const bool sameId=p.sender==uartRequest.sender&&p.peer==uartRequest.peer&&p.transfer==uartRequest.transfer;
  if(sameId&&(uartCached||uartActive)) {
    if(p.offset!=uartRequest.offset){echoFailure(p,Error::Conflict);return true;}
    // A PC retry gets the same capture, including when it has a new radio sequence.
    if(int32_t(p.sequence-uartHighestSequence)>0)uartHighestSequence=p.sequence;
    if(uartCached){Packet replay=uartReply;replay.sequence=p.sequence;send(replay);}
    else {uartRequest.sequence=p.sequence;uartReply.sequence=p.sequence;}
    return true;
  }
  if(uartHighestSequence&&int32_t(p.sequence-uartHighestSequence)<=0)return false;
  if(uartActive||echoActive||serialbridge::ownsPort()||staging.busy()||pidlink::radio.engine().mirror().run){echoFailure(p,Error::Busy);return true;}
  if(!PID_FLASH_TARGET_ENABLE){echoFailure(p,Error::Disabled);return true;}
  motorbridge::pause(now);
  digitalWrite(47,HIGH);pinMode(47,OUTPUT);
  digitalWrite(21,LOW);pinMode(21,OUTPUT);
  pinMode(slavepins::kSwdClock,INPUT);pinMode(slavepins::kSwdData,INPUT);pinMode(slavepins::kSwo,INPUT);
  // The shared translator drives these channels when OE# is enabled. Do not
  // leave its reset input floating while listening to the target UART.
  digitalWrite(15,HIGH);pinMode(15,OUTPUT);
  digitalWrite(16,LOW);pinMode(16,OUTPUT);
  echoUart.setRxBufferSize(512);echoUart.begin(115200,SERIAL_8N1,18,17);
  while(echoUart.available())echoUart.read();
  uartRequest=p;uartReply=Packet();uartReply.op=Op::UartReply;
  uartReply.sender=p.peer;uartReply.peer=p.sender;uartReply.transfer=p.transfer;
  uartReply.sequence=p.sequence;uartReply.offset=p.offset;
  uartHighestSequence=p.sequence;uartStarted=now;uartCached=false;uartActive=true;digitalWrite(47,LOW);
  return true;
}
void pollUartRead(uint32_t now) {
  if(!uartActive)return;
  const auto& e=pidlink::radio.engine();
  if(!e.online(now)||uartRequest.sender!=e.peerSession()||uartRequest.peer!=e.session()) {
    releaseUartRead();uartCached=false;return;
  }
  while(uartReply.length<kLegacyChunk&&echoUart.available())uartReply.data[uartReply.length++]=uint8_t(echoUart.read());
  if(uartReply.length==kLegacyChunk||uint32_t(now-uartStarted)>=uartRequest.offset) {
    uartCached=true;releaseUartRead();send(uartReply);
  }
}
bool slaveReceive(const uint8_t* bytes,size_t n,uint32_t now) {
  Packet p;if(!decode(bytes,n,p)||p.op==Op::Reply||p.op==Op::EchoReply||p.op==Op::UartReply||p.op==Op::MotorReply||p.op==Op::LogReply||p.op==Op::SerialReply)return false;
  const auto& r=pidlink::radio;const auto& e=r.engine();
  if(p.sender!=e.peerSession()||p.peer!=e.session()||!e.online(now))return false;
  if(p.op==Op::BulkAck)return false;
  if(p.op==Op::BulkData) {
    if(!bulkSlaveActive||p.sender!=bulkSlaveOwner||p.sequence!=bulkSlaveToken||p.transfer!=staging.transfer)return false;
    if(bulkSlaveError==Error::Ok)bulkSlaveError=staging.appendWindow(p.transfer,p.offset,p.data,p.length,bulkSlaveBitmap);
    bulkAckPending=true;++bulkPackets;
    if(bulkSlaveError!=Error::Ok||bulkPackets>=4||staging.received==staging.size)bulkLastAck=now-8;
    return true;
  }
  if(logOp(p.op))return receiveSerialLog(p);
  if(serialOp(p.op)) {
    if(!PID_FLASH_TARGET_ENABLE){Packet reply;makeReply(p,Error::Disabled,reply);send(reply);return true;}
    return serialbridge::receive(p,now,echoActive||uartActive||swdHoldUntil||staging.state==State::Receiving||staging.state==State::Ready||staging.busy());
  }
  if(swdHoldUntil){Packet reply;makeReply(p,Error::Busy,reply);send(reply);return true;}
  if(p.op==Op::MotorStatus||p.op==Op::MotorTarget) {
    if(serialbridge::ownsPort()){echoFailure(p,Error::Busy);return true;}
    serialbridge::selectMotor();
    Packet reply;reply.op=Op::MotorReply;reply.sender=p.peer;reply.peer=p.sender;
    reply.transfer=p.transfer;reply.sequence=p.sequence;
    if(p.op==Op::MotorTarget) {
      if(p.length!=8||!motorbridge::targets(get32(p.data),get32(p.data+4))){echoFailure(p,Error::Invalid);return true;}
      reply.length=8;std::memcpy(reply.data,p.data,8);
    } else {
      MotorStatus status;uint32_t age;
      if(p.length){echoFailure(p,Error::Invalid);return true;}
      if(!motorbridge::status(status,age,now)){echoFailure(p,Error::Timeout);return true;}
      put32(reply.data,age);put32(reply.data+4,motorbridge::inhibited()?1:0);
      MotorProto_Encode(&status,2,MOTOR_STATUS_SIZE,reply.data+8);reply.length=8+MOTOR_STATUS_SIZE;
    }
    send(reply);return true;
  }
  if(p.op==Op::UartRead)return receiveUartRead(p,now);
  if(echoOp(p.op))return receiveEcho(p,now);
  if(cachedHost!=p.sender){cachedHost=p.sender;cachedSequence=0;}
  if(p.sequence==cachedSequence) {
    if(!sameRequest(p,lastRequest))return false;
    send(lastReply);return true;
  }
  if(cachedSequence&&int32_t(p.sequence-cachedSequence)<=0)return false;
  Error error=Error::Invalid;
  switch(p.op) {
    case Op::Begin:
      if(echoActive||uartActive){error=Error::Busy;break;}
      if(staging.busy()){error=Error::Busy;break;}
      motorbridge::pause(now);
      if((p.length==40||p.length==44||p.length==48)&&!e.mirror().run) {
        const uint32_t prior=staging.transfer;
        const Target selected=p.length>=44?Target(get32(p.data+40)):Target::G431;
        const Interface interface=p.length==48?Interface(get32(p.data+44)):Interface::Uart;
        const uint32_t requestedSize=get32(p.data),kb=get32(p.data+4);
        if(!capacityValid(selected,kb)||!interfaceValid(selected,interface)||requestedSize<8||requestedSize>kb*1024||requestedSize>kMaxImage){error=Error::Invalid;break;}
        // A conflicting/repeated manifest must not free an existing verified image.
        if(!(prior==p.transfer&&staging.state!=State::Idle)&&requestedSize>imageCapacity) {
          const uint32_t caps=MALLOC_CAP_8BIT|(requestedSize>kInternalImageLimit?MALLOC_CAP_SPIRAM:MALLOC_CAP_INTERNAL);
          uint8_t* replacement=static_cast<uint8_t*>(heap_caps_malloc(requestedSize,caps));
          if(!replacement){error=Error::NoMemory;break;}
          if(image)heap_caps_free(image);
          image=replacement;imageCapacity=requestedSize;staging.storage(image,imageCapacity);
        }
        error=staging.begin(p.transfer,requestedSize,kb,p.data+8,selected,interface);
        if(error==Error::Ok){bulkSlaveActive=bulkAckPending=false;bulkSlaveBitmap=0;serialbridge::poll(now,true);}
        if(error==Error::Ok&&prior!=p.transfer){lastJobError=Error::Ok;workerDevice=0;workerFlashKb=0;workerUsedRam.store(false);goAcknowledged.store(false);progress(0,0);}
      }
      break;
    case Op::BulkOpen:
      if(p.transfer!=staging.transfer){error=Error::WrongTransfer;break;}
      if(staging.state!=State::Receiving||(p.length!=1&&p.length!=2)||p.data[0]!=bulk::kWindow||p.offset!=staging.received||p.offset%kChunk){error=Error::Invalid;break;}
      bulkSlaveActive=true;bulkAckPending=false;bulkSlaveOwner=p.sender;bulkSlaveToken=p.sequence;
      bulkSlaveBitmap=bulkPackets=0;bulkLastAck=now;bulkSlaveError=Error::Ok;
      bulkSlaveDropEvery=p.length==2?p.data[1]:0;bulkAckAttempts=bulkAckDrops=0;error=Error::Ok;break;
    case Op::Data:bulkSlaveActive=false;error=staging.append(p.transfer,p.offset,p.data,p.length);break;
    case Op::Seal:if(!p.length){error=staging.seal(p.transfer,sha256);if(error==Error::Ok)bulkSlaveActive=bulkAckPending=false;}break;
    case Op::Flash:
      if(echoActive||uartActive){error=Error::Busy;break;}
      if(p.transfer!=staging.transfer){error=Error::WrongTransfer;break;}
      if(p.length>1||(p.length==1&&(!p.data[0]||(p.data[0]&~3u)))||
          (p.length==1&&(p.data[0]&2)&&staging.interface!=Interface::Swd)){error=Error::Invalid;break;}
      if(staging.state==State::Programming||staging.state==State::Done||staging.state==State::Failed){error=Error::Ok;break;}
      if(staging.state!=State::Ready||e.mirror().run){error=Error::Busy;break;}
      if(!serialbridge::readyForFlash()){error=Error::Busy;break;}
      if(!PID_FLASH_TARGET_ENABLE){error=Error::Disabled;break;}
      if(!worker){error=Error::NoMemory;break;}
      // Latch before acknowledging: duplicate Flash cannot schedule another erase.
      staging.state=State::Programming;lastJobError=Error::Ok;workerDone.store(false);progress(0,0);
      swdRecoveryState.store(0);
      runAfterProgram=p.length==1&&(p.data[0]&1);legacySwdProgram=p.length==1&&(p.data[0]&2);
      workerUsedRam.store(false);goAcknowledged.store(false);
      xTaskNotifyGive(worker);error=Error::Ok;break;
    case Op::Query:
      if(!p.length)error=p.transfer==staging.transfer?Error::Ok:Error::WrongTransfer;
      break;
    case Op::Abort:if(!p.length){error=staging.abort(p.transfer);if(error==Error::Ok)bulkSlaveActive=bulkAckPending=false;}break;
    default:break;
  }
  cachedSequence=p.sequence;lastRequest=p;makeReply(p,error,lastReply);send(lastReply);return true;
}
bool hostReceive(const uint8_t* bytes,size_t n,uint32_t now) {
  Packet p;if(!decode(bytes,n,p))return false;
  if(p.op==Op::BulkAck) {
    if(!bulkHostActive||p.sender!=bulkPeerSession||p.peer!=bulkHostSession||p.transfer!=bulkSender.transfer||p.sequence!=bulkHostToken||(p.length!=8&&p.length!=12)||p.data[1]!=1||p.data[2]!=bulk::kWindow||p.data[3]||p.data[0]>uint8_t(Error::Start))return false;
    if(p.length==12)bulkAckDropsSeen=get32(p.data+8);
    if(bulkSender.error==Error::Ok)bulkSender.error=p.data[0]?Error(p.data[0]):bulkSender.acknowledge(p.offset,get32(p.data+4),now);
    bulkNotice=true;return true;
  }
  if(!pending)return false;
  if(p.sender!=outgoing.peer||p.peer!=outgoing.sender||p.transfer!=outgoing.transfer||p.sequence!=outgoing.sequence)return false;
  if(serialOp(outgoing.op)) {
    if(p.op==Op::SerialReply){if(p.length<32||p.length>128||p.data[0]!=uint8_t(outgoing.op)||p.data[5]!=1||p.data[7]>uint8_t(Error::Start))return false;}
    else if(p.op!=Op::Reply||p.length<2||!p.data[1]||p.data[1]>uint8_t(Error::Start))return false;
    hostReply=p;replyRetries=attempts?attempts-1:0;pending=false;replyReady=true;return true;
  }
  if(logOp(outgoing.op)) {
    if(p.op==Op::LogReply) {
      if(p.length<20||p.data[0]!=uint8_t(outgoing.op)||p.data[5]!=1)return false;
    } else if(p.op!=Op::Reply||p.length<2||!p.data[1]||p.data[1]>uint8_t(Error::Start))return false;
    hostReply=p;replyRetries=attempts?attempts-1:0;pending=false;replyReady=true;return true;
  }
  if(outgoing.op==Op::MotorStatus||outgoing.op==Op::MotorTarget) {
    if(p.op!=Op::MotorReply&&(p.op!=Op::Reply||p.length<2||!p.data[1]||p.data[1]>uint8_t(Error::Start)))return false;
    if(p.op==Op::MotorReply&&p.length!=(outgoing.op==Op::MotorStatus?104:8))return false;
    hostReply=p;replyRetries=attempts?attempts-1:0;pending=false;replyReady=true;return true;
  }
  if(outgoing.op==Op::UartRead) {
    if(p.op!=Op::UartReply&&(p.op!=Op::Reply||p.length<2||!p.data[1]||p.data[1]>uint8_t(Error::Start)))return false;
    if(p.op==Op::UartReply&&p.offset!=outgoing.offset)return false;
    hostReply=p;replyRetries=attempts?attempts-1:0;pending=false;replyReady=true;uartLocked=false;
    return true;
  }
  if(echoOp(outgoing.op)) {
    if(p.op!=Op::EchoReply&&(p.op!=Op::Reply||p.length<2||!p.data[1]||p.data[1]>uint8_t(Error::Start)))return false;
    if(p.op==Op::EchoReply&&p.offset!=outgoing.offset)return false;
    if(p.op==Op::EchoReply&&outgoing.op==Op::EchoData&&(p.length!=outgoing.length||std::memcmp(p.data,outgoing.data,p.length)))return false;
    hostReply=p;replyRetries=attempts?attempts-1:0;pending=false;replyReady=true;
    if(outgoing.op==Op::EchoClose||p.op==Op::Reply)echoLocked=false;
    return true;
  }
  if(p.op!=Op::Reply||(p.length!=20&&p.length!=24&&p.length!=28&&p.length!=32))return false;
  if(p.data[0]>uint8_t(State::Failed)||p.data[1]>uint8_t(Error::Start))return false;
  if(p.length==20){put32(p.data+20,uint32_t(Target::G431));p.length=24;}
  if(p.length==24){put32(p.data+24,0);p.length=28;}
  if(p.length==28){put32(p.data+28,uint32_t(Interface::Uart));p.length=32;}
  if(!interfaceValid(Target(get32(p.data+20)),Interface(get32(p.data+28))))return false;
  if(outgoing.op==Op::BulkOpen&&p.data[1]==0) {
    if(!(p.data[3]&16)||p.offset!=outgoing.offset)return false;
    bulkSender.begin(p.transfer,get32(p.data+4),p.offset);bulkHostActive=true;bulkNotice=false;
    bulkHostToken=outgoing.sequence;bulkHostSession=outgoing.sender;bulkPeerSession=outgoing.peer;bulkLastReport=now;
    bulkDropEvery=outgoing.length==2?outgoing.data[1]:0;bulkDataAttempts=bulkDataDrops=bulkAckDropsSeen=0;
  }
  hostReply=p;replyRetries=attempts?attempts-1:0;pending=false;replyReady=true;
  if((outgoing.op==Op::Abort&&p.data[1]==0&&State(p.data[0])==State::Idle)||
      State(p.data[0])==State::Done||State(p.data[0])==State::Failed)locked=false;
  return true;
}
bool numeric(const char* text,uint32_t& value,unsigned base=10) {
  if(!text||!*text||*text=='-'||*text=='+')return false;
  errno=0;char* end=nullptr;const unsigned long n=std::strtoul(text,&end,base);
  if(*end||errno==ERANGE||n>0xfffffffful)return false;value=uint32_t(n);return true;
}
int hex(char c) { if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1; }
bool unhex(const char* text,uint8_t* bytes,size_t n) {
  if(!text||std::strlen(text)!=n*2)return false;
  for(size_t i=0;i<n;++i){int a=hex(text[i*2]),b=hex(text[i*2+1]);if(a<0||b<0)return false;bytes[i]=uint8_t((a<<4)|b);}
  return true;
}
} // namespace
void onAuxTransmitted(const uint8_t* bytes,size_t n,uint32_t now) {
  if(!bulkHostActive||n<32||bytes[3]!=uint8_t(Op::BulkData)||get32(bytes+4)!=bulkHostSession||get32(bytes+8)!=bulkPeerSession||get32(bytes+12)!=bulkSender.transfer||get32(bytes+16)!=bulkHostToken)return;
  bulkSender.transmitted(get32(bytes+20),now);
}
void beginHost(){pidlink::radio.setAuxReceiver(hostReceive);pidlink::radio.setAuxTransmitted(onAuxTransmitted);}
void beginSlave(){
  pidlink::radio.setAuxReceiver(slaveReceive);
  if(PID_FLASH_TARGET_ENABLE)xTaskCreatePinnedToCore(programWorker,"stm32_rom",6144,nullptr,1,&worker,0);
}
void pollSlave(uint32_t now) {
  if(bulkSlaveActive&&bulkAckPending&&uint32_t(now-bulkLastAck)>=8&&sendBulkAck()){bulkAckPending=false;bulkPackets=0;bulkLastAck=now;}
  pollSerialLog();
  if(swdHoldUntil&&int32_t(now-swdHoldUntil)>=0)releaseSwdInput();
  if(workerDone.exchange(false)) {
    lastJobError=workerError;staging.state=workerError==Error::Ok?State::Done:State::Failed;
  }
  pollEcho(millis());
  pollUartRead(millis());
  serialbridge::poll(now,staging.state==State::Receiving||staging.state==State::Ready||staging.busy());
  motorbridge::poll(millis(),!serialbridge::blocksMotor()&&!echoActive&&!uartActive&&!swdHoldUntil&&
      staging.state!=State::Receiving&&staging.state!=State::Ready&&!staging.busy()&&PID_FLASH_TARGET_ENABLE);
}
void pollHost(uint32_t now) {
  if(bulkHostActive&&bulkSender.error==Error::Ok) {
    const auto& e=pidlink::radio.engine();
    if(!pidlink::radio.ready()||e.session()!=bulkHostSession||e.peerSession()!=bulkPeerSession)bulkSender.error=Error::Link;
    else for(unsigned i=0;i<bulk::kWindow;++i) {
      auto* chunk=bulkSender.next(now);if(!chunk)break;
      Packet p;p.op=Op::BulkData;p.sender=bulkHostSession;p.peer=bulkPeerSession;p.transfer=bulkSender.transfer;p.sequence=bulkHostToken;
      p.offset=chunk->offset;p.length=chunk->length;std::memcpy(p.data,chunk->data,p.length);
      const bool drop=bulkDropEvery&&(bulkDataAttempts+1)%bulkDropEvery==0;
      if(!drop&&!send(p))break;
      ++bulkDataAttempts;
      if(drop){++bulkDataDrops;bulkSender.sent(*chunk,now);}else bulkSender.scheduled(*chunk);
    }
    if(bulkSender.error!=Error::Ok)bulkNotice=true;
  }
  if(!pending)return;
  const auto& r=pidlink::radio;const auto& e=r.engine();
  if(!r.ready()||outgoing.sender!=e.session()||outgoing.peer!=e.peerSession()) {localError(Error::Link,outgoing.transfer);return;}
  if(attempts&&uint32_t(now-lastSend)<150)return;
  if(attempts==20){localError(Error::Timeout,outgoing.transfer);return;}
  if(send(outgoing)){if(attempts)++retries;++attempts;lastSend=now;}
}
bool hostPending(){return pending||replyReady;}
bool hostLocked(){return locked||echoLocked||uartLocked;}
bool hostBulkActive(){return bulkHostActive;}
bool hostBinaryReading(){return bulkDecoder.collecting()||bulkUsbQuarantine;}
bool hostBinaryByte(uint8_t value,uint32_t) {
  bool ready=false,invalid=false;
  const bool consumed=bulkDecoder.feed(value,bulkUsbFrame,ready,invalid);
  if(!consumed)return bulkHostActive||bulkUsbQuarantine;
  if(invalid){bulkUsbQuarantine=true;if(bulkHostActive)bulkSender.error=Error::Invalid;bulkNotice=true;}
  if(!ready)return consumed;
  const auto& f=bulkUsbFrame;
  if(f.kind==bulk::UsbKind::Exit&&(f.transfer==0||f.transfer==bulkSender.transfer)){bulkUsbQuarantine=false;bulkHostActive=false;bulkNotice=true;return true;}
  if(!bulkHostActive||f.transfer!=bulkSender.transfer){bulkNotice=true;return true;}
  if(f.kind==bulk::UsbKind::Data){const Error e=bulkSender.enqueue(f.offset,f.data,f.length);if(e!=Error::Ok)bulkSender.error=e;}
  bulkNotice=true;return true;
}
bool targetActive(){return targetEnabled.load()||echoActive||uartActive||motorbridge::owned();}
bool serialLogging(){return logActive;}
Error recoverSwdTarget() {
  if(!PID_FLASH_TARGET_ENABLE)return Error::Disabled;
  if(!worker)return Error::NoMemory;
  if(swdRecoveryState.load()||staging.busy()||staging.state==State::Receiving||staging.state==State::Ready||
      echoActive||uartActive||serialbridge::ownsPort()||swdHoldUntil||pidlink::radio.engine().mirror().run)return Error::Busy;
  motorbridge::pause(millis());
  staging.transfer=staging.size=staging.received=0;staging.flashKb=512;cachedSequence=0;
  staging.target=Target::F407VE;staging.interface=Interface::Swd;
  staging.state=State::Programming;workerDone.store(false);lastJobError=Error::Ok;
  goAcknowledged.store(false);runAfterProgram=false;swdRecoveryState.store(1);
  xTaskNotifyGive(worker);return Error::Ok;
}
Error probeSwdTarget(uint32_t* values,size_t count) {
  static const uint32_t addresses[]={0xe0042000,0xe000edf0,0x40023c00,0x40023c0c,
      0x40023c10,0x40023c14,0x1fff7a20,0x08000000,0x08000004,0x08000008,0x1fff7a10};
  if(!values||count!=2*sizeof(addresses)/sizeof(addresses[0]))return Error::Invalid;
  if(!PID_FLASH_TARGET_ENABLE)return Error::Disabled;
  if(staging.busy()||staging.state==State::Receiving||staging.state==State::Ready||echoActive||uartActive||serialbridge::ownsPort()||swdHoldUntil)return Error::Busy;
  motorbridge::pause(millis());
  digitalWrite(47,HIGH);pinMode(47,OUTPUT);
  digitalWrite(21,LOW);pinMode(21,OUTPUT);
  digitalWrite(15,LOW);pinMode(15,OUTPUT);
  pinMode(16,INPUT);pinMode(17,INPUT_PULLUP);pinMode(18,INPUT_PULLUP);pinMode(slavepins::kSwo,INPUT);
  digitalWrite(slavepins::kSwdClock,LOW);pinMode(slavepins::kSwdClock,OUTPUT);pinMode(slavepins::kSwdData,INPUT);
  targetEnabled.store(true);digitalWrite(47,LOW);
  TargetSwd pins(5);ArmSwd arm(pins);Error error=arm.connect();
  // Allow external reset circuitry and Flash power-up to settle before diagnosis.
  if(error==Error::Ok)pins.wait(20);
  constexpr size_t n=sizeof(addresses)/sizeof(addresses[0]);
  for(size_t i=0;i<n&&error==Error::Ok;++i)error=arm.read32(addresses[i],values[i]);
  // Cross-check the physical reset path with Cortex-M SYSRESETREQ. DEMCR's
  // reset catch remains enabled, so no unverified application is resumed.
  if(error==Error::Ok)error=arm.write32(0xe000ed0c,0x05fa0004);
  if(error==Error::Ok)pins.wait(20);
  for(size_t i=0;i<n&&error==Error::Ok;++i)error=arm.read32(addresses[i],values[n+i]);
  digitalWrite(47,HIGH);targetEnabled.store(false);
  pinMode(slavepins::kSwdClock,INPUT);pinMode(slavepins::kSwdData,INPUT);pinMode(15,INPUT);pinMode(21,INPUT_PULLUP);
  return error;
}
Error checkSwdRam() {
  if(!PID_FLASH_TARGET_ENABLE)return Error::Disabled;
  if(staging.busy()||staging.state==State::Receiving||staging.state==State::Ready||echoActive||uartActive||
      serialbridge::ownsPort()||swdHoldUntil||pidlink::radio.engine().mirror().run)return Error::Busy;
  motorbridge::pause(millis());
  digitalWrite(47,HIGH);pinMode(47,OUTPUT);digitalWrite(21,LOW);pinMode(21,OUTPUT);
  digitalWrite(15,LOW);pinMode(15,OUTPUT);pinMode(16,INPUT);
  pinMode(17,INPUT_PULLUP);pinMode(18,INPUT_PULLUP);pinMode(slavepins::kSwo,INPUT);
  digitalWrite(slavepins::kSwdClock,LOW);pinMode(slavepins::kSwdClock,OUTPUT);pinMode(slavepins::kSwdData,INPUT);
  targetEnabled.store(true);digitalWrite(47,LOW);
  TargetSwd pins;ArmSwd arm(pins);F407Swd programmer(arm,Target::F407VE);
  const Error error=programmer.ramCheck();
  swdDpidr.store(arm.debugPortId);
  swdTrace.store(uint32_t(arm.lastAck)|(uint32_t(arm.lastRequest)<<8)|(uint32_t(arm.connectStep)<<16)|(uint32_t(error)<<24));
  digitalWrite(47,HIGH);targetEnabled.store(false);
  pinMode(slavepins::kSwdClock,INPUT);pinMode(slavepins::kSwdData,INPUT);pinMode(15,INPUT);pinMode(21,INPUT_PULLUP);
  return error;
}
SwdDiagnostics swdDiagnostics(){
  const uint32_t packed=swdTrace.load();
  return {swdDpidr.load(),uint8_t(packed),uint8_t(packed>>8),uint8_t(packed>>16),Error(packed>>24),swdCapacityWord.load(),swdOptionBytes.load(),swdOptionBytesAfter.load(),swdRecoveryState.load()};
}
Error checkSwdInput(uint8_t& isolated,uint8_t& internalPullup,uint8_t& targetLevel,bool hold){
  if(staging.busy()||echoActive||uartActive||serialbridge::ownsPort()||targetEnabled.load())return Error::Busy;
  if(pidlink::radio.engine().mirror().run)return Error::Busy;
  motorbridge::pause(millis());
  digitalWrite(47,HIGH);pinMode(47,OUTPUT);
  pinMode(slavepins::kSwdData,INPUT);delayMicroseconds(20);isolated=digitalRead(slavepins::kSwdData);
  pinMode(slavepins::kSwdData,INPUT_PULLUP);delayMicroseconds(20);internalPullup=digitalRead(slavepins::kSwdData);
  pinMode(slavepins::kSwdData,INPUT);
  digitalWrite(21,LOW);pinMode(21,OUTPUT); // B -> A, target owns SWDIO.
  digitalWrite(slavepins::kSwdClock,LOW);pinMode(slavepins::kSwdClock,OUTPUT);
  pinMode(15,INPUT);pinMode(16,INPUT);
  pinMode(17,INPUT_PULLUP);pinMode(18,INPUT_PULLUP);pinMode(slavepins::kSwo,INPUT);
  digitalWrite(47,LOW);delayMicroseconds(100);targetLevel=digitalRead(slavepins::kSwdData);
  if(hold){swdHoldUntil=millis()+180000;targetEnabled.store(true);return Error::Ok;}
  digitalWrite(47,HIGH);
  pinMode(slavepins::kSwdClock,INPUT);pinMode(slavepins::kSwdData,INPUT);pinMode(21,INPUT_PULLUP);
  return Error::Ok;
}
void releaseSwdInput(){
  if(!swdHoldUntil)return;
  digitalWrite(47,HIGH);
  pinMode(slavepins::kSwdClock,INPUT);pinMode(slavepins::kSwdData,INPUT);pinMode(21,INPUT_PULLUP);
  swdHoldUntil=0;targetEnabled.store(false);
}
bool hostCommand(const char* text) {
  const bool stream=!std::strncmp(text,"serial",6)&&(!text[6]||text[6]==' '||text[6]=='\t');
  const bool log=!std::strncmp(text,"log",3)&&(!text[3]||text[3]==' '||text[3]=='\t');
  const bool echo=!std::strncmp(text,"echo",4)&&(!text[4]||text[4]==' '||text[4]=='\t');
  const bool uart=!std::strncmp(text,"uart",4)&&(!text[4]||text[4]==' '||text[4]=='\t');
  const bool motor=!std::strncmp(text,"motor",5)&&(!text[5]||text[5]==' '||text[5]=='\t');
  if(!stream&&!log&&!echo&&!uart&&!motor&&(std::strncmp(text,"fw",2)|| (text[2]&&text[2]!=' '&&text[2]!='\t')))return false;
  // Keep the accepted request and its unconsumed response intact. An early
  // USB retry must never cancel it or replace its correlation ID with zero.
  if(pending||replyReady)return true;
  outgoing=Packet();outgoing.op=stream?Op::SerialOpen:log?Op::LogOpen:motor?Op::MotorStatus:uart?Op::UartRead:echo?Op::EchoOpen:Op::Query;
  char buffer[512];
  if(std::strlen(text)>=sizeof(buffer)){localError(Error::Invalid);return true;}
  std::strcpy(buffer,text);char* save=nullptr;const char* w[8];unsigned count=0;
  for(char* v=strtok_r(buffer," \t",&save);v;v=strtok_r(nullptr," \t",&save)){if(count==8){localError(Error::Invalid);return true;}w[count++]=v;}
  Packet p;bool valid=false;
  if(count>=3&&numeric(w[2],p.transfer,16)&&p.transfer) {
    if(stream) {
      if((count==7||count==8)&&!std::strcmp(w[1],"open")) {
        uint32_t baud,bits,parity,stop;
        if(numeric(w[3],baud)&&numeric(w[4],bits)&&numeric(w[5],parity)&&numeric(w[6],stop)&&bits<=255&&parity<=255&&stop<=255&&(count==7||!std::strcmp(w[7],"loop"))) {
          LogConfig c;c.baud=baud;c.bits=bits;c.parity=parity;c.stop=stop;
          if(c.valid()){p.op=Op::SerialOpen;p.length=8;put32(p.data,baud);p.data[4]=bits;p.data[5]=parity;p.data[6]=stop;p.data[7]=count==8?2:1;valid=true;}
        }
      } else if(count==4&&!std::strcmp(w[1],"read")&&numeric(w[3],p.offset)){p.op=Op::SerialRead;valid=true;}
      else if(count==3&&!std::strcmp(w[1],"close")){p.op=Op::SerialClose;valid=true;}
      else if(count==6&&!std::strcmp(w[1],"write")) {
        uint32_t epoch;const size_t n=std::strlen(w[5])/2;
        if(n&&n<=kSerialTxChunk&&numeric(w[3],p.offset)&&numeric(w[4],epoch,16)&&epoch&&unhex(w[5],p.data+4,n)) {
          p.op=Op::SerialWrite;p.length=uint16_t(n+4);put32(p.data,epoch);valid=true;
        }
      }
    } else if(log) {
      if(count==7&&!std::strcmp(w[1],"open")) {
        uint32_t baud,bits,parity,stop;
        if(numeric(w[3],baud)&&numeric(w[4],bits)&&numeric(w[5],parity)&&numeric(w[6],stop)&&bits<=255&&parity<=255&&stop<=255) {
          LogConfig config;config.baud=baud;config.bits=uint8_t(bits);config.parity=uint8_t(parity);config.stop=uint8_t(stop);
          if(config.valid()){p.op=Op::LogOpen;p.length=8;put32(p.data,baud);p.data[4]=bits;p.data[5]=parity;p.data[6]=stop;p.data[7]=1;valid=true;}
        }
      } else if(count==4&&!std::strcmp(w[1],"read")&&numeric(w[3],p.offset)){p.op=Op::LogRead;valid=true;}
      else if(count==3&&!std::strcmp(w[1],"close")){p.op=Op::LogClose;valid=true;}
    } else if(motor) {
      if(count==3&&!std::strcmp(w[1],"status")){p.op=Op::MotorStatus;valid=true;}
      else if(count==5&&!std::strcmp(w[1],"target")) {
        uint32_t left,right;
        if(numeric(w[3],left)&&numeric(w[4],right)&&left<=40000&&right<=40000) {
          p.op=Op::MotorTarget;p.length=8;put32(p.data,left);put32(p.data+4,right);valid=true;
        }
      }
    } else if(uart) {
      if(count==4&&!std::strcmp(w[1],"read")&&numeric(w[3],p.offset)&&p.offset>=50&&p.offset<=1000) {
        p.op=Op::UartRead;valid=true;
      }
    } else if(echo) {
      if(!std::strcmp(w[1],"open")&&(count==3||count==5)) {
        uint32_t drop=0,corrupt=0;
        if(count==3||(numeric(w[3],drop)&&numeric(w[4],corrupt)&&drop<=100000&&corrupt<=100000)) {
          p.op=Op::EchoOpen;p.length=8;put32(p.data,drop);put32(p.data+4,corrupt);valid=true;
        }
      } else if(!std::strcmp(w[1],"data")&&count==5) {
        const size_t size=std::strlen(w[4])/2;
        if(size&&size<=kLegacyChunk&&numeric(w[3],p.offset)&&unhex(w[4],p.data,size)){p.op=Op::EchoData;p.length=size;valid=true;}
      } else if(!std::strcmp(w[1],"close")&&count==4&&numeric(w[3],p.offset)){p.op=Op::EchoClose;valid=true;}
    } else if(!std::strcmp(w[1],"begin")&&(count==6||count==7||count==8)) {
      uint32_t size,kb;
      if(numeric(w[3],size)&&numeric(w[4],kb)&&unhex(w[5],p.data+8,32)) {
        Target selected=Target::G431;
        Interface interface=Interface::Uart;
        if(count>=7) {
          if(!std::strcmp(w[6],"f103c8"))selected=Target::F103C8;
          else if(!std::strcmp(w[6],"f407vg"))selected=Target::F407VG;
          else if(!std::strcmp(w[6],"f407ve"))selected=Target::F407VE;
          else if(std::strcmp(w[6],"g431")){localError(Error::Invalid,p.transfer);return true;}
        }
        if(count==8) {
          if(!std::strcmp(w[7],"swd"))interface=Interface::Swd;
          else if(std::strcmp(w[7],"uart")){localError(Error::Invalid,p.transfer);return true;}
        }
        if(!capacityValid(selected,kb)||!interfaceValid(selected,interface)||size<8||size>kb*1024){localError(Error::Invalid,p.transfer);return true;}
        p.op=Op::Begin;p.length=count==8?48:count==7?44:40;put32(p.data,size);put32(p.data+4,kb);
        if(count>=7)put32(p.data+40,uint32_t(selected));
        if(count==8)put32(p.data+44,uint32_t(interface));
        hostTarget=selected;hostInterface=interface;valid=true;
      }
    } else if(!std::strcmp(w[1],"bulk")&&(count==4||count==5)&&numeric(w[3],p.offset)) {
      uint32_t drop=0;
      if(count==4||(numeric(w[4],drop)&&drop<=255)){p.op=Op::BulkOpen;p.length=2;p.data[0]=bulk::kWindow;p.data[1]=drop;valid=true;}
    } else if(!std::strcmp(w[1],"data")&&count==5) {
      size_t size=std::strlen(w[4])/2;
      if(size&&size<=kChunk&&numeric(w[3],p.offset)&&unhex(w[4],p.data,size)){p.op=Op::Data;p.length=size;valid=true;}
    } else if(count==3) {
      if(!std::strcmp(w[1],"seal")){p.op=Op::Seal;valid=true;}
      else if(!std::strcmp(w[1],"status")){p.op=Op::Query;valid=true;}
      else if(!std::strcmp(w[1],"abort")){p.op=Op::Abort;valid=true;}
    } else if(count>=4&&count<=6&&!std::strcmp(w[1],"flash")&&!std::strcmp(w[3],"ERASE")) {
      uint8_t flags=0;valid=true;
      for(unsigned i=4;i<count;++i){const uint8_t flag=!std::strcmp(w[i],"RUN")?1:!std::strcmp(w[i],"LEGACY")?2:0;
        if(!flag||(flags&flag)){valid=false;break;}flags|=flag;}
      if(valid){p.op=Op::Flash;if(flags){p.length=1;p.data[0]=flags;}}
    }
  }
  if(!valid){localError(Error::Invalid,p.transfer);return true;}
  if(bulkHostActive&&(p.op==Op::Seal||p.op==Op::Flash||p.op==Op::Data)){localError(Error::Busy,p.transfer);return true;}
  if(p.op==Op::Begin||p.op==Op::Abort||p.op==Op::BulkOpen){bulkHostActive=false;bulkNotice=false;}
  outgoing=p;
  if(motor&&locked){localError(Error::Busy,p.transfer);return true;}
  if((uart&&(locked||echoLocked||uartLocked))||(!uart&&uartLocked)) {localError(Error::Busy,p.transfer);return true;}
  if((echo&&(locked||(echoLocked&&p.op==Op::EchoOpen)))||(!echo&&echoLocked&&(p.op==Op::Begin||p.op==Op::Flash))){localError(Error::Busy,p.transfer);return true;}
  if(p.op==Op::EchoOpen){echoRadioDropEvery=get32(p.data);echoRadioSends=echoRadioDrops=0;}
  const auto& r=pidlink::radio;const auto& e=r.engine();
  if(!r.ready()||!e.ackFresh(millis())){localError(Error::Link,p.transfer);return true;}
  p.sender=e.session();p.peer=e.peerSession();if(++nextSequence==0)++nextSequence;p.sequence=nextSequence;
  outgoing=p;attempts=0;pending=true;replyReady=false;
  if(p.op==Op::Begin||p.op==Op::Flash)locked=true;
  if(p.op==Op::EchoOpen)echoLocked=true;
  if(p.op==Op::UartRead)uartLocked=true;
  return true;
}
bool takeHostReply(char* out,size_t size) {
  if(!replyReady) {
    const uint32_t now=millis();
    if(!bulkNotice||(bulkHostActive&&bulkSender.error==Error::Ok&&bulkSender.received!=bulkSender.size&&uint32_t(now-bulkLastReport)<20))return false;
    bulkNotice=false;bulkLastReport=now;
    snprintf(out,size,"{\"type\":\"bulk\",\"id\":%lu,\"received\":%lu,\"queued\":%lu,\"credit\":%lu,\"retries\":%lu,\"rto_ms\":%lu,\"active\":%s,\"error\":\"%s\",\"injected_data_drops\":%lu,\"injected_ack_drops\":%lu}",
      (unsigned long)bulkSender.transfer,(unsigned long)bulkSender.received,(unsigned long)bulkSender.queued,(unsigned long)bulkSender.freeBytes(),
      (unsigned long)bulkSender.retries,(unsigned long)bulkSender.rto,bulkHostActive?"true":"false",errorName(bulkSender.error),
      (unsigned long)bulkDataDrops,(unsigned long)bulkAckDropsSeen);return true;
  }
  replyReady=false;
  const Packet& p=hostReply;
  if(serialOp(outgoing.op)) {
    const bool valid=p.op==Op::SerialReply&&p.length>=32;
    const Error error=valid?Error(p.data[7]):Error(p.data[1]);
    char hexBytes[193];const char* digits="0123456789abcdef";
    const size_t length=valid?p.length-32:0;
    for(size_t i=0;i<length;++i){hexBytes[2*i]=digits[p.data[32+i]>>4];hexBytes[2*i+1]=digits[p.data[32+i]&15];}hexBytes[2*length]=0;
    snprintf(out,size,"{\"type\":\"serial_stream\",\"id\":%lu,\"op\":%u,\"offset\":%lu,\"ok\":%s,\"error\":\"%s\",\"hex\":\"%s\",\"active\":%s,\"paused\":%s,\"epoch\":%lu,\"tx_total\":%lu,\"dropped\":%lu,\"rx_errors\":%lu,\"baud\":%lu,\"pause_events\":%lu,\"channel\":%u}",
      (unsigned long)p.transfer,unsigned(outgoing.op),(unsigned long)p.offset,error==Error::Ok?"true":"false",errorName(error),hexBytes,
      valid&&(p.data[1]&1)?"true":"false",valid&&(p.data[1]&2)?"true":"false",(unsigned long)(valid?get32(p.data+8):0),
      (unsigned long)(valid?get32(p.data+12):0),(unsigned long)(valid?get32(p.data+16):0),(unsigned long)(valid?get32(p.data+20):0),
      (unsigned long)(valid?get32(p.data+24):0),(unsigned long)(valid?get32(p.data+28):0),unsigned(valid?p.data[6]:0));
    return true;
  }
  if(logOp(outgoing.op)) {
    const bool ok=p.op==Op::LogReply;char hexBytes[(kChunk-20)*2+1];const char* digits="0123456789abcdef";
    const size_t length=ok?p.length-20:0;
    for(size_t i=0;i<length;++i){hexBytes[i*2]=digits[p.data[20+i]>>4];hexBytes[i*2+1]=digits[p.data[20+i]&15];}hexBytes[length*2]=0;
    snprintf(out,size,"{\"type\":\"serial_log\",\"id\":%lu,\"op\":%u,\"offset\":%lu,\"ok\":%s,\"error\":\"%s\",\"hex\":\"%s\",\"dropped\":%lu,\"rx_errors\":%lu,\"baud\":%lu,\"retries\":%lu}",
        static_cast<unsigned long>(p.transfer),unsigned(outgoing.op),static_cast<unsigned long>(p.offset),ok?"true":"false",
        ok?"ok":errorName(Error(p.data[1])),hexBytes,static_cast<unsigned long>(ok?get32(p.data+8):0),
        static_cast<unsigned long>(ok?get32(p.data+12):0),static_cast<unsigned long>(ok?get32(p.data+16):0),static_cast<unsigned long>(replyRetries));
    return true;
  }
  if(outgoing.op==Op::MotorStatus||outgoing.op==Op::MotorTarget) {
    const bool ok=p.op==Op::MotorReply;char hexBytes[kChunk*2+1];const char* digits="0123456789abcdef";
    const size_t length=ok?p.length:0;
    for(size_t i=0;i<length;i++){hexBytes[i*2]=digits[p.data[i]>>4];hexBytes[i*2+1]=digits[p.data[i]&15];}hexBytes[length*2]=0;
    snprintf(out,size,"{\"type\":\"motor\",\"id\":%lu,\"op\":%u,\"ok\":%s,\"error\":\"%s\",\"hex\":\"%s\"}",
      static_cast<unsigned long>(p.transfer),unsigned(outgoing.op),ok?"true":"false",ok?"ok":errorName(Error(p.data[1])),hexBytes);
    return true;
  }
  if(outgoing.op==Op::UartRead) {
    const bool ok=p.op==Op::UartReply;
    char hexBytes[kChunk*2+1];const char* digits="0123456789abcdef";
    const size_t length=ok?p.length:0;
    for(size_t i=0;i<length;++i){hexBytes[i*2]=digits[p.data[i]>>4];hexBytes[i*2+1]=digits[p.data[i]&15];}
    hexBytes[length*2]=0;
    snprintf(out,size,"{\"type\":\"uart\",\"id\":%lu,\"op\":12,\"offset\":%lu,\"ok\":%s,\"error\":\"%s\",\"hex\":\"%s\",\"retries\":%lu}",
      static_cast<unsigned long>(p.transfer),static_cast<unsigned long>(outgoing.offset),ok?"true":"false",
      ok?"ok":errorName(Error(p.data[1])),hexBytes,static_cast<unsigned long>(replyRetries));
    return true;
  }
  if(echoOp(outgoing.op)) {
    const bool ok=p.op==Op::EchoReply;
    char hexBytes[kChunk*2+1];const char* hexDigits="0123456789abcdef";
    size_t length=ok?p.length:0;
    for(size_t i=0;i<length;++i){hexBytes[i*2]=hexDigits[p.data[i]>>4];hexBytes[i*2+1]=hexDigits[p.data[i]&15];}hexBytes[length*2]=0;
    snprintf(out,size,"{\"type\":\"echo\",\"id\":%lu,\"op\":%u,\"offset\":%lu,\"ok\":%s,\"error\":\"%s\",\"hex\":\"%s\",\"retries\":%lu,\"total_retries\":%lu,\"injected_drops\":%lu}",
      static_cast<unsigned long>(p.transfer),unsigned(outgoing.op),static_cast<unsigned long>(outgoing.offset),ok?"true":"false",
      ok?"ok":errorName(Error(p.data[1])),hexBytes,static_cast<unsigned long>(replyRetries),static_cast<unsigned long>(retries),static_cast<unsigned long>(echoRadioDrops));
    return true;
  }
  const auto* target=profile(Target(get32(p.data+20)));
  const bool swd=Interface(get32(p.data+28))==Interface::Swd;
  snprintf(out,size,"{\"type\":\"fw\",\"id\":%lu,\"request\":%lu,\"op\":%u,\"offset\":%lu,\"ok\":%s,\"error\":\"%s\",\"state\":\"%s\","
      "\"received\":%lu,\"size\":%lu,\"stage\":%u,\"progress\":%lu,\"device_id\":%lu,\"actual_flash_kb\":%lu,\"job_error\":\"%s\","
      "\"target_enabled\":%s,\"go_ack\":%s,\"target\":\"%s\",\"profile\":%lu,\"interface\":\"%s\",\"start_ack\":%s,\"retries\":%lu,\"total_retries\":%lu,\"max_chunk\":%u,\"serial_waiting\":%s,\"bulk_window\":%u,\"swd_ram\":%s,\"swd_algorithm\":\"%s\"}",
      static_cast<unsigned long>(p.transfer),static_cast<unsigned long>(p.sequence),unsigned(outgoing.op),
      static_cast<unsigned long>(outgoing.offset),p.data[1]==0?"true":"false",
      errorName(Error(p.data[1])),stateName(State(p.data[0])),static_cast<unsigned long>(p.offset),
      static_cast<unsigned long>(get32(p.data+4)),unsigned(p.data[2]),static_cast<unsigned long>(get32(p.data+8)),
      static_cast<unsigned long>(get32(p.data+12)),static_cast<unsigned long>(get32(p.data+24)),errorName(Error(get32(p.data+16))),(p.data[3]&1)?"true":"false",
      !swd&&(p.data[3]&2)?"true":"false",
      target?target->name:"unknown",static_cast<unsigned long>(get32(p.data+20)),
      swd?"swd":"uart",(p.data[3]&2)?"true":"false",
      static_cast<unsigned long>(replyRetries),static_cast<unsigned long>(retries),unsigned((p.data[3]&4)?kChunk:kLegacyChunk),(p.data[3]&8)?"true":"false",unsigned((p.data[3]&16)?bulk::kWindow:0),
      (p.data[3]&32)?"true":"false",swd?((p.data[3]&64)?"ram_1k":"legacy_word"):"uart_rom");
  return true;
}
} // namespace pidflash
