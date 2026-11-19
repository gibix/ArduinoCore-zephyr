#!/usr/bin/env bash
# Copyright (c) Arduino s.r.l. and/or its affiliated companies
# SPDX-License-Identifier: Apache-2.0
#
# Run the MCUboot OTA simulator tests: build MCUboot for native_sim (if
# needed), compile the ota_flow sketch for native_sim_ota, then run the
# scenarios of ota_sim.py. Exits with code 1 if any scenario failed.
#
# Usage: run_tests.sh [-w <work dir>] [<scenario>...]
#
# Run from the root of the core, with the native_sim_ota loader built
# (extra/build.sh native_sim_ota) and the core installed as FQBN.
#
# Environment:
#   FQBN               default: arduino-git:zephyr:native_sim_ota
#   ARDUINO_CLI_FLAGS  extra arguments for 'arduino-cli compile'

set -eu

TESTS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SIM_DIR=$(dirname "$TESTS_DIR")
CORE_DIR=$(cd "$SIM_DIR/../../.." && pwd)
TOPDIR=$(cd "$CORE_DIR/.." && pwd)
FQBN=${FQBN:-arduino-git:zephyr:native_sim_ota}
MCUBOOT_BUILD=$CORE_DIR/build/ota_sim/mcuboot
LOADER=$CORE_DIR/firmwares/zephyr-native_sim_ota_native_64.exe
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

if [ ! -x "$LOADER" ] ; then
	echo "No $LOADER: run extra/build.sh native_sim_ota first" >&2
	exit 2
fi

echo "Building MCUboot for native_sim"
if [ -f "$MCUBOOT_BUILD/build.ninja" ] ; then
	west build -d "$MCUBOOT_BUILD" > "$OUT/mcuboot.log" 2>&1
else
	west build -b native_sim/native/64 -d "$MCUBOOT_BUILD" "$TOPDIR/bootloader/mcuboot/boot/zephyr" -- \
		-DEXTRA_CONF_FILE="$SIM_DIR/mcuboot.conf" \
		-DEXTRA_DTC_OVERLAY_FILE="$SIM_DIR/ota_sim.overlay" \
		"-DEXTRA_ZEPHYR_MODULES=$CORE_DIR/loader/mcuboot_hooks;$SIM_DIR" > "$OUT/mcuboot.log" 2>&1
fi || { tail -n 50 "$OUT/mcuboot.log" ; exit 1 ; }

echo "Compiling ota_flow for $FQBN"
# shellcheck disable=SC2086 # ARDUINO_CLI_FLAGS is a list of arguments
arduino-cli compile --no-color -b "$FQBN" ${ARDUINO_CLI_FLAGS:-} --build-path "$OUT/sketch" \
	"$TESTS_DIR/ota_flow" > "$OUT/compile.log" 2>&1 || { cat "$OUT/compile.log" ; exit 1 ; }

python3 "$TESTS_DIR/ota_sim.py" \
	--mcuboot "$MCUBOOT_BUILD/zephyr/zephyr.exe" \
	--loader "$LOADER" \
	--sketch "$OUT/sketch/ota_flow.ino.bin-zsk.bin" \
	--imgtool "$TOPDIR/bootloader/mcuboot/scripts/imgtool.py" \
	--key "$TOPDIR/bootloader/mcuboot/root-ec-p256.pem" \
	"$@"
