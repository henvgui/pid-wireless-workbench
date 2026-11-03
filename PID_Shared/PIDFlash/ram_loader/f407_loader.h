#pragma once
#include <stdint.h>
#define RAM_CODE 0x20000000u
#define RAM_DATA 0x20001000u
#define RAM_MAILBOX 0x20001800u
#define RAM_STACK 0x20002000u
#define RAM_BLOCK 1024u
#define RAM_MAGIC 0x50494431u
#define RAM_DONE 0x444f4e45u
#define RAM_NONCE_XOR 0xa59c37e1u
enum { RAM_PROBE=1, RAM_PROGRAM=2, RAM_CHECK=3 };
enum { RAM_OK=0, RAM_ARGUMENT=1, RAM_IDENTITY=2, RAM_CRC=3,
       RAM_CONFIG=4, RAM_BUSY_TIMEOUT=5, RAM_FLASH_ERROR=6 };
typedef struct {
  uint32_t magic,command,address,length,buffer,crc,nonce;
  uint32_t state,result,processed,returned_nonce,flash_status,flash_limit;
} RamMailbox;
#ifdef __cplusplus
extern "C" {
#endif
void ram_work(volatile RamMailbox* box);
#ifdef __cplusplus
}
#endif
