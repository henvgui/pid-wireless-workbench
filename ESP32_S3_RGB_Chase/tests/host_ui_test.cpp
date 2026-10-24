#include "host_ui.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cmath>

using namespace hostui;
using stepcontrol::Rgb;

unsigned head(const Rgb* pixels) {
  unsigned count = 0, position = 0;
  for (unsigned i = 0; i < 40; ++i) {
    if (pixels[i].r == 255 || pixels[i].g == 255 || pixels[i].b == 255) {
      ++count;
      position = i;
    }
  }
  assert(count == 1);
  return position;
}
unsigned level(const Rgb& p) {
  return p.r>p.g ? (p.r>p.b?p.r:p.b) : (p.g>p.b?p.g:p.b);
}
int channelDifference(unsigned a, unsigned b) {
  return a>b ? int(a-b) : int(b-a);
}
void nearFrame(const Rgb* a, const Rgb* b, int limit) {
  for (unsigned i=0;i<40;++i) {
    assert(channelDifference(a[i].r,b[i].r)<=limit);
    assert(channelDifference(a[i].g,b[i].g)<=limit);
    assert(channelDifference(a[i].b,b[i].b)<=limit);
  }
}
float movement(float from, float to) {
  float delta = to-from;
  if (delta>20.0f) delta-=40.0f;
  if (delta< -20.0f) delta+=40.0f;
  return delta;
}

int main() {
  Controller ui;
  Rgb a[40], b[40];
  ui.reset(0);
  assert(!ui.active(0) && !ui.active(999999));
  assert(ui.snapshot().selected == 3 && ui.value(3) == 100);
  for (uint8_t i = 0; i < 3; ++i) assert(ui.value(i) == 0);
  ui.render(0,a);
  unsigned lit = 0;
  for (const Rgb& p : a) if (p.r || p.g || p.b) ++lit;
  assert(lit == 40);
  for (unsigned i=0;i<40;++i) {
    assert(level(a[i])==level(a[0])); // One shared brightness envelope.
    const Rgb next = a[(i+1)%40];
    const int limit = level(a[i])/5+2;
    assert(int(a[i].r)-next.r<=limit && int(next.r)-a[i].r<=limit);
    assert(int(a[i].g)-next.g<=limit && int(next.g)-a[i].g<=limit);
    assert(int(a[i].b)-next.b<=limit && int(next.b)-a[i].b<=limit);
  }
  assert(std::memcmp(&a[0],&a[1],sizeof(Rgb))!=0); // Adjacent colors differ gradually.
  assert(ui.value(0)==0 && ui.value(1)==0 && ui.value(2)==0 && ui.value(3)==100);
  ui.render(2000,b);
  assert(int(level(a[0]))-level(b[0])<=1 && int(level(b[0]))-level(a[0])<=1);
  assert(std::memcmp(a,b,sizeof(a))!=0); // Colors flow at matching breath levels.
  ui.render(1000,b);
  const unsigned brightest = level(b[0]);
  ui.render(3000,b);
  const unsigned dimmest = level(b[0]);
  assert(dimmest*100>=brightest*9 && dimmest*100<=brightest*11);
  for (const Rgb& pixel : b) assert(level(pixel)==dimmest);
  assert(ui.scene().pixel(0)==0 && ui.value(0)==0 && ui.value(3)==100);

  // Frequent detents must keep the entry fade's original deadline and preserve
  // the moving cursor's position/velocity when its target changes.
  ui.move(0,1,100);
  int detents = 1;
  for (uint32_t t=120;t<=900;t+=20) {
    ui.render(t,a);
    const float before = ui.visualPosition(t);
    const float velocityBefore = movement(ui.visualPosition(t-1),before);
    ui.move(0,1,t);
    ++detents;
    ui.render(t,b);
    nearFrame(a,b,1);
    assert(std::fabs(movement(before,ui.visualPosition(t)))<0.0001f);
    const float velocityAfter = movement(ui.visualPosition(t),ui.visualPosition(t+1));
    assert(std::fabs(velocityAfter-velocityBefore)<0.004f);
    if (t>=550) assert(!ui.transitioning(t));
  }
  assert(ui.value(0)==detents*100);
  const float beforeReverse = ui.visualPosition(920);
  const float incomingVelocity = movement(ui.visualPosition(919),beforeReverse);
  ui.render(920,a);
  ui.move(0,-1,920);
  ui.render(920,b);
  nearFrame(a,b,1);
  assert(std::fabs(movement(beforeReverse,ui.visualPosition(920)))<0.0001f);
  assert(std::fabs(movement(ui.visualPosition(920),ui.visualPosition(921))-incomingVelocity)<0.004f);
  assert(ui.value(0)==(detents-1)*100);
  ui.render(1220,a);
  assert(head(a)==ui.scene().pixel(0));
  assert(std::fabs(movement(ui.visualPosition(1220),float(ui.scene().pixel(0))))<0.0001f);
  ui.render(1400,b);
  assert(head(a)==head(b) && std::memcmp(a,b,sizeof(a))!=0);
  assert(!ui.transitioning(1400));
  ui.press(0,1500);
  ui.move(0,1,1520);
  assert(ui.transitioning(1749) && !ui.transitioning(1750)); // Turning cannot restart a color fade either.
  ui.reset(0);

  // Continued rotations must not restart the rainbow clock; its loop seam is continuous.
  ui.setColor(0,0,0);
  ui.move(0,1,4000);
  assert(ui.accentHueDegrees(4000)==180 && ui.value(0)==100);
  ui.render(7999,a);
  ui.render(8000,b);
  nearFrame(a,b,1);
  assert(ui.accentHueDegrees(8000)==0 && head(a)==head(b) && ui.value(0)==100);
  ui.reset(0);

  // Enter from the visible rainbow, preserving its hue instead of jumping to a preset.
  ui.render(1234,a);
  ui.move(0,1,1234);
  ui.render(1234,b);
  nearFrame(a,b,0);
  assert(ui.transitioning(1234) && ui.value(0)==100 && ui.scene().pixel(0)==39);
  assert(ui.accentHueDegrees(1234)>=110 && ui.accentHueDegrees(1234)<=112);
  ui.render(1235,b);
  nearFrame(a,b,1);
  ui.render(1234+kEnterTransitionMs,b);
  assert(!ui.transitioning(1234+kEnterTransitionMs) && head(b)==39);
  assert(ui.accentHueDegrees(1234+kEnterTransitionMs)>=130 &&
         ui.accentHueDegrees(1234+kEnterTransitionMs)<=132);
  const Rgb background = b[20];
  assert(level(background)>0 && level(background)<level(b[39]));
  assert(channelDifference(unsigned(background.r)*255/level(background)+b[39].r,255)<=6);
  assert(channelDifference(unsigned(background.g)*255/level(background)+b[39].g,255)<=6);
  assert(channelDifference(unsigned(background.b)*255/level(background)+b[39].b,255)<=6);
  ui.render(2000,a);
  ui.render(6233,b);
  assert(head(a)==head(b) && std::memcmp(a,b,sizeof(a))!=0);
  for (unsigned i=0;i<40;++i) assert(level(a[i])==level(b[i]));
  assert(ui.age(6233)==4999 && ui.value(0)==100); // Color motion never changes data or the idle timer.
  assert(!ui.transitioning(6233));
  ui.render(6233,a);
  ui.render(6234,b); // Timeout starts from the rainbow color at the deadline.
  nearFrame(a,b,1);
  assert(!ui.active(6234) && ui.transitioning(6234));
  ui.render(6235,b);
  nearFrame(a,b,1);
  ui.render(6600,a);
  assert(std::memcmp(a,b,sizeof(a))!=0 && ui.value(0)==100);
  ui.move(0,-1,6600); // Turning during the return fade must also be continuous.
  ui.render(6600,b);
  nearFrame(a,b,0);
  assert(ui.active(6600) && ui.value(0)==0 && ui.scene().pixel(0)==0);
  ui.render(7100,a);
  const uint8_t firstColor = ui.color(0);
  ui.press(0,7100);
  ui.render(7100,b);
  nearFrame(a,b,0);
  assert(ui.color(0)==(firstColor+1)%stepcontrol::kPaletteCount);
  ui.render(7400,b);
  assert(head(b)==0 && !ui.transitioning(7400));
  ui.render(12100,a);
  ui.render(12101,b);
  nearFrame(a,b,1);
  assert(ui.transitioning(12100) && !ui.transitioning(13000));
  ui.render(13000,b);
  for (const Rgb& pixel : b) assert(level(pixel)==level(b[0]));
  assert(ui.value(0)==0 && ui.value(3)==100);

  ui.reset(100);
  ui.move(0,1,100);
  assert(ui.active(100) && ui.active(5099) && !ui.active(5100));
  assert(ui.value(0)==100 && ui.value(1)==0 && ui.value(2)==0);
  assert(ui.scene().pixel(0)==39 && !ui.scene().independent());
  ui.move(1,1,200);
  assert(ui.scene().pixel(0)==38 && ui.value(1)==100);
  ui.move(2,1,300);
  assert(ui.scene().pixel(0)==37 && ui.value(2)==100);
  ui.move(3,1,400);
  assert(ui.scene().pixel(0)==36 && ui.value(3)==1000);
  ui.render(600,a);
  ui.render(5399,b);
  assert(head(a)==36 && head(b)==36);
  assert(std::memcmp(a,b,sizeof(a)) != 0); // The cursor holds position while rainbow colors keep moving.
  for (unsigned i=0;i<40;++i) assert(level(a[i])==level(b[i]));
  unsigned heads = 0;
  lit = 0;
  for (const Rgb& p : a) {
    if (p.r || p.g || p.b) ++lit;
    if (p.r==255 || p.g==255 || p.b==255) ++heads;
  }
  unsigned foreground = 0;
  const unsigned backgroundLevel = level(a[16]);
  for (const Rgb& p : a) if (level(p)>backgroundLevel) ++foreground;
  assert(heads==1 && lit==40 && foreground<=stepcontrol::kTrailLength);
  assert(!ui.active(5400));
  ui.render(5399,a);
  ui.render(5400,b);
  nearFrame(a,b,1);
  ui.render(5700,b);
  assert(std::memcmp(a,b,sizeof(a)) != 0);
  assert(ui.scene().pixel(0)==36 && ui.value(0)==100 && ui.value(3)==1000);

  const unsigned beforeMove = ui.scene().pixel(0);
  ui.move(0,1,6000);
  assert(ui.active(6000) && ui.value(0)==1100);
  assert(ui.scene().pixel(0)==(beforeMove+39)%40);
  const uint8_t original = ui.color(1);
  ui.press(1,8000);
  assert(ui.color(1)==(original+1)%stepcontrol::kPaletteCount);
  assert(ui.color(0)==stepcontrol::kKnobs[0].color);
  assert(ui.snapshot().selected==1 && ui.active(12999) && !ui.active(13000));
  const unsigned beforePress = ui.scene().pixel(0);
  ui.press(2,15000);
  assert(ui.active(15000) && ui.scene().pixel(0)==beforePress);
  const uint32_t age = ui.age(15100);
  ui.move(99,1,15100); ui.move(0,0,15100); ui.press(99,15100);
  ui.setColor(0,99,15100);
  assert(ui.age(15100)==age); // Invalid input never postpones idle.

  ui.reset(0);
  ui.move(3,-1,1); assert(ui.value(3)==10);
  ui.move(3,-1,2); assert(ui.value(3)==1);
  ui.move(3,-1,3); assert(ui.value(3)==1);
  ui.move(0,1,4); assert(ui.value(0)==1);
  ui.move(0,-1,5); ui.move(0,-1,6); assert(ui.value(0)==0);
  ui.move(3,1,7); ui.move(3,1,8); ui.move(3,1,9); ui.move(3,1,10);
  assert(ui.value(3)==1000);
  for (int n=0;n<1004;++n) ui.move(0,1,11);
  assert(ui.value(0)==kMaxValue);
  for (int n=0;n<1004;++n) ui.move(0,-1,12);
  assert(ui.value(0)==0);

  char text[16];
  formatValue(0,text,sizeof(text)); assert(!std::strcmp(text,"0.00"));
  formatValue(1,text,sizeof(text)); assert(!std::strcmp(text,"0.01"));
  formatValue(1000,text,sizeof(text)); assert(!std::strcmp(text,"10.00"));
  formatValue(kMaxValue,text,sizeof(text)); assert(!std::strcmp(text,"9999.99"));

  const uint32_t touch = UINT32_MAX-1000;
  ui.reset(touch-1);
  ui.press(0,touch);
  assert(ui.active(uint32_t(touch+4999)) && !ui.active(uint32_t(touch+5000)));
  ui.move(2,-1,uint32_t(touch+6000));
  assert(ui.active(uint32_t(touch+6001)));
  ui.render(uint32_t(touch+10999),a);
  ui.render(uint32_t(touch+11000),b);
  nearFrame(a,b,1); // The return boundary also remains continuous across timer wrap.
  assert(ui.transitioning(uint32_t(touch+11000)));
  assert(!ui.transitioning(uint32_t(touch+11900)));
  ui.reset(123);
  assert(!ui.active(123) && ui.value(0)==0 && ui.value(3)==100);
  puts("PASS: continuous position/velocity during rapid detents and reversal, independent fade deadlines, inherited rainbow and complementary background, held target, continuous mode fades, timeout/rollover and parameter bounds");
}
