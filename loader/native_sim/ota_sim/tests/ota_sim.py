#!/usr/bin/env python3
# Copyright (c) Arduino s.r.l. and/or its affiliated companies
# SPDX-License-Identifier: Apache-2.0
"""MCUboot OTA simulator tests.

Each power-on runs MCUboot (native_sim, mcuboot_sim_boot.c), which prints its
boot decision and exits, then the loader (native_sim_ota) with the ota_flow
sketch on the same flash and EEPROM files. A reboot ends the loader with exit
code 100 (mcuboot_sim_loader.c); the next power-on starts MCUboot again.

The sketch runs the commands it reads on stdin (see ota_flow.ino), all given
when the loader starts; an image follows its "update <size>" command.

Usage: ota_sim.py --mcuboot <exe> --loader <exe> --sketch <bin>
                  --imgtool <imgtool.py> --key <pem> [-w <dir>] [<scenario>...]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

FLASH_SIZE = 2048 * 1024
SLOT0 = 0x0000C000
SLOT1 = 0x00075000
SLOT_SIZE = 0x69000      # slot0; slot1 is one sector larger
SECTOR = 0x1000          # swap-using-offset: slot1 images start one sector in
HEADER_SIZE = 0x200

EXIT_NO_IMAGE = 3        # mcuboot_sim_boot.c
EXIT_REBOOT = 100        # mcuboot_sim_loader.c
EXIT_POWER_CUT = 102     # power_cut.c
TIMEOUT = 120            # real seconds for one run


class Fail(Exception):
    pass


def check(cond, what):
    if not cond:
        raise Fail(what)


def update(image):
    """Commands to stage image."""
    return f"update {len(image)}\n".encode() + image


def replies(out, cmd):
    """The return codes the sketch printed for cmd."""
    return [int(v) for v in re.findall(rf'^{cmd} (-?\d+)$', out, re.M)]


class Sim:
    def __init__(self, opts, work):
        self.opts = opts
        self.work = work
        self.flash = os.path.join(work, 'flash.bin')
        self.eeprom = os.path.join(work, 'eeprom.bin')
        self.log = []
        self.summary = None

    # --- images and flash -----------------------------------------------

    def sign(self, version, size, confirm=False, key=None):
        """An MCUboot image of a random payload; confirm: a permanent
        request (USB DFU), padded to put its trailer at the end of slot1."""
        payload = os.path.join(self.work, 'payload.bin')
        out = os.path.join(self.work, 'image.bin')
        with open(payload, 'wb') as f:
            f.write(os.urandom(size))
        cmd = [sys.executable, self.opts.imgtool, 'sign', '-k', key or self.opts.key,
               '--header-size', hex(HEADER_SIZE), '--pad-header', '--align', '8',
               '--version', version, '--slot-size', hex(SLOT_SIZE)]
        if confirm:
            cmd += ['--pad', '--confirm']
        subprocess.run(cmd + [payload, out], check=True, stdout=subprocess.DEVNULL)
        with open(out, 'rb') as f:
            return f.read()

    def keygen(self):
        key = os.path.join(self.work, 'other-key.pem')
        subprocess.run([sys.executable, self.opts.imgtool, 'keygen', '-t', 'ecdsa-p256',
                        '-k', key], check=True, stdout=subprocess.DEVNULL)
        return key

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

    def read(self, offset, size):
        with open(self.flash, 'rb') as f:
            f.seek(offset)
            return f.read(size)

    def snapshot(self):
        return [open(p, 'rb').read() if os.path.exists(p) else None
                for p in (self.flash, self.eeprom)]

    def restore(self, snap):
        for path, data in zip((self.flash, self.eeprom), snap):
            if data is not None:
                with open(path, 'wb') as f:
                    f.write(data)
            elif os.path.exists(path):
                os.remove(path)

    # --- runs -----------------------------------------------------------

    def run(self, name, args, script=b''):
        r = subprocess.run(args + [f'--flash={self.flash}', f'--eeprom={self.eeprom}'],
                           input=script, capture_output=True, timeout=TIMEOUT)
        out = r.stdout.decode(errors='replace').replace('\r', '')
        self.log.append(f'--- {name}')
        self.log.extend(out.splitlines())
        ops = re.search(r'^SIM-(?:BOOT|REBOOT): .*ops=(\d+)', out, re.M)
        self.ops = int(ops.group(1)) if ops else None
        return r.returncode, out

    @staticmethod
    def cut_args(cut, torn):
        return ([f'--power_cut_after={cut}'] if cut else []) + (['--power_cut_torn'] if torn else [])

    def mcuboot(self, cut=None, torn=False):
        """Run MCUboot; set booted (version or None), swap (its swap type)
        and ops (flash operations, when not cut). Return the exit code."""
        rc, out = self.run('mcuboot', [self.opts.mcuboot, '--stop_at=10'] + self.cut_args(cut, torn))
        m = re.search(r'^SIM-BOOT: off=0x[0-9a-f]+ ver=([0-9.]+)\+0 ', out, re.M)
        swap = re.search(r'Swap type: (\w+)', out)
        self.booted = m.group(1) if m and rc == 0 else None
        self.swap = swap.group(1) if swap else None
        check(rc in (0, EXIT_NO_IMAGE, EXIT_POWER_CUT), f"MCUboot exit code {rc}")
        return rc

    def loader(self, script=b'', stop_at=None, cut=None, torn=False):
        """Run the loader (after mcuboot()) with the sketch commands of
        script, until the sketch exits, a reboot or stop_at simulated
        seconds. --no-rt: the 60 s trial window takes milliseconds."""
        args = [self.opts.loader, f'--sketch={self.opts.sketch}', '--no-rt']
        if stop_at:
            args.append(f'--stop_at={stop_at}')
        return self.run(f'loader (booted {self.booted})', args + self.cut_args(cut, torn), script)

    def power_on(self, expect_version, script=b'', stop_at=None):
        """MCUboot, which must boot expect_version, then the loader."""
        self.mcuboot()
        check(self.booted == expect_version, f"MCUboot booted {self.booted}, expected {expect_version}")
        return self.loader(script, stop_at)

    def ota(self, running, image, staged=0):
        """Stage image from the running version and reboot into MCUboot."""
        rc, out = self.power_on(running, update(image) + b"start\n")
        check(replies(out, 'staged') == [staged], f"staging returned {replies(out, 'staged')}")
        check(rc == EXIT_REBOOT, f"no reboot after ota_loader_start(), exit code {rc}")

    def sweep(self, stage, after):
        """Cut the power at every flash operation of stage(cut, torn) in
        turn, clean then torn, from the current flash; after(where,
        completed) checks the following power-ons. stage() returns its exit
        code; without a cut it must report its flash operations (ops)."""
        snap = self.snapshot()
        stage(None, False)
        total = self.ops
        check(total, "the uncut run reported no flash operations")
        for n in range(1, total + 1):
            for torn in (False, True):
                where = f"cut at op {n}{' (torn)' if torn else ''}"
                self.restore(snap)
                self.log.clear()
                completed = stage(n, torn) != EXIT_POWER_CUT
                # a torn cut of the last operation has nothing to cut before
                check(not completed or (torn and n == total), f"{where}: no power cut")
                after(where, completed)
        self.summary = f"{total} operations, each cut clean and torn"


# --- scenarios ----------------------------------------------------------
# Each starts from v1 programmed over SWD (no trailer: MCUboot and the loader
# treat it as confirmed) unless it says otherwise.

def factory(sim):
    """Return the v1 image."""
    sim.erase()
    v1 = sim.sign('1.0.0', 60000)
    sim.program(SLOT0, v1)
    return v1


def scenario_confirmed(sim):
    """O1: an OTA update that confirms itself stays installed."""
    factory(sim)
    sim.ota('1.0.0', sim.sign('2.0.0', 70000))
    rc, out = sim.power_on('2.0.0', b"confirm\nsleep 90000\nexit 0\n")
    check(sim.swap == 'test', f"swap type {sim.swap}, expected test")
    check(replies(out, 'confirm') == [0], "ota_confirm() failed")
    check(rc == 0, f"rebooted after ota_confirm(), exit code {rc}")
    sim.power_on('2.0.0', stop_at=90)
    check(sim.swap == 'none', f"swap type {sim.swap} after the confirm, expected none")


def scenario_not_confirmed(sim):
    """O2: an OTA update never confirmed is reverted after the trial window."""
    factory(sim)
    sim.ota('1.0.0', sim.sign('2.0.0', 70000))
    rc, out = sim.power_on('2.0.0', stop_at=120)
    check(rc == EXIT_REBOOT, f"no reboot in the trial window, exit code {rc}")
    check('Test image not confirmed' in out, "no trial window message")
    rc, _ = sim.power_on('1.0.0', stop_at=120)
    check(sim.swap == 'revert', f"swap type {sim.swap}, expected revert")
    check(rc == 0, f"the reverted image rebooted, exit code {rc}")


def scenario_feed_watchdog(sim):
    """ota_feed_watchdog() restarts the trial window; without it, revert."""
    for feed, expect_rc in ((b"feed\n", 0), (b"", EXIT_REBOOT)):
        factory(sim)
        sim.ota('1.0.0', sim.sign('2.0.0', 70000))
        rc, _ = sim.power_on('2.0.0', b"sleep 45000\n" + feed + b"sleep 45000\nconfirm\nexit 0\n")
        check(rc == expect_rc, f"{'with' if feed else 'without'} ota_feed_watchdog(): "
                               f"exit code {rc}, expected {expect_rc}")


def scenario_update_while_testing(sim):
    """A test image must confirm itself before it can stage an update."""
    factory(sim)
    sim.ota('1.0.0', sim.sign('2.0.0', 70000))
    v3 = sim.sign('3.0.0', 50000)
    rc, out = sim.power_on('2.0.0', update(v3) + b"confirm\n" + update(v3) + b"start\n")
    check(replies(out, 'staged') == [-16, 0], f"staging returned {replies(out, 'staged')}, "
                                              "expected -EBUSY before the confirm, then 0")
    check(rc == EXIT_REBOOT, f"no reboot, exit code {rc}")
    rc, _ = sim.power_on('3.0.0', b"confirm\nexit 0\n")
    check(rc == 0, "v3 did not run")


def bad_image(sim, image, staged=0):
    factory(sim)
    sim.ota('1.0.0', image, staged)
    for _ in range(2):    # rejected for good: v1 on the next power-ons too
        rc, _ = sim.power_on('1.0.0', stop_at=90)
        check(rc == 0, f"v1 rebooted after a rejected image, exit code {rc}")


def scenario_bad_signature(sim):
    """O4: an image signed with another key is not installed."""
    bad_image(sim, sim.sign('2.0.0', 70000, key=sim.keygen()))


def scenario_bad_hash(sim):
    """O4: an image with a flipped payload byte is not installed."""
    image = bytearray(sim.sign('2.0.0', 70000))
    image[HEADER_SIZE + 1000] ^= 0x01
    bad_image(sim, bytes(image))


def scenario_truncated(sim):
    """O4: a truncated image is refused by ota_loader_ready()."""
    factory(sim)
    rc, out = sim.power_on('1.0.0', update(sim.sign('2.0.0', 70000)[:40000]) + b"exit 0\n")
    check(replies(out, 'staged') and replies(out, 'staged')[0] != 0,
          "an image cut in its payload was staged")
    sim.power_on('1.0.0', stop_at=10)


def scenario_truncated_tlv(sim):
    """O4: an image cut in its TLVs (hash, signature) is not installed."""
    # ota_loader_ready() only compares the payload size with the file size:
    # it stages this image, and MCUboot has to reject it.
    bad_image(sim, sim.sign('2.0.0', 70000)[:-40])


def scenario_permanent(sim):
    """A permanent request (USB DFU) overwrites slot0 and is never reverted."""
    factory(sim)
    sim.program(SLOT1 + SECTOR, sim.sign('3.0.0', 50000, confirm=True))
    rc, _ = sim.power_on('3.0.0', stop_at=120)
    check(sim.swap == 'perm', f"swap type {sim.swap}, expected perm")
    check(rc == 0, f"the permanent image rebooted, exit code {rc}")
    sim.power_on('3.0.0', stop_at=10)


# --- power loss -----------------------------------------------------------

def expect_boot(sim, where, *versions):
    sim.mcuboot()
    check(sim.booted in versions, f"{where}: booted {sim.booted}, expected {' or '.join(versions)}")


def no_reboot(sim, where):
    rc, _ = sim.loader(stop_at=120)
    check(rc == 0, f"{where}: {sim.booted} rebooted (exit code {rc}): reboot loop")


def scenario_power_cut_swap(sim):
    """Power loss at every flash operation of the test swap: still v2, then reverts to an intact v1."""
    v1 = factory(sim)
    sim.ota('1.0.0', sim.sign('2.0.0', 70000))

    def after(where, completed):
        if not completed:
            expect_boot(sim, where, '2.0.0')
            # never confirmed: the loader reboots after the trial window
            rc, _ = sim.loader(stop_at=120)
            check(rc == EXIT_REBOOT, f"{where}: v2 did not revert")
        # (a completed swap booted v2 once, unconfirmed: the next boot reverts)
        expect_boot(sim, where, '1.0.0')
        check(sim.read(SLOT0, len(v1)) == v1, f"{where}: slot0 differs from v1 after the revert")

    sim.sweep(sim.mcuboot, after)


def scenario_power_cut_revert(sim):
    """Power loss at every flash operation of the revert: v1 boots intact."""
    v1 = factory(sim)
    sim.ota('1.0.0', sim.sign('2.0.0', 70000))
    rc, _ = sim.power_on('2.0.0', stop_at=120)
    check(rc == EXIT_REBOOT, "v2 did not reboot")

    def after(where, completed):
        for _ in range(2):    # and it stays there
            expect_boot(sim, where, '1.0.0')
            check(sim.read(SLOT0, len(v1)) == v1, f"{where}: slot0 differs from v1")

    sim.sweep(sim.mcuboot, after)


def scenario_power_cut_staging(sim):
    """Power loss at every flash operation of the loader staging an update: v1, or v2 as a test."""
    factory(sim)
    v2 = sim.sign('2.0.0', 70000)
    sim.mcuboot()
    seen = set()

    def stage(cut, torn):
        rc, _ = sim.loader(update(v2) + b"start\n", cut=cut, torn=torn)
        return rc

    def after(where, completed):
        expect_boot(sim, where, '1.0.0', '2.0.0')
        seen.add(sim.booted)
        # v2 only after MCUboot checked its hash and signature, and as a test
        check(sim.booted == '1.0.0' or sim.swap == 'test', f"{where}: v2 booted as {sim.swap}")

    sim.sweep(stage, after)
    sim.summary += f"; booted {sorted(seen)}"


def scenario_power_cut_permanent(sim):
    """Power loss at every flash operation of a permanent overwrite: v1 or v3, never a reboot loop."""
    factory(sim)
    sim.program(SLOT1 + SECTOR, sim.sign('3.0.0', 50000, confirm=True))

    def after(where, completed):
        expect_boot(sim, where, '1.0.0', '3.0.0')
        no_reboot(sim, where)

    sim.sweep(sim.mcuboot, after)


def scenario_power_cut_bootstrap(sim):
    """Power loss at every flash operation of a bootstrap (empty slot0): v3, never a reboot loop."""
    # Fails at the torn write of the slot0 magic: MCUboot boots the image
    # with magic=bad and never reverts it, the loader cannot confirm it and
    # reboots after the trial window (README.md).
    sim.erase()
    sim.program(SLOT1 + SECTOR, sim.sign('3.0.0', 50000))

    def after(where, completed):
        expect_boot(sim, where, '3.0.0')
        no_reboot(sim, where)

    sim.sweep(sim.mcuboot, after)


SCENARIOS = {name[len('scenario_'):]: fn for name, fn in globals().items()
             if name.startswith('scenario_')}

# Known bugs: reported as XFAIL; a pass is reported, to drop the entry.
KNOWN_FAILURES = {
    'power_cut_bootstrap': "torn slot0 magic: 60 s reboot loop (loader trial window)",
}


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    for arg in ('mcuboot', 'loader', 'sketch', 'imgtool', 'key'):
        p.add_argument(f'--{arg}', required=True)
    p.add_argument('-w', '--work', help="keep the files of each scenario here")
    p.add_argument('scenarios', nargs='*', metavar='scenario',
                   help=f"default: all of {', '.join(SCENARIOS)}")
    opts = p.parse_args()
    unknown = set(opts.scenarios) - set(SCENARIOS)
    if unknown:
        p.error(f"unknown scenarios: {', '.join(sorted(unknown))}")

    root = opts.work or tempfile.mkdtemp(prefix='ota_sim.')
    failed = xfailed = 0
    names = opts.scenarios or list(SCENARIOS)
    for name in names:
        work = os.path.join(root, name)
        shutil.rmtree(work, ignore_errors=True)
        os.makedirs(work)
        sim = Sim(opts, work)
        doc = SCENARIOS[name].__doc__
        known = KNOWN_FAILURES.get(name)
        try:
            SCENARIOS[name](sim)
            print(f"PASS: ota_sim: {name}: {doc}")
            if sim.summary:
                print(f"  {sim.summary}")
            if known:
                print(f"  passes although listed in KNOWN_FAILURES ({known}): drop it")
                failed += 1
        except (Fail, subprocess.SubprocessError) as e:
            if known:
                xfailed += 1
                print(f"XFAIL: ota_sim: {name}: {doc}\n  {e}\n  known: {known}")
            else:
                failed += 1
                print(f"FAIL: ota_sim: {name}: {doc}\n  {e}")
                for line in sim.log[-60:]:
                    print(f"  | {line}")
        sys.stdout.flush()
        with open(os.path.join(work, 'log.txt'), 'w') as f:
            f.write('\n'.join(sim.log) + '\n')
    if not opts.work:
        shutil.rmtree(root, ignore_errors=True)
    print(f"{len(names) - failed - xfailed} passed, {failed} failed, {xfailed} known failures")
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
