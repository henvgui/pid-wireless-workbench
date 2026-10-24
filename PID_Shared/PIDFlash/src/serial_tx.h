#pragma once
#include "flash_core.h"
#include <cstring>
namespace pidflash {
constexpr size_t kSerialTxChunk=128;
class SerialTxLedger {
 public:
  void reset(uint32_t epoch){epoch_=epoch;total_=0;size_=0;fault_=false;}
  template<class Writer> Error write(uint32_t epoch,uint32_t offset,const uint8_t* data,size_t size,Writer writer) {
    if(epoch!=epoch_)return Error::Stale;
    if(!data||!size||size>kSerialTxChunk)return Error::Invalid;
    if(size_&&offset==offset_)return size==size_&&!std::memcmp(data,last_,size)?lastResult_:Error::Conflict;
    if(fault_)return Error::Conflict;
    if(offset!=total_||size>UINT32_MAX-total_)return Error::Offset;
    const size_t written=writer(data,size);
    if(written==SIZE_MAX)return Error::Busy;
    offset_=offset;size_=size;std::memcpy(last_,data,size);
    if(written>size){fault_=true;return lastResult_=Error::Invalid;}
    total_+=uint32_t(written);
    fault_=written!=size;return lastResult_=fault_?Error::Timeout:Error::Ok;
  }
  uint32_t total() const{return total_;}
  uint32_t epoch() const{return epoch_;}
 private:
  uint32_t epoch_=0,total_=0,offset_=0;
  size_t size_=0;uint8_t last_[kSerialTxChunk]={};
  bool fault_=false;Error lastResult_=Error::Ok;
};
inline bool serialOp(Op op){return op>=Op::SerialOpen&&op<=Op::SerialClose;}
}
