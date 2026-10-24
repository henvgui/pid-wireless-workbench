#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Same 32+payload PF frame across ESP-NOW and UART. No heap allocation. */
typedef struct {
    uint32_t sender, peer, transfer, total, crc, frames, sequence;
    uint16_t request_size, reply_size;
    uint8_t active, request[160], reply[160];
} EchoTarget;
typedef struct { uint16_t size; uint8_t bytes[160]; } EchoParser;
uint32_t echo_crc32(const uint8_t* data, size_t size);
/* Known test applications; neither a radio ACK nor arbitrary UART data qualifies. */
int echo_marker_valid(const uint8_t* bytes, size_t size);
/* Produces only a CRC-checked complete frame; scans past corrupt/partial data. */
size_t echo_parser_feed(EchoParser* parser, uint8_t byte, uint8_t* output);
/* Rejects stale/conflicting/out-of-order requests. Duplicate replies are cached. */
size_t echo_target_handle(EchoTarget* target, const uint8_t* request, size_t size, uint8_t* reply);
#ifdef __cplusplus
}
#endif
