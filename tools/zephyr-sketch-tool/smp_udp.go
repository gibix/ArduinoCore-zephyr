package main

import (
	"fmt"
	"net"
	"time"
)

/*
 * SMP over UDP - the OTA transport.
 *
 * The same requests as the wired path, and the same SMP header built by
 * smpConn.body(), but none of the serial framing: no base64, no NLIP markers,
 * no length prefix and no CRC16. A datagram carries one SMP frame, and UDP's
 * own checksum does what the CRC did. That is why this file is short - only
 * send and receive differ, so smp.go dispatches on smpConn.pkt and everything
 * above that (retries, sequence matching, the upload loop) is shared.
 *
 * The peer is the LOADER, not the bootloader. Zephyr's UDP transport is started
 * for us - CONFIG_MCUMGR_TRANSPORT_UDP_AUTOMATIC_INIT defaults to y, and it
 * attaches to interfaces as they come up - so the board is reachable on port
 * 1337 as soon as the running sketch has an address. Uploads go to slot 2, the
 * secondary, and MCUboot swaps on the next boot with its own rollback.
 *
 * 🔴 There is no authentication and the images are hash-only, not signed
 * (BOOT_SIGNATURE_TYPE_NONE). A wired upload needs physical access; this does
 * not. Trusted networks only until signing is in place.
 */

// udpMTU is CONFIG_MCUMGR_TRANSPORT_UDP_MTU's default. A response never
// approaches it; a request must not exceed it, which bounds the chunk size.
const udpMTU = 1500

// smpOpenUDP dials the board's SMP port. UDP is connectionless, so this cannot
// fail on an unreachable board - that only shows up as a timeout on the first
// request, which is what the handshake is for.
func smpOpenUDP(addr string, timeout time.Duration) (*smpConn, error) {
	if _, _, err := net.SplitHostPort(addr); err != nil {
		// Accept a bare address and supply the default port.
		addr = net.JoinHostPort(addr, "1337")
	}

	conn, err := net.Dial("udp", addr)
	if err != nil {
		return nil, fmt.Errorf("dial %s: %w", addr, err)
	}

	c := newSMPConn(nil)
	c.pkt = conn
	c.pktBuf = make([]byte, udpMTU)
	c.readTimeout = timeout
	return c, nil
}

func (c *smpConn) sendDatagram(body []byte) error {
	if len(body) > udpMTU {
		return fmt.Errorf("frame of %d bytes exceeds the %d-byte UDP MTU; lower -smp-chunk",
			len(body), udpMTU)
	}
	if c.debug {
		fmt.Printf("TX %d bytes\n", len(body))
	}
	if err := c.pkt.SetWriteDeadline(time.Now().Add(c.writeTimeout)); err != nil {
		return err
	}
	_, err := c.pkt.Write(body)
	return err
}

func (c *smpConn) recvDatagram() ([]byte, error) {
	if err := c.pkt.SetReadDeadline(time.Now().Add(c.readTimeout)); err != nil {
		return nil, err
	}
	n, err := c.pkt.Read(c.pktBuf)
	if err != nil {
		if ne, ok := err.(net.Error); ok && ne.Timeout() {
			return nil, fmt.Errorf("timed out waiting for a response")
		}
		return nil, fmt.Errorf("read: %w", err)
	}
	if n < smpHeaderLen {
		return nil, fmt.Errorf("short datagram: %d bytes", n)
	}
	if c.debug {
		fmt.Printf("RX %d bytes\n", n)
	}
	return c.pktBuf[:n], nil
}

// flushDatagrams drops anything still queued from an earlier attempt, so a late
// reply is never mistaken for the answer to the current request. Same reasoning
// as the serial flushInput, and just as necessary: a retry after a timeout is
// exactly when a straggler arrives.
func (c *smpConn) flushDatagrams() {
	buf := make([]byte, udpMTU)
	for {
		if err := c.pkt.SetReadDeadline(time.Now()); err != nil {
			return
		}
		if _, err := c.pkt.Read(buf); err != nil {
			return
		}
	}
}
