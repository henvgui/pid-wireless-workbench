#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <cstdio>
#include <cstring>
#include "soc/gpio_reg.h"
#include "step_control.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3) || ARDUINO_USB_MODE != 1 || ARDUINO_USB_CDC_ON_BOOT != 1
#error "Select ESP32-S3 with native hardware USB CDC enabled."
#endif

namespace {
Adafruit_NeoPixel strip(rgbshow::kLedCount, rgbshow::kPin, NEO_GRB+NEO_KHZ800);
stepcontrol::Quadrature encoder;
stepcontrol::Button button;
stepcontrol::Comet comet;
stepcontrol::Rgb pixels[rgbshow::kLedCount];
portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;
constexpr uint8_t kQueueSize = 64;
volatile int8_t queue[kQueueSize];
volatile uint8_t readIndex = 0, writeIndex = 0;
volatile uint32_t rawEdges = 0, dropped = 0;
uint32_t stepsCW = 0, stepsCCW = 0, presses = 0, simulated = 0, frames = 0;
uint32_t lastFrameMs = 0, lastReportMs = 0;
uint8_t brightness = rgbshow::kMaxBrightness, edgesPerStep = 4;
bool reverseEncoder = false, dirty = true, reportPending = false;
char command[128];
size_t commandLength = 0;
bool collecting = false, overflow = false, skipLF = false;
uint32_t lastByteMs = 0;

uint8_t IRAM_ATTR readAB() {
  const uint32_t bits = REG_READ(GPIO_IN_REG);
  return (((bits >> stepcontrol::kPinA) & 1) << 1) |
          ((bits >> stepcontrol::kPinB) & 1);
}

void IRAM_ATTR onEncoderEdge() {
  portENTER_CRITICAL_ISR(&encoderMux);
  ++rawEdges;
  const int8_t step = encoder.onEdge(readAB());
  if (step) {
    const uint8_t next = (writeIndex + 1) % kQueueSize;
    if (next == readIndex) ++dropped;
    else { queue[writeIndex] = step; writeIndex = next; }
  }
  portEXIT_CRITICAL_ISR(&encoderMux);
}

void resetDecoder() {
  portENTER_CRITICAL(&encoderMux);
  encoder.begin(readAB(), edgesPerStep);
  readIndex = writeIndex = 0;
  rawEdges = dropped = 0;
  portEXIT_CRITICAL(&encoderMux);
}

void status() {
  uint32_t edgeCount, invalidCount, dropCount;
  uint8_t ab;
  portENTER_CRITICAL(&encoderMux);
  edgeCount = rawEdges; invalidCount = encoder.invalid(); dropCount = dropped;
  ab = readAB();
  portEXIT_CRITICAL(&encoderMux);
  char line[512];
  snprintf(line, sizeof(line),
      "STEP_ENCODER version=20261013 pin_a=15 pin_b=16 pin_sw=17 rgb_pin=47 leds=40 "
      "a=%u b=%u sw=%u position=%u color=%s palette=%u brightness=%u "
      "cw=%lu ccw=%lu presses=%lu edges_per_step=%u reverse=%u "
      "raw_edges=%lu invalid=%lu dropped=%lu simulated=%lu frames=%lu uptime_ms=%lu\n",
      unsigned(ab>>1), unsigned(ab&1), unsigned(digitalRead(stepcontrol::kPinSwitch)),
      unsigned(comet.pixel()+1), stepcontrol::kColorNames[comet.color()],
      unsigned(comet.color()+1), unsigned(brightness),
      static_cast<unsigned long>(stepsCW), static_cast<unsigned long>(stepsCCW),
      static_cast<unsigned long>(presses), unsigned(edgesPerStep), unsigned(reverseEncoder),
      static_cast<unsigned long>(edgeCount), static_cast<unsigned long>(invalidCount),
      static_cast<unsigned long>(dropCount), static_cast<unsigned long>(simulated),
      static_cast<unsigned long>(frames), static_cast<unsigned long>(millis()));
  Serial.print(line);
}

void serviceEncoder(uint32_t now) {
  for (uint8_t i=0; i<kQueueSize; ++i) {
    int8_t step = 0;
    portENTER_CRITICAL(&encoderMux);
    if (readIndex != writeIndex) {
      step = queue[readIndex]; readIndex = (readIndex+1)%kQueueSize;
    }
    portEXIT_CRITICAL(&encoderMux);
    if (!step) break;
    if (reverseEncoder) step = -step;
    comet.move(step);
    if (step > 0) ++stepsCW; else ++stepsCCW;
    dirty = reportPending = true;
  }
  if (button.update(digitalRead(stepcontrol::kPinSwitch)==LOW, now)) {
    comet.nextColor();
    ++presses;
    dirty = reportPending = true;
  }
}

bool number(const char* text, unsigned maximum, unsigned& result) {
  if (!*text) return false;
  unsigned n = 0;
  for (; *text; ++text) {
    if (*text<'0' || *text>'9') return false;
    n = n*10 + (*text-'0');
    if (n > maximum) return false;
  }
  result = n;
  return true;
}

void execute() {
  char* save = nullptr;
  const char* name = strtok_r(command, " \t", &save);
  const char* arg = strtok_r(nullptr, " \t", &save);
  const char* extra = strtok_r(nullptr, " \t", &save);
  if (!name || extra) { Serial.print("ERR invalid command\n"); return; }
  unsigned value = 0;
  if (!strcmp(name,"help") && !arg) {
    Serial.print("STEP: !status, ?, !reverse on/off, !edges 2/4, !color 1..8/next, "
                 "!brightness 0..40, !reset. Test only: !move cw/ccw (simulated).\n");
    return;
  } else if (!strcmp(name,"status") && !arg) {
    status(); return;
  } else if (!strcmp(name,"reverse") && arg &&
             (!strcmp(arg,"on") || !strcmp(arg,"off"))) {
    reverseEncoder = !strcmp(arg,"on");
    resetDecoder();
  } else if (!strcmp(name,"edges") && arg &&
             (!strcmp(arg,"2") || !strcmp(arg,"4"))) {
    edgesPerStep = arg[0]-'0';
    resetDecoder();
  } else if (!strcmp(name,"color") && arg && !strcmp(arg,"next")) {
    comet.nextColor(); dirty = true;
  } else if (!strcmp(name,"color") && arg && number(arg,8,value) && value>=1) {
    comet.setColor(value-1); dirty = true;
  } else if (!strcmp(name,"brightness") && arg && number(arg,40,value)) {
    brightness = value; dirty = true;
  } else if (!strcmp(name,"move") && arg &&
             (!strcmp(arg,"cw") || !strcmp(arg,"ccw"))) {
    comet.move(!strcmp(arg,"cw") ? 1 : -1);
    ++simulated;
    dirty = true;
  } else if (!strcmp(name,"reset") && !arg) {
    comet.reset();
    stepsCW = stepsCCW = presses = simulated = 0;
    resetDecoder();
    button.begin(digitalRead(stepcontrol::kPinSwitch)==LOW, millis());
    dirty = true;
  } else {
    Serial.print("ERR unknown command or invalid argument; send !help\n");
    return;
  }
  status();
}

void serviceUSB(uint32_t now) {
  bool replied = false;
  if (collecting && uint32_t(now-lastByteMs)>=2000) {
    collecting = overflow = false;
    commandLength = 0;
    Serial.print("ERR incomplete command; send a newline\n");
    replied = true;
  }
  for (uint16_t count=0; count<256 && Serial.available()>0; ++count) {
    const int value = Serial.read();
    if (value<0) break;
    const char c = static_cast<char>(value);
    if (skipLF) { skipLF = false; if (c=='\n') continue; }
    if (collecting) {
      lastByteMs = now;
      if (c=='\n' || c=='\r') {
        if (overflow) Serial.print("ERR command too long or invalid text\n");
        else { command[commandLength] = '\0'; execute(); }
        skipLF = c=='\r';
        collecting = overflow = false;
        commandLength = 0;
        replied = true;
      } else if ((c<32 && c!='\t') || c>126) overflow = true;
      else if (commandLength+1 < sizeof(command)) command[commandLength++] = c;
      else overflow = true;
    } else if (c=='!') {
      collecting = true; overflow = false; commandLength = 0; lastByteMs = now;
    } else if (c=='?') {
      status(); replied = true;
    } else { Serial.write(static_cast<uint8_t>(value)); replied = true; }
  }
  if (replied) Serial.flush();
}

void render(uint32_t now) {
  comet.render(pixels);
  strip.setBrightness(brightness);
  for (uint16_t i=0; i<rgbshow::kLedCount; ++i)
    strip.setPixelColor(i,pixels[i].r,pixels[i].g,pixels[i].b);
  strip.show();
  ++frames;
  lastFrameMs = now;
  dirty = false;
}
}  // namespace

void setup() {
  // The encoder has external 10k pull-ups; internal pull-ups also keep inputs defined.
  // No Wi-Fi, Bluetooth, OLED, or other unsoldered controls are initialized.
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  pinMode(stepcontrol::kPinA,INPUT_PULLUP);
  pinMode(stepcontrol::kPinB,INPUT_PULLUP);
  pinMode(stepcontrol::kPinSwitch,INPUT_PULLUP);
  resetDecoder();
  button.begin(digitalRead(stepcontrol::kPinSwitch)==LOW,millis());
  comet.reset();
  attachInterrupt(stepcontrol::kPinA,onEncoderEdge,CHANGE);
  attachInterrupt(stepcontrol::kPinB,onEncoderEdge,CHANGE);
  strip.begin();
  render(millis());
}

void loop() {
  const uint32_t now = millis();
  serviceEncoder(now);
  serviceUSB(now);
  if (dirty && uint32_t(now-lastFrameMs)>=8) render(now);
  if (reportPending && Serial && uint32_t(now-lastReportMs)>=50) {
    status();
    lastReportMs = now;
    reportPending = false;
  }
  delay(1);
}
