#include <Arduino.h>
#include <Preferences.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <atomic>
#include "host_local_app.h"
#include "host_local_core.h"
#include "host_oled.h"
#if defined(PID_LINK_ENABLED)
#include <PIDLink.h>
#endif
#if defined(PID_FLASH_ENABLED)
#include <PIDFlash.h>
#endif

namespace hostapp {
namespace {
hostui::Controller* panel = nullptr;
hostlocal::Session session;
Preferences storage;
stepcontrol::Button runButton, saveButton;
bool storageReady=false, streaming=true, displayDirty=true;
bool parametersPending=false, runPending=false, savePending=false;
bool savedResult=false;
uint32_t lastParameters=0, toastAt=0;
const char* toast="";
char previousHeader[32] = {};
#if defined(PID_LINK_ENABLED)
char tx[1024] = {};
Preferences peerStorage;
pidlink::PeerCatalog peers;
bool peerStorageReady=false,peerConfigurationOk=true;
uint32_t peerDoubleClicks=0,peerSwitches=0;
const char* peerFailure="invalid_peer";
// Host schematic: 3V3 -> R1 1k -> LED2 -> GPIO38 (active LOW).
constexpr uint8_t kLinkLedPin=38;
bool linkLedOn=false;
bool previouslyHealthy=false,linkPending=true;
uint32_t peerChanges=0,lastLinkReport=0;
uint8_t linkSignature=255;
#else
char tx[384] = {};
#endif
size_t txLength=0, txOffset=0;
TaskHandle_t usbFlushTask=nullptr;
std::atomic<bool> usbFlushDone{false};
bool usbFlushRequested=false;
void usbFlushWorker(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    Serial.setTxTimeoutMs(250);
    size_t sent=0;
    while (sent<txLength && Serial) {
      const size_t count=Serial.write(reinterpret_cast<const uint8_t*>(tx+sent),txLength-sent);
      if (!count) break;
      sent+=count;
    }
    Serial.flush();
    // A ring-buffer wrap can make the final hardware packet exactly 64 bytes
    // even when the whole reply is not. Send a separate short packet after drain.
    if (Serial) { Serial.write(uint8_t('\n')); Serial.flush(); }
    usbFlushDone.store(true);
  }
}
struct Feedback {
  bool pending=false;
  float target=0, measured=0, output=0;
  uint32_t time=0;
};
Feedback feedback[4];

hostlocal::Parameters parameters() {
  const hostui::Snapshot value=panel->snapshot();
  return {{value.values[0],value.values[1],value.values[2]},value.step};
}
void observe() {
  if (session.observe(parameters())) { parametersPending=true; displayDirty=true; }
}
void queue(const char* format, ...) {
  va_list args; va_start(args,format);
  int length=vsnprintf(tx,sizeof(tx)-3,format,args);
  va_end(args);
  if (length<0 || size_t(length)>=sizeof(tx)-3) {
    std::strcpy(tx,"{\"type\":\"error\",\"code\":\"reply_too_long\"}");
    length=int(std::strlen(tx));
  }
  txLength=size_t(length); tx[txLength++]='\n';
  // End with a short native USB packet, including exactly-64-byte replies.
  if (txLength%64==0) tx[txLength++]='\n';
  tx[txLength]=0; txOffset=0;
}
void error(const char* code) { queue("{\"type\":\"error\",\"code\":\"%s\"}",code); }
#if defined(PID_LINK_ENABLED)
pidlink::Snapshot wirelessState() {
  const auto& p=session.parameters(); pidlink::Snapshot result;
  result.p=p.values[0]; result.i=p.values[1]; result.d=p.values[2]; result.step=p.step;
  result.revision=session.revision(); result.request=session.runRequest(); result.run=session.requestedRun();
  return result;
}
bool wirelessSynced(uint32_t now) {
  const auto& e=pidlink::radio.engine();
  return e.synced(now) && pidlink::equal(e.acknowledged(),wirelessState());
}
void updateLinkLed(uint32_t now) {
  // Keep it steady during parameter edits; require a fresh application ACK.
  const bool connected=pidlink::radio.ready() && pidlink::radio.engine().ackFresh(now);
  if (connected!=linkLedOn) {
    linkLedOn=connected;
    digitalWrite(kLinkLedPin,connected?LOW:HIGH);
  }
}
#endif
void stateReply(const char* type="state") {
  char values[4][16];
  const auto& p=session.parameters();
  for (uint8_t i=0;i<3;++i) hostui::formatValue(p.values[i],values[i],sizeof(values[i]));
  hostui::formatValue(p.step,values[3],sizeof(values[3]));
  const char* extra="";
#if defined(PID_LINK_ENABLED)
  char wireless[256],peerMac[18];pidlink::formatPeerMac(peers.mac(peers.active()),peerMac);
  const auto& e=pidlink::radio.engine();
  snprintf(wireless,sizeof(wireless),",\"wireless\":true,\"link_online\":%s,\"synced\":%s,\"ack_scope\":\"slave\",\"peer_name\":\"%s\",\"peer_mac\":\"%s\",\"peer_index\":%u,\"peer_count\":%u",
      e.online(millis())?"true":"false",wirelessSynced(millis())?"true":"false",pidlink::peerColorName(peers.active()),peerMac,unsigned(peers.index()),unsigned(peers.count()));
  extra=wireless;
#endif
  queue("{\"type\":\"%s\",\"p\":%s,\"i\":%s,\"d\":%s,\"step\":%s,\"revision\":%lu,"
        "\"dirty\":%s,\"saved\":%s,\"storage\":%s,\"run_request\":%s,\"request_id\":%lu,\"ack\":%s,\"stream\":%s%s}",
        type,values[0],values[1],values[2],values[3],static_cast<unsigned long>(session.revision()),
        session.dirty()?"true":"false",session.saved()?"true":"false",storageReady?"true":"false",
        session.requestedRun()?"true":"false",static_cast<unsigned long>(session.runRequest()),
        session.runConfirmed()?"true":"false",streaming?"true":"false",extra);
}
#if defined(PID_LINK_ENABLED)
void linkReply() {
  const auto& r=pidlink::radio; const auto& e=r.engine(); const uint32_t now=millis();
  char peerMac[18];pidlink::formatPeerMac(peers.mac(peers.active()),peerMac);
  queue("{\"type\":\"link\",\"enabled\":%s,\"ready\":%s,\"online\":%s,\"synced\":%s,\"encrypted\":true,"
        "\"channel\":1,\"session\":%lu,\"peer_session\":%lu,\"rx_age_ms\":%lu,\"sent\":%lu,\"received\":%lu,"
        "\"retries\":%lu,\"timeouts\":%lu,\"rejected\":%lu,\"dropped\":%lu,\"delivery_failures\":%lu,"
        "\"radio_error\":%d,\"link_led_pin\":38,\"link_led_on\":%s,\"ack_scope\":\"slave\",\"target_connected\":false,\"peer_name\":\"%s\",\"peer_mac\":\"%s\",\"peer_index\":%u,\"peer_count\":%u}",
      r.enabled()?"true":"false",r.ready()?"true":"false",e.online(now)?"true":"false",wirelessSynced(now)?"true":"false",
      static_cast<unsigned long>(e.session()),static_cast<unsigned long>(e.peerSession()),
      static_cast<unsigned long>(e.lastRxAge(now)),static_cast<unsigned long>(r.sent()),
      static_cast<unsigned long>(r.received()),static_cast<unsigned long>(e.retries()),
      static_cast<unsigned long>(e.timeouts()),static_cast<unsigned long>(r.rejected()),
      static_cast<unsigned long>(r.dropped()),static_cast<unsigned long>(r.deliveryFailures()),int(r.error()),
      linkLedOn?"true":"false",pidlink::peerColorName(peers.active()),peerMac,unsigned(peers.index()),unsigned(peers.count()));
}
#endif
void showToast(const char* value,uint32_t now) { toast=value; toastAt=now; displayDirty=true; }
#if defined(PID_LINK_ENABLED)
char peerToast[24]={};
void peerFeedback(uint32_t now){snprintf(peerToast,sizeof(peerToast),"%s %u/%u",pidlink::peerColorName(peers.active()),unsigned(peers.index()),unsigned(peers.count()));showToast(peerToast,now);}
bool storePeers(const pidlink::PeerCatalog& value){uint8_t bytes[pidlink::kPeerCatalogBytes];return peerStorageReady&&value.encode(bytes,sizeof(bytes))&&peerStorage.putBytes("catalog",bytes,sizeof(bytes))==sizeof(bytes);}
bool applyPeers(const pidlink::PeerCatalog& candidate,uint32_t now){
  uint8_t oldBytes[pidlink::kPeerCatalogBytes],newBytes[pidlink::kPeerCatalogBytes];peers.encode(oldBytes,sizeof(oldBytes));candidate.encode(newBytes,sizeof(newBytes));
  if(peerConfigurationOk&&!std::memcmp(oldBytes,newBytes,sizeof(oldBytes))){peerFeedback(now);return true;}
#if defined(PID_FLASH_ENABLED)
  if(!pidflash::hostPeerSwitchAllowed(now)){peerFailure="peer_busy";showToast("CLOSE UART/FW",now);return false;}
#endif
  const auto& e=pidlink::radio.engine();
  if(session.requestedRun()||(e.online(now)&&session.runRequest()&&!session.runConfirmed())){peerFailure="stop_before_switch";showToast("STOP FIRST",now);return false;}
  if(!storePeers(candidate)){peerFailure="peer_storage_failed";showToast("PEER SAVE FAIL",now);return false;}
  auto& r=pidlink::radio;
  const bool changed=std::memcmp(peers.mac(peers.active()),candidate.mac(candidate.active()),6)!=0;
  if(!r.configurePeer(candidate.mac(candidate.active()))||(!r.enabled()&&!r.begin(pidlink::Role::Host))){
    storePeers(peers);r.configurePeer(peers.mac(peers.active()));peerFailure="peer_radio_failed";showToast("PEER LINK FAIL",now);return false;
  }
  peers=candidate;peerConfigurationOk=true;
  if(changed){++peerSwitches;session.invalidateRunAcknowledgement();previouslyHealthy=false;peerChanges=0;
#if defined(PID_FLASH_ENABLED)
    pidflash::hostPeerChanged();
#endif
  }
  linkPending=true;linkSignature=255;displayDirty=true;peerFeedback(now);return true;
}
void peersReply(){
  char entries[768]={};size_t used=0;
  for(unsigned i=0;i<pidlink::kPeerColors;++i)if(peers.has(uint8_t(i))){char mac[18];pidlink::formatPeerMac(peers.mac(uint8_t(i)),mac);
    const int n=snprintf(entries+used,sizeof(entries)-used,"%s{\"name\":\"%s\",\"mac\":\"%s\",\"selected\":%s}",used?",":"",pidlink::peerColorName(uint8_t(i)),mac,i==peers.active()?"true":"false");
    if(n<0||size_t(n)>=sizeof(entries)-used){error("reply_too_long");return;}used+=size_t(n);}
  queue("{\"type\":\"peers\",\"active\":\"%s\",\"index\":%u,\"count\":%u,\"storage\":%s,\"valid\":%s,\"double_clicks\":%lu,\"switches\":%lu,\"peers\":[%s]}",
    pidlink::peerColorName(peers.active()),unsigned(peers.index()),unsigned(peers.count()),peerStorageReady?"true":"false",peerConfigurationOk?"true":"false",(unsigned long)peerDoubleClicks,(unsigned long)peerSwitches,entries);
}
bool peerCommand(const char* text,uint32_t now){
  if(std::strncmp(text,"peer",4)||(text[4]&&text[4]!=' '&&text[4]!='\t'))return false;
  char buffer[128];if(std::strlen(text)>=sizeof(buffer)){error("invalid_peer_command");return true;}std::strcpy(buffer,text);
  const char* words[5];unsigned count=0;char* save=nullptr;
  for(char* word=strtok_r(buffer," \t",&save);word;word=strtok_r(nullptr," \t",&save)){if(count==5){error("invalid_peer_command");return true;}words[count++]=word;}
  if(count==1||(count==2&&!std::strcmp(words[1],"list"))){peersReply();return true;}
  if(count==2&&!std::strcmp(words[1],"next")){auto candidate=peers;candidate.select(candidate.next());if(applyPeers(candidate,now))peersReply();else error(peerFailure);return true;}
  pidlink::PeerCatalog candidate=peers;bool valid=false;
  if(count==2&&!std::strcmp(words[1],"reset")){candidate.defaults();valid=true;}
  if(count==3||count==4){const int color=pidlink::peerColor(words[2]);
    if(color>=0&&count==3&&!std::strcmp(words[1],"select"))valid=candidate.select(uint8_t(color));
    else if(color>=0&&count==3&&!std::strcmp(words[1],"remove"))valid=candidate.remove(uint8_t(color));
    else if(color>=0&&count==4&&!std::strcmp(words[1],"add")){uint8_t mac[6];valid=pidlink::parsePeerMac(words[3],mac)&&std::memcmp(mac,pidlink::radio.localAddress(),6)&&candidate.put(uint8_t(color),mac);}
  }
  if(!valid)error("invalid_or_conflicting_peer");else if(!applyPeers(candidate,now))error(peerFailure);else peersReply();return true;
}
#endif
bool readProfile(hostlocal::Parameters& value) {
  uint8_t bytes[hostlocal::kProfileSize];
  return storageReady && storage.getBytesLength("profile")==sizeof(bytes) &&
      storage.getBytes("profile",bytes,sizeof(bytes))==sizeof(bytes) &&
      hostlocal::decodeProfile(bytes,sizeof(bytes),value);
}
bool saveProfile(uint32_t now) {
  observe();
  uint8_t bytes[hostlocal::kProfileSize];
  hostlocal::encodeProfile(session.parameters(),bytes);
  savedResult=storageReady && storage.putBytes("profile",bytes,sizeof(bytes))==sizeof(bytes);
  if (savedResult) session.markSaved();
  showToast(savedResult?"SAVED":"SAVE FAIL",now);
  savePending=true;
  return savedResult;
}
bool requestRun(bool value) {
#if defined(PID_FLASH_ENABLED)
  if(value&&pidflash::hostLocked()){showToast("FW BUSY",millis());return false;}
#endif
#if defined(PID_LINK_ENABLED)
  if (value && !pidlink::radio.engine().ackFresh(millis())) { showToast("NO LINK",millis()); return false; }
#endif
  if (session.requestRun(value)) { runPending=true; displayDirty=true; }
  return true;
}
void updateHeader(uint32_t now) {
  char text[32];
  const char* mode=session.runRequest()==0 ? "LOCAL" :
      session.runConfirmed() ? (session.requestedRun()?"RUN ACK":"STOP ACK") :
                               (session.requestedRun()?"RUN?":"STOP?");
#if defined(PID_LINK_ENABLED)
  const auto& e=pidlink::radio.engine();
  if (!pidlink::radio.ready()) mode="W ERROR";
  else if (!e.online(now)) mode="W OFFLINE";
  else if (!e.synced(now)) mode="W SYNC";
  else mode=session.runRequest()==0?"W LINK":session.requestedRun()?"W RUN ACK":"W STOP ACK";
#endif
#if defined(PID_LINK_ENABLED)
  if(!peerConfigurationOk)snprintf(text,sizeof(text),"PEER CONFIG ERROR");
  else if(*toast&&uint32_t(now-toastAt)<1500){if(toast==peerToast)snprintf(text,sizeof(text),"%s",peerToast);else snprintf(text,sizeof(text),"%s %s",pidlink::peerColorName(peers.active()),toast);}
  else snprintf(text,sizeof(text),"%s %u/%u %s %s",pidlink::peerColorName(peers.active()),unsigned(peers.index()),unsigned(peers.count()),mode,session.dirty()?"*":"S");
#else
  if (*toast && uint32_t(now-toastAt)<1500) snprintf(text,sizeof(text),"%s",toast);
  else snprintf(text,sizeof(text),"%s %s",mode,session.dirty()?"*":"SAVED");
#endif
  if (std::strcmp(text,previousHeader)) {
    std::strcpy(previousHeader,text);
    hostoled::publishHeader(text);
  }
}
void pump() {
  if (usbFlushRequested) {
    if (usbFlushDone.exchange(false)) { usbFlushRequested=false; txLength=txOffset=0; }
    return;
  }
  if (!txLength || !Serial) return;
  // Exactly one task owns the complete JSON write and its flush. Main-loop
  // code keeps the buffer immutable until completion and continues animation.
  if (usbFlushTask) { usbFlushRequested=true; xTaskNotifyGive(usbFlushTask); }
  else { Serial.write(reinterpret_cast<const uint8_t*>(tx),txLength); Serial.flush(); txLength=txOffset=0; }
}
void prepareEvent(uint32_t now) {
#if defined(PID_FLASH_ENABLED)
  if(!txLength&&Serial) {
    char reply[640];
    if(pidflash::takeHostReply(reply,sizeof(reply))){queue("%s",reply);return;}
  }
#endif
  if (txLength || !streaming || !Serial) return;
  if (runPending) {
    queue("{\"type\":\"run_request\",\"request_id\":%lu,\"run\":%s,\"ack\":%s}",
        static_cast<unsigned long>(session.runRequest()),session.requestedRun()?"true":"false",
        session.runConfirmed()?"true":"false");
    runPending=false;
  } else if (savePending) {
    queue("{\"type\":\"saved\",\"ok\":%s,\"revision\":%lu}",savedResult?"true":"false",
          static_cast<unsigned long>(session.revision()));
    savePending=false;
  } else if (parametersPending && uint32_t(now-lastParameters)>=100) {
    stateReply("params"); parametersPending=false; lastParameters=now;
#if defined(PID_LINK_ENABLED)
  } else if (linkPending && uint32_t(now-lastLinkReport)>=100) {
    linkReply(); linkPending=false; lastLinkReport=now;
#endif
  } else {
    for (uint8_t i=0;i<4;++i) {
      Feedback& f=feedback[i];
      if (!f.pending) continue;
      queue("{\"type\":\"telemetry\",\"source\":\"usb\",\"channel\":%u,\"time_ms\":%lu,"
            "\"target\":%.7g,\"measured\":%.7g,\"error\":%.7g,\"output\":%.7g}",unsigned(i),
            static_cast<unsigned long>(f.time),double(f.target),double(f.measured),
            double(f.target-f.measured),double(f.output));
      f.pending=false; break;
    }
  }
}
}

void begin(hostui::Controller& ui) {
  panel=&ui;
  // HWCDC touches its FIFO from both write/flush and its ISR without a shared
  // cross-core lock. Keep the writer on the core where Serial.begin installed it.
  xTaskCreatePinnedToCore(usbFlushWorker,"host_usb_flush",2048,nullptr,1,&usbFlushTask,xPortGetCoreID());
  storageReady=storage.begin("pid-host",false);
  hostlocal::Parameters stored;
  const bool loaded=readProfile(stored);
  if (loaded) panel->setParameters(stored.values[0],stored.values[1],stored.values[2],stored.step);
  session.begin(parameters(),loaded);
  pinMode(hostlocal::kRunPin,INPUT_PULLUP); pinMode(hostlocal::kSavePin,INPUT_PULLUP);
  runButton.begin(digitalRead(hostlocal::kRunPin)==LOW,millis());
  saveButton.begin(digitalRead(hostlocal::kSavePin)==LOW,millis());
#if defined(PID_LINK_ENABLED)
  digitalWrite(kLinkLedPin,HIGH);
  pinMode(kLinkLedPin,OUTPUT);
  linkLedOn=false;
  peers.defaults();peerStorageReady=peerStorage.begin("pid-peers",false);
  if(peerStorageReady&&peerStorage.isKey("catalog")){uint8_t bytes[pidlink::kPeerCatalogBytes];
    peerConfigurationOk=peerStorage.getBytesLength("catalog")==sizeof(bytes)&&peerStorage.getBytes("catalog",bytes,sizeof(bytes))==sizeof(bytes)&&peers.decode(bytes,sizeof(bytes));}
  if(peerConfigurationOk){pidlink::radio.configurePeer(peers.mac(peers.active()));pidlink::radio.begin(pidlink::Role::Host);}
#if defined(PID_FLASH_ENABLED)
  pidflash::beginHost();
#endif
#endif
  updateHeader(millis());
}
void poll(uint32_t now) {
  observe();
#if defined(PID_LINK_ENABLED)
  auto& r=pidlink::radio;
  r.receive(now);
#if defined(PID_FLASH_ENABLED)
  pidflash::pollHost(now);
#endif
  const auto& e=r.engine();
  const bool healthy=e.ackFresh(now);
  const bool restarted=peerChanges!=0 && peerChanges!=e.peerChanges();
  if (((previouslyHealthy && !healthy) || restarted) && session.requestedRun()) requestRun(false);
  if (!healthy) session.invalidateRunAcknowledgement();
  previouslyHealthy=healthy; peerChanges=e.peerChanges();
#endif
  if (runButton.update(digitalRead(hostlocal::kRunPin)==LOW,now)) requestRun(!session.requestedRun());
  if (saveButton.update(digitalRead(hostlocal::kSavePin)==LOW,now)) saveProfile(now);
#if defined(PID_LINK_ENABLED)
  r.setDesired(wirelessState());
  if (e.synced(now) && session.runRequest()!=0) session.acknowledgeRun(e.acknowledged().request,e.acknowledged().run);
  r.transmit(now);
  updateLinkLed(now);
  const uint8_t signature=unsigned(e.online(now)) | (unsigned(e.synced(now))<<1) | (unsigned(r.ready())<<2);
  if (signature!=linkSignature) { linkSignature=signature; linkPending=true; }
#endif
  updateHeader(now);
  prepareEvent(now);
  pump();
}
bool handleCommand(const char* text,uint32_t now) {
#if defined(PID_LINK_ENABLED)
  if(peerCommand(text,now))return true;
#endif
#if defined(PID_FLASH_ENABLED)
  if(pidflash::hostCommand(text)) {
    if(pidflash::hostLocked())requestRun(false);
    return true;
  }
#endif
#if defined(PID_LINK_ENABLED)
  if (!std::strcmp(text,"link")) { linkReply(); return true; }
#endif
  hostlocal::Command command;
  const auto parsed=hostlocal::parse(text,command);
  if (parsed==hostlocal::Parsed::Unknown) return false;
  if (parsed==hostlocal::Parsed::Invalid) { error("invalid_arguments"); return true; }
  observe();
  switch (command.kind) {
    case hostlocal::Kind::Get: stateReply(); break;
    case hostlocal::Kind::Pid: {
      const auto& p=command.parameters;
      panel->setParameters(p.values[0],p.values[1],p.values[2],session.parameters().step);
      observe(); stateReply(); break;
    }
    case hostlocal::Kind::Step: {
      const auto& p=session.parameters();
      panel->setParameters(p.values[0],p.values[1],p.values[2],command.parameters.step);
      observe(); stateReply(); break;
    }
    case hostlocal::Kind::Run:
#if defined(PID_FLASH_ENABLED)
      if(pidflash::hostLocked()){error("firmware_busy");break;}
#endif
      if (requestRun(true)) stateReply(); else error("slave_not_ready");
      break;
    case hostlocal::Kind::Stop: requestRun(false); stateReply(); break;
    case hostlocal::Kind::Save:
      if (saveProfile(now)) stateReply(); else error("storage_write_failed");
      break;
    case hostlocal::Kind::Load: {
      hostlocal::Parameters p;
      if (!readProfile(p)) { error("no_valid_profile"); break; }
      panel->setParameters(p.values[0],p.values[1],p.values[2],p.step);
      observe(); session.markSaved(); showToast("LOADED",now); stateReply(); break;
    }
    case hostlocal::Kind::Forget:
      if (!storageReady || (storage.isKey("profile") && !storage.remove("profile"))) { error("storage_erase_failed"); break; }
      session.forget(); showToast("UNSAVED",now); stateReply(); break;
    case hostlocal::Kind::Stream:
      streaming=command.flag;
      if (streaming) { parametersPending=true; runPending=session.runRequest()!=0; }
      stateReply(); break;
    case hostlocal::Kind::Feedback:
      feedback[command.channel].target=command.target;
      feedback[command.channel].measured=command.measured;
      feedback[command.channel].output=command.output;
      feedback[command.channel].time=now;
      feedback[command.channel].pending=true;
      queue("{\"type\":\"ok\",\"command\":\"feedback\",\"channel\":%u}",unsigned(command.channel)); break;
    case hostlocal::Kind::AckRun:
#if defined(PID_LINK_ENABLED)
      error("wireless_ack_required"); break;
#else
      if (!session.acknowledgeRun(command.request,command.flag)) { error("stale_or_mismatched_ack"); break; }
      displayDirty=true; stateReply(); break;
#endif
    default: error("unknown_command");
  }
  return true;
}
bool canAcceptCommand() {
  return txLength==0
#if defined(PID_FLASH_ENABLED)
      &&!pidflash::hostPending()
#endif
      ;
}
bool takeDisplayDirty() { const bool result=displayDirty; displayDirty=false; return result; }
Status status() {
  observe();
  return {storageReady,session.saved(),session.dirty(),session.requestedRun(),session.runConfirmed(),
          session.runRequest(),session.revision()};
}
void nextPeer(uint32_t now){
#if defined(PID_LINK_ENABLED)
  ++peerDoubleClicks;auto candidate=peers;candidate.select(candidate.next());if(!applyPeers(candidate,now))showToast(!std::strcmp(peerFailure,"stop_before_switch")?"STOP FIRST":"PEER BUSY",now);
#else
  (void)now;
#endif
}
}  // namespace hostapp
