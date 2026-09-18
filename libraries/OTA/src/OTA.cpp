/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "OTA.h"

#include <zephyr/fs/fs.h>
#include <zephyr/storage/flash_map.h>

#include <string.h>
#include <strings.h>
#include <stdlib.h>

extern "C" {
int ota_sketch_ready(void);
int ota_sketch_start(void);
}

/*
 * The loader installs CONFIG_OTA_SKETCH_UPDATE_PATH at boot. We stage into the
 * temporary name and let ota_sketch_ready() do the rename, so an interrupted
 * transfer can never be picked up as a valid image.
 */
#define OTA_STAGING_PATH CONFIG_OTA_SKETCH_UPDATE_PATH CONFIG_OTA_SKETCH_TEMP_PATH_POSTFIX

#if defined(FIXED_PARTITION_EXISTS) && FIXED_PARTITION_EXISTS(user_sketch)
#define OTA_MAX_SKETCH_SIZE FIXED_PARTITION_SIZE(user_sketch)
#endif

static struct fs_file_t ota_file;

/* CRC-32 (IEEE), the same polynomial the OTA container is built with. */
static const uint32_t crc_table[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
    0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988, 0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
    0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
    0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172, 0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
    0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
    0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924, 0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
    0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
    0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e, 0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
    0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb,
    0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0, 0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
    0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
    0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a, 0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683,
    0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7,
    0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc, 0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
    0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
    0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236, 0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f,
    0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
    0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38, 0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
    0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
    0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2, 0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db,
    0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf,
    0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94, 0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

uint32_t OTAClass::crc_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *d = (const uint8_t *)data;
    while (len--) {
        crc = crc_table[(crc ^ *d++) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

void OTAClass::feedWatchdog()
{
    if (_feed_watchdog_func) {
        _feed_watchdog_func();
    }
}

bool OTAClass::isOtaCapable()
{
    /* Mount point is everything up to and including the ':' of the staging path. */
    const char *colon = strchr(CONFIG_OTA_SKETCH_UPDATE_PATH, ':');
    if (colon == nullptr) {
        return false;
    }

    char mount_point[32];
    size_t len = (size_t)(colon - CONFIG_OTA_SKETCH_UPDATE_PATH) + 1;
    if (len >= sizeof(mount_point)) {
        return false;
    }
    memcpy(mount_point, CONFIG_OTA_SKETCH_UPDATE_PATH, len);
    mount_point[len] = '\0';

    struct fs_statvfs stat;
    return fs_statvfs(mount_point, &stat) == 0;
}

OTAClass::Error OTAClass::fail(Error err)
{
    _error = err;
    _state = State::Failed;
    abort();
    return _error;
}

void OTAClass::abort()
{
    if (_file_open) {
        fs_close(&ota_file);
        _file_open = false;
    }
    fs_unlink(OTA_STAGING_PATH);
    if (_state != State::Failed) {
        _state = State::Idle;
    }
}

OTAClass::Error OTAClass::begin()
{
    _state = State::Header;
    _error = Error::None;
    _header_bytes = 0;
    _crc = 0xFFFFFFFF;
    _payload_expected = 0;
    _program_length = 0;

    if (!isOtaCapable()) {
        return fail(Error::NoOtaStorage);
    }

    /* A leftover staging file from an aborted attempt would otherwise be
     * appended to. */
    fs_unlink(OTA_STAGING_PATH);

    fs_file_t_init(&ota_file);
    if (fs_open(&ota_file, OTA_STAGING_PATH, FS_O_CREATE | FS_O_WRITE) < 0) {
        return fail(Error::OtaStorageOpen);
    }
    _file_open = true;

    return Error::None;
}

size_t OTAClass::write(const uint8_t *buf, size_t len)
{
    size_t consumed = 0;

    if (_state == State::Idle) {
        fail(Error::OtaStorageInit);
        return 0;
    }

    while (consumed < len) {
        if (_state == State::Header) {
            size_t want = OTA_HEADER_SIZE - _header_bytes;
            size_t take = (len - consumed < want) ? len - consumed : want;

            memcpy((uint8_t *)&_header + _header_bytes, buf + consumed, take);
            _header_bytes += take;
            consumed += take;

            if (_header_bytes < OTA_HEADER_SIZE) {
                break;
            }

            /* len counts from offset 8, so it covers magic + version + payload. */
            if (_header.len < OTA_HEADER_SIZE - 8) {
                fail(Error::OtaHeaderLength);
                return consumed;
            }
            _payload_expected = _header.len - (OTA_HEADER_SIZE - 8);

#if defined(OTA_BOARD_MAGIC)
            if (_header.magic_number != OTA_BOARD_MAGIC) {
                fail(Error::OtaHeaderMagic);
                return consumed;
            }
#endif
            if (_header.version[0] & OTA_FLAG_COMPRESS) {
                fail(Error::OtaDecompress);
                return consumed;
            }
#if defined(OTA_MAX_SKETCH_SIZE)
            if (_payload_expected > OTA_MAX_SKETCH_SIZE) {
                fail(Error::OtaHeaderLength);
                return consumed;
            }
#endif
            /* CRC is taken over the file from offset 8 onwards. */
            _crc = crc_update(_crc, (const uint8_t *)&_header + 8, OTA_HEADER_SIZE - 8);
            _state = State::Payload;
            continue;
        }

        if (_state != State::Payload) {
            break;
        }

        size_t remaining = _payload_expected - _program_length;
        size_t take = len - consumed;
        if (take > remaining) {
            take = remaining;
        }
        if (take == 0) {
            break;  /* trailing bytes past the declared length are ignored */
        }

        if (fs_write(&ota_file, buf + consumed, take) != (ssize_t)take) {
            fail(Error::OtaStorageInit);
            return consumed;
        }

        _crc = crc_update(_crc, buf + consumed, take);
        _program_length += take;
        consumed += take;
        feedWatchdog();

        if (_program_length == _payload_expected) {
            _state = State::Complete;
        }
    }

    return consumed;
}

int OTAClass::readFrom(const char *path)
{
    struct fs_file_t in;
    fs_file_t_init(&in);

    if (fs_open(&in, path, FS_O_READ) < 0) {
        return (int)fail(Error::OtaStorageOpen);
    }

    uint8_t buf[512];
    ssize_t n;
    while ((n = fs_read(&in, buf, sizeof(buf))) > 0) {
        if (write(buf, n) != (size_t)n) {
            fs_close(&in);
            return (int)_error;
        }
    }
    fs_close(&in);

    if (n < 0) {
        return (int)fail(Error::OtaStorageOpen);
    }
    return (int)_program_length;
}

int OTAClass::readFrom(Stream &stream, size_t len, uint32_t timeout_ms)
{
    uint8_t buf[256];
    size_t received = 0;
    uint32_t last = millis();

    while (received < len) {
        int avail = stream.available();
        if (avail <= 0) {
            if (millis() - last > timeout_ms) {
                return (int)fail(Error::OtaHeaderLength);
            }
            feedWatchdog();
            continue;
        }

        size_t want = len - received;
        if (want > sizeof(buf)) {
            want = sizeof(buf);
        }
        int n = stream.readBytes(buf, want);
        if (n <= 0) {
            continue;
        }

        if (write(buf, n) != (size_t)n) {
            return (int)_error;
        }
        received += n;
        last = millis();
    }

    return (int)_program_length;
}

/*
 * Minimal HTTP/1.1 GET. Only what an OTA fetch needs: no redirects, no chunked
 * transfer encoding, no TLS. The body is streamed straight into write() so the
 * image is never held in RAM.
 */
int OTAClass::readFrom(Client &client, const char *host, uint16_t port, const char *path,
                       uint32_t timeout_ms)
{
    if (!client.connect(host, port)) {
        return (int)fail(Error::OtaConnect);
    }

    client.print("GET ");
    client.print(path);
    client.print(" HTTP/1.1\r\nHost: ");
    client.print(host);
    client.print("\r\nConnection: close\r\n\r\n");

    /* Read the status line and headers one line at a time. */
    char line[128];
    bool first_line = true;
    long content_length = -1;
    uint32_t last = millis();

    while (true) {
        if (!client.available()) {
            if (!client.connected() || millis() - last > timeout_ms) {
                client.stop();
                return (int)fail(Error::OtaDownload);
            }
            feedWatchdog();
            continue;
        }
        last = millis();

        size_t n = client.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        if (n > 0 && line[n - 1] == '\r') {
            line[n - 1] = '\0';
        }

        if (first_line) {
            /* "HTTP/1.1 200 OK" */
            if (strncmp(line, "HTTP/1.", 7) != 0 || atoi(line + 9) != 200) {
                client.stop();
                return (int)fail(Error::OtaDownload);
            }
            first_line = false;
            continue;
        }

        if (line[0] == '\0') {
            break;  /* end of headers */
        }

        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            content_length = atol(line + 15);
        } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 &&
                   strstr(line, "chunked") != nullptr) {
            client.stop();
            return (int)fail(Error::OtaDownload);
        }
    }

    if (content_length <= 0) {
        client.stop();
        return (int)fail(Error::OtaDownload);
    }

    uint8_t buf[512];
    long received = 0;
    last = millis();

    while (received < content_length) {
        int avail = client.available();
        if (avail <= 0) {
            if (!client.connected() || millis() - last > timeout_ms) {
                break;
            }
            feedWatchdog();
            continue;
        }

        size_t want = (size_t)(content_length - received);
        if (want > sizeof(buf)) {
            want = sizeof(buf);
        }
        int got = client.read(buf, want);
        if (got <= 0) {
            continue;
        }

        if (write(buf, got) != (size_t)got) {
            client.stop();
            return (int)_error;
        }
        received += got;
        last = millis();
    }

    client.stop();

    if (received != content_length) {
        return (int)fail(Error::OtaDownload);
    }
    return (int)_program_length;
}

OTAClass::Error OTAClass::update()
{
    if (_state == State::Failed) {
        return _error;
    }
    if (_state != State::Complete) {
        return fail(Error::OtaHeaderLength);
    }

    if ((_crc ^ 0xFFFFFFFF) != _header.crc32) {
        return fail(Error::OtaHeaderCrc);
    }

    if (fs_close(&ota_file) < 0) {
        _file_open = false;
        return fail(Error::OtaStorageInit);
    }
    _file_open = false;

    /* Renames the staging file to the name the loader looks for at boot. */
    if (ota_sketch_ready() < 0) {
        return fail(Error::OtaRename);
    }

    _error = Error::None;
    return _error;
}

void OTAClass::reset()
{
    /* Reboots; the loader installs the staged image before starting it. */
    ota_sketch_start();
}

const char *OTAClass::errorString()
{
    switch (_error) {
    case Error::None:                return "no error";
    case Error::NoOtaStorage:        return "no OTA storage available";
    case Error::NoCapableBootloader: return "bootloader not OTA capable";
    case Error::OtaStorageInit:      return "OTA storage write failed";
    case Error::OtaStorageOpen:      return "OTA storage open failed";
    case Error::OtaHeaderLength:     return "OTA length mismatch or truncated image";
    case Error::OtaHeaderCrc:        return "OTA CRC mismatch";
    case Error::OtaDecompress:       return "compressed OTA images are not supported";
    case Error::OtaHeaderMagic:      return "OTA image is for a different board";
    case Error::OtaConnect:          return "could not connect to the OTA server";
    case Error::OtaDownload:         return "OTA download failed";
    case Error::OtaRename:           return "could not mark the update as ready";
    default:                         return "unknown error";
    }
}

OTAClass OTA;
