/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ota_api.h"

#include <zephyr/fs/fs.h>
#include <zephyr/sys/reboot.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ota_loader, CONFIG_OTA_LOG_LEVEL);

int ota_loader_ready(void)
{
	struct fs_dirent entry;

	if (fs_stat(OTA_LOADER_FILENAME, &entry) < 0) {
		LOG_WRN("tried to perform an ota without binary present");
		return -1;
	}

	if (sizeof(CONFIG_OTA_LOADER_TEMP_PATH_POSTFIX) > 1) {
		int ret = fs_rename(OTA_LOADER_FILENAME, CONFIG_OTA_LOADER_UPDATE_PATH);
		if (ret < 0) {
			LOG_ERR("unable to rename ota file: %d", ret);
			return ret;
		}
	}

	LOG_INF("OTA marked as ready");
	return 0;
}

int ota_loader_start(void)
{
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}
