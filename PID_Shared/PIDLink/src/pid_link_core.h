#pragma once
#include <stddef.h>
#include <stdint.h>

namespace pidlink {
constexpr size_t kFrameSize=48;
constexpr uint32_t kHeartbeatMs=500,kTimeoutMs=1500,kRetryMs=80,kCoalesceMs=40;
enum class Role { Host, Slave };
enum class Kind:uint8_t { State=1,Ack=2,Hello=3 };
struct Snapshot {
  int32_t p=0,i=0,d=0,step=100;
  uint32_t revision=0,request=0;
  bool run=false;
};
bool valid(const Snapshot& data);
bool equal(const Snapshot& a,const Snapshot& b);
bool newer(uint32_t a,uint32_t b);
struct Frame {
  Kind kind=Kind::Hello;
  uint32_t session=0,sequence=0,echoSession=0,echoSequence=0;
  Snapshot data;
};
void encode(const Frame& frame,uint8_t* bytes);
bool decode(const uint8_t* bytes,size_t size,Frame& frame);

// No Arduino dependencies: deterministic protocol, retries and boot-session handling.
class Engine {
 public:
  void begin(Role role,uint32_t session);
  bool receive(const Frame& frame,uint32_t now);
  void setDesired(const Snapshot& value);
  bool next(uint32_t now,Frame& frame);
  void disconnect();
  bool online(uint32_t now) const { return received_ && uint32_t(now-lastRx_)<kTimeoutMs; }
  bool ackFresh(uint32_t now) const { return ackValid_ && online(now) && uint32_t(now-lastAck_)<kTimeoutMs; }
  bool synced(uint32_t now) const { return ackFresh(now) && equal(desired_,acked_); }
  bool mirrorValid() const { return mirrorValid_; }
  const Snapshot& mirror() const { return mirror_; }
  const Snapshot& acknowledged() const { return acked_; }
  uint32_t session() const { return session_; }
  uint32_t peerSession() const { return peerSession_; }
  uint32_t peerChanges() const { return peerChanges_; }
  uint32_t lastRxAge(uint32_t now) const { return received_ ? uint32_t(now-lastRx_) : UINT32_MAX; }
  uint32_t retries() const { return retries_; }
  uint32_t timeouts() const { return timeouts_; }
  uint32_t applied() const { return applied_; }
 private:
  bool changePeer(uint32_t session);
  uint32_t sequence();
  Role role_=Role::Host;
  uint32_t session_=0,sequence_=0,peerSession_=0,peerChanges_=0,peerSequence_=0;
  uint32_t retired_[4]={},retiredIndex_=0,lastRx_=0,lastAck_=0,lastSent_=0,lastHello_=0,cooldownAt_=0;
  uint32_t pendingSequence_=0,stateSequence_=0,retries_=0,timeouts_=0,applied_=0;
  uint8_t attempts_=0;
  bool received_=false,ackValid_=false,desiredSet_=false,dirty_=false,awaiting_=false;
  bool sent_=false,helloSent_=false,cooldown_=false,mirrorValid_=false,ackPending_=false;
  Snapshot desired_,pending_,acked_,mirror_;
};
}  // namespace pidlink
