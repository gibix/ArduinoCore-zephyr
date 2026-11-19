/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loader side of the MCUboot OTA simulator:
 * - start the watchdog, as MCUboot does on the board before jumping;
 * - turn a reboot (sys_reboot(), watchdog bite) into the end of the process,
 *   so the runner starts MCUboot next. native_sim would re-execute the loader
 *   itself. sys_arch_reboot() is wrapped at link time (CMakeLists.txt).
 *
 * Exit code 100: reboot, 101: watchdog not started.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/init.h>
#include <zephyr/sys/printk.h>
#include <posix_board_if.h>

#define SIM_EXIT_REBOOT 100
#define SIM_EXIT_NO_WDT 101

void __wrap_sys_arch_reboot(int type)
{
	/* Parsed by run_ota.sh: keep the format. */
	printk("SIM-REBOOT: type=%d\n", type);
	posix_exit(SIM_EXIT_REBOOT);
}

#if DT_HAS_ALIAS(watchdog0)
static int sim_watchdog_start(void)
{
	const struct device *wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

	if (!device_is_ready(wdt) || wdt_setup(wdt, 0) != 0) {
		printk("SIM-REBOOT: watchdog not started\n");
		posix_exit(SIM_EXIT_NO_WDT);
	}
	return 0;
}

SYS_INIT(sim_watchdog_start, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);
#endif
