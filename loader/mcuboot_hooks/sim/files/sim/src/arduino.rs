// Copyright (c) Arduino s.r.l. and/or its affiliated companies
//
// SPDX-License-Identifier: Apache-2.0

// Tests of the Arduino MCUboot hooks (core loader/mcuboot_hooks), included in
// image.rs so that they use its helpers. Built with the arduino-hooks feature.

impl Images {
    /// A permanent request as MCUboot's USB DFU and Zephyr's
    /// boot_request_upgrade() leave it (boot_set_next()): magic, image_ok
    /// and swap info PERM in the secondary trailer. The images come with
    /// the magic already written.
    #[cfg(feature = "arduino-hooks")]
    fn mark_arduino_permanent(&self, flash: &mut SimMultiFlash) {
        self.mark_permanent_upgrades(flash, 1);
        for image in &self.images {
            let slot = &image.slots[1];
            let dev = flash.get_mut(&slot.dev_id).unwrap();
            let mut info = vec![dev.erased_val(); dev.align()];
            info[0] = 3; // BOOT_SWAP_TYPE_PERM, image 0
            dev.write(slot.trailer_off + c::boot_max_align(), &info).unwrap();
        }
    }

    /// try_upgrade() with an Arduino permanent request.
    #[cfg(feature = "arduino-hooks")]
    fn try_arduino_perm_upgrade(&self, stop: Option<i32>) -> (SimMultiFlash, i32) {
        let mut flash = self.flash.clone();
        self.mark_arduino_permanent(&mut flash);

        let mut counter = stop.unwrap_or(0);
        let (first_interrupted, count) = match c::boot_go(&mut flash, &self.areadesc,
                                                          Some(&mut counter), None, false) {
            x if x.interrupted() => (true, stop.unwrap()),
            x if x.success() => (false, -counter),
            x => panic!("Unknown return: {:?}", x),
        };

        counter = 0;
        if first_interrupted {
            match c::boot_go(&mut flash, &self.areadesc, Some(&mut counter), None, false) {
                x if x.interrupted() => panic!("Shouldn't stop again"),
                x if x.success() => (),
                x => panic!("Unknown return: {:?}", x),
            }
        }

        (flash, count - counter)
    }

    /// Arduino image access hooks: a permanent request (USB DFU upload)
    /// overwrites the primary slot instead of swapping, also when the
    /// copy is interrupted at any flash operation, and is never reverted.
    pub fn run_arduino_perm_copy_with_fails(&self) -> bool {
        if !Caps::Bootstrap.present() || !self.is_swap_upgrade() {
            return false;
        }

        let mut fails = 0;
        let (_, total_flash_ops) = self.try_arduino_perm_upgrade(None);

        // 0: no interruption.
        for i in 0 .. total_flash_ops {
            let stop = if i == 0 { None } else { Some(i) };
            let (mut flash, _) = self.try_arduino_perm_upgrade(stop);

            if !self.verify_images(&flash, 0, 1) {
                warn!("Primary slot is not the new image, step {} of {}",
                      i, total_flash_ops);
                fails += 1;
            }
            if self.verify_images(&flash, 1, 0) {
                warn!("Old image kept in the secondary slot (swapped), step {} of {}",
                      i, total_flash_ops);
                fails += 1;
            }
            if !self.verify_trailers(&flash, 0, BOOT_MAGIC_GOOD,
                                     BOOT_FLAG_SET, BOOT_FLAG_SET) {
                warn!("Mismatched trailer for the primary slot, step {} of {}",
                      i, total_flash_ops);
                fails += 1;
            }
            if !self.verify_trailers(&flash, 1, BOOT_MAGIC_UNSET,
                                     BOOT_FLAG_UNSET, BOOT_FLAG_UNSET) {
                warn!("Mismatched trailer for the secondary slot, step {} of {}",
                      i, total_flash_ops);
                fails += 1;
            }

            // Later boots keep the new image and write nothing.
            for _ in 0 .. 2 {
                let mut counter = 0;
                if !c::boot_go(&mut flash, &self.areadesc, Some(&mut counter),
                               None, false).success() {
                    warn!("Later boot failed, step {} of {}", i, total_flash_ops);
                    fails += 1;
                }
                if counter != 0 {
                    warn!("Later boot wrote {} times, step {} of {}",
                          -counter, i, total_flash_ops);
                    fails += 1;
                }
            }
            if !self.verify_images(&flash, 0, 1) {
                warn!("New image reverted, step {} of {}", i, total_flash_ops);
                fails += 1;
            }

            if skip_slow_test() {
                break;
            }
        }

        if fails > 0 {
            error!("{} failures over {} interruption points", fails,
                   total_flash_ops);
        }

        fails > 0
    }

    /// Arduino hooks, USB DFU on a retention boot mode request
    /// (dfu_boot_mode.c), with the download staged in the secondary slot
    /// as MCUboot's DFU leaves it.
    #[cfg(feature = "arduino-hooks")]
    pub fn run_arduino_dfu_boot_mode(&self) -> bool {
        if !Caps::Bootstrap.present() || !self.is_swap_upgrade() {
            return false;
        }

        let mut fails = 0;

        // 1. A request enters DFU once, is cleared, and the download is
        // installed in the same boot.
        let mut flash = self.flash.clone();
        self.mark_arduino_permanent(&mut flash);
        let mut io = c::ArduinoBootIo { boot_mode: 1, ..Default::default() };
        if !c::arduino_boot(&mut flash, &self.areadesc, &mut io) ||
            io.dfu_entered != 1 || io.boot_mode != 0 || io.boot_rc != 0 {
            warn!("1. DFU request: {:?}", io);
            fails += 1;
        }
        if !self.verify_images(&flash, 0, 1) {
            warn!("1. Download not installed");
            fails += 1;
        }

        // 2. Without a request nothing waits in DFU; the queued test update
        // is swapped in as usual.
        let mut flash = self.flash.clone();
        let mut io = c::ArduinoBootIo::default();
        if !c::arduino_boot(&mut flash, &self.areadesc, &mut io) ||
            io.dfu_entered != 0 || io.boot_rc != 0 || !self.verify_images(&flash, 0, 1) {
            warn!("2. No request: {:?}", io);
            fails += 1;
        }

        // 3. A request during an interrupted test swap: the swap is finished
        // first, DFU is skipped and the request is not left behind.
        // (The images come with a test update queued in slot 1.) Stop halfway,
        // once the swap status is recorded.
        let mut probe = self.flash.clone();
        let mut total = 0;
        if !c::boot_go(&mut probe, &self.areadesc, Some(&mut total), None, false).success() {
            warn!("3. Test swap failed");
            fails += 1;
        }
        let mut flash = self.flash.clone();
        let mut counter = -total / 2;
        if !c::boot_go(&mut flash, &self.areadesc, Some(&mut counter), None,
                       false).interrupted() {
            warn!("3. Test swap was not interrupted");
            fails += 1;
        }
        let mut io = c::ArduinoBootIo { boot_mode: 1, ..Default::default() };
        if !c::arduino_boot(&mut flash, &self.areadesc, &mut io) ||
            io.dfu_entered != 0 || io.boot_mode != 0 || io.boot_rc != 0 {
            warn!("3. DFU during a swap: {:?}", io);
            fails += 1;
        }
        if !self.verify_images(&flash, 0, 1) {
            warn!("3. Interrupted swap not completed");
            fails += 1;
        }

        // 4. No bootable image: reboot into DFU, wait there, and install a
        // download on the next boot.
        let mut flash = self.flash.clone();
        for image in &self.images {
            for slot in &image.slots {
                flash.get_mut(&slot.dev_id).unwrap().erase(slot.base_off, slot.len).unwrap();
            }
        }
        let mut io = c::ArduinoBootIo::default();
        if c::arduino_boot(&mut flash, &self.areadesc, &mut io) ||
            io.boot_rc == 0 || io.rebooted != 1 || io.boot_mode != 1 {
            warn!("4. No image, first boot: {:?}", io);
            fails += 1;
        }
        if c::arduino_boot(&mut flash, &self.areadesc, &mut io) ||
            io.dfu_entered != 1 || io.rebooted != 1 || io.boot_mode != 1 {
            warn!("4. No image, DFU without a download: {:?}", io);
            fails += 1;
        }
        // The download: the image without its trailer, then a permanent
        // request.
        for image in &self.images {
            let slot = &image.slots[1];
            let mut buf = vec![0u8; slot.trailer_off - slot.base_off];
            self.flash.get(&slot.dev_id).unwrap().read(slot.base_off, &mut buf).unwrap();
            flash.get_mut(&slot.dev_id).unwrap().write(slot.base_off, &buf).unwrap();
        }
        self.mark_upgrades(&mut flash, 1);
        self.mark_arduino_permanent(&mut flash);
        if !c::arduino_boot(&mut flash, &self.areadesc, &mut io) ||
            io.dfu_entered != 1 || io.boot_mode != 0 || io.boot_rc != 0 ||
            !self.verify_images(&flash, 0, 1) {
            warn!("4. No image, DFU with a download: {:?}", io);
            fails += 1;
        }

        if fails > 0 {
            error!("{} DFU boot mode failures", fails);
        }
        fails > 0
    }

    /// Arduino hooks, double tap of the reset button into USB DFU
    /// (double_tap.c).
    #[cfg(feature = "arduino-hooks")]
    pub fn run_arduino_double_tap(&self) -> bool {
        if !Caps::Bootstrap.present() || !self.is_swap_upgrade() {
            return false;
        }

        let mut fails = 0;
        let cleared = [0u8; 4];

        // 1. One press: the window opens and closes, the board boots.
        let mut flash = self.flash.clone();
        let mut io = c::ArduinoBootIo { pin_reset: 1, ..Default::default() };
        if !c::arduino_boot(&mut flash, &self.areadesc, &mut io) ||
            io.window_slept != 1 || io.dfu_entered != 0 || io.cookie != cleared ||
            io.boot_rc != 0 {
            warn!("1. Single press: {:?}", io);
            fails += 1;
        }

        // 2. A second press inside the window: DFU on the next boot, once.
        let mut io = c::ArduinoBootIo { pin_reset: 1, press_in_window: 1,
                                        ..Default::default() };
        if c::arduino_boot(&mut flash, &self.areadesc, &mut io) ||
            io.reset_in_window != 1 || io.cookie == cleared {
            warn!("2. First press: {:?}", io);
            fails += 1;
        }
        io.press_in_window = 0;
        if !c::arduino_boot(&mut flash, &self.areadesc, &mut io) ||
            io.dfu_entered != 1 || io.window_slept != 0 || io.cookie != cleared ||
            io.boot_mode != 0 || io.boot_rc != 0 {
            warn!("2. Second press: {:?}", io);
            fails += 1;
        }
        if !c::arduino_boot(&mut flash, &self.areadesc, &mut io) ||
            io.dfu_entered != 0 || io.boot_rc != 0 {
            warn!("2. Press after DFU: {:?}", io);
            fails += 1;
        }

        // 3. Software and watchdog resets never wait.
        let mut io = c::ArduinoBootIo::default();
        if !c::arduino_boot(&mut flash, &self.areadesc, &mut io) ||
            io.window_slept != 0 || io.dfu_entered != 0 || io.boot_rc != 0 {
            warn!("3. Software reset: {:?}", io);
            fails += 1;
        }

        if fails > 0 {
            error!("{} double tap failures", fails);
        }
        fails > 0
    }
}
