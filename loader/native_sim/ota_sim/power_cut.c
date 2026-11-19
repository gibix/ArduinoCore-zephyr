/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Power loss at a chosen flash operation, for MCUboot and the loader of the
 * OTA simulator. Every flash_write() and flash_erase() call on the flash
 * simulator is an operation, numbered by the simulator's own statistics
 * (flash_write_calls + flash_erase_calls). With --power_cut_after=<n>,
 * operation n is not done (with --power_cut_torn, only its first half is)
 * and the process ends with exit code 102, like the board losing power. A
 * torn operation too short to be cut inside is done whole, and the power is
 * lost before the next one.
 *
 * The byte write and unit erase callbacks of the simulator see each
 * operation as it happens, whoever calls it (flash_area_*, stream_flash).
 * The count so far is printed by mcuboot_sim_boot.c and mcuboot_sim_loader.c
 * (ops=<n>), so a runner can cut at every operation of a boot in turn.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash/flash_simulator.h>
#include <zephyr/init.h>
#include <zephyr/stats/stats.h>
#include <zephyr/sys/printk.h>
#include <posix_board_if.h>
#include <posix_native_task.h>
#include <cmdline.h>

#define SIM_EXIT_POWER_CUT 102

#define FLASH_NODE  DT_NODELABEL(flash0)
#define ERASE_UNIT  DT_PROP(FLASH_NODE, erase_block_size)
#define ERASE_VALUE 0xff

static const struct device *const flash_dev = DEVICE_DT_GET(DT_PARENT(FLASH_NODE));

static unsigned int cut_after;
static bool cut_torn;

/* Counters of the flash simulator statistics */
static const uint32_t *write_calls;
static const uint32_t *erase_calls;

/* Bytes or units of the current operation done so far */
static unsigned int op_seen;
static unsigned int op_done;

unsigned int sim_flash_ops(void)
{
	return write_calls && erase_calls ? *write_calls + *erase_calls : 0;
}

static void power_cut(const char *what, off_t off)
{
	/* Parsed by ota_sim.py: keep the format. */
	printk("SIM-POWER-CUT: op=%u %s at 0x%lx after %u\n", sim_flash_ops(), what,
	       (unsigned long)off, op_done);
	posix_exit(SIM_EXIT_POWER_CUT);
}

/* Account one byte or unit of the current operation; true to cut now. */
static bool cut_here(unsigned int limit)
{
	unsigned int op = sim_flash_ops();

	if (op != op_seen) {
		/* A torn operation too short to be cut inside (one sector, one
		 * write block) is cut right after it, before this one starts. */
		if (cut_torn && op_seen == cut_after) {
			power_cut("after", 0);
		}
		op_seen = op;
		op_done = 0;
	}
	if (op != cut_after || op_done < limit) {
		op_done++;
		return false;
	}
	return true;
}

static int write_byte(const struct device *dev, off_t offset, uint8_t data)
{
	ARG_UNUSED(dev);

	/* A torn write keeps its first 8-byte write blocks (half of a 16-byte
	 * trailer write, a few blocks of a larger one). */
	if (cut_here(cut_torn ? 8 : 0)) {
		power_cut(cut_torn ? "torn-write" : "write", offset);
	}
	return data;
}

static int erase_unit(const struct device *dev, off_t unit_offset)
{
	size_t size;
	uint8_t *mem = flash_simulator_get_memory(dev, &size);

	/* A torn erase leaves the first sector of a multi-sector erase done. */
	if (cut_here(cut_torn ? 1 : 0)) {
		power_cut(cut_torn ? "torn-erase" : "erase", unit_offset);
	}
	/* With this callback set, the simulator leaves the erase to us. */
	memset(mem + unit_offset, ERASE_VALUE, ERASE_UNIT);
	return 0;
}

static const struct flash_simulator_cb power_cut_cb = {
	.write_byte = write_byte,
	.erase_unit = erase_unit,
};

static int find_counter(struct stats_hdr *hdr, void *arg, const char *name, uint16_t off)
{
	ARG_UNUSED(arg);

	if (!strcmp(name, "flash_write_calls")) {
		write_calls = (const uint32_t *)((const uint8_t *)hdr + off);
	} else if (!strcmp(name, "flash_erase_calls")) {
		erase_calls = (const uint32_t *)((const uint8_t *)hdr + off);
	}
	return 0;
}

static int power_cut_init(void)
{
	struct stats_hdr *stats = stats_group_find("flash_sim_stats");

	if (stats == NULL || stats->s_size != sizeof(uint32_t)) {
		printk("SIM-POWER-CUT: no flash simulator statistics\n");
		posix_exit(1);
	}
	(void)stats_walk(stats, find_counter, NULL);
	if (write_calls == NULL || erase_calls == NULL) {
		printk("SIM-POWER-CUT: no flash simulator call counters\n");
		posix_exit(1);
	}
	flash_simulator_set_callbacks(flash_dev, &power_cut_cb);
	return 0;
}

/* After the flash simulator (POST_KERNEL, FLASH_INIT_PRIORITY) registered
 * its statistics, before the flash users (MCUboot's main(), the loader's
 * APPLICATION init). */
BUILD_ASSERT(CONFIG_FLASH_INIT_PRIORITY < 99, "power_cut_init() must run after the flash");
SYS_INIT(power_cut_init, POST_KERNEL, 99);

static void power_cut_options(void)
{
	static struct args_struct_t options[] = {
		{.option = "power_cut_after",
		 .name = "n",
		 .type = 'u',
		 .dest = (void *)&cut_after,
		 .descript = "Lose power at flash operation n (1 = the first), exit code 102"},
		{.is_switch = true,
		 .option = "power_cut_torn",
		 .type = 'b',
		 .dest = (void *)&cut_torn,
		 .descript = "The operation cut by power_cut_after is done in part"},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(options);
}

NATIVE_TASK(power_cut_options, PRE_BOOT_1, 2);
