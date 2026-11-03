#include "swd_programmer.h"
#include <cassert>
#include <map>
#include <iostream>
using namespace pidflash;
struct Pins:SwdPins {
  uint32_t time=0;unsigned asserted=0,released=0;
  void output(bool)override{}void data(bool)override{}bool data()override{return false;}
  void clock(bool)override{}void halfPeriod()override{}
  void reset(bool value)override{if(value)++asserted;else ++released;}
  uint32_t now()override{return time;}void wait(uint32_t ms)override{time+=ms;}
};
struct StartTarget:ArmSwd {
  std::map<uint32_t,uint32_t> memory={{kBase,0x20001000},{kBase+4,kBase+0x101}};
  std::map<unsigned,uint32_t> core={{13,0x20002000},{15,0x20000014},{16,1u<<24},{20,1}};
  uint32_t state=0x3000b,dr=0,catchReset=0;
  unsigned resetRequests=0,resumes=0,resetReads=0;
  bool noReset=false,stuckRam=false,wrongMask=false,wrongSp=false,wrongThumb=false,romBoot=false,loseResetAck=false,loseRunAck=false;
  explicit StartTarget(Pins& p):ArmSwd(p){}
  Error read32(uint32_t a,uint32_t& v)override{
    if(a==0xe000edf0){v=state;if(resetRequests&&!noReset&&resetReads++==0)v|=1u<<25;}
    else if(a==0xe000edf8)v=dr;
    else{assert(memory.count(a));v=memory[a];}
    return Error::Ok;
  }
  Error write32(uint32_t a,uint32_t v)override{
    if(a==0xe000edfc){catchReset=v;return Error::Ok;}
    if(a==0xe000edf4){assert(!(v&0x10000));dr=core[v&31];return Error::Ok;}
    if(a==0xe000edf0){assert(v==0xa05f0003||v==0xa05f0001);
      if(v==0xa05f0003)state=0x30003;
      else{++resumes;assert(resetRequests==1&&catchReset==0);state=0x10001;if(loseRunAck)return Error::Timeout;}
      return Error::Ok;
    }
    assert(a==0xe000ed0c&&v==0x05fa0004&&catchReset==1);++resetRequests;
    if(!noReset){core[13]=wrongSp?0x20002000:memory[kBase];core[15]=stuckRam?0x20000014:romBoot?0x1fff0100:memory[kBase+4]&~1u;
      core[20]=wrongMask?1:0;core[16]=wrongThumb?0:1u<<24;state=0x30003;}
    return loseResetAck?Error::Timeout:Error::Ok;
  }
};
int main(){
  {Pins p;StartTarget t(p);assert(t.start()==Error::Ok&&t.resumes==1&&t.resetRequests==1&&t.startStep==6&&t.startResetSeen);
   assert(t.startBeforePc==0x20000014&&t.startBeforeSpecial==1&&t.startPc==kBase+0x100&&t.startSp==0x20001000&&t.startSpecial==0);
   assert(!p.asserted&&p.released==1);}
  {Pins p;StartTarget t(p);t.noReset=true;
   assert(t.start()==Error::Timeout&&t.resetRequests==1&&!t.resumes&&!t.startResetSeen&&p.time==1000);}
  for(unsigned failure=0;failure<5;++failure){Pins p;StartTarget t(p);t.stuckRam=failure==0;t.wrongMask=failure==1;
   t.wrongSp=failure==2;t.wrongThumb=failure==3;t.romBoot=failure==4;
   assert(t.start()==Error::Start&&t.resetRequests==1&&!t.resumes&&t.startResetSeen&&t.startStep==4);}
  {Pins p;StartTarget t(p);t.loseResetAck=true;assert(t.start()==Error::Timeout&&t.resetRequests==1&&!t.resumes);}
  {Pins p;StartTarget t(p);t.loseRunAck=true;assert(t.start()==Error::Timeout&&t.resetRequests==1&&t.resumes==1&&t.startStep==5);}
  {Pins p;StartTarget t(p);t.state=0x10001;assert(t.start()==Error::Start&&!t.resetRequests&&!t.resumes);}
  for(uint32_t sp:{0u,0x20001004u,0x20020008u}){Pins p;StartTarget t(p);t.memory[kBase]=sp;assert(t.start()==Error::Start&&!t.resetRequests);}
  std::cout<<"PASS: system reset observed; Flash PC/SP and PRIMASK reset verified before RUN; stale SRAM/WFI, bad context and ambiguous ACK never report successful start.\n";
}
