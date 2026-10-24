#pragma once
#include "host_ui.h"

namespace hostoled {
struct Health {
  bool busReady, acknowledged;
  uint8_t address, error;
  uint32_t frames;
};
void begin(const hostui::Snapshot& initial);
void publish(const hostui::Snapshot& value);
#if defined(PID_HOST_LOCAL)
void publishHeader(const char* text);
#endif
Health health();
}  // namespace hostoled
