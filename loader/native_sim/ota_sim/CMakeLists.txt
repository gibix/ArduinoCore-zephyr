# Copyright (c) Arduino s.r.l. and/or its affiliated companies
# SPDX-License-Identifier: Apache-2.0

# native_sim stand-ins for the MCUboot OTA simulator (README.md), built into
# both MCUboot and the loader.
if(CONFIG_ARCH_POSIX)
  zephyr_library()
  zephyr_library_sources_ifdef(CONFIG_ARDUINO_RETAINED_EEPROM retained_mem_eeprom.c)
  if(CONFIG_MCUBOOT)
    zephyr_library_sources(mcuboot_sim_boot.c)
    zephyr_library_link_libraries(MCUBOOT_BOOTUTIL)
  endif()
endif()
