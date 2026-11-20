/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MCUboot on native_sim cannot jump into the image: it prints the boot
 * decision on one line and exits. The runner (run_ota.sh) then starts the
 * loader on the same flash and EEPROM files, like a jump after a warm reset.
 *
 * Exit codes: 0 image selected, 3 no bootable image (102: power_cut.c).
 * ops=<n> is the number of flash operations of this boot (power_cut.c).
 */

#include <zephyr/kernel.h>
#include <posix_board_if.h>

#include "bootutil/bootutil.h"
#include "bootutil/boot_hooks.h"
#include "bootutil/fault_injection_hardening.h"
#include "bootutil/mcuboot_status.h"

unsigned int sim_flash_ops(void);

#define SIM_EXIT_BOOT     0
#define SIM_EXIT_NO_IMAGE 3

fih_ret boot_image_jump_hook(struct boot_rsp *rsp)
{
	const struct image_version *v = &rsp->br_hdr->ih_ver;

	/* Parsed by run_ota.sh: keep the format. */
	printk("SIM-BOOT: off=0x%x ver=%u.%u.%u+%u ops=%u\n", (unsigned int)rsp->br_image_off,
	       v->iv_major, v->iv_minor, v->iv_revision, (unsigned int)v->iv_build_num,
	       sim_flash_ops());
	posix_exit(SIM_EXIT_BOOT);
}

void mcuboot_status_change(mcuboot_status_type_t status)
{
	if (status == MCUBOOT_STATUS_NO_BOOTABLE_IMAGE_FOUND) {
		printk("SIM-BOOT: no bootable image ops=%u\n", sim_flash_ops());
			posix_exit(SIM_EXIT_NO_IMAGE);
	}
}
