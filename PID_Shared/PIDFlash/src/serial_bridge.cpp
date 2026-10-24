#include "serial_bridge.h"
#include "serial_tx.h"
#include "serial_log.h"
#include "motor_bridge.h"
#include "slave_target_pins.h"
#include <PIDLink.h>
#include <Arduino.h>
#include <HardwareSerial.h>
#include <driver/uart.h>
#include <esp_system.h>
#include <new>
#include <atomic>
namespace pidflash { namespace serialbridge {
namespace {
HardwareSerial uart{1};LogBuffer<32768>* buffer=nullptr;LogConfig config;SerialTxLedger tx;
bool active=false,opened=false,paused=false,draining=false,loopback=false,serialSelected=false,cached=false;
uint32_t owner=0,session=0,highest=0,pauses=0;std::atomic<uint32_t> errors{0};Packet lastRequest,lastReply;
bool same(const Packet& a,const Packet& b) {
  return a.op==b.op&&a.sender==b.sender&&a.transfer==b.transfer&&a.offset==b.offset&&a.length==b.length&&!std::memcmp(a.data,b.data,a.length);
}
void send(const Packet& p){uint8_t bytes[kMaxPacket];const size_t n=encode(p,bytes);if(n)pidlink::radio.sendAux(bytes,n);}
bool emptyTx(){return !opened||uart_wait_tx_done(UART_NUM_1,0)==ESP_OK;}
void collect(){if(opened)for(unsigned i=0;i<2048&&uart.available();++i)buffer->append(uint8_t(uart.read()));}
void release(){
  if(!opened)return;
  collect();digitalWrite(47,HIGH);uart_set_loop_back(UART_NUM_1,false);uart.end();
  pinMode(17,INPUT_PULLUP);pinMode(18,INPUT_PULLUP);pinMode(15,INPUT);pinMode(16,INPUT);pinMode(21,INPUT_PULLUP);opened=false;
}
bool openPort(){
  motorbridge::pause(millis());digitalWrite(47,HIGH);pinMode(47,OUTPUT);digitalWrite(21,LOW);pinMode(21,OUTPUT);
  pinMode(slavepins::kSwdClock,INPUT);pinMode(slavepins::kSwdData,INPUT);pinMode(slavepins::kSwo,INPUT);
  digitalWrite(15,HIGH);pinMode(15,OUTPUT);digitalWrite(16,LOW);pinMode(16,OUTPUT);
  uart.setRxBufferSize(4096);uart.setTxBufferSize(2048);
  const uint32_t format=0x08000010u|((config.bits-5u)<<2)|(config.parity==1?3u:config.parity==2?2u:0u)|(config.stop==2?0x20u:0u);
  uart.begin(config.baud,format,18,17);if(!uart)return false;
  uart.onReceiveError([](hardwareSerial_error_t){errors.fetch_add(1);});
  uart_set_loop_back(UART_NUM_1,loopback);opened=true;
  // Internal self-test isolates the target through the external translator.
  digitalWrite(47,loopback?HIGH:LOW);return true;
}
}
bool ownsPort(){return active;}bool blocksMotor(){return serialSelected;}
void selectMotor(){if(!active)serialSelected=false;}
bool waitingTx(){return draining;}bool readyForFlash(){return !active||(paused&&!draining&&!opened);}
void poll(uint32_t,bool blocked){
  if(!active)return;collect();
  if(blocked){if(!paused){paused=true;++pauses;}draining=!emptyTx();if(!draining)release();}
  else if(paused){draining=false;if(openPort())paused=false;}
}
bool receive(const Packet& p,uint32_t,bool blocked){
  if(!serialOp(p.op))return false;
  if(cached&&p.sender==lastRequest.sender&&p.sequence==lastRequest.sequence){if(same(p,lastRequest))send(lastReply);return true;}
  if(p.sender==owner&&highest&&int32_t(p.sequence-highest)<=0)return true;
  Packet reply;reply.op=Op::SerialReply;reply.sender=p.peer;reply.peer=p.sender;reply.transfer=p.transfer;reply.sequence=p.sequence;reply.offset=p.offset;reply.length=32;
  Error error=Error::Ok;
  if(p.op==Op::SerialOpen){
    LogConfig c;
    if(p.length!=8||p.offset||p.data[7]<1||p.data[7]>2)error=Error::Invalid;
    else {
      c.baud=get32(p.data);c.bits=p.data[4];c.parity=p.data[5];c.stop=p.data[6];
      if(!c.valid())error=Error::Invalid;
      else if(blocked)error=Error::Busy;
      else if(active&&owner==p.sender&&session==p.transfer){if(!(c==config)||(p.data[7]==2)!=loopback)error=Error::Conflict;}
      else if(!emptyTx())error=Error::Busy;
      else {
        release();if(!buffer)buffer=new(std::nothrow) LogBuffer<32768>();
        if(!buffer)error=Error::NoMemory;
        else {
          config=c;loopback=p.data[7]==2;owner=p.sender;session=p.transfer;serialSelected=true;
          uint32_t epoch=esp_random();tx.reset(epoch?epoch:1);buffer->reset();errors=0;pauses=0;paused=draining=false;
          active=openPort();if(!active)error=Error::NoMemory;
        }
      }
    }
  } else if(session!=p.transfer||owner!=p.sender)error=Error::WrongTransfer;
  else if(p.op==Op::SerialClose){if(p.length)error=Error::Invalid;else if(!emptyTx())error=Error::Busy;else {release();active=paused=draining=false;}}
  else if(!active)error=Error::Invalid;
  else if(p.op==Op::SerialRead){
    if(p.length)error=Error::Invalid;
    else {collect();size_t length=0;uint32_t actual=0;error=buffer->read(p.offset,reply.data+32,96,actual,length);reply.offset=actual;reply.length=uint16_t(32+length);}
  } else if(p.op==Op::SerialWrite){
    if(blocked||paused||!opened)error=Error::Busy;
    else if(p.length<5||p.length>4+kSerialTxChunk)error=Error::Invalid;
    else error=tx.write(get32(p.data),p.offset,p.data+4,p.length-4,[](const uint8_t* bytes,size_t n)->size_t {
      if(uart.availableForWrite()<int(n))return SIZE_MAX;return uart.write(bytes,n);
    });
  }
  reply.data[0]=uint8_t(p.op);reply.data[1]=(active?1:0)|(paused?2:0);reply.data[2]=config.bits;reply.data[3]=config.parity;reply.data[4]=config.stop;reply.data[5]=1;
  reply.data[6]=loopback?3:2;reply.data[7]=uint8_t(error);put32(reply.data+8,tx.epoch());put32(reply.data+12,tx.total());put32(reply.data+16,buffer?buffer->dropped():0);
  put32(reply.data+20,errors.load());put32(reply.data+24,config.baud);put32(reply.data+28,pauses);
  highest=p.sequence;lastRequest=p;lastReply=reply;cached=true;send(reply);return true;
}
} }
