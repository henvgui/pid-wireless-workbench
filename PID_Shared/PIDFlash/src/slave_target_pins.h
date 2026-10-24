#pragma once
#include <cstdint>

namespace pidflash { namespace slavepins {
// Slave schematic 2026-10-16: module pads 20/21/22, respectively.
// GPIO11 (module pad 19) is not connected to the SWD interface.
constexpr uint8_t kSwdClock=12;
constexpr uint8_t kSwdData=13;
constexpr uint8_t kSwo=14;
} }
