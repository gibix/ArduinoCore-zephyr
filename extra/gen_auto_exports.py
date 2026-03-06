#!/usr/bin/env python

# Copyright (c) Arduino s.r.l. and/or its affiliated companies
# SPDX-License-Identifier: Apache-2.0

"""
Generate auto_exports.c with FORCE_EXPORT_SYM() calls for all public functions
in a Zephyr loader ELF that aren't already exported via LLEXT.
"""

import argparse
import re
import sys

from elftools.common.exceptions import ELFError
from elftools.common.utils import parse_cstring_from_stream, struct_parse
from elftools.construct.macros import UNInt32, UNInt64
from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection

NativePtr = None

def get_str_at(elf, addr):
    for section in elf.iter_sections():
        if (section['sh_type'] == 'SHT_NOBITS'
                or addr < section['sh_addr']
                or addr >= section['sh_addr'] + section['sh_size']):
            continue
        file_offset = section['sh_offset'] + addr - section['sh_addr']
        return parse_cstring_from_stream(elf.stream, file_offset).decode(
            'utf-8', errors='replace')
    return None

def get_ptr_at(elf, addr):
    for section in elf.iter_sections():
        if (section['sh_type'] == 'SHT_NOBITS'
                or addr < section['sh_addr']
                or addr >= section['sh_addr'] + section['sh_size']):
            continue
        file_offset = section['sh_offset'] + addr - section['sh_addr']
        return struct_parse(NativePtr, elf.stream, file_offset)
    return None

def get_llext_exported(elf):
    """Get the set of names and addresses already exported via LLEXT."""
    names = set()
    addrs = set()
    for section in elf.iter_sections():
        if not isinstance(section, SymbolTableSection):
            continue
        for symbol in section.iter_symbols():
            if not symbol.name.startswith("__llext_sym_"):
                continue
            llext_sym_addr = symbol['st_value']
            sym_name = get_str_at(elf, get_ptr_at(elf, llext_sym_addr))
            sym_value = get_ptr_at(elf, llext_sym_addr + NativePtr.length)
            if sym_name:
                names.add(sym_name)
            if sym_value:
                addrs.add(sym_value)
    return names, addrs

# Default exclude patterns (regexes matched against the full symbol name)
DEFAULT_EXCLUDES = [
    r'^_',                  # private/internal (_*, __*)
    r'^arch_',              # architecture internals
    r'^z_(?!log_)',         # Zephyr private (but not z_log_*)
    r'^boot_',              # boot internals
    r'^reset_',             # reset handlers
    r'^isr_',               # interrupt handlers
    r'^sys_\w+_init$',      # init-level functions
    r'^llext_',             # LLEXT loader internals (arbitrary code loading)
    r'^mbedtls_',           # TLS internals (use socket API instead)
    r'^whd_',               # CYW43 WiFi driver internals
    r'^HAL_',               # STM32 HAL (raw peripheral access)
    r'^LL_',                # STM32 low-level driver
    r'^stm32_',             # STM32 vendor internals
    r'^udc_',               # USB device controller internals
    r'^USB_',               # raw USB register access
    r'^SDMMC_',             # raw SD/MMC peripheral
    r'^FMC_',               # raw memory controller
    r'^dma_',               # raw DMA (use Zephyr DMA API)
    r'^dmamux_',            # raw DMA mux
    r'^cy_',                # Cypress vendor internals
    r'^airoc_',             # Infineon AIROC internals
    r'^mipi_',              # raw MIPI peripheral
    r'^sdio_',              # raw SDIO peripheral
    r'^soc_',               # SoC internals
    r'^do_llext_',          # LLEXT internals
    r'^main$',              # loader main
    r'^idle$',              # idle thread
    # NXP vendor internals
    r'^wlan_',              # NXP WLAN driver
    r'^wifi_',              # NXP WiFi driver internals
    r'^mlan_',              # NXP MLAN driver
    r'^wrapper_',           # NXP driver wrappers
    r'^OSA_',               # NXP OS abstraction
    r'^FLEXSPI_',           # NXP FlexSPI peripheral
    r'^CLOCK_',             # NXP clock control
    r'^POWER_',             # NXP power control
    r'^SPI_',               # NXP SPI HAL
    r'^I2C_',               # NXP I2C HAL
    r'^nxp_',               # NXP internals
    r'^memc_',              # memory controller driver
    r'^OSTIMER_',           # NXP OS timer peripheral
    r'^OCOTP_',             # NXP OTP/fuse controller
    r'^cau_',               # NXP CAU crypto/temp
    # Silicon Labs vendor internals
    r'^sl_',                # Silicon Labs API
    r'^sli_',               # Silicon Labs internal
    r'^ll_',                # Silicon Labs link layer
    r'^RAIL_',              # Silicon Labs radio
    r'^hci_',               # HCI internals
    r'^bgbuf_',             # Silicon Labs BLE buffers
    r'^CMU_',               # Silicon Labs clock management
    r'^EMU_',               # Silicon Labs energy management
    r'^MSC_',               # Silicon Labs memory system controller
    r'^EUSART_',            # Silicon Labs EUSART peripheral
    r'^USART_',             # Silicon Labs USART peripheral
    r'^BTLE_',              # Silicon Labs BLE internals
    r'^usch_',              # Silicon Labs USB host
    r'^psa_',               # PSA crypto internals
    r'^bg_',                # Silicon Labs BLE generic
    r'^sleeptimer_',        # Silicon Labs sleeptimer HAL
    r'^llcp_',              # Silicon Labs link layer control
    r'^RAC_',               # Silicon Labs radio controller
    r'^SYSTEM_',            # Silicon Labs system internals
    # Common driver/subsystem internals
    r'^shell_',             # Zephyr shell internals
    r'^pm_',                # power management internals
    r'^zperf_',             # Zephyr network perf tool
    r'^jesd',               # JEDEC flash internals
]

def main():
    global NativePtr

    parser = argparse.ArgumentParser(
        description='Generate LLEXT auto-export C file from loader ELF')
    parser.add_argument('elf', help='Path to zephyr.elf')
    parser.add_argument('-x', '--exclude', action='append', default=[],
                        help='Additional exclude regex patterns')
    parser.add_argument('-o', '--output', default=None,
                        help='Output file (default: stdout)')
    args = parser.parse_args()

    excludes = [re.compile(p) for p in DEFAULT_EXCLUDES + args.exclude]

    with open(args.elf, 'rb') as f:
        try:
            elf = ELFFile(f)
        except ELFError as ex:
            sys.stderr.write(f'ELF error: {ex}\n')
            sys.exit(1)

        if elf.elfclass == 32:
            NativePtr = UNInt32("ptr")
        elif elf.elfclass == 64:
            NativePtr = UNInt64("ptr")

        exported_names, exported_addrs = get_llext_exported(elf)

        candidates = []
        for section in elf.iter_sections():
            if not isinstance(section, SymbolTableSection):
                continue
            for symbol in section.iter_symbols():
                name = symbol.name
                if not name:
                    continue
                if (symbol['st_info']['bind'] != 'STB_GLOBAL'
                        or symbol['st_info']['type'] != 'STT_FUNC'
                        or symbol['st_shndx'] == 'SHN_UNDEF'):
                    continue
                if name in exported_names or symbol['st_value'] in exported_addrs:
                    continue
                if any(pat.search(name) for pat in excludes):
                    continue
                candidates.append(name)

    candidates.sort()

    out = open(args.output, 'w') if args.output else sys.stdout
    try:
        out.write('/* Auto-generated by gen_auto_exports.py — do not edit! */\n')
        out.write('#include <zephyr/llext/symbol.h>\n\n')
        out.write('#define FORCE_EXPORT_SYM(name) \\\n')
        out.write('       extern void name(void); \\\n')
        out.write('       EXPORT_SYMBOL(name);\n\n')
        for name in candidates:
            out.write(f'FORCE_EXPORT_SYM({name})\n')
        out.write(f'\n/* {len(candidates)} symbols auto-exported */\n')
    finally:
        if args.output:
            out.close()

    sys.stderr.write(f'gen_auto_exports: {len(candidates)} new symbols '
                     f'({len(exported_names)} already exported)\n')

if __name__ == '__main__':
    main()
