/* wolfBoot lab application for ALIENTEK STM32F407ZGT6 Explorer.
 * LEDs: PF9 (LED0), PF10 (LED1), active-low.
 * KEY0: PE4, active-low; only after a signed image is staged in UPDATE.
 * v1: LED0 blinks; v2: LED1 blinks and confirms; v3: both blink,
 * deliberately does NOT confirm, to demonstrate rollback on next reset.
 * Signed image version is read at runtime from the wolfBoot header.
 * TEST ONLY: no OTA transport; ST-LINK stages the update images.
 */
#include <stdint.h>
#include "target.h"
#include "wolfboot/wolfboot.h"

#define RCC_AHB1ENR (*(volatile uint32_t *)0x40023830u)
#define GPIOE_MODER (*(volatile uint32_t *)0x40021000u)
#define GPIOE_PUPDR (*(volatile uint32_t *)0x4002100Cu)
#define GPIOE_IDR   (*(volatile uint32_t *)0x40021010u)
#define GPIOF_MODER (*(volatile uint32_t *)0x40021400u)
#define GPIOF_BSRR  (*(volatile uint32_t *)0x40021418u)
#define SCB_AIRCR   (*(volatile uint32_t *)0xE000ED0Cu)

#define LED0  (1u << 9)
#define LED1  (1u << 10)
#define KEY0  (1u << 4)

static void delay_short(void)
{
    volatile uint32_t n;
    for (n = 0; n < 6000000u; ++n)
        __asm volatile ("nop");
}

static void gpio_init(void)
{
    RCC_AHB1ENR |= (1u << 4) | (1u << 5); /* GPIOE and GPIOF clock */
    (void)RCC_AHB1ENR;
    GPIOF_MODER = (GPIOF_MODER & ~((3u << 18) | (3u << 20))) |
                  (1u << 18) | (1u << 20); /* PF9/PF10 outputs */
    GPIOF_BSRR = LED0 | LED1; /* both LEDs off (active-low) */
    GPIOE_MODER &= ~(3u << 8); /* PE4 input */
    GPIOE_PUPDR = (GPIOE_PUPDR & ~(3u << 8)) | (1u << 8);
}

static void mcu_reset(void)
{
    __asm volatile ("dsb");
    SCB_AIRCR = (0x5FAu << 16) | (1u << 2); /* SYSRESETREQ */
    __asm volatile ("dsb");
    for (;;) { }
}

void main(void)
{
    uint32_t v;
    uint8_t state;
    gpio_init();
    v = wolfBoot_current_firmware_version();

    /* Real products confirm only after hardware/application self-test. */
    if ((v == 2u) &&
        (wolfBoot_get_partition_state(PART_BOOT, &state) == 0) &&
        (state == IMG_STATE_TESTING)) {
        wolfBoot_success();
    }
    /* v3 intentionally remains TESTING, so a subsequent reset rolls back. */

    /* If the button was held through reset, require release first. */
    while ((GPIOE_IDR & KEY0) == 0u) { }

    for (;;) {
        uint32_t on = (v == 1u) ? LED0 : (v == 2u) ? LED1 : (LED0 | LED1);
        GPIOF_BSRR = ((LED0 | LED1) << 16); /* both on */
        GPIOF_BSRR = ((LED0 | LED1) & ~on); /* unwanted LEDs off */
        delay_short();
        GPIOF_BSRR = LED0 | LED1; /* both off */
        delay_short();

        /* KEY0 is a lab-only update trigger, not an OTA protocol. */
        if ((v != 3u) && ((GPIOE_IDR & KEY0) == 0u)) {
            delay_short(); /* simple debounce; do not use in production */
            if ((GPIOE_IDR & KEY0) == 0u) {
                wolfBoot_update_trigger();
                mcu_reset();
            }
        }
    }
}
