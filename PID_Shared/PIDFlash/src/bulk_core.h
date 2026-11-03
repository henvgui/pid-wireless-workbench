#pragma once
#include "flash_core.h"
namespace pidflash { namespace bulk {
constexpr size_t kWindow=8,kSlots=32,kCache=kSlots*kChunk,kUsbPayload=3072,kUsbRaw=kUsbPayload+20,kUsbEncoded=kUsbRaw+kUsbRaw/254+2;
enum class UsbKind:uint8_t { Data=1,Exit=2,Query=3 };
struct UsbFrame { UsbKind kind;uint32_t transfer,offset;uint16_t length;uint8_t data[kUsbPayload]; };
size_t encodeUsb(const UsbFrame& frame,uint8_t* output,size_t capacity);
class UsbDecoder {
 public:
  bool feed(uint8_t value,UsbFrame& frame,bool& ready,bool& invalid);
  bool collecting() const { return collecting_; }
 private:
  uint8_t bytes_[kUsbEncoded]={};size_t length_=0;bool collecting_=false,overflow_=false;
};
struct Chunk {uint8_t data[kChunk];uint32_t offset=0,sentAt=0;uint16_t length=0;uint8_t attempts=0;bool acknowledged=false,driverQueued=false;};
class Sender {
 public:
  void begin(uint32_t transfer,uint32_t size,uint32_t start);
  Error enqueue(uint32_t offset,const uint8_t* data,size_t n);
  Chunk* next(uint32_t now);
  void sent(Chunk& chunk,uint32_t now);
  void scheduled(Chunk& chunk){chunk.driverQueued=true;}
  void transmitted(uint32_t offset,uint32_t now);
  Error acknowledge(uint32_t received,uint32_t bitmap,uint32_t now);
  uint32_t freeBytes() const { return kCache-(queued-received); }
  uint32_t transfer=0,size=0,received=0,queued=0,retries=0,rto=80;
  Error error=Error::Ok;
 private:
  Chunk chunks_[kSlots];uint32_t srtt_=0,variation_=0;
};
} }
