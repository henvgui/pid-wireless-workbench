#include "host_local_core.h"
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace hostlocal {
namespace {
bool stepValid(int32_t step) { return step==1 || step==10 || step==100 || step==1000; }
uint16_t crc(const uint8_t* bytes, size_t length) {
  uint16_t value = 0xFFFF;
  for (size_t i=0;i<length;++i) {
    value ^= uint16_t(bytes[i])<<8;
    for (uint8_t bit=0;bit<8;++bit) value = value&0x8000 ? uint16_t((value<<1)^0x1021) : uint16_t(value<<1);
  }
  return value;
}
void put32(uint8_t* bytes, uint32_t value) {
  for (uint8_t i=0;i<4;++i) bytes[i] = uint8_t(value>>(i*8));
}
uint32_t get32(const uint8_t* bytes) {
  uint32_t value = 0;
  for (uint8_t i=0;i<4;++i) value |= uint32_t(bytes[i])<<(i*8);
  return value;
}
bool unsignedNumber(const char* text, uint32_t& value) {
  if (!text || !*text) return false;
  uint32_t result = 0;
  for (;*text;++text) {
    if (*text<'0' || *text>'9') return false;
    const uint32_t digit = *text-'0';
    if (result>(UINT32_MAX-digit)/10) return false;
    result = result*10+digit;
  }
  value = result;
  return true;
}
bool real(const char* text, float& value) {
  if (!text || !*text) return false;
  char* end = nullptr;
  const float result = std::strtof(text,&end);
  if (*end || !std::isfinite(result) || std::fabs(result)>1000000.0f) return false;
  value = result;
  return true;
}
}

bool valid(const Parameters& value) {
  for (int32_t item : value.values) if (item<0 || item>kMaximum) return false;
  return stepValid(value.step);
}
bool equal(const Parameters& a, const Parameters& b) {
  return a.values[0]==b.values[0] && a.values[1]==b.values[1] && a.values[2]==b.values[2] && a.step==b.step;
}
bool decimal(const char* text, int32_t& hundredths) {
  if (!text || *text<'0' || *text>'9') return false;
  int32_t whole = 0, fraction = 0;
  while (*text>='0' && *text<='9') {
    if (whole>999 || whole*10+(*text-'0')>9999) return false;
    whole = whole*10+(*text++-'0');
  }
  if (*text=='.') {
    ++text;
    if (*text<'0' || *text>'9') return false;
    fraction = (*text++-'0')*10;
    if (*text>='0' && *text<='9') fraction += *text++-'0';
  }
  if (*text) return false;
  hundredths = whole*100+fraction;
  return true;
}
void encodeProfile(const Parameters& value, uint8_t* bytes) {
  std::memset(bytes,0,kProfileSize);
  bytes[0]='P'; bytes[1]='I'; bytes[2]='D'; bytes[3]=1;
  for (uint8_t i=0;i<3;++i) put32(bytes+4+i*4,uint32_t(value.values[i]));
  put32(bytes+16,uint32_t(value.step));
  const uint16_t checksum = crc(bytes,22);
  bytes[22]=uint8_t(checksum); bytes[23]=uint8_t(checksum>>8);
}
bool decodeProfile(const uint8_t* bytes, size_t size, Parameters& value) {
  if (!bytes || size!=kProfileSize || bytes[0]!='P' || bytes[1]!='I' || bytes[2]!='D' || bytes[3]!=1 ||
      bytes[20] || bytes[21] || crc(bytes,22)!=(uint16_t(bytes[22])|(uint16_t(bytes[23])<<8))) return false;
  Parameters next;
  for (uint8_t i=0;i<3;++i) next.values[i]=int32_t(get32(bytes+4+i*4));
  next.step=int32_t(get32(bytes+16));
  if (!valid(next)) return false;
  value=next;
  return true;
}

Parsed parse(const char* text, Command& command) {
  command = Command();
  if (!text || std::strlen(text)>=128) return Parsed::Invalid;
  char buffer[128]; std::strcpy(buffer,text);
  const char* tokens[8]; unsigned count = 0;
  char* save = nullptr;
  for (char* token=strtok_r(buffer," \t",&save);token;token=strtok_r(nullptr," \t",&save)) {
    if (count==8) return Parsed::Invalid;
    tokens[count++]=token;
  }
  if (!count) return Parsed::Unknown;
  const struct { const char* name; Kind kind; } kinds[] = {
    {"get",Kind::Get},{"pid",Kind::Pid},{"step",Kind::Step},{"run",Kind::Run},{"stop",Kind::Stop},
    {"save",Kind::Save},{"load",Kind::Load},{"forget",Kind::Forget},{"stream",Kind::Stream},
    {"feedback",Kind::Feedback},{"ackrun",Kind::AckRun}};
  for (const auto& item : kinds) if (!std::strcmp(tokens[0],item.name)) command.kind=item.kind;
  if (command.kind==Kind::Unknown) return Parsed::Unknown;
  switch (command.kind) {
    case Kind::Pid:
      if (count!=4) return Parsed::Invalid;
      for (uint8_t i=0;i<3;++i) if (!decimal(tokens[i+1],command.parameters.values[i])) return Parsed::Invalid;
      break;
    case Kind::Step:
      if (count!=2 || !decimal(tokens[1],command.parameters.step) || !stepValid(command.parameters.step)) return Parsed::Invalid;
      break;
    case Kind::Stream:
      if (count!=2 || (std::strcmp(tokens[1],"on") && std::strcmp(tokens[1],"off"))) return Parsed::Invalid;
      command.flag=!std::strcmp(tokens[1],"on"); break;
    case Kind::Feedback: {
      uint32_t channel;
      if (count!=5 || !unsignedNumber(tokens[1],channel) || channel>3 || !real(tokens[2],command.target) ||
          !real(tokens[3],command.measured) || !real(tokens[4],command.output)) return Parsed::Invalid;
      command.channel=uint8_t(channel); break;
    }
    case Kind::AckRun: {
      uint32_t state;
      if (count!=3 || !unsignedNumber(tokens[1],command.request) || !command.request ||
          !unsignedNumber(tokens[2],state) || state>1) return Parsed::Invalid;
      command.flag=state!=0; break;
    }
    default: if (count!=1) return Parsed::Invalid;
  }
  return Parsed::Ok;
}

void Session::begin(const Parameters& current, bool saved) {
  current_ = saved_ = valid(current) ? current : Parameters{{0,0,0},100};
  savedValid_ = saved && valid(current); requestedRun_ = confirmed_ = false;
  revision_ = runRequest_ = 0;
}
bool Session::observe(const Parameters& current) {
  if (!valid(current) || equal(current_,current)) return false;
  current_=current; ++revision_; return true;
}
bool Session::requestRun(bool running) {
  if (runRequest_ && requestedRun_==running) return false;
  requestedRun_=running; confirmed_=false;
  if (++runRequest_==0) ++runRequest_;
  return true;
}
bool Session::acknowledgeRun(uint32_t request, bool running) {
  if (!runRequest_ || request!=runRequest_ || running!=requestedRun_) return false;
  confirmed_=true; return true;
}
}  // namespace hostlocal
