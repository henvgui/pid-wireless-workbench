#include "peer_catalog.h"
#include "pid_link_core.h"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace pidlink;
int main(){
 PeerCatalog peers;peers.defaults();assert(peers.count()==1&&peers.active()==0&&peers.index()==1&&peers.next()==0);
 uint8_t blue[6];assert(parsePeerMac("02:00:00:00:00:02",blue));
 assert(peers.put(1,blue)&&peers.select(peers.next())&&peers.active()==1&&peers.index()==2&&peers.next()==0);
 assert(!peers.put(2,blue));assert(!peers.remove(7));assert(peers.remove(0)&&peers.active()==1&&peers.count()==1&&!peers.remove(1));
 for(unsigned c=0;c<kPeerColors;++c){uint8_t mac[]={2,0,0,0,0,uint8_t(c+1)};assert(peers.put(uint8_t(c),mac));}
 assert(peers.count()==8);for(unsigned n=0;n<16;++n)assert(peers.select(peers.next()));
 uint8_t bytes[kPeerCatalogBytes];assert(peers.encode(bytes,sizeof(bytes)));PeerCatalog copy;assert(copy.decode(bytes,sizeof(bytes))&&copy.active()==peers.active()&&copy.count()==8);
 for(unsigned bit=0;bit<sizeof(bytes)*8;++bit){bytes[bit/8]^=1u<<(bit%8);assert(!copy.decode(bytes,sizeof(bytes))&&copy.count()==8);bytes[bit/8]^=1u<<(bit%8);}
 assert(!copy.decode(bytes,sizeof(bytes)-1));assert(!peers.select(255));
 for(const char* bad:{"00:00:00:00:00:00","FF:FF:FF:FF:FF:FF","01:00:00:00:00:02","02-00-00-00-00-02","020000000002F","02:00:00:00:00:0G"})assert(!parsePeerMac(bad,blue));
 assert(parsePeerMac("441bf68d1018",blue));char text[18];formatPeerMac(blue,text);assert(!std::strcmp(text,"44:1B:F6:8D:10:18"));
 for(unsigned c=0;c<kPeerColors;++c){assert(peerColor(peerColorName(uint8_t(c)))==int(c));uint8_t r,g,b;peerColorRgb(uint8_t(c),r,g,b);assert(r||g||b);}
 assert(peerColor("green")==-1&&peerColor("GREEN\"}")==-1);
 // Routing restarts the host session: stale state for GREEN cannot be applied
 // by BLUE, and returning to GREEN accepts its still-running boot challenge.
 Engine host,green,other;host.begin(Role::Host,10);green.begin(Role::Slave,20);other.begin(Role::Slave,30);
 Snapshot value;value.step=1;host.setDesired(value);Frame f,old;
 assert(green.next(0,f)&&host.receive(f,0)&&host.next(0,old)&&green.receive(old,0));
 assert(green.next(0,f)&&f.kind==Kind::Ack&&host.receive(f,0));
 host.begin(Role::Host,40);host.setDesired(value);assert(!other.receive(old,1));
 assert(other.next(1,f)&&host.receive(f,1)&&host.next(1,f)&&other.receive(f,1)&&!green.receive(f,1));
 host.begin(Role::Host,50);host.setDesired(value);assert(green.next(500,f)&&host.receive(f,500)&&host.next(500,f)&&green.receive(f,500));
 std::cout<<"PASS: persistent color/MAC roster, duplicates, bounds, corrupted storage, exclusive routing and fresh host sessions.\n";
}
