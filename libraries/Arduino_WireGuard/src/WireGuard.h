/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef WIREGUARD_H
#define WIREGUARD_H

#include <Arduino.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/virtual.h>
#include <zephyr/net/virtual_mgmt.h>
#include <zephyr/net/wireguard.h>
#include <zephyr/sys/base64.h>

#ifdef __cplusplus
}
#endif

class WireGuardClass {
public:
	/**
	 * @brief Set up the WireGuard VPN tunnel.
	 *
	 * @param localIP     VPN local address in CIDR notation, e.g. "10.0.0.2/24"
	 * @param privateKey  Base64-encoded private key
	 * @param remotePubKey Base64-encoded public key of the remote peer
	 * @param endpoint    Peer endpoint as "ip:port", e.g. "203.0.113.1:51820"
	 * @param allowedIPs  Comma-separated allowed IPs in CIDR, default "0.0.0.0/0"
	 * @param keepalive   Persistent keepalive interval in seconds, default 25
	 * @return 0 on success, negative errno on failure
	 */
	int begin(const char *localIP, const char *privateKey,
		  const char *remotePubKey, const char *endpoint,
		  const char *allowedIPs = "0.0.0.0/0",
		  int keepalive = 25);

	/** Tear down the VPN tunnel. */
	void end();

	/** @return the peer ID assigned by the kernel, or -1 if not connected. */
	int peerID() { return _peer_id; }

private:
	struct net_if *_vpn_iface = nullptr;
	int _peer_id = -1;

	struct net_if *findVPNInterface();
};

extern WireGuardClass WireGuard;

#endif // WIREGUARD_H
