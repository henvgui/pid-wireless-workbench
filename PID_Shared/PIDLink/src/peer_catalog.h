#pragma once
#include <cstdint>
#include <cstddef>
namespace pidlink {
constexpr uint8_t kPeerColors=8;
constexpr size_t kPeerCatalogBytes=60;
const char* peerColorName(uint8_t color);
int peerColor(const char* name);
void peerColorRgb(uint8_t color,uint8_t& red,uint8_t& green,uint8_t& blue);
bool validPeerMac(const uint8_t* mac);
bool parsePeerMac(const char* text,uint8_t* mac);
void formatPeerMac(const uint8_t* mac,char* text);
class PeerCatalog {
 public:
  void defaults();
  bool put(uint8_t color,const uint8_t* mac);
  bool remove(uint8_t color);
  bool select(uint8_t color);
  bool has(uint8_t color)const{return color<kPeerColors&&(mask_&(1u<<color));}
  uint8_t active()const{return active_;}
  uint8_t count()const;
  uint8_t index()const;
  uint8_t next()const;
  const uint8_t* mac(uint8_t color)const{return has(color)?macs_[color]:nullptr;}
  bool encode(uint8_t* bytes,size_t size)const;
  bool decode(const uint8_t* bytes,size_t size);
 private:
  uint8_t mask_=0,active_=0,macs_[kPeerColors][6]={};
};
} // namespace pidlink
