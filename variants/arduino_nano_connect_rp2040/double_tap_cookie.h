/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Double-tap BOOTSEL cookie. Address from double_tap_cookie.dtsi so SFU and
 * loader always agree. Magic matches the Pico SDK convention.
 */

#pragma once

#include <zephyr/devicetree.h>
#include <stdint.h>

#define DOUBLE_TAP_COOKIE_ADDR DT_REG_ADDR(DT_NODELABEL(double_tap_cookie))

#define DOUBLE_TAP_COOKIE ((volatile uint32_t *)DOUBLE_TAP_COOKIE_ADDR)

#define DOUBLE_TAP_MAGIC_0 0xf01681deU
#define DOUBLE_TAP_MAGIC_1 0xbd729b29U
#define DOUBLE_TAP_MAGIC_2 0xd359be7aU

static inline void double_tap_cookie_arm(void)
{
	DOUBLE_TAP_COOKIE[0] = DOUBLE_TAP_MAGIC_0;
	DOUBLE_TAP_COOKIE[1] = DOUBLE_TAP_MAGIC_1;
	DOUBLE_TAP_COOKIE[2] = DOUBLE_TAP_MAGIC_2;
}

static inline void double_tap_cookie_clear(void)
{
	DOUBLE_TAP_COOKIE[0] = 0U;
}

static inline bool double_tap_cookie_is_armed(void)
{
	return DOUBLE_TAP_COOKIE[0] == DOUBLE_TAP_MAGIC_0 &&
	       DOUBLE_TAP_COOKIE[1] == DOUBLE_TAP_MAGIC_1 &&
	       DOUBLE_TAP_COOKIE[2] == DOUBLE_TAP_MAGIC_2;
}
