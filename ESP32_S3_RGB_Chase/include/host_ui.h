#pragma once
#include <stddef.h>
#include <stdint.h>
#include "step_control.h"

namespace hostui {
constexpr uint32_t kIdleMs = 5000;
constexpr uint32_t kEnterTransitionMs = 450;
constexpr uint32_t kReturnTransitionMs = 900;
constexpr uint32_t kManualColorPeriodMs = 8000;
constexpr uint32_t kRgbFrameMs = 4;
constexpr int32_t kMaxValue = 999999;  // Hundredths: 9999.99.
struct Snapshot {
  int32_t values[3];
  int32_t step;
  uint8_t selected;
};
void formatValue(int32_t hundredths, char* output, size_t capacity);

class Controller {
 public:
  void reset(uint32_t now);
  void move(uint8_t knob, int8_t direction, uint32_t now);
  void press(uint8_t knob, uint32_t now);
  void setColor(uint8_t knob, uint8_t color, uint32_t now);
  bool rgbEnabled() const { return rgbEnabled_; }
  void setRgbEnabled(bool enabled) { rgbEnabled_ = enabled; }
  void toggleRgb() { rgbEnabled_ = !rgbEnabled_; }
  bool active(uint32_t now) const {
    return touched_ && uint32_t(now - lastTouch_) < kIdleMs;
  }
  uint32_t age(uint32_t now) const { return touched_ ? uint32_t(now-lastTouch_) : 0; }
  uint8_t color(uint8_t knob) const { return colors_[knob]; }
  int32_t value(uint8_t knob) const;
  Snapshot snapshot() const;
  bool setParameters(int32_t p, int32_t i, int32_t d, int32_t step);
  void render(uint32_t now, stepcontrol::Rgb* pixels) const;
  bool transitioning(uint32_t now) const;
  uint16_t accentHueDegrees(uint32_t now) const;
  float visualPosition(uint32_t now) const;
  stepcontrol::KnobScene& scene() { return scene_; }
 private:
  struct Motion { float position, velocity; };
  void touch(uint8_t knob, uint32_t now, bool recolor = false);
  Motion motionAt(uint32_t now) const;
  float tailFacingAt(uint32_t now) const;
  void choosePaletteColor(uint8_t knob);
  float accentHueAt(uint32_t now) const;
  void renderManual(uint32_t now, stepcontrol::Rgb* pixels) const;
  void renderIdle(uint32_t now, stepcontrol::Rgb* pixels) const;
  stepcontrol::KnobScene scene_;
  int32_t values_[3] = {};
  uint8_t colors_[stepcontrol::kKnobCount] = {};
  uint8_t stepIndex_ = 2;
  bool touched_ = false;
  bool rgbEnabled_ = true;
  uint32_t lastTouch_ = 0, startedAt_ = 0;
  uint32_t transitionMs_ = 0;
  uint32_t fadeStartedAt_ = 0, motionStartedAt_ = 0, tailStartedAt_ = 0;
  float motionPosition_ = 0.0f, motionVelocity_ = 0.0f, motionTarget_ = 0.0f;
  float tailFrom_ = 1.0f, tailTarget_ = 1.0f;
  uint32_t accentStartedAt_ = 0;
  float accentHue_ = 0.0f;
  stepcontrol::Rgb fadeFrom_[rgbshow::kLedCount] = {};
};
}  // namespace hostui
