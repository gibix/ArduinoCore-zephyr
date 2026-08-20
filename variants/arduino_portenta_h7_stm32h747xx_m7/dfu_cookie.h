/*
 * Copyright (c) 2026 Arduino SA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Handoff word between a running image and MCUboot's serial recovery.
 *
 * MCUboot enters serial recovery when it finds this cookie set at
 * PRE_KERNEL_1 — normally because the user physically double-tapped
 * reset.  Writing the same magic from software and resetting is
 * indistinguishable from a double-tap.
 *
 * The storage is a reserved-memory region declared in dfu_cookie.dtsi.
 */

#pragma once

#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#define DFU_DOUBLE_RESET_MAGIC 0x44524655U /* "DFRU" */

#define DFU_COOKIE_ADDR DT_REG_ADDR(DT_NODELABEL(dfu_cookie))

#define DFU_COOKIE (*(volatile uint32_t *)DFU_COOKIE_ADDR)
