/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * "ota" shell command: download an update over plain HTTP into the staging
 * area (ota.h), then reboot into it.
 *
 *   ota fetch http://<ipv4>[:port]/<path>
 *   ota reboot
 *   ota confirm
 *   ota status
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/shell/shell.h>

#include <ota.h>

#if defined(CONFIG_OTA_BACKEND_MCUBOOT)
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#endif

#define HTTP_TIMEOUT_MS 10000

static uint8_t buf[1024];

struct url {
	char host[16];
	uint16_t port;
	const char *path;
};

static int parse_url(const char *s, struct url *url) {
	const char *host, *end, *colon;

	if (strncmp(s, "http://", 7) != 0) {
		return -EINVAL;
	}
	host = s + 7;
	end = strchr(host, '/');
	url->path = end ? end : "/";
	if (end == NULL) {
		end = host + strlen(host);
	}

	colon = memchr(host, ':', end - host);
	url->port = colon ? atoi(colon + 1) : 80;
	if (colon == NULL) {
		colon = end;
	}
	if (colon - host >= sizeof(url->host) || url->port == 0) {
		return -EINVAL;
	}
	memcpy(url->host, host, colon - host);
	url->host[colon - host] = '\0';
	return 0;
}

static int http_connect(const struct shell *sh, const struct url *url) {
	struct net_sockaddr_in addr = {
		.sin_family = NET_AF_INET,
		.sin_port = net_htons(url->port),
	};
	int sock;

	if (zsock_inet_pton(NET_AF_INET, url->host, &addr.sin_addr) != 1) {
		shell_error(sh, "host must be an IPv4 address: %s", url->host);
		return -EINVAL;
	}

	sock = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);
	if (sock < 0) {
		return -errno;
	}

	if (zsock_connect(sock, (struct net_sockaddr *)&addr, sizeof(addr)) < 0) {
		int err = -errno;

		shell_error(sh, "connect to %s:%u failed: %d", url->host, url->port, err);
		zsock_close(sock);
		return err;
	}

	return sock;
}

/*
 * Receive with a timeout. Polls instead of blocking in recv(), which has been
 * seen to sleep forever with data already queued on the socket.
 */
static ssize_t recv_some(int sock, void *dst, size_t len) {
	struct zsock_pollfd pfd = {.fd = sock, .events = ZSOCK_POLLIN};
	int64_t end = k_uptime_get() + HTTP_TIMEOUT_MS;

	while (k_uptime_get() < end) {
		ssize_t n = zsock_recv(sock, dst, len, ZSOCK_MSG_DONTWAIT);

		if (n >= 0 || errno != EAGAIN) {
			return n;
		}
		(void)zsock_poll(&pfd, 1, 1000);
	}

	errno = ETIMEDOUT;
	return -1;
}

static int send_all(int sock, const char *s) {
	size_t len = strlen(s);

	while (len > 0) {
		ssize_t n = zsock_send(sock, s, len, 0);

		if (n < 0) {
			return -errno;
		}
		s += n;
		len -= n;
	}
	return 0;
}

/*
 * Read the response header into buf. Returns the body length and leaves the
 * body bytes already received at buf + *body_off, *body_len bytes long.
 */
static long read_header(const struct shell *sh, int sock, size_t *body_off, size_t *body_len) {
	size_t used = 0;
	char *end = NULL;
	long length = -1;

	while (end == NULL) {
		ssize_t n;

		if (used == sizeof(buf) - 1) {
			shell_error(sh, "HTTP header too long");
			return -E2BIG;
		}
		n = recv_some(sock, buf + used, sizeof(buf) - 1 - used);
		if (n <= 0) {
			shell_error(sh, "no HTTP response");
			return -EIO;
		}
		used += n;
		buf[used] = '\0';
		end = strstr((char *)buf, "\r\n\r\n");
	}

	if (strncmp((char *)buf, "HTTP/1.", 7) != 0 || atoi((char *)buf + 9) != 200) {
		*strchr((char *)buf, '\r') = '\0';
		shell_error(sh, "HTTP error: %s", (char *)buf);
		return -ENOENT;
	}

	for (char *line = strstr((char *)buf, "\r\n") + 2; line < end;
	     line = strstr(line, "\r\n") + 2) {
		if (strncasecmp(line, "Content-Length:", 15) == 0) {
			length = atol(line + 15);
		}
	}

	if (length <= 0) {
		shell_error(sh, "missing Content-Length");
		return -EINVAL;
	}

	*body_off = (uint8_t *)end + 4 - buf;
	*body_len = used - *body_off;
	return length;
}

static int cmd_fetch(const struct shell *sh, size_t argc, char **argv) {
	char request[160];
	struct url url;
	size_t body_off, body_len;
	long length, received = 0, next_report = 0;
	int64_t start = k_uptime_get();
	int sock, ret;

	if (parse_url(argv[1], &url)) {
		shell_error(sh, "expected http://<ipv4>[:port]/<path>");
		return -EINVAL;
	}

	sock = http_connect(sh, &url);
	if (sock < 0) {
		return sock;
	}

	snprintk(request, sizeof(request),
		 "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", url.path, url.host);
	ret = send_all(sock, request);
	if (ret) {
		goto out;
	}

	length = read_header(sh, sock, &body_off, &body_len);
	if (length < 0) {
		ret = length;
		goto out;
	}

	shell_print(sh, "downloading %ld bytes", length);
	ret = ota_write_begin(OTA_LOADER, length);
	if (ret) {
		shell_error(sh, "cannot stage %ld bytes: %d", length, ret);
		goto out;
	}

	/* The first chunk already sits in buf after the header. */
	memmove(buf, buf + body_off, body_len);
	for (ssize_t n = body_len; received < length;) {
		if (n > 0) {
			ret = ota_write(buf, n);
			if (ret) {
				shell_error(sh, "write failed at %ld: %d", received, ret);
				goto out;
			}
			received += n;
			if (received >= next_report) {
				shell_print(sh, "  %ld / %ld", received, length);
				next_report += 64 * 1024;
			}
		}
		if (received >= length) {
			break;
		}
		n = recv_some(sock, buf, sizeof(buf));
		if (n <= 0) {
			break;
		}
	}

	ret = ota_write_end();
	if (ret) {
		shell_error(sh, "download incomplete (%ld of %ld bytes)", received, length);
		goto out;
	}

	ret = ota_loader_ready();
	if (ret) {
		shell_error(sh, "image rejected: %d", ret);
		goto out;
	}

	shell_print(sh, "%ld bytes in %lld ms, ready: run \"ota reboot\"", received,
		    k_uptime_get() - start);
out:
	zsock_close(sock);
	return ret;
}

static int cmd_reboot(const struct shell *sh, size_t argc, char **argv) {
	shell_print(sh, "rebooting into the update");
	k_msleep(200);
	return ota_loader_start();
}

static int cmd_confirm(const struct shell *sh, size_t argc, char **argv) {
	int ret = ota_confirm();

	if (ret) {
		shell_error(sh, "confirm failed: %d", ret);
	} else {
		shell_print(sh, "running image confirmed");
	}
	return ret;
}

#if defined(CONFIG_OTA_BACKEND_MCUBOOT)
/* From bootutil/image.h */
#define IMAGE_MAGIC          0x96f3b83d
#define IMAGE_TLV_INFO_MAGIC 0x6907
#define IMAGE_TLV_SHA256     0x10

/*
 * Print the image at off in fa: version, size and the start of its SHA-256,
 * which tells images of the same version apart. Returns false if none.
 */
static bool print_image(const struct shell *sh, const struct flash_area *fa, off_t off,
			const char *what) {
	uint8_t hdr[28], tlv[8];
	bool sha = false;
	off_t end;

	if (flash_area_read(fa, off, hdr, sizeof(hdr)) || sys_get_le32(&hdr[0]) != IMAGE_MAGIC) {
		return false;
	}

	/* header + body + protected TLVs, then the TLV info and the TLVs */
	off += sys_get_le16(&hdr[8]) + sys_get_le32(&hdr[12]) + sys_get_le16(&hdr[10]);
	if (flash_area_read(fa, off, tlv, 4) || sys_get_le16(&tlv[0]) != IMAGE_TLV_INFO_MAGIC) {
		return false;
	}

	for (end = off + sys_get_le16(&tlv[2]), off += 4; off + 8 <= end;
	     off += 4 + sys_get_le16(&tlv[2])) {
		if (flash_area_read(fa, off, tlv, sizeof(tlv))) {
			return false;
		}
		if (sys_get_le16(&tlv[0]) == IMAGE_TLV_SHA256) {
			sha = true;
			break;
		}
	}

	/* ih_ver: major, minor, revision (16 bit), build (32 bit) at 20 */
	shell_print(sh, "%s: %u.%u.%u+%u, %u bytes, sha256 %02x%02x%02x%02x%s", what, hdr[20],
		    hdr[21], sys_get_le16(&hdr[22]), sys_get_le32(&hdr[24]),
		    sys_get_le32(&hdr[12]), sha ? tlv[4] : 0, sha ? tlv[5] : 0, sha ? tlv[6] : 0,
		    sha ? tlv[7] : 0, sha ? "" : " (none)");
	return true;
}

static void print_slots(const struct shell *sh) {
	const struct flash_area *fa;
	off_t staged = boot_get_image_start_offset(PARTITION_ID(slot1_partition));
	bool any = false;

	if (flash_area_open(PARTITION_ID(slot0_partition), &fa) == 0) {
		if (!print_image(sh, fa, 0, "slot0 running")) {
			shell_print(sh, "slot0: no image");
		}
		flash_area_close(fa);
	}

	if (flash_area_open(PARTITION_ID(slot1_partition), &fa) == 0) {
		/* An update is staged past the first sector; a swap leaves the
		 * previous image at the start of the slot. */
		any |= print_image(sh, fa, staged, "slot1 staged");
		if (staged != 0) {
			any |= print_image(sh, fa, 0, "slot1 previous");
		}
		if (!any) {
			shell_print(sh, "slot1: no image");
		}
		flash_area_close(fa);
	}
}
#endif

static int cmd_status(const struct shell *sh, size_t argc, char **argv) {
#if defined(CONFIG_OTA_BACKEND_MCUBOOT)
	static const char *const swap[] = {"?", "none", "test", "perm", "revert", "fail"};
	int type = mcuboot_swap_type();

	print_slots(sh);
	shell_print(sh, "running image %s, next boot: %s",
		    boot_is_img_confirmed() ? "confirmed" : "NOT confirmed (test)",
		    type > 0 && type < ARRAY_SIZE(swap) ? swap[type] : "?");
#else
	shell_print(sh, "updates are staged as files");
#endif
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ota_cmds,
	SHELL_CMD_ARG(fetch, NULL, "Download and stage an update: fetch http://<ipv4>[:port]/<path>",
		      cmd_fetch, 2, 0),
	SHELL_CMD_ARG(reboot, NULL, "Reboot and install the staged update", cmd_reboot, 1, 0),
	SHELL_CMD_ARG(confirm, NULL, "Keep the running (test) image", cmd_confirm, 1, 0),
	SHELL_CMD_ARG(status, NULL, "Show the images and the update state", cmd_status, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ota, &ota_cmds, "Sketch and loader updates", NULL);
