#include "rgb_rmt.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

bool RgbRmt::begin() {
  if (installed_) return healthy_;
  rmt_config_t config = {};
  config.rmt_mode = RMT_MODE_TX;
  config.channel = kChannel;
  config.gpio_num = static_cast<gpio_num_t>(rgbshow::kPin);
  config.clk_div = 2;  // 80 MHz APB / 2: 25 ns per tick.
  config.mem_block_num = 2;
  config.tx_config.idle_output_en = true;
  config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
  config.tx_config.carrier_en = false;
  config.tx_config.loop_en = false;
  error_ = rmt_config(&config);
  if (error_ != ESP_OK) return false;
  error_ = rmt_driver_install(kChannel, 0, ESP_INTR_FLAG_IRAM);
  if (error_ != ESP_OK) return false;
  installed_ = true;
  uint32_t clock = 0;
  error_ = rmt_get_counter_clock(kChannel, &clock);
  if (error_ == ESP_OK && clock != 40000000) error_ = ESP_ERR_INVALID_STATE;
  healthy_ = error_ == ESP_OK;
  return healthy_;
}

bool RgbRmt::write(const stepcontrol::Rgb* pixels, uint8_t brightness) {
  if (!healthy_ || !pixels) { ++failures_; return false; }
  if (stalled_) {
    // Retry a failed channel at most once per second so USB stays responsive.
    if (int32_t(xTaskGetTickCount() - retryAt_) < 0) { ++failures_; return false; }
    stalled_ = false;
  }
  if (brightness > rgbshow::kMaxBrightness) brightness = rgbshow::kMaxBrightness;
  size_t n = 0;
  litPixels_ = 0;
  for (uint16_t i = 0; i < rgbshow::kLedCount; ++i) {
    const uint8_t bytes[] = {
#if defined(PID_RGB_TEMPORAL_DITHER)
      dither_.scale(i,0,pixels[i].g,brightness),
      dither_.scale(i,1,pixels[i].r,brightness),
      dither_.scale(i,2,pixels[i].b,brightness)};
#else
      uint8_t((uint16_t(pixels[i].g) * brightness + 127) / 255),
      uint8_t((uint16_t(pixels[i].r) * brightness + 127) / 255),
      uint8_t((uint16_t(pixels[i].b) * brightness + 127) / 255)};
#endif
    if (bytes[0] || bytes[1] || bytes[2]) ++litPixels_;
    for (uint8_t byte : bytes) {
      for (uint8_t mask = 0x80; mask; mask >>= 1) {
        rmt_item32_t& item = items_[n++];
        item.val = 0;
        item.level0 = 1;
#if defined(PID_RGB_DATASHEET_TIMING)
        // XL-5050RGBC-WS2812B-S datasheet p7: keep T0H below its 400 ns
        // maximum and T1L below 400 ns; both symbols remain 1.25 us long.
        item.duration0 = (byte & mask) ? 36 : 12;  // 900 / 300 ns high.
        item.level1 = 0;
        item.duration1 = (byte & mask) ? 14 : 38;  // 350 / 950 ns low.
#else
        // 400/800 ns is the timing confirmed on this board. A 300 ns 0-code was
        // tried on 2026-10-14 and left all 40 LEDs dark, so do not shorten it.
        item.duration0 = (byte & mask) ? 32 : 16;  // 800 / 400 ns high.
        item.level1 = 0;
        item.duration1 = (byte & mask) ? 18 : 34;  // Total 1250 ns per bit.
#endif
      }
    }
  }
  // 300 us continuous low at the end of every frame, including black frames.
  items_[n].val = 0;
  items_[n].duration0 = 6000;
  items_[n].duration1 = 6000;
  ++n;
  error_ = rmt_write_items(kChannel, items_, static_cast<int>(n), false);
  if (error_ == ESP_OK) error_ = rmt_wait_tx_done(kChannel, pdMS_TO_TICKS(100));
  if (error_ != ESP_OK) {
    rmt_tx_stop(kChannel);
    stalled_ = true;
    retryAt_ = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
    ++failures_;
    return false;
  }
  ++sent_;
  return true;
}
