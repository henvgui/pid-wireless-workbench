#include "PIDLink.h"
#include <Arduino.h>
#include <WiFi.h>
#include <cstring>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "../private/pair_keys.h"
#include "pair_devices.h"

namespace pidlink {
namespace {
uint8_t peerMac[6]={};
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
constexpr size_t kInboxBytes=250;
constexpr uint8_t kInboxCount=32;
uint8_t inbox[kInboxCount][kInboxBytes]={};
uint16_t inboxSize[kInboxCount]={};
struct AuxItem {uint8_t bytes[kInboxBytes];uint16_t size;};
AuxItem urgent[8],normal[16];
uint8_t urgentRead=0,urgentWrite=0,urgentCount=0,normalRead=0,normalWrite=0,normalCount=0;
bool sending=false;
AuxReceiver auxReceiver=nullptr;
AuxTransmitted auxTransmitted=nullptr;
uint8_t readAt=0,writeAt=0,count=0;
uint32_t droppedCount=0,deliveryFailureCount=0;
void onReceive(const uint8_t* mac,const uint8_t* data,int length) {
  if (!mac || !data || length<=0 || length>int(kInboxBytes)) return;
  portENTER_CRITICAL(&mux);
  if(std::memcmp(mac,peerMac,6)){portEXIT_CRITICAL(&mux);return;}
  if (count==kInboxCount) ++droppedCount;
  else { std::memcpy(inbox[writeAt],data,size_t(length)); inboxSize[writeAt]=length; writeAt=(writeAt+1)%kInboxCount; ++count; }
  portEXIT_CRITICAL(&mux);
}
void onSend(const uint8_t* mac,esp_now_send_status_t result) {
  portENTER_CRITICAL(&mux);
  if(!mac||std::memcmp(mac,peerMac,6)){portEXIT_CRITICAL(&mux);return;}
  sending=false;portEXIT_CRITICAL(&mux);
  if (result!=ESP_NOW_SEND_SUCCESS) {
    portENTER_CRITICAL(&mux); ++deliveryFailureCount; portEXIT_CRITICAL(&mux);
  }
}
}
Radio radio;
bool Radio::begin(Role role) {
  role_=role; enabled_=true; ready_=false; error_=ESP_OK;
  portENTER_CRITICAL(&mux); readAt=writeAt=count=0;urgentRead=urgentWrite=urgentCount=normalRead=normalWrite=normalCount=0;sending=false;portEXIT_CRITICAL(&mux);
  WiFi.persistent(false); WiFi.setAutoReconnect(false);
  if (!WiFi.mode(WIFI_STA)) { error_=ESP_FAIL; return false; }
  WiFi.disconnect(false,false);
  error_=esp_wifi_get_mac(WIFI_IF_STA,localMac_);
  // Each new slave has its own factory MAC; the host roster authorizes which
  // one can communicate. All slaves still bind to this host and encrypted key.
  if(error_!=ESP_OK||!validPeerMac(localMac_)||(role==Role::Host&&std::memcmp(localMac_,kConfiguredHostMac,6))||
      (role==Role::Slave&&!std::memcmp(localMac_,kConfiguredHostMac,6))){error_=ESP_ERR_INVALID_ARG;return false;}
  if(role==Role::Host&&!validPeerMac(hostPeer_))std::memcpy(hostPeer_,kDefaultSlaveMac,6);
  portENTER_CRITICAL(&mux);std::memcpy(peerMac,role==Role::Host?hostPeer_:kConfiguredHostMac,6);portEXIT_CRITICAL(&mux);
  error_=esp_wifi_set_ps(WIFI_PS_NONE);
  if (error_==ESP_OK) error_=esp_wifi_set_channel(kChannel,WIFI_SECOND_CHAN_NONE);
  if (error_==ESP_OK) error_=esp_now_init();
  if (error_==ESP_OK) error_=esp_now_set_pmk(kPrimaryKey);
  esp_now_peer_info_t peer={};
  std::memcpy(peer.peer_addr,peerMac,6); std::memcpy(peer.lmk,kPairKey,16);
  peer.channel=kChannel; peer.ifidx=WIFI_IF_STA; peer.encrypt=true;
  if (error_==ESP_OK) error_=esp_now_add_peer(&peer);
  if (error_==ESP_OK) error_=esp_now_register_recv_cb(onReceive);
  if (error_==ESP_OK) error_=esp_now_register_send_cb(onSend);
  uint32_t session=esp_random(); if (!session) session=1;
  engine_.begin(role,session); ready_=error_==ESP_OK;
  return ready_;
}
bool Radio::enable(bool value) {
  if (value==enabled_ && (!value || ready_)) return true;
  if (!value) {
    esp_now_unregister_recv_cb(); esp_now_unregister_send_cb(); esp_now_deinit();
    ready_=enabled_=false; engine_.disconnect(); WiFi.mode(WIFI_OFF);
    portENTER_CRITICAL(&mux); readAt=writeAt=count=0; portEXIT_CRITICAL(&mux);
    return true;
  }
  return begin(role_);
}
void Radio::receive(uint32_t now) {
  if (!ready_) return;
  for (unsigned i=0;i<kInboxCount;++i) {
    uint8_t bytes[kInboxBytes]; size_t length=0; bool available=false;
    portENTER_CRITICAL(&mux);
    if (count) { length=inboxSize[readAt]; std::memcpy(bytes,inbox[readAt],length); readAt=(readAt+1)%kInboxCount; --count; available=true; }
    portEXIT_CRITICAL(&mux);
    if (!available) break;
    if (length>=2 && bytes[0]=='P' && bytes[1]=='F') {
      if (auxReceiver && auxReceiver(bytes,length,now)) ++receivedCount_;
      else ++rejected_;
      continue;
    }
    Frame frame;
    if (decode(bytes,length,frame) && engine_.receive(frame,now)) ++receivedCount_;
    else ++rejected_;
  }
}
void Radio::transmit(uint32_t now) {
  if (!ready_) return;
  portENTER_CRITICAL(&mux);bool busy=sending;portEXIT_CRITICAL(&mux);if(busy)return;
  uint8_t bytes[kInboxBytes];size_t size=0;bool auxiliary=false,high=false;
  Frame frame;
  if(engine_.next(now,frame)){encode(frame,bytes);size=kFrameSize;}
  else {
    portENTER_CRITICAL(&mux);
    if(urgentCount){auto& item=urgent[urgentRead];size=item.size;std::memcpy(bytes,item.bytes,size);high=true;}
    else if(normalCount){auto& item=normal[normalRead];size=item.size;std::memcpy(bytes,item.bytes,size);}
    portEXIT_CRITICAL(&mux);auxiliary=true;
  }
  if(!size)return;
  portENTER_CRITICAL(&mux);sending=true;portEXIT_CRITICAL(&mux);
  const esp_err_t result=esp_now_send(peerMac,bytes,size);
  if (result==ESP_OK) ++sent_;
  portENTER_CRITICAL(&mux);
  if(result!=ESP_OK){sending=false;++deliveryFailureCount;}
  else if(auxiliary){if(high){urgentRead=(urgentRead+1)%8;--urgentCount;}else{normalRead=(normalRead+1)%16;--normalCount;}}
  portEXIT_CRITICAL(&mux);
  if(result==ESP_OK&&auxiliary&&auxTransmitted)auxTransmitted(bytes,size,millis());
}
void Radio::setAuxReceiver(AuxReceiver receiver) { auxReceiver=receiver; }
void Radio::setAuxTransmitted(AuxTransmitted callback){auxTransmitted=callback;}
bool Radio::sendAux(const uint8_t* bytes,size_t size,bool priority) {
  if (!ready_ || !bytes || !size || size>kInboxBytes) return false;
  bool accepted=false;portENTER_CRITICAL(&mux);
  if(priority&&urgentCount<8){auto& item=urgent[urgentWrite];item.size=size;std::memcpy(item.bytes,bytes,size);urgentWrite=(urgentWrite+1)%8;++urgentCount;accepted=true;}
  else if(!priority&&normalCount<16){auto& item=normal[normalWrite];item.size=size;std::memcpy(item.bytes,bytes,size);normalWrite=(normalWrite+1)%16;++normalCount;accepted=true;}
  portEXIT_CRITICAL(&mux);return accepted;
}
uint32_t Radio::dropped() const {
  portENTER_CRITICAL(&mux); const uint32_t result=droppedCount; portEXIT_CRITICAL(&mux); return result;
}
uint32_t Radio::deliveryFailures() const {
  portENTER_CRITICAL(&mux); const uint32_t result=deliveryFailureCount; portEXIT_CRITICAL(&mux); return result;
}
const uint8_t* Radio::peerAddress()const{return peerMac;}
bool Radio::configurePeer(const uint8_t* mac){
  if(role_!=Role::Host||!validPeerMac(mac)||!std::memcmp(mac,kConfiguredHostMac,6)){error_=ESP_ERR_INVALID_ARG;return false;}
  if(!enabled_){std::memcpy(hostPeer_,mac,6);return true;}
  if(!ready_){enable(false);std::memcpy(hostPeer_,mac,6);return begin(Role::Host);}
  if(!std::memcmp(peerMac,mac,6))return true;
  const uint32_t began=millis();
  for(;;){portENTER_CRITICAL(&mux);const bool busy=sending;portEXIT_CRITICAL(&mux);
    if(!busy)break;if(uint32_t(millis()-began)>=30){error_=ESP_ERR_INVALID_STATE;return false;}delay(1);}
  esp_now_peer_info_t peer={};std::memcpy(peer.peer_addr,mac,6);std::memcpy(peer.lmk,kPairKey,16);
  peer.channel=kChannel;peer.ifidx=WIFI_IF_STA;peer.encrypt=true;
  error_=esp_now_add_peer(&peer);if(error_!=ESP_OK&&error_!=ESP_ERR_ESPNOW_EXIST)return false;
  uint8_t old[6];
  portENTER_CRITICAL(&mux);
  std::memcpy(old,peerMac,6);std::memcpy(peerMac,mac,6);std::memcpy(hostPeer_,mac,6);
  readAt=writeAt=count=0;urgentRead=urgentWrite=urgentCount=normalRead=normalWrite=normalCount=0;sending=false;
  portEXIT_CRITICAL(&mux);
  esp_now_del_peer(old);
  uint32_t session=esp_random();if(!session||session==engine_.session())session=engine_.session()+1;if(!session)session=1;
  engine_.begin(Role::Host,session);error_=ESP_OK;return true;
}
}  // namespace pidlink
