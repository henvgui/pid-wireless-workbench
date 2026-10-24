#include "motor_bridge.h"
#include "PIDLink.h"
#include "slave_target_pins.h"
#include <Arduino.h>
#include <HardwareSerial.h>
#include <cstring>

namespace pidflash { namespace motorbridge {
namespace {
HardwareSerial uart{1};
MotorParser parser={};
MotorStatus latest={};
bool enabled=false,received=false;
bool mustStop=true;
uint32_t lastSend=0,lastStatus=0,sequence=0,peer=0;
uint32_t leftTarget=10000,rightTarget=10000;
void sendState(bool run,uint32_t now) {
  const auto& e=pidlink::radio.engine();const auto& m=e.mirror();
  MotorCommand command={};uint8_t bytes[MOTOR_COMMAND_SIZE];
  command.session=e.session();if(++sequence==0)++sequence;command.sequence=sequence;
  command.revision=m.revision;command.request=m.request;
  command.p100=uint32_t(m.p);command.i100=uint32_t(m.i);command.d100=uint32_t(m.d);
  command.target_l100=leftTarget;command.target_r100=rightTarget;
  command.run=run?1:0;command.limit100=5000;
  MotorProto_Encode(&command,1,MOTOR_COMMAND_SIZE,bytes);
  uart.write(bytes,sizeof(bytes));lastSend=now;
}
}
bool owned(){return enabled;}
bool inhibited(){return mustStop;}
void pause(uint32_t) {
  if(!enabled)return;
  digitalWrite(47,HIGH);uart.end();
  pinMode(17,INPUT_PULLUP);pinMode(18,INPUT_PULLUP);
  pinMode(15,INPUT);pinMode(16,INPUT);pinMode(21,INPUT_PULLUP);
  enabled=false;parser.size=0;
}
bool targets(uint32_t left100,uint32_t right100) {
  if(left100>40000||right100>40000)return false;
  leftTarget=left100;rightTarget=right100;lastSend=0;return true;
}
bool status(MotorStatus& value,uint32_t& age,uint32_t now) {
  age=now-lastStatus;
  if(!received||age>500||latest.session!=pidlink::radio.engine().session())return false;
  value=latest;return true;
}
void poll(uint32_t now,bool available) {
  const auto& r=pidlink::radio;const auto& e=r.engine();
  const bool link=r.ready()&&e.mirrorValid()&&e.online(now)&&e.lastRxAge(now)<800;
  if(!available||!link) {
    if(enabled&&!link){mustStop=true;sendState(false,now);}
    pause(now);return;
  }
  if(peer!=e.peerSession()){peer=e.peerSession();received=false;mustStop=true;}
  if(!e.mirror().run)mustStop=false;
  if(!enabled) {
    digitalWrite(47,HIGH);pinMode(47,OUTPUT);
    digitalWrite(21,LOW);pinMode(21,OUTPUT);
    pinMode(slavepins::kSwdClock,INPUT);pinMode(slavepins::kSwdData,INPUT);pinMode(slavepins::kSwo,INPUT);
    digitalWrite(15,HIGH);pinMode(15,OUTPUT);digitalWrite(16,LOW);pinMode(16,OUTPUT);
    uart.setRxBufferSize(1024);uart.begin(115200,SERIAL_8N1,18,17);
    parser.size=0;enabled=true;digitalWrite(47,LOW);lastSend=now-100;
  }
  for(unsigned i=0;i<512&&uart.available();i++) {
    uint8_t bytes[MOTOR_STATUS_SIZE];const size_t n=MotorProto_Feed(&parser,uint8_t(uart.read()),bytes);
    MotorStatus s;
    if(n==MOTOR_STATUS_SIZE&&MotorProto_DecodeStatus(bytes,n,&s)&&s.session==e.session()&&
       (!received||int32_t(s.sequence-latest.sequence)>=0)) {
      latest=s;lastStatus=now;received=true;
    }
  }
  if(uint32_t(now-lastSend)>=100)sendState(e.mirror().run&&!mustStop,now);
}
} }
