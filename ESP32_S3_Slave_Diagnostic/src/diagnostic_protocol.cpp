#include "diagnostic_protocol.h"
#include <cstring>

namespace diagnostic {
namespace {
bool byte(const char* text, uint8_t& value) {
  if (!text || !*text) return false;
  unsigned result=0;
  for (;*text;++text) {
    if (*text<'0' || *text>'9') return false;
    result=result*10+unsigned(*text-'0');
    if (result>255) return false;
  }
  value=uint8_t(result);
  return true;
}
}
bool parse(const char* text, Command& result) {
  result=Command();
  if (!text || std::strlen(text)>=128) return false;
  char buffer[128]; std::strcpy(buffer,text);
  const char* words[5]; unsigned count=0;
  char* next=nullptr;
  for (char* word=strtok_r(buffer," \t",&next);word;word=strtok_r(nullptr," \t",&next)) {
    if (count==5) return false;
    words[count++]=word;
  }
  if (!count) return false;
  Command parsed;
  const struct {const char* name; Kind kind;} names[] = {
    {"ping",Kind::Ping},{"info",Kind::Info},{"status",Kind::Status},{"pins",Kind::Pins},
    {"rgb",Kind::Rgb},{"off",Kind::Off},{"auto",Kind::Auto},{"watch",Kind::Watch},{"help",Kind::Help},
    {"link",Kind::Link},{"radio",Kind::Radio},{"swd",Kind::Swd},{"swdcheck",Kind::SwdCheck},
    {"swdhold",Kind::SwdHold},{"swdrelease",Kind::SwdRelease},{"swdprobe",Kind::SwdProbe},{"swdunprotect",Kind::SwdUnprotect},{"ramcheck",Kind::RamCheck},{"starttrace",Kind::StartTrace}};
  for (const auto& entry:names) if (!std::strcmp(words[0],entry.name)) parsed.kind=entry.kind;
  if (parsed.kind==Kind::Invalid) return false;
  if (parsed.kind==Kind::Rgb) {
    if (count!=4 || !byte(words[1],parsed.red) || !byte(words[2],parsed.green) || !byte(words[3],parsed.blue)) return false;
  } else if (parsed.kind==Kind::SwdUnprotect) {
    if(count!=2||std::strcmp(words[1],"ERASE_ALL_F407VE"))return false;
  } else if (parsed.kind==Kind::Watch || parsed.kind==Kind::Radio) {
    if (count!=2 || (std::strcmp(words[1],"on") && std::strcmp(words[1],"off"))) return false;
    parsed.enabled=!std::strcmp(words[1],"on");
  } else if (count!=1) return false;
  result=parsed;
  return true;
}
}  // namespace diagnostic
