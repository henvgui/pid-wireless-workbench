#include "step_control.h"

namespace stepcontrol {
// Module pads verified against the original host PCB netlist; these are GPIO IDs.
const KnobPins kKnobs[kKnobCount] = {
    {"P",4,5,6,10,3}, {"I",7,10,11,20,5},
    {"D",12,13,14,30,6}, {"STEP",15,16,17,0,0}};
const char* const kColorNames[kPaletteCount] = {
    "red", "orange", "yellow", "green", "cyan", "blue", "purple", "pink"};
const uint8_t kTrailLevels[kTrailLength] = {255,170,110,70,42,24,12};
namespace {
const Rgb palette[kPaletteCount] = {
    {255,0,0}, {255,70,0}, {255,200,0}, {0,255,0},
    {0,220,255}, {0,0,255}, {150,0,255}, {255,0,100}};
}

void Button::begin(bool pressed, uint32_t now) {
  raw_ = stable_ = pressed;
  changedAt_ = now;
}

bool Button::update(bool pressed, uint32_t now) {
  if (pressed != raw_) {
    raw_ = pressed;
    changedAt_ = now;
  }
  if (raw_ == stable_ || uint32_t(now - changedAt_) < kButtonDebounceMs)
    return false;
  stable_ = raw_;
  return stable_;
}

void HoldButton::begin(bool pressed, uint32_t now) {
  raw_ = stable_ = pressed;
  armed_ = reported_ = false;
  changedAt_ = pressedAt_ = now;
}

ButtonEvent HoldButton::update(bool pressed, uint32_t now) {
  if (pressed != raw_) {
    raw_ = pressed;
    changedAt_ = now;
  }
  if (raw_ != stable_ && uint32_t(now-changedAt_) >= kButtonDebounceMs) {
    stable_ = raw_;
    if (stable_) {
      pressedAt_ = changedAt_;
      armed_ = true;
      reported_ = false;
    } else if (armed_) {
      armed_ = false;
      if (!reported_) {
        // Measure to the raw release, not to its later debounce deadline.
        return uint32_t(changedAt_-pressedAt_) >= kButtonHoldMs ?
            ButtonEvent::Hold : ButtonEvent::Click;
      }
    }
  }
  if (armed_ && stable_ && raw_ && !reported_ &&
      uint32_t(now-pressedAt_) >= kButtonHoldMs) {
    reported_ = true;
    return ButtonEvent::Hold;
  }
  return ButtonEvent::None;
}

void Comet::reset(uint16_t position, uint8_t color) {
  for (uint8_t i=0; i<kTrailLength; ++i) history_[i] = position % rgbshow::kLedCount;
  historyLength_ = 1;
  color_ = color % kPaletteCount;
}

void Comet::move(int8_t direction) {
  if (!direction) return;
  const uint16_t position = (history_[0] + rgbshow::kLedCount +
                             (direction > 0 ? 1 : -1)) % rgbshow::kLedCount;
  for (uint8_t i=kTrailLength-1; i>0; --i) history_[i] = history_[i-1];
  history_[0] = position;
  if (historyLength_ < kTrailLength) ++historyLength_;
}

void Comet::render(Rgb* pixels) const {
  for (uint16_t i=0; i<rgbshow::kLedCount; ++i) pixels[i] = {};
  const Rgb color = palette[color_];
  // Oldest first: revisiting a pixel never creates a second full-brightness head.
  for (uint8_t age=historyLength_; age>0; --age) {
    const uint8_t level = kTrailLevels[age-1];
    pixels[toPixel(history_[age-1])] = {
        uint8_t(uint16_t(color.r)*level/255),
        uint8_t(uint16_t(color.g)*level/255),
        uint8_t(uint16_t(color.b)*level/255)};
  }
}

void KnobScene::reset() {
  for (uint8_t i=0;i<kKnobCount;++i) cursors_[i].reset(kKnobs[i].position,kKnobs[i].color);
  shared_.reset();
  selected_ = 3;
}
void KnobScene::move(uint8_t knob, int8_t direction) {
  if (knob >= kKnobCount) return;
  selected_ = knob;
  cursor(knob).move(direction);
}
void KnobScene::nextColor(uint8_t knob) {
  if (knob >= kKnobCount) return;
  selected_ = knob;
  cursor(knob).nextColor();
}
void KnobScene::setColor(uint8_t knob, uint8_t color) {
  if (knob >= kKnobCount) return;
  selected_ = knob;
  cursor(knob).setColor(color);
}
uint16_t KnobScene::pixel(uint8_t knob) const {
  return cursor(knob < kKnobCount ? knob : selected_).pixel();
}
uint8_t KnobScene::color(uint8_t knob) const {
  return cursor(knob < kKnobCount ? knob : selected_).color();
}
void KnobScene::render(Rgb* pixels) const {
  if (!independent_) { shared_.render(pixels); return; }
  for (uint16_t i=0;i<rgbshow::kLedCount;++i) pixels[i] = {};
  Rgb layer[rgbshow::kLedCount];
  for (uint8_t knob=0;knob<kKnobCount;++knob) {
    cursors_[knob].render(layer);
    for (uint16_t i=0;i<rgbshow::kLedCount;++i) {
      if (layer[i].r > pixels[i].r) pixels[i].r = layer[i].r;
      if (layer[i].g > pixels[i].g) pixels[i].g = layer[i].g;
      if (layer[i].b > pixels[i].b) pixels[i].b = layer[i].b;
    }
  }
}
}  // namespace stepcontrol
