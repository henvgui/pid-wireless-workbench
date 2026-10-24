#ifndef MOTOR_PROTOCOL_H
#define MOTOR_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define MOTOR_COMMAND_SIZE 56U
#define MOTOR_STATUS_SIZE 96U
#define MOTOR_APP_ID 0x3144504DU
#define MOTOR_FAULT_LINK 1U
#define MOTOR_FAULT_STALL_L 2U
#define MOTOR_FAULT_STALL_R 4U
#define MOTOR_FAULT_WAIT_STOP 8U
#define MOTOR_FAULT_TIMING 16U

/* Gains, RPM and PWM duty use hundredths. STEP belongs to the host UI. */
typedef struct {
  uint32_t session, sequence, revision, request;
  uint32_t p100, i100, d100, target_l100, target_r100, run, limit100;
} MotorCommand;
typedef struct {
  uint32_t app, session, sequence, revision, request, tick_ms, running, fault;
  uint32_t target_l100, target_r100, speed_l100, speed_r100, duty_l100, duty_r100;
  uint32_t p100, i100, d100, encoder_l, encoder_r, command_age_ms, limit100;
} MotorStatus;
typedef struct { uint16_t size; uint8_t bytes[MOTOR_STATUS_SIZE]; } MotorParser;
typedef char MotorCommandLayout[(sizeof(MotorCommand)==44U)?1:-1];
typedef char MotorStatusLayout[(sizeof(MotorStatus)==84U)?1:-1];

static inline uint32_t MotorProto_Get32(const uint8_t *p) {
  return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static inline void MotorProto_Put32(uint8_t *p,uint32_t v) {
  unsigned i;for(i=0;i<4;i++)p[i]=(uint8_t)(v>>(8U*i));
}
static inline uint32_t MotorProto_Crc(const uint8_t *p,size_t n) {
  uint32_t crc=~0U;unsigned i;
  while(n--){crc^=*p++;for(i=0;i<8;i++)crc=(crc>>1)^((crc&1U)?0xEDB88320U:0U);}
  return ~crc;
}
static inline void MotorProto_Encode(const void *body,uint8_t kind,uint16_t size,uint8_t *out) {
  uint32_t words[21];unsigned i,count=(size-12U)/4U;
  memset(out,0,size);out[0]='M';out[1]='P';out[2]=1;out[3]=kind;
  out[4]=(uint8_t)size;out[5]=(uint8_t)(size>>8);
  memcpy(words,body,count*4U);
  for(i=0;i<count;i++)MotorProto_Put32(out+8U+4U*i,words[i]);
  MotorProto_Put32(out+size-4U,MotorProto_Crc(out,size-4U));
}
static inline int MotorProto_Valid(const uint8_t *p,size_t n,uint8_t kind) {
  return p&&n>=12&&p[0]=='M'&&p[1]=='P'&&p[2]==1&&p[3]==kind&&
    ((size_t)p[4]|((size_t)p[5]<<8))==n&&!p[6]&&!p[7]&&
    MotorProto_Get32(p+n-4U)==MotorProto_Crc(p,n-4U);
}
static inline int MotorProto_CommandValid(const MotorCommand *c) {
  return c->session&&c->sequence&&c->p100<=999999U&&c->i100<=999999U&&c->d100<=999999U&&
    c->target_l100<=40000U&&c->target_r100<=40000U&&c->run<=1U&&
    (!c->run||c->request)&&c->limit100<=10000U;
}
static inline int MotorProto_DecodeCommand(const uint8_t *p,size_t n,MotorCommand *out) {
  uint32_t words[11];unsigned i;
  if(n!=MOTOR_COMMAND_SIZE||!MotorProto_Valid(p,n,1))return 0;
  for(i=0;i<11;i++)words[i]=MotorProto_Get32(p+8U+4U*i);
  memcpy(out,words,sizeof(*out));return MotorProto_CommandValid(out);
}
static inline int MotorProto_DecodeStatus(const uint8_t *p,size_t n,MotorStatus *out) {
  uint32_t words[21];unsigned i;
  if(n!=MOTOR_STATUS_SIZE||!MotorProto_Valid(p,n,2))return 0;
  for(i=0;i<21;i++)words[i]=MotorProto_Get32(p+8U+4U*i);
  memcpy(out,words,sizeof(*out));return out->app==MOTOR_APP_ID;
}
static inline size_t MotorProto_Feed(MotorParser *parser,uint8_t byte,uint8_t *out) {
  uint16_t n;
  if(parser->size==MOTOR_STATUS_SIZE){memmove(parser->bytes,parser->bytes+1,--parser->size);}
  parser->bytes[parser->size++]=byte;
  for(;;) {
    if(parser->size<2)return 0;
    if(parser->bytes[0]!='M'||parser->bytes[1]!='P')goto discard;
    if(parser->size<8)return 0;
    n=(uint16_t)(parser->bytes[4]|((uint16_t)parser->bytes[5]<<8));
    if(parser->bytes[2]!=1||parser->bytes[6]||parser->bytes[7]||
       !((parser->bytes[3]==1&&n==MOTOR_COMMAND_SIZE)||(parser->bytes[3]==2&&n==MOTOR_STATUS_SIZE)))goto discard;
    if(parser->size<n)return 0;
    if(!MotorProto_Valid(parser->bytes,n,parser->bytes[3]))goto discard;
    memcpy(out,parser->bytes,n);parser->size=(uint16_t)(parser->size-n);
    memmove(parser->bytes,parser->bytes+n,parser->size);return n;
discard:
    memmove(parser->bytes,parser->bytes+1,--parser->size);
  }
}
#endif
