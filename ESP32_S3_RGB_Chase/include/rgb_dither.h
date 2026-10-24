#pragma once
#include <stdint.h>
#include "rgb_show.h"

namespace rgboutput {
// Preserve the fractional part of low-brightness scaling across frames. Distinct
// pixel/channel phases keep the ring from rounding up/down in unison.
class BrightnessDither {
 public:
  BrightnessDither() { reset(); }
  void reset() {
    for (uint16_t i=0;i<rgbshow::kLedCount;++i)
      for (uint8_t channel=0;channel<3;++channel)
        error_[i][channel] = uint8_t((i*255u/rgbshow::kLedCount+channel*85u)%255u);
  }
  uint8_t scale(uint16_t pixel, uint8_t channel, uint8_t value, uint8_t brightness) {
    const uint16_t total = uint16_t(value)*brightness+error_[pixel][channel];
    error_[pixel][channel] = uint8_t(total%255u);
    return uint8_t(total/255u);
  }
 private:
  uint8_t error_[rgbshow::kLedCount][3] = {};
};
}  // namespace rgboutput
