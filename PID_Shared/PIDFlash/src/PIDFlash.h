#pragma once
#include "flash_core.h"
namespace pidflash {
void beginHost();
void beginSlave();
void pollHost(uint32_t now);
void pollSlave(uint32_t now);
bool hostCommand(const char* text);
bool hostPending();
bool hostLocked();
bool hostBulkActive();
bool hostBinaryReading();
bool hostBinaryByte(uint8_t value,uint32_t now);
bool takeHostReply(char* output,size_t size);
bool targetActive();
struct SwdDiagnostics {
  uint32_t dpidr;
  uint8_t ack,request,step;
  Error error;
  uint32_t capacityWord,optionBytes;
  uint32_t optionBytesAfter;
  uint8_t recoveryState; // 0 none, 1 running, 2 complete, 3 failed.
};
SwdDiagnostics swdDiagnostics();
// Fixed read-only F407 diagnosis. Pulses reset and halts the core; no Flash/RAM writes.
// Leaves the target halted so an unverified application is never started.
Error probeSwdTarget(uint32_t* values,size_t count);
// Explicit full-erasure recovery, never part of normal programming.
Error recoverSwdTarget();
// Brief static input-path check, no target reset or clock pulses; restores OE#.
Error checkSwdInput(uint8_t& isolated,uint8_t& internalPullup,uint8_t& targetLevel,bool hold=false);
void releaseSwdInput();
// H1 UART0 is receive-only while a remote log session is open.
bool serialLogging();
} // namespace pidflash
