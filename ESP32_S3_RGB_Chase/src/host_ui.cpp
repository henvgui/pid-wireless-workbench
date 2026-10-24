#include "host_ui.h"
#include <cmath>
#include <cstdio>

namespace hostui {
namespace {
constexpr int32_t kSteps[] = {1, 10, 100, 1000};
constexpr float kTau = 6.28318530718f;
constexpr uint32_t kBreathPeriodMs = 4000, kColorPeriodMs = 4000;
constexpr uint8_t kMinimumBreathLevel = 26;
constexpr uint8_t kBackgroundLevel = 48;
constexpr uint32_t kColorTransitionMs = 250, kTailTurnMs = 120;
constexpr float kMotionTauMs = 20.0f;
uint8_t level(const stepcontrol::Rgb& color) {
  return color.r > color.g ? (color.r > color.b ? color.r : color.b) :
                            (color.g > color.b ? color.g : color.b);
}
float colorHue(const stepcontrol::Rgb& color) {
  const float maximum = level(color);
  const float minimum = color.r < color.g ? (color.r < color.b ? color.r : color.b) :
                                           (color.g < color.b ? color.g : color.b);
  const float delta = maximum-minimum;
  if (delta == 0.0f) return 0.0f;
  float h = maximum == color.r ? (float(color.g)-color.b)/delta :
            maximum == color.g ? 2.0f+(float(color.b)-color.r)/delta :
                                 4.0f+(float(color.r)-color.g)/delta;
  h /= 6.0f;
  return h < 0.0f ? h+1.0f : h;
}
float easedProgress(uint32_t elapsed, uint32_t duration) {
  if (elapsed >= duration) return 1.0f;
  const float t = float(elapsed)/duration;
  return t*t*(3.0f-2.0f*t);
}
stepcontrol::Rgb blend(const stepcontrol::Rgb& from, const stepcontrol::Rgb& to, float amount) {
  return {uint8_t(from.r+(float(to.r)-from.r)*amount+0.5f),
          uint8_t(from.g+(float(to.g)-from.g)*amount+0.5f),
          uint8_t(from.b+(float(to.b)-from.b)*amount+0.5f)};
}
stepcontrol::Rgb hue(float h, uint8_t level) {
  h = (h-std::floor(h))*6.0f;
  const uint8_t falling = uint8_t(float(level) * (1.0f - (h - int(h))));
  const uint8_t rising = uint8_t(float(level) * (h - int(h)));
  switch (int(h)) {
    case 0: return {level, rising, 0};
    case 1: return {falling, level, 0};
    case 2: return {0, level, rising};
    case 3: return {0, falling, level};
    case 4: return {rising, 0, level};
    default: return {level, 0, falling};
  }
}
void deposit(float* mask, float position, float amount) {
  position -= std::floor(position/rgbshow::kLedCount)*rgbshow::kLedCount;
  const unsigned first = unsigned(position);
  const float fraction = position-first;
  mask[first] += amount*(1.0f-fraction);
  mask[(first+1)%rgbshow::kLedCount] += amount*fraction;
}
}

void formatValue(int32_t value, char* output, size_t capacity) {
  std::snprintf(output, capacity, "%ld.%02ld", static_cast<long>(value/100),
                static_cast<long>(value%100));
}

void Controller::reset(uint32_t now) {
  scene_.setIndependent(false);
  scene_.reset();
  for (uint8_t i = 0; i < stepcontrol::kKnobCount; ++i)
    colors_[i] = stepcontrol::kKnobs[i].color;
  for (auto& value : values_) value = 0;
  stepIndex_ = 2;
  touched_ = false;
  rgbEnabled_ = true;
  lastTouch_ = startedAt_ = now;
  transitionMs_ = 0;
  fadeStartedAt_ = motionStartedAt_ = tailStartedAt_ = now;
  motionPosition_ = motionVelocity_ = motionTarget_ = 0.0f;
  tailFrom_ = tailTarget_ = 1.0f;
  accentStartedAt_ = now;
  accentHue_ = 0.0f;
}

void Controller::touch(uint8_t knob, uint32_t now, bool recolor) {
  const bool entering = !active(now);
  // Mode/color fades have their own clock: rotation never restarts them.
  if (entering || recolor) {
    render(now,fadeFrom_);
    fadeStartedAt_ = now;
    transitionMs_ = entering ? kEnterTransitionMs : kColorTransitionMs;
  }
  if (entering) {
    accentHue_ = colorHue(fadeFrom_[scene_.pixel(knob)]);
    accentStartedAt_ = now;
  }
  scene_.select(knob);
  scene_.setColor(knob, colors_[knob]);
  lastTouch_ = now;
  touched_ = true;
}

void Controller::move(uint8_t knob, int8_t direction, uint32_t now) {
  if (knob >= stepcontrol::kKnobCount || !direction) return;
  touch(knob, now);
  const Motion motion = motionAt(now);
  const float sign = direction > 0 ? 1.0f : -1.0f;
  float target = motionTarget_-sign;
  const float wrap = target < 0.0f ? float(rgbshow::kLedCount) :
                     target >= rgbshow::kLedCount ? -float(rgbshow::kLedCount) : 0.0f;
  motionPosition_ = motion.position+wrap;
  motionVelocity_ = motion.velocity;
  motionTarget_ = target+wrap;
  motionStartedAt_ = now;
  if (sign != tailTarget_) {
    tailFrom_ = tailFacingAt(now);
    tailTarget_ = sign;
    tailStartedAt_ = now;
  }
  scene_.move(knob, direction);
  if (knob == 3) {
    if (direction > 0 && stepIndex_ < 3) ++stepIndex_;
    else if (direction < 0 && stepIndex_ > 0) --stepIndex_;
  } else {
    int32_t next = values_[knob] + (direction > 0 ? kSteps[stepIndex_] : -kSteps[stepIndex_]);
    values_[knob] = next < 0 ? 0 : next > kMaxValue ? kMaxValue : next;
  }
}

void Controller::press(uint8_t knob, uint32_t now) {
  if (knob >= stepcontrol::kKnobCount) return;
  touch(knob, now,true);
  colors_[knob] = (colors_[knob] + 1) % stepcontrol::kPaletteCount;
  choosePaletteColor(knob);
}

void Controller::setColor(uint8_t knob, uint8_t color, uint32_t now) {
  if (knob >= stepcontrol::kKnobCount || color >= stepcontrol::kPaletteCount) return;
  touch(knob, now,true);
  colors_[knob] = color;
  choosePaletteColor(knob);
}

void Controller::choosePaletteColor(uint8_t knob) {
  scene_.setColor(knob,colors_[knob]);
  stepcontrol::Rgb paletteFrame[rgbshow::kLedCount];
  scene_.render(paletteFrame);
  accentHue_ = colorHue(paletteFrame[scene_.pixel(knob)]);
  accentStartedAt_ = lastTouch_;
}

Controller::Motion Controller::motionAt(uint32_t now) const {
  const uint32_t elapsed = uint32_t(now-motionStartedAt_);
  if (elapsed >= 300) return {motionTarget_,0.0f};
  const float t = float(elapsed);
  const float offset = motionPosition_-motionTarget_;
  const float slope = motionVelocity_+offset/kMotionTauMs;
  const float decay = std::exp(-t/kMotionTauMs);
  return {motionTarget_+(offset+slope*t)*decay,
          (motionVelocity_-slope*t/kMotionTauMs)*decay};
}

float Controller::visualPosition(uint32_t now) const {
  const float position = motionAt(now).position;
  return position-std::floor(position/rgbshow::kLedCount)*rgbshow::kLedCount;
}

float Controller::tailFacingAt(uint32_t now) const {
  return tailFrom_+(tailTarget_-tailFrom_)*easedProgress(uint32_t(now-tailStartedAt_),kTailTurnMs);
}

float Controller::accentHueAt(uint32_t now) const {
  if (!touched_) return accentHue_;
  // The return animation starts from the actual color at the idle deadline.
  const uint32_t sample = active(now) ? now : uint32_t(lastTouch_+kIdleMs);
  const float h = accentHue_+float(uint32_t(sample-accentStartedAt_)%kManualColorPeriodMs)/kManualColorPeriodMs;
  return h-std::floor(h);
}

uint16_t Controller::accentHueDegrees(uint32_t now) const {
  return uint16_t(accentHueAt(now)*360.0f)%360;
}

void Controller::renderManual(uint32_t now, stepcontrol::Rgb* pixels) const {
  float mask[rgbshow::kLedCount] = {};
  const float position = visualPosition(now);
  const float clockwise = (tailFacingAt(now)+1.0f)*0.5f;
  deposit(mask,position,255.0f);
  for (uint8_t age=1;age<stepcontrol::kTrailLength;++age) {
    deposit(mask,position+age,stepcontrol::kTrailLevels[age]*clockwise);
    deposit(mask,position-age,stepcontrol::kTrailLevels[age]*(1.0f-clockwise));
  }
  const float accent = accentHueAt(now);
  const stepcontrol::Rgb background = hue(accent+0.5f,kBackgroundLevel);
  const stepcontrol::Rgb foreground = hue(accent,255);
  for (uint16_t i=0;i<rgbshow::kLedCount;++i) {
    pixels[i] = blend(background,foreground,mask[i]/255.0f);
  }
}

int32_t Controller::value(uint8_t knob) const {
  return knob == 3 ? kSteps[stepIndex_] : knob < 3 ? values_[knob] : 0;
}

Snapshot Controller::snapshot() const {
  return {{values_[0], values_[1], values_[2]}, kSteps[stepIndex_], scene_.selected()};
}

bool Controller::setParameters(int32_t p, int32_t i, int32_t d, int32_t step) {
  if (p<0 || p>kMaxValue || i<0 || i>kMaxValue || d<0 || d>kMaxValue) return false;
  uint8_t index = 0;
  while (index<4 && kSteps[index]!=step) ++index;
  if (index==4) return false;
  values_[0]=p; values_[1]=i; values_[2]=d; stepIndex_=index;
  return true;
}

void Controller::renderIdle(uint32_t now, stepcontrol::Rgb* pixels) const {
  // Resume from the stopped head's color and position at the idle deadline.
  const uint32_t elapsed = touched_ ? uint32_t(now-lastTouch_)-kIdleMs : uint32_t(now-startedAt_);
  const float phase = float(elapsed%kBreathPeriodMs)/kBreathPeriodMs+(touched_ ? 0.25f : 0.0f);
  const float breath = (1.0f-std::cos(kTau*(phase+0.25f)))*0.5f;
  // Adjacent LEDs differ by one small hue step. Every LED shares the same breath.
  const uint8_t level = uint8_t(kMinimumBreathLevel + (255-kMinimumBreathLevel)*breath);
  const float colorPhase = float(elapsed%kColorPeriodMs)/kColorPeriodMs+
      (touched_ ? accentHueAt(now)-float(scene_.pixel(scene_.selected()))/rgbshow::kLedCount : 0.0f);
  for (uint16_t i = 0; i < rgbshow::kLedCount; ++i)
    pixels[i] = hue(colorPhase+float(i)/rgbshow::kLedCount,level);
}

bool Controller::transitioning(uint32_t now) const {
  if (!touched_) return false;
  const uint32_t elapsed = uint32_t(now-lastTouch_);
  return elapsed < kIdleMs ? uint32_t(now-fadeStartedAt_) < transitionMs_ : elapsed-kIdleMs < kReturnTransitionMs;
}

void Controller::render(uint32_t now, stepcontrol::Rgb* pixels) const {
  if (!rgbEnabled_) {
    for (uint16_t i=0;i<rgbshow::kLedCount;++i) pixels[i] = {};
    return;
  }
  stepcontrol::Rgb manualFrame[rgbshow::kLedCount];
  if (active(now)) {
    renderManual(now,manualFrame);
    const float amount = easedProgress(uint32_t(now-fadeStartedAt_),transitionMs_);
    for (uint16_t i=0;i<rgbshow::kLedCount;++i)
      pixels[i] = blend(fadeFrom_[i],manualFrame[i],amount);
    return;
  }
  renderIdle(now,pixels);
  if (touched_ && transitioning(now)) {
    renderManual(uint32_t(lastTouch_+kIdleMs),manualFrame);
    const float amount = easedProgress(uint32_t(now-lastTouch_)-kIdleMs,kReturnTransitionMs);
    for (uint16_t i=0;i<rgbshow::kLedCount;++i)
      pixels[i] = blend(manualFrame[i],pixels[i],amount);
  }
}
}  // namespace hostui
