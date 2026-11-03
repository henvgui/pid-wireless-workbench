#include "swd_programmer.h"
#include <algorithm>
#include <cassert>
#include <deque>
#include <iostream>
#include <map>
#include <vector>
using namespace pidflash;
namespace {
unsigned parity(uint32_t v){unsigned p=0;for(unsigned i=0;i<32;++i)p^=(v>>i)&1;return p;}
struct Pins:SwdPins {
  bool toTarget=false,level=false,clockLevel=false;uint32_t time=0;
  std::vector<bool> driven;std::deque<bool> responses;unsigned resets=0;
  void output(bool output) override {toTarget=output;}
  void data(bool value) override {level=value;}
  bool data() override {assert(!clockLevel&&!responses.empty());bool value=responses.front();responses.pop_front();return value;}
  void clock(bool value) override {clockLevel=value;if(value&&toTarget)driven.push_back(level);}
  void halfPeriod() override {}
  void reset(bool) override {++resets;}
  uint32_t now() override {return time;}
  void wait(uint32_t ms) override {time+=ms;}
  void bits(uint32_t value,unsigned n){for(unsigned i=0;i<n;++i)responses.push_back((value>>i)&1);}
  void ack(unsigned value=1){bits(value,3);}
  void reply(uint32_t value,bool bad=false){ack();bits(value,32);bits(parity(value)^unsigned(bad),1);}
  uint32_t sent(size_t offset,unsigned n){uint32_t value=0;for(unsigned i=0;i<n;++i)value|=uint32_t(driven.at(offset+i))<<i;return value;}
};
struct Memory:SwdTarget {
  std::vector<uint8_t> flash=std::vector<uint8_t>(1024*1024,0xaa);
  std::map<uint32_t,uint32_t> registers;
  std::vector<unsigned> erased;
  unsigned writes=0,starts=0,keys=0,optionKeys=0,optionStarts=0;uint32_t time=0;
  bool optionBusy=false,loseOptionAck=false,loseOptionPersistence=false;
  bool connected=false,corrupt=false,loseWriteAck=false,eraseError=false,eraseBusy=false,busy=false;
  Memory(){registers[0xe0042000]=0x10000413;registers[0x1fff7a20]=1024u<<16;registers[0x40023c10]=0x80000000;registers[0x40023c00]=0x605;registers[0x40023c14]=0x0fffaaed;}
  Error connect() override {connected=true;if(optionStarts&&loseOptionPersistence)registers[0x40023c14]=0x0fffffed;return Error::Ok;}
  Error read32(uint32_t address,uint32_t& value) override {
    assert(connected);
    if(address>=kBase&&address<kBase+flash.size()) {
      const uint32_t offset=address-kBase;assert(!(offset&3));value=get32(flash.data()+offset)^(corrupt?1:0);
    } else value=address==0x40023c0c&&busy?1u<<16:registers[address];
    return Error::Ok;
  }
  Error write32(uint32_t address,uint32_t value) override {
    assert(connected);
    if(address>=kBase&&address<kBase+flash.size()) {
      assert((registers[0x40023c10]&0x80000303)==0x201);
      const uint32_t offset=address-kBase;assert(get32(flash.data()+offset)==0xffffffff);
      put32(flash.data()+offset,value);++writes;
      return loseWriteAck?Error::Timeout:Error::Ok;
    }
    if(address==0x40023c08) {
      assert(value==(optionKeys?0x4c5d6e7fu:0x08192a3bu));
      if(++optionKeys==2)registers[0x40023c14]&=~1u;
    } else if(address==0x40023c14) {
      assert(!(registers[address]&1));
      registers[address]=value&~2u;
      if(value&2) {
        ++optionStarts;assert(((value>>8)&0xff)==0xaa);
        std::fill(flash.begin(),flash.end(),0xff);busy=optionBusy;
        if(loseOptionAck)return Error::Timeout;
      }
    } else if(address==0x40023c04) {
      assert(value==(keys?0xcdef89ab:0x45670123));
      if(++keys==2)registers[0x40023c10]&=~0x80000000u;
    } else if(address==0x40023c0c)registers[address]&=~value;
    else {
      registers[address]=value;
      if(address==0x40023c10&&(value&(1u<<16))) {
        assert(!(value&0x80000000)&&(value&0x303)==0x202);
        const unsigned sector=(value>>3)&15;
        static const unsigned bounds[]={0,16384,32768,49152,65536,131072,262144,393216,524288,655360,786432,917504,1048576};
        assert(sector<12);erased.push_back(sector);
        std::fill(flash.begin()+bounds[sector],flash.begin()+bounds[sector+1],0xff);
        registers[address]&=~(1u<<16);
        if(eraseError)registers[0x40023c0c]=0x10;
        busy=eraseBusy;
      }
    }
    return Error::Ok;
  }
  Error start() override {++starts;return Error::Ok;}
  uint32_t now() override {return time;}
  void wait(uint32_t ms) override {time+=ms;}
};
std::vector<uint8_t> firmware(size_t size) {
  std::vector<uint8_t> bytes(size);for(size_t i=0;i<size;++i)bytes[i]=uint8_t(i*37);
  put32(bytes.data(),0x20020000);put32(bytes.data()+4,kBase+9);return bytes;
}
}
int main() {
  {Pins pins;for(unsigned i=0;i<3;++i){pins.ack();pins.ack();pins.reply(0);}
   pins.ack();pins.reply(0);pins.reply(0);pins.reply(0);ArmSwd arm(pins);
   assert(arm.start()==Error::Ok&&pins.responses.empty()&&pins.resets==2&&pins.time==10);
   assert(pins.sent(57,32)==0xa05f0003&&pins.sent(171,32)==0&&pins.sent(285,32)==0xa05f0001);}
  {Pins pins;for(unsigned i=0;i<3;++i)pins.ack();pins.reply(0);pins.reply(0);
   ArmSwd arm(pins);uint32_t words[]={123,456};assert(arm.writeBlock(0x20000000,words,2)==Error::Ok);
   assert(pins.responses.empty()&&pins.sent(8,32)==0x20000000&&pins.sent(57,32)==123&&pins.sent(106,32)==456);}
  {Pins pins;for(unsigned i=0;i<2;++i){pins.ack();pins.ack();pins.reply(0);pins.reply(0);}
   ArmSwd arm(pins);uint32_t words[]={123,456};assert(arm.writeBlock(0x200003fc,words,2)==Error::Ok);
   assert(pins.responses.empty()&&pins.sent(8,32)==0x200003fc&&pins.sent(138,32)==0x20000400);}
  {Pins pins;pins.ack();pins.ack();pins.reply(0);pins.reply(0xa2);ArmSwd arm(pins);uint32_t word=0;
   assert(arm.writeBlock(0x20000000,&word,1)==Error::SwdFault&&pins.responses.empty());}
  {Pins pins;pins.ack();pins.ack(4);ArmSwd arm(pins);uint32_t words[]={123,456};
   assert(arm.writeBlock(0x20000000,words,2)==Error::SwdFault&&pins.responses.empty());}
  {Pins pins;ArmSwd arm(pins);uint32_t value=0;
   assert(arm.writeBlock(1,&value,1)==Error::Invalid&&arm.writeBlock(0,&value,0)==Error::Invalid);
   assert(arm.writeBlock(0,nullptr,1)==Error::Invalid&&arm.writeBlock(0xfffffffc,&value,2)==Error::Invalid&&pins.driven.empty());}
  {Pins pins;pins.ack();pins.reply(0xdeadbeef);pins.reply(0x11223344);pins.reply(0x55667788);pins.reply(0);
   ArmSwd arm(pins);uint32_t values[2]={};
   assert(arm.readBlock(0x08000000,values,2)==Error::Ok&&values[0]==0x11223344&&values[1]==0x55667788);
   assert(pins.responses.empty()&&pins.driven.size()==113&&pins.sent(49,8)==0x9f&&pins.sent(65,8)==0x9f&&pins.sent(81,8)==0xbd);}
  {Pins pins;for(uint32_t value:{123u,456u}){pins.ack();pins.reply(0);pins.reply(value);pins.reply(0);}
   ArmSwd arm(pins);uint32_t values[2]={};assert(arm.readBlock(0x080003fc,values,2)==Error::Ok);
   assert(values[0]==123&&values[1]==456&&pins.sent(8,32)==0x080003fc&&pins.sent(105,32)==0x08000400&&pins.responses.empty());}
  {Pins pins;pins.ack();pins.reply(0);pins.reply(123,true);ArmSwd arm(pins);uint32_t values[2]={};
   assert(arm.readBlock(0x08000000,values,2)==Error::SwdParity&&pins.responses.empty());}
  {Pins pins;pins.ack();pins.reply(0);pins.reply(123);pins.reply(0xa2);ArmSwd arm(pins);uint32_t value=0;
   assert(arm.readBlock(0x08000000,&value,1)==Error::SwdFault&&pins.responses.empty());}
  {Pins pins;ArmSwd arm(pins);uint32_t value=0;
   assert(arm.readBlock(1,&value,1)==Error::Invalid&&arm.readBlock(0,&value,0)==Error::Invalid);
   assert(arm.readBlock(0,nullptr,1)==Error::Invalid&&arm.readBlock(0xfffffffc,&value,2)==Error::Invalid&&pins.driven.empty());}
  {Pins pins;pins.ack();pins.ack();pins.reply(0);pins.reply(512u<<16);pins.reply(0);pins.ack();ArmSwd arm(pins);uint16_t value=0;
   assert(arm.read16(0x1fff7a22,value)==Error::Ok&&value==512&&pins.responses.empty());
   assert(pins.sent(8,32)==0x23000051&&pins.sent(57,32)==0x1fff7a22);
   assert(pins.sent(pins.driven.size()-41,32)==0x23000052);}
  {Pins pins;ArmSwd arm(pins);uint16_t value=0;assert(arm.read16(3,value)==Error::Invalid&&pins.driven.empty());}
  {Pins pins;pins.ack();pins.ack();pins.reply(0);pins.reply(512u<<16,true);pins.ack();ArmSwd arm(pins);uint16_t value=0;
   assert(arm.read16(0x1fff7a22,value)==Error::SwdParity&&pins.responses.empty());
   assert(pins.sent(pins.driven.size()-41,32)==0x23000052);}
  {Pins pins;pins.reply(0x2ba01477);ArmSwd arm(pins);uint32_t value=0;
   assert(arm.transfer(false,true,0,value)==Error::Ok&&value==0x2ba01477);
   assert(pins.sent(0,8)==0xa5&&pins.responses.empty());}
  {Pins pins;pins.reply(123,true);ArmSwd arm(pins);uint32_t value=0;assert(arm.transfer(false,true,0,value)==Error::SwdParity);}
  {Pins pins;pins.ack(7);ArmSwd arm(pins);uint32_t value=0;
   assert(arm.transfer(false,true,0,value)==Error::SwdProtocol&&arm.lastAck==7&&arm.lastRequest==0xa5);}
  {Pins pins;pins.reply(0x12345678);ArmSwd arm(pins);
   assert(arm.connect()==Error::SwdProtocol&&arm.debugPortId==0x12345678&&arm.connectStep==2&&pins.time==2);}
  {Pins pins;pins.ack(2);pins.reply(987);ArmSwd arm(pins);uint32_t value=0;
   assert(arm.transfer(false,true,0,value)==Error::Ok&&value==987&&pins.time==1);
   assert(pins.sent(0,8)==0xa5&&pins.sent(16,8)==0xa5);}
  {Pins pins;pins.ack(4);ArmSwd arm(pins);uint32_t value=0;assert(arm.transfer(false,false,4,value)==Error::SwdFault&&pins.driven.size()==16);}
  {Pins pins;pins.ack();ArmSwd arm(pins);uint32_t value=0x12345678;
   assert(arm.transfer(true,false,12,value)==Error::Ok&&pins.sent(0,8)==0xbb&&pins.sent(8,32)==value&&pins.sent(40,1)==parity(value));}
  {Pins pins;pins.ack();pins.reply(0xaaaaaaaa);pins.reply(0x12345678);pins.reply(0);ArmSwd arm(pins);uint32_t value=0;
   assert(arm.read32(0x20000000,value)==Error::Ok&&value==0x12345678);
   assert(pins.sent(0,8)==0x8b&&pins.sent(49,8)==0x9f&&pins.sent(65,8)==0xbd&&pins.responses.empty());}
  for(const auto& boundary:std::vector<std::pair<size_t,size_t>>{{1027,1},{16384,1},{16385,2},{65536,4},{65537,5},{131072,5},{131073,6},{1048576,12}}) {
    auto bytes=firmware(boundary.first);Memory memory;F407Swd programmer(memory);
    assert(programmer.program(bytes.data(),bytes.size())==Error::Ok);
    assert(programmer.deviceId==0x413&&programmer.flashKb==1024&&memory.erased.size()==boundary.second);
    assert(std::equal(bytes.begin(),bytes.end(),memory.flash.begin()));
    assert(memory.registers[0x40023c10]==0x80000000&&memory.registers[0x40023c00]==5);
    assert(programmer.start()==Error::Ok&&programmer.start()==Error::Invalid&&memory.starts==1);
  }
  for(bool identity:{false,true}) {
    auto bytes=firmware(1027);Memory memory;
    memory.registers[identity?0xe0042000:0x1fff7a20]=identity?0x410:512u<<16;F407Swd programmer(memory);
    assert(programmer.program(bytes.data(),bytes.size())==(identity?Error::Device:Error::Capacity));
    assert(memory.erased.empty()&&!memory.keys&&programmer.start()==Error::Invalid);
  }
  {auto bytes=firmware(1027);Memory memory;memory.registers[0x1fff7a20]=0xffffffff;
   F407Swd programmer(memory,Target::F407VE);
   assert(programmer.program(bytes.data(),bytes.size())==Error::Capacity&&programmer.flashKb==65535);
   assert(memory.erased.empty()&&!memory.keys&&programmer.start()==Error::Invalid);}
  for(uint32_t options:{0x0fffffedu,0x0fffccedu,0xffffffffu}) {
   auto bytes=firmware(1027);Memory memory;memory.registers[0x1fff7a20]=0xffffffff;memory.registers[0x40023c14]=options;
   F407Swd programmer(memory,Target::F407VE);assert(programmer.program(bytes.data(),bytes.size())==Error::FlashStatus);
   assert(memory.erased.empty()&&!memory.keys&&programmer.start()==Error::Invalid);}
  {auto bytes=firmware(1027);Memory memory;memory.registers[0x1fff7a20]=0xffffffff;F407Swd programmer(memory);
   assert(programmer.program(bytes.data(),bytes.size())==Error::Capacity&&memory.erased.empty()&&!memory.keys);}
  {auto bytes=firmware(1027);Memory memory;memory.registers[0x1fff7a20]=0xffffffff;memory.corrupt=true;
   F407Swd programmer(memory,Target::F407VE);assert(programmer.program(bytes.data(),bytes.size())==Error::Capacity);
   assert(memory.erased.empty()&&!memory.keys&&programmer.start()==Error::Invalid);}
  {auto bytes=firmware(512*1024);Memory memory;memory.registers[0x1fff7a20]=512u<<16;
   F407Swd programmer(memory,Target::F407VE);
   assert(programmer.program(bytes.data(),bytes.size())==Error::Ok&&programmer.flashKb==512);
   assert(memory.erased.size()==8&&std::equal(bytes.begin(),bytes.end(),memory.flash.begin()));
   assert(std::all_of(memory.flash.begin()+512*1024,memory.flash.end(),[](uint8_t b){return b==0xaa;}));
   assert(programmer.start()==Error::Ok);}
  {Memory memory;memory.registers[0x40023c14]=0x0fffffed;F407Swd programmer(memory,Target::F407VE);
   assert(programmer.removeReadProtection()==Error::Ok&&memory.optionStarts==1&&memory.optionKeys==2);
   assert(programmer.optionBytes==0x0fffffed&&programmer.optionBytesAfter==0x0fffaaed);
   assert(std::all_of(memory.flash.begin(),memory.flash.end(),[](uint8_t b){return b==0xff;}));
   assert(programmer.start()==Error::Invalid&&!memory.starts);}
  for(uint32_t option:{0x0fffaaedu,0x0fffccedu,0xffffffffu}) {
   Memory memory;memory.registers[0x40023c14]=option;F407Swd programmer(memory,Target::F407VE);
   assert(programmer.removeReadProtection()==Error::Invalid&&!memory.optionKeys&&!memory.optionStarts);}
  {Memory memory;memory.registers[0x40023c14]=0x0fffffed;memory.loseOptionPersistence=true;
   F407Swd programmer(memory,Target::F407VE);
   assert(programmer.removeReadProtection()==Error::FlashStatus&&programmer.optionBytesAfter==0x0fffffed&&memory.optionStarts==1);
   assert(programmer.start()==Error::Invalid&&!memory.starts);}
  {Memory memory;F407Swd programmer(memory);assert(programmer.removeReadProtection()==Error::Invalid&&!memory.connected);}
  {Memory memory;memory.registers[0xe0042000]=0x410;F407Swd programmer(memory,Target::F407VE);
   assert(programmer.removeReadProtection()==Error::Device&&!memory.optionKeys);}
  for(bool timeout:{false,true}) {
   Memory memory;memory.registers[0x40023c14]=0x0fffffed;memory.optionBusy=timeout;memory.loseOptionAck=!timeout;
   F407Swd programmer(memory,Target::F407VE);assert(programmer.removeReadProtection()==Error::Timeout&&memory.optionStarts==1);
   assert(programmer.start()==Error::Invalid&&!memory.starts&&(!timeout||memory.time==60000));}
  {auto bytes=firmware(512*1024+1);Memory memory;F407Swd programmer(memory,Target::F407VE);
   assert(programmer.program(bytes.data(),bytes.size())==Error::Invalid&&!memory.connected);}
  {auto bytes=firmware(1027);Memory memory;F407Swd programmer(memory,Target::F407VE);
   assert(programmer.program(bytes.data(),bytes.size())==Error::Capacity&&memory.erased.empty()&&!memory.keys);}
  {auto bytes=firmware(1027);Memory memory;memory.corrupt=true;F407Swd programmer(memory);
   assert(programmer.program(bytes.data(),bytes.size())==Error::Verify&&programmer.start()==Error::Invalid);}
  {auto bytes=firmware(1027);Memory memory;memory.loseWriteAck=true;F407Swd programmer(memory);
   assert(programmer.program(bytes.data(),bytes.size())==Error::Timeout&&memory.writes==1&&programmer.start()==Error::Invalid);}
  {auto bytes=firmware(1027);Memory memory;memory.eraseError=true;F407Swd programmer(memory);
   assert(programmer.program(bytes.data(),bytes.size())==Error::FlashStatus&&!memory.writes);}
  {auto bytes=firmware(1027);Memory memory;memory.eraseBusy=true;F407Swd programmer(memory);
   assert(programmer.program(bytes.data(),bytes.size())==Error::Timeout&&!memory.writes&&memory.time==10000);}
  {auto bytes=firmware(1027);put32(bytes.data(),0x20020008);Memory memory;F407Swd programmer(memory);
   assert(programmer.program(bytes.data(),bytes.size())==Error::Invalid&&!memory.connected);}
  {std::vector<uint8_t> bytes(131073);Staging staging(bytes.data(),131072);uint8_t sha[32]={};
   assert(staging.begin(1,131073,1024,sha,Target::F407VG,Interface::Swd)==Error::NoMemory);
   staging.storage(bytes.data(),bytes.size());assert(staging.begin(1,131073,1024,sha,Target::F407VG,Interface::Swd)==Error::Ok);
   assert(staging.begin(1,131073,1024,sha,Target::F407VG,Interface::Uart)==Error::Conflict);
   assert(staging.begin(2,8,64,sha,Target::F103C8,Interface::Swd)==Error::Invalid);}
  std::cout<<"PASS: SWD request bits/parity/WAIT/FAULT/posted reads; F407 ID/capacity gates, every erase boundary, 1 MiB write/readback, lock/cache handling, ambiguous write stops, no unverified start, and bounded staging storage.\n";
}
