/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Arduino.h"

#define NANO_RP2040_LED_PIN 6

void _on_1200_bps() {
    /*
     * Call reset_usb_boot() directly via the RP2040 ROM lookup table.
     * This avoids depending on rom_reset_usb_boot from libpico (not
     * exported to LLEXT sketches). The ROM is always at 0x00000000:
     *   0x14: 16-bit offset of the function table
     *   0x18: 16-bit offset of the table lookup function
     * 'UB' is the 2-byte code for reset_usb_boot (ROM_FUNC_RESET_USB_BOOT).
     */
    typedef void *(*rom_lookup_fn)(uint16_t *table, uint32_t code);
    typedef void __attribute__((noreturn)) (*reset_usb_boot_fn)(uint32_t, uint32_t);

    rom_lookup_fn lookup = (rom_lookup_fn)(uintptr_t)(*(volatile uint16_t *)0x18u);
    uint16_t *table      = (uint16_t *)(uintptr_t)(*(volatile uint16_t *)0x14u);

    reset_usb_boot_fn fn = (reset_usb_boot_fn) lookup(table, ('U' | ('B' << 8)));
    fn(1u << NANO_RP2040_LED_PIN, 0u);
}
