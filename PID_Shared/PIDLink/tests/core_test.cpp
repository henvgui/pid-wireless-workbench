#include "pid_link_core.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#undef assert
#define assert(condition) do { if (!(condition)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#condition); std::exit(1); } } while (0)
using namespace pidlink;
int main() {
  Engine host,slave; host.begin(Role::Host,100); slave.begin(Role::Slave,200);
  Snapshot data; data.p=125; data.i=10; data.d=250; data.step=10; data.revision=1;
  host.setDesired(data);
  Frame f,ack;
  assert(!host.next(0,f)); // Requires slave boot challenge before sending state.
  assert(slave.next(0,f) && f.kind==Kind::Hello && host.receive(f,0));
  assert(host.next(0,f) && f.kind==Kind::State);
  uint8_t bytes[kFrameSize]; encode(f,bytes);
  Frame decoded; assert(decode(bytes,sizeof(bytes),decoded) && equal(f.data,decoded.data));
  for (unsigned bit=0;bit<kFrameSize*8;++bit) {
    uint8_t damaged[kFrameSize]; std::memcpy(damaged,bytes,sizeof(bytes)); damaged[bit/8]^=1<<(bit%8);
    assert(!decode(damaged,sizeof(damaged),decoded));
  }
  assert(!decode(bytes,sizeof(bytes)-1,decoded));
  assert(slave.receive(f,0) && slave.mirrorValid() && slave.applied()==1);
  assert(slave.next(0,ack) && ack.kind==Kind::Ack && host.receive(ack,0) && host.synced(0));
  assert(slave.receive(f,20) && slave.applied()==1); // Retransmission is idempotent.
  assert(slave.next(20,ack));
  data.p=225; data.revision=2; host.setDesired(data);
  assert(!host.synced(21) && !host.next(21,f));
  assert(host.next(40,f));
  assert(host.receive(ack,41) && !host.synced(41)); // Old ACK can be received but cannot confirm a new state.
  Frame retry; assert(!host.next(119,retry) && host.next(120,retry));
  assert(retry.sequence==f.sequence && equal(retry.data,f.data) && host.retries()==1);
  assert(slave.receive(retry,120) && slave.next(120,ack) && host.receive(ack,120) && host.synced(120));
  // New state supersedes old in-flight state; its old acknowledgement is ignored.
  data.run=true; data.request=1; host.setDesired(data); assert(host.next(160,f));
  assert(slave.receive(f,160) && slave.next(160,ack));
  data.run=false; data.request=2; host.setDesired(data);
  assert(host.receive(ack,161) && !host.synced(161));
  assert(host.next(200,f) && slave.receive(f,200) && slave.next(200,ack));
  assert(host.receive(ack,200) && host.synced(200));
  Frame stale=f;
  // New slave boot rejects State packets addressed to the old boot session.
  slave.begin(Role::Slave,201); assert(!slave.receive(stale,201));
  assert(slave.next(201,f) && host.receive(f,201) && !host.synced(201));
  assert(host.peerChanges()==2 && host.next(240,f) && f.echoSession==201);
  assert(slave.receive(f,240) && slave.next(240,ack) && host.receive(ack,240));
  Frame oldHello; oldHello.kind=Kind::Hello; oldHello.session=200; oldHello.sequence=99;
  assert(!host.receive(oldHello,250));
  // New host boot can sync; delayed packets from the retired host cannot revert it.
  host.begin(Role::Host,101); host.setDesired(data);
  assert(slave.next(701,f) && host.receive(f,701) && host.next(701,f));
  assert(slave.receive(f,701) && slave.peerChanges()==2);
  stale.echoSession=201; assert(!slave.receive(stale,702));
  assert(slave.next(701,ack) && host.receive(ack,701));
  assert(!host.online(2201) && !host.ackFresh(2201) && !host.synced(2201));
  assert(!slave.online(2201));
  // Lost ACK: four sends, then bounded cooldown and heartbeat recovery.
  host.begin(Role::Host,300); slave.begin(Role::Slave,400); host.setDesired(data);
  assert(slave.next(0,f) && host.receive(f,0));
  for (uint32_t now=0;now<=240;now+=80) assert(host.next(now,f));
  assert(!host.next(320,f) && host.timeouts()==1 && host.retries()==3);
  assert(!host.next(819,f) && host.next(820,f));
  assert(slave.receive(f,820) && slave.next(820,ack) && host.receive(ack,820) && host.synced(820));
  // Unsigned time rollover keeps deadlines valid.
  host.begin(Role::Host,500); slave.begin(Role::Slave,600); host.setDesired(data);
  assert(slave.next(0xfffffff0,f) && host.receive(f,0xfffffff0) && host.next(0xfffffff0,f));
  assert(host.next(0x40,f));
  assert(newer(1,0xffffffffu) && !newer(0xffffffffu,1));
  std::puts("PASS: CRC corruption, atomic state, handshake challenges, retry/idempotence, stale ACK/session rejection, timeout and rollover");
}
