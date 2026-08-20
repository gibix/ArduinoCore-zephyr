/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "dfu_cookie.h"

void _on_1200_bps() {
    DFU_COOKIE = DFU_DOUBLE_RESET_MAGIC;
    __DSB();
    NVIC_SystemReset();
}

void initVariant(void) {
    // check the BLUE LED
    /* Set led1 inactive since the Arduino bootloader leaves it active */
    const struct gpio_dt_spec led2 = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
    if (!gpio_is_ready_dt(&led2)) {
        return;
    }

    gpio_pin_configure_dt(&led2, GPIO_OUTPUT_INACTIVE);
}
