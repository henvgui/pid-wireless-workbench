#include "PIDLink.h"
#include <Arduino.h>
#include <WiFi.h>
#include <cstring>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "../private/pair_keys.h"

namespace pidlink {
namespace {
const uint8_t hostMac[6]={0x44,0x1b,0xf6,0x8d,0x10,0x20};
const uint8_t slaveMac[6]={0x44,0x1b,0xf6,0x8d,0x10,0x18};
uint8_t peerMac[6]={};
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
constexpr size_t kInboxBytes=250;
uint8_t inbox[8][kInboxBytes]={};
uint16_t inboxSize[8]={};
AuxReceiver auxReceiver=nullptr;
uint8_t readAt=0,writeAt=0,count=0;
uint32_t droppedCount=0,deliveryFailureCount=0;
void onReceive(const uint8_t* mac,const uint8_t* data,int length) {
  if (!mac || !data || length<=0 || length>int(kInboxBytes) || std::memcmp(mac,peerMac,6)) return;
  portENTER_CRITICAL(&mux);
  if (count==8) ++droppedCount;
  else { std::memcpy(inbox[writeAt],data,size_t(length)); inboxSize[writeAt]=length; writeAt=(writeAt+1)%8; ++count; }
  portEXIT_CRITICAL(&mux);
}
void onSend(const uint8_t*,esp_now_send_status_t result) {
  if (result!=ESP_NOW_SEND_SUCCESS) {
    portENTER_CRITICAL(&mux); ++deliveryFailureCount; portEXIT_CRITICAL(&mux);
  }
}
}
Radio radio;
bool Radio::begin(Role role) {
  role_=role; enabled_=true; ready_=false; error_=ESP_OK;
  portENTER_CRITICAL(&mux); readAt=writeAt=count=0; portEXIT_CRITICAL(&mux);
  WiFi.persistent(false); WiFi.setAutoReconnect(false);
  if (!WiFi.mode(WIFI_STA)) { error_=ESP_FAIL; return false; }
  WiFi.disconnect(false,false);
  uint8_t local[6]; error_=esp_wifi_get_mac(WIFI_IF_STA,local);
  const uint8_t* expected=role==Role::Host?hostMac:slaveMac;
  if (error_!=ESP_OK || std::memcmp(local,expected,6)) { error_=ESP_ERR_INVALID_ARG; return false; }
  std::memcpy(peerMac,role==Role::Host?slaveMac:hostMac,6);
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
  for (unsigned i=0;i<8;++i) {
    uint8_t bytes[kInboxBytes]; size_t length=0; bool available=false;
    portENTER_CRITICAL(&mux);
    if (count) { length=inboxSize[readAt]; std::memcpy(bytes,inbox[readAt],length); readAt=(readAt+1)%8; --count; available=true; }
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
  Frame frame; if (!engine_.next(now,frame)) return;
  uint8_t bytes[kFrameSize]; encode(frame,bytes);
  const esp_err_t result=esp_now_send(peerMac,bytes,sizeof(bytes));
  if (result==ESP_OK) ++sent_;
  else { portENTER_CRITICAL(&mux); ++deliveryFailureCount; portEXIT_CRITICAL(&mux); }
}
void Radio::setAuxReceiver(AuxReceiver receiver) { auxReceiver=receiver; }
bool Radio::sendAux(const uint8_t* bytes,size_t size) {
  if (!ready_ || !bytes || !size || size>kInboxBytes) return false;
  if (esp_now_send(peerMac,bytes,size)!=ESP_OK) return false;
  ++sent_; return true;
}
uint32_t Radio::dropped() const {
  portENTER_CRITICAL(&mux); const uint32_t result=droppedCount; portEXIT_CRITICAL(&mux); return result;
}
uint32_t Radio::deliveryFailures() const {
  portENTER_CRITICAL(&mux); const uint32_t result=deliveryFailureCount; portEXIT_CRITICAL(&mux); return result;
}
}  // namespace pidlink
