#pragma once
#include <cstdint>

namespace diagnostic {
enum class Kind { Invalid, Ping, Info, Status, Pins, Rgb, Off, Auto, Watch, Help, Link, Radio, Swd, SwdCheck, SwdHold, SwdRelease, SwdProbe, SwdUnprotect, RamCheck, StartTrace };
struct Command {
  Kind kind = Kind::Invalid;
  uint8_t red = 0, green = 0, blue = 0;
  bool enabled = false;
};
bool parse(const char* text, Command& result);
}  // namespace diagnostic
