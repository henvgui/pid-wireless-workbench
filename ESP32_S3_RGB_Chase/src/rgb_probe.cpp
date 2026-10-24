#include <Arduino.h>
#include "rgb_show.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3) || ARDUINO_USB_MODE != 1 || ARDUINO_USB_CDC_ON_BOOT != 1
#error "Select ESP32-S3 with native hardware USB CDC enabled."
#endif

// Temporary RGB data-path diagnostic. No NeoPixel/RMT output is attached:
// H/L hold GPIO47 at a DC level so an ordinary multimeter can trace the signal.
namespace {
bool highLevel = false;
void status() {
  Serial.printf("RGB_PROBE gpio=%u level=%u uptime_ms=%lu\n",
      unsigned(rgbshow::kPin), unsigned(highLevel),
      static_cast<unsigned long>(millis()));
}
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  pinMode(rgbshow::kPin, OUTPUT);
  digitalWrite(rgbshow::kPin, LOW);
}

void loop() {
  bool replied = false;
  for (uint16_t count=0; count<128 && Serial.available()>0; ++count) {
    const int c = Serial.read();
    if (c=='H' || c=='h' || c=='L' || c=='l') {
      highLevel = c=='H' || c=='h';
      digitalWrite(rgbshow::kPin, highLevel ? HIGH : LOW);
      status();
    } else if (c=='?') status();
    else if (c>=0) Serial.write(static_cast<uint8_t>(c));
    replied = true;
  }
  if (replied) Serial.flush();
  delay(1);
}
