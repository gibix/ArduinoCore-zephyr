/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <Arduino.h>
#include <api/Client.h>
#include <zephyr/autoconf.h>

#include <stdint.h>
#include <stddef.h>

#ifndef CONFIG_OTA
#error "This variant's loader was built without CONFIG_OTA; the OTA library cannot be used."
#endif

/*
 * Header of the .ota container emitted by zephyr-sketch-tool -ota.
 * `len` and `crc32` cover the file from offset 8 onwards, so the CRC includes
 * `magic_number` and `version` as well as the payload.
 */
struct OTAHeader {
    uint32_t len;
    uint32_t crc32;
    uint32_t magic_number;
    uint8_t  version[8];
} __attribute__((packed));

#define OTA_HEADER_SIZE   20
#define OTA_FLAG_COMPRESS 0x40  /* version[0] bit 6 */

/* Must match build.ota.magic in boards.txt for the same board. */
#if defined(ARDUINO_PORTENTA_H7_M7)
#define OTA_BOARD_MAGIC 0x2341025B
#elif defined(ARDUINO_GIGA)
#define OTA_BOARD_MAGIC 0x23410266
#elif defined(ARDUINO_OPTA)
#define OTA_BOARD_MAGIC 0x23410064
#elif defined(ARDUINO_PORTENTA_C33)
#define OTA_BOARD_MAGIC 0x23410068
#elif defined(ARDUINO_NICLA_VISION)
#define OTA_BOARD_MAGIC 0x2341025F
#elif defined(ARDUINO_NANO_RP2040_CONNECT)
#define OTA_BOARD_MAGIC 0x2341005E
#endif

/*
 * Stages a sketch update coming from any local source.
 *
 * Bytes are fed in through write(), which peels the .ota header, verifies it
 * and streams the payload into the loader's staging file. update() commits the
 * staged image and reset() reboots into it.
 */
class OTAClass {
public:
    enum class Error : int {
        None                = 0,
        NoOtaStorage        = -1,
        NoCapableBootloader = -2,
        OtaStorageInit      = -3,
        OtaStorageOpen      = -4,
        OtaHeaderLength     = -5,
        OtaHeaderCrc        = -6,
        OtaDecompress       = -7,
        OtaHeaderMagic      = -8,
        OtaConnect          = -10,
        OtaDownload         = -12,
        OtaRename           = -23,
    };

    bool isOtaCapable();

    /* Opens the staging file and resets the parser. */
    Error begin();

    /* Feeds update bytes. Returns the number consumed, short on error. */
    size_t write(const uint8_t *buf, size_t len);

    /* Ingests a .ota file already present on the filesystem. */
    int readFrom(const char *path);

    /* Ingests `len` bytes off a Stream, giving up after `timeout_ms` of silence. */
    int readFrom(Stream &stream, size_t len, uint32_t timeout_ms = 10000);

    /*
     * Fetches a .ota file over plain HTTP and stages it. The Client is supplied
     * by the caller, so this works with any transport (WiFi, Ethernet, ...)
     * without the library depending on one.
     */
    int readFrom(Client &client, const char *host, uint16_t port, const char *path,
                 uint32_t timeout_ms = 10000);

    /* Verifies the staged image and marks it for installation on next boot. */
    Error update();

    /* Reboots into the loader, which installs the staged image. Does not return. */
    void reset();

    /* Discards a staged image, leaving the current sketch untouched. */
    void abort();

    uint32_t length() const { return _program_length; }
    Error error() const { return _error; }
    const char *errorString();

    void setFeedWatchdogFunc(void (*func)(void)) { _feed_watchdog_func = func; }
    void feedWatchdog();

private:
    enum class State : uint8_t { Idle, Header, Payload, Complete, Failed };

    Error fail(Error err);
    static uint32_t crc_update(uint32_t crc, const void *data, size_t len);

    State _state = State::Idle;
    Error _error = Error::None;

    OTAHeader _header;
    size_t   _header_bytes = 0;
    uint32_t _crc = 0;
    uint32_t _payload_expected = 0;
    uint32_t _program_length = 0;
    bool     _file_open = false;

    void (*_feed_watchdog_func)(void) = nullptr;
};

extern OTAClass OTA;
