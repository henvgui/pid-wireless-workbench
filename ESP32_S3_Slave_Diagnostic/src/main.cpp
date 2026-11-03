#include <Arduino.h>
#include <HardwareSerial.h>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <atomic>
#include "driver/rmt.h"
#include "esp_intr_alloc.h"
#include "esp_system.h"
#include "board_pins.h"
#include "diagnostic_protocol.h"
#if defined(PID_LINK_ENABLED)
#include <PIDLink.h>
#endif
#if defined(PID_FLASH_ENABLED)
#include <PIDFlash.h>
#endif

namespace {
#if defined(PID_FLASH_ENABLED)
#if defined(BOARD_HAS_PSRAM)
const char* kFirmware="slave_flash_psram";
#else
const char* kFirmware="slave_flash";
#endif
const char* kVersion="20261103.3";
const char* kRadio="espnow";
#elif defined(PID_LINK_ENABLED)
const char* kFirmware="slave_wireless";
const char* kVersion="20261017";
const char* kRadio="espnow";
#else
const char* kFirmware="slave_diagnostic";
const char* kVersion="20261016";
const char* kRadio="off";
#endif
constexpr uint8_t kBrightness=32;
constexpr rmt_channel_t kRgbChannel=RMT_CHANNEL_0;
uint32_t bootId=0, loops=0, pingSequence=0, lastRgb=0, rgbFrames=0, rgbFailures=0;
esp_err_t rgbError=ESP_ERR_INVALID_STATE;
bool rgbReady=false, automatic=true;
uint8_t red=0,green=0,blue=0;
rmt_item32_t rgbItems[25] = {};
TaskHandle_t usbFlushTask=nullptr;
std::atomic<bool> usbFlushDone{false};
void usbFlushWorker(void*);

struct Console {
  explicit Console(bool isUsb):usb(isUsb) {}
  bool usb;
  bool receiving=false,overflow=false,watch=false,announced=false;
  char input[128] = {};
  size_t inputSize=0;
  uint32_t lastByte=0,lastWatch=0;
  char output[512] = {};
  size_t outputSize=0,outputOffset=0;
  bool flushRequested=false;
  bool connected() const { return usb ? bool(Serial) : true; }
  int available() const { return usb ? Serial.available() : Serial0.available(); }
  int read() { return usb ? Serial.read() : Serial0.read(); }
  int room() const { return usb ? Serial.availableForWrite() : Serial0.availableForWrite(); }
  size_t write(const uint8_t* bytes,size_t size) {
    return usb ? Serial.write(bytes,size) : Serial0.write(bytes,size);
  }
  void clearInput() { receiving=overflow=false; inputSize=0; }
  void reply(const char* format,...) {
    va_list args; va_start(args,format);
    int size=vsnprintf(output,sizeof(output)-3,format,args);
    va_end(args);
    if (size<0 || size>=int(sizeof(output)-3)) {
      size=snprintf(output,sizeof(output)-3,"{\"type\":\"error\",\"code\":\"reply_too_long\"}");
    }
    outputSize=size; output[outputSize++]='\n';
    // Native USB short packet terminates replies even at exact 64-byte multiples.
    if (outputSize%64==0) output[outputSize++]='\n';
    outputOffset=0;
  }
  void pump() {
    if (usb && flushRequested) {
      if (usbFlushDone.exchange(false)) { flushRequested=false; outputSize=outputOffset=0; }
      return;
    }
    if (!connected() || !outputSize) return;
    if (usb && usbFlushTask) { flushRequested=true; xTaskNotifyGive(usbFlushTask); return; }
    const int free=room();
    if (free<=0) return;
    size_t count=outputSize-outputOffset;
    if (count>(usb?63u:64u)) count=usb?63u:64u;
    if (count>size_t(free)) count=free;
    outputOffset+=write(reinterpret_cast<uint8_t*>(output+outputOffset),count);
    if (outputOffset==outputSize) {
      if (usb && usbFlushTask) { flushRequested=true; xTaskNotifyGive(usbFlushTask); }
      else { outputOffset=outputSize=0; }
    }
  }
};
Console usb(true),uart(false);
void usbFlushWorker(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    Serial.setTxTimeoutMs(250);
    size_t sent=0;
    while (sent<usb.outputSize && Serial) {
      const size_t count=Serial.write(reinterpret_cast<const uint8_t*>(usb.output+sent),usb.outputSize-sent);
      if (!count) break;
      sent+=count;
    }
    Serial.flush();
    if (usb.outputSize && usb.output[usb.outputSize-1]=='\n' && Serial) {
      Serial.write(uint8_t('\n')); Serial.flush();
    }
    usbFlushDone.store(true);
  }
}

const char* resetName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:return "power_on";
    case ESP_RST_EXT:return "external";
    case ESP_RST_SW:return "software";
    case ESP_RST_PANIC:return "panic";
    case ESP_RST_INT_WDT:return "interrupt_watchdog";
    case ESP_RST_TASK_WDT:return "task_watchdog";
    case ESP_RST_WDT:return "watchdog";
    case ESP_RST_DEEPSLEEP:return "deep_sleep";
    case ESP_RST_BROWNOUT:return "brownout";
    case ESP_RST_SDIO:return "sdio";
    default:return "unknown";
  }
}
bool linkConnected(uint32_t now) {
#if defined(PID_LINK_ENABLED)
  const auto& r=pidlink::radio;
  return r.ready() && r.engine().online(now) && r.engine().mirrorValid();
#else
  return false;
#endif
}
const char* rgbMode(uint32_t now) {
  if (!automatic) return "solid";
#if defined(PID_LINK_ENABLED)
  return linkConnected(now)?"link_solid":"link_breath";
#else
  return "heartbeat";
#endif
}
const char* targetOutputs() {
#if defined(PID_FLASH_ENABLED)
  return pidflash::targetActive()?"enabled":"disabled";
#else
  return "disabled";
#endif
}
void info(Console& console,const char* type="info") {
  console.reply("{\"type\":\"%s\",\"firmware\":\"%s\",\"version\":\"%s\","
      "\"chip\":\"%s\",\"revision\":%u,\"cores\":%u,\"cpu_mhz\":%u,\"flash_bytes\":%lu,"
      "\"psram_initialized\":%s,\"psram_bytes\":%lu,\"free_psram\":%lu,\"boot_id\":\"%08lx\",\"reset_reason\":\"%s\",\"radio\":\"%s\","
      "\"interface\":\"%s\",\"rgb_pin\":38,\"target_outputs\":\"%s\"}",
      type,kFirmware,kVersion,ESP.getChipModel(),unsigned(ESP.getChipRevision()),unsigned(ESP.getChipCores()),
      unsigned(ESP.getCpuFreqMHz()),static_cast<unsigned long>(ESP.getFlashChipSize()),
      ESP.getPsramSize()?"true":"false",static_cast<unsigned long>(ESP.getPsramSize()),static_cast<unsigned long>(ESP.getFreePsram()),
      static_cast<unsigned long>(bootId),resetName(),kRadio,console.usb?"usb":"uart0",targetOutputs());
}
void status(Console& console) {
  console.reply("{\"type\":\"status\",\"boot_id\":\"%08lx\",\"uptime_ms\":%lu,\"loops\":%lu,"
      "\"free_heap\":%lu,\"min_free_heap\":%lu,\"reset_reason\":\"%s\",\"radio\":\"%s\","
      "\"rgb_pin\":38,\"rgb_mode\":\"%s\",\"rgb\":[%u,%u,%u],\"brightness\":32,"
      "\"rgb_frames\":%lu,\"rgb_error\":%d,\"rgb_failures\":%lu,\"translator_oe\":%u,"
      "\"target_outputs\":\"%s\",\"watch\":%s}",
      static_cast<unsigned long>(bootId),static_cast<unsigned long>(millis()),
      static_cast<unsigned long>(loops),static_cast<unsigned long>(ESP.getFreeHeap()),
      static_cast<unsigned long>(ESP.getMinFreeHeap()),resetName(),kRadio,rgbMode(millis()),
      unsigned(red),unsigned(green),unsigned(blue),static_cast<unsigned long>(rgbFrames),int(rgbError),
      static_cast<unsigned long>(rgbFailures),unsigned(digitalRead(board::kTranslatorDisable)),
      targetOutputs(),console.watch?"true":"false");
}
#if defined(PID_LINK_ENABLED)
void linkStatus(Console& console) {
  const auto& r=pidlink::radio; const auto& e=r.engine(); const auto& d=e.mirror();
  console.reply("{\"type\":\"link\",\"enabled\":%s,\"ready\":%s,\"online\":%s,\"valid\":%s,\"encrypted\":true,"
      "\"channel\":1,\"session\":%lu,\"peer_session\":%lu,\"rx_age_ms\":%lu,\"p\":%.2f,\"i\":%.2f,\"d\":%.2f,\"step\":%.2f,"
      "\"revision\":%lu,\"request_id\":%lu,\"run_request\":%s,\"applied\":%lu,\"sent\":%lu,\"received\":%lu,"
      "\"rejected\":%lu,\"dropped\":%lu,\"radio_error\":%d,\"target_outputs\":\"%s\"}",
      r.enabled()?"true":"false",r.ready()?"true":"false",e.online(millis())?"true":"false",
      e.mirrorValid()?"true":"false",static_cast<unsigned long>(e.session()),
      static_cast<unsigned long>(e.peerSession()),static_cast<unsigned long>(e.lastRxAge(millis())),
      double(d.p)/100,double(d.i)/100,double(d.d)/100,double(d.step)/100,
      static_cast<unsigned long>(d.revision),static_cast<unsigned long>(d.request),d.run?"true":"false",
      static_cast<unsigned long>(e.applied()),static_cast<unsigned long>(r.sent()),
      static_cast<unsigned long>(r.received()),static_cast<unsigned long>(r.rejected()),
      static_cast<unsigned long>(r.dropped()),int(r.error()),targetOutputs());
}
#endif
void execute(Console& console) {
  diagnostic::Command command;
  if (!diagnostic::parse(console.input,command)) {
    console.reply("{\"type\":\"error\",\"code\":\"invalid_command_or_arguments\"}"); return;
  }
  using diagnostic::Kind;
  switch (command.kind) {
    case Kind::StartTrace:
#if defined(PID_FLASH_ENABLED)
      {uint32_t v[8]={};pidflash::swdStartDiagnostics(v,8);
       console.reply("{\"type\":\"starttrace\",\"step\":%lu,\"reset_seen\":%s,\"before_pc\":\"0x%08lx\",\"before_special\":\"0x%08lx\",\"pc\":\"0x%08lx\",\"sp\":\"0x%08lx\",\"special\":\"0x%08lx\",\"dhcsr\":\"0x%08lx\"}",
           (unsigned long)v[0],v[1]?"true":"false",(unsigned long)v[2],(unsigned long)v[3],(unsigned long)v[4],(unsigned long)v[5],(unsigned long)v[6],(unsigned long)v[7]);}
#else
      console.reply("{\"type\":\"error\",\"code\":\"programmer_not_built\"}");
#endif
      break;
    case Kind::RamCheck:
#if defined(PID_FLASH_ENABLED)
      {
        const auto error=pidflash::checkSwdRam();
        console.reply("{\"type\":\"ramcheck\",\"error\":\"%s\",\"flash_written\":false,\"target_halted\":%s}",
            pidflash::errorName(error),error==pidflash::Error::Ok?"true":"false");
      }
#else
      console.reply("{\"type\":\"error\",\"code\":\"programmer_not_built\"}");
#endif
      break;
    case Kind::SwdUnprotect:
#if defined(PID_FLASH_ENABLED)
      {
        const auto error=pidflash::recoverSwdTarget();
        console.reply("{\"type\":\"swdrecovery\",\"accepted\":%s,\"error\":\"%s\",\"erases_all_flash\":true}",
            error==pidflash::Error::Ok?"true":"false",pidflash::errorName(error));
      }
#else
      console.reply("{\"type\":\"error\",\"code\":\"programmer_not_built\"}");
#endif
      break;
    case Kind::SwdProbe:
#if defined(PID_FLASH_ENABLED)
      {
        uint32_t values[22]={};const auto error=pidflash::probeSwdTarget(values,22);
        console.reply("{\"type\":\"swdprobe\",\"error\":\"%s\",\"halt_confirmed\":%s,\"half_period_us\":5,\"values\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu],\"after_reset\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu]}",
            pidflash::errorName(error),(values[1]&(1u<<17))?"true":"false",static_cast<unsigned long>(values[0]),static_cast<unsigned long>(values[1]),
            static_cast<unsigned long>(values[2]),static_cast<unsigned long>(values[3]),static_cast<unsigned long>(values[4]),
            static_cast<unsigned long>(values[5]),static_cast<unsigned long>(values[6]),static_cast<unsigned long>(values[7]),
            static_cast<unsigned long>(values[8]),static_cast<unsigned long>(values[9]),static_cast<unsigned long>(values[10]),
            static_cast<unsigned long>(values[11]),static_cast<unsigned long>(values[12]),static_cast<unsigned long>(values[13]),
            static_cast<unsigned long>(values[14]),static_cast<unsigned long>(values[15]),static_cast<unsigned long>(values[16]),
            static_cast<unsigned long>(values[17]),static_cast<unsigned long>(values[18]),static_cast<unsigned long>(values[19]),
            static_cast<unsigned long>(values[20]),static_cast<unsigned long>(values[21]));
      }
#else
      console.reply("{\"type\":\"error\",\"code\":\"programmer_not_built\"}");
#endif
      break;
    case Kind::SwdRelease:
#if defined(PID_FLASH_ENABLED)
      pidflash::releaseSwdInput();status(console);
#else
      console.reply("{\"type\":\"error\",\"code\":\"programmer_not_built\"}");
#endif
      break;
    case Kind::SwdHold:
    case Kind::SwdCheck:
#if defined(PID_FLASH_ENABLED)
      {
        uint8_t isolated=0,pullup=0,target=0;
        const auto error=pidflash::checkSwdInput(isolated,pullup,target,command.kind==Kind::SwdHold);
        console.reply("{\"type\":\"swdcheck\",\"error\":\"%s\",\"isolated_dio\":%u,\"internal_pullup_dio\":%u,"
            "\"target_dio\":%u,\"oe_disable\":%u,\"target_outputs\":\"%s\"}",pidflash::errorName(error),
            unsigned(isolated),unsigned(pullup),unsigned(target),unsigned(digitalRead(47)),targetOutputs());
      }
#else
      console.reply("{\"type\":\"error\",\"code\":\"programmer_not_built\"}");
#endif
      break;
    case Kind::Swd:
#if defined(PID_FLASH_ENABLED)
      {
        const auto trace=pidflash::swdDiagnostics();
        console.reply("{\"type\":\"swd\",\"dpidr\":%lu,\"ack\":%u,\"request\":%u,\"connect_step\":%u,\"error\":\"%s\","
            "\"clk\":%u,\"dio\":%u,\"nrst\":%u,\"dir\":%u,\"oe_disable\":%u,\"target_outputs\":\"%s\",\"capacity_word\":%lu,\"option_bytes\":%lu,\"option_bytes_after\":%lu,\"recovery_state\":%u}",
            static_cast<unsigned long>(trace.dpidr),unsigned(trace.ack),unsigned(trace.request),unsigned(trace.step),
            pidflash::errorName(trace.error),unsigned(digitalRead(board::kSwdClock)),unsigned(digitalRead(board::kSwdData)),
            unsigned(digitalRead(15)),unsigned(digitalRead(21)),unsigned(digitalRead(47)),targetOutputs(),
            static_cast<unsigned long>(trace.capacityWord),static_cast<unsigned long>(trace.optionBytes),
            static_cast<unsigned long>(trace.optionBytesAfter),unsigned(trace.recoveryState));
      }
#else
      console.reply("{\"type\":\"error\",\"code\":\"programmer_not_built\"}");
#endif
      break;
    case Kind::Ping:
      console.reply("{\"type\":\"pong\",\"seq\":%lu,\"uptime_ms\":%lu,\"boot_id\":\"%08lx\"}",
          static_cast<unsigned long>(++pingSequence),static_cast<unsigned long>(millis()),
          static_cast<unsigned long>(bootId)); break;
    case Kind::Info:info(console); break;
    case Kind::Status:status(console); break;
    case Kind::Pins:
      console.reply("{\"type\":\"pins\",\"rgb\":38,\"oe_disable\":47,\"debug_tx\":43,\"debug_rx\":44,"
          "\"target_tx\":17,\"target_rx\":18,\"target_nrst\":15,\"target_boot\":16,"
          "\"swd_clk\":%u,\"swd_dio\":%u,\"swo\":%u,\"swd_dir\":21,\"sda\":8,\"scl\":9,"
          "\"ext\":[4,5,6,7],\"target_outputs\":\"%s\"}",
          unsigned(board::kSwdClock),unsigned(board::kSwdData),unsigned(board::kSwo),targetOutputs()); break;
    case Kind::Rgb:
      automatic=false; red=command.red; green=command.green; blue=command.blue;
      status(console); break;
    case Kind::Off:automatic=false; red=green=blue=0; status(console); break;
    case Kind::Auto:automatic=true; status(console); break;
    case Kind::Watch:console.watch=command.enabled; console.lastWatch=millis(); status(console); break;
    case Kind::Link:
#if defined(PID_LINK_ENABLED)
      linkStatus(console); break;
#else
      console.reply("{\"type\":\"error\",\"code\":\"wireless_not_built\"}"); break;
#endif
    case Kind::Radio:
#if defined(PID_LINK_ENABLED)
      pidlink::radio.enable(command.enabled); linkStatus(console); break;
#else
      console.reply("{\"type\":\"error\",\"code\":\"wireless_not_built\"}"); break;
#endif
    case Kind::Help:
      console.reply("{\"type\":\"help\",\"commands\":[\"!ping\",\"!info\",\"!status\",\"!pins\","
          "\"!rgb R G B (0..255)\",\"!off\",\"!auto\",\"!watch on\",\"!watch off\",\"!help\"]}"); break;
    default:break;
  }
}
void poll(Console& console,uint32_t now) {
  console.pump();
  if (!console.connected()) {
    console.announced=false;
    if (!console.flushRequested) console.outputSize=console.outputOffset=0;
    console.clearInput(); return;
  }
  if (console.flushRequested) return;
  if (!console.announced) { info(console,"ready"); console.announced=true; }
  console.pump();
  if (console.outputSize) return;
  for (unsigned n=0;n<128 && console.available() && !console.outputSize;++n) {
    const char c=char(console.read());
    if (!console.receiving) {
      if (c=='!') { console.receiving=true; console.lastByte=now; }
      else if (c!='\r' && c!='\n') {
        console.output[0]=c; console.outputSize=1; console.outputOffset=0;
      }
      continue;
    }
    console.lastByte=now;
    if (c=='\r' || c=='\n') {
      if (console.overflow) console.reply("{\"type\":\"error\",\"code\":\"command_too_long_or_invalid_byte\"}");
      else { console.input[console.inputSize]=0; execute(console); }
      console.clearInput();
    } else if (static_cast<uint8_t>(c)<32 && c!='\t') console.overflow=true;
    else if (console.inputSize+1>=sizeof(console.input)) console.overflow=true;
    else if (!console.overflow) console.input[console.inputSize++]=c;
  }
  if (!console.outputSize && console.receiving && uint32_t(now-console.lastByte)>=2000) {
    console.clearInput(); console.reply("{\"type\":\"error\",\"code\":\"command_timeout\"}");
  }
  if (!console.outputSize && !console.receiving && !console.available() && console.watch &&
      uint32_t(now-console.lastWatch)>=1000) {
    status(console); console.lastWatch=now;
  }
  console.pump();
}

void beginRgb() {
  rmt_config_t config={};
  config.rmt_mode=RMT_MODE_TX; config.channel=kRgbChannel;
  config.gpio_num=static_cast<gpio_num_t>(board::kRgb); config.clk_div=2; config.mem_block_num=1;
  config.tx_config.idle_output_en=true; config.tx_config.idle_level=RMT_IDLE_LEVEL_LOW;
  rgbError=rmt_config(&config);
  if (rgbError==ESP_OK) rgbError=rmt_driver_install(kRgbChannel,0,ESP_INTR_FLAG_IRAM);
  if (rgbError==ESP_OK) {
    uint32_t hz=0; rgbError=rmt_get_counter_clock(kRgbChannel,&hz);
    if (rgbError==ESP_OK && hz!=40000000) rgbError=ESP_ERR_INVALID_STATE;
  }
  rgbReady=rgbError==ESP_OK;
}
void updateRgb(uint32_t now) {
  if (uint32_t(now-lastRgb)<20) return;
  lastRgb=now;
  if (automatic) {
    const uint32_t phase=now%1000;
    // Unconnected: green breathing. Connected: steady green, including RUN.
    const float pulse=phase<400 ? std::sin(float(phase)*3.14159265f/400.0f) : 0;
    red=blue=0; green=uint8_t(12+243*pulse);
#if defined(PID_LINK_ENABLED)
    if (linkConnected(now)) green=255;
#endif
  }
  if (!rgbReady) return;
  const uint8_t bytes[]={uint8_t((uint16_t(green)*kBrightness+127)/255),
      uint8_t((uint16_t(red)*kBrightness+127)/255),uint8_t((uint16_t(blue)*kBrightness+127)/255)};
  unsigned n=0;
  for (uint8_t value:bytes) for (uint8_t mask=0x80;mask;mask>>=1) {
    rmt_item32_t& item=rgbItems[n++]; item.val=0;
    item.level0=1; item.duration0=(value&mask)?36:12; // 900 / 300 ns.
    item.level1=0; item.duration1=(value&mask)?14:38; // 350 / 950 ns.
  }
  rgbItems[n].val=0; rgbItems[n].duration0=6000; rgbItems[n].duration1=6000; // 300 us reset.
  rgbError=rmt_write_items(kRgbChannel,rgbItems,25,false);
  if (rgbError==ESP_OK) rgbError=rmt_wait_tx_done(kRgbChannel,pdMS_TO_TICKS(10));
  if (rgbError==ESP_OK) ++rgbFrames;
  else { ++rgbFailures; rmt_tx_stop(kRgbChannel); rgbReady=false; }
}
}  // namespace

void setup() {
  // Disable target translators before setting up any other I/O.
  digitalWrite(board::kTranslatorDisable,HIGH);
  pinMode(board::kTranslatorDisable,OUTPUT);
  pinMode(board::kTargetReset,INPUT);
  pinMode(board::kTargetBoot,INPUT);
  pinMode(board::kTargetTx,INPUT_PULLUP);
  pinMode(board::kTargetRx,INPUT_PULLUP);
  pinMode(board::kSwdClock,INPUT);
  pinMode(board::kSwdData,INPUT);
  pinMode(board::kSwo,INPUT);
  pinMode(board::kSwdDirection,INPUT_PULLUP);
  for (uint8_t pin:board::kExternal) pinMode(pin,INPUT);
  bootId=esp_random();
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  xTaskCreatePinnedToCore(usbFlushWorker,"slave_usb_flush",2048,nullptr,1,&usbFlushTask,xPortGetCoreID());
  Serial0.begin(115200,SERIAL_8N1,board::kDebugRx,board::kDebugTx);
  beginRgb();
#if defined(PID_LINK_ENABLED)
  pidlink::radio.begin(pidlink::Role::Slave);
#if defined(PID_FLASH_ENABLED)
  pidflash::beginSlave();
#endif
#endif
}
void loop() {
  ++loops;
  const uint32_t now=millis();
#if defined(PID_LINK_ENABLED)
#if defined(PID_FLASH_ENABLED)
  pidflash::pollSlave(now);
#endif
  pidlink::radio.receive(now); pidlink::radio.transmit(now);
#endif
  poll(usb,now);
#if defined(PID_FLASH_ENABLED)
  if(!pidflash::serialLogging())poll(uart,now);
#else
  poll(uart,now);
#endif
  updateRgb(now);
  delay(1);
}
