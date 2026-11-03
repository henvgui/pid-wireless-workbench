#include "bulk_core.h"
#include <cstring>
#include <algorithm>
namespace pidflash { namespace bulk {
namespace {
bool decodeUsb(const uint8_t* input,size_t size,UsbFrame& f) {
  uint8_t raw[kUsbRaw];size_t read=0,used=0;
  while(read<size){const unsigned code=input[read++];if(!code||read+code-1>size)return false;
    for(unsigned i=1;i<code;++i){if(used==sizeof(raw))return false;raw[used++]=input[read++];}
    if(code!=255&&read<size){if(used==sizeof(raw))return false;raw[used++]=0;}}
  if(used<20||raw[0]!='B'||raw[1]!='U'||raw[2]!=1||raw[3]<1||raw[3]>3||raw[14]||raw[15])return false;
  const uint16_t length=uint16_t(raw[12])|(uint16_t(raw[13])<<8);
  if(length>kUsbPayload||used!=20u+length||get32(raw+16+length)!=crc32(raw,16+length))return false;
  if(raw[3]!=1&&length)return false;
  f.kind=UsbKind(raw[3]);f.transfer=get32(raw+4);f.offset=get32(raw+8);f.length=length;
  std::memcpy(f.data,raw+16,length);return true;
}
}
size_t encodeUsb(const UsbFrame& f,uint8_t* output,size_t capacity) {
  if(!output||capacity<kUsbEncoded+2||f.length>kUsbPayload)return 0;
  uint8_t raw[kUsbRaw]={};raw[0]='B';raw[1]='U';raw[2]=1;raw[3]=uint8_t(f.kind);
  put32(raw+4,f.transfer);put32(raw+8,f.offset);raw[12]=uint8_t(f.length);raw[13]=uint8_t(f.length>>8);
  std::memcpy(raw+16,f.data,f.length);put32(raw+16+f.length,crc32(raw,16+f.length));
  size_t used=2,codeAt=1;uint8_t code=1;output[0]=0;
  for(size_t i=0;i<20u+f.length;++i){if(raw[i]==0){output[codeAt]=code;codeAt=used++;code=1;}
    else {output[used++]=raw[i];if(++code==255){output[codeAt]=code;codeAt=used++;code=1;}}}
  output[codeAt]=code;output[used++]=0;return used;
}
bool UsbDecoder::feed(uint8_t value,UsbFrame& frame,bool& ready,bool& invalid) {
  ready=invalid=false;
  if(!collecting_){if(value)return false;collecting_=true;length_=0;overflow_=false;return true;}
  if(value){if(length_<sizeof(bytes_))bytes_[length_++]=value;else overflow_=true;return true;}
  if(!length_&&!overflow_)return true;
  ready=!overflow_&&decodeUsb(bytes_,length_,frame);invalid=!ready;collecting_=false;length_=0;overflow_=false;return true;
}
void Sender::begin(uint32_t id,uint32_t n,uint32_t start) {
  transfer=id;size=n;received=queued=start;retries=0;rto=80;srtt_=variation_=0;error=Error::Ok;
  for(auto& c:chunks_)c=Chunk();
  if(!id||!n||start>n||(start!=n&&start%kChunk))error=Error::Invalid;
}
Error Sender::enqueue(uint32_t offset,const uint8_t* data,size_t n) {
  if(error!=Error::Ok)return error;
  if(!data||!n||n>kUsbPayload||offset>size||n>size-offset||offset%kChunk||(n%kChunk&&offset+n!=size))return Error::Invalid;
  if(offset<queued){if(offset<received||n>queued-offset)return Error::Stale;
    for(size_t i=0;i<n;i+=kChunk){auto& c=chunks_[((offset+i)/kChunk)%kSlots];const size_t part=std::min(kChunk,n-i);
      if(c.offset!=offset+i||c.length!=part||std::memcmp(c.data,data+i,part))return Error::Conflict;}return Error::Ok;}
  if(offset!=queued)return Error::Offset;
  if(n>freeBytes())return Error::Busy;
  for(size_t i=0;i<n;i+=kChunk){auto& c=chunks_[((offset+i)/kChunk)%kSlots];c=Chunk();c.offset=offset+i;c.length=std::min(kChunk,n-i);std::memcpy(c.data,data+i,c.length);}
  queued+=n;return Error::Ok;
}
Chunk* Sender::next(uint32_t now) {
  if(error!=Error::Ok)return nullptr;
  const uint32_t end=std::min(queued,received+uint32_t(kWindow*kChunk));
  for(unsigned pass=0;pass<2;++pass)for(uint32_t offset=received;offset<end;offset+=kChunk){auto& c=chunks_[(offset/kChunk)%kSlots];
    if(c.acknowledged||c.driverQueued)continue;
    if((!pass&&!c.attempts)||(pass&&c.attempts&&uint32_t(now-c.sentAt)>=rto)){
      if(c.attempts>=9){error=Error::Timeout;return nullptr;}return &c;}}
  return nullptr;
}
void Sender::sent(Chunk& c,uint32_t now){c.driverQueued=false;if(c.attempts)++retries;++c.attempts;c.sentAt=now;}
void Sender::transmitted(uint32_t offset,uint32_t now){
  if(offset<received||offset>=queued||offset%kChunk)return;
  auto& c=chunks_[(offset/kChunk)%kSlots];
  if(c.offset==offset&&c.driverQueued){if(c.acknowledged)c.driverQueued=false;else sent(c,now);}
}
Error Sender::acknowledge(uint32_t confirmed,uint32_t bitmap,uint32_t now) {
  if(confirmed<received)return Error::Ok;
  if(confirmed>queued||(confirmed!=size&&confirmed%kChunk)||(bitmap&~255u)||(bitmap&1u)||(confirmed==size&&bitmap))return Error::Invalid;
  for(uint32_t p=received;p<confirmed;p+=kChunk)if(!chunks_[(p/kChunk)%kSlots].attempts)return Error::Invalid;
  for(unsigned i=0;i<kWindow;++i)if(bitmap&(1u<<i)){
    const uint32_t p=confirmed+i*kChunk;if(p>=queued||!chunks_[(p/kChunk)%kSlots].attempts)return Error::Invalid;}
  for(uint32_t p=received;p<queued&&p<confirmed+kWindow*kChunk;p+=kChunk){auto& c=chunks_[(p/kChunk)%kSlots];
    if(p<confirmed||(bitmap&(1u<<((p-confirmed)/kChunk)))){
      if(!c.acknowledged&&c.attempts==1){const uint32_t sample=std::max(1u,uint32_t(now-c.sentAt));
        if(!srtt_){srtt_=sample;variation_=sample/2;}else{variation_=(3*variation_+(srtt_>sample?srtt_-sample:sample-srtt_))/4;srtt_=(7*srtt_+sample)/8;}
        rto=std::max(40u,std::min(300u,srtt_+4*variation_));}c.acknowledged=true;}}
  received=confirmed;return Error::Ok;
}
} }
