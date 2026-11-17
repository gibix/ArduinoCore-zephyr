// Copyright (c) Arduino s.r.l. and/or its affiliated companies
//
// SPDX-License-Identifier: Apache-2.0

//! Tests of the Arduino MCUboot hooks (core loader/mcuboot_hooks), with the
//! arduino-hooks feature. Run tests/core.rs with the same features as well,
//! except perm_with_fails and perm_with_random_fails: with the hooks a
//! permanent update is a copy, not a swap.

#![cfg(feature = "arduino-hooks")]

use bootsim::{testlog, ImagesBuilder, NO_DEPS};

macro_rules! sim_test {
    ($name:ident, $maker:ident($($margs:expr),*), $test:ident($($targs:expr),*)) => {
        #[test]
        fn $name() {
            testlog::setup();
            ImagesBuilder::each_device(|r| {
                let image = r.$maker($($margs),*);
                assert!(!image.$test($($targs),*));
            });
        }
    };
}

sim_test!(arduino_perm_copy_with_fails, make_image(&NO_DEPS, true), run_arduino_perm_copy_with_fails());
sim_test!(arduino_dfu_boot_mode, make_image(&NO_DEPS, true), run_arduino_dfu_boot_mode());
sim_test!(arduino_double_tap, make_image(&NO_DEPS, true), run_arduino_double_tap());
