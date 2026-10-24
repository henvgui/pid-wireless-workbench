#include "step_control.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace stepcontrol;
int main() {
  bool used[48] = {};
  for (const KnobPins& k : kKnobs) {
    assert(k.a<32 && k.b<32 && k.sw<32);
    assert(!used[k.a]); used[k.a]=true;
    assert(!used[k.b]); used[k.b]=true;
    assert(!used[k.sw]); used[k.sw]=true;
  }
  assert(!used[rgbshow::kPin]);
  KnobScene scene;
  scene.reset();
  assert(scene.independent() && scene.selected()==3);
  const uint16_t start[] = {30,20,10,0};
  const uint8_t colors[] = {3,5,6,0};
  for (uint8_t i=0;i<4;++i) {
    assert(scene.pixel(i)==start[i] && scene.color(i)==colors[i]);
    scene.move(i,1);
    assert(scene.pixel(i)==(start[i]+39)%40);
    for (uint8_t other=i+1;other<4;++other) assert(scene.pixel(other)==start[other]);
    scene.nextColor(i);
    assert(scene.color(i)==(colors[i]+1)%8);
    for (uint8_t other=i+1;other<4;++other) assert(scene.color(other)==colors[other]);
  }
  Rgb frozen[40],frame[40];
  scene.render(frozen);
  for (int i=0;i<100;++i) {
    scene.render(frame); assert(!memcmp(frame,frozen,sizeof(frame)));
  }
  scene.reset();
  scene.setIndependent(false);
  scene.move(0,1);
  assert(scene.pixel(3)==39);
  scene.move(2,-1);
  assert(scene.pixel(0)==0 && scene.pixel(1)==0);
  scene.nextColor(1);
  assert(scene.color(0)==1 && scene.color(3)==1);
  scene.setIndependent(true);
  for (uint8_t i=0;i<4;++i)
    assert(scene.pixel(i)==start[i] && scene.color(i)==colors[i]);
  // Overlaying heads/trails is bounded, and a stopped knob remains frozen.
  for (int i=0;i<10;++i) scene.move(0,1);
  assert(scene.pixel(0)==scene.pixel(1));
  scene.render(frame);
  assert(frame[scene.pixel(0)].g==255 && frame[scene.pixel(1)].b==255);
  const uint16_t unchanged = scene.pixel(3);
  scene.move(0,1); scene.nextColor(2);
  assert(scene.pixel(3)==unchanged && scene.color(3)==0);
  scene.move(99,1); scene.nextColor(99); // Invalid IDs cannot overwrite another knob.
  assert(scene.pixel(3)==unchanged);
  puts("PASS: four independent cursors/colors, stopped-frame stability, shared mode, preserved independent state, overlay, unique GPIOs");
}
