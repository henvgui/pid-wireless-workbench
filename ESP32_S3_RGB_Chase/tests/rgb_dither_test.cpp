#include "rgb_dither.h"
#include <cassert>
#include <cstdio>

int main() {
  rgboutput::BrightnessDither output;
  // A constant dim channel must retain its exact mean, including values that
  // ordinary rounding makes identical. Black/full-scale must never flicker.
  for (unsigned brightness=0;brightness<=40;++brightness) {
    for (unsigned value=0;value<=255;++value) {
      output.reset();
      unsigned sum = 0;
      for (unsigned frame=0;frame<255;++frame) {
        const unsigned sample = output.scale(7,1,value,brightness);
        assert(sample<=brightness);
        assert(sample==value*brightness/255 || sample==(value*brightness+254)/255);
        if (!value || !brightness) assert(sample==0);
        if (value==255) assert(sample==brightness);
        sum += sample;
      }
      assert(sum==value*brightness);
    }
  }
  // On a moving gradient, cumulative rounding error stays below one hardware
  // level per channel, instead of repeatedly rounding every frame the same way.
  output.reset();
  int target = 0, actual = 0;
  for (unsigned frame=0;frame<4000;++frame) {
    const unsigned value = frame%256;
    target += value*30;
    actual += output.scale(0,0,value,30)*255;
    assert(actual-target>-255 && actual-target<255);
  }
  // Neighbouring LEDs do not all emit the extra low-brightness level together.
  output.reset();
  unsigned roundedUp = 0;
  for (unsigned pixel=0;pixel<40;++pixel)
    if (output.scale(pixel,0,128,30)==16) ++roundedUp;
  assert(roundedUp>0 && roundedUp<40);
  puts("PASS: exact dim-channel averages, bounds/black/full-scale, changing-gradient error bound, staggered pixel phases");
}
