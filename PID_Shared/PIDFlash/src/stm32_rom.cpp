#include "stm32_rom.h"
#include <cstring>
namespace pidflash {
Error Stm32Rom::ack(uint32_t timeout) {
  const int b=io_.read(timeout);return b==0x79?Error::Ok:b==0x1f?Error::Nack:Error::Timeout;
}
Error Stm32Rom::command(uint8_t v) {
  const uint8_t b[]={v,uint8_t(v^0xff)};
  return io_.write(b,2)?ack():Error::Timeout;
}
Error Stm32Rom::address(uint32_t v) {
  uint8_t b[]={uint8_t(v>>24),uint8_t(v>>16),uint8_t(v>>8),uint8_t(v),0};
  b[4]=b[0]^b[1]^b[2]^b[3];return io_.write(b,5)?ack():Error::Timeout;
}
Error Stm32Rom::readMemory(uint32_t addr,uint8_t* bytes,size_t n) {
  if(!n||n>256)return Error::Invalid;
  Error e=command(0x11);if(e!=Error::Ok)return e;
  e=address(addr);if(e!=Error::Ok)return e;
  const uint8_t count[]={uint8_t(n-1),uint8_t((n-1)^0xff)};
  if(!io_.write(count,2))return Error::Timeout;
  e=ack();if(e!=Error::Ok)return e;
  for(size_t i=0;i<n;++i) { const int b=io_.read(1500);if(b<0)return Error::Timeout;bytes[i]=uint8_t(b); }
  return Error::Ok;
}
Error Stm32Rom::probe(uint32_t expectedKb,const TargetProfile& target) {
  const uint8_t sync=0x7f;if(!io_.write(&sync,1))return Error::Timeout;
  Error e=ack();if(e!=Error::Ok)return e;
  e=command(0x00);if(e!=Error::Ok)return e;
  const int count=io_.read(1500);if(count<1||count>64)return Error::Unsupported;
  const int v=io_.read(1500);if(v<0)return Error::Timeout;version=uint8_t(v);
  bool read=false,write=false,legacyErase=false,extendedErase=false,id=false;
  for(int i=0;i<count;++i) {
    const int b=io_.read(1500);if(b<0)return Error::Timeout;
    read|=b==0x11;write|=b==0x31;legacyErase|=b==0x43;extendedErase|=b==0x44;id|=b==0x02;goSupported_|=b==0x21;
  }
  e=ack();if(e!=Error::Ok)return e;
  eraseCommand_=target.target==Target::F103C8&&legacyErase?0x43:extendedErase?0x44:0;
  if(!read||!write||!eraseCommand_||!id)return Error::Unsupported;
  e=command(0x02);if(e!=Error::Ok)return e;
  if(io_.read(1500)!=1)return Error::Device;
  const int hi=io_.read(1500),lo=io_.read(1500);if(hi<0||lo<0)return Error::Timeout;
  deviceId=uint16_t((hi<<8)|lo);e=ack();if(e!=Error::Ok)return e;
  // Check the family and reported capacity before erase. Some C8 boards report
  // 128 KiB; their selected image/erase range must still remain inside 64 KiB.
  if(deviceId!=target.deviceId)return Error::Device;
  uint8_t kb[2];e=readMemory(target.capacityAddress,kb,2);if(e!=Error::Ok)return e;
  flashKb=uint16_t(uint32_t(kb[0])|(uint32_t(kb[1])<<8));
  const bool c8LargerReport=target.target==Target::F103C8&&expectedKb==64&&flashKb==128;
  return flashKb==expectedKb||c8LargerReport?Error::Ok:Error::Capacity;
}
Error Stm32Rom::program(const uint8_t* image,uint32_t size,uint32_t kb,Progress progress,Target selected) {
  goSupported_=false;
  verified_=false;
  deviceId=0;flashKb=0;version=0;
  const TargetProfile* target=profile(selected);
  if(!target||!capacityValid(selected,kb)||size>kb*1024||!vectorsValid(image,size,selected))return Error::Invalid;
  if(progress)progress(1,0);
  Error e=probe(kb,*target);if(e!=Error::Ok)return e;
  if(progress)progress(2,0);
  e=command(eraseCommand_);if(e!=Error::Ok)return e;
  const uint32_t pages=eraseUnits(selected,size);
  if(!pages||pages>64)return Error::Invalid;
  uint8_t erase[131];size_t len=0;
  if(eraseCommand_==0x44)erase[len++]=uint8_t((pages-1)>>8);
  erase[len++]=uint8_t(pages-1);
  for(uint32_t page=0;page<pages;++page){
    if(eraseCommand_==0x44)erase[len++]=uint8_t(page>>8);
    erase[len++]=uint8_t(page);
  }
  uint8_t sum=0;for(size_t i=0;i<len;++i)sum^=erase[i];erase[len++]=sum;
  if(!io_.write(erase,len))return Error::Timeout;
  e=ack(target->eraseTimeoutMs);if(e!=Error::Ok)return e;
  for(uint32_t offset=0;offset<size;offset+=256) {
    const uint32_t amount=size-offset<256?size-offset:256;
    const uint32_t alignment=target->writeAlignment;
    const uint32_t padded=(amount+alignment-1)&~(alignment-1);
    e=command(0x31);if(e!=Error::Ok)return e;
    e=address(kBase+offset);if(e!=Error::Ok)return e;
    uint8_t block[258];block[0]=uint8_t(padded-1);sum=block[0];
    for(uint32_t i=0;i<padded;++i){block[i+1]=i<amount?image[offset+i]:0xff;sum^=block[i+1];}
    block[padded+1]=sum;if(!io_.write(block,padded+2))return Error::Timeout;
    // Ambiguous UART ACK: fail and require a fresh erase; never blindly write twice.
    e=ack(5000);if(e!=Error::Ok)return e;
    if(progress)progress(3,offset+amount);
  }
  for(uint32_t offset=0;offset<size;offset+=256) {
    const uint32_t n=size-offset<256?size-offset:256;uint8_t block[256];
    e=readMemory(kBase+offset,block,n);if(e!=Error::Ok)return e;
    if(std::memcmp(block,image+offset,n))return Error::Verify;
    if(progress)progress(4,offset+n);
  }
  verified_=true;return Error::Ok;
}
Error Stm32Rom::go() {
  if(!verified_)return Error::Invalid;
  if(!goSupported_)return Error::Unsupported;
  verified_=false; // An ambiguous GO acknowledgement must not trigger blind replay.
  Error e=command(0x21);return e==Error::Ok?address(kBase):e;
}
} // namespace pidflash
