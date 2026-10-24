#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <cstring>
#include "host_oled.h"

namespace hostoled {
namespace {
// Wire owns GPIO8/9. Leave U8g2's hardware I2C pins unset (verified OLED fix).
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
portMUX_TYPE displayMux = portMUX_INITIALIZER_UNLOCKED;
hostui::Snapshot mailbox = {};
uint32_t revision = 0;
Health state = {};
#if defined(PID_HOST_LOCAL)
char headerMailbox[32] = "LOCAL *";
#endif
bool acknowledges(uint8_t address, uint8_t& error) {
  Wire.beginTransmission(address);
  error = Wire.endTransmission();
  return error == 0;
}

void worker(void*) {
  Health current = {};
  current.busReady = Wire.begin(8, 9, 100000);
  Wire.setTimeOut(25);
  bool initialized = false;
  uint32_t shown = 0, lastCheck = 0;
  for (;;) {
    hostui::Snapshot data;
    uint32_t next;
#if defined(PID_HOST_LOCAL)
    char header[32];
#endif
    portENTER_CRITICAL(&displayMux);
    data = mailbox;
    next = revision;
#if defined(PID_HOST_LOCAL)
    std::memcpy(header,headerMailbox,sizeof(header));
#endif
    portEXIT_CRITICAL(&displayMux);
    const uint32_t now = millis();
    if (current.busReady && (!initialized || next != shown || uint32_t(now-lastCheck)>=1000)) {
      if (initialized && !acknowledges(current.address, current.error)) initialized = false;
      if (!initialized) {
        current.address = 0;
        for (uint8_t address : {uint8_t(0x3C), uint8_t(0x3D)}) {
          if (!acknowledges(address, current.error)) continue;
          current.address = address;
          oled.setI2CAddress(address << 1);
          oled.setBusClock(100000);
          oled.begin();
          initialized = true;
          break;
        }
      }
      if (initialized) {
        oled.clearBuffer();
#if defined(PID_HOST_LOCAL)
        oled.setFont(u8g2_font_5x7_tf);
        oled.drawStr(1,8,header);
        oled.setFont(u8g2_font_6x12_tf);
#else
        oled.setFont(u8g2_font_7x14B_tf);
#endif
        for (uint8_t row = 0; row < 4; ++row) {
          const bool selected = data.selected == row;
#if defined(PID_HOST_LOCAL)
          const uint8_t y=11+row*13, height=13, baseline=y+10;
#else
          const uint8_t y=row*16, height=16, baseline=y+13;
#endif
          oled.setDrawColor(1);
          if (selected) oled.drawBox(0, y, 128, height);
          oled.setDrawColor(selected ? 0 : 1);
          char value[16];
          hostui::formatValue(row == 3 ? data.step : data.values[row], value, sizeof(value));
          oled.drawStr(3, baseline, stepcontrol::kKnobs[row].name);
          oled.drawStr(125-oled.getStrWidth(value), baseline, value);
        }
        oled.setDrawColor(1);
        oled.sendBuffer();
        ++current.frames;
        current.acknowledged = acknowledges(current.address, current.error);
        if (!current.acknowledged) initialized = false;
        shown = next;
      } else current.acknowledged = false;
      lastCheck = now;
      portENTER_CRITICAL(&displayMux);
      state = current;
      portEXIT_CRITICAL(&displayMux);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
}

void begin(const hostui::Snapshot& initial) {
  publish(initial);
  if (xTaskCreatePinnedToCore(worker, "host_oled", 4096, nullptr, 1, nullptr, 0) != pdPASS) {
    portENTER_CRITICAL(&displayMux);
    state.error = 255;
    portEXIT_CRITICAL(&displayMux);
  }
}

void publish(const hostui::Snapshot& value) {
  portENTER_CRITICAL(&displayMux);
  mailbox = value;
  ++revision;
  portEXIT_CRITICAL(&displayMux);
}

Health health() {
  portENTER_CRITICAL(&displayMux);
  const Health result = state;
  portEXIT_CRITICAL(&displayMux);
  return result;
}
#if defined(PID_HOST_LOCAL)
void publishHeader(const char* text) {
  portENTER_CRITICAL(&displayMux);
  std::strncpy(headerMailbox,text,sizeof(headerMailbox)-1);
  headerMailbox[sizeof(headerMailbox)-1]=0;
  ++revision;
  portEXIT_CRITICAL(&displayMux);
}
#endif
}  // namespace hostoled
