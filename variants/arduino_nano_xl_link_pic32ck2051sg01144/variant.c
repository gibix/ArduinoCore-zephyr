/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/retention/bootmode.h>
#include <zephyr/sys/reboot.h>

/* Ask MCUboot for USB DFU. Plain C: the loader builds variant.c too. */
void nano_xl_link_request_dfu(void) {
	(void)bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
	sys_reboot(SYS_REBOOT_WARM);
}

/* The loader's override; sketches use variant.cpp's C++ one. */
void _on_1200_bps(void) {
	nano_xl_link_request_dfu();
}
