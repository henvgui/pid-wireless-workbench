#pragma once
#include <stddef.h>
#include <stdint.h>

namespace hostlocal {
constexpr uint8_t kRunPin = 18, kSavePin = 21;
constexpr int32_t kMaximum = 999999;
constexpr size_t kProfileSize = 24;
struct Parameters {
  int32_t values[3];
  int32_t step;
};
bool valid(const Parameters& value);
bool equal(const Parameters& a, const Parameters& b);
bool decimal(const char* text, int32_t& hundredths);
void encodeProfile(const Parameters& value, uint8_t* bytes);
bool decodeProfile(const uint8_t* bytes, size_t size, Parameters& value);

enum class Kind { Unknown, Get, Pid, Step, Run, Stop, Save, Load, Forget, Stream, Feedback, AckRun };
enum class Parsed { Unknown, Invalid, Ok };
struct Command {
  Kind kind = Kind::Unknown;
  Parameters parameters = {{0,0,0},100};
  uint32_t request = 0;
  uint8_t channel = 0;
  bool flag = false;
  float target = 0, measured = 0, output = 0;
};
Parsed parse(const char* text, Command& command);

class Session {
 public:
  void begin(const Parameters& current, bool saved);
  bool observe(const Parameters& current);
  void markSaved() { saved_ = current_; savedValid_ = true; }
  void forget() { savedValid_ = false; }
  bool dirty() const { return !savedValid_ || !equal(current_,saved_); }
  bool saved() const { return savedValid_; }
  const Parameters& parameters() const { return current_; }
  uint32_t revision() const { return revision_; }
  bool requestRun(bool running);
  bool acknowledgeRun(uint32_t request, bool running);
  bool requestedRun() const { return requestedRun_; }
  uint32_t runRequest() const { return runRequest_; }
  bool runConfirmed() const { return confirmed_; }
  void invalidateRunAcknowledgement() { confirmed_=false; }
 private:
  Parameters current_ = {{0,0,0},100}, saved_ = {{0,0,0},100};
  bool savedValid_ = false, requestedRun_ = false, confirmed_ = false;
  uint32_t revision_ = 0, runRequest_ = 0;
};
}  // namespace hostlocal
