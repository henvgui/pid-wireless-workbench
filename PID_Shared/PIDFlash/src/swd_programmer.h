#pragma once
#include "stm32_rom.h"

namespace pidflash {
// SWDIO changes direction at each turnaround. The ESP32 adapter must also
// switch the external translator direction; an input GPIO alone is not enough.
class SwdPins {
 public:
  virtual ~SwdPins()=default;
  virtual void output(bool toTarget)=0;
  virtual void data(bool high)=0;
  virtual bool data()=0;
  virtual void clock(bool high)=0;
  virtual void halfPeriod()=0;
  virtual void reset(bool asserted)=0;
  virtual uint32_t now()=0;
  virtual void wait(uint32_t ms)=0;
};
class SwdTarget {
 public:
  virtual ~SwdTarget()=default;
  virtual Error connect()=0;
  virtual Error read32(uint32_t address,uint32_t& value)=0;
  virtual Error readBlock(uint32_t address,uint32_t* values,size_t words) {
    if(!values||!words||(address&3)||words>(UINT32_MAX-address)/4u+1u)return Error::Invalid;
    for(size_t i=0;i<words;++i){const Error e=read32(address+uint32_t(i*4),values[i]);if(e!=Error::Ok)return e;}
    return Error::Ok;
  }
  virtual Error read16(uint32_t address,uint16_t& value) {
    if(address&1)return Error::Invalid;
    uint32_t word=0;const Error e=read32(address&~3u,word);
    if(e==Error::Ok)value=uint16_t(word>>((address&2)*8));return e;
  }
  virtual Error write32(uint32_t address,uint32_t value)=0;
  virtual Error start()=0;
  virtual uint32_t now()=0;
  virtual void wait(uint32_t ms)=0;
};
class ArmSwd:public SwdTarget {
 public:
  explicit ArmSwd(SwdPins& pins):pins_(pins) {}
  Error transfer(bool ap,bool read,uint8_t address,uint32_t& value);
  Error connect() override;
  Error read32(uint32_t address,uint32_t& value) override;
  Error readBlock(uint32_t address,uint32_t* values,size_t words) override;
  Error read16(uint32_t address,uint16_t& value) override;
  Error write32(uint32_t address,uint32_t value) override;
  Error start() override;
  uint32_t now() override { return pins_.now(); }
  void wait(uint32_t ms) override { pins_.wait(ms); }
  uint32_t debugPortId=0;
  uint8_t lastAck=0,lastRequest=0,connectStep=0;
 private:
  void bit(bool value);
  void bits(uint32_t value,unsigned count);
  uint32_t readBits(unsigned count);
  void turnaround();
  void lineReset();
  Error put(bool ap,uint8_t address,uint32_t value);
  Error posted(uint8_t address,uint32_t& value);
  Error status();
  SwdPins& pins_;
};
class F407Swd {
 public:
  explicit F407Swd(SwdTarget& target,Target selected=Target::F407VG):target_(target),selected_(selected) {}
  Error program(const uint8_t* image,uint32_t size,Progress progress=nullptr);
  Error start();
  // Explicit recovery only: RDP=0xff -> 0xaa causes full user-Flash erasure.
  // Never called by program(); accepts only the confirmed F407VE profile.
  Error removeReadProtection();
  uint16_t deviceId=0,flashKb=0;
  uint32_t capacityWord=0,optionBytes=0,optionBytesAfter=0;
 private:
  Error ready(uint32_t timeout);
  Error execute(const uint8_t* image,uint32_t size,Progress progress);
  SwdTarget& target_;
  Target selected_;
  bool verified_=false,unlocked_=false;
};
} // namespace pidflash
