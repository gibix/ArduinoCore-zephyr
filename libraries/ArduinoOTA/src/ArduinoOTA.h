/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

class ArduinoOTAClass {
public:
    void setURL(const char *url);
    int begin();
    int download();
    void update();
    const char* errorString();

private:
    int parseURL();

    const char *_url = nullptr;
    char _host[128];
    char _path[256];
    uint16_t _port = 80;
    uint32_t _program_length = 0;
    const char *_error = nullptr;
};

extern ArduinoOTAClass ArduinoOTA;
