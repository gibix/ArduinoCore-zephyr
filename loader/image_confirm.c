/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#if defined(CONFIG_BOOTLOADER_MCUBOOT) && (CONFIG_LOADER_CONFIRM_DELAY_MS > 0)
#include <zephyr/kernel.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(image_confirm);

/*
 * Confirm a test image (swap modes) once the sketch has run for a while. The
 * thread has the lowest priority, like the watchdog feed: a sketch that hogs
 * the CPU starves both, the watchdog fires and MCUboot reverts the image.
 */
void image_confirm_thread(void *p1, void *p2, void *p3) {
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (boot_is_img_confirmed()) {
		return;
	}

	k_msleep(CONFIG_LOADER_CONFIRM_DELAY_MS);

	/* The sketch may have confirmed it already (ota_confirm()). */
	if (boot_is_img_confirmed()) {
		return;
	}

	if (boot_write_img_confirmed()) {
		LOG_ERR("Unable to confirm the running image");
	} else {
		LOG_INF("Running image confirmed");
	}
}

K_THREAD_DEFINE(image_confirm_thread_id, 1024, image_confirm_thread, NULL, NULL, NULL,
				K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

#endif /* CONFIG_BOOTLOADER_MCUBOOT && CONFIG_LOADER_CONFIRM_DELAY_MS > 0 */
