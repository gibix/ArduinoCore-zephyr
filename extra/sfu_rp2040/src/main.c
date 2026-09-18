/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SFU for RP2040: ROM -> boot2 -> SFU -> loader -> sketch.
 * Checks /ota:/UPDATE.BIN, flashes it if present, then jumps to loader.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/irq.h>
#include <cmsis_core.h>
#include <pico/bootrom.h>

#include <double_tap_cookie.h>

#define UPDATE_FILE_PATH "/ota:/UPDATE.BIN"

#define CODE_PARTITION_ID  FIXED_PARTITION_ID(code_partition)
#define SKETCH_PARTITION_ID FIXED_PARTITION_ID(user_sketch)

/* Arm at PRE_KERNEL_1 so the dead zone before the cookie check is minimal. */
#define DOUBLE_TAP_WINDOW_MS 500

#define NANO_RP2040_LED_PIN 6

static bool double_tap_latched;

static int double_tap_arm(void)
{
	double_tap_latched = double_tap_cookie_is_armed();

	if (double_tap_latched) {
		double_tap_cookie_clear();
	} else {
		double_tap_cookie_arm();
	}

	return 0;
}
SYS_INIT(double_tap_arm, PRE_KERNEL_1, 0);

/* XIP base address for the RP2040 */
#define XIP_BASE           0x10000000
#define LOADER_VECTOR_ADDR (XIP_BASE + FIXED_PARTITION_OFFSET(code_partition))

#define CHUNK_SIZE         4096

static void jump_to_loader(void)
{
	uint32_t *vtor = (uint32_t *)LOADER_VECTOR_ADDR;
	uint32_t sp = vtor[0];
	uint32_t reset_handler = vtor[1];

	__disable_irq();

	/* A stale PendSV hangs the loader's first context switch. */
	for (int i = 0; i < (CONFIG_NUM_IRQS + 31) / 32; i++) {
		NVIC->ICER[i] = 0xFFFFFFFF;
		NVIC->ICPR[i] = 0xFFFFFFFF;
	}

	sys_clock_disable();
#ifdef SCB_ICSR_STTNS_Msk
	SCB->ICSR = (SCB->ICSR & SCB_ICSR_STTNS_Msk) |
		    SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
#else
	SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
#endif

	/* Switch from PSP back to MSP so __set_MSP writes the live SP. */
	__set_CONTROL(0);
	__ISB();

#if defined(CONFIG_ARM_MPU)
	MPU->CTRL = 0;
	__DSB();
	__ISB();
#endif

	SCB->VTOR = LOADER_VECTOR_ADDR;
	__DSB();
	__ISB();

	__set_MSP(sp);
	__ISB();

	((void (*)(void))reset_handler)();
}

static int apply_update(const char *path)
{
	struct fs_file_t file;
	struct fs_dirent entry;
	const struct flash_area *code_fa;
	const struct flash_area *sketch_fa;
	uint8_t buf[CHUNK_SIZE];
	off_t write_offset;
	ssize_t n;
	int rc;

	if (fs_stat(path, &entry) != 0) {
		return -ENOENT;
	}

	printk("SFU: update found (%ld bytes)\n", (long)entry.size);

	rc = flash_area_open(CODE_PARTITION_ID, &code_fa);
	if (rc) {
		printk("SFU: can't open code partition (%d)\n", rc);
		return rc;
	}

	if (entry.size == 0 ||
	    (size_t)entry.size > code_fa->fa_size + FIXED_PARTITION_SIZE(user_sketch)) {
		printk("SFU: invalid update size\n");
		flash_area_close(code_fa);
		return -EINVAL;
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_READ);
	if (rc < 0) {
		printk("SFU: failed to open %s (%d)\n", path, rc);
		flash_area_close(code_fa);
		return rc;
	}

	printk("SFU: erasing code_partition (%u bytes)\n",
	       (unsigned)code_fa->fa_size);
	rc = flash_area_erase(code_fa, 0, code_fa->fa_size);
	if (rc) {
		printk("SFU: erase code failed (%d)\n", rc);
		goto out;
	}

	/* Bundle: file larger than code_partition spills into user_sketch. */
	if ((size_t)entry.size > code_fa->fa_size) {
		rc = flash_area_open(SKETCH_PARTITION_ID, &sketch_fa);
		if (rc) {
			printk("SFU: can't open sketch partition (%d)\n", rc);
			goto out;
		}
		printk("SFU: erasing user_sketch (%u bytes)\n",
		       (unsigned)sketch_fa->fa_size);
		rc = flash_area_erase(sketch_fa, 0, sketch_fa->fa_size);
		flash_area_close(sketch_fa);
		if (rc) {
			printk("SFU: erase sketch failed (%d)\n", rc);
			goto out;
		}
	}

	printk("SFU: writing update...\n");
	write_offset = 0;
	while ((n = fs_read(&file, buf, sizeof(buf))) > 0) {
		if (n % 4 != 0) {
			memset(buf + n, 0xff, 4 - (n % 4));
			n = (n + 3) & ~3;
		}

		/* Overflow into user_sketch once code_partition is full. */
		if ((size_t)write_offset < code_fa->fa_size) {
			rc = flash_area_write(code_fa, write_offset, buf, n);
		} else {
			rc = flash_area_open(SKETCH_PARTITION_ID, &sketch_fa);
			if (rc == 0) {
				rc = flash_area_write(sketch_fa,
					write_offset - code_fa->fa_size,
					buf, n);
				flash_area_close(sketch_fa);
			}
		}

		if (rc) {
			printk("SFU: write failed at offset 0x%lx (%d)\n",
			       (long)write_offset, rc);
			goto out;
		}
		write_offset += n;
	}

	printk("SFU: wrote %ld bytes\n", (long)write_offset);
	fs_unlink(path);

out:
	fs_close(&file);
	flash_area_close(code_fa);
	return rc;
}

int main(void)
{
	int rc;

	/* Double-tap latched at PRE_KERNEL_1. */
	if (double_tap_latched) {
		reset_usb_boot(1 << NANO_RP2040_LED_PIN, 0);
		/* never returns */
	}

	/* Hold the double-tap window, then disarm. */
	k_msleep(DOUBLE_TAP_WINDOW_MS);
	double_tap_cookie_clear();

	printk("SFU: checking for update\n");

	rc = apply_update(UPDATE_FILE_PATH);

	if (rc == 0) {
		printk("SFU: update complete, rebooting\n");
		sys_reboot(SYS_REBOOT_COLD);
	} else if (rc != -ENOENT) {
		printk("SFU: update failed (%d)\n", rc);
	}

	printk("SFU: booting loader at 0x%x\n", LOADER_VECTOR_ADDR);
	jump_to_loader();

	return 0;
}
