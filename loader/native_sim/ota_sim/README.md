# MCUboot OTA simulator

Runs the MCUboot OTA flow of the Nano XL Link on the host: MCUboot with the
update policy of `loader/mcuboot_hooks`, then the loader with the MCUboot OTA
backend (`loader/ota/ota_mcuboot.c`) and a sketch, on a simulated flash.
Unlike `loader/mcuboot_hooks/sim` (bootutil alone, in the MCUboot Rust
simulator), the loader, the OTA library and the trial window run too.

```mermaid
flowchart LR
  R[ota_sim.py] -->|power-on| M[MCUboot<br/>native_sim]
  M -->|"SIM-BOOT: ver=..., exit 0"| R
  R -->|start, stdin commands| L[loader native_sim_ota<br/>+ ota_flow sketch]
  L -->|"reboot: exit 100"| R
  F[(flash.bin<br/>eeprom.bin)] --- M
  F --- L
```

- MCUboot and the loader are two native_sim executables sharing the flash
  (`--flash`) and the EEPROM (`--eeprom`) files. A "reboot" is the end of one
  process and the start of the next.
- `mcuboot_sim_boot.c`: MCUboot prints its boot decision and exits instead of
  jumping (`boot_image_jump_hook()`); exit code 3 means no bootable image.
- `mcuboot_sim_loader.c`: `sys_arch_reboot()` is wrapped at link time to end
  the loader with exit code 100 (native_sim would re-execute the loader).
  It also starts the counter watchdog, as MCUboot does on the board.
- `power_cut.c`: numbers the flash operations of a run (the simulator's call
  counters) and, with `--power_cut_after=<n>` (`--power_cut_torn`), loses
  power at operation n: the process ends with exit code 102, operation n not
  done (torn: done in part). Both images print their count (`ops=<n>`).
- `retained_mem_eeprom.c`: retained memory in the simulated EEPROM, for the
  boot mode and MCUboot's bootloader info, at the Nano XL Link offsets.
- `ota_sim.overlay`: swap-using-offset layout (slot1 one sector larger),
  8-byte writes, retention areas and the counter watchdog, for both images.
- `mcuboot.conf`, `loader.conf`: the Nano XL Link configuration without USB.
  The loader is the `native_sim_ota` board (`-DARDUINO_OTA_SIM=y`).

## Running

```sh
./extra/build.sh native_sim_ota
loader/native_sim/ota_sim/tests/run_tests.sh [-w <work dir>] [<scenario>...]
```

`-w` keeps the flash, EEPROM and log of each scenario. The loader runs with
`--no-rt`: the 60 s trial window takes milliseconds.

## Scenarios (`tests/ota_sim.py`)

Each starts from v1 programmed over SWD (no trailer).

| Scenario | Checks |
|---|---|
| `confirmed` | O1: OTA, swap as a test, `ota_confirm()`, no revert |
| `not_confirmed` | O2: no confirm, reboot after 60 s, revert |
| `feed_watchdog` | `ota_feed_watchdog()` keeps a 90 s trial alive; without it, revert |
| `update_while_testing` | `ota_write_begin()` refuses an update before the confirm (-EBUSY) |
| `bad_signature`, `bad_hash` | O4: MCUboot rejects the image, v1 stays |
| `truncated` | `ota_loader_ready()` refuses an image cut in its payload |
| `truncated_tlv` | an image cut in its TLVs is staged, MCUboot rejects it |
| `permanent` | a permanent request (USB DFU) overwrites slot0 for good |
| `power_cut_swap` | power loss at every operation of the test swap: v2 boots, then v1 comes back byte for byte |
| `power_cut_revert` | power loss at every operation of the revert: v1 byte for byte |
| `power_cut_staging` | power loss at every operation of `ota_write()` and the upgrade request: v1, or v2 as a test |
| `power_cut_permanent` | power loss at every operation of a permanent overwrite: no reboot loop |
| `power_cut_bootstrap` | power loss at every operation of a bootstrap (empty slot0): no reboot loop |

Each power-cut scenario cuts every operation twice, clean and torn.
`power_cut_bootstrap` fails: a torn write of the slot0 magic leaves
`magic=bad`, which MCUboot boots and never reverts, but which the loader
cannot confirm (`boot_write_img_confirmed()` writes nothing) and reboots
after the trial window, every 60 s.

The sketch (`tests/ota_flow`) runs line commands from stdin. The Serial RX
buffer drops the bytes that do not fit, so images are sent in 512-byte chunks,
each acknowledged by the sketch.

## Not simulated

USB (loader DFU, MCUboot DFU, double tap), the 1200-bps touch, and a hung
sketch: a busy loop does not advance the simulated time, so the watchdog
never bites. The loader runs under `setarch -R`: PR 771 maps the sketch at
0x10000000, which the randomized host heap can cover (about 1 run in 100).
