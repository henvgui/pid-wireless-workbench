#pragma once
#include "flash_core.h"
namespace pidflash {
class RomIo {
 public:
  virtual ~RomIo()=default;
  virtual bool write(const uint8_t* bytes,size_t size)=0;
  virtual int read(uint32_t timeoutMs)=0;
};
using Progress=void (*)(uint8_t stage,uint32_t done);
class Stm32Rom {
 public:
  explicit Stm32Rom(RomIo& io):io_(io) {}
  Error program(const uint8_t* image,uint32_t size,uint32_t expectedKb,Progress progress=nullptr,Target target=Target::G431);
  Error go();
  uint16_t deviceId=0;
  uint16_t flashKb=0;
  uint8_t version=0;
 private:
  Error ack(uint32_t timeout=1500);
  Error command(uint8_t value);
  Error address(uint32_t value);
  Error readMemory(uint32_t address,uint8_t* bytes,size_t size);
  Error probe(uint32_t expectedKb,const TargetProfile& target);
  RomIo& io_;
  uint8_t eraseCommand_=0;
  bool goSupported_=false;
  bool verified_=false;
};
} // namespace pidflash
