#pragma once
#include "flash_core.h"
#include <cstring>

namespace pidflash {
struct LogConfig {
  uint32_t baud=115200;
  uint8_t bits=8,parity=0,stop=1; // parity: 0 none, 1 odd, 2 even.
  bool valid() const { return baud>=1200&&baud<=921600&&bits>=5&&bits<=8&&parity<=2&&(stop==1||stop==2); }
  bool operator==(const LogConfig& other) const {
    return baud==other.baud&&bits==other.bits&&parity==other.parity&&stop==other.stop;
  }
};
// Returned bytes remain available until the next cursor acknowledges them.
// Lost radio/USB replies therefore do not silently consume received UART data.
template<size_t Capacity> class LogBuffer {
 public:
  void reset() { first_=end_=dropped_=0; }
  void append(uint8_t value) {
    if(uint32_t(end_-first_)==Capacity){++first_;++dropped_;}
    bytes_[end_%Capacity]=value;++end_;
  }
  Error read(uint32_t cursor,uint8_t* out,size_t limit,uint32_t& actual,size_t& size) {
    if(int32_t(cursor-end_)>0)return Error::Offset;
    actual=int32_t(cursor-first_)<0?first_:cursor;
    first_=actual;
    size=uint32_t(end_-actual);if(size>limit)size=limit;
    for(size_t i=0;i<size;++i)out[i]=bytes_[(actual+uint32_t(i))%Capacity];
    return Error::Ok;
  }
  uint32_t dropped() const { return dropped_; }
  uint32_t end() const { return end_; }
 private:
  static_assert(Capacity>0&&Capacity<0x80000000u,"bounded log ring");
  uint8_t bytes_[Capacity]={};
  uint32_t first_=0,end_=0,dropped_=0;
};
inline bool logOp(Op op) { return op==Op::LogOpen||op==Op::LogRead||op==Op::LogClose; }
} // namespace pidflash
