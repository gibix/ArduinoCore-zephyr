/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Arduino_OTA_Legacy.h"

#include <Arduino.h>
#include <SocketWrapper.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>

#include <cmsis_core.h>

#if defined(CONFIG_SOC_SERIES_STM32H7X)
#include <stm32h7xx.h>
#endif

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define OTA_TEMP_PATH    "/ota:/UPDATE.BIN.OTA"
#define OTA_FILE_PATH    "/ota:/UPDATE.BIN"
#define OTA_BUF_SIZE     4096

#if defined(CONFIG_SOC_SERIES_STM32H7X)
#define OTA_MAGIC_BOOT   0x07AA
#define OTA_STORAGE_TYPE 0xA4   // QSPI_FLASH | FATFS | MBR
#define OTA_MBR_PART     2
#endif

bool ArduinoOTALegacyClass::isOtaCapable()
{
#if defined(CONFIG_SOC_SERIES_STM32H7X)
    // STM32H7: check bootloader version at fixed address
    const uint8_t *bootloader_data = (const uint8_t *)(0x08000000 + 0x1F000);
    return bootloader_data[1] >= 22;
#else
    // C33: SFU is always present if properly flashed
    return true;
#endif
}

ArduinoOTALegacyClass::Error ArduinoOTALegacyClass::begin()
{
    _program_length = 0;
    _error = Error::None;

    if (!isOtaCapable()) {
        _error = Error::NoCapableBootloader;
        return _error;
    }

    struct fs_statvfs stat;
    if (fs_statvfs("/ota:", &stat) < 0) {
        _error = Error::OtaStorageInit;
        return _error;
    }

    return Error::None;
}


ArduinoOTALegacyClass::Error ArduinoOTALegacyClass::update()
{
#if defined(CONFIG_SOC_SERIES_STM32H7X)
    // STM32H7: write RTC backup registers to signal bootloader
    struct fs_dirent entry;
    if (fs_stat(OTA_FILE_PATH, &entry) < 0) {
        _error = Error::OtaStorageOpen;
        return _error;
    }
    _program_length = entry.size;

    uint32_t rtc_base = (uint32_t)&(RTC->BKP0R);

    // in EDK use this instead of HAL_RTCEx_BKUPWrite
    *(__IO uint32_t *)(rtc_base + RTC_BKP_DR0 * 4U) = OTA_MAGIC_BOOT;
    *(__IO uint32_t *)(rtc_base + RTC_BKP_DR1 * 4U) = OTA_STORAGE_TYPE;
    *(__IO uint32_t *)(rtc_base + RTC_BKP_DR2 * 4U) = OTA_MBR_PART;
    *(__IO uint32_t *)(rtc_base + RTC_BKP_DR3 * 4U) = _program_length;
#endif
    // C33: SFU checks for UPDATE.BIN.OTA on every boot — no action needed

    return Error::None;
}


void ArduinoOTALegacyClass::reset()
{
    NVIC_SystemReset();
}

