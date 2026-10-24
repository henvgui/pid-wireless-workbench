#pragma once
#include "driver/rmt.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "step_control.h"
#if defined(PID_RGB_TEMPORAL_DITHER)
#include "rgb_dither.h"
#endif

// Comparison backend: allocate one RMT channel once, encode the complete frame
// in internal RAM, and expose actual driver return codes to USB diagnostics.
class RgbRmt {
 public:
  bool begin();
  bool write(const stepcontrol::Rgb* pixels, uint8_t brightness);
  esp_err_t error() const { return error_; }
  uint32_t sent() const { return sent_; }
  uint32_t failures() const { return failures_; }
  uint16_t litPixels() const { return litPixels_; }
  bool dithering() const {
#if defined(PID_RGB_TEMPORAL_DITHER)
    return true;
#else
    return false;
#endif
  }

 private:
  static constexpr rmt_channel_t kChannel = RMT_CHANNEL_0;
  rmt_item32_t items_[rgbshow::kLedCount * 24 + 1] = {};
  bool installed_ = false, healthy_ = false, stalled_ = false;
  TickType_t retryAt_ = 0;
  esp_err_t error_ = ESP_ERR_INVALID_STATE;
  uint32_t sent_ = 0, failures_ = 0;
  uint16_t litPixels_ = 0;
#if defined(PID_RGB_TEMPORAL_DITHER)
  rgboutput::BrightnessDither dither_;
#endif
};
