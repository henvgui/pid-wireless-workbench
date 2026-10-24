#include "swd_programmer.h"
namespace pidflash {
namespace {
constexpr uint32_t kDhcsr=0xe000edf0,kDemcr=0xe000edfc;
constexpr uint32_t kFlashAcr=0x40023c00,kFlashKey=0x40023c04;
constexpr uint32_t kFlashSr=0x40023c0c,kFlashCr=0x40023c10;
constexpr uint32_t kBusy=1u<<16,kErrors=0xf2,kWord=2u<<8;
unsigned parity(uint32_t value) { value^=value>>16;value^=value>>8;value^=value>>4;return (0x6996u>>(value&15))&1; }
}
// Match ARM CMSIS-DAP SW_DP.c: cycles finish high; sample SWDIO during low,
// before the rising edge. Sampling after rising shifts a target's response.
void ArmSwd::bit(bool value) { pins_.data(value);pins_.clock(false);pins_.halfPeriod();pins_.clock(true);pins_.halfPeriod(); }
void ArmSwd::bits(uint32_t value,unsigned count) { for(unsigned i=0;i<count;++i)bit((value>>i)&1); }
uint32_t ArmSwd::readBits(unsigned count) {
  uint32_t value=0;
  for(unsigned i=0;i<count;++i){pins_.clock(false);pins_.halfPeriod();if(pins_.data())value|=1u<<i;pins_.clock(true);pins_.halfPeriod();}
  return value;
}
void ArmSwd::turnaround(){pins_.clock(false);pins_.halfPeriod();pins_.clock(true);pins_.halfPeriod();}
void ArmSwd::lineReset(){pins_.output(true);bits(0xffffffff,32);bits(0xffffffff,32);}
Error ArmSwd::transfer(bool ap,bool read,uint8_t address,uint32_t& value) {
  const uint8_t fields=uint8_t(ap)|(uint8_t(read)<<1)|uint8_t((address&12));
  const uint8_t request=1|(fields<<1)|(parity(fields)<<5)|0x80;
  lastRequest=request;
  for(unsigned attempt=0;attempt<64;++attempt) {
    pins_.output(true);bits(request,8);
    pins_.output(false);turnaround();
    const uint32_t ack=readBits(3);lastAck=uint8_t(ack);
    if(ack!=1) {
      turnaround();pins_.output(true);bits(0,8);
      if(ack==2){pins_.wait(1);continue;}
      return ack==4?Error::SwdFault:Error::SwdProtocol;
    }
    if(read) {
      value=readBits(32);const uint32_t check=readBits(1);
      turnaround();pins_.output(true);bits(0,8);
      return check==parity(value)?Error::Ok:Error::SwdParity;
    }
    turnaround();pins_.output(true);bits(value,32);bits(parity(value),1);bits(0,8);
    return Error::Ok;
  }
  return Error::Timeout;
}
Error ArmSwd::put(bool ap,uint8_t address,uint32_t value){return transfer(ap,false,address,value);}
Error ArmSwd::posted(uint8_t address,uint32_t& value) {
  uint32_t ignored=0;Error e=transfer(true,true,address,ignored);
  return e==Error::Ok?transfer(false,true,12,value):e;
}
Error ArmSwd::status() {
  uint32_t value=0;Error e=transfer(false,true,4,value);
  return e!=Error::Ok?e:(value&0xa2)?Error::SwdFault:Error::Ok;
}
Error ArmSwd::read32(uint32_t address,uint32_t& value) {
  if(address&3)return Error::Invalid;
  Error e=put(true,4,address);if(e!=Error::Ok)return e;
  e=posted(12,value);return e==Error::Ok?status():e;
}
Error ArmSwd::write32(uint32_t address,uint32_t value) {
  if(address&3)return Error::Invalid;
  Error e=put(true,4,address);if(e!=Error::Ok)return e;
  e=put(true,12,value);return e==Error::Ok?status():e;
}
Error ArmSwd::readBlock(uint32_t address,uint32_t* values,size_t words) {
  if(!values||!words||(address&3)||words>(UINT32_MAX-address)/4u+1u)return Error::Invalid;
  size_t offset=0;
  while(offset<words) {
    const uint32_t current=address+uint32_t(offset*4);
    // MEM-AP single increment is already configured by connect(). Reseed TAR
    // conservatively at every 1 KiB boundary, including unaligned block starts.
    size_t count=(1024u-(current&1023u))/4u;
    if(count>words-offset)count=words-offset;
    Error e=put(true,4,current);if(e!=Error::Ok)return e;
    uint32_t ignored=0;e=transfer(true,true,12,ignored);if(e!=Error::Ok)return e;
    for(size_t i=0;i<count;++i) {
      // AP reads are posted: each AP read returns the preceding value. Finish
      // with DP RDBUFF so the last word is drained without another AP access.
      e=transfer(i+1<count,true,12,values[offset+i]);if(e!=Error::Ok)return e;
    }
    e=status();if(e!=Error::Ok)return e;
    offset+=count;
  }
  return Error::Ok;
}
Error ArmSwd::read16(uint32_t address,uint16_t& value) {
  if(address&1)return Error::Invalid;
  Error e=put(true,0,0x23000051); // MEM-AP halfword access.
  if(e==Error::Ok)e=put(true,4,address);
  uint32_t raw=0;if(e==Error::Ok)e=posted(12,raw);
  if(e==Error::Ok)e=status();
  const Error restore=put(true,0,0x23000052); // Restore word accesses before returning.
  if(e!=Error::Ok)return e;if(restore!=Error::Ok)return restore;
  value=uint16_t(raw>>((address&2)*8));return Error::Ok;
}
Error ArmSwd::connect() {
  debugPortId=0;connectStep=1;
  pins_.reset(true);pins_.wait(2);pins_.clock(false);lineReset();bits(0xe79e,16);lineReset();bits(0,8);
  connectStep=2;
  uint32_t id=0;Error e=transfer(false,true,0,id);debugPortId=id;if(e!=Error::Ok)return e;
  if((id&0x0fffffff)!=0x0ba01477)return Error::SwdProtocol;
  connectStep=3;
  e=put(false,0,0x1e);if(e!=Error::Ok)return e; // Clear sticky errors once before any target writes.
  connectStep=4;
  e=put(false,8,0);if(e!=Error::Ok)return e;    // AP0, register bank 0.
  connectStep=5;
  e=put(false,4,0x50000000);if(e!=Error::Ok)return e;
  const uint32_t started=pins_.now();
  for(;;) {
    uint32_t power=0;e=transfer(false,true,4,power);if(e!=Error::Ok)return e;
    if((power&0xa0000000)==0xa0000000)break;
    if(uint32_t(pins_.now()-started)>=1000)return Error::Timeout;
    pins_.wait(1);
  }
  // Word accesses, HPROT privileged/data, single address increment.
  connectStep=6;
  e=put(true,0,0x23000052);if(e!=Error::Ok)return e;
  connectStep=7;
  e=write32(kDemcr,1);if(e!=Error::Ok)return e; // Catch reset before application can remap debug pins.
  connectStep=8;
  e=write32(kDhcsr,0xa05f0003);if(e!=Error::Ok)return e;
  connectStep=9;
  pins_.reset(false);
  const uint32_t released=pins_.now();
  for(;;) {
    uint32_t state=0;e=read32(kDhcsr,state);if(e!=Error::Ok)return e;
    if(state&(1u<<17)){connectStep=10;return Error::Ok;}
    if(uint32_t(pins_.now()-released)>=1000)return Error::Timeout;
    pins_.wait(1);
  }
}
Error ArmSwd::start() {
  pins_.reset(true);
  Error e=write32(kDemcr,0);if(e!=Error::Ok)return e;
  e=write32(kDhcsr,0xa05f0001);if(e!=Error::Ok)return e;
  pins_.reset(false);pins_.wait(10);
  uint32_t state=0;e=read32(kDhcsr,state);if(e!=Error::Ok)return e;
  return state&((1u<<17)|(1u<<19))?Error::Start:Error::Ok;
}
Error F407Swd::ready(uint32_t timeout) {
  const uint32_t started=target_.now();
  for(;;) {
    uint32_t value=0;Error e=target_.read32(kFlashSr,value);if(e!=Error::Ok)return e;
    if(!(value&kBusy))return value&kErrors?Error::FlashStatus:Error::Ok;
    if(uint32_t(target_.now()-started)>=timeout)return Error::Timeout;
    target_.wait(1);
  }
}
Error F407Swd::execute(const uint8_t* image,uint32_t size,Progress progress) {
  Error e=target_.connect();if(e!=Error::Ok)return e;
  uint32_t value=0;e=target_.read32(0xe0042000,value);if(e!=Error::Ok)return e;
  deviceId=uint16_t(value&0xfff);if(deviceId!=0x413)return Error::Device;
  e=target_.read32(0x40023c14,optionBytes);if(e!=Error::Ok)return e;
  if(((optionBytes>>8)&0xff)!=0xaa)return Error::FlashStatus;
  e=target_.read16(0x1fff7a22,flashKb);if(e!=Error::Ok)return e;
  if(flashKb!=profile(selected_)->maxKb) {
    e=target_.read32(0x1fff7a20,capacityWord);if(e!=Error::Ok)return e;
    e=target_.read32(0x40023c14,optionBytes);if(e!=Error::Ok)return e;
    // An invalid factory size can accompany failed reads of all nonvolatile
    // memory. Never erase based on a profile override in that condition.
    return Error::Capacity;
  }
  // Freeze watchdogs while the core is halted, and invalidate/disable ART caches.
  e=target_.read32(0xe0042008,value);if(e!=Error::Ok)return e;
  e=target_.write32(0xe0042008,value|0x1800);if(e!=Error::Ok)return e;
  e=target_.read32(kFlashAcr,value);if(e!=Error::Ok)return e;
  value&=~0x600u;e=target_.write32(kFlashAcr,value);if(e!=Error::Ok)return e;
  e=target_.write32(kFlashAcr,value|0x1800);if(e!=Error::Ok)return e;
  e=target_.write32(kFlashAcr,value);if(e!=Error::Ok)return e;
  e=ready(1000);if(e!=Error::Ok)return e;
  e=target_.read32(kFlashCr,value);if(e!=Error::Ok)return e;
  if(value&0x80000000) {
    e=target_.write32(kFlashKey,0x45670123);if(e!=Error::Ok)return e;
    e=target_.write32(kFlashKey,0xcdef89ab);if(e!=Error::Ok)return e;
    e=target_.read32(kFlashCr,value);if(e!=Error::Ok)return e;
    if(value&0x80000000)return Error::FlashStatus;
  }
  unlocked_=true;e=target_.write32(kFlashSr,0xf3);if(e!=Error::Ok)return e;
  const uint32_t count=eraseUnits(selected_,size);
  if(progress)progress(2,0);
  for(uint32_t sector=0;sector<count;++sector) {
    const uint32_t control=kWord|2|(sector<<3);
    e=target_.write32(kFlashCr,control);if(e!=Error::Ok)return e;
    e=target_.write32(kFlashCr,control|(1u<<16));if(e!=Error::Ok)return e;
    e=ready(10000);if(e!=Error::Ok)return e;
  }
  e=target_.write32(kFlashCr,kWord|1);if(e!=Error::Ok)return e;
  for(uint32_t offset=0;offset<size;offset+=4) {
    uint32_t word=0xffffffff;
    for(uint32_t i=0;i<4&&offset+i<size;++i)word=(word&~(0xffu<<(8*i)))|(uint32_t(image[offset+i])<<(8*i));
    e=target_.write32(kBase+offset,word);if(e!=Error::Ok)return e;
    e=ready(100);if(e!=Error::Ok)return e;
    if(progress&&((offset&255)==252||offset+4>=size))progress(3,offset+4<size?offset+4:size);
  }
  e=target_.write32(kFlashCr,0x80000000);if(e!=Error::Ok)return e;
  unlocked_=false;
  uint32_t block[64];
  for(uint32_t offset=0;offset<size;offset+=sizeof(block)) {
    const uint32_t length=size-offset<sizeof(block)?size-offset:sizeof(block);
    e=target_.readBlock(kBase+offset,block,(length+3)/4);if(e!=Error::Ok)return e;
    for(uint32_t i=0;i<length;++i)if(uint8_t(block[i/4]>>(8*(i%4)))!=image[offset+i])return Error::Verify;
    if(progress)progress(4,offset+length);
  }
  return Error::Ok;
}
Error F407Swd::program(const uint8_t* image,uint32_t size,Progress progress) {
  verified_=unlocked_=false;deviceId=flashKb=0;capacityWord=optionBytes=0;
  if(!isF407(selected_)||!vectorsValid(image,size,selected_))return Error::Invalid;
  if(progress)progress(1,0);
  const Error e=execute(image,size,progress);
  if(unlocked_) { uint32_t state=0;if(target_.read32(kFlashSr,state)==Error::Ok&&!(state&kBusy))target_.write32(kFlashCr,0x80000000); }
  unlocked_=false;verified_=e==Error::Ok;return e;
}
Error F407Swd::start() {
  if(!verified_)return Error::Invalid;
  verified_=false; // Never replay reset/start after an ambiguous response.
  return target_.start();
}
Error F407Swd::removeReadProtection() {
  verified_=false;deviceId=flashKb=0;optionBytes=optionBytesAfter=0;
  if(selected_!=Target::F407VE)return Error::Invalid;
  Error e=target_.connect();if(e!=Error::Ok)return e;
  uint32_t value=0;e=target_.read32(0xe0042000,value);if(e!=Error::Ok)return e;
  deviceId=uint16_t(value&0xfff);if(deviceId!=0x413)return Error::Device;
  e=target_.read32(0x40023c14,optionBytes);if(e!=Error::Ok)return e;
  // Scope this recovery to the repeatedly observed RDP=ff condition. Never
  // raise protection, change BOR/WRP, or attempt irreversible RDP level 2.
  if((optionBytes&0xf0000000u)||((optionBytes>>8)&0xff)!=0xff)return Error::Invalid;
  e=target_.read32(0x40023c14,value);if(e!=Error::Ok)return e;
  if(value!=optionBytes)return Error::Invalid;
  e=ready(1000);if(e!=Error::Ok)return e;
  if(optionBytes&1) {
    e=target_.write32(0x40023c08,0x08192a3b);if(e!=Error::Ok)return e;
    e=target_.write32(0x40023c08,0x4c5d6e7f);if(e!=Error::Ok)return e;
    e=target_.read32(0x40023c14,value);if(e!=Error::Ok)return e;
    if(value!=(optionBytes&~1u))return Error::FlashStatus;
  }
  const uint32_t control=(optionBytes&~0xff03u)|0xaa00u;
  e=target_.write32(0x40023c14,control);if(e!=Error::Ok)return e;
  // Exactly one start, no replay after an ambiguous SWD response or timeout.
  e=target_.write32(0x40023c14,control|2);if(e!=Error::Ok)return e;
  e=ready(60000);if(e!=Error::Ok)return e;
  e=target_.write32(0x40023c14,control|1);if(e!=Error::Ok)return e;
  // A shadow-register readback is insufficient: actual failed devices can
  // report aa here, then reload ff on reset. Confirm the persisted setting.
  e=target_.connect();if(e!=Error::Ok)return e;
  target_.wait(20);
  e=target_.read32(0x40023c14,optionBytesAfter);if(e!=Error::Ok)return e;
  return optionBytesAfter==(control|1)?Error::Ok:Error::FlashStatus;
}
} // namespace pidflash
