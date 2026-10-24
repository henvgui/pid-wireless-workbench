#include "step_control.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace stepcontrol;
int main() {
  Quadrature q;
  q.begin(3);
  // Contact bounce must cancel, instead of emitting an extra detent.
  const uint8_t bouncingCW[] = {1,3,1,0,1,0,2,0,2,3};
  int steps=0;
  for (uint8_t ab : bouncingCW) steps+=q.onEdge(ab);
  assert(steps==1);
  const uint8_t ccw[] = {2,0,1,3};
  for (uint8_t ab : ccw) steps+=q.onEdge(ab);
  assert(steps==0 && q.invalid()==0);
  q.begin(3);
  assert(q.onEdge(0)==0 && q.invalid()==1);
  assert(q.onEdge(1)==0 && q.onEdge(3)==0);
  q.begin(3,2);
  assert(q.onEdge(1)==0 && q.onEdge(0)==1);
  assert(q.onEdge(2)==0 && q.onEdge(3)==1);
  // Partial turn then reversal cannot create a phantom step.
  q.begin(3);
  for (uint8_t ab : {uint8_t(1),uint8_t(0),uint8_t(1),uint8_t(3)})
    assert(q.onEdge(ab)==0);
  for (int i=0;i<1000;++i) {
    assert(q.onEdge(1)==0 && q.onEdge(0)==0 && q.onEdge(2)==0);
    assert(q.onEdge(3)==1);
  }
  // A lost pair of edges mid-turn must not shift later steps off the detent.
  q.begin(3);
  assert(q.onEdge(1)==0 && q.onEdge(2)==0 && q.invalid()==1);
  assert(q.onEdge(3)==0);
  for (int i=0;i<10;++i) {
    assert(q.onEdge(1)==0 && q.onEdge(0)==0 && q.onEdge(2)==0);
    assert(q.onEdge(3)==1);
  }
  for (int i=0;i<10;++i) {
    assert(q.onEdge(2)==0 && q.onEdge(0)==0 && q.onEdge(1)==0);
    assert(q.onEdge(3)==-1);
  }
  // Power-up between detents: the first partial turn only synchronises.
  q.begin(0);
  assert(q.onEdge(2)==0 && q.onEdge(3)==0);
  assert(q.onEdge(1)==0 && q.onEdge(0)==0 && q.onEdge(2)==0 && q.onEdge(3)==1);
  // Two-edge detents at both 00 and 11; bounce at a detent emits nothing.
  q.begin(0,2);
  assert(q.onEdge(2)==0 && q.onEdge(0)==0 && q.onEdge(2)==0 && q.onEdge(3)==1);
  assert(q.onEdge(2)==0 && q.onEdge(0)==-1);
  assert(q.onEdge(1)==0 && q.onEdge(0)==0);

  Button button;
  button.begin(false,0);
  assert(!button.update(true,1));
  assert(!button.update(false,4));
  assert(!button.update(true,8));
  assert(!button.update(true,32));
  assert(button.update(true,33));
  assert(!button.update(true,9000));
  assert(!button.update(false,9001));
  assert(!button.update(false,9026));
  assert(!button.update(true,9030));
  assert(button.update(true,9055));
  button.begin(false,UINT32_MAX-20);
  assert(!button.update(true,UINT32_MAX-10));
  assert(button.update(true,15));
  button.begin(true,0);
  assert(!button.update(true,100));

  Comet comet;
  comet.reset();
  Rgb frame[rgbshow::kLedCount], frozen[rgbshow::kLedCount];
  comet.render(frame);
  assert(frame[0].r==255 && frame[1].r==0);
  comet.move(1);
  assert(comet.pixel()==39);
  comet.render(frame);
  assert(frame[39].r==255 && frame[0].r==170);
  comet.move(-1);
  assert(comet.pixel()==0);
  comet.render(frame);
  assert(frame[0].r==255 && frame[39].r==170);
  for (int i=0;i<40;++i) comet.move(1);
  assert(comet.pixel()==0);
  comet.render(frozen);
  for (int i=0;i<100;++i) {
    comet.render(frame);
    assert(!memcmp(frame,frozen,sizeof(frame)));
  }
  comet.setColor(3);
  comet.render(frame);
  assert(frame[0].r==0 && frame[0].g==255);
  unsigned lit=0,heads=0;
  for (const Rgb c : frame) { lit+=c.g>0; heads+=c.g==255; }
  assert(lit==kTrailLength && heads==1);
  for (unsigned i=0;i<kPaletteCount;++i) comet.nextColor();
  assert(comet.color()==3);
  comet.reset();
  comet.move(-1);
  assert(comet.pixel()==1);
  puts("PASS: quadrature bounce/reversal/noise, two/four-edge modes, button debounce/hold/rollover, LED direction/wrap/frozen trail/color");
}
