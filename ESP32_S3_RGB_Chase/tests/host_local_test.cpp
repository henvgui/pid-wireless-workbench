#include "host_local_core.h"
#include "host_ui.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace hostlocal;
int main() {
  int32_t number=77;
  assert(decimal("0",number) && number==0);
  assert(decimal("12.3",number) && number==1230);
  assert(decimal("9999.99",number) && number==999999);
  const char* bad[]={"","-1","+1",".1","1.","1.234","1e2","nan","inf","10000","12x"," 1"};
  for (const char* text : bad) { number=77; assert(!decimal(text,number) && number==77); }
  Command command;
  assert(parse("pid 1.25 0.01 9999.99",command)==Parsed::Ok);
  assert(command.parameters.values[0]==125 && command.parameters.values[1]==1 && command.parameters.values[2]==999999);
  assert(parse("pid 1 2 -3",command)==Parsed::Invalid);
  assert(parse("pid 1 2",command)==Parsed::Invalid);
  assert(parse("step 0.1",command)==Parsed::Ok && command.parameters.step==10);
  assert(parse("step 0.02",command)==Parsed::Invalid);
  assert(parse("run extra",command)==Parsed::Invalid);
  assert(parse("run",command)==Parsed::Ok && command.kind==Kind::Run);
  assert(parse("color next",command)==Parsed::Unknown);
  assert(parse("feedback 2 -45.5 -46.25 0.5",command)==Parsed::Ok && command.channel==2);
  assert(parse("feedback 4 1 2 3",command)==Parsed::Invalid);
  assert(parse("feedback 0 nan 2 3",command)==Parsed::Invalid);
  assert(parse("feedback 0 1 inf 3",command)==Parsed::Invalid);
  assert(parse("feedback 0 1e39 2 3",command)==Parsed::Invalid);
  assert(parse("ackrun 4294967295 1",command)==Parsed::Ok && command.request==UINT32_MAX);
  assert(parse("ackrun 4294967296 1",command)==Parsed::Invalid);
  assert(parse("ackrun 0 1",command)==Parsed::Invalid);
  assert(parse("stream on",command)==Parsed::Ok && command.flag);
  assert(parse("stream 1",command)==Parsed::Invalid);

  const Parameters original={{125,1,999999},10};
  uint8_t bytes[kProfileSize];
  encodeProfile(original,bytes);
  Parameters decoded={{0,0,0},100};
  assert(decodeProfile(bytes,sizeof(bytes),decoded) && equal(decoded,original));
  for (size_t byte=0;byte<sizeof(bytes);++byte) {
    for (unsigned bit=0;bit<8;++bit) {
      bytes[byte]^=uint8_t(1u<<bit);
      const Parameters before=decoded;
      assert(!decodeProfile(bytes,sizeof(bytes),decoded) && equal(before,decoded));
      bytes[byte]^=uint8_t(1u<<bit);
    }
  }
  assert(!decodeProfile(bytes,sizeof(bytes)-1,decoded));
  Parameters invalid={{1,-1,2},100}; encodeProfile(invalid,bytes);
  assert(!decodeProfile(bytes,sizeof(bytes),decoded));

  Session session;
  session.begin(original,false);
  assert(session.dirty() && !session.saved() && !session.runConfirmed() && !session.requestedRun());
  session.markSaved(); assert(!session.dirty());
  Parameters next=original; ++next.values[1];
  assert(session.observe(next) && session.dirty() && session.revision()==1);
  assert(!session.observe(next) && session.revision()==1);
  assert(!session.observe(invalid) && equal(session.parameters(),next));
  assert(session.observe(original) && !session.dirty());
  session.forget(); assert(!session.saved() && session.dirty());
  assert(!session.acknowledgeRun(0,false));
  assert(session.requestRun(true)); const uint32_t first=session.runRequest();
  assert(!session.requestRun(true) && session.runRequest()==first);
  assert(!session.acknowledgeRun(first,false) && !session.runConfirmed());
  assert(session.acknowledgeRun(first,true) && session.runConfirmed());
  assert(session.requestRun(false) && session.runRequest()!=first && !session.runConfirmed());
  assert(!session.acknowledgeRun(first,true));
  assert(session.acknowledgeRun(session.runRequest(),false));
  session.begin(original,true);
  assert(!session.requestedRun() && !session.runConfirmed() && !session.runRequest());

  hostui::Controller ui; ui.reset(0);
  stepcontrol::Rgb before[40],after[40]; ui.render(1500,before);
  assert(ui.setParameters(125,250,375,10));
  assert(ui.value(0)==125 && ui.value(1)==250 && ui.value(2)==375 && ui.value(3)==10);
  ui.render(1500,after);
  assert(!std::memcmp(before,after,sizeof(before)) && !ui.active(1500));
  assert(!ui.setParameters(999999,999999,-1,1));
  assert(!ui.setParameters(1,2,3,2));
  assert(ui.value(0)==125 && ui.value(1)==250 && ui.value(2)==375 && ui.value(3)==10);
  ui.move(0,1,1600); assert(ui.value(0)==135);
  puts("PASS: strict USB arguments, finite feedback, CRC/versioned profiles and atomic rejection, save/dirty state, request acknowledgement sequencing, restart defaults and RGB-independent parameter restore");
}
