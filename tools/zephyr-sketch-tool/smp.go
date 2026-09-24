// Copyright (c) Arduino s.r.l. and/or its affiliated companies
// SPDX-License-Identifier: Apache-2.0

// An mcumgr/SMP client, just large enough to upload an image to a board sitting
// in MCUboot's serial recovery mode and reset it.
//
// Why this lives here rather than being the stock `mcumgr` CLI: platform.txt
// recipes may only invoke Arduino toolchain tools, and this tool is already one
// of them ({runtime.tools.zephyr-sketch-tool}). It also already knows how to
// build the MCUboot image being uploaded (mcuboot.go), so producing it and
// pushing it stay in one place. Everything below is deliberately the minimum
// that MCUboot's boot/boot_serial accepts - it is not a general SMP library, and
// the stock mcumgr remains the thing to reach for when debugging by hand.
//
// The wire format, as implemented by boot/boot_serial/src/boot_serial.c:
//
//	frame  = be16(len(body)+2) || body || be16(crc16_itu_t(body))
//	body   = SMP header (8 B) || CBOR payload
//	line   = marker || base64(frame fragment) || '\n'
//	marker = 0x06 0x09 for the first line of a frame, 0x04 0x14 after
//
// Fragments are capped so a whole line stays within the bootloader's
// CONFIG_BOOT_MAX_LINE_INPUT_LEN, and a whole frame within its
// CONFIG_BOOT_SERIAL_MAX_RECEIVE_SIZE.

package main

import (
	"bufio"
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"os"
	"time"

	"go.bug.st/serial"
)

const (
	// NLIP line markers, from boot_serial_priv.h.
	nlipPktStart1  = 0x06
	nlipPktStart2  = 0x09
	nlipDataStart1 = 0x04
	nlipDataStart2 = 0x14

	// SMP operations and groups, from boot_serial_priv.h.
	smpOpWrite    = 2
	smpGroupOS    = 0
	smpGroupImage = 1
	smpIDReset    = 5
	smpIDUpload   = 1
	// Group 0, id 1: boot_serial answers this one unconditionally with a bare
	// rc:0 - no #ifdef, no state changed, nothing read from flash. `image
	// list` looks like the natural probe but is the most expensive command
	// there is: with BOOT_SERIAL_IMG_GRP_HASH it validates and SHA-256s every
	// slot holding an image, i.e. up to ~1.2 MB hashed before the upload
	// starts, which can outlast the per-attempt timeout on its own.
	smpIDConsEchoCtrl = 1

	smpHeaderLen = 8

	// Must match the bootloader's own limits. BOOT_SERIAL_FRAME_MTU is 124
	// (a 127-byte line minus the 2 marker bytes and the terminator), and
	// CONFIG_BOOT_SERIAL_MAX_RECEIVE_SIZE defaults to 1024.
	smpFrameMTU    = 124
	smpMaxFrameLen = 1024

	// 🔴 The real constraint is not the byte budget above but the number of
	// LINES a frame takes.
	//
	// MCUboot's RX interrupt handler takes one buffer per line from a pool of
	// CONFIG_BOOT_LINE_BUFS, and a host that writes a whole frame back to back
	// outruns the recovery loop that drains them. When the pool empties the
	// handler drops the rest of the frame - "Not enough memory to store
	// incoming data!" on a console nobody is watching - so the frame never
	// completes and the bootloader simply never answers. On the host that is
	// indistinguishable from a board that is not in recovery at all.
	//
	// Worse, only BOOT_LINE_BUFS-1 are ever available: boot_uart_fifo_getline()
	// recycles a buffer on the *following* call ("Recycle cmd buffer returned
	// previous time"), so the consumer always holds one. A frame of exactly
	// BOOT_LINE_BUFS lines is therefore at the edge, and whether its last line
	// finds a free buffer depends on how fast the loop happens to be running.
	//
	// Confirmed on hardware 2026-08-17: a 928-byte chunk (12 lines) never got a
	// reply, while the same transfer at 672 bytes (8 lines) went through. Six
	// lines leaves a spare buffer and takes the question off the table.
	smpStockLineBufs = 8
	smpUsableLines   = smpStockLineBufs - 1

	// Payload bytes per upload request, sized so a frame fits in 6 lines - one
	// spare buffer even on a stock bootloader - and is a whole number of flash
	// write blocks:
	//   6 lines * 124 chars = 744 base64 chars = 558 decoded bytes
	//   558 - 2 (length) - 8 (SMP header) - 2 (CRC) - 35 (CBOR) = 511  ->  480
	//
	// Boards whose bootloader raises CONFIG_BOOT_LINE_BUFS - as
	// extra/mcuboot/nano_chandler_bfm.conf does - can go faster with
	// -smp-chunk. This default has to work against a stock one.
	smpDefaultChunk = 480

	// How many times a request is resent before giving up. The handshake gets
	// more than the transfer: the board may still be on its way into recovery
	// when the port appears. Six attempts at the default 2s timeout is a ~12s
	// window to start talking, and a board that is simply not in recovery says
	// so within that.
	smpRetries      = 3
	smpFirstRetries = 6

	// How long the offset-0 request may take: it covers erasing the whole
	// target slot, which is 512K of 4K pages on this part.
	smpEraseTimeout = 30 * time.Second
)

// smpConn is a framed SMP connection. It is written against io.ReadWriter
// rather than serial.Port so the framing can be exercised against a fake
// bootloader in tests - this protocol has to be right the first time on
// hardware, where a mistake looks like a board that no longer boots.
type smpConn struct {
	rw           io.ReadWriter
	in           *bufio.Reader
	seq          uint8
	writeTimeout time.Duration
	readTimeout  time.Duration

	// Dump every line to stderr. The protocol is only observable from the
	// host side - the bootloader's own console is the transport - so this is
	// the one way to see what actually went over the wire.
	debug bool

	// Reused across every frame of an upload: ~300 requests, each of which
	// would otherwise allocate and copy the payload three more times on its
	// way to the wire.
	reader   io.Reader
	bodyBuf  []byte
	frameBuf []byte
	encBuf   []byte
	lineBuf  []byte

	// Non-nil when this is an SMP-over-UDP connection, in which case the
	// serial framing below is bypassed entirely - see smp_udp.go.
	pkt    net.Conn
	pktBuf []byte

	writeTimer *time.Timer
	writeDone  chan error
}

// Optional capabilities of the transport. A serial.Port implements all three;
// the in-memory pipe the tests use implements none, and needs none.
type inputFlusher interface{ ResetInputBuffer() error }
type readTimeouter interface{ SetReadTimeout(time.Duration) error }

// setReadTimeout adjusts how long a single response may take.
//
// Needed because the first upload request is not like the others: at offset 0
// MCUboot prepares the target slot, and on this part erasing 512K takes far
// longer than a round trip. Timing it out is not harmless - the erase continues
// while the host resends, so retries pile up behind a busy flash controller and
// the upload never starts. (Diagnosed on hardware 2026-08-17: uploads worked
// with the original 10s timeout and stopped the moment it was lowered to 2s.)
func (c *smpConn) setReadTimeout(d time.Duration) {
	if p, ok := c.rw.(readTimeouter); ok {
		_ = p.SetReadTimeout(d)
	}
}

// timeoutAsEOF turns a serial read timeout into io.EOF.
//
// go.bug.st/serial reports a read timeout as (0, nil), which bufio.Reader
// treats as "nothing yet, try again" - so a silent board becomes an infinite
// loop rather than an error. Anything wrapped here has already had a read
// deadline set, so a zero-length read means the deadline expired.
type timeoutAsEOF struct {
	r io.Reader
}

func (t timeoutAsEOF) Read(p []byte) (int, error) {
	n, err := t.r.Read(p)
	if n == 0 && err == nil {
		return 0, io.EOF
	}
	return n, err
}

func newSMPConn(rw io.ReadWriter) *smpConn {
	timer := time.NewTimer(0)
	if !timer.Stop() {
		<-timer.C
	}

	return &smpConn{
		rw:           rw,
		in:           bufio.NewReader(timeoutAsEOF{rw}),
		reader:       timeoutAsEOF{rw},
		writeTimeout: 5 * time.Second,
		bodyBuf:      make([]byte, smpMaxFrameLen),
		frameBuf:     make([]byte, smpMaxFrameLen),
		encBuf:       make([]byte, base64.StdEncoding.EncodedLen(smpMaxFrameLen)),
		lineBuf:      make([]byte, 0, base64.StdEncoding.EncodedLen(smpMaxFrameLen)*2),
		writeTimer:   timer,
		writeDone:    make(chan error, 1),
	}
}

func smpOpen(portName string, timeout time.Duration) (*smpConn, error) {
	// The baud rate is meaningless for a USB CDC-ACM port - the device
	// ignores it - but the API demands one.
	port, err := serial.Open(portName, &serial.Mode{BaudRate: 115200})
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", portName, err)
	}
	if err := port.SetReadTimeout(timeout); err != nil {
		port.Close()
		return nil, fmt.Errorf("set timeout on %s: %w", portName, err)
	}
	c := newSMPConn(port)
	c.readTimeout = timeout
	c.flushInput()
	return c, nil
}

func (c *smpConn) Close() error {
	if c.pkt != nil {
		return c.pkt.Close()
	}
	if closer, ok := c.rw.(io.Closer); ok {
		return closer.Close()
	}
	return nil
}

// flushInput discards anything already in flight, on both sides of the buffer.
//
// A previous run that failed or was interrupted leaves its unread responses in
// the kernel's receive buffer, and they are delivered to whoever opens the port
// next. Parsing those as answers to the current request produces CRC errors and
// sequence mismatches that look exactly like a board that is not responding -
// including, misleadingly, a *fast* failure. (Diagnosed on hardware 2026-08-17:
// the first upload after a fresh port always worked, every later one failed
// until the port was reopened by something that flushed, such as a 1200-bps
// touch.)
//
// Called before every attempt, not just at open: a retry follows a timeout, and
// the late answer to the previous attempt is exactly the kind of straggler that
// must not be mistaken for this one's.
func (c *smpConn) flushInput() {
	if c.pkt != nil {
		c.flushDatagrams()
		return
	}
	if f, ok := c.rw.(inputFlusher); ok {
		_ = f.ResetInputBuffer()
	}
	c.in.Reset(c.reader)
}

// crc16ITUT is Zephyr's crc16_itu_t: poly 0x1021, MSB first, no reflection and
// no final XOR, seeded with 0 by the bootloader. Appending the CRC big-endian
// makes the receiver's check of "CRC over body+crc == 0" pass.
func crc16ITUT(data []byte) uint16 {
	var crc uint16
	for _, b := range data {
		crc ^= uint16(b) << 8
		for i := 0; i < 8; i++ {
			if crc&0x8000 != 0 {
				crc = crc<<1 ^ 0x1021
			} else {
				crc <<= 1
			}
		}
	}
	return crc
}

// write bounds a serial write in time.
//
// go.bug.st/serial has a read timeout but no write timeout, and a write to a
// CDC-ACM port whose device is not draining blocks in the tty layer forever -
// which is exactly what happens when the upload is aimed at a board that is
// running the loader instead of sitting in recovery: nothing on the device
// reads that endpoint, the kernel buffer fills, and the tool hangs with no
// output and no timeout. (Observed on hardware 2026-08-17: a 10-minute wedge.)
//
// The goroutine is abandoned rather than cancelled - there is no way to
// interrupt the blocked write - but every caller turns this error into a failed
// upload and exits. One goroutine and one timer per *frame*, both reused across
// the ~300 frames of an upload; the timer is stopped rather than left to fire.
func (c *smpConn) write(buf []byte) error {
	go func() {
		// The library's unix Write is a bare write(2) with no short-write
		// loop, so finish the job here.
		for off := 0; off < len(buf); {
			n, err := c.rw.Write(buf[off:])
			if err != nil {
				c.writeDone <- err
				return
			}
			off += n
		}
		c.writeDone <- nil
	}()

	c.writeTimer.Reset(c.writeTimeout)
	defer c.writeTimer.Stop()

	select {
	case err := <-c.writeDone:
		return err
	case <-c.writeTimer.C:
		return fmt.Errorf("the board stopped accepting data (write blocked for %s) - "+
			"is it still in serial recovery?", c.writeTimeout)
	}
}

// send frames one SMP body and writes it out as base64 lines.
//
// The whole frame goes out in a single write. The bytes on the wire are
// identical either way - the line structure is in the buffer - and the frame is
// already sized (see smpDefaultChunk) on the assumption that the host emits all
// of its lines before the bootloader's recovery loop drains any, so coalescing
// stays inside the envelope that sizing was designed for. It is simply fewer
// USB transfers, and one bounded write instead of one per line.
func (c *smpConn) send(body []byte) error {
	if c.pkt != nil {
		return c.sendDatagram(body)
	}

	frameLen := 2 + len(body) + 2
	if frameLen > smpMaxFrameLen {
		return fmt.Errorf("frame of %d bytes exceeds the bootloader's %d-byte receive buffer",
			frameLen, smpMaxFrameLen)
	}

	frame := c.frameBuf[:0]
	frame = binary.BigEndian.AppendUint16(frame, uint16(len(body)+2))
	frame = append(frame, body...)
	frame = binary.BigEndian.AppendUint16(frame, crc16ITUT(body))

	encoded := c.encBuf[:base64.StdEncoding.EncodedLen(len(frame))]
	base64.StdEncoding.Encode(encoded, frame)

	out := c.lineBuf[:0]
	for off := 0; off < len(encoded); off += smpFrameMTU {
		end := off + smpFrameMTU
		if end > len(encoded) {
			end = len(encoded)
		}

		if off == 0 {
			out = append(out, nlipPktStart1, nlipPktStart2)
		} else {
			out = append(out, nlipDataStart1, nlipDataStart2)
		}
		out = append(out, encoded[off:end]...)
		out = append(out, '\n')
	}

	if c.debug {
		fmt.Fprintf(os.Stderr, "TX %q\n", out)
	}
	if err := c.write(out); err != nil {
		return fmt.Errorf("write: %w", err)
	}
	return nil
}

// recv reads lines until a complete, CRC-valid frame arrives, and returns it -
// SMP header included, so the caller can match the sequence number.
//
// Lines that do not start with a marker are skipped rather than treated as an
// error: a board that still has a console on this port will interleave log
// output, and there is no value in failing an upload over it.
func (c *smpConn) recv() ([]byte, error) {
	if c.pkt != nil {
		return c.recvDatagram()
	}

	var frame []byte

	for {
		line, err := c.in.ReadBytes('\n')
		if err != nil {
			if err == io.EOF || len(line) == 0 {
				return nil, fmt.Errorf("timed out waiting for a response")
			}
			return nil, fmt.Errorf("read: %w", err)
		}

		if c.debug {
			fmt.Fprintf(os.Stderr, "RX %q\n", line)
		}

		// Trim the terminator and any CR the far side added.
		for len(line) > 0 && (line[len(line)-1] == '\n' || line[len(line)-1] == '\r') {
			line = line[:len(line)-1]
		}
		if len(line) < 2 {
			continue
		}

		switch {
		case line[0] == nlipPktStart1 && line[1] == nlipPktStart2:
			frame = nil
		case line[0] == nlipDataStart1 && line[1] == nlipDataStart2:
			if frame == nil {
				continue // continuation without a start: not ours
			}
		default:
			continue // console noise
		}

		chunk, err := base64.StdEncoding.DecodeString(string(line[2:]))
		if err != nil {
			return nil, fmt.Errorf("undecodable response line: %w", err)
		}
		frame = append(frame, chunk...)

		if len(frame) < 2 {
			continue
		}
		want := int(binary.BigEndian.Uint16(frame))
		if len(frame)-2 < want {
			continue // more fragments to come
		}

		body := frame[2 : 2+want]
		if crc16ITUT(body) != 0 {
			return nil, fmt.Errorf("response failed its CRC check")
		}
		body = body[:len(body)-2] // drop the trailing CRC

		if len(body) < smpHeaderLen {
			return nil, fmt.Errorf("response is only %d bytes, shorter than an SMP header", len(body))
		}
		return body, nil
	}
}

// body prepends an SMP write header to a CBOR payload, in the reusable buffer.
func (c *smpConn) body(group uint16, id uint8, payload []byte) []byte {
	b := c.bodyBuf[:smpHeaderLen]
	b[0] = smpOpWrite /* nh_version stays 0: SMP v1, which is what boot_serial speaks */
	b[1] = 0
	binary.BigEndian.PutUint16(b[2:], uint16(len(payload)))
	binary.BigEndian.PutUint16(b[4:], group)
	b[6] = c.seq
	b[7] = id
	c.seq++
	return append(b, payload...)
}

// request sends one SMP write request and returns the response payload,
// retrying until `attempts` are spent.
//
// Retrying is not belt and braces, it is required. `arduino-cli upload` starts
// this the moment the recovery port enumerates, which happens at SYS_INIT -
// well before MCUboot reaches boot_console_init() and installs the UART
// callback that fills its line buffers. Anything sent in that window is dropped
// on the floor, and without a retry the upload fails on a board that is
// perfectly healthy and about to start listening. (Observed on hardware
// 2026-08-17: the same image uploaded by hand seconds later worked every time.)
//
// The sequence number is what makes retrying safe: a late reply to an earlier
// attempt is discarded rather than mistaken for the answer to this one.
func (c *smpConn) request(group uint16, id uint8, payload []byte, attempts int) ([]byte, error) {
	var lastErr error

	for attempt := 0; attempt < attempts; attempt++ {
		c.flushInput()

		seq := c.seq
		if err := c.send(c.body(group, id, payload)); err != nil {
			return nil, err
		}

		for {
			rsp, err := c.recv()
			if err != nil {
				lastErr = err
				break // resend
			}
			if rsp[6] != seq {
				continue // a straggler from a previous attempt
			}
			return rsp[smpHeaderLen:], nil
		}
	}

	return nil, lastErr
}

// The four keys this client ever sends, encoded once.
var (
	cborKeyImage = cborText("image")
	cborKeyLen   = cborText("len")
	cborKeyOff   = cborText("off")
	cborKeyData  = cborText("data")
)

// --- just enough CBOR ------------------------------------------------------
//
// Only the shapes this protocol uses: a definite-length map of short text keys
// to unsigned integers, byte strings and booleans.

func cborMapHeader(n int) []byte {
	return cborUint(0xA0, uint64(n))
}

func cborUint(major byte, v uint64) []byte {
	switch {
	case v < 24:
		return []byte{major | byte(v)}
	case v <= 0xFF:
		return []byte{major | 24, byte(v)}
	case v <= 0xFFFF:
		return []byte{major | 25, byte(v >> 8), byte(v)}
	case v <= 0xFFFFFFFF:
		return []byte{major | 26, byte(v >> 24), byte(v >> 16), byte(v >> 8), byte(v)}
	default:
		out := []byte{major | 27}
		return binary.BigEndian.AppendUint64(out, v)
	}
}

func cborText(s string) []byte {
	return append(cborUint(0x60, uint64(len(s))), s...)
}

func cborBytes(b []byte) []byte {
	return append(cborUint(0x40, uint64(len(b))), b...)
}

// cborFindUint returns the value of a top-level unsigned/negative integer key
// in a CBOR map. Returns ok=false if the key is absent.
//
// This walks the encoding rather than parsing it into a structure: the only
// answers wanted are "rc" and "off", both small integers, and everything else
// in a response can be skipped without being understood.
func cborFindInt(data []byte, key string) (int64, bool) {
	if len(data) == 0 || data[0]&0xE0 != 0xA0 {
		return 0, false
	}
	n := int(data[0] & 0x1F)
	p := 1

	for i := 0; i < n; i++ {
		k, next, ok := cborString(data, p)
		if !ok {
			return 0, false
		}
		p = next

		if k == key {
			return cborInt(data, p)
		}
		p, ok = cborSkip(data, p)
		if !ok {
			return 0, false
		}
	}
	return 0, false
}

func cborString(data []byte, p int) (string, int, bool) {
	if p >= len(data) || data[p]&0xE0 != 0x60 {
		return "", 0, false
	}
	l := int(data[p] & 0x1F)
	p++
	if l >= 24 {
		return "", 0, false // no key this protocol uses is that long
	}
	if p+l > len(data) {
		return "", 0, false
	}
	return string(data[p : p+l]), p + l, true
}

// cborInt reads an unsigned (major 0) or negative (major 1) integer.
func cborInt(data []byte, p int) (int64, bool) {
	if p >= len(data) {
		return 0, false
	}
	major := data[p] & 0xE0
	if major != 0x00 && major != 0x20 {
		return 0, false
	}
	v, next, ok := cborArg(data, p)
	if !ok || next > len(data) {
		return 0, false
	}
	if major == 0x20 {
		return -1 - int64(v), true
	}
	return int64(v), true
}

// cborArg decodes the argument of the item at p, returning it and the offset
// just past the head.
func cborArg(data []byte, p int) (uint64, int, bool) {
	if p >= len(data) {
		return 0, 0, false
	}
	ai := data[p] & 0x1F
	p++
	switch {
	case ai < 24:
		return uint64(ai), p, true
	case ai == 24:
		if p >= len(data) {
			return 0, 0, false
		}
		return uint64(data[p]), p + 1, true
	case ai == 25:
		if p+2 > len(data) {
			return 0, 0, false
		}
		return uint64(binary.BigEndian.Uint16(data[p:])), p + 2, true
	case ai == 26:
		if p+4 > len(data) {
			return 0, 0, false
		}
		return uint64(binary.BigEndian.Uint32(data[p:])), p + 4, true
	case ai == 27:
		if p+8 > len(data) {
			return 0, 0, false
		}
		return binary.BigEndian.Uint64(data[p:]), p + 8, true
	}
	return 0, 0, false
}

// cborSkip advances past one complete item.
func cborSkip(data []byte, p int) (int, bool) {
	if p >= len(data) {
		return 0, false
	}
	major := data[p] & 0xE0

	switch major {
	case 0x00, 0x20: // uint, negative
		_, next, ok := cborArg(data, p)
		return next, ok
	case 0x40, 0x60: // bstr, tstr
		l, next, ok := cborArg(data, p)
		if !ok || next+int(l) > len(data) {
			return 0, false
		}
		return next + int(l), true
	case 0x80, 0xA0: // array, map
		l, next, ok := cborArg(data, p)
		if !ok {
			return 0, false
		}
		items := int(l)
		if major == 0xA0 {
			items *= 2
		}
		for i := 0; i < items; i++ {
			next, ok = cborSkip(data, next)
			if !ok {
				return 0, false
			}
		}
		return next, true
	case 0xE0: // simple values: true/false/null, or a float
		ai := data[p] & 0x1F
		if ai < 24 {
			return p + 1, true
		}
		_, next, ok := cborArg(data, p)
		return next, ok
	}
	return 0, false
}

// --- commands --------------------------------------------------------------

// smpCheckRC reports the "rc" of a response as an error, if it carries a
// non-zero one.
func smpCheckRC(rsp []byte) error {
	rc, ok := cborFindInt(rsp, "rc")
	if !ok || rc == 0 {
		return nil
	}
	// The codes that actually happen here; anything else is passed through.
	switch rc {
	case 3:
		return fmt.Errorf("bootloader rejected the request (rc=3, EINVAL) - " +
			"often means the image does not fit the target slot, or the slot number is wrong")
	case 2:
		return fmt.Errorf("bootloader is out of memory (rc=2)")
	case 8:
		return fmt.Errorf("bootloader does not support this command (rc=8) - " +
			"is CONFIG_MCUBOOT_SERIAL_DIRECT_IMAGE_UPLOAD enabled?")
	default:
		return fmt.Errorf("bootloader returned rc=%d", rc)
	}
}

// smpHandshake confirms the board is in serial recovery and listening, before
// an upload commits to pushing hundreds of kilobytes at it.
//
// `image list` is the cheapest thing MCUboot answers: one line out, one line
// back, no state changed. Doing it first turns the two failure modes that look
// identical from the outside - "not in recovery" and "not listening yet" - into
// a clear message and a bounded wait respectively, instead of a stalled
// transfer.
func smpHandshake(c *smpConn) error {
	if _, err := c.request(smpGroupOS, smpIDConsEchoCtrl, nil, smpFirstRetries); err != nil {
		if c.pkt != nil {
			return fmt.Errorf("no answer from mcumgr at %s. The board must be running a "+
				"sketch that has brought the network up, and be reachable on UDP "+
				"port 1337 (%w)", c.pkt.RemoteAddr(), err)
		}
		return fmt.Errorf("the board is not answering mcumgr on this port, so it is not "+
			"in serial recovery mode. Double-tap reset (or let arduino-cli do the "+
			"1200-bps touch) and try again (%w)", err)
	}
	return nil
}

// smpUpload writes an image into the given direct-upload slot.
//
// Slot numbering is the bootloader's, from flash_area_id_from_direct_image():
// 0 and 1 both mean the primary slot, 2 the secondary. It is NOT an MCUboot
// image index.
func smpUpload(c *smpConn, image []byte, slot int, chunkSize int) error {
	off := 0
	started := time.Now()
	lastReport := started

	// Reused every iteration: the keys never change and the buffer is sized
	// once for the largest request this loop can build.
	payload := make([]byte, 0, 64+chunkSize)

	for off < len(image) {
		end := off + chunkSize
		if end > len(image) {
			end = len(image)
		}
		chunk := image[off:end]

		// "image" and "len" are only read from the packet with off == 0;
		// sending them every time would just waste payload budget.
		payload = payload[:0]
		if off == 0 {
			payload = append(payload, cborMapHeader(4)...)
			payload = append(payload, cborKeyImage...)
			payload = append(payload, cborUint(0x00, uint64(slot))...)
			payload = append(payload, cborKeyLen...)
			payload = append(payload, cborUint(0x00, uint64(len(image)))...)
		} else {
			payload = append(payload, cborMapHeader(2)...)
		}
		payload = append(payload, cborKeyOff...)
		payload = append(payload, cborUint(0x00, uint64(off))...)
		payload = append(payload, cborKeyData...)
		payload = append(payload, cborUint(0x40, uint64(len(chunk)))...)
		payload = append(payload, chunk...)

		// At offset 0 the bootloader erases the slot before answering.
		if off == 0 {
			c.setReadTimeout(smpEraseTimeout)
		}

		rsp, err := c.request(smpGroupImage, smpIDUpload, payload, smpRetries)

		if off == 0 {
			c.setReadTimeout(c.readTimeout)
		}
		if err != nil {
			return fmt.Errorf("at offset %d: %w", off, err)
		}
		if err := smpCheckRC(rsp); err != nil {
			return fmt.Errorf("at offset %d: %w", off, err)
		}

		// The bootloader reports how much it has accepted. Trusting it
		// rather than our own count is what resynchronises after it
		// silently discards a chunk it did not want.
		next, ok := cborFindInt(rsp, "off")
		if !ok {
			return fmt.Errorf("at offset %d: response carried no offset", off)
		}
		if int(next) <= off {
			return fmt.Errorf("at offset %d: bootloader did not advance (reported %d)", off, next)
		}
		off = int(next)

		if time.Since(lastReport) > time.Second {
			fmt.Printf("\rUploading: %d/%d bytes (%.0f%%)",
				off, len(image), 100*float64(off)/float64(len(image)))
			lastReport = time.Now()
		}
	}

	elapsed := time.Since(started)
	fmt.Printf("\rUploaded %d bytes in %.1fs (%.1f KB/s)\n",
		len(image), elapsed.Seconds(),
		float64(len(image))/1024/elapsed.Seconds())
	return nil
}

// smpReset asks the bootloader to reboot, which is what starts the image just
// uploaded.
//
// A reset produces no useful reply - the board is gone before it can be read -
// so a timeout here is success, not failure.
func smpReset(c *smpConn) error {
	if err := c.send(c.body(smpGroupOS, smpIDReset, cborMapHeader(0))); err != nil {
		return err
	}
	_, _ = c.recv()
	return nil
}

// runSMPUpload is the entry point used by the upload recipe: open the port,
// push the image, reset.
func runSMPUpload(portName, udpAddr, imagePath string, slot, chunkSize int, reset, markTest bool, timeout time.Duration, debug bool) error {
	image, err := os.ReadFile(imagePath)
	if err != nil {
		return fmt.Errorf("read %s: %w", imagePath, err)
	}
	if len(image) == 0 {
		return fmt.Errorf("%s is empty", imagePath)
	}

	// One of the two transports. `udpAddr` wins when set: SMP over UDP is the
	// OTA path and speaks to the running loader, where -smp-port speaks to
	// MCUboot over the wire. Everything after this point is identical.
	var c *smpConn
	if udpAddr != "" {
		c, err = smpOpenUDP(udpAddr, timeout)
		portName = udpAddr
	} else {
		c, err = smpOpen(portName, timeout)
	}
	if err != nil {
		return err
	}
	defer c.Close()
	c.debug = debug

	fmt.Printf("Uploading %s (%d bytes) to slot %d on %s\n",
		imagePath, len(image), slot, portName)

	if err := smpHandshake(c); err != nil {
		return err
	}

	if err := smpUpload(c, image, slot, chunkSize); err != nil {
		return err
	}

	// An OTA is not finished by the upload: MCUboot only swaps an image whose
	// pending flag is set, and img_mgmt sets that from the state command, not
	// from the upload. Deliberately NOT implied by "slot 2" - the secondary
	// slot also stages bootloader packages and sketch deltas, and neither is
	// an MCUboot image to be swapped.
	if markTest {
		if err := markStagedImage(c, imagePath, false); err != nil {
			return err
		}
	}

	if reset {
		fmt.Printf("Resetting the board\n")
		if err := smpReset(c); err != nil {
			return err
		}
	}
	return nil
}

func runSMPResetOnly(portName string, timeout time.Duration, debug bool) error {
	c, err := smpOpen(portName, timeout)
	if err != nil {
		return err
	}
	defer c.Close()
	c.debug = debug

	if err := smpHandshake(c); err != nil {
		return err
	}

	fmt.Printf("Resetting the board on %s\n", portName)
	return smpReset(c)
}
