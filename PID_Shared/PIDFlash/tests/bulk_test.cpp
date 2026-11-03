#include "bulk_core.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include <random>
#include <algorithm>
using namespace pidflash;
bool hashImage(const uint8_t* p,size_t n,uint8_t* hash){for(unsigned i=0;i<8;++i)put32(hash+i*4,crc32(p,n)+i);return true;}
std::vector<uint8_t> image(size_t n){std::vector<uint8_t> v(n);for(size_t i=0;i<n;++i)v[i]=uint8_t(i*71);put32(v.data(),0x20010000);put32(v.data()+4,kBase+9);return v;}
void framing(){
  bulk::UsbFrame f;f.kind=bulk::UsbKind::Data;f.transfer=123;f.offset=0;f.length=bulk::kUsbPayload;
  for(size_t i=0;i<f.length;++i)f.data[i]=uint8_t(i);
  std::memcpy(f.data+250,"!run\n!fw flash ERASE\n",21);
  uint8_t encoded[bulk::kUsbEncoded+2];size_t n=bulk::encodeUsb(f,encoded,sizeof(encoded));assert(n);
  bulk::UsbDecoder decoder;bulk::UsbFrame decoded;bool ready=false,bad=false;unsigned count=0;
  for(size_t i=0;i<n;++i){assert(decoder.feed(encoded[i],decoded,ready,bad));assert(!bad);if(ready)++count;}
  assert(count==1&&decoded.transfer==123&&decoded.length==f.length&&!std::memcmp(decoded.data,f.data,f.length));
  encoded[20]^=1;count=0;unsigned errors=0;
  for(size_t i=0;i<n;++i){decoder.feed(encoded[i],decoded,ready,bad);count+=ready;errors+=bad;}assert(!count&&errors==1);
  n=bulk::encodeUsb(f,encoded,sizeof(encoded));for(size_t i=0;i<100;++i)decoder.feed(encoded[i],decoded,ready,bad);
  decoder.feed(0,decoded,ready,bad);assert(bad);
  f.kind=bulk::UsbKind::Exit;f.length=0;n=bulk::encodeUsb(f,encoded,sizeof(encoded));count=0;
  for(size_t i=0;i<n;++i){decoder.feed(encoded[i],decoded,ready,bad);count+=ready;}assert(count==1&&decoded.kind==bulk::UsbKind::Exit);
  assert(!decoder.feed('!',decoded,ready,bad));
}
void window(){
  auto source=image(192*10+3);std::vector<uint8_t> memory(source.size());uint8_t digest[32];hashImage(source.data(),source.size(),digest);
  Staging s(memory.data(),memory.size());assert(s.begin(11,source.size(),512,digest,Target::F407VE,Interface::Swd)==Error::Ok);
  uint32_t mask=0;assert(s.appendWindow(11,192*8,source.data()+192*8,192,mask)==Error::Offset);
  assert(s.appendWindow(11,192,source.data()+192,192,mask)==Error::Ok&&s.received==0&&mask==2);
  assert(s.seal(11,hashImage)==Error::Incomplete);
  uint8_t corrupt[192];std::memcpy(corrupt,source.data()+192,192);corrupt[0]^=1;
  assert(s.appendWindow(11,192,corrupt,192,mask)==Error::Conflict);
  assert(s.appendWindow(11,0,source.data(),192,mask)==Error::Ok&&s.received==384&&!mask);
  assert(s.appendWindow(11,0,source.data(),192,mask)==Error::Ok);
  assert(s.appendWindow(12,384,source.data()+384,192,mask)==Error::WrongTransfer);
  bulk::Sender tx;tx.begin(11,source.size(),0);assert(tx.enqueue(0,source.data(),source.size())==Error::Ok);
  assert(tx.acknowledge(192,0,0)==Error::Invalid);auto* c=tx.next(0);assert(c&&c->offset==0);tx.sent(*c,0);
  assert(tx.acknowledge(0,1,1)==Error::Invalid);assert(tx.acknowledge(0,2,1)==Error::Invalid);
  assert(tx.acknowledge(192,0,20)==Error::Ok);assert(tx.acknowledge(0,0,21)==Error::Ok&&tx.received==192);
  for(unsigned i=0;i<9;++i){auto* p=tx.next(1000+i*1000);assert(p);tx.sent(*p,1000+i*1000);}
}
struct Delivery{uint32_t at,offset;uint16_t length;uint8_t data[192];};
struct Ack{uint32_t at,received,bitmap;};
void simulation(size_t size,unsigned loss){
  auto source=image(size);std::vector<uint8_t> memory(size);uint8_t digest[32];hashImage(source.data(),size,digest);
  Staging rx(memory.data(),size);assert(rx.begin(43,size,1024,digest,Target::F407VG,Interface::Swd)==Error::Ok);
  bulk::Sender tx;tx.begin(43,size,0);std::mt19937 random(42+loss);std::vector<Delivery> flying;std::vector<Ack> acks;
  uint32_t mask=0,packetCount=0,lastAck=0;bool pending=false;uint32_t now;
  for(now=0;now<200000&&tx.received<size;++now){
    while(tx.queued<size){const size_t n=std::min(bulk::kUsbPayload,size-tx.queued);if(n>tx.freeBytes())break;assert(tx.enqueue(tx.queued,source.data()+tx.queued,n)==Error::Ok);}
    for(unsigned i=0;i<8;++i){auto* c=tx.next(now);if(!c)break;Delivery d={now+1+random()%9,c->offset,c->length,{}};std::memcpy(d.data,c->data,c->length);tx.sent(*c,now);
      if(random()%100>=loss){flying.push_back(d);if(random()%5==0){d.at+=3;flying.push_back(d);}}}
    std::shuffle(flying.begin(),flying.end(),random);
    for(size_t i=0;i<flying.size();){if(flying[i].at>now){++i;continue;}const auto d=flying[i];flying.erase(flying.begin()+i);
      assert(rx.appendWindow(43,d.offset,d.data,d.length,mask)==Error::Ok);pending=true;++packetCount;}
    if(pending&&(packetCount>=4||uint32_t(now-lastAck)>=8||rx.received==size)){
      if(random()%100>=loss)acks.push_back({now+1+random()%9,rx.received,mask});
      pending=false;lastAck=now;packetCount=0;}
    for(size_t i=0;i<acks.size();){if(acks[i].at>now){++i;continue;}auto a=acks[i];acks.erase(acks.begin()+i);assert(tx.acknowledge(a.received,a.bitmap,now)==Error::Ok);}
    assert(tx.error==Error::Ok);
  }
  assert(tx.received==size&&rx.received==size&&!mask);assert(rx.seal(43,hashImage)==Error::Ok);assert(source==memory);
  std::printf("PASS %u bytes, %u%% data/ACK loss, reorder/duplicates, %u retries\n",unsigned(size),loss,unsigned(tx.retries));
}
void timeout(){bulk::Sender tx;auto v=image(192);tx.begin(7,192,0);assert(tx.enqueue(0,v.data(),192)==Error::Ok);
  auto* queued=tx.next(0);assert(queued);tx.scheduled(*queued);assert(!tx.next(5000)&&queued->attempts==0);
  assert(tx.acknowledge(192,0,5001)==Error::Invalid);tx.transmitted(0,5002);assert(queued->attempts==1&&!tx.next(5003));
  assert(tx.acknowledge(192,0,5004)==Error::Ok);
  tx.begin(1,192,0);assert(tx.enqueue(0,v.data(),v.size())==Error::Ok);
  for(unsigned i=0;i<9;++i){auto* c=tx.next(i*400);assert(c);tx.sent(*c,i*400);}assert(!tx.next(4000)&&tx.error==Error::Timeout);
  tx.begin(2,192,0);assert(tx.enqueue(0,v.data(),192)==Error::Ok);auto* c=tx.next(0xfffffff0);assert(c);tx.sent(*c,0xfffffff0);assert(!tx.next(10));assert(tx.next(100));}
int main(){framing();window();timeout();simulation(3075,0);simulation(131072,1);simulation(131075,5);simulation(1048576,10);std::puts("PASS bulk framing, selective retry, bounds, hashes, and timeout");}
