#include "flash_core.h"
#include <cstring>
namespace pidflash {
const TargetProfile* profile(Target target) {
  static const TargetProfile g431={Target::G431,"g431",0x468,2048,0x1fff75e0,0x20008000,0x10002800,128,8,30000};
  static const TargetProfile f103={Target::F103C8,"f103c8",0x410,1024,0x1ffff7e0,0x20005000,0,64,4,30000};
  // Main SRAM stack only. F4 ROM GO can disable the CCM clock before startup.
  static const TargetProfile f407={Target::F407VG,"f407vg",0x413,0,0x1fff7a22,0x20020000,0,1024,4,120000};
  static const TargetProfile f407ve={Target::F407VE,"f407ve",0x413,0,0x1fff7a22,0x20020000,0,512,4,120000};
  if(target==Target::G431)return &g431;
  if(target==Target::F103C8)return &f103;
  if(target==Target::F407VG)return &f407;
  if(target==Target::F407VE)return &f407ve;
  return nullptr;
}
bool capacityValid(Target target,uint32_t kb) {
  return target==Target::F103C8?kb==64:target==Target::F407VG?kb==1024:target==Target::F407VE?kb==512:target==Target::G431&&(kb==32||kb==64||kb==128);
}
bool interfaceValid(Target target,Interface interface) {
  return profile(target)&&(interface==Interface::Uart||(interface==Interface::Swd&&isF407(target)));
}
uint32_t eraseUnits(Target target,uint32_t size) {
  const auto* p=profile(target);
  if(!p||!size||size>uint32_t(p->maxKb)*1024)return 0;
  if(!isF407(target))return (size+p->pageSize-1)/p->pageSize;
  if(size<=64*1024)return (size+16*1024-1)/(16*1024);
  if(size<=128*1024)return 5;
  return 5+(size-128*1024+128*1024-1)/(128*1024);
}
uint32_t get32(const uint8_t* p) { uint32_t v=0; for(unsigned i=0;i<4;++i)v|=uint32_t(p[i])<<(i*8); return v; }
void put32(uint8_t* p,uint32_t v) { for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(i*8)); }
uint32_t crc32(const uint8_t* p,size_t n) {
  uint32_t crc=~0u;
  while(n--) { crc^=*p++; for(unsigned i=0;i<8;++i)crc=(crc>>1)^((crc&1)?0xedb88320u:0); }
  return ~crc;
}
size_t encode(const Packet& p,uint8_t* out) {
  if(p.length>kChunk)return 0;
  std::memset(out,0,32+p.length); out[0]='P';out[1]='F';out[2]=1;out[3]=uint8_t(p.op);
  put32(out+4,p.sender);put32(out+8,p.peer);put32(out+12,p.transfer);
  put32(out+16,p.sequence);put32(out+20,p.offset);out[24]=uint8_t(p.length);out[25]=uint8_t(p.length>>8);
  std::memcpy(out+28,p.data,p.length);put32(out+28+p.length,crc32(out,28+p.length));
  return 32+p.length;
}
bool decode(const uint8_t* in,size_t n,Packet& out) {
  if(!in||n<32||n>kMaxPacket||in[0]!='P'||in[1]!='F'||in[2]!=1||in[3]<1||in[3]>uint8_t(Op::BulkAck)||in[26]||in[27])return false;
  const uint16_t size=uint16_t(in[24])|(uint16_t(in[25])<<8);
  if(size>kChunk||n!=32u+size||get32(in+28+size)!=crc32(in,28+size))return false;
  Packet p;p.op=Op(in[3]);p.sender=get32(in+4);p.peer=get32(in+8);p.transfer=get32(in+12);
  p.sequence=get32(in+16);p.offset=get32(in+20);p.length=size;
  if(!p.sender||!p.peer||!p.transfer||!p.sequence)return false;
  std::memcpy(p.data,in+28,size);out=p;return true;
}
bool vectorsValid(const uint8_t* data,uint32_t size,Target target) {
  const TargetProfile* p=profile(target);
  if(!p||!data||size<8||size>uint32_t(p->maxKb)*1024)return false;
  const uint32_t sp=get32(data),pc=get32(data+4);
  const bool stack=(sp>0x20000000&&sp<=p->sramEnd)||(p->ccmEnd&&sp>0x10000000&&sp<=p->ccmEnd);
  return stack&&!(sp&7)&&(pc&1)&&(pc&~1u)>=kBase+8&&(pc&~1u)<kBase+size;
}
Error Staging::begin(uint32_t id,uint32_t n,uint32_t kb,const uint8_t* sha,Target selected,Interface selectedInterface) {
  if(busy())return Error::Busy;
  if(!id||!sha||!capacityValid(selected,kb)||!interfaceValid(selected,selectedInterface)||n<8||n>kb*1024||n>kMaxImage)return Error::Invalid;
  if(id==transfer&&state!=State::Idle) {
    return size==n&&flashKb==kb&&target==selected&&interface==selectedInterface&&!std::memcmp(digest,sha,32)?Error::Ok:Error::Conflict;
  }
  if(!bytes_||n>capacity_)return Error::NoMemory;
  transfer=id;size=n;flashKb=kb;target=selected;interface=selectedInterface;received=0;std::memcpy(digest,sha,32);state=State::Receiving;return Error::Ok;
}
Error Staging::append(uint32_t id,uint32_t offset,const uint8_t* data,size_t n) {
  if(id!=transfer)return Error::WrongTransfer;
  if(busy())return Error::Busy;
  if(state!=State::Receiving&&state!=State::Ready)return Error::Invalid;
  if(!data||!n||n>kChunk||offset>size||n>size-offset)return Error::Invalid;
  if(offset<received) {
    if(n>received-offset)return Error::Offset;
    return std::memcmp(bytes_+offset,data,n)?Error::Conflict:Error::Ok;
  }
  if(offset!=received)return Error::Offset;
  if(state!=State::Receiving)return Error::Invalid;
  std::memcpy(bytes_+received,data,n);received+=n;return Error::Ok;
}
Error Staging::appendWindow(uint32_t id,uint32_t offset,const uint8_t* data,size_t n,uint32_t& bitmap) {
  if(id!=transfer)return Error::WrongTransfer;
  if(state!=State::Receiving)return busy()?Error::Busy:Error::Invalid;
  if(!data||!n||offset%kChunk||offset>=size||n!=(size-offset<kChunk?size-offset:kChunk))return Error::Invalid;
  if(offset<received)return n<=received-offset&&!std::memcmp(bytes_+offset,data,n)?Error::Ok:Error::Conflict;
  const uint32_t slot=(offset-received)/kChunk;
  if(slot>=8)return Error::Offset;
  if(bitmap&(1u<<slot))return std::memcmp(bytes_+offset,data,n)?Error::Conflict:Error::Ok;
  std::memcpy(bytes_+offset,data,n);bitmap|=1u<<slot;
  while(bitmap&1u){received+=size-received<kChunk?size-received:kChunk;bitmap>>=1;}
  return Error::Ok;
}
Error Staging::seal(uint32_t id,Hash hash) {
  if(id!=transfer)return Error::WrongTransfer;
  if(state==State::Ready)return Error::Ok;
  if(state!=State::Receiving)return busy()?Error::Busy:Error::Invalid;
  if(received!=size)return Error::Incomplete;
  uint8_t actual[32];
  if(!hash||!hash(bytes_,size,actual)||std::memcmp(digest,actual,32))return Error::Hash;
  if(!vectorsValid(bytes_,size,target))return Error::Vector;
  state=State::Ready;return Error::Ok;
}
Error Staging::abort(uint32_t id) {
  if(busy())return Error::Busy;
  if(transfer&&id!=transfer)return Error::WrongTransfer;
  state=State::Idle;transfer=id;received=size=flashKb=0;return Error::Ok;
}
const char* stateName(State s) {
  const char* names[]={"idle","receiving","ready","programming","done","failed"};
  return uint8_t(s)<6?names[uint8_t(s)]:"invalid";
}
const char* errorName(Error e) {
  const char* names[]={"ok","invalid","busy","wrong_transfer","offset","conflict","incomplete","sha256_mismatch","invalid_vectors",
    "target_disabled","no_memory","timeout","uart_nack","unsupported_command","wrong_device","flash_capacity","readback_mismatch","stale_request","no_link",
    "swd_parity","swd_fault","swd_protocol","flash_status","start_failed"};
  return uint8_t(e)<sizeof(names)/sizeof(names[0])?names[uint8_t(e)]:"unknown";
}
} // namespace pidflash
