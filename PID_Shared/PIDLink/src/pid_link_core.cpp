#include "pid_link_core.h"
#include <cstring>

namespace pidlink {
namespace {
void put(uint8_t* out,uint32_t value) { for (unsigned i=0;i<4;++i) out[i]=uint8_t(value>>(8*i)); }
uint32_t get(const uint8_t* in) { uint32_t value=0; for (unsigned i=0;i<4;++i) value|=uint32_t(in[i])<<(8*i); return value; }
uint16_t crc(const uint8_t* in,size_t length) {
  uint16_t value=0xffff;
  while (length--) {
    value^=uint16_t(*in++)<<8;
    for (unsigned i=0;i<8;++i) value=value&0x8000?uint16_t((value<<1)^0x1021):uint16_t(value<<1);
  }
  return value;
}
}
bool valid(const Snapshot& v) {
  return v.p>=0 && v.p<=999999 && v.i>=0 && v.i<=999999 && v.d>=0 && v.d<=999999 &&
      (v.step==1 || v.step==10 || v.step==100 || v.step==1000) && (!v.run || v.request!=0);
}
bool equal(const Snapshot& a,const Snapshot& b) {
  return a.p==b.p && a.i==b.i && a.d==b.d && a.step==b.step && a.revision==b.revision && a.request==b.request && a.run==b.run;
}
bool newer(uint32_t a,uint32_t b) { return int32_t(a-b)>0; }
void encode(const Frame& f,uint8_t* out) {
  std::memset(out,0,kFrameSize);
  out[0]='P'; out[1]='L'; out[2]=1; out[3]=uint8_t(f.kind);
  put(out+4,f.session); put(out+8,f.sequence); put(out+12,f.echoSession); put(out+16,f.echoSequence);
  put(out+20,f.data.revision); put(out+24,f.data.request);
  put(out+28,uint32_t(f.data.p)); put(out+32,uint32_t(f.data.i)); put(out+36,uint32_t(f.data.d));
  put(out+40,uint32_t(f.data.step)); out[44]=f.data.run?1:0;
  const uint16_t sum=crc(out,46); out[46]=uint8_t(sum); out[47]=uint8_t(sum>>8);
}
bool decode(const uint8_t* in,size_t size,Frame& out) {
  if (!in || size!=kFrameSize || in[0]!='P' || in[1]!='L' || in[2]!=1 || in[3]<1 || in[3]>3 ||
      in[44]>1 || in[45]!=0 || crc(in,46)!=(uint16_t(in[46])|(uint16_t(in[47])<<8))) return false;
  Frame f; f.kind=Kind(in[3]); f.session=get(in+4); f.sequence=get(in+8);
  f.echoSession=get(in+12); f.echoSequence=get(in+16); f.data.revision=get(in+20); f.data.request=get(in+24);
  f.data.p=int32_t(get(in+28)); f.data.i=int32_t(get(in+32)); f.data.d=int32_t(get(in+36));
  f.data.step=int32_t(get(in+40)); f.data.run=in[44]!=0;
  if (!f.session || !f.sequence || !valid(f.data)) return false;
  out=f; return true;
}
void Engine::begin(Role role,uint32_t session) { *this=Engine(); role_=role; session_=session?session:1; }
void Engine::disconnect() { received_=ackValid_=false; awaiting_=false; dirty_=desiredSet_; cooldown_=false; }
uint32_t Engine::sequence() { if (++sequence_==0) ++sequence_; return sequence_; }
bool Engine::changePeer(uint32_t session) {
  if (session==peerSession_) return true;
  for (uint32_t old:retired_) if (old==session) return false;
  if (peerSession_) retired_[retiredIndex_++%4]=peerSession_;
  peerSession_=session; peerSequence_=0; ++peerChanges_;
  ackValid_=awaiting_=mirrorValid_=false; dirty_=desiredSet_; cooldown_=false;
  return true;
}
void Engine::setDesired(const Snapshot& value) {
  if (!valid(value) || (desiredSet_ && equal(value,desired_))) return;
  desired_=value; desiredSet_=dirty_=true; awaiting_=false; cooldown_=false;
}
bool Engine::receive(const Frame& f,uint32_t now) {
  if (!f.session || !f.sequence || !valid(f.data)) return false;
  if (role_==Role::Host) {
    if (f.kind!=Kind::Hello && f.kind!=Kind::Ack) return false;
    if (f.kind==Kind::Hello) { if (!changePeer(f.session)) return false; }
    else if (f.session!=peerSession_) return false;
    if (peerSequence_ && !newer(f.sequence,peerSequence_)) return false;
    peerSequence_=f.sequence; lastRx_=now; received_=true;
    if (f.kind==Kind::Ack && awaiting_ && f.echoSession==session_ &&
        f.echoSequence==pendingSequence_ && equal(f.data,pending_)) {
      acked_=f.data; lastAck_=now; ackValid_=true; awaiting_=dirty_=false;
    }
    return true;
  }
  // A new slave boot issues a fresh challenge. Old queued State packets cannot apply.
  if (f.kind!=Kind::State || f.echoSession!=session_ || !changePeer(f.session)) return false;
  if (mirrorValid_ && f.sequence==stateSequence_) {
    if (!equal(f.data,mirror_)) return false;
  } else {
    if (mirrorValid_ && !newer(f.sequence,stateSequence_)) return false;
    mirror_=f.data; mirrorValid_=true; stateSequence_=f.sequence; ++applied_;
  }
  lastRx_=now; received_=true; ackPending_=true;
  return true;
}
bool Engine::next(uint32_t now,Frame& out) {
  if (role_==Role::Slave) {
    if (ackPending_) {
      out=Frame(); out.kind=Kind::Ack; out.session=session_; out.sequence=sequence();
      out.echoSession=peerSession_; out.echoSequence=stateSequence_; out.data=mirror_; ackPending_=false;
      return true;
    }
    if (!helloSent_ || uint32_t(now-lastHello_)>=kHeartbeatMs) {
      out=Frame(); out.kind=Kind::Hello; out.session=session_; out.sequence=sequence();
      out.echoSession=peerSession_; out.echoSequence=stateSequence_; out.data=mirror_;
      lastHello_=now; helloSent_=true; return true;
    }
    return false;
  }
  if (!peerSession_ || !desiredSet_ || !online(now)) return false;
  if (cooldown_) {
    if (uint32_t(now-cooldownAt_)<kHeartbeatMs) return false;
    cooldown_=false;
  }
  if (!awaiting_ && !dirty_ && uint32_t(now-lastSent_)>=kHeartbeatMs) dirty_=true;
  if (!awaiting_ && dirty_) {
    if (sent_ && uint32_t(now-lastSent_)<kCoalesceMs) return false;
    pending_=desired_; pendingSequence_=sequence(); awaiting_=true; attempts_=0;
  }
  if (!awaiting_ || (attempts_ && uint32_t(now-lastSent_)<kRetryMs)) return false;
  if (attempts_==4) {
    ++timeouts_; awaiting_=false; cooldown_=dirty_=true; cooldownAt_=now; return false;
  }
  out=Frame(); out.kind=Kind::State; out.session=session_; out.sequence=pendingSequence_;
  out.echoSession=peerSession_; out.data=pending_;
  if (attempts_) ++retries_;
  ++attempts_; lastSent_=now; sent_=true; return true;
}
}  // namespace pidlink
