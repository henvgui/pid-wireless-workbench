#include "swd_programmer.h"
#include "f407_loader_image.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>
#include <algorithm>
#define RAM_LOADER_NATIVE
#include "../ram_loader/f407_loader.c"
using namespace pidflash;
struct RamMemory;
static RamMemory* active;
struct RamMemory:SwdTarget {
  std::vector<uint8_t> flash=std::vector<uint8_t>(512*1024,0xa5),ram=std::vector<uint8_t>(16384);
  std::map<uint32_t,uint32_t> regs={{0xe0042000,0x413},{0x1fff7a20,512u<<16},{0x40023c14,0x0fffaaed},{0x40023c10,0x80000000}};
  unsigned erases=0,programs=0,calls=0,keys=0,starts=0;uint32_t time=0;
  bool corruptCode=false,corruptBuffer=false,stale=false,timeout=false,flashError=false,forceBusy=false,failProbe=false,legacy=false;
  Error connect() override {active=this;return Error::Ok;}
  uint32_t word(uint32_t a){if(a>=RAM_CODE&&a<RAM_CODE+ram.size())return get32(ram.data()+a-RAM_CODE);
    if(a>=kBase&&a<kBase+flash.size())return get32(flash.data()+a-kBase);
    if(a==0x40023c0c&&forceBusy)return 0x10000;
    return regs[a];}
  void store(uint32_t a,uint32_t v){
    if(a>=RAM_CODE&&a<RAM_CODE+ram.size()){put32(ram.data()+a-RAM_CODE,v);return;}
    if(a>=kBase&&a<kBase+flash.size()){
      assert(regs[0x40023c10]==0x201&&word(a)==0xffffffff);++programs;put32(flash.data()+a-kBase,v);
      if(flashError)regs[0x40023c0c]=0x10;
      return;
    }
    if(a==0x40023c04){if(++keys==2)regs[0x40023c10]&=~0x80000000u;return;}
    if(a==0x40023c0c){regs[a]&=~v;return;}
    regs[a]=v;
    if(a==0x40023c10&&(v&0x10000)){
      static const unsigned bounds[]={0,16384,32768,49152,65536,131072,262144,393216,524288};
      unsigned sector=(v>>3)&15;assert(sector<8);++erases;
      std::fill(flash.begin()+bounds[sector],flash.begin()+bounds[sector+1],255);regs[a]&=~0x10000u;
    }
  }
  Error read32(uint32_t a,uint32_t& v) override {v=word(a);if(corruptCode&&a==RAM_CODE)v^=1;return Error::Ok;}
  Error write32(uint32_t a,uint32_t v) override {store(a,v);return Error::Ok;}
  bool supportsRam() const override {return !legacy;}
  Error runAlgorithm(uint32_t entry,uint32_t sp,uint32_t arg,uint32_t bp,uint32_t) override {
    assert(entry==kRamEntry&&sp==RAM_STACK&&arg==RAM_MAILBOX&&bp==kRamBreakpoint);++calls;
    auto* box=reinterpret_cast<RamMailbox*>(ram.data()+RAM_MAILBOX-RAM_CODE);
    if(timeout&&box->command==RAM_PROGRAM)return Error::Timeout;
    if(failProbe&&box->command==RAM_PROBE)return Error::SwdProtocol;
    if(corruptBuffer&&box->command==RAM_PROGRAM)ram[RAM_DATA-RAM_CODE]^=1;
    active=this;ram_work(box);if(stale)box->returned_nonce^=1;return Error::Ok;
  }
  Error start() override {++starts;return Error::Ok;}
  uint32_t now() override {return time;}
  void wait(uint32_t ms) override {time+=ms;}
};
uint32_t ram_read32(uint32_t a){return active->word(a);}
uint16_t ram_read16(uint32_t a){return uint16_t(active->word(a&~3u)>>((a&2)*8));}
void ram_write32(uint32_t a,uint32_t v){active->store(a,v);}
static std::vector<uint8_t> image(size_t n){std::vector<uint8_t> v(n);for(size_t i=0;i<n;++i)v[i]=uint8_t(i*37);put32(v.data(),0x20020000);put32(v.data()+4,kBase+9);return v;}
struct NullPins:SwdPins {
  uint32_t time=0;void output(bool)override{}void data(bool)override{}bool data()override{return false;}
  void clock(bool)override{}void halfPeriod()override{}void reset(bool)override{}uint32_t now()override{return time;}void wait(uint32_t ms)override{time+=ms;}
};
struct Core:ArmSwd {
  NullPins& pins;std::map<unsigned,uint32_t> core;uint32_t dr=0,state=0x30003,pc=kRamBreakpoint;
  unsigned launches=0,halts=0;uint32_t extraState=0;bool noReady=false,noHalt=false,badStart=false;
  explicit Core(NullPins& p):ArmSwd(p),pins(p){}
  Error read32(uint32_t a,uint32_t& v) override {assert(a==0xe000edf0||a==0xe000edf8);v=a==0xe000edf0?state:dr;return Error::Ok;}
  Error write32(uint32_t a,uint32_t v) override {
    if(a==0xe000edf8)dr=v;
    else if(a==0xe000edf4){if(v&0x10000)core[v&31]=dr;else dr=(v&31)==15?pc:core[v&31];if(noReady)state&=~0x10000u;}
    else {assert(a==0xe000edf0);
      if(v==0xa05f0009){++launches;state=(noHalt?0:0x30009)|extraState;if(badStart)return Error::Timeout;}
      else{assert(v==0xa05f000b);++halts;state=0x3000b;}}
    return Error::Ok;
  }
};
int main(){
  {NullPins p;Core core(p);assert(core.runAlgorithm(kRamEntry,RAM_STACK,RAM_MAILBOX,kRamBreakpoint,10)==Error::Ok);
   assert(core.launches==1&&core.core[0]==RAM_MAILBOX&&core.core[13]==RAM_STACK&&core.core[16]==0x1000000);}
  {NullPins p;Core core(p);core.pc=0x08000100;assert(core.runAlgorithm(kRamEntry,RAM_STACK,RAM_MAILBOX,kRamBreakpoint,10)==Error::SwdProtocol&&core.launches==1);}
  {NullPins p;Core core(p);core.pc=kRamBreakpoint+2;assert(core.runAlgorithm(kRamEntry,RAM_STACK,RAM_MAILBOX,kRamBreakpoint,10)==Error::Ok&&core.launches==1);}
  for(uint32_t error:{1u<<19,1u<<25}){NullPins p;Core core(p);core.extraState=error;
    assert(core.runAlgorithm(kRamEntry,RAM_STACK,RAM_MAILBOX,kRamBreakpoint,10)==Error::SwdProtocol&&core.launches==1&&core.halts==2);}
  {NullPins p;Core core(p);core.noHalt=true;assert(core.runAlgorithm(kRamEntry,RAM_STACK,RAM_MAILBOX,kRamBreakpoint,10)==Error::Timeout&&core.launches==1&&core.halts==2&&p.time==10);}
  {NullPins p;Core core(p);core.noReady=true;assert(core.runAlgorithm(kRamEntry,RAM_STACK,RAM_MAILBOX,kRamBreakpoint,10)==Error::Timeout&&!core.launches&&p.time==100);}
  {NullPins p;Core core(p);core.badStart=true;assert(core.runAlgorithm(kRamEntry,RAM_STACK,RAM_MAILBOX,kRamBreakpoint,10)==Error::Timeout&&core.launches==1&&core.halts==2);}
  {NullPins p;Core core(p);assert(core.runAlgorithm(0x08000000,RAM_STACK,RAM_MAILBOX,kRamBreakpoint,10)==Error::Invalid&&!core.launches);}
  {RamMemory m;F407Swd f(m,Target::F407VE);assert(f.ramCheck()==Error::Ok&&f.usedRam&&m.calls==2&&!m.erases&&!m.programs&&!m.keys&&f.start()==Error::Invalid);}
  for(size_t n:{9u,1023u,1024u,1025u,16385u,131073u,524288u}){
    RamMemory m;auto bytes=image(n);F407Swd f(m,Target::F407VE);
    assert(f.program(bytes.data(),bytes.size())==Error::Ok&&f.usedRam&&m.calls==1+(n+1023)/1024);
    assert(m.programs==(n+3)/4&&std::equal(bytes.begin(),bytes.end(),m.flash.begin())&&m.regs[0x40023c10]==0x80000000);
    if(n&3)for(unsigned i=0;i<4-(n&3);++i)assert(m.flash[n+i]==255);
    assert(f.start()==Error::Ok&&f.start()==Error::Invalid&&m.starts==1);
  }
  for(unsigned failure=0;failure<6;++failure){
    RamMemory m;auto bytes=image(1027);F407Swd f(m,Target::F407VE);
    m.corruptCode=failure==0;m.stale=failure==1;m.failProbe=failure==2;m.corruptBuffer=failure==3;m.timeout=failure==4;m.flashError=failure==5;
    assert(f.program(bytes.data(),bytes.size())!=Error::Ok&&f.start()==Error::Invalid&&!m.starts);
    if(failure<3)assert(!m.erases&&!m.keys);else assert(m.erases==1);
    assert(m.programs==(failure==5?1u:0u)); // No repeat after ambiguous execution.
  }
  {RamMemory m;auto bytes=image(1027);F407Swd f(m,Target::F407VE);assert(f.program(bytes.data(),bytes.size(),nullptr,true)==Error::Ok&&!f.usedRam&&!m.calls&&m.programs==257);}
  // Exercise the actual C algorithm's validation and watchdog-bounded busy loop.
  {RamMemory m;active=&m;RamMailbox box={RAM_MAGIC,RAM_PROGRAM,kBase,4,RAM_DATA,0,7,0,0,0,0,0,kBase+512*1024};
   box.crc=crc32(m.ram.data()+RAM_DATA-RAM_CODE,4);m.regs[0x40023c10]=0x201;
   m.forceBusy=true;ram_work(&box);assert(box.result==RAM_BUSY_TIMEOUT&&!m.programs);
   m.forceBusy=false;box.address=kBase+512*1024;ram_work(&box);assert(box.result==RAM_ARGUMENT&&!m.programs);
   box.address=kBase;box.buffer=0;ram_work(&box);assert(box.result==RAM_ARGUMENT);
   box.buffer=RAM_DATA;box.length=1028;ram_work(&box);assert(box.result==RAM_ARGUMENT);
   box.length=4;box.crc^=1;ram_work(&box);assert(box.result==RAM_CRC&&!m.programs);
   box.crc^=1;m.regs[0x40023c10]=0x80000000;ram_work(&box);assert(box.result==RAM_CONFIG&&!m.programs);
   m.regs[0xe0042000]=0x410;ram_work(&box);assert(box.result==RAM_IDENTITY&&!m.programs);
  }
  std::cout<<"PASS: SRAM algorithm CRC/ranges/nonce/status/timeouts, pre-erase probe, core invocation, all size boundaries and manual legacy path; no ambiguous execution replay.\n";
}
