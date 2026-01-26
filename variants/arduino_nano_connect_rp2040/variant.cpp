/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Arduino.h"
#include <stdint.h>

/*
 * Arm the loader's double_tap magic and reset. The loader's
 * double_tap_check() (loader/fixups.c) sees the magic on next boot and
 * enters USB BOOTSEL via libpico's reset_usb_boot(). Magic values must
 * match those in loader/fixups.c.
 */
extern "C" uint32_t magic_location[3];

void _on_1200_bps() {
    magic_location[0] = 0xf01681de;
    magic_location[1] = 0xbd729b29;
    magic_location[2] = 0xd359be7a;
    NVIC_SystemReset();
}
