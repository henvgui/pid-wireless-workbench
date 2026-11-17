#pragma once
#include "host_ui.h"

namespace hostapp {
struct Status {
  bool storage, saved, dirty, runRequested, runAcknowledged;
  uint32_t runRequest, revision;
};
void begin(hostui::Controller& ui);
void poll(uint32_t now);
bool handleCommand(const char* text, uint32_t now);
bool canAcceptCommand();
bool takeDisplayDirty();
Status status();
void nextPeer(uint32_t now);
}  // namespace hostapp
