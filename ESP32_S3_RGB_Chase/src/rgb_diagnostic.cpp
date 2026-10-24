#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "rgb_show.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3) || ARDUINO_USB_MODE != 1 || ARDUINO_USB_CDC_ON_BOOT != 1
#error "Select ESP32-S3 with native hardware USB CDC enabled."
#endif

namespace {
Adafruit_NeoPixel strip(rgbshow::kLedCount,rgbshow::kPin,NEO_GRB+NEO_KHZ800);
uint8_t color = 0;
bool automatic = true, enabled = true, dirty = true;
uint32_t lastChange = 0, lastFrame = 0, frames = 0;
const char* const names[] = {"red","green","blue"};

void status() {
  Serial.printf("RGB_DIAGNOSTIC version=20261014 pin=47 leds=40 brightness=40 "
                "color=%s auto=%u enabled=%u frames=%lu uptime_ms=%lu\n",
                names[color], unsigned(automatic), unsigned(enabled),
                static_cast<unsigned long>(frames), static_cast<unsigned long>(millis()));
}

void render(uint32_t now) {
  const uint32_t value = !enabled ? 0 : strip.Color(color==0 ? 255:0,
      color==1 ? 255:0, color==2 ? 255:0);
  for (uint16_t i=0;i<rgbshow::kLedCount;++i) strip.setPixelColor(i,value);
  strip.show();
  ++frames;
  lastFrame = now;
  dirty = false;
}
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  // Give the RGB rail time to settle, then retransmit identical frames periodically.
  // This is a temporary diagnostic and does not read any encoders or start radios.
  delay(150);
  strip.begin();
  strip.setBrightness(rgbshow::kMaxBrightness);
  lastChange = millis();
  render(lastChange);
}

void loop() {
  bool replied = false;
  for (uint16_t count=0;count<128 && Serial.available()>0;++count) {
    const int c = Serial.read();
    if (c=='R' || c=='G' || c=='B') {
      color = c=='R' ? 0 : c=='G' ? 1 : 2;
      automatic = false;
      enabled = true;
      dirty = true;
      status();
    } else if (c=='A') {
      automatic = enabled = true;
      lastChange = millis();
      status();
    } else if (c=='O') {
      automatic = enabled = false;
      dirty = true;
      status();
    } else if (c=='?') status();
    else if (c>=0) Serial.write(static_cast<uint8_t>(c));
    replied = true;
  }
  if (replied) Serial.flush();
  const uint32_t now = millis();
  if (automatic && uint32_t(now-lastChange)>=2000) {
    color = (color+1)%3;
    lastChange = now;
    dirty = true;
  }
  if (dirty || uint32_t(now-lastFrame)>=250) render(now);
  delay(1);
}
