#pragma once
#include <cstdint>
#if defined(PID_FLASH_ENABLED)
#include <slave_target_pins.h>
#endif

// SCH_Schematic2_2026-10-16.pdf, slave, page 1. GPIO numbers, not module pad numbers.
namespace board {
constexpr uint8_t kRgb = 38;             // U2 pad 31 -> U3 A -> LED1 DI.
constexpr uint8_t kTranslatorDisable = 47; // U4/U5 OE#, HIGH disables both.
constexpr uint8_t kSwdDirection = 21;    // U4 DIR4; keep at input/pullup.
constexpr uint8_t kDebugTx = 43;         // U2 pad 37, TXD0 -> H1 pin 3.
constexpr uint8_t kDebugRx = 44;         // U2 pad 36, RXD0 -> H1 pin 2.
constexpr uint8_t kTargetTx = 17;
constexpr uint8_t kTargetRx = 18;
constexpr uint8_t kTargetReset = 15;
constexpr uint8_t kTargetBoot = 16;
#if defined(PID_FLASH_ENABLED)
constexpr uint8_t kSwdClock = pidflash::slavepins::kSwdClock;
constexpr uint8_t kSwdData = pidflash::slavepins::kSwdData;
constexpr uint8_t kSwo = pidflash::slavepins::kSwo;
#else
constexpr uint8_t kSwdClock = 12;
constexpr uint8_t kSwdData = 13;
constexpr uint8_t kSwo = 14;
#endif
constexpr uint8_t kSda = 8;
constexpr uint8_t kScl = 9;
constexpr uint8_t kExternal[] = {4,5,6,7};
}  // namespace board
