#include "flash_core.h"
#include "echo_target.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>
using namespace pidflash;
int main() {
    EchoTarget target={};EchoParser parser={};
    Packet p;p.op=Op::EchoOpen;p.sender=123;p.peer=456;p.transfer=789;p.sequence=1;p.length=8;
    uint8_t bytes[kMaxPacket],out[kMaxPacket],parsed[kMaxPacket];
    size_t n=encode(p,bytes),r=echo_target_handle(&target,bytes,n,out);Packet response;
    assert(r&&decode(out,r,response)&&response.op==Op::EchoReply&&response.length==8);
    assert(echo_marker_valid(response.data,response.length));
#if defined(PID_ECHO_F407VE)
    assert(!std::memcmp(response.data,"F407VE01",8));
#elif defined(PID_ECHO_F407)
    assert(!std::memcmp(response.data,"F407ECHO",8));
#else
    assert(!std::memcmp(response.data,"F103ECHO",8));
#endif
    assert(echo_marker_valid(reinterpret_cast<const uint8_t*>("F103ECHO"),8));
    assert(echo_marker_valid(reinterpret_cast<const uint8_t*>("F407ECHO"),8));
    assert(echo_marker_valid(reinterpret_cast<const uint8_t*>("F407VE01"),8));
    assert(!echo_marker_valid(reinterpret_cast<const uint8_t*>("BAD!ECHO"),8));
    assert(!echo_marker_valid(response.data,7));
    assert(echo_target_handle(&target,bytes,n,parsed)==r&&!std::memcmp(parsed,out,r));
    const uint8_t check[]={'1','2','3','4','5','6','7','8','9'};assert(echo_crc32(check,9)==0xcbf43926);
    std::mt19937 rng(678);std::vector<uint8_t> text(131091),returned;unsigned dropped=0,corrupted=0,frames=0;
    for(auto& b:text)b=uint8_t(rng());
    for(size_t offset=0;offset<text.size();offset+=kLegacyChunk) {
        p.op=Op::EchoData;p.offset=uint32_t(offset);++p.sequence;p.length=uint16_t(std::min(kLegacyChunk,text.size()-offset));
        std::memcpy(p.data,text.data()+offset,p.length);n=encode(p,bytes);
        for(;;) {
            if(rng()%5==0){++dropped;continue;}
            if(rng()%7==0){bytes[n-1]^=1;assert(!echo_target_handle(&target,bytes,n,out));bytes[n-1]^=1;++corrupted;continue;}
            r=echo_target_handle(&target,bytes,n,out);assert(r);
            if(rng()%5==0){++dropped;continue;}break;
        }
        assert(decode(out,r,response)&&response.offset==offset&&response.length==p.length);
        returned.insert(returned.end(),response.data,response.data+response.length);++frames;
        // New sequence, same data offset: ACK loss must not duplicate the stream.
        ++p.sequence;n=encode(p,bytes);assert(echo_target_handle(&target,bytes,n,out));
        assert(target.total==offset+p.length&&target.frames==frames);
        p.data[0]^=1;++p.sequence;n=encode(p,bytes);assert(!echo_target_handle(&target,bytes,n,out));p.data[0]^=1;
    }
    assert(returned==text&&dropped&&corrupted);
    p.op=Op::EchoClose;p.offset=uint32_t(text.size());++p.sequence;p.length=0;n=encode(p,bytes);
    r=echo_target_handle(&target,bytes,n,out);assert(r&&decode(out,r,response)&&response.length==12);
    assert(get32(response.data)==text.size()&&get32(response.data+4)==crc32(text.data(),text.size())&&get32(response.data+8)==frames);
    assert(echo_target_handle(&target,bytes,n,parsed)==r&&!std::memcmp(parsed,out,r));
    // A closed stream cannot accept late data, and a partial/corrupt UART frame
    // must not prevent resynchronisation with the following valid frame.
    p.op=Op::EchoData;p.length=1;++p.sequence;n=encode(p,bytes);assert(!echo_target_handle(&target,bytes,n,out));
    p.op=Op::EchoOpen;p.transfer=987;p.offset=0;p.length=8;++p.sequence;n=encode(p,bytes);
    unsigned complete=0;
    for(unsigned i=0;i<19;++i)assert(!echo_parser_feed(&parser,bytes[i],parsed));
    for(size_t i=0;i<n;++i)if(echo_parser_feed(&parser,bytes[i],parsed))++complete;
    if(!complete)for(size_t i=0;i<n;++i)if(echo_parser_feed(&parser,bytes[i],parsed))++complete;
    assert(complete>=1&&decode(parsed,n,response));
    // All bit corruptions are refused before updating the receiver.
    for(size_t i=0;i<n;++i){bytes[i]^=1;assert(!echo_target_handle(&target,bytes,n,out));bytes[i]^=1;}
    std::cout<<"PASS: end-to-end frame CRC, 131091 binary bytes, loss/corruption, exact and re-sequenced duplicates, unique-frame/stream CRC, stale/closed/conflicting data, UART parser resynchronisation.\n";
}
