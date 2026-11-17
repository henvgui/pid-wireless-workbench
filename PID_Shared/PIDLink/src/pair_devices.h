#pragma once

#if __has_include("../private/pair_devices.h")
#include "../private/pair_devices.h"
#else
#include <stdint.h>

namespace pidlink {
static constexpr uint8_t kConfiguredHostMac[6]={0x44,0x1b,0xf6,0x8d,0x10,0x20};
static constexpr uint8_t kDefaultSlaveMac[6]={0x44,0x1b,0xf6,0x8d,0x10,0x18};
}
#endif
