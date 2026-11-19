/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LOADER_TRIAL_WINDOW_H_
#define LOADER_TRIAL_WINDOW_H_

/*
 * Restart the trial window of an unconfirmed test image: the watchdog thread
 * reboots (and MCUboot reverts) CONFIG_LOADER_TRIAL_WINDOW_MS from now
 * instead of from boot. Implemented in watchdog_feed_thread.c.
 */
void loader_trial_window_restart(void);

#endif /* LOADER_TRIAL_WINDOW_H_ */
