#include <stdint.h>

#define REG32(address) (*(volatile uint32_t *)(address))
#define RCC_APB2ENR REG32(0x40021018u)
#define GPIOA_CRL   REG32(0x40010800u)
#define GPIOA_BSRR  REG32(0x40010810u)
#define SCB_VTOR    REG32(0xe000ed08u)

int main(void) {
    __asm volatile("cpsid i" ::: "memory");
    SCB_VTOR = 0x08000000u;
    RCC_APB2ENR |= 1u << 2;          /* Enable GPIOA clock. */
    (void)RCC_APB2ENR;
    GPIOA_BSRR = 1u << 3;           /* Preload PA3 output HIGH. */
    GPIOA_CRL = (GPIOA_CRL & ~(0xfu << 12)) | (0x2u << 12);
    /* PA3: general-purpose push-pull output, maximum speed 2 MHz. */
    for (;;) {
        __asm volatile("wfi");
    }
}
