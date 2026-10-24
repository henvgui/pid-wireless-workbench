#include "serial_log.h"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace pidflash;
int main() {
  LogConfig config;
  for(unsigned bits=5;bits<=8;++bits)for(unsigned parity=0;parity<=2;++parity)for(unsigned stop=1;stop<=2;++stop) {
    config.bits=bits;config.parity=parity;config.stop=stop;assert(config.valid());
  }
  config.baud=0;assert(!config.valid());config.baud=921601;assert(!config.valid());
  config=LogConfig();config.bits=9;assert(!config.valid());config=LogConfig();config.parity=3;assert(!config.valid());config=LogConfig();config.stop=3;assert(!config.valid());
  LogBuffer<8> ring;uint8_t out[8]={};uint32_t actual=0;size_t size=0;
  for(unsigned i=0;i<6;++i)ring.append(uint8_t(i));
  assert(ring.read(0,out,3,actual,size)==Error::Ok&&actual==0&&size==3&&out[2]==2);
  // A reply lost over radio/USB must be reproducible without consuming bytes.
  uint8_t retry[8];assert(ring.read(0,retry,3,actual,size)==Error::Ok&&!std::memcmp(out,retry,3));
  assert(ring.read(3,out,8,actual,size)==Error::Ok&&size==3&&out[0]==3);
  for(unsigned i=6;i<14;++i)ring.append(uint8_t(i));
  assert(ring.dropped()==3&&ring.end()==14);
  assert(ring.read(3,out,8,actual,size)==Error::Ok&&actual==6&&size==8&&out[0]==6&&out[7]==13);
  assert(ring.read(15,out,8,actual,size)==Error::Offset);
  assert(ring.read(14,out,8,actual,size)==Error::Ok&&size==0);
  ring.append(0);ring.append(255);assert(ring.read(14,out,8,actual,size)==Error::Ok&&size==2&&out[0]==0&&out[1]==255);
  ring.reset();assert(ring.dropped()==0&&ring.end()==0);
  for(Op op:{Op::LogOpen,Op::LogRead,Op::LogClose,Op::LogReply}) {
    Packet p;p.op=op;p.sender=1;p.peer=2;p.transfer=3;p.sequence=4;p.length=128;
    for(unsigned i=0;i<128;++i)p.data[i]=uint8_t(i);
    uint8_t bytes[kMaxPacket];auto n=encode(p,bytes);Packet result;
    assert(decode(bytes,n,result)&&result.op==op&&result.data[127]==127);
    bytes[40]^=1;assert(!decode(bytes,n,result));
  }
  std::cout<<"PASS: all UART formats, bounded cursor ring, lost reply replay, explicit overflow gap, binary bytes, invalid cursor and radio CRC.\n";
}
