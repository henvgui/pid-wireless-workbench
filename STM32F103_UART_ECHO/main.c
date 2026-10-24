#include <stdint.h>
#include "echo_target.h"
#define REG32(a) (*(volatile uint32_t*)(a))
#define RCC_CR REG32(0x40021000u)
#define RCC_CFGR REG32(0x40021004u)
#define RCC_APB2RSTR REG32(0x4002100cu)
#define RCC_APB2ENR REG32(0x40021018u)
#define GPIOA_CRL REG32(0x40010800u)
#define GPIOA_CRH REG32(0x40010804u)
#define GPIOA_BSRR REG32(0x40010810u)
#define AFIO_MAPR REG32(0x40010004u)
#define USART_SR REG32(0x40013800u)
#define USART_DR REG32(0x40013804u)
#define USART_BRR REG32(0x40013808u)
#define USART_CR1 REG32(0x4001380cu)
#define USART_CR2 REG32(0x40013810u)
#define USART_CR3 REG32(0x40013814u)
static EchoTarget target;
static EchoParser parser;
static uint8_t incoming[160],reply[160];
int main(void) {
    __asm volatile("cpsid i" ::: "memory");
    REG32(0xe000ed08u)=0x08000000u;
    /* ROM GO may leave a different clock configuration; explicitly select HSI. */
    RCC_CR|=1u;while(!(RCC_CR&(1u<<1))){}
    RCC_CFGR&=~3u;while(RCC_CFGR&12u){}
    RCC_CFGR=0;RCC_CR&=~(1u<<24);
    RCC_APB2ENR|=(1u<<0)|(1u<<2)|(1u<<14);
    RCC_APB2RSTR|=1u<<14;RCC_APB2RSTR&=~(1u<<14);
    AFIO_MAPR&=~(1u<<2);
    GPIOA_BSRR=1u<<3;
    GPIOA_CRL=(GPIOA_CRL&~(0xfu<<12))|(2u<<12); /* retain PA3 HIGH */
    GPIOA_CRH=(GPIOA_CRH&~(0xffu<<4))|(0x4au<<4); /* PA9 AF PP, PA10 floating RX */
    USART_CR1=0;USART_CR2=0;USART_CR3=0;
    USART_BRR=69; /* 8 MHz / 115200, actual 115942, ~0.64% error */
    USART_CR1=(1u<<13)|(1u<<3)|(1u<<2); /* 115200 8N1 application protocol */
    for(;;) {
        uint32_t sr=USART_SR;
        if(sr&0x2fu) {
            uint8_t byte=(uint8_t)USART_DR;
            if(sr&15u){parser.size=0;continue;}
            if(sr&(1u<<5)) {
                size_t n=echo_parser_feed(&parser,byte,incoming);
                if(n) {
                    size_t count=echo_target_handle(&target,incoming,n,reply),i;
                    for(i=0;i<count;++i){while(!(USART_SR&(1u<<7))){}USART_DR=reply[i];}
                    if(count)while(!(USART_SR&(1u<<6))){}
                }
            }
        }
    }
}
