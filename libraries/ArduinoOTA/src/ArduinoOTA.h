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
    void setURL(const char *url);

    // Set expected board magic number for verification.
    // If not set, magic check is skipped.
    void setMagic(uint32_t magic);

    int begin();
    int download();       // returns 0 on success, negative on error
    void update();        // sets RTC registers + reboots (does not return)
    const char* errorString();

private:
    int parseURL();
    int httpDownload(const char *filepath);
    int verifyOTA(const char *filepath, bool *compressed);
    int decompressOTA(const char *src, const char *dst);
    int copyFile(const char *src, const char *dst);

    const char *_url = nullptr;
    char _host[128];
    char _path[256];
    uint16_t _port = 80;
    uint32_t _program_length = 0;
    uint32_t _magic = 0;
    bool _magic_set = false;
    const char *_error = nullptr;
};

extern ArduinoOTAClass ArduinoOTA;
