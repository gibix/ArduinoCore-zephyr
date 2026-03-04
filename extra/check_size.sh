#!/bin/bash
#
# Wrapper for arm-zephyr-eabi-size that dynamically adjusts section
# accounting based on CONFIG_LLEXT_RODATA_NO_RELOC.
#
# When CONFIG_LLEXT_RODATA_NO_RELOC=y (in loader/prj.conf or variant .conf):
#   .llext.rodata.noreloc stays in flash -> rename to .flash.llext.* so
#   regex.data won't count it against the LLEXT heap limit.
#
# When CONFIG_LLEXT_RODATA_NO_RELOC=n or unset:
#   .llext.rodata.noreloc is copied to LLEXT heap -> keep the name so
#   regex.data counts it against the heap limit.
#
# Usage (from platform.txt):
#   recipe.size.pattern=bash "{runtime.platform.path}/extra/check_size.sh"
#     "{compiler.path}{compiler.size.cmd}" "{build.path}/{build.project_name}.elf"
#     "{build.variant.path}"
#

SIZE_CMD="$1"
ELF="$2"
VARIANT_DIR="$3"

output=$("$SIZE_CMD" -A "$ELF")

# Check if RODATA_NO_RELOC is enabled in the loader or variant config.
# The loader's prj.conf is two levels up from the variant dir (in the
# platform root), but we also check the variant-specific .conf.
PLATFORM_DIR=$(dirname "$(dirname "$VARIANT_DIR")")
rodata_no_reloc=false
for conf in "$PLATFORM_DIR/loader/prj.conf" "$VARIANT_DIR"/*.conf; do
  if [ -f "$conf" ] && grep -q "^CONFIG_LLEXT_RODATA_NO_RELOC=y" "$conf" 2>/dev/null; then
    rodata_no_reloc=true
    break
  fi
done

if $rodata_no_reloc; then
  # Rename .llext.* -> .flash.llext.* so regex.data won't match,
  # but regex (flash/program space) still matches via .flash.llext.
  echo "$output" | sed 's/^\.llext\./.flash.llext./'
else
  echo "$output"
fi
