#pragma once
#include "pid_link_core.h"
#include "peer_catalog.h"
#include "esp_err.h"

namespace pidlink {
constexpr uint8_t kChannel=1;
using AuxReceiver=bool (*)(const uint8_t*,size_t,uint32_t);
using AuxTransmitted=void (*)(const uint8_t*,size_t,uint32_t);
class Radio {
 public:
  bool begin(Role role);
  bool enable(bool value);
  void receive(uint32_t now);
  void transmit(uint32_t now);
  void setDesired(const Snapshot& value) { engine_.setDesired(value); }
  const Engine& engine() const { return engine_; }
  bool ready() const { return ready_; }
  bool enabled() const { return enabled_; }
  esp_err_t error() const { return error_; }
  uint32_t sent() const { return sent_; }
  uint32_t received() const { return receivedCount_; }
  uint32_t rejected() const { return rejected_; }
  uint32_t dropped() const;
  uint32_t deliveryFailures() const;
  void setAuxReceiver(AuxReceiver receiver);
  void setAuxTransmitted(AuxTransmitted callback);
  bool sendAux(const uint8_t* bytes,size_t size,bool priority=false);
  // Host-only, registered peer; call only when upper-layer transactions are idle.
  bool configurePeer(const uint8_t* mac);
  const uint8_t* peerAddress()const;
  const uint8_t* localAddress()const{return localMac_;}
 private:
  Engine engine_;
  Role role_=Role::Host;
  bool ready_=false,enabled_=false;
  esp_err_t error_=ESP_OK;
  uint32_t sent_=0,receivedCount_=0,rejected_=0;
  uint8_t hostPeer_[6]={},localMac_[6]={};
};
extern Radio radio;
}  // namespace pidlink
