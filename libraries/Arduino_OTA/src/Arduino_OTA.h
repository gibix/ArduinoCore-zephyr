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

class ArduinoOTAClass {
public:
    enum class Error : int {
        None                 =  0,
        NoCapableBootloader  = -1,
        OtaStorageInit       = -3,
        OtaStorageOpen       = -4,
        OtaHeaderLength      = -5,
        OtaHeaderCrc         = -6,
        OtaDownload          = -12,
    };

    static bool isOtaCapable();

    void setURL(const char *url);

    // Set CA certificate for HTTPS downloads (PEM format).
    void setCACert(const char *ca_cert_pem);

    // Set expected board magic number for verification.
    // If not set, magic check is skipped.
    void setMagic(uint32_t magic);

    Error begin();            // check bootloader + verify storage accessible
    int download();           // HTTP GET -> temp file (returns bytes written, or negative Error)
    int decompress();         // verify header+CRC -> LZSS -> UPDATE.BIN (returns decompressed size, or negative Error)
    Error update();           // write RTC backup registers
    void reset();             // NVIC_SystemReset (does not return)

    void setFeedWatchdogFunc(void (*func)(void));
    void feedWatchdog();

    const char* errorString();

private:
    int parseURL();
    int httpDownload(const char *filepath);

    const char *_url = nullptr;
    const char *_ca_cert = nullptr;
    bool _use_tls = false;
    char _host[128];
    char _path[256];
    uint16_t _port = 80;
    uint32_t _program_length = 0;
    uint32_t _magic = 0;
    bool _magic_set = false;
    Error _error = Error::None;
    void (*_feed_watchdog_func)(void) = nullptr;
};

extern ArduinoOTAClass ArduinoOTA;
