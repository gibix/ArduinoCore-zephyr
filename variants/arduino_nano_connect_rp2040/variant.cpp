/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Arduino.h"
#include <stdint.h>

#include "double_tap_cookie.h"

void _on_1200_bps() {
    double_tap_cookie_arm();
    NVIC_SystemReset();
}
