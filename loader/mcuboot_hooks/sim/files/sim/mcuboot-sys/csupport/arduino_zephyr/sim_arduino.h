/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The Zephyr APIs the Arduino MCUboot hooks (dfu_boot_mode.c, double_tap.c)
 * use, reduced to what the simulator needs: the retention boot mode and the
 * retained RAM survive a reset, the reset cause is chosen by the test, and
 * USB DFU, sleeping and rebooting are recorded instead of happening.
 *
 * Only arduino_boot.c includes this, once, so the state can live here.
 */

#ifndef H_SIM_ARDUINO_
#define H_SIM_ARDUINO_

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* What survives a reset, and what one boot did. Mirrors ArduinoBootIo in
 * mcuboot-sys/src/c.rs. */
struct arduino_boot_io {
    /* In/out: retained across resets. */
    int32_t boot_mode;
    uint8_t cookie[4];
    /* In. */
    uint8_t pin_reset;
    uint8_t press_in_window;
    /* Out. */
    uint8_t dfu_entered;
    uint8_t rebooted;
    uint8_t window_slept;
    uint8_t reset_in_window;
    uint8_t pad[2];
    int32_t boot_rc;
};

static __thread struct arduino_boot_io *sim_io;
static __thread jmp_buf sim_reset_jmp;

#define SIM_RESET_IN_WINDOW 1
#define SIM_REBOOT          2

/* kernel.h, device.h, init.h, devicetree.h */
#ifndef ARG_UNUSED
#define ARG_UNUSED(x) (void)(x)
#endif
#ifndef BUILD_ASSERT
#define BUILD_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
#define SYS_INIT(fn, level, prio)
#define CONFIG_RETAINED_MEM_INIT_PRIORITY 0
#define CONFIG_APPLICATION_INIT_PRIORITY  90

typedef struct { int64_t ticks; } k_timeout_t;
#define K_FOREVER ((k_timeout_t){ .ticks = -1 })

struct device { int unused; };
static const struct device sim_retained_ram;
#define DT_NODELABEL(label) label
#define DEVICE_DT_GET(node) (&sim_retained_ram)

/* A second press of the reset button while the window is open resets the
 * board: control goes back to the start of the boot. */
static inline int32_t k_msleep(int32_t ms)
{
    ARG_UNUSED(ms);
    sim_io->window_slept = 1;
    if (sim_io->press_in_window) {
        longjmp(sim_reset_jmp, SIM_RESET_IN_WINDOW);
    }
    return 0;
}

/* retention/bootmode.h */
enum BOOT_MODE_TYPES {
    BOOT_MODE_TYPE_NORMAL = 0x00,
    BOOT_MODE_TYPE_BOOTLOADER,
};

static inline int bootmode_check(uint8_t boot_mode)
{
    return sim_io->boot_mode == boot_mode ? 1 : 0;
}

static inline int bootmode_set(uint8_t boot_mode)
{
    sim_io->boot_mode = boot_mode;
    return 0;
}

static inline int bootmode_clear(void)
{
    sim_io->boot_mode = BOOT_MODE_TYPE_NORMAL;
    return 0;
}

/* sys/reboot.h */
#define SYS_REBOOT_WARM 0
#define SYS_REBOOT_COLD 1

static inline void sys_reboot(int type)
{
    ARG_UNUSED(type);
    sim_io->rebooted = 1;
    longjmp(sim_reset_jmp, SIM_REBOOT);
}

/* drivers/retained_mem.h: the double-tap cookie. */
static inline int retained_mem_read(const struct device *dev, size_t offset,
                                    uint8_t *buffer, size_t size)
{
    ARG_UNUSED(dev);
    memcpy(buffer, &sim_io->cookie[offset], size);
    return 0;
}

static inline int retained_mem_write(const struct device *dev, size_t offset,
                                     const uint8_t *buffer, size_t size)
{
    ARG_UNUSED(dev);
    memcpy(&sim_io->cookie[offset], buffer, size);
    return 0;
}

/* drivers/hwinfo.h */
#define CONFIG_HWINFO
#define RESET_PIN      (1u << 0)
#define RESET_SOFTWARE (1u << 1)

static inline int hwinfo_get_reset_cause(uint32_t *cause)
{
    *cause = sim_io->pin_reset ? RESET_PIN : RESET_SOFTWARE;
    return 0;
}

/* usbd_dfu.h: the download is staged in the secondary slot by the test. */
static inline int boot_usb_dfu_enable(void)
{
    return 0;
}

static inline void boot_usb_dfu_wait(k_timeout_t delay)
{
    ARG_UNUSED(delay);
    sim_io->dfu_entered = 1;
}

#endif /* H_SIM_ARDUINO_ */
