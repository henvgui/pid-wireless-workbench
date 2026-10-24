#include <stdint.h>
#include "echo_target.h"

#define REG32(a) (*(volatile uint32_t*)(a))
#define RCC_CR       REG32(0x40023800u)
#define RCC_CFGR     REG32(0x40023808u)
#define RCC_APB2RSTR REG32(0x40023824u)
#define RCC_AHB1ENR  REG32(0x40023830u)
#define RCC_APB2ENR  REG32(0x40023844u)
#define GPIOA_MODER  REG32(0x40020000u)
#define GPIOA_OTYPER REG32(0x40020004u)
#define GPIOA_SPEED  REG32(0x40020008u)
#define GPIOA_PUPDR  REG32(0x4002000cu)
#define GPIOA_AFRH   REG32(0x40020024u)
#define USART_SR    REG32(0x40011000u)
#define USART_DR    REG32(0x40011004u)
#define USART_BRR   REG32(0x40011008u)
#define USART_CR1   REG32(0x4001100cu)
#define USART_CR2   REG32(0x40011010u)
#define USART_CR3   REG32(0x40011014u)

static EchoTarget target;
static EchoParser parser;
static uint8_t incoming[160],reply[160];

int main(void) {
    __asm volatile("cpsid i" ::: "memory");
    REG32(0xe000ed08u)=0x08000000u;
    /* HSI 16 MHz, AHB/APB prescalers /1; no external crystal dependency. */
    RCC_CR|=1u;
    while(!(RCC_CR&(1u<<1))){}
    RCC_CFGR&=~3u;
    while(RCC_CFGR&12u){}
    RCC_CFGR=0;
    RCC_CR&=~(1u<<24);
    RCC_AHB1ENR|=1u;
    (void)RCC_AHB1ENR;
    RCC_APB2ENR|=1u<<4;
    RCC_APB2RSTR|=1u<<4;
    RCC_APB2RSTR&=~(1u<<4);
    /* USART1: PA9 TX, PA10 RX, AF7. PA13/PA14 remain available for SWD. */
    GPIOA_MODER=(GPIOA_MODER&~((3u<<18)|(3u<<20)))|(2u<<18)|(2u<<20);
    GPIOA_OTYPER&=~((1u<<9)|(1u<<10));
    GPIOA_SPEED=(GPIOA_SPEED&~((3u<<18)|(3u<<20)))|(2u<<18)|(2u<<20);
    GPIOA_PUPDR=(GPIOA_PUPDR&~((3u<<18)|(3u<<20)))|(1u<<18)|(1u<<20);
    GPIOA_AFRH=(GPIOA_AFRH&~((15u<<4)|(15u<<8)))|(7u<<4)|(7u<<8);
    USART_CR1=0;USART_CR2=0;USART_CR3=0;
    USART_BRR=139u; /* 16 MHz / 139 = 115108 baud, -0.08% quantisation. */
    USART_CR1=(1u<<13)|(1u<<3)|(1u<<2); /* UE, TE, RE; 115200 8N1. */
    for(;;) {
        const uint32_t status=USART_SR;
        if(status&0x2fu) {
            const uint8_t byte=(uint8_t)USART_DR; /* SR/DR sequence clears errors. */
            if(status&15u){parser.size=0;continue;}
            if(status&(1u<<5)) {
                const size_t size=echo_parser_feed(&parser,byte,incoming);
                if(size) {
                    const size_t count=echo_target_handle(&target,incoming,size,reply);
                    for(size_t i=0;i<count;++i){while(!(USART_SR&(1u<<7))){}USART_DR=reply[i];}
                    if(count)while(!(USART_SR&(1u<<6))){}
                }
            }
        }
    }
}
