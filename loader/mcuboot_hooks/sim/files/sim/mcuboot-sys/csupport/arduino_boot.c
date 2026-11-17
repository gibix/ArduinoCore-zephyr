/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * One boot of the Zephyr port with the Arduino hooks, in the order
 * boot/zephyr/main.c runs them: the double tap's SYS_INITs, boot_go_hook()
 * (USB DFU on a boot mode request), boot_go(), and the
 * MCUBOOT_STATUS_NO_BOOTABLE_IMAGE_FOUND action hook when it fails. The hooks
 * are the core's own files (ARDUINO_MCUBOOT_DFU_BOOT_MODE_FILE,
 * ARDUINO_MCUBOOT_DOUBLE_TAP_FILE), built against the stand-ins in
 * arduino_zephyr/.
 */

#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"

#define MCUBOOT_ACTION_HOOKS

#include "sim_arduino.h"
#include "bootsim.h"
#include <bootutil/bootutil.h>
#include <bootutil/boot_hooks.h>
#include <bootutil/fault_injection_hardening.h>
#include <bootutil/mcuboot_status.h>

#include ARDUINO_MCUBOOT_DFU_BOOT_MODE_FILE
#include ARDUINO_MCUBOOT_DOUBLE_TAP_FILE

struct area_desc;
struct sim_context;

extern void sim_set_flash_areas(struct area_desc *areas);
extern void sim_reset_flash_areas(void);
extern void sim_set_context(struct sim_context *ctx);
extern void sim_reset_context(void);
extern int invoke_boot_go(struct sim_context *ctx, struct area_desc *adesc,
                          struct boot_rsp *rsp, int image_id);

/*
 * Returns 0 when the boot reached its end (io->boot_rc holds boot_go()'s
 * result, -1 if boot_go() did not run), 1 when the board was reset before
 * that (io->reset_in_window or io->rebooted), and boot_go()'s -0x13579 when
 * the flash simulation stopped it.
 */
int invoke_arduino_boot(struct sim_context *ctx, struct area_desc *adesc,
                        struct boot_rsp *rsp, struct arduino_boot_io *io)
{
    volatile fih_ret hook_rc = FIH_BOOT_HOOK_REGULAR;
    int rc;

    sim_io = io;
    io->dfu_entered = 0;
    io->rebooted = 0;
    io->window_slept = 0;
    io->reset_in_window = 0;
    io->boot_rc = -1;

    /* SYS_INITs, then boot_go_hook(), with the flash of this boot. */
    sim_set_flash_areas(adesc);
    sim_set_context(ctx);
    switch (setjmp(sim_reset_jmp)) {
    case 0:
        tapped = false;
        double_tap_arm();
        double_tap_check();
        hook_rc = boot_go_hook(rsp);
        break;
    case SIM_RESET_IN_WINDOW:
        io->reset_in_window = 1;
        break;
    default:
        break;
    }
    sim_reset_flash_areas();
    sim_reset_context();
    if (io->reset_in_window || io->rebooted) {
        sim_io = NULL;
        return 1;
    }

    if (FIH_EQ(hook_rc, FIH_BOOT_HOOK_REGULAR)) {
        rc = invoke_boot_go(ctx, adesc, rsp, -1);
        if (rc == -0x13579) {
            sim_io = NULL;
            return rc;
        }
        io->boot_rc = rc;
    }

    if (io->boot_rc != 0) {
        if (setjmp(sim_reset_jmp) == 0) {
            mcuboot_status_change(MCUBOOT_STATUS_NO_BOOTABLE_IMAGE_FOUND);
        }
    }

    sim_io = NULL;
    return io->rebooted ? 1 : 0;
}
