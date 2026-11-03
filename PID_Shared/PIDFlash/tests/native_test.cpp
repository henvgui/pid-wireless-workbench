#include "flash_core.h"
#include "stm32_rom.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <deque>
#include <iostream>
#include <random>
#include <vector>
using namespace pidflash;
// Native tests use a deterministic digest stand-in. Real SHA256 is exercised on ESP32.
bool digest(const uint8_t* b,size_t n,uint8_t* h){for(unsigned i=0;i<8;++i)put32(h+i*4,crc32(b,n)+i);return true;}
std::vector<uint8_t> firmware(size_t n,Target target=Target::G431) {
  std::vector<uint8_t> b(n);for(size_t i=0;i<n;++i)b[i]=uint8_t(i*31);
  put32(b.data(),target==Target::F103C8?0x20005000:0x20008000);put32(b.data()+4,kBase+9);return b;
}
struct Rom:RomIo {
  enum Mode {Sync,Command,Address,ReadCount,WriteData,EraseData} mode=Sync;
  uint8_t cmd=0;uint32_t addressValue=0;uint16_t id=0x468,kb=128;
  unsigned erases=0,writes=0,pageSize=2048,alignment=8;uint32_t capacityAddress=0x1fff75e0;
  bool legacyErase=false,extendedErase=true;
  bool goSupported=true;unsigned goCommands=0;
  bool corrupt=false,missingWrite=false,loseWriteAck=false;
  std::vector<unsigned> erasedPages,writeSizes;
  std::deque<uint8_t> rx;std::vector<uint8_t> flash=std::vector<uint8_t>(kMaxImage,0xaa);
  explicit Rom(Target target=Target::G431) {
    if(target==Target::F103C8){id=0x410;kb=64;pageSize=1024;alignment=4;capacityAddress=0x1ffff7e0;legacyErase=true;extendedErase=false;}
    if(target==Target::F407VG){id=0x413;kb=1024;pageSize=0;alignment=4;capacityAddress=0x1fff7a22;}
  }
  void ack(){rx.push_back(0x79);}
  bool write(const uint8_t* p,size_t n) override {
    if(mode==Sync){assert(n==1&&p[0]==0x7f);ack();mode=Command;return true;}
    if(mode==Command) {
      assert(n==2&&(p[0]^p[1])==0xff);cmd=p[0];ack();
      if(cmd==0) {
        const uint8_t commands[]={0,2,0x11,uint8_t(missingWrite?0x21:0x31)};
        rx.push_back(uint8_t(4+unsigned(legacyErase)+unsigned(extendedErase)+unsigned(goSupported)));
        rx.push_back(legacyErase?0x22:0x31);
        for(int c:commands)rx.push_back(c);
        if(goSupported)rx.push_back(0x21);
        if(legacyErase)rx.push_back(0x43);
        if(extendedErase)rx.push_back(0x44);
        ack();
      }
      else if(cmd==2){rx.push_back(1);rx.push_back(uint8_t(id>>8));rx.push_back(uint8_t(id));ack();}
      else if(cmd==0x44||cmd==0x43){assert(cmd==0x43?legacyErase:extendedErase);mode=EraseData;}
      else {assert(cmd==0x11||cmd==0x31||cmd==0x21);mode=Address;}
      return true;
    }
    if(mode==Address) {
      assert(n==5&&(p[0]^p[1]^p[2]^p[3]^p[4])==0);
      addressValue=(uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];
      ack();
      if(cmd==0x21){assert(goSupported&&addressValue==kBase);++goCommands;mode=Command;}
      else mode=cmd==0x11?ReadCount:WriteData;
      return true;
    }
    if(mode==ReadCount) {
      assert(n==2&&(p[0]^p[1])==0xff);ack();
      const size_t amount=size_t(p[0])+1;
      for(size_t i=0;i<amount;++i) {
        if(addressValue==capacityAddress){assert(amount==2);rx.push_back(i?uint8_t(kb>>8):uint8_t(kb));}
        else {assert(addressValue>=kBase&&addressValue-kBase+amount<=flash.size());rx.push_back(flash[addressValue-kBase+i]^(corrupt&&i==0?1:0));}
      }
      mode=Command;return true;
    }
    if(mode==EraseData) {
      const bool ext=cmd==0x44;
      const size_t count=ext?(size_t(p[0])<<8|p[1])+1:size_t(p[0])+1;
      assert(n==(ext?count*2+3:count+2));
      uint8_t sum=0;for(size_t i=0;i<n;++i)sum^=p[i];assert(sum==0);
      for(size_t i=0;i<count;++i){
        size_t page=ext?(size_t(p[2+i*2])<<8|p[3+i*2]):p[1+i];
        if(pageSize) {
          assert(page<uint32_t(kb)*1024/pageSize);
          std::fill(flash.begin()+page*pageSize,flash.begin()+(page+1)*pageSize,0xff);
        } else {
          static const unsigned boundaries[]={0,16384,32768,49152,65536,131072,262144,393216,524288,655360,786432,917504,1048576};
          assert(page<12);std::fill(flash.begin()+boundaries[page],flash.begin()+boundaries[page+1],0xff);
        }
        erasedPages.push_back(unsigned(page));
      }
      ++erases;ack();mode=Command;return true;
    }
    assert(mode==WriteData);const size_t amount=size_t(p[0])+1;assert(n==amount+2&&amount%alignment==0&&addressValue%alignment==0);
    uint8_t sum=0;for(size_t i=0;i<n;++i)sum^=p[i];assert(sum==0);
    assert(addressValue>=kBase&&addressValue-kBase+amount<=flash.size());
    for(size_t i=0;i<amount;++i){assert(flash[addressValue-kBase+i]==0xff);flash[addressValue-kBase+i]=p[i+1];}
    ++writes;writeSizes.push_back(unsigned(amount));if(!loseWriteAck)ack();mode=Command;return true;
  }
  int read(uint32_t) override {if(rx.empty())return -1;int b=rx.front();rx.pop_front();return b;}
};
int main() {
  {Packet large;large.op=Op::Data;large.sender=1;large.peer=2;large.transfer=3;large.sequence=4;large.length=kChunk;
   for(size_t i=0;i<kChunk;++i)large.data[i]=uint8_t(i);
   uint8_t encoded[kMaxPacket];Packet decoded;const size_t n=encode(large,encoded);
   assert(n==224&&decode(encoded,n,decoded)&&decoded.length==192&&!std::memcmp(decoded.data,large.data,192));
   for(size_t i=0;i<n;++i){encoded[i]^=1;assert(!decode(encoded,n,decoded));encoded[i]^=1;}}
  const uint8_t check[]={'1','2','3','4','5','6','7','8','9'};assert(crc32(check,9)==0xcbf43926);
  auto image=firmware(128*1024);uint8_t sha[32];digest(image.data(),image.size(),sha);
  std::vector<uint8_t> stored(image.size());Staging stage(stored.data(),stored.size());
  assert(stage.begin(1,uint32_t(image.size()),128,sha)==Error::Ok);
  Packet p;p.op=Op::Data;p.sender=10;p.peer=20;p.transfer=1;p.sequence=1;p.length=128;
  uint8_t bytes[kMaxPacket];std::memcpy(p.data,image.data(),128);size_t n=encode(p,bytes);Packet decoded;
  assert(decode(bytes,n,decoded));for(size_t i=0;i<n;++i){bytes[i]^=1;assert(!decode(bytes,n,decoded));bytes[i]^=1;}
  for(Op op:{Op::UartRead,Op::UartReply,Op::MotorStatus,Op::MotorReply,Op::MotorTarget}) {
    Packet capture;capture.op=op;capture.sender=10;capture.peer=20;capture.transfer=5;capture.sequence=7;capture.offset=500;
    capture.length=op==Op::UartReply?kChunk:0;
    std::memcpy(capture.data,image.data(),capture.length);
    const size_t size=encode(capture,bytes);
    assert(decode(bytes,size,decoded)&&decoded.op==op&&decoded.offset==500&&decoded.length==capture.length);
    bytes[size-1]^=1;assert(!decode(bytes,size,decoded));bytes[size-1]^=1;
    bytes[3]=uint8_t(Op::BulkAck)+1;put32(bytes+28+capture.length,crc32(bytes,28+capture.length));
    assert(!decode(bytes,size,decoded));
  }
  std::mt19937 rng(12345);unsigned dropped=0,replayed=0;
  for(uint32_t offset=0;offset<image.size();) {
    p.offset=offset;std::memcpy(p.data,image.data()+offset,128);++p.sequence;n=encode(p,bytes);
    bool accepted=false;
    for(unsigned attempts=0;attempts<100&&!accepted;++attempts) {
      if(rng()%10<3){++dropped;continue;}
      assert(decode(bytes,n,decoded));assert(stage.append(1,decoded.offset,decoded.data,decoded.length)==Error::Ok);
      if(rng()%10<3){++replayed;continue;}accepted=true;
    }
    assert(accepted);offset+=128;
  }
  assert(dropped>0&&replayed>0&&stored==image&&stage.received==image.size());
  assert(stage.begin(1,uint32_t(image.size()),128,sha)==Error::Ok&&stage.received==image.size());
  assert(stage.append(1,0,image.data(),128)==Error::Ok);
  stored[100]^=1;assert(stage.append(1,0,image.data(),128)==Error::Conflict);
  assert(stage.seal(1,digest)==Error::Hash);stored[100]^=1;
  assert(stage.seal(1,digest)==Error::Ok&&stage.state==State::Ready);
  assert(stage.seal(1,digest)==Error::Ok);stage.state=State::Programming;
  assert(stage.begin(2,8,32,sha)==Error::Busy&&stage.abort(1)==Error::Busy);
  stage.state=State::Ready;assert(stage.abort(1)==Error::Ok);
  assert(stage.begin(3,100,32,sha)==Error::Ok);
  assert(stage.append(3,128,image.data(),128)==Error::Invalid);
  assert(stage.append(3,1,image.data(),8)==Error::Offset);
  assert(stage.seal(3,digest)==Error::Incomplete);
  assert(stage.begin(4,65537,64,sha)==Error::Invalid);
  auto small=firmware(273);Rom good;Stm32Rom programmer(good);
  assert(programmer.program(small.data(),small.size(),128)==Error::Ok);
  assert(programmer.deviceId==0x468&&good.erases==1&&good.writes==2);
  assert(std::equal(small.begin(),small.end(),good.flash.begin()));
  assert(good.flash[273]==0xff&&good.flash[280]==0xff&&good.flash[2048]==0xaa);
  Rom wrong;wrong.id=0x469;Stm32Rom wrongProgrammer(wrong);
  assert(wrongProgrammer.program(small.data(),small.size(),128)==Error::Device&&wrong.erases==0);
  Rom capacity;capacity.kb=64;Stm32Rom capacityProgrammer(capacity);
  assert(capacityProgrammer.program(small.data(),small.size(),128)==Error::Capacity&&capacity.erases==0);
  assert(capacityProgrammer.flashKb==64);
  Rom missing;missing.missingWrite=true;Stm32Rom missingProgrammer(missing);
  assert(missingProgrammer.program(small.data(),small.size(),128)==Error::Unsupported&&missing.erases==0);
  Rom corrupt;corrupt.corrupt=true;Stm32Rom corruptProgrammer(corrupt);
  assert(corruptProgrammer.program(small.data(),small.size(),128)==Error::Verify);
  Rom timeout;timeout.loseWriteAck=true;Stm32Rom timeoutProgrammer(timeout);
  assert(timeoutProgrammer.program(small.data(),small.size(),128)==Error::Timeout&&timeout.writes==1);
  small[0]=1;Rom bad;Stm32Rom badProgrammer(bad);
  assert(badProgrammer.program(small.data(),small.size(),128)==Error::Invalid&&bad.erases==0);
  auto f103=firmware(1027,Target::F103C8);
  assert(vectorsValid(f103.data(),f103.size(),Target::F103C8));
  auto g431=firmware(1029);
  assert(!vectorsValid(g431.data(),g431.size(),Target::F103C8));
  put32(g431.data(),0x10001000);assert(!vectorsValid(g431.data(),g431.size(),Target::F103C8));
  digest(f103.data(),f103.size(),sha);
  assert(stage.begin(10,uint32_t(f103.size()),64,sha,Target::F103C8)==Error::Ok);
  assert(stage.target==Target::F103C8);
  assert(stage.begin(10,uint32_t(f103.size()),64,sha,Target::G431)==Error::Conflict);
  assert(stage.begin(11,65537,64,sha,Target::F103C8)==Error::Invalid);
  assert(stage.begin(11,1029,128,sha,Target::F103C8)==Error::Invalid);
  assert(stage.begin(11,1029,64,sha,Target(99))==Error::Invalid);
  for(uint32_t offset=0;offset<f103.size();offset+=kChunk) {
    const size_t amount=std::min(kChunk,f103.size()-offset);
    assert(stage.append(10,offset,f103.data()+offset,amount)==Error::Ok);
  }
  assert(stage.seal(10,digest)==Error::Ok);
  Rom oldBoot(Target::F103C8);Stm32Rom oldProgrammer(oldBoot);
  assert(oldProgrammer.program(f103.data(),f103.size(),64,nullptr,Target::F103C8)==Error::Ok);
  assert(oldProgrammer.deviceId==0x410&&oldBoot.erases==1&&oldBoot.writes==5);
  assert(oldBoot.writeSizes.back()==4);
  assert(oldBoot.erasedPages==std::vector<unsigned>({0,1}));
  assert(std::equal(f103.begin(),f103.end(),oldBoot.flash.begin()));
  assert(oldBoot.flash[1029]==0xff&&oldBoot.flash[1031]==0xff&&oldBoot.flash[2048]==0xaa);
  assert(oldProgrammer.go()==Error::Ok&&oldBoot.goCommands==1);
  assert(oldProgrammer.go()==Error::Invalid&&oldBoot.goCommands==1);
  Rom extBoot(Target::F103C8);extBoot.legacyErase=false;extBoot.extendedErase=true;
  Stm32Rom extProgrammer(extBoot);
  assert(extProgrammer.program(f103.data(),f103.size(),64,nullptr,Target::F103C8)==Error::Ok);
  assert(extBoot.erasedPages==std::vector<unsigned>({0,1}));
  auto full=firmware(64*1024,Target::F103C8);Rom fullBoot(Target::F103C8);Stm32Rom fullProgrammer(fullBoot);
  assert(fullProgrammer.program(full.data(),full.size(),64,nullptr,Target::F103C8)==Error::Ok);
  assert(fullBoot.erasedPages.size()==64&&fullBoot.erasedPages.back()==63&&fullBoot.writes==256);
  Rom wrongFamily;Stm32Rom wrongFamilyProgrammer(wrongFamily);
  assert(wrongFamilyProgrammer.program(f103.data(),f103.size(),64,nullptr,Target::F103C8)==Error::Device&&wrongFamily.erases==0);
  Rom f103Capacity(Target::F103C8);f103Capacity.kb=128;Stm32Rom f103CapacityProgrammer(f103Capacity);
  assert(f103CapacityProgrammer.program(f103.data(),f103.size(),64,nullptr,Target::F103C8)==Error::Ok);
  assert(f103CapacityProgrammer.flashKb==128);
  assert(f103Capacity.erasedPages==std::vector<unsigned>({0,1}));
  Rom fullLargerBoot(Target::F103C8);fullLargerBoot.kb=128;Stm32Rom fullLargerProgrammer(fullLargerBoot);
  assert(fullLargerProgrammer.program(full.data(),full.size(),64,nullptr,Target::F103C8)==Error::Ok);
  assert(fullLargerBoot.erasedPages.size()==64&&fullLargerBoot.erasedPages.back()==63);
  assert(fullLargerBoot.flash[64*1024]==0xaa&&fullLargerBoot.flash.back()==0xaa);
  for(uint16_t unexpected:{uint16_t(0),uint16_t(32),uint16_t(256),uint16_t(0xffff)}) {
    Rom invalidCapacity(Target::F103C8);invalidCapacity.kb=unexpected;Stm32Rom invalidCapacityProgrammer(invalidCapacity);
    assert(invalidCapacityProgrammer.program(f103.data(),f103.size(),64,nullptr,Target::F103C8)==Error::Capacity);
    assert(invalidCapacityProgrammer.flashKb==unexpected&&invalidCapacity.erases==0);
  }
  Rom f103Timeout(Target::F103C8);f103Timeout.loseWriteAck=true;Stm32Rom f103TimeoutProgrammer(f103Timeout);
  assert(f103TimeoutProgrammer.program(f103.data(),f103.size(),64,nullptr,Target::F103C8)==Error::Timeout&&f103Timeout.writes==1);
  assert(f103TimeoutProgrammer.go()==Error::Invalid&&f103Timeout.goCommands==0);
  Rom noGo(Target::F103C8);noGo.goSupported=false;Stm32Rom noGoProgrammer(noGo);
  assert(noGoProgrammer.program(f103.data(),f103.size(),64,nullptr,Target::F103C8)==Error::Ok);
  assert(noGoProgrammer.go()==Error::Unsupported&&noGo.goCommands==0);
  Rom noErase(Target::F103C8);noErase.legacyErase=false;Stm32Rom noEraseProgrammer(noErase);
  assert(noEraseProgrammer.program(f103.data(),f103.size(),64,nullptr,Target::F103C8)==Error::Unsupported&&noErase.erases==0);
  for(const auto& boundary:std::vector<std::pair<uint32_t,uint32_t>>{{9,1},{16384,1},{16385,2},{65536,4},{65537,5},{131072,5},{131073,6},{1048576,12}}) {
    auto f407=firmware(boundary.first,Target::F407VG);Rom boot(Target::F407VG);Stm32Rom programmer(boot);
    assert(programmer.program(f407.data(),f407.size(),1024,nullptr,Target::F407VG)==Error::Ok);
    assert(boot.erasedPages.size()==boundary.second);
    assert(std::equal(f407.begin(),f407.end(),boot.flash.begin()));
  }
  std::cout<<"PASS: transport/CRC/retry/duplicate/SHA gates; G431/F103 regression; F407 UART sector boundaries, full 1 MiB image, ID/capacity and full readback.\n";
}
