/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "WireGuard.h"
#include <string.h>

/* Callback for net_if_foreach: find the first VPN virtual interface */
static void iface_find_vpn(struct net_if *iface, void *user_data)
{
	struct net_if **result = (struct net_if **)user_data;

	if (*result != NULL) {
		return; /* already found */
	}

	if (net_if_l2(iface) != &NET_L2_GET_NAME(VIRTUAL)) {
		return;
	}

	if (!(net_virtual_get_iface_capabilities(iface) & VIRTUAL_INTERFACE_VPN)) {
		return;
	}

	*result = iface;
}

struct net_if *WireGuardClass::findVPNInterface()
{
	struct net_if *iface = NULL;
	net_if_foreach(iface_find_vpn, &iface);
	return iface;
}

int WireGuardClass::begin(const char *localIP, const char *privateKey,
			  const char *remotePubKey, const char *endpoint,
			  const char *allowedIPs, int keepalive)
{
	int ret;

	/* 1. Find the VPN virtual interface */
	_vpn_iface = findVPNInterface();
	if (_vpn_iface == NULL) {
		return -ENODEV;
	}

	/* 2. Decode and set private key */
	uint8_t priv_key[NET_VIRTUAL_MAX_PUBLIC_KEY_LEN];
	size_t olen;

	ret = base64_decode(priv_key, sizeof(priv_key), &olen,
			    (const uint8_t *)privateKey, strlen(privateKey));
	if (ret < 0) {
		return ret;
	}

	struct virtual_interface_req_params params = {};
	params.private_key.data = priv_key;
	params.private_key.len = olen;

	ret = net_mgmt(NET_REQUEST_VIRTUAL_INTERFACE_SET_PRIVATE_KEY,
		       _vpn_iface, &params, sizeof(params));

	memset(priv_key, 0, sizeof(priv_key));

	if (ret < 0) {
		return ret;
	}

	/* 3. Add local IP address to VPN interface */
	struct sockaddr_storage addr_storage = {};
	struct sockaddr *paddr = (struct sockaddr *)&addr_storage;
	uint8_t mask_len = 0;

	const char *next = net_ipaddr_parse_mask(localIP, strlen(localIP),
						 paddr, &mask_len);
	if (next == NULL) {
		return -EINVAL;
	}

	if (paddr->sa_family == AF_INET) {
		struct sockaddr_in *addr4 = (struct sockaddr_in *)paddr;

		if (net_if_ipv4_addr_add(_vpn_iface, &addr4->sin_addr,
					 NET_ADDR_MANUAL, 0) == NULL) {
			return -ENOENT;
		}

		struct sockaddr_in mask = {};
		ret = net_mask_len_to_netmask(AF_INET, mask_len,
					      (struct sockaddr *)&mask);
		if (ret < 0) {
			return ret;
		}

		net_if_ipv4_set_netmask_by_addr(_vpn_iface,
						&addr4->sin_addr,
						&mask.sin_addr);
	} else {
		return -EAFNOSUPPORT;
	}

	/* 4. Build peer config */
	struct wireguard_peer_config peer_config = {};

	peer_config.public_key = remotePubKey;
	peer_config.keepalive_interval = keepalive;

	/* Parse endpoint (ip:port) */
	if (!net_ipaddr_parse(endpoint, strlen(endpoint), paddr)) {
		return -EINVAL;
	}

	if (paddr->sa_family == AF_INET) {
		memcpy(&peer_config.endpoint_ip, paddr, sizeof(struct sockaddr_in));
	} else if (paddr->sa_family == AF_INET6) {
		memcpy(&peer_config.endpoint_ip, paddr, sizeof(struct sockaddr_in6));
	} else {
		return -EAFNOSUPPORT;
	}

	/* Parse allowed IPs */
	const char *aip = allowedIPs;
	int idx = 0;

	do {
		next = net_ipaddr_parse_mask(aip, strlen(aip),
					     paddr, &mask_len);
		if (next == NULL) {
			return -EINVAL;
		}

		if (idx >= WIREGUARD_MAX_SRC_IPS) {
			return -ENOMEM;
		}

		if (paddr->sa_family == AF_INET) {
			struct sockaddr_in *a4 = (struct sockaddr_in *)paddr;
			memcpy(&peer_config.allowed_ip[idx].addr.in_addr,
			       &a4->sin_addr, sizeof(struct in_addr));
			peer_config.allowed_ip[idx].addr.family = AF_INET;
		} else if (paddr->sa_family == AF_INET6) {
			struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)paddr;
			memcpy(&peer_config.allowed_ip[idx].addr.in6_addr,
			       &a6->sin6_addr, sizeof(struct in6_addr));
			peer_config.allowed_ip[idx].addr.family = AF_INET6;
		} else {
			return -EAFNOSUPPORT;
		}

		peer_config.allowed_ip[idx].mask_len = mask_len;
		peer_config.allowed_ip[idx].is_valid = true;
		idx++;

		aip = next;
	} while (aip != NULL && *aip != '\0');

	/* 5. Add peer */
	struct net_if *peer_iface = NULL;

	ret = wireguard_peer_add(&peer_config, &peer_iface);
	if (ret < 0) {
		return ret;
	}

	_peer_id = ret;

	/* 6. Route all traffic through the VPN interface */
	struct in_addr gw = {};
	net_if_ipv4_set_gw(_vpn_iface, &gw);

	return 0;
}

void WireGuardClass::end()
{
	if (_peer_id >= 0) {
		wireguard_peer_remove(_peer_id);
		_peer_id = -1;
	}
	_vpn_iface = NULL;
}

WireGuardClass WireGuard;
