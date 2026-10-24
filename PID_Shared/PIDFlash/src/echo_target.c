#include "echo_target.h"
static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void put32(uint8_t* p,uint32_t value) { unsigned i; for(i=0;i<4;++i)p[i]=(uint8_t)(value>>(i*8)); }
static void copy(uint8_t* out,const uint8_t* in,size_t n) { while(n--)*out++=*in++; }
static int equal(const uint8_t* a,const uint8_t* b,size_t n) { while(n--)if(*a++!=*b++)return 0; return 1; }
int echo_marker_valid(const uint8_t* bytes,size_t size) {
    return bytes&&size==8&&(equal(bytes,(const uint8_t*)"F103ECHO",8)||equal(bytes,(const uint8_t*)"F407ECHO",8)||equal(bytes,(const uint8_t*)"F407VE01",8));
}
static uint32_t crc_update(uint32_t crc,const uint8_t* data,size_t n) {
    while(n--) { unsigned i; crc^=*data++;for(i=0;i<8;++i)crc=(crc>>1)^((crc&1)?0xedb88320u:0); }
    return crc;
}
uint32_t echo_crc32(const uint8_t* data,size_t n) { return ~crc_update(~0u,data,n); }
static int valid(const uint8_t* p,size_t n) {
    size_t len;
    if(!p||n<32||n>160||p[0]!='P'||p[1]!='F'||p[2]!=1||p[26]||p[27])return 0;
    len=(size_t)p[24]|((size_t)p[25]<<8);
    return len<=128&&n==32+len&&get32(p+28+len)==echo_crc32(p,28+len)&&
        get32(p+4)&&get32(p+8)&&get32(p+12)&&get32(p+16);
}
static void consume(EchoParser* p,size_t n) {
    size_t i;p->size=(uint16_t)(p->size-n);for(i=0;i<p->size;++i)p->bytes[i]=p->bytes[i+n];
}
size_t echo_parser_feed(EchoParser* p,uint8_t byte,uint8_t* out) {
    if(p->size==160)consume(p,1);
    p->bytes[p->size++]=byte;
    while(p->size>=28) {
        size_t n=32u+p->bytes[24]+((size_t)p->bytes[25]<<8);
        if(p->bytes[0]!='P'||p->bytes[1]!='F'||p->bytes[2]!=1||p->bytes[26]||p->bytes[27]||n>160) {consume(p,1);continue;}
        if(p->size<n)return 0;
        if(!valid(p->bytes,n)){consume(p,1);continue;}
        copy(out,p->bytes,n);consume(p,n);return n;
    }
    return 0;
}
size_t echo_target_handle(EchoTarget* t,const uint8_t* p,size_t n,uint8_t* out) {
    uint32_t sender,peer,id,seq,offset;size_t len,reply_len=0;unsigned op;
    if(!valid(p,n))return 0;
    sender=get32(p+4);peer=get32(p+8);id=get32(p+12);seq=get32(p+16);offset=get32(p+20);len=n-32;op=p[3];
    if(t->request_size==n&&equal(t->request,p,n)){copy(out,t->reply,t->reply_size);return t->reply_size;}
    if(op==8) {
        if(len!=8||offset)return 0;
        /* Re-opening the same session under a different sequence cannot rewind it. */
        if(t->sender==sender&&t->peer==peer&&t->transfer==id)return 0;
        t->sender=sender;t->peer=peer;t->transfer=id;t->total=0;t->crc=~0u;t->frames=0;t->active=1;
        reply_len=8;
#if defined(PID_ECHO_F407VE)
        copy(out+28,(const uint8_t*)"F407VE01",8);
#elif defined(PID_ECHO_F407)
        copy(out+28,(const uint8_t*)"F407ECHO",8);
#else
        copy(out+28,(const uint8_t*)"F103ECHO",8);
#endif
    } else {
        if(!t->active||sender!=t->sender||peer!=t->peer||id!=t->transfer||(int32_t)(seq-t->sequence)<=0)return 0;
        if(offset!=t->total) {
            /* A PC retry may use a new sequence after its earlier ACK was lost. */
            if(op!=9||t->request[3]!=9||offset!=get32(t->request+20)||len!=t->request_size-32u||!equal(p+28,t->request+28,len))return 0;
            reply_len=len;copy(out+28,p+28,len);goto emit;
        }
        if(op==9) {
            if(!len||len>128||t->total>0x04000000u-len)return 0;
            t->crc=crc_update(t->crc,p+28,len);t->total+=(uint32_t)len;++t->frames;
            reply_len=len;copy(out+28,p+28,len);
        } else if(op==10) {
            if(len)return 0;
            reply_len=12;put32(out+28,t->total);put32(out+32,~t->crc);put32(out+36,t->frames);t->active=0;
        } else return 0;
    }
emit:
    copy(out,p,28);out[3]=11;put32(out+4,peer);put32(out+8,sender);
    out[24]=(uint8_t)reply_len;out[25]=0;
    put32(out+28+reply_len,echo_crc32(out,28+reply_len));
    t->sequence=seq;t->request_size=(uint16_t)n;t->reply_size=(uint16_t)(32+reply_len);
    copy(t->request,p,n);copy(t->reply,out,t->reply_size);return t->reply_size;
}
