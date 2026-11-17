# MCUboot simulator tests of the hooks

The MCUboot simulator (`mcuboot/sim`, Rust) runs bootutil on simulated
flash and can interrupt a boot at every flash operation. These files add
the hooks of this directory to it:

- `files/`: new files copied over the MCUboot tree:
  - `sim/mcuboot-sys/csupport/arduino_hooks.c` builds `boot_hooks.c`;
  - `sim/mcuboot-sys/csupport/arduino_boot.c` runs `double_tap.c` and
    `dfu_boot_mode.c` in the order of the Zephyr port (`boot/zephyr/main.c`),
    against the Zephyr stand-ins in `arduino_zephyr/`;
  - `sim/src/arduino.rs` and `sim/tests/arduino.rs` are the hook tests.
- `mcuboot-sim.patch`: the `arduino-hooks` feature, the build of the files
  above and the Rust entry point of `arduino_boot.c`, as changes to MCUboot
  files.

Run them with `extra/ci_mcuboot_sim.sh` from the root of the core (`-f` skips
the slow power-loss tests). It checks out the MCUboot revision of `west.yml`
in `build/mcuboot-sim`, so the patch must apply to that revision: rebase it
when MCUboot is bumped. The upstream `core` tests run with the hooks as well,
except `perm_with_fails` and `perm_with_random_fails`: with the hooks a
permanent update is a copy, not a swap.

In CI the tests are the `mcuboot` job of `test_native_sim.yml`; expected
failures are listed in `variants/native_sim_native_64/known_example_issues.txt`.
