#include <stdint.h>

#define RCC_APB2ENR   (*(volatile uint32_t *)0x40021018UL)
#define GPIOC_CRH     (*(volatile uint32_t *)0x40011004UL)
#define GPIOC_ODR     (*(volatile uint32_t *)0x4001100CUL)

void _init(void) { }

int main(void)
{
  /* Enable GPIOC and configure PC13 as 10 MHz push-pull output. */
  RCC_APB2ENR |= (1UL << 4);
  GPIOC_CRH = (GPIOC_CRH & ~(0xFUL << 20)) | (0x1UL << 20);

  for (;;) {
    GPIOC_ODR ^= (1UL << 13); /* Blue Pill LED is normally active-low. */
    for (volatile uint32_t delay = 0; delay < 800000UL; ++delay) {
      __asm volatile ("nop");
    }
  }
}
