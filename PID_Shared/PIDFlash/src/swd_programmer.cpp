#include "swd_programmer.h"
#include "f407_loader_image.h"
#include "../ram_loader/f407_loader.h"
#include <cstring>
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
Error ArmSwd::writeBlock(uint32_t address,const uint32_t* values,size_t words) {
  if(!values||!words||(address&3)||words>(UINT32_MAX-address)/4u+1u)return Error::Invalid;
  for(size_t offset=0;offset<words;) {
    const uint32_t current=address+uint32_t(offset*4);
    size_t count=(1024u-(current&1023u))/4u;if(count>words-offset)count=words-offset;
    Error e=put(true,4,current);if(e!=Error::Ok)return e;
    for(size_t i=0;i<count;++i){e=put(true,12,values[offset+i]);if(e!=Error::Ok)return e;}
    // Drain posted AP writes before observing sticky bus errors or changing TAR.
    uint32_t ignored=0;e=transfer(false,true,12,ignored);if(e!=Error::Ok)return e;
    e=status();if(e!=Error::Ok)return e;offset+=count;
  }
  return Error::Ok;
}
Error ArmSwd::coreRegister(uint8_t index,uint32_t& value,bool write) {
  Error e=Error::Ok;
  if(write)e=write32(0xe000edf8,value);
  if(e==Error::Ok)e=write32(0xe000edf4,uint32_t(index)|(write?0x10000u:0));
  if(e!=Error::Ok)return e;
  const uint32_t start=now();
  for(;;) {
    uint32_t status=0;e=read32(kDhcsr,status);if(e!=Error::Ok)return e;
    if(!(status&(1u<<17)))return Error::SwdProtocol;
    if(status&(1u<<16))break;
    if(uint32_t(now()-start)>=100)return Error::Timeout;
    wait(1);
  }
  return write?Error::Ok:read32(0xe000edf8,value);
}
Error ArmSwd::runAlgorithm(uint32_t entry,uint32_t stack,uint32_t argument,uint32_t breakpoint,uint32_t timeout) {
  // Fixed reserved main-SRAM layout; never execute a supplied Flash address.
  if(entry!=RAM_CODE||stack!=RAM_STACK||argument!=RAM_MAILBOX||
      breakpoint<RAM_CODE||breakpoint>=RAM_CODE+2048||!timeout||timeout>5000)return Error::Invalid;
  uint32_t status=0;Error e=read32(kDhcsr,status);if(e!=Error::Ok)return e;
  if(!(status&(1u<<17)))return Error::Busy;
  e=write32(kDhcsr,0xa05f000b);if(e!=Error::Ok)return e; // Mask interrupts while halted.
  const uint8_t registers[]={0,13,14,15,16};
  uint32_t values[]={argument,stack,entry|1u,entry|1u,1u<<24};
  for(size_t i=0;i<5;++i){e=coreRegister(registers[i],values[i],true);if(e!=Error::Ok)return e;}
  // Clear stale reset status before starting. Never replay after uncertain ACK.
  e=read32(kDhcsr,status);if(e!=Error::Ok)return e;
  e=write32(kDhcsr,0xa05f0009);
  if(e!=Error::Ok){write32(kDhcsr,0xa05f000b);return e;}
  const uint32_t started=now();
  for(;;) {
    e=read32(kDhcsr,status);if(e!=Error::Ok)break;
    if(status&((1u<<19)|(1u<<25))){e=Error::SwdProtocol;break;}
    if(status&(1u<<17)) {
      uint32_t pc=0;e=coreRegister(15,pc,false);if(e!=Error::Ok)break;
      if((pc&~1u)!=breakpoint&&(pc&~1u)!=breakpoint+2)e=Error::SwdProtocol;
      return e;
    }
    if(uint32_t(now()-started)>=timeout){e=Error::Timeout;break;}
    wait(1);
  }
  write32(kDhcsr,0xa05f000b); // Best effort halt; caller must not start unverified code.
  return e;
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
  startStep=1;startResetSeen=false;
  startBeforePc=startBeforeSpecial=startPc=startSp=startSpecial=startDhcsr=0;
  uint32_t sp=0,pc=0;
  Error e=read32(kBase,sp);if(e!=Error::Ok)return e;
  e=read32(kBase+4,pc);if(e!=Error::Ok)return e;
  if(sp<=0x20000000u||sp>0x20020000u||(sp&7)||!(pc&1)||
      (pc&~1u)<kBase+8||(pc&~1u)>=kBase+1024*1024)return Error::Start;
  e=read32(kDhcsr,startDhcsr);if(e!=Error::Ok)return e;
  if(!(startDhcsr&(1u<<17)))return Error::Start;
  e=coreRegister(15,startBeforePc,false);if(e!=Error::Ok)return e;
  e=coreRegister(20,startBeforeSpecial,false);if(e!=Error::Ok)return e;
  // The SRAM loader leaves PC in SRAM and PRIMASK=1. Merely unhalting it
  // executes the loader's WFI loop, while S_HALT=0 falsely looks like RUN.
  // Require an observed system reset and validate the reset-vector context.
  startStep=2;pins_.reset(false);
  e=write32(kDhcsr,0xa05f0003);if(e!=Error::Ok)return e; // Clear C_MASKINTS while halted.
  e=write32(kDemcr,1);if(e!=Error::Ok)return e; // Catch reset before the first app instruction.
  e=read32(kDhcsr,startDhcsr);if(e!=Error::Ok)return e; // Clear old sticky reset indication.
  startStep=3;
  e=write32(0xe000ed0c,0x05fa0004);if(e!=Error::Ok)return e; // One SYSRESETREQ; never replay.
  const uint32_t began=now();
  for(;;) {
    e=read32(kDhcsr,startDhcsr);if(e!=Error::Ok)return e;
    if(startDhcsr&(1u<<25))startResetSeen=true;
    if(startDhcsr&(1u<<19))return Error::Start;
    if(startResetSeen&&(startDhcsr&(1u<<17))&&!(startDhcsr&(1u<<25)))break;
    if(uint32_t(now()-began)>=1000)return Error::Timeout;
    wait(1);
  }
  startStep=4;
  uint32_t xpsr=0;
  e=coreRegister(15,startPc,false);if(e!=Error::Ok)return e;
  e=coreRegister(13,startSp,false);if(e!=Error::Ok)return e;
  e=coreRegister(20,startSpecial,false);if(e!=Error::Ok)return e;
  e=coreRegister(16,xpsr,false);if(e!=Error::Ok)return e;
  if((startPc&~1u)!=(pc&~1u)||startSp!=sp||(startSpecial&0x0701ff01u)||!(xpsr&(1u<<24)))return Error::Start;
  startStep=5;
  e=write32(kDhcsr,0xa05f0003);if(e!=Error::Ok)return e;
  e=write32(kDemcr,0);if(e!=Error::Ok)return e;
  e=write32(kDhcsr,0xa05f0001);if(e!=Error::Ok)return e;
  wait(10);e=read32(kDhcsr,startDhcsr);if(e!=Error::Ok)return e;
  if(startDhcsr&((1u<<17)|(1u<<19)|(1u<<25)))return Error::Start;
  startStep=6;return Error::Ok;
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
Error F407Swd::identify() {
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
  return Error::Ok;
}
Error F407Swd::prepareRam() {
  if(!target_.supportsRam())return Error::Unsupported;
  Error e=target_.writeBlock(RAM_CODE,kRamCode,sizeof(kRamCode)/4);if(e!=Error::Ok)return e;
  uint32_t check[64];
  for(size_t offset=0;offset<sizeof(kRamCode)/4;offset+=64) {
    const size_t n=sizeof(kRamCode)/4-offset<64?sizeof(kRamCode)/4-offset:64;
    e=target_.readBlock(RAM_CODE+uint32_t(offset*4),check,n);if(e!=Error::Ok)return e;
    for(size_t i=0;i<n;++i)if(check[i]!=kRamCode[offset+i])return Error::Verify;
  }
  return callRam(RAM_PROBE,0,nullptr,0);
}
Error F407Swd::callRam(uint32_t command,uint32_t address,const uint32_t* data,uint32_t words) {
  static_assert(sizeof(RamMailbox)==52,"SRAM mailbox ABI");
  if(words>RAM_BLOCK/4||((command==RAM_PROGRAM||command==RAM_CHECK)&&(!data||!words)))return Error::Invalid;
  if(words){const Error e=target_.writeBlock(RAM_DATA,data,words);if(e!=Error::Ok)return e;}
  if(!++nonce_)++nonce_;
  RamMailbox box={RAM_MAGIC,command,address,words*4,RAM_DATA,
    words?crc32(reinterpret_cast<const uint8_t*>(data),words*4):0,nonce_,0,0,0,0,0,kBase+uint32_t(flashKb)*1024u};
  uint32_t mailbox[sizeof(box)/4];std::memcpy(mailbox,&box,sizeof(box));
  Error e=target_.writeBlock(RAM_MAILBOX,mailbox,sizeof(box)/4);
  if(e!=Error::Ok)return e;
  e=target_.runAlgorithm(kRamEntry,RAM_STACK,RAM_MAILBOX,kRamBreakpoint,2000);if(e!=Error::Ok)return e;
  e=target_.readBlock(RAM_MAILBOX,mailbox,sizeof(box)/4);if(e!=Error::Ok)return e;
  std::memcpy(&box,mailbox,sizeof(box));
  ramResult=box.result;ramStatus=box.flash_status;
  if(box.magic!=RAM_MAGIC||box.command!=command||box.address!=address||box.length!=words*4||box.buffer!=RAM_DATA||
      box.nonce!=nonce_||box.returned_nonce!=(nonce_^RAM_NONCE_XOR)||box.state!=RAM_DONE||
      box.flash_limit!=kBase+uint32_t(flashKb)*1024u)return Error::SwdProtocol;
  if(box.result==RAM_BUSY_TIMEOUT)return Error::Timeout;
  if(box.result==RAM_CRC)return Error::Verify;
  if(box.result)return Error::FlashStatus;
  return box.processed==(command==RAM_PROGRAM?words*4:0)?Error::Ok:Error::Incomplete;
}
Error F407Swd::ramCheck() {
  verified_=false;usedRam=false;ramResult=ramStatus=0;
  if(!isF407(selected_))return Error::Invalid;
  Error e=identify();if(e!=Error::Ok)return e;
  // Exercise TAR wrap with three chunks crossing multiple 1 KiB boundaries.
  uint32_t data[128],readback[128];
  for(unsigned chunk=0;chunk<3;++chunk) {
    for(unsigned i=0;i<128;++i)data[i]=(0x5a69c30fu^((chunk*128u+i)*0x9e3779b9u));
    const uint32_t address=0x200023fcu+chunk*512;
    e=target_.writeBlock(address,data,128);if(e!=Error::Ok)return e;
    e=target_.readBlock(address,readback,128);if(e!=Error::Ok)return e;
    for(unsigned i=0;i<128;++i)if(data[i]!=readback[i])return Error::Verify;
  }
  e=prepareRam();if(e!=Error::Ok)return e;
  for(unsigned i=0;i<128;++i)data[i]=0xa5a50000u+i;
  e=callRam(RAM_CHECK,0,data,128);usedRam=e==Error::Ok;return e;
}
Error F407Swd::execute(const uint8_t* image,uint32_t size,Progress progress) {
  Error e=identify();if(e!=Error::Ok)return e;
  uint32_t value=0;
  // Freeze watchdogs while the core is halted, and invalidate/disable ART caches.
  e=target_.read32(0xe0042008,value);if(e!=Error::Ok)return e;
  e=target_.write32(0xe0042008,value|0x1800);if(e!=Error::Ok)return e;
  e=target_.read32(kFlashAcr,value);if(e!=Error::Ok)return e;
  value&=~0x600u;e=target_.write32(kFlashAcr,value);if(e!=Error::Ok)return e;
  e=target_.write32(kFlashAcr,value|0x1800);if(e!=Error::Ok)return e;
  e=target_.write32(kFlashAcr,value);if(e!=Error::Ok)return e;
  e=ready(1000);if(e!=Error::Ok)return e;
  if(!legacy_&&target_.supportsRam()) {
    // Load, read back and execute a no-write probe BEFORE unlocking/erasing.
    e=prepareRam();if(e!=Error::Ok)return e;
    usedRam=true;
  }
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
  if(usedRam) {
    uint32_t buffer[RAM_BLOCK/4];
    for(uint32_t offset=0;offset<size;offset+=RAM_BLOCK) {
      const uint32_t length=size-offset<RAM_BLOCK?size-offset:RAM_BLOCK;
      for(uint32_t i=0;i<(length+3)/4;++i)buffer[i]=0xffffffff;
      for(uint32_t i=0;i<length;++i)buffer[i/4]=(buffer[i/4]&~(255u<<(8*(i%4))))|(uint32_t(image[offset+i])<<(8*(i%4)));
      e=callRam(RAM_PROGRAM,kBase+offset,buffer,(length+3)/4);if(e!=Error::Ok)return e;
      if(progress)progress(3,offset+length);
    }
  } else for(uint32_t offset=0;offset<size;offset+=4) {
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
Error F407Swd::program(const uint8_t* image,uint32_t size,Progress progress,bool legacy) {
  verified_=unlocked_=false;deviceId=flashKb=0;capacityWord=optionBytes=0;
  usedRam=false;ramResult=ramStatus=nonce_=0;legacy_=legacy;
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
