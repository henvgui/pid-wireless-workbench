#pragma once
#include <stdint.h>
#include "rgb_show.h"

#ifdef ARDUINO
#include "esp_attr.h"
#define STEP_ISR_ATTR IRAM_ATTR
#else
#define STEP_ISR_ATTR
#endif

namespace stepcontrol {
constexpr uint8_t kPinA = 15;
constexpr uint8_t kPinB = 16;
constexpr uint8_t kPinSwitch = 17;
constexpr uint8_t kPaletteCount = 8;
constexpr uint8_t kTrailLength = 7;
extern const uint8_t kTrailLevels[kTrailLength];
constexpr uint32_t kButtonDebounceMs = 25;
constexpr uint32_t kButtonHoldMs = 2000;
constexpr uint8_t kKnobCount = 4;
struct KnobPins {
  const char* name;
  uint8_t a, b, sw;
  uint16_t position;
  uint8_t color;
};
extern const KnobPins kKnobs[kKnobCount];
struct Rgb { uint8_t r, g, b; };
extern const char* const kColorNames[kPaletteCount];

// Call under the same critical section from the ISR and the main loop.
// One-bit Gray transitions cancel contact bounce; diagonal jumps are discarded.
// Steps are settled only on a detent state (11, or 00/11 in two-edge mode), so a
// lost edge or a start between detents can never shift where later steps land.
class Quadrature {
 public:
  void begin(uint8_t ab, uint8_t edges = 4) {
    previous_ = ab & 3;
    accumulator_ = 0;
    edges_ = edges;
    synced_ = detent(previous_);
    invalid_ = 0;
  }
  int8_t STEP_ISR_ATTR onEdge(uint8_t ab) {
    ab &= 3;
    const uint8_t changed = previous_ ^ ab;
    if (!changed) return 0;
    if (changed == 3) {
      // Two edges were lost: direction is unknown until the next detent.
      previous_ = ab;
      accumulator_ = 0;
      synced_ = false;
      ++invalid_;
      return 0;
    }
    // With A in bit 1: 11 -> 01 -> 00 -> 10 -> 11 is positive.
    if (synced_) accumulator_ += ((previous_ & 1) ^ (ab >> 1)) ? 1 : -1;
    previous_ = ab;
    if (!detent(ab)) return 0;
    // Between two detents the net count is 0 (bounce or reversal) or +/-edges.
    const int8_t step = !synced_ ? 0 : accumulator_ > 0 ? 1 : accumulator_ < 0 ? -1 : 0;
    accumulator_ = 0;
    synced_ = true;
    return step;
  }
  uint32_t invalid() const { return invalid_; }
 private:
  bool STEP_ISR_ATTR detent(uint8_t ab) const { return ab == 3 || (edges_ == 2 && ab == 0); }
  uint8_t previous_ = 3;
  uint8_t edges_ = 4;
  int8_t accumulator_ = 0;
  bool synced_ = true;
  uint32_t invalid_ = 0;
};

class Button {
 public:
  void begin(bool pressed, uint32_t now);
  bool update(bool pressed, uint32_t now);
 private:
  bool raw_ = false;
  bool stable_ = false;
  uint32_t changedAt_ = 0;
};

enum class ButtonEvent : uint8_t { None, Click, Hold };
enum class ClickEvent : uint8_t { None, Single, Double };
constexpr uint32_t kDoubleClickMs=350,kClickMaximumMs=600;
// Confirm single click only after the double-click window; held-at-boot and
// long presses cannot select another device. No extra click follows a double.
class DoubleClickButton {
 public:
  void begin(bool pressed,uint32_t now);
  ClickEvent update(bool pressed,uint32_t now);
 private:
  bool raw_=false,stable_=false,armed_=false,pending_=false,second_=false;
  uint32_t changedAt_=0,pressedAt_=0,releasedAt_=0;
};

// Short clicks fire on release, allowing a hold to consume the whole gesture.
// A button held during boot/reset must first be released before it is armed.
class HoldButton {
 public:
  void begin(bool pressed, uint32_t now);
  ButtonEvent update(bool pressed, uint32_t now);
 private:
  bool raw_ = false, stable_ = false, armed_ = false, reported_ = false;
  uint32_t changedAt_ = 0, pressedAt_ = 0;
};

class Comet {
 public:
  void reset(uint16_t position = 0, uint8_t color = 0);
  void move(int8_t direction);
  void nextColor() { color_ = (color_ + 1) % kPaletteCount; }
  void setColor(uint8_t color) { if (color < kPaletteCount) color_ = color; }
  void render(Rgb* pixels) const;
  uint8_t color() const { return color_; }
  uint16_t logicalPosition() const { return history_[0]; }
  uint16_t pixel() const { return toPixel(history_[0]); }
 private:
  // PCB1.pcbdoc: the 40 DI/DO-connected LEDs run CCW in the top view.
  // Positive logical motion is CW, so the data-chain index must decrease.
  static uint16_t toPixel(uint16_t position) {
    return (rgbshow::kLedCount - position) % rgbshow::kLedCount;
  }
  uint16_t history_[kTrailLength] = {};
  uint8_t historyLength_ = 1;
  uint8_t color_ = 0;
};

class KnobScene {
 public:
  void reset();
  void select(uint8_t knob) { if (knob < kKnobCount) selected_ = knob; }
  void setIndependent(bool value) { independent_ = value; }
  bool independent() const { return independent_; }
  uint8_t selected() const { return selected_; }
  void move(uint8_t knob, int8_t direction);
  void nextColor(uint8_t knob);
  void setColor(uint8_t knob, uint8_t color);
  uint16_t pixel(uint8_t knob) const;
  uint8_t color(uint8_t knob) const;
  void render(Rgb* pixels) const;
 private:
  Comet& cursor(uint8_t knob) { return independent_ ? cursors_[knob] : shared_; }
  const Comet& cursor(uint8_t knob) const { return independent_ ? cursors_[knob] : shared_; }
  Comet cursors_[kKnobCount];
  Comet shared_;
  uint8_t selected_ = 3;
  bool independent_ = true;
};
}  // namespace stepcontrol
