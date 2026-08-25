/*
 * Board-specific loader hooks (plain C, built into the loader).
 * _on_1200_bps() overrides the weak default so a 1200-bps USB touch
 * enters MCUboot serial recovery instead of a plain reboot.
 */

#include <cmsis_core.h>
#include "dfu_cookie.h"

void _on_1200_bps(void) {
    DFU_COOKIE = DFU_DOUBLE_RESET_MAGIC;
    __DSB();
    NVIC_SystemReset();
}
