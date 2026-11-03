/* Runs entirely in main SRAM. No libc, Flash literals, interrupts or DMA.
 * Host performs identity/capacity checks, erase/unlock, lock and full readback.
 * This algorithm never erases or changes option bytes. */
#include "f407_loader.h"
#ifdef RAM_LOADER_NATIVE
extern uint32_t ram_read32(uint32_t address);
extern uint16_t ram_read16(uint32_t address);
extern void ram_write32(uint32_t address,uint32_t value);
#define READ32(a) ram_read32(a)
#define READ16(a) ram_read16(a)
#define WRITE32(a,v) ram_write32(a,v)
#else
#define READ32(a) (*(volatile uint32_t*)(a))
#define READ16(a) (*(volatile uint16_t*)(a))
#define WRITE32(a,v) (*(volatile uint32_t*)(a)=(v))
#endif
#define FLASH_SR 0x40023c0cu
#define FLASH_CR 0x40023c10u
static uint32_t buffer_crc(uint32_t address,uint32_t length) {
  uint32_t crc=0xffffffffu;
  for(uint32_t offset=0;offset<length;offset+=4) {
    uint32_t word=READ32(address+offset);
    for(unsigned byte=0;byte<4;++byte) {
      crc^=word&255u;word>>=8;
      for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));
    }
  }
  return crc^0xffffffffu;
}
static uint32_t ready(volatile RamMailbox* box) {
  /* Bounded even if the controller never completes. Host has a second timeout. */
  for(uint32_t remaining=2000000;remaining;--remaining) {
    uint32_t status=READ32(FLASH_SR);box->flash_status=status;
    if(!(status&0x10000u))return status&0xf2u?RAM_FLASH_ERROR:RAM_OK;
    if(!(remaining&1023u))WRITE32(0x40003000u,0xaaaau); /* Feed, never start IWDG. */
  }
  return RAM_BUSY_TIMEOUT;
}
void ram_work(volatile RamMailbox* box) {
  box->state=0;box->processed=0;box->returned_nonce=0;box->flash_status=0;
  uint32_t result=RAM_ARGUMENT;
  if(box->magic!=RAM_MAGIC||!box->nonce)goto done;
  if(box->command!=RAM_PROBE&&box->command!=RAM_CHECK&&box->command!=RAM_PROGRAM)goto done;
  if((READ32(0xe0042000u)&0xfffu)!=0x413u) {result=RAM_IDENTITY;goto done;}
  {uint32_t kb=READ16(0x1fff7a22u);
   if((kb!=512&&kb!=1024)||box->flash_limit!=0x08000000u+kb*1024u) {result=RAM_IDENTITY;goto done;}}
  if(box->command==RAM_PROBE){result=RAM_OK;goto done;}
  if(box->buffer!=RAM_DATA||!box->length||box->length>RAM_BLOCK||(box->length&3u))goto done;
  if(buffer_crc(box->buffer,box->length)!=box->crc){result=RAM_CRC;goto done;}
  if(box->command==RAM_CHECK){result=RAM_OK;goto done;}
  if((box->address&3u)||box->address<0x08000000u||box->address>=box->flash_limit||
      box->length>box->flash_limit-box->address)goto done;
  /* PG=1, PSIZE=x32, LOCK/SER/MER/STRT=0. Voltage must be 2.7..3.6 V. */
  if((READ32(FLASH_CR)&0x80010307u)!=0x201u){result=RAM_CONFIG;goto done;}
  result=ready(box);if(result)goto done;
  for(uint32_t offset=0;offset<box->length;offset+=4) {
    WRITE32(0x40003000u,0xaaaau);
    WRITE32(box->address+offset,READ32(box->buffer+offset));
    result=ready(box);if(result)goto done;
    box->processed=offset+4;
  }
done:
  box->result=result;box->returned_nonce=box->nonce^RAM_NONCE_XOR;box->state=RAM_DONE;
}
#ifndef RAM_LOADER_NATIVE
__attribute__((section(".entry"),used,noreturn))
void ram_entry(volatile RamMailbox* box) {
  __asm volatile("cpsid i" ::: "memory");
  if((uint32_t)box==RAM_MAILBOX)ram_work(box);
  __asm volatile("dsb\n.global ram_breakpoint\nram_breakpoint: bkpt #0" ::: "memory");
  for(;;)__asm volatile("wfi");
}
#endif
