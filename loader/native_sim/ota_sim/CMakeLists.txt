# Copyright (c) Arduino s.r.l. and/or its affiliated companies
# SPDX-License-Identifier: Apache-2.0

# native_sim stand-ins for the MCUboot OTA simulator (README.md), built into
# both MCUboot and the loader.
if(CONFIG_ARCH_POSIX)
  zephyr_library()
  zephyr_library_sources_ifdef(CONFIG_ARDUINO_RETAINED_EEPROM retained_mem_eeprom.c)
  if(CONFIG_MCUBOOT OR CONFIG_BOOTLOADER_MCUBOOT)
    # power loss at a flash operation (power_cut.c)
    zephyr_library_sources(power_cut.c)
  endif()
  if(CONFIG_MCUBOOT)
    zephyr_library_sources(mcuboot_sim_boot.c)
    zephyr_library_link_libraries(MCUBOOT_BOOTUTIL)
  elseif(CONFIG_BOOTLOADER_MCUBOOT)
    zephyr_library_sources(mcuboot_sim_loader.c)
    zephyr_ld_options(-Wl,--wrap=sys_arch_reboot)
  endif()
endif()
