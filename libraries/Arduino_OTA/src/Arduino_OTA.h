/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

// OTA header (20 bytes, matching Arduino_Portenta_OTA format)
struct OTAHeader {
    uint32_t len;           // file_size - 8
    uint32_t crc32;         // CRC-32 of file[8:]
    uint32_t magic_number;  // board-specific magic
    uint8_t  version[8];    // bitfield: header_ver(6)|compress(1)|reserved(1)|...
} __attribute__((packed));

#define OTA_HEADER_SIZE    20
#define OTA_FLAG_COMPRESS  0x40  // version[0] bit 6

//// Board-specific OTA magic numbers
#if defined(ARDUINO_PORTENTA_H7_M7)
#define OTA_BOARD_MAGIC 0x2341025B
#elif defined(ARDUINO_GIGA)
#define OTA_BOARD_MAGIC 0x23410266
#elif defined(ARDUINO_OPTA)
#define OTA_BOARD_MAGIC 0x23410064
#elif defined(ARDUINO_PORTENTA_C33)
#define OTA_BOARD_MAGIC 0x23410068
#endif

class ArduinoOTAClass {
public:
    enum class Error : int {
        None                 =  0,
        NoOtaStorage         = -1,
        NoCapableBootloader  = -2,
        OtaStorageInit       = -3,
        OtaStorageOpen       = -4,
        OtaHeaderLength      = -5,
        OtaHeaderCrc         = -6,
        OtaDownload          = -12,
    };


    void setURL(const char *url);
    void setCACert(const char *ca_cert_pem);
    int download();
    void setFeedWatchdogFunc(void (*func)(void));
    void feedWatchdog();
    const char* errorString();

    virtual ~ArduinoOTAClass() = default;
    virtual bool isOtaCapable();
    virtual Error begin();        // verify OTA storage accessible
    virtual int decompress();     // verify header+CRC, LZSS decompress -> UPDATE.BIN
    virtual Error update();       // signal bootloader/loader for update on reboot
    virtual void reset();         // reboot (does not return)

protected:
    int parseURL();
    int httpDownload(const char *filepath);
    static uint32_t crc_update(uint32_t crc, const void *data, size_t len);

    const char *_url = nullptr;
    const char *_ca_cert = nullptr;
    bool _use_tls = false;
    char _host[128];
    char _path[256];
    uint16_t _port = 80;
    uint32_t _program_length = 0;
    Error _error = Error::None;
    void (*_feed_watchdog_func)(void) = nullptr;
};
