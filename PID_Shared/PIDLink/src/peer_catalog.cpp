#include "peer_catalog.h"
#include "pair_devices.h"
#include <cstring>
#include <cstdio>
namespace pidlink {
namespace {
const char* const names[]={"GREEN","BLUE","RED","YELLOW","CYAN","PURPLE","ORANGE","PINK"};
const uint8_t colors[][3]={{0,255,0},{0,0,255},{255,0,0},{255,190,0},{0,220,255},{160,0,255},{255,70,0},{255,0,110}};
int hex(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
uint32_t crc(const uint8_t* p,size_t n){uint32_t v=~0u;while(n--){v^=*p++;for(unsigned b=0;b<8;++b)v=(v>>1)^(0xedb88320u&(0u-(v&1)));}return ~v;}
}
const char* peerColorName(uint8_t color){return color<kPeerColors?names[color]:"UNKNOWN";}
int peerColor(const char* name){if(!name)return -1;for(unsigned i=0;i<kPeerColors;++i)if(!std::strcmp(name,names[i]))return int(i);return -1;}
void peerColorRgb(uint8_t c,uint8_t& r,uint8_t& g,uint8_t& b){if(c>=kPeerColors)c=0;r=colors[c][0];g=colors[c][1];b=colors[c][2];}
bool validPeerMac(const uint8_t* m){if(!m||(m[0]&1))return false;uint8_t any=0;for(unsigned i=0;i<6;++i)any|=m[i];return any!=0;}
bool parsePeerMac(const char* text,uint8_t* mac){
 if(!text||!mac)return false;const size_t n=std::strlen(text);if(n!=12&&n!=17)return false;
 uint8_t result[6];for(unsigned i=0;i<6;++i){const unsigned p=i*(n==17?3:2);int a=hex(text[p]),b=hex(text[p+1]);
  if(a<0||b<0||(n==17&&i<5&&text[p+2]!=':'))return false;result[i]=uint8_t((a<<4)|b);}
 if(!validPeerMac(result))return false;std::memcpy(mac,result,6);return true;
}
void formatPeerMac(const uint8_t* m,char* s){std::snprintf(s,18,"%02X:%02X:%02X:%02X:%02X:%02X",m[0],m[1],m[2],m[3],m[4],m[5]);}
void PeerCatalog::defaults(){*this=PeerCatalog();put(0,kDefaultSlaveMac);}
bool PeerCatalog::put(uint8_t c,const uint8_t* m){
 if(c>=kPeerColors||!validPeerMac(m))return false;
 for(unsigned i=0;i<kPeerColors;++i)if(i!=c&&has(i)&&!std::memcmp(m,macs_[i],6))return false;
 if(!mask_)active_=c;mask_|=uint8_t(1u<<c);std::memcpy(macs_[c],m,6);return true;
}
bool PeerCatalog::remove(uint8_t c){if(!has(c)||count()==1)return false;mask_&=uint8_t(~(1u<<c));std::memset(macs_[c],0,6);
 if(active_==c){for(unsigned i=0;i<kPeerColors;++i)if(has(i)){active_=uint8_t(i);break;}}return true;}
bool PeerCatalog::select(uint8_t c){if(!has(c))return false;active_=c;return true;}
uint8_t PeerCatalog::count()const{uint8_t n=0;for(unsigned i=0;i<kPeerColors;++i)n+=has(i);return n;}
uint8_t PeerCatalog::index()const{uint8_t n=0;for(unsigned i=0;i<=active_;++i)n+=has(i);return n;}
uint8_t PeerCatalog::next()const{for(unsigned i=1;i<=kPeerColors;++i){uint8_t c=uint8_t((active_+i)%kPeerColors);if(has(c))return c;}return active_;}
bool PeerCatalog::encode(uint8_t* p,size_t size)const{
 if(!p||size!=kPeerCatalogBytes||!has(active_))return false;
 std::memset(p,0,size);std::memcpy(p,"PEER",4);p[4]=1;p[5]=mask_;p[6]=active_;std::memcpy(p+8,macs_,48);
 uint32_t sum=crc(p,56);for(unsigned i=0;i<4;++i)p[56+i]=uint8_t(sum>>(8*i));return true;
}
bool PeerCatalog::decode(const uint8_t* p,size_t size){
 if(!p||size!=kPeerCatalogBytes||std::memcmp(p,"PEER",4)||p[4]!=1||!p[5]||p[7])return false;
 uint32_t sum=0;for(unsigned i=0;i<4;++i)sum|=uint32_t(p[56+i])<<(8*i);if(sum!=crc(p,56))return false;
 PeerCatalog next;for(unsigned i=0;i<kPeerColors;++i)if(p[5]&(1u<<i)){if(!next.put(uint8_t(i),p+8+i*6))return false;}
 if(!next.select(p[6]))return false;*this=next;return true;
}
} // namespace pidlink
