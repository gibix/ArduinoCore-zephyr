/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ArduinoOTA.h"

#include <zephyr/net/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>

#include <cmsis_core.h>
#include <stm32h7xx.h>

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define OTA_FILE_PATH    "/ota:/UPDATE.BIN"
#define OTA_BUF_SIZE     4096

#define OTA_MAGIC        0x07AA
#define OTA_STORAGE_TYPE 0xA4   // QSPI_FLASH | FATFS | MBR
#define OTA_MBR_PART     2

ArduinoOTAClass ArduinoOTA;

void ArduinoOTAClass::setURL(const char *url)
{
    _url = url;
    _error = nullptr;
}

int ArduinoOTAClass::begin()
{
    _program_length = 0;
    _error = nullptr;
    return 0;
}

int ArduinoOTAClass::parseURL()
{
    // Expect "http://host:port/path" or "http://host/path"
    if (_url == nullptr) {
        _error = "URL not set";
        return -1;
    }

    const char *p = _url;
    if (strncmp(p, "http://", 7) != 0) {
        _error = "only http:// URLs supported";
        return -1;
    }
    p += 7;

    // Extract host (and optional port)
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

    // Host
    size_t host_len;
    if (colon) {
        host_len = colon - host_start;
    } else if (slash) {
        host_len = slash - host_start;
    } else {
        host_len = strlen(host_start);
    }

    if (host_len == 0 || host_len >= sizeof(_host)) {
        _error = "host too long or empty";
        return -1;
    }
    memcpy(_host, host_start, host_len);
    _host[host_len] = '\0';

    // Port
    if (colon) {
        _port = (uint16_t)atoi(colon + 1);
    } else {
        _port = 80;
    }

    // Path
    if (slash) {
        size_t path_len = strlen(slash);
        if (path_len >= sizeof(_path)) {
            _error = "path too long";
            return -1;
        }
        memcpy(_path, slash, path_len + 1);
    } else {
        _path[0] = '/';
        _path[1] = '\0';
    }

    return 0;
}

int ArduinoOTAClass::download()
{
    if (parseURL() != 0) {
        return -1;
    }

    // Resolve hostname
    struct addrinfo hints = {};
    struct addrinfo *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", _port);

    int ret = getaddrinfo(_host, port_str, &hints, &res);
    if (ret != 0 || res == nullptr) {
        _error = "DNS resolution failed";
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        _error = "socket() failed";
        return -1;
    }

    ret = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (ret < 0) {
        zsock_close(sock);
        _error = "connect() failed";
        return -1;
    }

    // Send HTTP GET request
    char req_buf[512];
    int req_len = snprintf(req_buf, sizeof(req_buf),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Connection: close\r\n"
        "\r\n",
        _path, _host, _port);

    ret = send(sock, req_buf, req_len, 0);
    if (ret < 0) {
        zsock_close(sock);
        _error = "send() failed";
        return -1;
    }

    // Receive HTTP response headers
    // Read into buffer, find end of headers (\r\n\r\n)
    static uint8_t buf[OTA_BUF_SIZE];
    int total_hdr = 0;
    int hdr_end = -1;
    uint32_t content_length = 0;

    while (total_hdr < (int)sizeof(buf) - 1) {
        ssize_t n = recv(sock, buf + total_hdr, sizeof(buf) - 1 - total_hdr, 0);
        if (n <= 0) {
            zsock_close(sock);
            _error = "recv() failed during headers";
            return -1;
        }
        total_hdr += n;
        buf[total_hdr] = '\0';

        // Look for end of headers
        char *eoh = strstr((char *)buf, "\r\n\r\n");
        if (eoh) {
            hdr_end = (eoh - (char *)buf) + 4;
            break;
        }
    }

    if (hdr_end < 0) {
        zsock_close(sock);
        _error = "headers too large or missing";
        return -1;
    }

    // Check HTTP status
    if (strncmp((char *)buf, "HTTP/1.", 7) != 0) {
        zsock_close(sock);
        _error = "invalid HTTP response";
        return -1;
    }
    int status_code = atoi((char *)buf + 9);
    if (status_code != 200) {
        zsock_close(sock);
        _error = "HTTP status not 200";
        return -1;
    }

    // Parse Content-Length
    const char *cl = strstr((char *)buf, "Content-Length:");
    if (!cl) cl = strstr((char *)buf, "content-length:");
    if (cl) {
        content_length = (uint32_t)atol(cl + 15);
    }
    if (content_length == 0) {
        zsock_close(sock);
        _error = "missing or zero Content-Length";
        return -1;
    }

    // Open output file
    struct fs_file_t file;
    fs_file_t_init(&file);
    ret = fs_open(&file, OTA_FILE_PATH, FS_O_CREATE | FS_O_WRITE);
    if (ret < 0) {
        zsock_close(sock);
        _error = "fs_open() failed";
        return -1;
    }

    // Write any body data already received after headers
    uint32_t written = 0;
    int body_in_buf = total_hdr - hdr_end;
    if (body_in_buf > 0) {
        ret = fs_write(&file, buf + hdr_end, body_in_buf);
        if (ret < 0) {
            fs_close(&file);
            zsock_close(sock);
            _error = "fs_write() failed";
            return -1;
        }
        written += body_in_buf;
    }

    // Stream remaining body
    while (written < content_length) {
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        ret = fs_write(&file, buf, n);
        if (ret < 0) {
            fs_close(&file);
            zsock_close(sock);
            _error = "fs_write() failed";
            return -1;
        }
        written += n;
    }

    fs_close(&file);
    zsock_close(sock);

    if (written != content_length) {
        _error = "incomplete download";
        return -1;
    }

    _program_length = content_length;
    return 0;
}

void ArduinoOTAClass::update()
{
    uint32_t rtc_base = (uint32_t)&(RTC->BKP0R);

    // DR0: OTA magic
    *(__IO uint32_t *)(rtc_base + RTC_BKP_DR0 * 4U) = OTA_MAGIC;
    // DR1: storage type (QSPI_FLASH | FATFS | MBR)
    *(__IO uint32_t *)(rtc_base + RTC_BKP_DR1 * 4U) = OTA_STORAGE_TYPE;
    // DR2: MBR partition index
    *(__IO uint32_t *)(rtc_base + RTC_BKP_DR2 * 4U) = OTA_MBR_PART;
    // DR3: program length
    *(__IO uint32_t *)(rtc_base + RTC_BKP_DR3 * 4U) = _program_length;

    NVIC_SystemReset();
}

const char* ArduinoOTAClass::errorString()
{
    return _error ? _error : "no error";
}
