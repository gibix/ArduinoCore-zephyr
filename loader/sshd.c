/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Serve the loader shell over SSH (CONFIG_SSH_SERVER), optionally through a
 * WireGuard tunnel to one peer (CONFIG_WIREGUARD). Zephyr has no
 * configuration to start either; the Ethernet address itself comes from
 * net_config (CONFIG_NET_CONFIG_SETTINGS).
 */

#if defined(CONFIG_SSH_SERVER)

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/ssh/keygen.h>
#include <zephyr/net/ssh/server.h>
#include <zephyr/shell/shell_ssh.h>
#include <zephyr/sys/base64.h>

#if defined(CONFIG_WIREGUARD)
#include <zephyr/net/virtual.h>
#include <zephyr/net/virtual_mgmt.h>
#include <zephyr/net/wireguard.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(loader_sshd, LOG_LEVEL_INF);

#define HOST_KEY_INDEX 0
#define WG_KEEPALIVE_S 25

static int host_key_load(void) {
	/* Runs from SYS_INIT, on the main thread's stack */
	static const char key_b64[] = CONFIG_LOADER_SSHD_HOST_KEY;
	uint8_t der[(sizeof(key_b64) / 4) * 3 + 3];
	size_t len;
	int ret;

	ret = base64_decode(der, sizeof(der), &len, key_b64, sizeof(key_b64) - 1);
	if (ret == 0) {
		ret = ssh_keygen_import(HOST_KEY_INDEX, true, SSH_HOST_KEY_FORMAT_DER, der, len);
	}
	memset(der, 0, sizeof(der));
	return ret;
}

static int sshd_start(void) {
	/* Any address, so the server answers on Ethernet and in the tunnel. */
	struct net_sockaddr_in bind_addr = {
		.sin_family = NET_AF_INET,
		.sin_port = net_htons(CONFIG_SSH_PORT),
	};
	int ret = host_key_load();

	if (ret < 0) {
		LOG_ERR("no host key: %d", ret);
		return ret;
	}

	return ssh_server_start(ssh_server_instance(0), (struct net_sockaddr *)&bind_addr,
				HOST_KEY_INDEX, NULL, CONFIG_LOADER_SSHD_PASSWORD, NULL, 0,
				shell_sshd_event_callback, shell_sshd_transport_event_callback,
				NULL);
}

#if defined(CONFIG_WIREGUARD)

#if defined(CONFIG_NET_NAMESPACE_COMPAT_MODE)
#define NET_ADDR_IN4(a) ((a)->net_in_addr)
#else
#define NET_ADDR_IN4(a) ((a)->in_addr)
#endif

static void find_vpn(struct net_if *iface, void *user_data) {
	struct net_if **vpn = user_data;

	if (*vpn == NULL && net_if_l2(iface) == &NET_L2_GET_NAME(VIRTUAL) &&
	    (net_virtual_get_iface_capabilities(iface) & VIRTUAL_INTERFACE_VPN)) {
		*vpn = iface;
	}
}

static int parse_ipv4_prefix(const char *str, struct net_sockaddr_in *addr, uint8_t *len) {
	const char *next = net_ipaddr_parse_mask(str, strlen(str), (struct net_sockaddr *)addr, len);

	return (next == NULL || addr->sin_family != NET_AF_INET) ? -EINVAL : 0;
}

/* The peer's tunnel address: the first host of this end's network. */
static struct net_sockaddr_in peer_tunnel_addr = {
	.sin_family = NET_AF_INET,
	.sin_port = net_htons(9), /* discard */
};

/*
 * Zephyr only starts the handshake once traffic is routed to the peer: once
 * Ethernet has its address (net_config), send it one datagram; the persistent
 * keepalive takes over from there.
 */
static void kick_tunnel(struct k_work *work) {
	struct net_if *eth = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
	int sock;

	if (eth == NULL || !net_if_is_carrier_ok(eth) ||
	    net_if_ipv4_get_global_addr(eth, NET_ADDR_PREFERRED) == NULL) {
		k_work_reschedule(k_work_delayable_from_work(work), K_MSEC(100));
		return;
	}

	sock = zsock_socket(NET_AF_INET, NET_SOCK_DGRAM, NET_IPPROTO_UDP);
	if (sock >= 0) {
		(void)zsock_sendto(sock, "", 1, 0, (struct net_sockaddr *)&peer_tunnel_addr,
				   sizeof(peer_tunnel_addr));
		zsock_close(sock);
	}
}

static K_WORK_DELAYABLE_DEFINE(kick_work, kick_tunnel);

static int wireguard_up(void) {
	struct virtual_interface_req_params params = {0};
	struct wireguard_peer_config peer = {0};
	struct net_sockaddr_in addr, mask;
	struct net_if *vpn = NULL;
	struct net_if *peer_iface = NULL;
	uint8_t key[NET_VIRTUAL_MAX_PUBLIC_KEY_LEN];
	uint32_t mask_bits;
	uint8_t len;
	size_t olen;
	int ret;

	net_if_foreach(find_vpn, &vpn);
	if (vpn == NULL) {
		return -ENODEV;
	}

	ret = base64_decode(key, sizeof(key), &olen, CONFIG_LOADER_WIREGUARD_PRIVATE_KEY,
			    strlen(CONFIG_LOADER_WIREGUARD_PRIVATE_KEY));
	if (ret || olen != sizeof(key)) {
		return -EINVAL;
	}
	params.private_key.data = key;
	params.private_key.len = sizeof(key);
	ret = net_mgmt(NET_REQUEST_VIRTUAL_INTERFACE_SET_PRIVATE_KEY, vpn, &params, sizeof(params));
	memset(key, 0, sizeof(key));
	if (ret < 0) {
		return ret;
	}

	/* This end of the tunnel; the peer is the rest of its network */
	if (parse_ipv4_prefix(CONFIG_LOADER_WIREGUARD_ADDRESS, &addr, &len) ||
	    net_mask_len_to_netmask(NET_AF_INET, len, (struct net_sockaddr *)&mask) ||
	    net_if_ipv4_addr_add(vpn, &addr.sin_addr, NET_ADDR_MANUAL, 0) == NULL ||
	    !net_if_ipv4_set_netmask_by_addr(vpn, &addr.sin_addr, &mask.sin_addr) ||
	    !net_ipaddr_parse(CONFIG_LOADER_WIREGUARD_PEER_ENDPOINT,
			      strlen(CONFIG_LOADER_WIREGUARD_PEER_ENDPOINT),
			      (struct net_sockaddr *)&peer.endpoint_ip)) {
		return -EINVAL;
	}
	peer.allowed_ip[0].addr.family = NET_AF_INET;
	NET_ADDR_IN4(&peer.allowed_ip[0].addr) = addr.sin_addr;
	peer.allowed_ip[0].mask_len = len;
	peer.allowed_ip[0].is_valid = true;
	peer.public_key = CONFIG_LOADER_WIREGUARD_PEER_PUBLIC_KEY;
	peer.keepalive_interval = WG_KEEPALIVE_S;

	ret = wireguard_peer_add(&peer, &peer_iface);
	if (ret < 0) {
		return ret;
	}

	mask_bits = len ? ~0U << (32 - len) : 0;
	peer_tunnel_addr.sin_addr.s_addr =
		net_htonl((net_ntohl(addr.sin_addr.s_addr) & mask_bits) + 1);
	k_work_schedule(&kick_work, K_NO_WAIT);
	return 0;
}
#endif /* CONFIG_WIREGUARD */

static int sshd_init(void) {
	int ret = sshd_start();

	if (ret < 0) {
		LOG_ERR("SSH server failed to start: %d", ret);
	}

#if defined(CONFIG_WIREGUARD)
	ret = wireguard_up();
	if (ret < 0) {
		LOG_ERR("WireGuard setup failed: %d", ret);
	}
#endif
	return 0;
}

/* After wireguard_init (NET_INIT_PRIO) and net_config (NET_CONFIG_INIT_PRIO) */
BUILD_ASSERT(CONFIG_NET_INIT_PRIO < 99 && CONFIG_NET_CONFIG_INIT_PRIO < 99);
SYS_INIT(sshd_init, APPLICATION, 99);

#endif /* CONFIG_SSH_SERVER */
