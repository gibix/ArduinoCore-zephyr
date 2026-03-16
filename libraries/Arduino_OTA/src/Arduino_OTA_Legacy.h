/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <Arduino_OTA.h>

class ArduinoOTALegacyClass: public ArduinoOTAClass {
public:
    bool isOtaCapable() override;

    Error begin() override;
    Error update() override;
    void reset() override;
};
