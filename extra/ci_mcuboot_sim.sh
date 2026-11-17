#!/usr/bin/env bash
# Copyright (c) Arduino s.r.l. and/or its affiliated companies
# SPDX-License-Identifier: Apache-2.0
#
# Run the MCUboot simulator (mcuboot/sim) with the Arduino MCUboot hooks of
# this core (loader/mcuboot_hooks): the upstream core tests with the hooks
# built in, and the hook tests of loader/mcuboot_hooks/sim. MCUboot is the
# revision west.yml pins, so the tested bootloader is the one that ships.
#
# Usage: ci_mcuboot_sim.sh [-r <report>] [-f] [<config>...]
#   -r  write a JSON report as run_sketches.sh does (default: $SKETCH_REPORT,
#       if set), one entry per test and configuration
#   -f  fast: skip the power-loss tests that interrupt every flash operation
#   <config>: a swap mode, swap-offset, swap-move or swap-scratch, optionally
#       followed by '+<cargo features>' (default: the swap modes the hooks
#       support, plus the configuration of each variant that builds the hooks
#       into MCUboot, see variant_configs)
#
# Environment:
#   MCUBOOT_SIM_DIR  MCUboot checkout to use or create (default: build/mcuboot-sim)
#
# Needs git, cargo and a C compiler. Exits with code 1 if any test failed.
# Run from the root of the core.

set -u -o pipefail

REPORT=${SKETCH_REPORT:-}
FAST=false
while getopts "r:f" opt ; do
	case $opt in
		r) REPORT=$OPTARG ;;
		f) FAST=true ;;
		*) sed -n -e '/^# Usage/,/^# Needs/p' "$0" >&2 ; exit 1 ;;
	esac
done
shift $((OPTIND - 1))

CORE_DIR=$(pwd)
SIM_SRC=$CORE_DIR/loader/mcuboot_hooks/sim
MCUBOOT_DIR=${MCUBOOT_SIM_DIR:-$CORE_DIR/build/mcuboot-sim}
COMMON_FEATURES=sig-ecdsa,bootstrap,validate-primary-slot,arduino-hooks

# mcuboot_ref: '<url> <revision>' of the mcuboot project in west.yml
mcuboot_ref() {
	awk '
		/^ *remotes:/ { sect = "remotes" } /^ *projects:/ { sect = "projects" }
		sect == "remotes" && /- name:/ { r = $3 }
		sect == "remotes" && /url-base:/ { base[r] = $2 }
		sect == "projects" && /- name:/ { p = $3 }
		sect == "projects" && p == "mcuboot" && /remote:/ { remote = $2 }
		sect == "projects" && p == "mcuboot" && /revision:/ { rev = $2 }
		END { if (rev != "") print base[remote] "/mcuboot " rev }
	' west.yml
}

# variant_configs: one '<swap mode>+<features>' per variant whose MCUboot
# configuration enables the hooks
variant_configs() {
	local conf mode features
	for conf in variants/*/mcuboot.conf ; do
		[ -f "$conf" ] || continue
		grep -qE '^CONFIG_(BOOT_IMAGE_ACCESS_HOOKS|ARDUINO_MCUBOOT_DFU_BOOT_MODE)=y' "$conf" || continue
		mode=swap-scratch
		grep -q '^CONFIG_BOOT_SWAP_USING_OFFSET=y' "$conf" && mode=swap-offset
		grep -q '^CONFIG_BOOT_SWAP_USING_MOVE=y' "$conf" && mode=swap-move
		features=$COMMON_FEATURES
		grep -q '^CONFIG_BOOT_SIGNATURE_TYPE_RSA=y' "$conf" && features=${features/sig-ecdsa/sig-rsa}
		grep -q '^CONFIG_BOOT_SIGNATURE_TYPE_ED25519=y' "$conf" && features=${features/sig-ecdsa/sig-ed25519}
		echo "$mode+$features"
	done
}

prepare() {
	local url rev
	read -r url rev <<< "$(mcuboot_ref)"
	if [ -z "${rev:-}" ] ; then
		echo "No mcuboot project in west.yml"
		return 1
	fi
	echo "MCUboot $url @ $rev"
	if [ ! -d "$MCUBOOT_DIR/.git" ] ; then
		git init -q "$MCUBOOT_DIR" || return 1
	fi
	cd "$MCUBOOT_DIR" || return 1
	if ! git cat-file -e "$rev^{commit}" 2> /dev/null ; then
		git fetch -q --depth 1 "$url" "$rev" || return 1
	fi
	# (the cargo build tree is kept, as a cache)
	git checkout -q --force "$rev" && git clean -q -fdx -e /target || return 1
	git submodule -q update --init --depth 1 ext/mbedtls-3.6.0 || return 1
	cp -r "$SIM_SRC/files/." . || return 1
	if ! git apply "$SIM_SRC/mcuboot-sim.patch" ; then
		echo "loader/mcuboot_hooks/sim/mcuboot-sim.patch does not apply to MCUboot $rev:"
		echo "rebase it on the MCUboot revision of west.yml"
		return 1
	fi
	cd "$CORE_DIR" || return 1
}

ENTRIES=$(mktemp)
LOG=$(mktemp)
trap 'rm -f "$ENTRIES" "$LOG"' EXIT
FAILED=0

# entry <name> <compiled> <run passed> [<issues file>]: add a report entry
entry() {
	local issues=/dev/null
	[ $# -lt 4 ] || issues=$4
	jq -n -c --arg name "$1" --argjson compiled "$2" --argjson passed "$3" \
		--rawfile issues "$issues" '{
			name: $name,
			compilation_success: $compiled,
			issues: ($issues | split("\n") | map(select(length > 0)) | .[:200])
		} + if $compiled then { run_success: $passed } else {} end' >> "$ENTRIES"
	$3 || FAILED=$((FAILED + 1))
}

# run_config <swap mode>[+<features>]: build and run the tests of one
# configuration, adding their results to the report
run_config() {
	local mode=${1%%+*} features=$COMMON_FEATURES name status out
	[[ $1 != *+* ]] || features=${1#*+}
	[ "$mode" == swap-scratch ] || features="$mode,$features" # scratch is the default
	echo "=== $1"
	out=$(mktemp -d)
	(
		cd "$MCUBOOT_DIR/sim" || exit 1
		export ARDUINO_MCUBOOT_HOOKS=$CORE_DIR/loader/mcuboot_hooks/boot_hooks.c
		$FAST || export MCUBOOT_RUN_SLOW_TESTS=1
		cargo test --release --no-run --features "$features" --test core --test arduino \
			> "$out/build.log" 2>&1 || exit 2
		RUST_LOG=warn cargo test --release --no-fail-fast --features "$features" --test core --test arduino -- \
			--exact --skip perm_with_fails --skip perm_with_random_fails > "$out/test.log" 2>&1
		exit 0
	)
	case $? in
		2)
			tail -n 100 "$out/build.log"
			entry "mcuboot/$1/build" false false "$out/build.log"
			rm -rf "$out"
			return ;;
	esac
	# (without the simulator's warning for each interrupted flash operation)
	grep -v '^\[WRN\]' "$out/test.log"
	# 'test <name> ... ok|FAILED', and the output of each failed test in a
	# '---- <name> stdout ----' section
	while read -r name status ; do
		if [ "$status" == ok ] ; then
			entry "mcuboot/$1/$name" true true
		else
			awk -v t="$name" '
				$0 == "---- " t " stdout ----" { on = 1; next }
				on && /^---- / { on = 0 } on && /^(failures|test result)/ { on = 0 }
				on' "$out/test.log" > "$out/issues"
			# the simulator logs its findings outside the captured test
			# output: add its errors and the warnings that name the
			# interruption point (the tests run in parallel, and passing
			# tests log expected mismatches as warnings)
			grep -E 'ERROR +bootsim::|WARN +bootsim::.*(step|stop=)' "$out/test.log" |
				sed -e 's/^\[[^]]*\] *//' >> "$out/issues"
			entry "mcuboot/$1/$name" true false "$out/issues"
		fi
	done < <(sed -n -E 's/^test ([a-z0-9_]+) \.\.\. (ok|FAILED)$/\1 \2/p' "$out/test.log")
	if ! grep -q '^test result: ' "$out/test.log" ; then
		entry "mcuboot/$1/run" true false "$out/test.log"
	fi
	rm -rf "$out"
}

if ! prepare > "$LOG" 2>&1 ; then
	cat "$LOG"
	entry "mcuboot/setup" false false "$LOG"
else
	cat "$LOG"
	if [ $# -eq 0 ] ; then
		set -- $( { printf '%s\n' swap-offset swap-move swap-scratch ; variant_configs ; } | awk '!seen[$0]++')
	fi
	for config in "$@" ; do
		run_config "$config"
	done
fi

if [ -n "$REPORT" ] ; then
	jq -s '{ boards: [ { board: "mcuboot-sim", sketches: . } ] }' "$ENTRIES" > "$REPORT"
fi
echo "MCUboot simulator: $(wc -l < "$ENTRIES") tests, $FAILED failed"
[ "$FAILED" -eq 0 ]
