/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Arduino_OTA.h"
#include "lzss.h"

#include <Arduino.h>
#include <SocketWrapper.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define OTA_TEMP_PATH    "/ota:/UPDATE.BIN.OTA"
#define OTA_FILE_PATH    "/ota:/UPDATE.BIN"
#define OTA_BUF_SIZE     4096

// CRC-32 table (IEEE, same as Arduino_Portenta_OTA)
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

uint32_t ArduinoOTAClass::crc_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *d = (const uint8_t *)data;
    while (len--) {
        crc = crc_table[(crc ^ *d++) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

void ArduinoOTAClass::setURL(const char *url)
{
    _url = url;
    _error = Error::None;
}

void ArduinoOTAClass::setCACert(const char *ca_cert_pem)
{
    _ca_cert = ca_cert_pem;
}

int ArduinoOTAClass::parseURL()
{
    if (_url == nullptr) {
        _error = Error::OtaDownload;
        return -1;
    }

    const char *p = _url;
    if (strncmp(p, "https://", 8) == 0) {
        _use_tls = true;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        _use_tls = false;
        p += 7;
    } else {
        _error = Error::OtaDownload;
        return -1;
    }

    const char *host_start = p;
    const char *colon = nullptr;
    const char *slash = nullptr;

    while (*p && *p != '/' && *p != ':') p++;
    if (*p == ':') {
        colon = p;
        p++;
        while (*p && *p != '/') p++;
    }
    if (*p == '/') {
        slash = p;
    }

    size_t host_len = colon ? (size_t)(colon - host_start)
                    : slash ? (size_t)(slash - host_start)
                    : strlen(host_start);

    if (host_len == 0 || host_len >= sizeof(_host)) {
        _error = Error::OtaDownload;
        return -1;
    }
    memcpy(_host, host_start, host_len);
    _host[host_len] = '\0';

    _port = colon ? (uint16_t)atoi(colon + 1) : (_use_tls ? 443 : 80);

    if (slash) {
        size_t path_len = strlen(slash);
        if (path_len >= sizeof(_path)) {
            _error = Error::OtaDownload;
            return -1;
        }
        memcpy(_path, slash, path_len + 1);
    } else {
        _path[0] = '/';
        _path[1] = '\0';
    }

    return 0;
}

int ArduinoOTAClass::httpDownload(const char *filepath)
{
    if (parseURL() != 0) return -1;

    ZephyrSocketWrapper sock;
    bool connected;

#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
    if (_use_tls) {
        connected = sock.connectSSL(_host, _port, _ca_cert);
    } else
#endif
    {
        connected = sock.connect(_host, _port);
    }

    if (!connected) {
        _error = Error::OtaDownload;
        return -1;
    }

    char req_buf[512];
    int req_len = snprintf(req_buf, sizeof(req_buf),
        "GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n\r\n",
        _path, _host, _port);

    int ret = sock.send((const uint8_t *)req_buf, req_len);
    if (ret < 0) {
        _error = Error::OtaDownload;
        return -1;
    }

    // Receive headers
    static uint8_t buf[OTA_BUF_SIZE];
    int total_hdr = 0;
    int hdr_end = -1;
    uint32_t content_length = 0;

    while (total_hdr < (int)sizeof(buf) - 1) {
        int n = sock.recv(buf + total_hdr, sizeof(buf) - 1 - total_hdr, 0);
        if (n == -EAGAIN || n == -EWOULDBLOCK) continue;
        if (n <= 0) {
            _error = Error::OtaDownload;
            return -1;
        }
        total_hdr += n;
        buf[total_hdr] = '\0';

        char *eoh = strstr((char *)buf, "\r\n\r\n");
        if (eoh) {
            hdr_end = (eoh - (char *)buf) + 4;
            break;
        }
    }

    if (hdr_end < 0) {
        _error = Error::OtaDownload;
        return -1;
    }

    if (strncmp((char *)buf, "HTTP/1.", 7) != 0) {
        _error = Error::OtaDownload;
        return -1;
    }
    int status_code = atoi((char *)buf + 9);
    if (status_code != 200) {
        _error = Error::OtaDownload;
        return -1;
    }

    const char *cl = strstr((char *)buf, "Content-Length:");
    if (!cl) cl = strstr((char *)buf, "content-length:");
    if (cl) {
        content_length = (uint32_t)atol(cl + 15);
    }
    if (content_length == 0) {
        _error = Error::OtaDownload;
        return -1;
    }

    // Open output file
    struct fs_file_t file;
    fs_file_t_init(&file);
    ret = fs_open(&file, filepath, FS_O_CREATE | FS_O_WRITE);
    if (ret < 0) {
        _error = Error::OtaStorageOpen;
        return -1;
    }

    // Write body data already in buffer
    uint32_t written = 0;
    int body_in_buf = total_hdr - hdr_end;
    if (body_in_buf > 0) {
        ret = fs_write(&file, buf + hdr_end, body_in_buf);
        if (ret < 0) {
            fs_close(&file);
            _error = Error::OtaDownload;
            return -1;
        }
        written += body_in_buf;
    }

    // Stream remaining body
    while (written < content_length) {
        feedWatchdog();
        int n = sock.recv(buf, sizeof(buf), 0);
        if (n == -EAGAIN || n == -EWOULDBLOCK) continue;
        if (n <= 0) break;
        ret = fs_write(&file, buf, n);
        if (ret < 0) {
            fs_close(&file);
            _error = Error::OtaDownload;
            return -1;
        }
        written += n;
    }

    fs_close(&file);

    if (written != content_length) {
        _error = Error::OtaDownload;
        return -1;
    }

    return (int)written;
}

int ArduinoOTAClass::download()
{
    int ret = httpDownload(OTA_TEMP_PATH);
    if (ret < 0) return (int)_error;
    return ret;
}

bool ArduinoOTAClass::isOtaCapable() { return false; }
ArduinoOTAClass::Error ArduinoOTAClass::begin() { return Error::NoOtaStorage; }
ArduinoOTAClass::Error ArduinoOTAClass::update() { return Error::NoOtaStorage; }
void ArduinoOTAClass::reset() {}

int ArduinoOTAClass::decompress()
{
    _error = Error::None;

    // Open temp file
    struct fs_file_t file;
    fs_file_t_init(&file);
    int ret = fs_open(&file, OTA_TEMP_PATH, FS_O_READ);
    if (ret < 0) {
        _error = Error::OtaStorageOpen;
        return (int)_error;
    }

    // Get file size
    fs_seek(&file, 0, FS_SEEK_END);
    off_t file_size = fs_tell(&file);
    fs_seek(&file, 0, FS_SEEK_SET);

    // Read header
    OTAHeader hdr;
    if (fs_read(&file, &hdr, OTA_HEADER_SIZE) != OTA_HEADER_SIZE) {
        fs_close(&file);
        _error = Error::OtaHeaderLength;
        return (int)_error;
    }

    // Check magic
#if defined(OTA_BOARD_MAGIC)
    if (hdr.magic_number != OTA_BOARD_MAGIC) {
        fs_close(&file);
        _error = Error::OtaHeaderCrc;
        return (int)_error;
    }
#endif

    // Validate header length field: hdr.len == file_size - 8
    if ((off_t)hdr.len != file_size - 8) {
        fs_close(&file);
        _error = Error::OtaHeaderLength;
        return (int)_error;
    }

    bool compressed = (hdr.version[0] & OTA_FLAG_COMPRESS) != 0;

    // Verify CRC-32: covers file[8:] (magic + version + payload)
    fs_seek(&file, 8, FS_SEEK_SET);
    uint32_t crc = 0xFFFFFFFF;
    uint8_t buf[512];
    ssize_t n;
    int chunk_count = 0;
    while ((n = fs_read(&file, buf, sizeof(buf))) > 0) {
        crc = crc_update(crc, buf, n);
        if (++chunk_count % 16 == 0)
            feedWatchdog();
    }
    crc ^= 0xFFFFFFFF;

    if (crc != hdr.crc32) {
        fs_close(&file);
        _error = Error::OtaHeaderCrc;
        return (int)_error;
    }

    fs_close(&file);

    int32_t output_size;
    if (compressed) {
        struct fs_file_t in_file, out_file;
        fs_file_t_init(&in_file);
        fs_file_t_init(&out_file);

        ret = fs_open(&in_file, OTA_TEMP_PATH, FS_O_READ);
        if (ret < 0) {
            _error = Error::OtaStorageOpen;
            fs_unlink(OTA_TEMP_PATH);
            return (int)_error;
        }

        ret = fs_open(&out_file, OTA_FILE_PATH, FS_O_CREATE | FS_O_WRITE);
        if (ret < 0) {
            fs_close(&in_file);
            _error = Error::OtaStorageOpen;
            fs_unlink(OTA_TEMP_PATH);
            return (int)_error;
        }

        fs_seek(&in_file, OTA_HEADER_SIZE, FS_SEEK_SET);
        output_size = lzss_decompress(&in_file, &out_file);

        fs_close(&in_file);
        fs_close(&out_file);

        if (output_size < 0) {
            _error = Error::OtaHeaderCrc;
            fs_unlink(OTA_TEMP_PATH);
            return (int)_error;
        }
    } else {
        struct fs_file_t in_file, out_file;
        fs_file_t_init(&in_file);
        fs_file_t_init(&out_file);

        ret = fs_open(&in_file, OTA_TEMP_PATH, FS_O_READ);
        if (ret < 0) {
            _error = Error::OtaStorageOpen;
            fs_unlink(OTA_TEMP_PATH);
            return (int)_error;
        }

        ret = fs_open(&out_file, OTA_FILE_PATH, FS_O_CREATE | FS_O_WRITE);
        if (ret < 0) {
            fs_close(&in_file);
            _error = Error::OtaStorageOpen;
            fs_unlink(OTA_TEMP_PATH);
            return (int)_error;
        }

        fs_seek(&in_file, OTA_HEADER_SIZE, FS_SEEK_SET);

        uint32_t total = 0;
        while ((n = fs_read(&in_file, buf, sizeof(buf))) > 0) {
            if (fs_write(&out_file, buf, n) != n) {
                fs_close(&in_file);
                fs_close(&out_file);
                _error = Error::OtaStorageOpen;
                fs_unlink(OTA_TEMP_PATH);
                return (int)_error;
            }
            total += n;
            if (total % (512 * 16) == 0)
                feedWatchdog();
        }

        fs_close(&in_file);
        fs_close(&out_file);
        output_size = (int32_t)total;
    }

    fs_unlink(OTA_TEMP_PATH);
    _program_length = (uint32_t)output_size;
    return (int)output_size;
}

void ArduinoOTAClass::setFeedWatchdogFunc(void (*func)(void))
{
    _feed_watchdog_func = func;
}

void ArduinoOTAClass::feedWatchdog()
{
    if (_feed_watchdog_func) _feed_watchdog_func();
}

const char* ArduinoOTAClass::errorString()
{
    switch (_error) {
    case Error::None:                return "no error";
    case Error::NoCapableBootloader: return "bootloader not OTA capable";
    case Error::NoOtaStorage:        return "no OTA storage available";
    case Error::OtaStorageInit:      return "OTA storage init failed";
    case Error::OtaStorageOpen:      return "OTA storage open failed";
    case Error::OtaHeaderLength:     return "OTA header length mismatch";
    case Error::OtaHeaderCrc:        return "OTA header CRC mismatch";
    case Error::OtaDownload:         return "OTA download failed";
    default:                         return "unknown error";
    }
}
