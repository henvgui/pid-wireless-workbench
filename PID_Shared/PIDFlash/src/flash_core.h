#pragma once
#include <cstddef>
#include <cstdint>

namespace pidflash {
constexpr uint32_t kBase=0x08000000, kMaxImage=1024*1024;
constexpr uint32_t kInternalImageLimit=128*1024;
constexpr size_t kLegacyChunk=128, kChunk=192, kMaxPacket=32+kChunk;
static_assert(kMaxPacket<=250,"Keep encrypted ESP-NOW frames within the v1 limit");
enum class Target:uint32_t { G431=1,F103C8=2,F407VG=3,F407VE=4 };
constexpr bool isF407(Target target) { return target==Target::F407VG||target==Target::F407VE; }
enum class Interface:uint32_t { Uart=1,Swd=2 };
struct TargetProfile {
  Target target;
  const char* name;
  uint16_t deviceId,pageSize;
  uint32_t capacityAddress,sramEnd,ccmEnd;
  uint16_t maxKb;
  uint8_t writeAlignment;
  uint32_t eraseTimeoutMs;
};
const TargetProfile* profile(Target target);
bool capacityValid(Target target,uint32_t kb);
bool interfaceValid(Target target,Interface interface);
uint32_t eraseUnits(Target target,uint32_t size);
enum class Op:uint8_t { Begin=1,Data,Seal,Flash,Query,Abort,Reply,EchoOpen,EchoData,EchoClose,EchoReply,UartRead,UartReply,MotorStatus,MotorReply,MotorTarget,LogOpen,LogRead,LogClose,LogReply,SerialOpen,SerialRead,SerialWrite,SerialClose,SerialReply };
enum class State:uint8_t { Idle,Receiving,Ready,Programming,Done,Failed };
enum class Error:uint8_t { Ok,Invalid,Busy,WrongTransfer,Offset,Conflict,Incomplete,Hash,Vector,
  Disabled,NoMemory,Timeout,Nack,Unsupported,Device,Capacity,Verify,Stale,Link,
  SwdParity,SwdFault,SwdProtocol,FlashStatus,Start };
struct Packet {
  Op op=Op::Query;
  uint32_t sender=0,peer=0,transfer=0,sequence=0,offset=0;
  uint16_t length=0;
  uint8_t data[kChunk]={};
};
uint32_t crc32(const uint8_t* data,size_t size);
uint32_t get32(const uint8_t* data);
void put32(uint8_t* data,uint32_t value);
size_t encode(const Packet& packet,uint8_t* output);
bool decode(const uint8_t* data,size_t size,Packet& packet);
bool vectorsValid(const uint8_t* data,uint32_t size,Target target=Target::G431);
const char* stateName(State value);
const char* errorName(Error value);
using Hash=bool (*)(const uint8_t*,size_t,uint8_t*);
class Staging {
 public:
  explicit Staging(uint8_t* bytes=nullptr,uint32_t capacity=kMaxImage):bytes_(bytes),capacity_(capacity) {}
  void storage(uint8_t* bytes,uint32_t capacity=kMaxImage) { bytes_=bytes;capacity_=capacity; }
  Error begin(uint32_t transfer,uint32_t size,uint32_t flashKb,const uint8_t* digest,Target target=Target::G431,Interface interface=Interface::Uart);
  Error append(uint32_t transfer,uint32_t offset,const uint8_t* data,size_t size);
  Error seal(uint32_t transfer,Hash hash);
  Error abort(uint32_t transfer);
  bool busy() const { return state==State::Programming; }
  uint32_t transfer=0,size=0,received=0,flashKb=0;
  Target target=Target::G431;
  Interface interface=Interface::Uart;
  State state=State::Idle;
  uint8_t digest[32]={};
 private:
  uint8_t* bytes_=nullptr;
  uint32_t capacity_=0;
};
} // namespace pidflash
