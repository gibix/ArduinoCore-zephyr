/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sketch of the MCUboot OTA simulator tests (ota_sim.py): runs the line
 * commands it reads on Serial and prints one result line for each.
 *
 *   update <size>  stream an image with ota_write(), <size> bytes in chunks
 *                  of at most CHUNK; "ack <n>" after each chunk, then
 *                  "staged <rc>" once ota_loader_ready() returned
 *   start          ota_loader_start(): reboot into MCUboot
 *   confirm        ota_confirm()
 *   feed           ota_feed_watchdog()
 *   sleep <ms>     delay()
 *   exit <code>    exit()
 *
 * The Serial RX buffer drops what does not fit, so the sender waits for each
 * "ack" before sending the next chunk.
 */

#include <ota.h>

#define CHUNK 512

static char line[64];
static size_t line_len;
static uint8_t chunk[CHUNK];

static void reply(const char *what, long value) {
	Serial.print(what);
	Serial.print(' ');
	Serial.println(value);
}

/* Read exactly len bytes, waiting as long as needed: the sender paces. */
static void read_exact(uint8_t *buf, size_t len) {
	size_t got = 0;

	while (got < len) {
		int c = Serial.read();

		if (c < 0) {
			delay(1);
			continue;
		}
		buf[got++] = (uint8_t)c;
	}
}

static void update(long size) {
	int rc = ota_write_begin(OTA_LOADER, (size_t)size);

	if (rc != 0) {
		reply("staged", rc);
		return;
	}
	reply("ack", 0);

	for (long done = 0; done < size;) {
		size_t n = min((long)CHUNK, size - done);

		read_exact(chunk, n);
		rc = ota_write(chunk, n);
		if (rc != 0) {
			reply("staged", rc);
			return;
		}
		done += n;
		reply("ack", done);
	}

	rc = ota_write_end();
	if (rc == 0) {
		rc = ota_loader_ready();
	}
	reply("staged", rc);
}

static void run(char *cmd) {
	char *arg = strchr(cmd, ' ');
	long value = arg ? atol(arg + 1) : 0;

	if (arg) {
		*arg = '\0';
	}

	if (!strcmp(cmd, "update")) {
		update(value);
	} else if (!strcmp(cmd, "start")) {
		reply("start", ota_loader_start());
	} else if (!strcmp(cmd, "confirm")) {
		reply("confirm", ota_confirm());
	} else if (!strcmp(cmd, "feed")) {
		reply("feed", ota_feed_watchdog());
	} else if (!strcmp(cmd, "sleep")) {
		delay(value);
		reply("slept", value);
	} else if (!strcmp(cmd, "exit")) {
		reply("exit", value);
		exit(value);
	} else {
		reply("unknown", 0);
	}
}

void setup() {
	Serial.begin(115200);
	Serial.println("ready");
}

void loop() {
	int c = Serial.read();

	if (c < 0) {
		delay(1);
		return;
	}
	if (c == '\r') {
		return;
	}
	if (c != '\n') {
		if (line_len < sizeof(line) - 1) {
			line[line_len++] = (char)c;
		}
		return;
	}
	line[line_len] = '\0';
	line_len = 0;
	run(line);
}
