#include "rgb_show.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace rgbshow;
namespace {
char reply[768];
Color frame[kLedCount];
bool execute(Show& show, const char* text, uint32_t now) {
  return show.command(text, now, reply, sizeof(reply));
}
void validFrame(const Color* pixels) {
  for (unsigned i = 0; i < kLedCount; ++i) {
    for (float v : {pixels[i].r, pixels[i].g, pixels[i].b}) {
      assert(std::isfinite(v));
      assert(v >= 0 && v <= 1);
    }
  }
}
void sameFrame(const Color* a, const Color* b) {
  for (unsigned i = 0; i < kLedCount; ++i) {
    assert(fabsf(a[i].r - b[i].r) < 0.00001f);
    assert(fabsf(a[i].g - b[i].g) < 0.00001f);
    assert(fabsf(a[i].b - b[i].b) < 0.00001f);
  }
}
}

int main() {
  Show show;
  show.begin(0);
  assert(show.automatic() && show.brightness() == 40 && show.speed() == 100);
  assert(execute(show, "list", 0));
  assert(strstr(reply, "1=rainbow_flow") && strstr(reply, "20=sunrise"));
  assert(execute(show, "help", 0) && strstr(reply, "!brightness <0..40>"));
  assert(execute(show, " \teffect ocean \r\n", 0));
  assert(show.effect() == 9 && !show.automatic());

  // Malformed values must not change settings or silently wrap.
  const char* invalid[] = {"effect 0", "effect 21", "effect -1", "effect 1 junk",
                          "effect", "speed 24", "speed 301", "speed 100 junk",
                          "speed 999999999999999999", "brightness -1", "brightness 41",
                          "auto maybe", "off junk", "next junk", "", "unknown"};
  for (const char* command : invalid) {
    assert(!execute(show, command, 0));
    assert(strncmp(reply, "ERR", 3) == 0);
    assert(show.effect() == 9 && show.speed() == 100 && show.brightness() == 40);
    assert(!show.automatic() && show.enabled());
  }

  // Every effect must produce finite colors, animate, and remain in manual mode.
  for (unsigned effect = 1; effect <= kEffectCount; ++effect) {
    Show instance;
    instance.begin(0);
    char command[32];
    snprintf(command, sizeof(command), "effect %u", effect);
    assert(execute(instance, command, 0));
    Color first[kLedCount];
    instance.update(1000);
    instance.render(first);
    bool animated = false, illuminated = false;
    for (uint32_t now = 1016; now < 11000; now += 16) {
      instance.update(now);
      instance.render(frame);
      validFrame(frame);
      assert(instance.effect() == effect - 1 && !instance.automatic());
      for (unsigned i = 0; i < kLedCount; ++i) {
        illuminated |= frame[i].r + frame[i].g + frame[i].b > 0.01f;
        animated |= fabsf(first[i].r - frame[i].r) + fabsf(first[i].g - frame[i].g)
                    + fabsf(first[i].b - frame[i].b) > 0.02f;
      }
    }
    assert(animated && illuminated);
  }

  show.begin(0);
  show.render(frame);
  for (unsigned step = 1; step <= kEffectCount * 2; ++step) {
    show.update(step * kEffectMs);
    show.render(frame);
    assert(show.effect() == step % kEffectCount);
    validFrame(frame);
  }

  // Crossfade starts at the previous displayed frame, including rapid changes.
  show.begin(0);
  show.update(1200);
  Color before[kLedCount];
  show.render(before);
  assert(execute(show, "effect fireworks", 1200));
  show.render(frame);
  sameFrame(before, frame);
  show.update(1600);
  show.render(before);
  assert(execute(show, "next", 1600));
  show.render(frame);
  sameFrame(before, frame);

  // Pause freezes animation, playlist age and an in-progress crossfade.
  assert(execute(show, "pause", 1600));
  show.render(before);
  show.update(50000);
  show.render(frame);
  sameFrame(before, frame);
  assert(execute(show, "resume", 50000));
  show.update(51000);
  show.render(frame);
  validFrame(frame);
  assert(execute(show, "off", 51000));
  show.render(frame);
  for (const Color c : frame) assert(c.r == 0 && c.g == 0 && c.b == 0);
  assert(execute(show, "on", 51000) && show.enabled());
  assert(execute(show, "brightness 0", 51000) && show.brightness() == 0);
  assert(execute(show, "brightness 40", 51000) && show.brightness() == 40);

  // Twice the speed advances animation twice as far, without changing playlist duration.
  Show normal, fast;
  normal.begin(0);
  fast.begin(0);
  assert(execute(normal, "auto off", 0));
  assert(execute(fast, "auto off", 0));
  assert(execute(fast, "speed 200", 0));
  normal.update(4000);
  fast.update(2000);
  normal.render(before);
  fast.render(frame);
  sameFrame(before, frame);

  // millis() rollover must not cause an unintended mode change or stop.
  show.begin(UINT32_MAX - 100);
  show.update(50);
  show.render(frame);
  assert(show.effect() == 0);
  show.update(7899);
  show.render(frame);
  assert(show.effect() == 1);
  validFrame(frame);

  // Small reply buffers remain terminated and never overwrite adjacent bytes.
  for (size_t capacity : {size_t(1), size_t(2), size_t(7), size_t(32)}) {
    char guarded[64];
    memset(guarded, '#', sizeof(guarded));
    assert(show.command("list", 7899, guarded + 1, capacity));
    assert(guarded[0] == '#' && guarded[capacity + 1] == '#');
    assert(memchr(guarded + 1, '\0', capacity));
  }
  puts("PASS: 20 animated effects, controls, invalid input, fades, pause, rollover, buffers");
}
