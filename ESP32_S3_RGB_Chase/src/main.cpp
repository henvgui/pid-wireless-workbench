#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "rgb_show.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3) || ARDUINO_USB_MODE != 1 || ARDUINO_USB_CDC_ON_BOOT != 1
#error "Select ESP32-S3 with native hardware USB CDC enabled."
#endif

namespace {
Adafruit_NeoPixel strip(rgbshow::kLedCount,rgbshow::kPin,NEO_GRB+NEO_KHZ800);
rgbshow::Show show;
rgbshow::Color frame[rgbshow::kLedCount];
uint32_t lastFrameMs = 0;
uint8_t appliedBrightness = rgbshow::kMaxBrightness;
char commandLine[128];
char reply[768];
size_t commandLength = 0;
bool collecting = false;
bool overflowed = false;
bool skipLF = false;
uint32_t lastCommandByteMs = 0;

void renderFrame(uint32_t now) {
  show.update(now);
  show.render(frame);
  if (appliedBrightness!=show.brightness()) {
    appliedBrightness = show.brightness();
    strip.setBrightness(appliedBrightness);
  }
  for (uint16_t i=0; i<rgbshow::kLedCount; ++i) {
    const rgbshow::Color c = frame[i];
    strip.setPixelColor(i,
        Adafruit_NeoPixel::gamma8(static_cast<uint8_t>(c.r*255)),
        Adafruit_NeoPixel::gamma8(static_cast<uint8_t>(c.g*255)),
        Adafruit_NeoPixel::gamma8(static_cast<uint8_t>(c.b*255)));
  }
  strip.show();
}

void serviceUSB() {
  bool replied = false;
  const uint32_t now = millis();
  if (collecting && now-lastCommandByteMs>=2000) {
    collecting = overflowed = false;
    commandLength = 0;
    Serial.print("ERR incomplete command; send a newline\n");
    replied = true;
  }
  // 每轮处理的字节有上限，连续USB输入不会饿死灯效刷新。
  for (uint16_t received=0; received<256 && Serial.available()>0; ++received) {
    const int value = Serial.read();
    if (value<0) break;
    const char c = static_cast<char>(value);
    if (skipLF) {
      skipLF = false;
      if (c=='\n') continue;
    }
    if (collecting) {
      lastCommandByteMs = millis();
      if (c=='\n' || c=='\r') {
        if (overflowed) {
          Serial.print("ERR command too long\n");
        } else {
          commandLine[commandLength]='\0';
          show.command(commandLine,millis(),reply,sizeof(reply));
          Serial.print(reply);
        }
        skipLF = c=='\r';
        collecting = overflowed = false;
        commandLength = 0;
        replied = true;
      } else if ((c<32 && c!='\t') || c>126) {
        overflowed = true;
      } else if (commandLength+1<sizeof(commandLine)) {
        commandLine[commandLength++]=c;
      } else {
        overflowed = true;
      }
    } else if (c=='!') {
      collecting = true;
      lastCommandByteMs = millis();
      commandLength = 0;
    } else if (c=='?') {
      show.update(millis());
      show.status(millis(),reply,sizeof(reply));
      Serial.print(reply);
      replied = true;
    } else {
      Serial.write(static_cast<uint8_t>(value));
      replied = true;
    }
  }
  if (replied) {
    // 沿用已验证的顺序：先完成USB回复，再发送下一帧RGB数据。
    Serial.flush();
  }
}
}  // namespace

void setup() {
  // 不初始化Wi-Fi、蓝牙；保留原生USB文本回显。
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  strip.begin();
  strip.setBrightness(appliedBrightness);
  lastFrameMs = millis();
  show.begin(lastFrameMs);
  renderFrame(lastFrameMs);
}
void loop() {
  serviceUSB();
  const uint32_t now = millis();
  if (now-lastFrameMs>=rgbshow::kFrameMs) {
    lastFrameMs = now;
    renderFrame(now);
  }
  delay(1);
}
