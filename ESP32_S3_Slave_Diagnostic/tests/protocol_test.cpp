#include "diagnostic_protocol.h"
#include "board_pins.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

int main() {
  using namespace diagnostic;
  Command command;
  assert(parse("ping",command) && command.kind==Kind::Ping);
  assert(parse("  rgb\t255 0 128 ",command) && command.kind==Kind::Rgb);
  assert(command.red==255 && command.green==0 && command.blue==128);
  assert(parse("watch on",command) && command.enabled);
  assert(parse("watch off",command) && !command.enabled);
  assert(parse("link",command) && command.kind==Kind::Link);
  assert(parse("swdprobe",command) && command.kind==Kind::SwdProbe);
  assert(parse("ramcheck",command) && command.kind==Kind::RamCheck);
  assert(!parse("ramcheck erase",command));
  assert(parse("swdunprotect ERASE_ALL_F407VE",command) && command.kind==Kind::SwdUnprotect);
  for(const char* text:{"swdunprotect","swdunprotect ERASE","swdunprotect erase_all_f407ve","swdunprotect ERASE_ALL_F407VE extra"})assert(!parse(text,command));
  assert(parse("radio on",command) && command.kind==Kind::Radio && command.enabled);
  assert(parse("radio off",command) && !command.enabled);
  for (const char* text:{"info","status","pins","off","auto","help"}) assert(parse(text,command));
  for (const char* text:{"","rgb","rgb 256 0 0","rgb -1 0 0","rgb 1.0 0 0","rgb 0 0 0 0",
      "rgb 1 2 x","rgb 999999999999999999999999 0 0","watch yes","radio yes","radio on extra","ping extra","reboot","RGB 1 2 3"}) {
    assert(!parse(text,command) && command.kind==Kind::Invalid);
  }
  assert(!parse("swdprobe extra",command));
  assert(!parse(nullptr,command));
  char oversized[129]; std::memset(oversized,'a',128); oversized[128]=0;
  assert(!parse(oversized,command));
  assert(board::kRgb==38 && board::kTranslatorDisable==47 && board::kRgb!=board::kTranslatorDisable);
  assert(board::kDebugTx==43 && board::kDebugRx==44);
  assert(board::kSwdClock==12 && board::kSwdData==13 && board::kSwo==14);
  std::puts("PASS: strict diagnostic arguments, RGB bounds, invalid commands have no action, slave pin separation");
}
