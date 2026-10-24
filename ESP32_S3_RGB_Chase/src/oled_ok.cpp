#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

namespace {
// Original host PCB: OLED1 SDA -> module pad 12 / GPIO8,
// SCL -> module pad 17 / GPIO9. OLED supply is 3.3 V.
constexpr uint8_t kSda = 8, kScl = 9;
constexpr uint32_t kBusClock = 100000;
// Wire owns the configured pins. Supplying them again to U8g2 makes its GPIO
// init call pinMode(OUTPUT) after Wire.begin(), replacing the I2C pin routing.
#if defined(OLED_USE_SH1106) && OLED_USE_SH1106
constexpr const char* kDriver = "SH1106";
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#else
constexpr const char* kDriver = "SSD1306";
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#endif
volatile bool busReady = false, displayReady = false, scanRequested = false;
volatile uint8_t address = 0, lastI2cError = 0;
volatile uint32_t draws = 0;
const char* volatile stage = "startup";

bool acknowledges(uint8_t candidate) {
  stage = "address_probe";
  Wire.beginTransmission(candidate);
  lastI2cError = Wire.endTransmission();
  stage = "probe_done";
  return lastI2cError == 0;
}

bool startDisplay() {
  for (uint8_t candidate : {uint8_t(0x3C), uint8_t(0x3D)}) {
    if (!acknowledges(candidate)) continue;
    address = candidate;
    oled.setI2CAddress(address << 1);
    oled.setBusClock(kBusClock);
    stage = "oled_init";
    oled.begin();
    oled.setPowerSave(0);
    oled.clearBuffer();
    oled.setFont(u8g2_font_logisoso24_tf);
    const int x = (oled.getDisplayWidth() - oled.getStrWidth("OK")) / 2;
    const int y = (oled.getDisplayHeight() + oled.getAscent() + oled.getDescent()) / 2;
    oled.drawStr(x, y, "OK");
    return true;
  }
  address = 0;
  return false;
}

void report() {
  Serial.printf("OLED_OK driver=%s width=128 height=64 sda=8 scl=9 "
                "i2c_ready=%u oled_ack=%u address=0x%02X draws=%lu uptime_ms=%lu "
                "sda_level=%u scl_level=%u last_i2c_error=%u stage=%s\n",
                kDriver, unsigned(busReady), unsigned(displayReady), unsigned(address),
                static_cast<unsigned long>(draws), static_cast<unsigned long>(millis()),
                unsigned(digitalRead(kSda)), unsigned(digitalRead(kScl)),
                unsigned(lastI2cError), stage);
}

void scanBus() {
  if (!busReady) { report(); return; }
  unsigned found = 0, nacks = 0, errors = 0, timeouts = 0;
  for (uint8_t candidate = 1; candidate < 127; ++candidate) {
    if (acknowledges(candidate)) {
      ++found;
      Serial.printf("I2C_DEVICE address=0x%02X\n", unsigned(candidate));
    } else if (lastI2cError == 2) ++nacks;
    else if (lastI2cError == 5) ++timeouts;
    else ++errors;
  }
  Serial.printf("I2C_SCAN devices=%u nacks=%u timeouts=%u errors=%u sda_level=%u scl_level=%u\n",
                found, nacks, timeouts, errors, unsigned(digitalRead(kSda)),
                unsigned(digitalRead(kScl)));
}

void updateDisplay() {
  if (!busReady) return;
  if (displayReady && !acknowledges(address)) displayReady = false;
  if (!displayReady) displayReady = startDisplay();
  if (displayReady) {
    stage = "frame_send";
    oled.sendBuffer();
    ++draws;
    displayReady = acknowledges(address);
  }
  stage = "idle";
}

void displayTask(void*) {
  // Keep all Wire/U8g2 calls on this worker. USB stays responsive if a bus call stalls.
  stage = "wire_begin";
  busReady = Wire.begin(kSda, kScl, kBusClock);
  Wire.setTimeOut(25);
  updateDisplay();
  uint32_t lastCheck = millis();
  for (;;) {
    if (scanRequested) {
      scanBus();
      scanRequested = false;
    }
    const uint32_t now = millis();
    if (uint32_t(now - lastCheck) >= 1000) {
      updateDisplay();
      lastCheck = now;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  Serial.setTxTimeoutMs(20);
  delay(150);
  if (xTaskCreatePinnedToCore(displayTask, "oled_test", 4096, nullptr, 1, nullptr, 0) != pdPASS)
    stage = "worker_start_failed";
}

void loop() {
  // No serial connection is required for startup. Diagnostics do not change pins.
  for (uint8_t count = 0; count < 64 && Serial.available(); ++count) {
    const int value = Serial.read();
    if (value == '?') report();
    else if (value == 'S') scanRequested = true;
    else if (value >= 0) Serial.write(static_cast<uint8_t>(value));
    Serial.flush();
  }
  static uint32_t lastReport = 0;
  if (Serial && uint32_t(millis() - lastReport) >= 2000) {
    report();
    Serial.flush();
    lastReport = millis();
  }
  delay(1);
}
