# WireGuard

A fast, simple VPN library for Arduino on Zephyr. Much smaller than TLS-based
alternatives.

## Why

Classic VPN uses:

- Private overlay network between devices
- Route traffic through a remote server
- Bypass censorship or geo-restrictions

Extra benefits over TLS:

- Mutual authentication built in (no need for mTLS)
- Works at L3: secures TCP, UDP, and any other protocol
- Native roaming: switch WiFi networks without dropping the tunnel
- Tiny footprint (~600 B vs ~8-15 KB RAM for TLS)
- Compatible with most commercial VPN services

## Use Cases

- Encrypted peer-to-peer link over IPv4 or IPv6
- Overlay network for distributed IoT devices
- NAT traversal without port forwarding
- Roaming: move between networks, keep the same tunnel address
- Secure remote access to home or office devices

## WireGuard and TLS

Compared to TLS (using `ZephyrSSLClient` with mbedTLS), WireGuard has a much
smaller RAM footprint at the sketch level:

| | WireGuard | TLS |
|--|-----------|-----|
| Static globals | ~37 B | ~2,040 B |
| Runtime heap | 0 | ~2 KB |
| Subsystem state | kernel-managed | ~4-6.5 KB (mbedTLS) |
| **Total** | **~600 B** | **~8-11 KB** |
| **Peak (handshake)** | **~600 B** | **~10-15 KB** |

The TLS cost is dominated by mbedTLS structures (`ssl_context`, `ssl_config`,
`x509_crt`) and the embedded PEM CA certificate (~1.9 KB). WireGuard's crypto
state is managed entirely by the Zephyr kernel networking layer — the sketch
only holds a lightweight handle.

WireGuard also operates at L3, so it secures all traffic (TCP, UDP, etc.)
without per-connection overhead, while TLS requires a separate handshake and
context for each TCP connection.
