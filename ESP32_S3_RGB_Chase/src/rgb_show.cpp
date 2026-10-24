#include "rgb_show.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace rgbshow {
const char* const kEffectNames[kEffectCount] = {
    "rainbow_flow", "twin_comets", "aurora", "scanner",
    "rainbow_chase", "ripples", "fire", "starfield",
    "plasma", "ocean", "confetti", "fireflies",
    "matrix", "lava", "fireworks", "heartbeat",
    "candy", "rainbow_zipper", "color_waves", "sunrise"};

namespace {
constexpr float TAU = 6.28318530718f;
constexpr uint16_t LED_COUNT = kLedCount;
float clamp01(float x) {
  return fminf(1.0f, fmaxf(0.0f, x));
}

float wrap(float x, float length) {
  return x - floorf(x / length) * length;
}

float wave(float phase) {
  return 0.5f + 0.5f * sinf(phase);
}

Color scaled(Color color, float level) {
  return {color.r * level, color.g * level, color.b * level};
}

Color add(Color a, Color b) {
  return {a.r + b.r, a.g + b.g, a.b + b.b};
}

Color mix(Color a, Color b, float weight) {
  return {a.r + (b.r - a.r) * weight,
          a.g + (b.g - a.g) * weight,
          a.b + (b.b - a.b) * weight};
}

Color hue(float position) {
  const float h = wrap(position, 1.0f) * 6.0f;
  const int sector = static_cast<int>(h);
  const float f = h - sector;
  switch (sector) {
    case 0: return {1, f, 0};
    case 1: return {1-f, 1, 0};
    case 2: return {0, 1, f};
    case 3: return {0, 1-f, 1};
    case 4: return {f, 0, 1};
    default: return {1, 0, 1-f};
  }
}

uint32_t hash(uint32_t value) {
  value ^= value >> 16;
  value *= 0x7feb352dU;
  value ^= value >> 15;
  value *= 0x846ca68bU;
  return value ^ (value >> 16);
}

float noise(uint32_t seed) {
  return static_cast<float>(hash(seed) & 65535U) / 65535.0f;
}

float cometTail(float distance, float length) {
  const float level = clamp01(1.0f - distance / length);
  return level * level;
}

float smoothNoise(uint16_t pixel, uint32_t now, uint32_t period) {
  const uint32_t tick = now / period;
  const float fraction = (now % period) / static_cast<float>(period);
  const float blend = fraction * fraction * (3 - 2 * fraction);
  const uint32_t seed = pixel * 137U + tick * 911U;
  return noise(seed) * (1-blend) + noise(seed+911U) * blend;
}

Color effectPixel(uint8_t effect, uint16_t pixel, uint32_t now) {
  const float t = static_cast<float>(now) * 0.001f;
  const float x = static_cast<float>(pixel);
  const float u = x / static_cast<float>(LED_COUNT);

  switch (effect) {
    case 0: {  // 彩虹流水：色带旋转，同时有柔和明暗波。
      const float level = 0.25f + 0.75f * wave(u * TAU * 2.0f - t * 3.0f);
      return scaled(hue(u * 1.5f - t * 0.15f), level);
    }
    case 1: {  // 两颗反向运动的彗星，带彩色拖尾。
      const float a = wrap(t * 13.0f, LED_COUNT);
      const float b = wrap(-t * 10.0f + LED_COUNT * 0.5f, LED_COUNT);
      Color color = scaled(hue(u + t * 0.05f), 0.025f);
      color = add(color, scaled(hue(t * 0.08f),
                                cometTail(wrap(a - x, LED_COUNT), 11.0f)));
      return add(color, scaled(hue(t * 0.08f + 0.5f),
                               cometTail(wrap(x - b, LED_COUNT), 9.0f)));
    }
    case 2: {  // 极光：青绿、蓝紫光带以不同速度叠加。
      const float green = wave(u * TAU * 2.0f - t * 1.4f);
      const float violet = wave(u * TAU * 3.0f + t * 0.9f);
      const float blue = wave(u * TAU - t * 0.7f);
      return {0.4f * violet * violet, 0.9f * green * green,
              0.2f + 0.55f * blue + 0.25f * violet};
    }
    case 3: {  // 往返扫描：两条光带交会，端点自然反向。
      const float head = wave(t * 2.1f - TAU * 0.25f) * (LED_COUNT - 1);
      const float first = cometTail(fabsf(x - head), 7.0f);
      const float second = cometTail(fabsf(x - (LED_COUNT - 1 - head)), 5.0f);
      return add(scaled(hue(t * 0.09f), first),
                 scaled(hue(t * 0.09f + 0.45f), second * 0.7f));
    }
    case 4: {  // 彩虹追逐：亮点穿过低亮度背景色带。
      const float distance = wrap(x - t * 14.0f, 5.0f);
      const float level = 0.06f + 0.94f * cometTail(distance, 2.2f);
      return scaled(hue(u - t * 0.12f), level);
    }
    case 5: {  // 多点涟漪：彩色波纹向两侧扩散。
      Color color = {0.0f, 0.0f, 0.0f};
      for (uint8_t source = 0; source < 3; ++source) {
        const float origin = (source + 0.5f) * LED_COUNT / 3.0f;
        const float age = wrap(t + source * 1.1f, 3.5f);
        const float ring = cometTail(fabsf(fabsf(x - origin) - age * 10.0f), 2.5f);
        color = add(color, scaled(hue(t * 0.06f + source / 3.0f),
                                  ring * (1.0f - age / 4.0f)));
      }
      return color;
    }
    case 6: {  // 火焰流：平滑噪声组成橙红流动和金色火星。
      const uint32_t tick = now / 90;
      const float fraction = static_cast<float>(now % 90) / 90.0f;
      const float flicker = noise(pixel * 137U + tick * 911U) * (1.0f - fraction)
                          + noise(pixel * 137U + (tick + 1) * 911U) * fraction;
      const float heat = clamp01(0.2f + 0.5f * wave(u * TAU * 2.0f - t * 4.0f)
                                + flicker * 0.45f);
      return {heat, heat * heat * 0.6f, heat * heat * heat * 0.045f};
    }
    case 7: {  // 星空：每颗灯独立呼吸，彩色星点叠加白色亮芯。
      const uint32_t seed = hash(pixel * 1237U + 17U);
      const float frequency = 0.2f + 0.25f * noise(seed);
      const float phase = wrap(t * frequency + noise(seed + 1U), 1.0f);
      const float pulse = sinf(phase * TAU * 0.5f);
      const float twinkle = pulse * pulse * pulse * pulse * pulse * pulse;
      const Color star = mix(hue(noise(seed + 2U) + t * 0.03f), {1, 1, 1},
                           twinkle * twinkle * 0.55f);
      return scaled(star, 0.015f + twinkle * 0.95f);
    }
    case 8: {  // 等离子：三组干涉波组成不断流动的彩色纹理。
      const float field = sinf(u * TAU * 2 + t * 1.1f)
                        + sinf(u * TAU * 5 - t * 0.8f)
                        + sinf(u * TAU * 3 + sinf(t * 0.6f) * 3);
      return scaled(hue(field * 0.17f + t * 0.055f), 0.45f + 0.55f * wave(field));
    }
    case 9: {  // 海浪：深蓝背景、青色浪峰和零散白色浪花。
      const float crest = wave(u * TAU * 3 - t * 1.6f);
      const float swell = wave(u * TAU + t * 0.7f);
      const float foam = powf(crest, 12) * smoothNoise(pixel, now, 250);
      return add(mix({0.005f, 0.04f, 0.25f}, {0, 0.65f, 0.8f}, crest * swell),
                 scaled({0.7f, 0.8f, 1}, foam * 0.75f));
    }
    case 10: {  // 彩纸：随机彩色火星淡入并缓慢消失。
      Color c = scaled(hue(u * 0.5f + t * 0.04f), 0.025f);
      const uint32_t tick = now / 160;
      const float fraction = (now % 160) / 160.0f;
      for (uint32_t age = 0; age < 10; ++age) {
        if (tick < age) continue;
        const uint32_t seed = hash((tick-age) * 313U + 91U);
        if (pixel != seed % LED_COUNT) continue;
        const float life = (age+fraction) / 10;
        const float pulse = sinf(life * TAU * 0.5f);
        c = add(c, scaled(mix(hue(noise(seed+1)), {1,1,1}, 0.12f), pulse*pulse));
      }
      return c;
    }
    case 11: {  // 萤火虫：黄绿光点独立呼吸，背景保持深绿色。
      const uint32_t seed = hash(pixel * 701U + 43U);
      const float pulse = wave(t * (0.6f + noise(seed)) + noise(seed+1) * TAU);
      return add({0, 0.015f, 0.002f},
                 scaled(mix({0.1f,1,0.02f}, {1,0.7f,0.01f}, noise(seed+2)), powf(pulse,8)));
    }
    case 12: {  // 代码雨：五段绿色流星，速度和位置各不相同。
      const uint32_t group = pixel / 8;
      const float head = wrap(t * (3 + group * 0.8f) + noise(group+31) * 8, 8);
      const float distance = wrap(head - (pixel % 8), 8);
      const float green = cometTail(distance,5);
      const float core = cometTail(distance,0.8f);
      return {core * 0.6f, 0.015f + green, core * 0.6f};
    }
    case 13: {  // 熔岩：紫红底色上的橙金色光团。
      const float blob = clamp01(wave(u * TAU * 2 + t * 0.6f)
                               * wave(u * TAU * 3 - t * 0.4f) * 1.5f);
      return mix({0.12f,0,0.035f}, {1,0.55f,0.015f}, blob*blob);
    }
    case 14: {  // 烟花：位置随机的亮芯向两边散开，最后成为火星。
      const uint32_t event = now / 3200;
      const float age = (now % 3200) * 0.001f;
      const uint32_t seed = hash(event * 997U + 7U);
      const float origin = 4 + noise(seed) * (LED_COUNT-9);
      const float distance = fabsf(x-origin);
      const float ring = cometTail(fabsf(distance-age*11),2.8f) * clamp01(1-age/2.7f);
      const float core = cometTail(distance,3) * clamp01(1-age/0.35f);
      return add(scaled(hue(noise(seed+1)+distance*0.014f),
                        ring * (0.55f + 0.45f * noise(seed+pixel*13))),
                 scaled({1,0.8f,0.5f},core));
    }
    case 15: {  // 心跳：两次柔和脉冲叠加紫红色光带。
      const float phase = wrap(t,2.6f);
      const float a = (phase-0.38f)/0.15f;
      const float b = (phase-0.78f)/0.22f;
      const float beat = clamp01(expf(-a*a)+0.65f*expf(-b*b));
      return scaled(mix({0.15f,0,0.4f},{1,0.02f,0.2f},beat),
                    0.05f + beat * (0.4f+0.6f*wave(u*TAU-t*2)));
    }
    case 16: {  // 糖果色：薄荷、粉红、蓝紫色块沿灯链旋转。
      const Color palette[] = {{1,0.08f,0.3f},{0.03f,0.9f,0.55f},
                               {0.3f,0.08f,1},{1,0.5f,0.05f}};
      const float position = wrap((x-t*5)/5,4);
      const uint8_t segment = static_cast<uint8_t>(position);
      const float edge = clamp01((position-segment-0.7f)/0.3f);
      return mix(palette[segment],palette[(segment+1)%4],edge);
    }
    case 17: {  // 彩虹拉链：两端彩色光点向中间汇合。
      const float center = (LED_COUNT-1)*0.5f;
      const float distance = fabsf(x-center);
      const float level = cometTail(wrap(distance+t*10,7),3);
      return scaled(hue(distance/LED_COUNT+t*0.07f),0.025f+level*0.975f);
    }
    case 18:  // 三色波：红绿蓝波以不同速度穿过整条灯链。
      return {powf(wave(u*TAU*2-t*1.3f),3),powf(wave(u*TAU*3+t*1),3),
              powf(wave(u*TAU-t*0.8f),3)};
    case 19: {  // 日出：暖红、橙金、柔白依次浮现。
      const float dawn = wave(t*0.45f-TAU*0.25f+u*1.5f);
      Color c = mix({0.5f,0.015f,0},{1,0.35f,0.02f},clamp01(dawn*2));
      c = mix(c,{1,0.8f,0.45f},clamp01((dawn-0.5f)*2));
      return scaled(c,0.1f+0.9f*dawn);
    }
    default: return {};

  }
}


bool whitespace(char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n'; }
bool number(const char* text, unsigned minimum, unsigned maximum, unsigned& result) {
  if (!*text) return false;
  unsigned value = 0;
  for (; *text; ++text) {
    if (*text<'0' || *text>'9') return false;
    const unsigned digit = *text-'0';
    if (value>maximum/10 || (value==maximum/10 && digit>maximum%10)) return false;
    value = value*10+digit;
  }
  if (value<minimum) return false;
  result = value;
  return true;
}
}  // namespace

void Show::begin(uint32_t now) {
  effect_ = 0;
  brightness_ = kMaxBrightness;
  speed_ = 100;
  automatic_ = enabled_ = true;
  paused_ = false;
  lastUpdate_ = now;
  animationMs_ = animationFraction_ = effectAge_ = frames_ = 0;
  fadeAge_ = kTransitionMs;
  for (uint16_t i=0; i<kLedCount; ++i) lastFrame_[i] = fadeFrom_[i] = {};
}
void Show::startFade() {
  for (uint16_t i=0; i<kLedCount; ++i) fadeFrom_[i] = lastFrame_[i];
  fadeAge_ = 0;
}
void Show::select(uint8_t effect) {
  startFade();
  effect_ = effect;
  effectAge_ = 0;
}
void Show::update(uint32_t now) {
  const uint32_t elapsed = now-lastUpdate_;
  lastUpdate_ = now;
  if (paused_) return;
  const uint64_t scaledTime = static_cast<uint64_t>(elapsed)*speed_+animationFraction_;
  animationMs_ += static_cast<uint32_t>(scaledTime/100);
  animationFraction_ = scaledTime%100;
  const uint64_t fade = static_cast<uint64_t>(fadeAge_)+elapsed;
  fadeAge_ = fade<kTransitionMs ? static_cast<uint32_t>(fade) : kTransitionMs;
  if (automatic_) {
    const uint64_t age = static_cast<uint64_t>(effectAge_)+elapsed;
    if (age>=kEffectMs) {
      select((effect_+age/kEffectMs)%kEffectCount);
      effectAge_ = age%kEffectMs;
    } else {
      effectAge_ = static_cast<uint32_t>(age);
    }
  }
}
void Show::render(Color* frame) {
  float blend = fadeAge_/static_cast<float>(kTransitionMs);
  blend = blend*blend*(3-2*blend);
  for (uint16_t i=0; i<kLedCount; ++i) {
    Color c = {};
    if (enabled_) {
      c = effectPixel(effect_,i,animationMs_);
      c = {clamp01(c.r),clamp01(c.g),clamp01(c.b)};
      if (fadeAge_<kTransitionMs) c = mix(fadeFrom_[i],c,blend);
    }
    frame[i] = lastFrame_[i] = c;
  }
  ++frames_;
}
void Show::status(uint32_t now, char* reply, size_t capacity) const {
  snprintf(reply,capacity,
           "RGB_SHOW pin=%u leds=%u brightness=%u effect=%u number=%u name=%s "
           "frames=%lu uptime_ms=%lu auto=%u speed=%u paused=%u enabled=%u effects=%u\n",
           static_cast<unsigned>(kPin),static_cast<unsigned>(kLedCount),
           static_cast<unsigned>(brightness_),static_cast<unsigned>(effect_),
           static_cast<unsigned>(effect_+1),kEffectNames[effect_],
           static_cast<unsigned long>(frames_),static_cast<unsigned long>(now),
           automatic_?1U:0U,static_cast<unsigned>(speed_),paused_?1U:0U,
           enabled_?1U:0U,static_cast<unsigned>(kEffectCount));
}
bool Show::command(const char* text, uint32_t now, char* reply, size_t capacity) {
  update(now);
  if (strlen(text)>=128) {
    snprintf(reply,capacity,"ERR command too long\n");
    return false;
  }
  char input[128];
  strcpy(input,text);
  char* verb = input;
  while (whitespace(*verb)) ++verb;
  char* end = verb+strlen(verb);
  while (end>verb && whitespace(end[-1])) *--end='\0';
  char* argument = verb;
  while (*argument && !whitespace(*argument)) ++argument;
  if (*argument) *argument++='\0';
  while (whitespace(*argument)) ++argument;
  const bool noArgument = !*argument;
  unsigned value = 0;
  if (strcmp(verb,"help")==0 && noArgument) {
    snprintf(reply,capacity,
             "RGB_HELP commands end with LF or CRLF:\n"
             "!list | !status | ?\n!effect <1..20|name> | !next | !prev\n"
             "!auto on|off | !speed <25..300> | !brightness <0..40>\n"
             "!pause | !resume | !off | !on\n"
             "Speed is percent; settings reset on reboot; other bytes echo.\n");
    return true;
  }
  if (strcmp(verb,"list")==0 && noArgument) {
    if (!capacity) return true;
    snprintf(reply,capacity,"RGB_EFFECTS");
    for (unsigned i=0; i<kEffectCount; ++i) {
      const size_t used = strlen(reply);
      if (used>=capacity) break;
      snprintf(reply+used,capacity-used," %u=%s",i+1,kEffectNames[i]);
    }
    const size_t used = strlen(reply);
    if (used<capacity) snprintf(reply+used,capacity-used,"\n");
    return true;
  }
  if (strcmp(verb,"status")==0 && noArgument) {
    status(now,reply,capacity);
    return true;
  }
  if (strcmp(verb,"effect")==0) {
    bool found = number(argument,1,kEffectCount,value);
    if (found) --value;
    for (unsigned i=0; !found && i<kEffectCount; ++i) {
      if (strcmp(argument,kEffectNames[i])==0) { value=i; found=true; }
    }
    if (!found) {
      snprintf(reply,capacity,"ERR effect needs 1..20 or a name from !list\n");
      return false;
    }
    select(value);
    automatic_ = paused_ = false;
    enabled_ = true;
  } else if ((strcmp(verb,"next")==0 || strcmp(verb,"prev")==0) && noArgument) {
    select((effect_+(strcmp(verb,"next")==0?1:kEffectCount-1))%kEffectCount);
    automatic_ = paused_ = false;
    enabled_ = true;
  } else if (strcmp(verb,"auto")==0 &&
             (strcmp(argument,"on")==0 || strcmp(argument,"off")==0)) {
    automatic_ = strcmp(argument,"on")==0;
    effectAge_ = 0;
    if (automatic_) { paused_=false; enabled_=true; }
  } else if (strcmp(verb,"speed")==0 && number(argument,25,300,value)) {
    speed_ = value;
  } else if (strcmp(verb,"brightness")==0 && number(argument,0,kMaxBrightness,value)) {
    brightness_ = value;
  } else if (strcmp(verb,"pause")==0 && noArgument) {
    paused_ = true;
  } else if (strcmp(verb,"resume")==0 && noArgument) {
    paused_ = false;
  } else if (strcmp(verb,"off")==0 && noArgument) {
    enabled_ = false;
  } else if (strcmp(verb,"on")==0 && noArgument) {
    if (!enabled_) startFade();
    enabled_ = true;
  } else {
    snprintf(reply,capacity,"ERR invalid command or value; use !help\n");
    return false;
  }
  snprintf(reply,capacity,
           "OK number=%u name=%s auto=%u speed=%u brightness=%u paused=%u enabled=%u\n",
           static_cast<unsigned>(effect_+1),kEffectNames[effect_],automatic_?1U:0U,
           static_cast<unsigned>(speed_),static_cast<unsigned>(brightness_),
           paused_?1U:0U,enabled_?1U:0U);
  return true;
}
}  // namespace rgbshow
