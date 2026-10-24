#pragma once
#include <stddef.h>
#include <stdint.h>

namespace rgbshow {
// 模组第24脚是GPIO47；灯序号按数据串联顺序计数。
constexpr uint8_t kPin = 47;
constexpr uint16_t kLedCount = 40;
constexpr uint8_t kMaxBrightness = 40;
constexpr uint8_t kEffectCount = 20;
constexpr uint32_t kFrameMs = 16;
constexpr uint32_t kEffectMs = 8000;
constexpr uint32_t kTransitionMs = 1000;
extern const char* const kEffectNames[kEffectCount];
struct Color { float r, g, b; };

// 和USB、NeoPixel驱动解耦，后续旋钮可复用灯效控制接口。
class Show {
 public:
  void begin(uint32_t now);
  void update(uint32_t now);
  void render(Color* frame);
  bool command(const char* text, uint32_t now, char* reply, size_t capacity);
  void status(uint32_t now, char* reply, size_t capacity) const;
  uint8_t brightness() const { return brightness_; }
  uint8_t effect() const { return effect_; }
  uint16_t speed() const { return speed_; }
  bool automatic() const { return automatic_; }
  bool paused() const { return paused_; }
  bool enabled() const { return enabled_; }

 private:
  void select(uint8_t effect);
  void startFade();
  uint8_t effect_ = 0;
  uint8_t brightness_ = kMaxBrightness;
  uint16_t speed_ = 100;
  bool automatic_ = true;
  bool paused_ = false;
  bool enabled_ = true;
  uint32_t lastUpdate_ = 0;
  uint32_t animationMs_ = 0;
  uint32_t animationFraction_ = 0;
  uint32_t effectAge_ = 0;
  uint32_t fadeAge_ = kTransitionMs;
  uint32_t frames_ = 0;
  Color lastFrame_[kLedCount] = {};
  Color fadeFrom_[kLedCount] = {};
};
}  // namespace rgbshow
