#include <Arduino.h>
#if defined(PID_RGB_DIRECT_RMT)
#include "rgb_rmt.h"
#else
#include <Adafruit_NeoPixel.h>
#endif
#include <cstdio>
#include <cstring>
#include "soc/gpio_reg.h"
#include "step_control.h"
#if defined(PID_HOST_UI)
#include "host_ui.h"
#include "host_oled.h"
#endif
#if defined(PID_HOST_LOCAL)
#include "host_local_app.h"
#endif
#if defined(PID_FLASH_ENABLED)
#include "PIDFlash.h"
#endif

#if !defined(CONFIG_IDF_TARGET_ESP32S3) || ARDUINO_USB_MODE != 1 || ARDUINO_USB_CDC_ON_BOOT != 1
#error "Select ESP32-S3 with native hardware USB CDC enabled."
#endif

namespace {
#if defined(PID_HOST_UI)
constexpr uint8_t kDefaultEdgesPerStep = 2;
constexpr uint8_t kBrightnessLimit = 30;
#else
constexpr uint8_t kDefaultEdgesPerStep = 4;
constexpr uint8_t kBrightnessLimit = rgbshow::kMaxBrightness;
#endif
struct Knob {
  stepcontrol::Quadrature decoder;
  stepcontrol::Button button;
  uint8_t index = 0, a = 0, b = 0, sw = 0, edges = kDefaultEdgesPerStep;
  bool reverse = false;
  uint32_t rawEdges = 0, dropped = 0;
  uint32_t cw = 0, ccw = 0, presses = 0, simulated = 0;
};
struct Event { uint8_t knob; int8_t direction; };
constexpr uint8_t kQueueSize = 64;
Knob knobs[stepcontrol::kKnobCount];
volatile Event events[kQueueSize];
volatile uint8_t readIndex = 0, writeIndex = 0;
portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;
#if defined(PID_RGB_DIRECT_RMT)
RgbRmt rgbOutput;  // Static internal DRAM, never PSRAM or an ISR stack buffer.
bool rgbTest = false;
uint32_t rgbTestStart = 0;
#else
Adafruit_NeoPixel strip(rgbshow::kLedCount,rgbshow::kPin,NEO_GRB+NEO_KHZ800);
#endif
#if defined(PID_HOST_UI)
hostui::Controller hostUi;
stepcontrol::HoldButton stepButton;
#if defined(PID_LINK_ENABLED)
stepcontrol::DoubleClickButton peerButton;
#endif
uint32_t stepHolds = 0;
stepcontrol::KnobScene& scene = hostUi.scene();
#else
stepcontrol::KnobScene scene;
#endif
stepcontrol::Rgb pixels[rgbshow::kLedCount];
uint8_t brightness = kBrightnessLimit, reportMask = 0;
uint32_t frames = 0, lastFrame = 0, lastReport = 0;
bool dirty = true;
#if defined(PID_FLASH_ENABLED)
char command[512];
#else
char command[128];
#endif
size_t commandLength = 0;
bool collecting = false, overflow = false, skipLF = false;
uint32_t lastByte = 0;

uint8_t IRAM_ATTR readAB(const Knob& knob) {
  const uint32_t bits = REG_READ(GPIO_IN_REG);
  return (((bits >> knob.a)&1)<<1) | ((bits >> knob.b)&1);
}

void IRAM_ATTR onEdge(void* argument) {
  Knob& knob = *static_cast<Knob*>(argument);
  portENTER_CRITICAL_ISR(&encoderMux);
  ++knob.rawEdges;
  int8_t step = knob.decoder.onEdge(readAB(knob));
  if (knob.reverse) step = -step;
  if (step) {
    const uint8_t next = (writeIndex+1)%kQueueSize;
    if (next == readIndex) ++knob.dropped;
    else {
      events[writeIndex].knob = knob.index;
      events[writeIndex].direction = step;
      writeIndex = next;
    }
  }
  portEXIT_CRITICAL_ISR(&encoderMux);
}

void resetDecoder(uint8_t index) {
  portENTER_CRITICAL(&encoderMux);
  Knob& knob = knobs[index];
  knob.decoder.begin(readAB(knob),knob.edges);
  knob.rawEdges = knob.dropped = 0;
  // Remove only this knob's queued events; other knobs keep their order.
  const uint8_t end = writeIndex;
  uint8_t destination = readIndex;
  for (uint8_t source=readIndex;source!=end;source=(source+1)%kQueueSize) {
    if (events[source].knob == index) continue;
    events[destination].knob = events[source].knob;
    events[destination].direction = events[source].direction;
    destination = (destination+1)%kQueueSize;
  }
  writeIndex = destination;
  portEXIT_CRITICAL(&encoderMux);
}

void header() {
#if defined(PID_HOST_UI)
  const uint32_t now = millis();
  Serial.printf("HOST_PANEL version=%s mode=%s idle_ms=%lu last_input_age_ms=%lu "
                "rgb_style=gradient_breath flow=%s rgb_transition=%u "
                "rgb_background=complement rgb_accent_hue_deg=%u rgb_manual_cycle_ms=%lu "
                "rgb_motion=subpixel_spring rgb_position_milli=%u rgb_frame_ms=%lu input_reports=%s ",
      "20261019",
      hostUi.active(now) ? "comet" : "breathing", static_cast<unsigned long>(hostui::kIdleMs),
      static_cast<unsigned long>(hostUi.age(now)), hostUi.active(now) ? "manual" : "gradient",
      unsigned(hostUi.transitioning(now)), unsigned(hostUi.accentHueDegrees(now)),
      static_cast<unsigned long>(hostui::kManualColorPeriodMs),
      unsigned(hostUi.visualPosition(now)*1000.0f), static_cast<unsigned long>(hostui::kRgbFrameMs),
#if defined(PID_HOST_LOCAL)
      "json");
#else
      "compact");
#endif
#else
  Serial.print("KNOB_ENCODER version=20261014 ");
#endif
  Serial.flush();
  Serial.printf("knobs=4 display=%s selected=%s "
                "rgb_pin=47 leds=40 brightness=%u brightness_limit=%u frames=%lu uptime_ms=%lu",
      scene.independent() ? "independent":"shared",
      stepcontrol::kKnobs[scene.selected()].name, unsigned(brightness), unsigned(kBrightnessLimit),
      static_cast<unsigned long>(frames),static_cast<unsigned long>(millis()));
  Serial.flush();
#if defined(PID_HOST_UI)
  Serial.printf(" rgb_enabled=%u step_hold_ms=%lu step_holds=%lu",
      unsigned(hostUi.rgbEnabled()), static_cast<unsigned long>(stepcontrol::kButtonHoldMs),
      static_cast<unsigned long>(stepHolds));
  Serial.flush();
#endif
#if defined(PID_HOST_UI)
  const hostoled::Health display = hostoled::health();
  Serial.printf(" oled_ack=%u oled_address=0x%02X oled_error=%u oled_frames=%lu",
      unsigned(display.acknowledged), unsigned(display.address), unsigned(display.error),
      static_cast<unsigned long>(display.frames));
  Serial.flush();
#endif
#if defined(PID_HOST_LOCAL)
  const hostapp::Status local=hostapp::status();
  Serial.printf(" app=%s wireless=%s profile_saved=%u profile_dirty=%u nvs_ready=%u "
                "run_requested=%u run_request_id=%lu run_ack=%u run_pin=18 save_pin=21 run_raw=%u save_raw=%u",
#if defined(PID_LINK_ENABLED)
      "host_wireless","espnow",
#else
      "local_usb","off",
#endif
      unsigned(local.saved),unsigned(local.dirty),unsigned(local.storage),unsigned(local.runRequested),
      static_cast<unsigned long>(local.runRequest),unsigned(local.runAcknowledged),
      unsigned(digitalRead(18)),unsigned(digitalRead(21)));
  Serial.flush();
#endif
#if defined(PID_RGB_DIRECT_RMT)
  Serial.printf(" rgb_driver=persistent_rmt rgb_dither=%u rgb_test=%u rgb_error=%d rgb_sent=%lu "
                "rgb_failures=%lu lit_pixels=%u\n", unsigned(rgbOutput.dithering()), unsigned(rgbTest), int(rgbOutput.error()),
      static_cast<unsigned long>(rgbOutput.sent()),
      static_cast<unsigned long>(rgbOutput.failures()), unsigned(rgbOutput.litPixels()));
#else
  Serial.print('\n');
#endif
}

void knobStatus(uint8_t index) {
  Knob& knob = knobs[index];
  uint32_t raw, invalid, dropped;
  uint8_t ab, edges;
  bool reverse;
  portENTER_CRITICAL(&encoderMux);
  raw = knob.rawEdges; invalid = knob.decoder.invalid(); dropped = knob.dropped;
  ab = readAB(knob); edges = knob.edges; reverse = knob.reverse;
  portEXIT_CRITICAL(&encoderMux);
  char line[384];
#if defined(PID_HOST_UI)
  char numericValue[16];
  hostui::formatValue(hostUi.value(index), numericValue, sizeof(numericValue));
#endif
  snprintf(line,sizeof(line),
      "KNOB name=%s pin_a=%u pin_b=%u pin_sw=%u a=%u b=%u sw=%u "
      "position=%u color=%s palette=%u cw=%lu ccw=%lu presses=%lu "
      "edges_per_step=%u reverse=%u raw_edges=%lu invalid=%lu dropped=%lu simulated=%lu"
#if defined(PID_HOST_UI)
      " value=%s"
#endif
      "\n",
      stepcontrol::kKnobs[index].name,unsigned(knob.a),unsigned(knob.b),unsigned(knob.sw),
      unsigned(ab>>1),unsigned(ab&1),unsigned(digitalRead(knob.sw)),
      unsigned(scene.pixel(index)+1),stepcontrol::kColorNames[
#if defined(PID_HOST_UI)
      hostUi.color(index)], unsigned(hostUi.color(index)+1),
#else
      scene.color(index)], unsigned(scene.color(index)+1),
#endif
      static_cast<unsigned long>(knob.cw),
      static_cast<unsigned long>(knob.ccw),static_cast<unsigned long>(knob.presses),
      unsigned(edges),unsigned(reverse),static_cast<unsigned long>(raw),
      static_cast<unsigned long>(invalid),static_cast<unsigned long>(dropped),
      static_cast<unsigned long>(knob.simulated)
#if defined(PID_HOST_UI)
      ,numericValue
#endif
      );
  Serial.print(line);
}

void status(uint8_t mask=15) {
  header();
  Serial.flush();
  for (uint8_t i=0;i<stepcontrol::kKnobCount;++i) {
    if (mask & (1<<i)) {
      knobStatus(i);
      // Drain complete lines before writing the next native-USB buffer chunk.
      Serial.flush();
    }
  }
  // A final short transfer releases replies whose last line fills whole USB packets.
  delay(1);
  Serial.print('\n');
  Serial.flush();
}

void serviceKnobs(uint32_t now) {
  for (uint8_t count=0;count<kQueueSize;++count) {
    Event event = {};
    bool available = false;
    portENTER_CRITICAL(&encoderMux);
    if (readIndex != writeIndex) {
      event.knob = events[readIndex].knob;
      event.direction = events[readIndex].direction;
      readIndex = (readIndex+1)%kQueueSize;
      available = true;
    }
    portEXIT_CRITICAL(&encoderMux);
    if (!available) break;
#if defined(PID_HOST_UI)
    hostUi.move(event.knob,event.direction,now);
#else
    scene.move(event.knob,event.direction);
#endif
    Knob& knob = knobs[event.knob];
    if (event.direction>0) ++knob.cw; else ++knob.ccw;
    reportMask |= 1<<event.knob;
    dirty = true;
  }
  for (uint8_t i=0;i<stepcontrol::kKnobCount;++i) {
    bool clicked = false;
#if defined(PID_LINK_ENABLED)
    if(i==0){const auto event=peerButton.update(digitalRead(knobs[i].sw)==LOW,now);
      if(event==stepcontrol::ClickEvent::Double)hostapp::nextPeer(now);
      // P is reserved for peer selection. A single click has no action.
      clicked=false;
    }else
#endif
#if defined(PID_HOST_UI)
    if (i == 3) {
      const auto event = stepButton.update(digitalRead(knobs[i].sw)==LOW,now);
      if (event == stepcontrol::ButtonEvent::Hold) {
        hostUi.toggleRgb();
        ++stepHolds;
        dirty = true;
      }
      clicked = event == stepcontrol::ButtonEvent::Click;
    } else
#endif
      clicked = knobs[i].button.update(digitalRead(knobs[i].sw)==LOW,now);
    if (clicked) {
#if defined(PID_HOST_UI)
      hostUi.press(i,now);
#else
      scene.nextColor(i);
#endif
      ++knobs[i].presses;
      reportMask |= 1<<i;
      dirty = true;
    }
  }
}

#if defined(PID_HOST_UI)
void reportInputs(uint32_t now) {
  if (!reportMask || !Serial || uint32_t(now-lastReport)<50) return;
  // Never wait for a host reader from the animation loop. Keep the latest values
  // pending when the USB buffer is full; explicit !status replies remain complete.
  Serial.setTxTimeoutMs(0);
  for (uint8_t i=0;i<stepcontrol::kKnobCount;++i) {
    if (!(reportMask & (1<<i))) continue;
    char value[16], line[100];
    hostui::formatValue(hostUi.value(i),value,sizeof(value));
    const int length = snprintf(line,sizeof(line),"INPUT name=%s value=%s position=%u\n",
        stepcontrol::kKnobs[i].name,value,unsigned(scene.pixel(i)+1));
    if (length>0 && size_t(length)<sizeof(line) && Serial.availableForWrite()>=length &&
        Serial.write(reinterpret_cast<const uint8_t*>(line),size_t(length))==size_t(length))
      reportMask &= ~(1<<i);
  }
  Serial.setTxTimeoutMs(250);
  lastReport = now;
}
#endif

bool number(const char* text, unsigned maximum, unsigned& result) {
  if (!*text) return false;
  unsigned n = 0;
  for (;*text;++text) {
    if (*text<'0' || *text>'9') return false;
    n = n*10+(*text-'0');
    if (n>maximum) return false;
  }
  result = n;
  return true;
}

int knobIndex(const char* text) {
  for (uint8_t i=0;i<stepcontrol::kKnobCount;++i) {
    const char* name = stepcontrol::kKnobs[i].name;
    size_t c=0;
    for (;text[c] && name[c];++c) {
      char letter = text[c];
      if (letter>='a' && letter<='z') letter -= 'a'-'A';
      if (letter!=name[c]) break;
    }
    if (!text[c] && !name[c]) return i;
  }
  return -1;
}

void execute() {
#if defined(PID_HOST_LOCAL)
  if (hostapp::handleCommand(command,millis())) return;
#endif
  char* save = nullptr;
  const char* name = strtok_r(command," \t",&save);
  const char* arg = strtok_r(nullptr," \t",&save);
  const char* extra = strtok_r(nullptr," \t",&save);
  if (!name || extra) { Serial.print("ERR invalid command\n"); return; }
  const uint8_t selected = scene.selected();
  unsigned value = 0;
  if (!strcmp(name,"help") && !arg) {
    Serial.printf("KNOBS: !status, ?, !knob P/I/D/STEP, !display independent/shared, "
        "!reverse on/off, !edges 2/4, !color next/1..8, !brightness 0..%u, !reset. "
        "Test only: !move cw/ccw (simulated). Per-knob commands use selected knob.\n", unsigned(kBrightnessLimit));
#if defined(PID_HOST_UI)
    Serial.print("RGB: hold STEP for 2000ms to toggle; short click changes color on release. "
                 "!rgb on/off/toggle; !status reports rgb_enabled and step_holds.\n");
#if defined(PID_HOST_LOCAL)
    Serial.print("HOST_LOCAL: !get, !pid P I D, !step VALUE, !run, !stop, !ackrun ID 0/1, "
        "!save, !load, !forget, !stream on/off, !feedback CHANNEL TARGET MEASURED OUTPUT. "
        "SAVE stores local P/I/D/STEP; run requests do not drive a target.\n");
#else
    Serial.print("HOST_PANEL: shared cursor only; idle breath after 5000ms. "
        "P/I/D use STEP=0.01/0.10/1.00/10.00, values 0.00..9999.99; RAM only.\n");
#endif
#endif
#if defined(PID_RGB_DIRECT_RMT)
    Serial.print("RMT comparison: !rgbtest on/off (all 40 LEDs, R/G/B every 2 seconds).\n");
#endif
    return;
  } else if (!strcmp(name,"status") && !arg) { status(); return; }
  else if (!strcmp(name,"knob") && arg && knobIndex(arg)>=0) {
    scene.select(knobIndex(arg)); dirty = true;
  } else if (!strcmp(name,"display") && arg &&
             (!strcmp(arg,"independent") || !strcmp(arg,"shared"))) {
#if defined(PID_HOST_UI)
    if (!strcmp(arg,"independent")) {
      Serial.print("ERR HOST_PANEL uses one shared cursor\n"); return;
    }
#endif
    scene.setIndependent(!strcmp(arg,"independent")); dirty = true;
  } else if (!strcmp(name,"reverse") && arg &&
             (!strcmp(arg,"on") || !strcmp(arg,"off"))) {
    portENTER_CRITICAL(&encoderMux);
    knobs[selected].reverse = !strcmp(arg,"on");
    portEXIT_CRITICAL(&encoderMux);
    resetDecoder(selected);
  } else if (!strcmp(name,"edges") && arg &&
             (!strcmp(arg,"2") || !strcmp(arg,"4"))) {
    portENTER_CRITICAL(&encoderMux);
    knobs[selected].edges = arg[0]-'0';
    portEXIT_CRITICAL(&encoderMux);
    resetDecoder(selected);
  } else if (!strcmp(name,"color") && arg && !strcmp(arg,"next")) {
#if defined(PID_HOST_UI)
    hostUi.press(selected,millis());
#else
    scene.nextColor(selected);
#endif
    dirty = true;
  } else if (!strcmp(name,"color") && arg && number(arg,8,value) && value>=1) {
#if defined(PID_HOST_UI)
    hostUi.setColor(selected,value-1,millis());
#else
    scene.setColor(selected,value-1);
#endif
    dirty = true;
  } else if (!strcmp(name,"brightness") && arg && number(arg,kBrightnessLimit,value)) {
    brightness = value; dirty = true;
#if defined(PID_HOST_UI)
  } else if (!strcmp(name,"rgb") && arg &&
             (!strcmp(arg,"on") || !strcmp(arg,"off") || !strcmp(arg,"toggle"))) {
    if (!strcmp(arg,"toggle")) hostUi.toggleRgb();
    else hostUi.setRgbEnabled(!strcmp(arg,"on"));
    dirty = true;
#endif
  } else if (!strcmp(name,"move") && arg &&
             (!strcmp(arg,"cw") || !strcmp(arg,"ccw"))) {
#if defined(PID_HOST_UI)
    hostUi.move(selected,!strcmp(arg,"cw") ? 1:-1,millis());
#else
    scene.move(selected,!strcmp(arg,"cw") ? 1:-1);
#endif
    ++knobs[selected].simulated; dirty = true;
#if defined(PID_RGB_DIRECT_RMT)
  } else if (!strcmp(name,"rgbtest") && arg &&
             (!strcmp(arg,"on") || !strcmp(arg,"off"))) {
    rgbTest = !strcmp(arg,"on");
    rgbTestStart = millis(); dirty = true;
#endif
  } else if (!strcmp(name,"reset") && !arg) {
#if defined(PID_HOST_UI)
    hostUi.reset(millis());
#else
    scene.reset();
#endif
    for (uint8_t i=0;i<stepcontrol::kKnobCount;++i) {
      knobs[i].cw = knobs[i].ccw = knobs[i].presses = knobs[i].simulated = 0;
      resetDecoder(i);
      knobs[i].button.begin(digitalRead(knobs[i].sw)==LOW,millis());
    }
#if defined(PID_HOST_UI)
    stepButton.begin(digitalRead(stepcontrol::kPinSwitch)==LOW,millis());
#if defined(PID_LINK_ENABLED)
    peerButton.begin(digitalRead(knobs[0].sw)==LOW,millis());
#endif
    stepHolds = 0;
#endif
    dirty = true;
  } else {
    Serial.print("ERR unknown command or invalid argument; send !help\n"); return;
  }
  status();
}

void serviceUSB(uint32_t now) {
  bool replied = false;
  if (collecting && uint32_t(now-lastByte)>=2000) {
    collecting = overflow = false; commandLength = 0;
    Serial.print("ERR incomplete command; send a newline\n"); replied = true;
  }
  for (uint16_t count=0;count<1024 && Serial.available()>0;++count) {
#if defined(PID_HOST_LOCAL)
    if (!hostapp::canAcceptCommand()
#if defined(PID_FLASH_ENABLED)
        &&!pidflash::hostBinaryReading()&&!pidflash::hostBulkActive()
#endif
        ) break;
#endif
    const int value = Serial.read();
    if (value<0) break;
#if defined(PID_FLASH_ENABLED)
    if(pidflash::hostBinaryByte(uint8_t(value),now))continue;
#endif
#if defined(PID_HOST_LOCAL)
    if(!hostapp::canAcceptCommand())continue;
#endif
    const char c = static_cast<char>(value);
    if (skipLF) { skipLF = false; if (c=='\n') continue; }
    if (collecting) {
      lastByte = now;
      if (c=='\r' || c=='\n') {
        if (overflow) Serial.print("ERR command too long or invalid text\n");
        else { command[commandLength]='\0'; execute(); }
        skipLF = c=='\r'; collecting = overflow = false; commandLength = 0; replied = true;
      } else if ((c<32 && c!='\t') || c>126) overflow = true;
      else if (commandLength+1<sizeof(command)) command[commandLength++]=c;
      else overflow = true;
    } else if (c=='!') {
      collecting = true; overflow = false; commandLength = 0; lastByte = now;
    } else if (c=='?') { status(); replied = true; }
    else { Serial.write(static_cast<uint8_t>(value)); replied = true; }
  }
  if (replied
#if defined(PID_HOST_LOCAL)
      && hostapp::canAcceptCommand()
#endif
      ) Serial.flush();
}

void render(uint32_t now) {
  uint8_t outputBrightness = brightness;
#if defined(PID_HOST_UI)
  hostUi.render(now,pixels);
  if (!hostUi.rgbEnabled()) outputBrightness = 0;
#else
  scene.render(pixels);
#endif
#if defined(PID_RGB_DIRECT_RMT)
  if (rgbTest) {
    const uint8_t phase = uint32_t(now-rgbTestStart)/2000%3;
    for (uint16_t i=0;i<rgbshow::kLedCount;++i)
      pixels[i] = {uint8_t(phase==0 ? 255:0), uint8_t(phase==1 ? 255:0),
                   uint8_t(phase==2 ? 255:0)};
  }
  rgbOutput.write(pixels,outputBrightness);
#else
  strip.setBrightness(outputBrightness);
  for (uint16_t i=0;i<rgbshow::kLedCount;++i)
    strip.setPixelColor(i,pixels[i].r,pixels[i].g,pixels[i].b);
  strip.show();
#endif
  ++frames; lastFrame = now; dirty = false;
}
}  // namespace

void setup() {
#if defined(PID_FLASH_ENABLED)
  // A 128-byte block uses 256 hex characters plus command metadata/newline.
  Serial.setRxBufferSize(8192);
#endif
  Serial.begin(115200);
  Serial.setDebugOutput(false);
#if defined(PID_HOST_UI)
  // Complete multi-line replies must tolerate Windows USB scheduling delays.
  Serial.setTxTimeoutMs(250);
#endif
  for (uint8_t i=0;i<stepcontrol::kKnobCount;++i) {
    Knob& knob = knobs[i];
    const stepcontrol::KnobPins& pins = stepcontrol::kKnobs[i];
    knob.index = i; knob.a = pins.a; knob.b = pins.b; knob.sw = pins.sw;
    pinMode(knob.a,INPUT_PULLUP); pinMode(knob.b,INPUT_PULLUP); pinMode(knob.sw,INPUT_PULLUP);
    resetDecoder(i);
    knob.button.begin(digitalRead(knob.sw)==LOW,millis());
  }
#if defined(PID_HOST_UI)
  hostUi.reset(millis());
  stepButton.begin(digitalRead(stepcontrol::kPinSwitch)==LOW,millis());
#if defined(PID_LINK_ENABLED)
  peerButton.begin(digitalRead(knobs[0].sw)==LOW,millis());
#endif
#if defined(PID_HOST_LOCAL)
  hostapp::begin(hostUi);
#endif
#else
  scene.reset();
#endif
  delay(150);
#if defined(PID_RGB_DIRECT_RMT)
  rgbOutput.begin();
  rgbTestStart = millis();
#else
  strip.begin();
#endif
  render(millis());
  for (uint8_t i=0;i<stepcontrol::kKnobCount;++i) {
    attachInterruptArg(knobs[i].a,onEdge,&knobs[i],CHANGE);
    attachInterruptArg(knobs[i].b,onEdge,&knobs[i],CHANGE);
  }
#if defined(PID_HOST_UI)
  hostoled::begin(hostUi.snapshot());
#endif
}

void loop() {
  uint32_t now = millis();
  serviceKnobs(now);
  serviceUSB(now);
  now = millis();  // USB commands may set a newer interaction timestamp.
#if defined(PID_HOST_LOCAL)
  hostapp::poll(now);
  if (hostapp::takeDisplayDirty()) dirty=true;
#endif
#if defined(PID_HOST_UI)
  static bool wasActive = false;
  const bool active = hostUi.active(now);
  if (active != wasActive) { dirty = true; wasActive = active; }
  if (dirty) hostoled::publish(hostUi.snapshot());
  // Keep the manual cursor fixed when stopped, while its rainbow colors keep flowing.
  if (uint32_t(now-lastFrame)>=hostui::kRgbFrameMs) render(now);
#else
  // Retransmit identical data for recovery; position and trail never advance with time.
  if ((dirty && uint32_t(now-lastFrame)>=8) || uint32_t(now-lastFrame)>=250) render(now);
#endif
#if defined(PID_HOST_UI)
#if defined(PID_HOST_LOCAL)
  reportMask=0;
#else
  reportInputs(now);
#endif
#else
  if (reportMask && Serial && uint32_t(now-lastReport)>=50) {
    status(reportMask); reportMask = 0; lastReport = now;
  }
#endif
  delay(1);
}
