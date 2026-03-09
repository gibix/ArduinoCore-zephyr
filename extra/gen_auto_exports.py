#!/usr/bin/env python

# Copyright (c) Arduino s.r.l. and/or its affiliated companies
# SPDX-License-Identifier: Apache-2.0

"""
Generate auto_exports.c with FORCE_EXPORT_SYM() calls for public functions
in a Zephyr loader ELF, using EDK headers as an allowlist.

Only symbols declared in the EDK's public headers (Zephyr API + Arduino core)
are exported. Vendor HAL headers under modules/ are excluded from scanning.

Sources for the allowlist:
  1. ctags function prototypes from public EDK headers
  2. __syscall declarations (mapped to z_impl_ symbols in the ELF)
  3. explicit force-export list (for symbols not in any header)
"""

import argparse
import os
import re
import subprocess
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

def get_edk_allowlist(edk_dir):
    """Build allowlist from EDK public headers using ctags + __syscall scan.

    Returns a set of symbol names that are declared in the public API headers.
    """
    allowed = set()

    # Directories to scan (public API only, not vendor HAL modules)
    scan_dirs = []
    for subdir in ['zephyr/include', 'ArduinoCore-zephyr']:
        path = os.path.join(edk_dir, subdir)
        if os.path.isdir(path):
            scan_dirs.append(path)

    if not scan_dirs:
        sys.stderr.write(f'gen_auto_exports: warning: no public header dirs '
                         f'found in {edk_dir}\n')
        return allowed

    # 1. ctags: extract function prototypes
    try:
        result = subprocess.run(
            ['ctags', '--c-kinds=p', '-x', '--_xformat=%N', '-R'] + scan_dirs,
            capture_output=True, text=True, timeout=60)
        for line in result.stdout.splitlines():
            name = line.strip()
            if name:
                allowed.add(name)
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        sys.stderr.write(f'gen_auto_exports: warning: ctags failed: {e}\n')

    # 2. __syscall declarations → z_impl_ symbols
    syscall_re = re.compile(
        r'__syscall\s+\S+[\s*]+(\w+)\s*\(')
    for scan_dir in scan_dirs:
        for root, dirs, files in os.walk(scan_dir):
            for fname in files:
                if not fname.endswith('.h'):
                    continue
                path = os.path.join(root, fname)
                try:
                    with open(path, errors='replace') as f:
                        for line in f:
                            m = syscall_re.search(line)
                            if m:
                                # The ELF symbol is z_impl_<name>
                                allowed.add('z_impl_' + m.group(1))
                except OSError:
                    continue

    return allowed

def read_force_list(path):
    """Read symbol names from a force-export list file.

    Returns a set of symbol names that should be exported regardless of
    whether they appear in EDK headers.  These bypass the type filter too,
    so STT_OBJECT symbols (variables, structs) can be force-exported.
    """
    names = set()
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            names.add(line)
    return names

# Safety blocklist: symbols that appear in public headers but should
# never be exported to sketches (dangerous or loader-internal).
SAFETY_BLOCKLIST = [
    r'^llext_',             # LLEXT loader internals (arbitrary code loading)
    r'^do_llext_',          # LLEXT internals
    r'^main$',              # loader main
    r'^idle$',              # idle thread
]

def main():
    global NativePtr

    parser = argparse.ArgumentParser(
        description='Generate LLEXT auto-export C file from loader ELF')
    parser.add_argument('elf', help='Path to zephyr.elf')
    parser.add_argument('--edk-dir', required=True,
                        help='Path to EDK include directory')
    parser.add_argument('-x', '--exclude', action='append', default=[],
                        help='Additional exclude regex patterns')
    parser.add_argument('-o', '--output', default=None,
                        help='Output file (default: stdout)')
    parser.add_argument('--force-list', default=None, metavar='FILE',
                        help='Path to file listing symbols to force-export')
    parser.add_argument('--log-excluded', default=None, metavar='FILE',
                        help='Write excluded symbols with reasons to FILE')
    args = parser.parse_args()

    blocklist = [(p, re.compile(p)) for p in SAFETY_BLOCKLIST + args.exclude]
    allowlist = get_edk_allowlist(args.edk_dir)
    force_set = read_force_list(args.force_list) if args.force_list else set()

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
        exported_addrs.discard(0)

        candidates = []
        excluded = []  # (name, reason)
        for section in elf.iter_sections():
            if not isinstance(section, SymbolTableSection):
                continue
            for symbol in section.iter_symbols():
                name = symbol.name
                if not name:
                    continue
                if (symbol['st_info']['bind'] != 'STB_GLOBAL'
                        or symbol['st_shndx'] == 'SHN_UNDEF'):
                    continue
                in_force = name in force_set
                is_func = symbol['st_info']['type'] == 'STT_FUNC'
                # Non-function symbols are only considered if force-listed
                if not is_func and not in_force:
                    continue
                if not in_force and (name in exported_names or symbol['st_value'] in exported_addrs):
                    continue
                # Safety blocklist
                blocked = None
                for pat_str, pat_re in blocklist:
                    if pat_re.search(name):
                        blocked = pat_str
                        break
                if blocked:
                    excluded.append((name, f'blocklist: {blocked}'))
                    continue
                # Allowlist check (force-listed symbols bypass this)
                if not in_force and name not in allowlist:
                    excluded.append((name, 'not in EDK headers'))
                    continue
                candidates.append(name)

    candidates.sort()
    excluded.sort()

    out = open(args.output, 'w') if args.output else sys.stdout
    try:
        out.write('/* Auto-generated by gen_auto_exports.py — do not edit! */\n')
        out.write('#include <zephyr/llext/symbol.h>\n\n')
        out.write('#define FORCE_EXPORT_SYM(name) \\\n')
        out.write('       extern __attribute__((weak)) void name(void); \\\n')
        out.write('       EXPORT_SYMBOL(name);\n\n')
        for name in candidates:
            out.write(f'FORCE_EXPORT_SYM({name})\n')
        out.write(f'\n/* {len(candidates)} symbols auto-exported */\n')
    finally:
        if args.output:
            out.close()

    if args.log_excluded:
        with open(args.log_excluded, 'w') as lf:
            lf.write(f'# {len(excluded)} symbols excluded, '
                     f'{len(candidates)} exported, '
                     f'{len(exported_names)} already exported\n')
            lf.write(f'# allowlist: {len(allowlist)} names from EDK headers, '
                     f'{len(force_set)} from force list\n')
            lf.write('#\n# symbol\treason\n')
            for name, reason in excluded:
                lf.write(f'{name}\t{reason}\n')

    sys.stderr.write(f'gen_auto_exports: {len(candidates)} new, '
                     f'{len(excluded)} excluded, '
                     f'{len(exported_names)} already exported '
                     f'(allowlist: {len(allowlist)}, '
                     f'force: {len(force_set)})\n')

if __name__ == '__main__':
    main()
