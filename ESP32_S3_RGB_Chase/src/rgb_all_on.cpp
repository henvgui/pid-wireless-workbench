#include <Arduino.h>
#include "rgb_rmt.h"
#include "driver/pcnt.h"
#include "driver/gpio.h"
#include "rom/gpio.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include "freertos/ringbuf.h"
#include <cstring>

#if !defined(CONFIG_IDF_TARGET_ESP32S3) || ARDUINO_USB_MODE != 1 || ARDUINO_USB_CDC_ON_BOOT != 1
#error "Select ESP32-S3 with native hardware USB CDC enabled."
#endif

namespace {
constexpr uint8_t kBrightness = 16;  // All 40 LEDs use all three channels for white.
RgbRmt output;
stepcontrol::Rgb pixels[rgbshow::kLedCount];
uint32_t lastFrame = 0;
bool pulseReady = false;
esp_err_t pulseError = ESP_ERR_INVALID_STATE;
int16_t calibrationEdges = -1, frameEdges = -1;
constexpr rmt_channel_t kCaptureChannel = RMT_CHANNEL_4;
RingbufHandle_t captureBuffer = nullptr;
rmt_item32_t capturedItems[1024];
uint16_t capturedWords = 0;
uint32_t captureTimeouts = 0;
esp_err_t captureError = ESP_ERR_INVALID_STATE;
bool captureReady = false;

bool pulseCheck(esp_err_t error) {
  pulseError = error;
  if (error != ESP_OK) pulseReady = false;
  return error == ESP_OK;
}

void beginPulseCounter() {
  pcnt_config_t config = {};
  // Route the existing output pad into PCNT manually. Letting pcnt_set_pin()
  // configure GPIO47 as input-only would disable the RGB output we are testing.
  config.pulse_gpio_num = PCNT_PIN_NOT_USED;
  config.ctrl_gpio_num = PCNT_PIN_NOT_USED;
  config.unit = PCNT_UNIT_0;
  config.channel = PCNT_CHANNEL_0;
  config.pos_mode = PCNT_COUNT_INC;
  config.neg_mode = PCNT_COUNT_DIS;
  config.lctrl_mode = config.hctrl_mode = PCNT_MODE_KEEP;
  config.counter_h_lim = 32767;
  config.counter_l_lim = -32768;
  if (!pulseCheck(pcnt_unit_config(&config))) return;
  if (!pulseCheck(pcnt_counter_pause(PCNT_UNIT_0))) return;
  if (!pulseCheck(pcnt_filter_disable(PCNT_UNIT_0))) return;
  gpio_matrix_in(rgbshow::kPin, PCNT_SIG_CH0_IN0_IDX, false);
  if (!pulseCheck(gpio_set_direction(static_cast<gpio_num_t>(rgbshow::kPin),GPIO_MODE_INPUT_OUTPUT))) return;
  pulseReady = true;
}

bool startCounting() {
  return pulseReady && pulseCheck(pcnt_counter_pause(PCNT_UNIT_0)) &&
      pulseCheck(pcnt_counter_clear(PCNT_UNIT_0)) &&
      pulseCheck(pcnt_counter_resume(PCNT_UNIT_0));
}

void stopCounting(int16_t& edges) {
  if (!pulseCheck(pcnt_counter_pause(PCNT_UNIT_0))) return;
  pulseCheck(pcnt_get_counter_value(PCNT_UNIT_0,&edges));
}

void calibrateCounter() {
  if (!startCounting()) return;
  for (uint8_t i=0;i<16;++i) {
    digitalWrite(rgbshow::kPin,HIGH);
    delayMicroseconds(10);
    digitalWrite(rgbshow::kPin,LOW);
    delayMicroseconds(10);
  }
  stopCounting(calibrationEdges);
  delayMicroseconds(300);
}

void beginCapture() {
  rmt_config_t config = {};
  config.channel = kCaptureChannel;
  config.rmt_mode = RMT_MODE_RX;
  config.gpio_num = static_cast<gpio_num_t>(rgbshow::kPin);
  config.clk_div = 2;
  config.mem_block_num = 2;
  config.rx_config.idle_threshold = 16000;  // 400 us continuous idle.
  config.rx_config.filter_en = false;
  captureError = rmt_config(&config);
  if (captureError == ESP_OK) captureError = rmt_driver_install(kCaptureChannel,8192,0);
  if (captureError == ESP_OK) captureError = rmt_get_ringbuf_handle(kCaptureChannel,&captureBuffer);
  // RX configuration disables output. Restore the existing TX route, then
  // enable input without calling gpio_set_direction after the TX mapping.
  const esp_err_t route = rmt_set_pin(RMT_CHANNEL_0,RMT_MODE_TX,
      static_cast<gpio_num_t>(rgbshow::kPin));
  PIN_INPUT_ENABLE(IO_MUX_GPIO47_REG);
  if (route != ESP_OK) captureError = route;
  captureReady = captureError == ESP_OK && captureBuffer;
}

bool startCapture() {
  if (!captureReady) return false;
  size_t bytes = 0;
  void* stale = nullptr;
  while ((stale=xRingbufferReceive(captureBuffer,&bytes,0)))
    vRingbufferReturnItem(captureBuffer,stale);
  captureError = rmt_rx_start(kCaptureChannel,true);
  return captureError == ESP_OK;
}

void finishCapture() {
  size_t bytes = 0;
  void* packet = xRingbufferReceive(captureBuffer,&bytes,pdMS_TO_TICKS(20));
  rmt_rx_stop(kCaptureChannel);
  capturedWords = 0;
  if (!packet) {
    captureError = ESP_ERR_TIMEOUT;
    ++captureTimeouts;
    return;
  }
  const size_t words = bytes/sizeof(rmt_item32_t);
  capturedWords = words>1024 ? 1024 : words;
  memcpy(capturedItems,packet,capturedWords*sizeof(rmt_item32_t));
  vRingbufferReturnItem(captureBuffer,packet);
  captureError = words>1024 ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

void dumpCapture() {
  Serial.printf("\nRGB_CAPTURE words=%u tick_ns=25 data=",unsigned(capturedWords));
  for (uint16_t i=0;i<capturedWords;++i)
    Serial.printf("%08lx",static_cast<unsigned long>(capturedItems[i].val));
  Serial.print('\n');
}

void render(uint32_t now) {
  const bool capturing = startCapture();
  const bool counting = startCounting();
  output.write(pixels,kBrightness);
  frameEdges = -1;
  if (counting) stopCounting(frameEdges);
  if (capturing) finishCapture();
  lastFrame = now;
}

void status() {
  Serial.printf("\nRGB_ALL_ON pin=47 leds=40 color=white brightness=%u "
                "rgb_driver=persistent_rmt rgb_error=%d rgb_sent=%lu "
                "rgb_failures=%lu lit_pixels=%u uptime_ms=%lu "
                "pcnt_error=%d calibration_edges=%d gpio47_edges=%d expected_edges=960 "
                "out_sel=%u pcnt_in=%u rx_error=%d rx_words=%u rx_timeouts=%lu\n",
      unsigned(kBrightness),int(output.error()),
      static_cast<unsigned long>(output.sent()),
      static_cast<unsigned long>(output.failures()),unsigned(output.litPixels()),
      static_cast<unsigned long>(millis()),int(pulseError),int(calibrationEdges),int(frameEdges),
      unsigned(REG_GET_FIELD(GPIO_FUNC47_OUT_SEL_CFG_REG,GPIO_FUNC47_OUT_SEL)),
      unsigned(REG_GET_FIELD(GPIO_FUNC33_IN_SEL_CFG_REG,GPIO_FUNC33_IN_SEL)),
      int(captureError),unsigned(capturedWords),static_cast<unsigned long>(captureTimeouts));
}
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  for (auto& pixel : pixels) pixel = {255,255,255};
  delay(150);
  pinMode(rgbshow::kPin,OUTPUT);
  digitalWrite(rgbshow::kPin,LOW);
  beginPulseCounter();
  calibrateCounter();
  output.begin();
  beginCapture();
  // gpio_set_direction(...INPUT_OUTPUT) also connects SIG_GPIO_OUT_IDX and
  // would overwrite the RMT mapping. Enable only the pad's input buffer here.
  if (pulseReady) PIN_INPUT_ENABLE(IO_MUX_GPIO47_REG);
  render(millis());
}

void loop() {
  for (uint8_t n=0;n<64 && Serial.available()>0;++n) {
    const int c = Serial.read();
    if (c=='?') status();
    else if (c=='C') dumpCapture();
    else if (c>=0) Serial.write(static_cast<uint8_t>(c));
  }
  const uint32_t now = millis();
  if (uint32_t(now-lastFrame)>=250) render(now);
  delay(1);
}
