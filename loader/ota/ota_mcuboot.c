/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * OTA through MCUboot: the update is a full MCUboot image (loader + sketch,
 * hash-only or signed) streamed into slot1. MCUboot verifies it on the next
 * boot; an image it rejects is erased and the running one keeps booting. In
 * swap modes the new image runs as a test and is swapped back on the next
 * reset unless confirmed (ota_confirm() or the loader's confirm thread); in
 * overwrite-only it replaces slot0 for good. Sketch and loader updates are the
 * same image.
 */

#include <errno.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>

#include <ota.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ota_mcuboot, CONFIG_OTA_LOG_LEVEL);

#define SLOT1_ID PARTITION_ID(slot1_partition)

/* From bootutil/image.h, which is not usable outside MCUboot. */
#define IMAGE_MAGIC               0x96f3b83d
#define IMAGE_HEADER_SIZE         32
#define IMAGE_TLV_INFO_MAGIC      0x6907
#define IMAGE_TLV_PROT_INFO_MAGIC 0x6908
#define IMAGE_TLV_SHA256          0x10
#define IMAGE_TLV_ECDSA_SIG       0x22

static struct flash_img_context img_ctx;
/* Where the image starts in slot1: one sector in with swap-using-offset. */
static off_t img_start;
static size_t expected;
static size_t received;
static bool writing;
static bool staged;

/* Erase the trailer page(s): boot_request_upgrade() writes there, and
 * progressive erase only clears the pages the image itself covers. */
static int erase_trailer(const struct flash_area *fa) {
	struct flash_pages_info page;
	off_t off = boot_get_trailer_status_offset(fa->fa_size);
	int rc;

	rc = flash_get_page_info_by_offs(flash_area_get_device(fa), fa->fa_off + off, &page);
	if (rc) {
		return rc;
	}

	off = page.start_offset - fa->fa_off;
	return flash_area_erase(fa, off, fa->fa_size - off);
}

int ota_write_begin(enum ota_type type, size_t size) {
	const struct flash_area *fa;
	int rc;

	ARG_UNUSED(type);
	writing = false;
	staged = false;

	rc = flash_area_open(SLOT1_ID, &fa);
	if (rc) {
		return rc;
	}

	img_start = boot_get_image_start_offset(SLOT1_ID);

	/* The image must end before the trailer MCUboot keeps at the slot's end. */
	if (size < IMAGE_HEADER_SIZE ||
	    img_start + size > (size_t)boot_get_trailer_status_offset(fa->fa_size)) {
		LOG_ERR("image of %u bytes does not fit slot1", (unsigned)size);
		flash_area_close(fa);
		return -EFBIG;
	}

	rc = erase_trailer(fa);
	flash_area_close(fa);
	if (rc) {
		LOG_ERR("trailer erase failed: %d", rc);
		return rc;
	}

	rc = flash_img_init(&img_ctx);
	if (rc) {
		return rc;
	}

	expected = size;
	received = 0;
	writing = true;
	return 0;
}

int ota_write(const void *buf, size_t len) {
	int rc;

	if (!writing) {
		return -EBADF;
	}

	if (received + len > expected) {
		LOG_ERR("more data than announced (%u bytes)", (unsigned)expected);
		writing = false;
		return -EFBIG;
	}

	rc = flash_img_buffered_write(&img_ctx, buf, len, false);
	if (rc) {
		LOG_ERR("slot1 write failed: %d", rc);
		writing = false;
		return rc;
	}

	received += len;
	return 0;
}

int ota_write_end(void) {
	size_t written;
	int rc;

	if (!writing) {
		return -EBADF;
	}

	writing = false;
	rc = flash_img_buffered_write(&img_ctx, NULL, 0, true);
	if (rc) {
		LOG_ERR("slot1 flush failed: %d", rc);
		return rc;
	}

	written = flash_img_bytes_written(&img_ctx);
	if (written != expected) {
		LOG_ERR("wrote %u bytes, expected %u", (unsigned)written, (unsigned)expected);
		return -EIO;
	}

	staged = true;
	return 0;
}

static int read_u16_pair(const struct flash_area *fa, off_t off, uint16_t *a, uint16_t *b) {
	uint8_t buf[4];
	int rc = flash_area_read(fa, off, buf, sizeof(buf));

	*a = sys_get_le16(&buf[0]);
	*b = sys_get_le16(&buf[2]);
	return rc;
}

/*
 * Structural check of the staged image, so a wrong file fails here instead of
 * being silently dropped by MCUboot. The hash and signature themselves are
 * verified by MCUboot.
 */
static int check_image(const struct flash_area *fa) {
	uint8_t hdr[16];
	uint16_t magic, len, type;
	bool sha256 = false, ecdsa = false;
	uint32_t img_size;
	uint16_t hdr_size, prot_size;
	off_t off, end;
	int rc;

	rc = flash_area_read(fa, img_start, hdr, sizeof(hdr));
	if (rc) {
		return rc;
	}

	if (sys_get_le32(&hdr[0]) != IMAGE_MAGIC) {
		LOG_ERR("not an MCUboot image (magic 0x%08x)", sys_get_le32(&hdr[0]));
		return -EINVAL;
	}

	hdr_size = sys_get_le16(&hdr[8]);
	prot_size = sys_get_le16(&hdr[10]);
	img_size = sys_get_le32(&hdr[12]);
	off = (off_t)hdr_size + img_size;

	if (hdr_size < IMAGE_HEADER_SIZE || off + prot_size + 4 > (off_t)expected) {
		LOG_ERR("image header does not match the %u bytes staged", (unsigned)expected);
		return -EINVAL;
	}

	if (prot_size) {
		rc = read_u16_pair(fa, img_start + off, &magic, &len);
		if (rc || magic != IMAGE_TLV_PROT_INFO_MAGIC || len != prot_size) {
			LOG_ERR("bad protected TLV area");
			return rc ? rc : -EINVAL;
		}
		off += prot_size;
	}

	rc = read_u16_pair(fa, img_start + off, &magic, &len);
	end = off + len;
	if (rc || magic != IMAGE_TLV_INFO_MAGIC || end > (off_t)expected) {
		LOG_ERR("bad TLV area");
		return rc ? rc : -EINVAL;
	}

	for (off += 4; off + 4 <= end; off += 4 + len) {
		rc = read_u16_pair(fa, img_start + off, &type, &len);
		if (rc) {
			return rc;
		}
		sha256 |= type == IMAGE_TLV_SHA256;
		ecdsa |= type == IMAGE_TLV_ECDSA_SIG;
	}

	if (!sha256) {
		LOG_ERR("image has no SHA-256");
		return -EINVAL;
	}

	if (IS_ENABLED(CONFIG_OTA_MCUBOOT_SIGNED) && !ecdsa) {
		LOG_ERR("image is not signed, MCUboot would reject it");
		return -EPERM;
	}

	return 0;
}

static int ota_ready(void) {
	const struct flash_area *fa;
	int rc;

	if (!staged) {
		LOG_WRN("tried to perform an ota without an image staged");
		return -ENOENT;
	}

	rc = flash_area_open(SLOT1_ID, &fa);
	if (rc) {
		return rc;
	}

	rc = check_image(fa);
	flash_area_close(fa);
	if (rc) {
		return rc;
	}

	/* Test: in swap modes the image must confirm itself or be reverted;
	 * overwrite-only installs it for good either way. */
	rc = boot_request_upgrade(BOOT_UPGRADE_TEST);
	if (rc) {
		LOG_ERR("upgrade request failed: %d", rc);
		return rc;
	}

	LOG_INF("OTA marked as ready");
	return 0;
}

static int ota_start(void) {
	LOG_INF("OTA started, reboot in progress");

	sys_reboot(SYS_REBOOT_COLD);
	return 0; // unreachable
}

int ota_sketch_ready(void) {
	return ota_ready();
}

int ota_sketch_start(void) {
	return ota_start();
}

int ota_loader_ready(void) {
	return ota_ready();
}

int ota_loader_start(void) {
	return ota_start();
}

int ota_confirm(void) {
	if (boot_is_img_confirmed()) {
		return 0;
	}

	return boot_write_img_confirmed();
}
