/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "Arduino_OTA_Loader.h"

#include <zephyr/fs/fs.h>
#include <zephyr/sys/reboot.h>

#define OTA_FILE_PATH    "/ota:/UPDATE.BIN"
#define OTA_SENTINEL_PATH "/ota:/OTA_UPDATE_PENDING"

bool ArduinoOTALoaderClass::isOtaCapable()
{
    struct fs_statvfs stat;
    return fs_statvfs("/ota:", &stat) == 0;
}

ArduinoOTALoaderClass::Error ArduinoOTALoaderClass::begin()
{
    _program_length = 0;
    _error = Error::None;

    if (!isOtaCapable()) {
        _error = Error::NoOtaStorage;
        return _error;
    }

    struct fs_statvfs stat;
    if (fs_statvfs("/ota:", &stat) < 0) {
        _error = Error::OtaStorageInit;
        return _error;
    }

    return Error::None;
}

ArduinoOTALoaderClass::Error ArduinoOTALoaderClass::update()
{
    struct fs_dirent entry;
    if (fs_stat(OTA_FILE_PATH, &entry) < 0) {
        _error = Error::OtaStorageOpen;
        return _error;
    }
    _program_length = entry.size;

    /* Create sentinel file for the loader to pick up on reboot */
    struct fs_file_t sentinel;
    fs_file_t_init(&sentinel);
    int ret = fs_open(&sentinel, OTA_SENTINEL_PATH, FS_O_CREATE | FS_O_WRITE);
    if (ret < 0) {
        _error = Error::OtaStorageOpen;
        return _error;
    }
    fs_close(&sentinel);

    return Error::None;
}


void ArduinoOTALoaderClass::reset()
{
    sys_reboot(SYS_REBOOT_COLD);
}

