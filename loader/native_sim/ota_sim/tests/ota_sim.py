#!/usr/bin/env python3
# Copyright (c) Arduino s.r.l. and/or its affiliated companies
# SPDX-License-Identifier: Apache-2.0
"""MCUboot OTA simulator tests.

Each power-on runs MCUboot (native_sim, mcuboot_sim_boot.c), which prints its
boot decision and exits, then the loader (native_sim_ota) with the ota_flow
sketch on the same flash and EEPROM files. A reboot ends the loader with exit
code 100 (mcuboot_sim_loader.c) and the next power-on starts MCUboot again.

The sketch runs the line commands it reads on stdin (see ota_flow.ino); an
image is streamed in chunks, each acknowledged by the sketch.

Usage: ota_sim.py --mcuboot <exe> --loader <exe> --sketch <bin>
                  --imgtool <imgtool.py> --key <pem> [-w <dir>] [<scenario>...]
"""

import argparse
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

FLASH_SIZE = 2048 * 1024
SLOT0 = 0x0000C000
SLOT1 = 0x00075000
SLOT_SIZE = 0x69000      # slot0; slot1 is one sector larger
SECTOR = 0x1000          # swap-using-offset: slot1 images start one sector in
HEADER_SIZE = 0x200
ALIGN = 8

EXIT_REBOOT = 100        # mcuboot_sim_loader.c
EXIT_NO_IMAGE = 3        # mcuboot_sim_boot.c
CHUNK = 512              # ota_flow.ino
RUN_TIMEOUT = 120        # real seconds for one loader run


class Fail(Exception):
    pass


class Loader:
    """A running loader: lines of its output, commands on its input."""

    def __init__(self, args, log):
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
        self.lines = queue.Queue()
        self.log = log
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        for raw in self.proc.stdout:
            text = raw.decode(errors='replace').replace('\r', '').rstrip('\n')
            self.log.append(text)
            self.lines.put(text)
        self.lines.put(None)

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def expect(self, regex, timeout=RUN_TIMEOUT):
        """Wait for an output line matching regex; return the match."""
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise Fail(f"timeout waiting for /{regex}/")
            try:
                line = self.lines.get(timeout=left)
            except queue.Empty:
                continue
            if line is None:
                raise Fail(f"loader exited waiting for /{regex}/")
            m = re.search(regex, line)
            if m:
                return m

    def update(self, image):
        """Stream an image with the update command; return the staged rc."""
        self.send(f"update {len(image)}\n")
        m = self.expect(r'^(ack 0|staged -?\d+)$')
        if m.group(1).startswith('staged'):
            return int(m.group(1).split()[1])
        for done in range(0, len(image), CHUNK):
            part = image[done:done + CHUNK]
            self.send(part)
            m = self.expect(r'^(ack|staged) (-?\d+)$')
            if m.group(1) == 'staged':
                return int(m.group(2))
        return int(self.expect(r'^staged (-?\d+)$').group(1))

    def wait(self, timeout=RUN_TIMEOUT):
        try:
            rc = self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            raise Fail("loader did not exit")
        # let the reader drain the output
        while self.lines.get() is not None:
            pass
        return rc


class Sim:
    def __init__(self, opts, work):
        self.opts = opts
        self.work = work
        self.flash = os.path.join(work, 'flash.bin')
        self.eeprom = os.path.join(work, 'eeprom.bin')
        self.log = []
        self.booted = None

    # --- images ---------------------------------------------------------

    def sign(self, version, size, confirm=False, key=None, pad=False):
        """An MCUboot image of a random payload of size bytes."""
        payload = os.path.join(self.work, 'payload.bin')
        out = os.path.join(self.work, 'image.bin')
        with open(payload, 'wb') as f:
            f.write(os.urandom(size))
        cmd = [sys.executable, self.opts.imgtool, 'sign', '-k', key or self.opts.key,
               '--header-size', hex(HEADER_SIZE), '--pad-header', '--align', str(ALIGN),
               '--version', version, '--slot-size', hex(SLOT_SIZE)]
        if pad:
            cmd.append('--pad')
        if confirm:
            cmd.append('--confirm')
        subprocess.run(cmd + [payload, out], check=True, stdout=subprocess.DEVNULL)
        with open(out, 'rb') as f:
            return f.read()

    def keygen(self):
        key = os.path.join(self.work, 'other-key.pem')
        if not os.path.exists(key):
            subprocess.run([sys.executable, self.opts.imgtool, 'keygen', '-t', 'ecdsa-p256',
                            '-k', key], check=True, stdout=subprocess.DEVNULL)
        return key

    # --- flash ----------------------------------------------------------

    def erase(self):
        """Erased flash and empty retained memory, as a new board."""
        # the flash simulator would extend a short file with 0x00, not erased
        with open(self.flash, 'wb') as f:
            f.write(b'\xff' * FLASH_SIZE)
        if os.path.exists(self.eeprom):
            os.remove(self.eeprom)

    def program(self, offset, data):
        """Write data at offset, like an SWD programmer."""
        with open(self.flash, 'r+b') as f:
            f.seek(offset)
            f.write(data)

    # --- power-on -------------------------------------------------------

    def files(self):
        return [f'--flash={self.flash}', f'--eeprom={self.eeprom}']

    def mcuboot(self):
        """Run MCUboot; set self.booted to the version it boots, or None."""
        r = subprocess.run([self.opts.mcuboot] + self.files() + ['--stop_at=10'],
                           stdin=subprocess.DEVNULL, capture_output=True, timeout=RUN_TIMEOUT)
        out = r.stdout.decode(errors='replace').replace('\r', '')
        self.log.append('--- mcuboot')
        self.log.extend(out.splitlines())
        m = re.search(r'SIM-BOOT: off=0x[0-9a-f]+ ver=([0-9.]+)\+0\b', out)
        swap = re.search(r'Swap type: (\w+)', out)
        self.swap = swap.group(1) if swap else None
        self.booted = m.group(1) if m and r.returncode == 0 else None
        if r.returncode not in (0, EXIT_NO_IMAGE):
            raise Fail(f"MCUboot exit code {r.returncode}")
        return self.booted

    def loader(self, stop_at=None, script=None, rt=False):
        """Start the loader (after mcuboot()) with the commands of script.

        Without rt the simulated time runs as fast as the host allows, also
        while the sketch waits for input: commands sent later than at the
        start may come after the trial window. A run that talks to an
        unconfirmed image (update handshake) needs rt."""
        args = [self.opts.loader] + self.files() + [f'--sketch={self.opts.sketch}']
        args.append('--rt' if rt else '--no-rt')
        if stop_at:
            args.append(f'--stop_at={stop_at}')
        self.log.append(f'--- loader (booted {self.booted})')
        ld = Loader(args, self.log)
        if script:
            ld.send(script)
        ld.expect(r'^ready$')
        return ld

    def power_on(self, expect_version, stop_at=None, script=None, rt=False):
        """MCUboot, which must boot expect_version, then the loader."""
        got = self.mcuboot()
        if got != expect_version:
            raise Fail(f"MCUboot booted {got}, expected {expect_version}")
        return self.loader(stop_at, script, rt)

    def ota(self, ld, image):
        """Stream image into the running loader and reboot into MCUboot."""
        rc = ld.update(image)
        if rc != 0:
            raise Fail(f"staging the update failed: {rc}")
        ld.send("start\n")
        rc = ld.wait()
        if rc != EXIT_REBOOT:
            raise Fail(f"no reboot after ota_loader_start(), exit code {rc}")


def check(cond, what):
    if not cond:
        raise Fail(what)


# --- scenarios ----------------------------------------------------------
# Each starts from a board programmed over SWD with v1 (no trailer: MCUboot
# and the loader treat it as confirmed) and checks the boot decisions.

def factory(sim):
    sim.erase()
    sim.program(SLOT0, sim.sign('1.0.0', 60000))


def scenario_confirmed(sim):
    """O1: an OTA update that confirms itself stays installed."""
    factory(sim)
    sim.ota(sim.power_on('1.0.0'), sim.sign('2.0.0', 70000))

    ld = sim.power_on('2.0.0', script="confirm\nsleep 90000\nexit 0\n")
    check(sim.swap == 'test', f"swap type {sim.swap}, expected test")
    check(ld.expect(r'^confirm (-?\d+)$').group(1) == '0', "ota_confirm() failed")
    ld.expect(r'^slept 90000$')
    check(ld.wait() == 0, "rebooted after ota_confirm()")

    sim.power_on('2.0.0', stop_at=90).wait()
    check(sim.swap == 'none', f"swap type {sim.swap} after the confirm, expected none")


def scenario_not_confirmed(sim):
    """O2: an OTA update never confirmed is reverted after the trial window."""
    factory(sim)
    sim.ota(sim.power_on('1.0.0'), sim.sign('2.0.0', 70000))

    rc = sim.power_on('2.0.0', stop_at=120).wait()
    check(rc == EXIT_REBOOT, f"no reboot in the trial window, exit code {rc}")
    check(any('Test image not confirmed' in l for l in sim.log), "no trial window message")

    rc = sim.power_on('1.0.0', stop_at=120).wait()
    check(sim.swap == 'revert', f"swap type {sim.swap}, expected revert")
    check(rc == 0, f"the reverted image rebooted, exit code {rc}")


def scenario_feed_watchdog(sim):
    """ota_feed_watchdog() restarts the trial window; without it, revert."""
    factory(sim)
    sim.ota(sim.power_on('1.0.0'), sim.sign('2.0.0', 70000))

    ld = sim.power_on('2.0.0', script="sleep 45000\nfeed\nsleep 45000\nconfirm\nexit 0\n")
    check(ld.expect(r'^feed (-?\d+)$').group(1) == '0', "ota_feed_watchdog() failed")
    check(ld.expect(r'^confirm (-?\d+)$').group(1) == '0', "ota_confirm() failed")
    check(ld.wait() == 0, "rebooted although the window was restarted")

    # control: the same 90 s without the feed run out of the window
    factory(sim)
    sim.ota(sim.power_on('1.0.0'), sim.sign('2.0.0', 70000))
    ld = sim.power_on('2.0.0', script="sleep 45000\nsleep 45000\nconfirm\nexit 0\n")
    check(ld.wait() == EXIT_REBOOT, "no revert without ota_feed_watchdog()")
    sim.power_on('1.0.0', stop_at=10).wait()


def scenario_update_while_testing(sim):
    """A test image must confirm itself before it can stage an update."""
    factory(sim)
    sim.ota(sim.power_on('1.0.0'), sim.sign('2.0.0', 70000))

    v3 = sim.sign('3.0.0', 50000)
    ld = sim.power_on('2.0.0', rt=True)
    rc = ld.update(v3)
    check(rc == -16, f"update of an unconfirmed image: {rc}, expected -EBUSY")
    ld.send("confirm\n")
    ld.expect(r'^confirm 0$')
    sim.ota(ld, v3)
    ld = sim.power_on('3.0.0', script="confirm\nexit 0\n")
    check(ld.wait() == 0, "v3 did not run")


def bad_image(sim, image, what):
    factory(sim)
    sim.ota(sim.power_on('1.0.0'), image)
    rc = sim.power_on('1.0.0', stop_at=90).wait()
    check(rc == 0, f"v1 rebooted after a rejected {what}, exit code {rc}")
    # rejected for good: still v1 on the next power-on
    sim.power_on('1.0.0', stop_at=10).wait()


def scenario_bad_signature(sim):
    """O4: an image signed with another key is not installed."""
    bad_image(sim, sim.sign('2.0.0', 70000, key=sim.keygen()), "foreign key")


def scenario_bad_hash(sim):
    """O4: an image with a flipped payload byte is not installed."""
    image = bytearray(sim.sign('2.0.0', 70000))
    image[HEADER_SIZE + 1000] ^= 0x01
    bad_image(sim, bytes(image), "flipped byte")


def scenario_truncated(sim):
    """O4: a truncated image is refused by ota_loader_ready()."""
    factory(sim)
    image = sim.sign('2.0.0', 70000)[:40000]
    ld = sim.power_on('1.0.0', rt=True)
    rc = ld.update(image)
    check(rc != 0, "an image cut in its payload was staged")
    ld.send("exit 0\n")
    ld.wait()
    sim.power_on('1.0.0', stop_at=10).wait()


def scenario_truncated_tlv(sim):
    """O4: an image cut in its TLVs (hash, signature) is not installed."""
    # ota_loader_ready() only compares the payload size with the file size:
    # it stages this image, and MCUboot has to reject it.
    bad_image(sim, sim.sign('2.0.0', 70000)[:-40], "image without its signature")


def scenario_permanent(sim):
    """A permanent request (USB DFU) overwrites slot0 and is never reverted."""
    factory(sim)
    sim.program(SLOT1 + SECTOR, sim.sign('3.0.0', 50000, confirm=True, pad=True))
    rc = sim.power_on('3.0.0', stop_at=120).wait()
    check(sim.swap == 'perm', f"swap type {sim.swap}, expected perm")
    check(rc == 0, f"the permanent image rebooted, exit code {rc}")
    sim.power_on('3.0.0', stop_at=10).wait()


SCENARIOS = {name[len('scenario_'):]: fn for name, fn in globals().items()
             if name.startswith('scenario_')}


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--mcuboot', required=True)
    p.add_argument('--loader', required=True)
    p.add_argument('--sketch', required=True)
    p.add_argument('--imgtool', required=True)
    p.add_argument('--key', required=True)
    p.add_argument('-w', '--work', help="keep the files of each scenario here")
    p.add_argument('scenarios', nargs='*', choices=[[]] + list(SCENARIOS), metavar='scenario',
                   help=f"default: all of {', '.join(SCENARIOS)}")
    opts = p.parse_args()

    root = opts.work or tempfile.mkdtemp(prefix='ota_sim.')
    passed = failed = 0
    for name in opts.scenarios or SCENARIOS:
        work = os.path.join(root, name)
        shutil.rmtree(work, ignore_errors=True)
        os.makedirs(work)
        sim = Sim(opts, work)
        doc = SCENARIOS[name].__doc__.strip()
        try:
            SCENARIOS[name](sim)
            print(f"PASS: ota_sim: {name}: {doc}")
            passed += 1
        except (Fail, subprocess.SubprocessError) as e:
            print(f"FAIL: ota_sim: {name}: {doc}\n  {e}")
            for line in sim.log[-60:]:
                print(f"  | {line}")
            failed += 1
        with open(os.path.join(work, 'log.txt'), 'w') as f:
            f.write('\n'.join(sim.log) + '\n')
    if not opts.work:
        shutil.rmtree(root, ignore_errors=True)
    print(f"{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
