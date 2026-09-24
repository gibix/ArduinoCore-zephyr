// Copyright (c) Arduino s.r.l. and/or its affiliated companies
// SPDX-License-Identifier: Apache-2.0

// Tests for the SMP client, checked against how MCUboot's boot/boot_serial
// actually decodes the wire - not against this file's own encoder. A framing
// bug here does not show up as a failed upload, it shows up as a board that no
// longer boots, so the fake bootloader below reimplements boot_serial's decode
// path (boot_serial_in_dec) independently rather than calling back into smp.go.

package main

import (
	"bufio"
	"bytes"
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"testing"
	"time"
)

// TestCRC16ITUT pins the CRC against the standard CRC-16/XMODEM check value,
// which is what Zephyr's crc16_itu_t computes when seeded with 0.
func TestCRC16ITUT(t *testing.T) {
	if got := crc16ITUT([]byte("123456789")); got != 0x31C3 {
		t.Fatalf("crc16ITUT(\"123456789\") = %#04x, want 0x31c3", got)
	}

	// The receiver's actual test: CRC computed over body||crc is zero.
	body := []byte{0x02, 0x00, 0x00, 0x05, 0x00, 0x01, 0x07, 0x01, 0xA0}
	framed := binary.BigEndian.AppendUint16(append([]byte{}, body...), crc16ITUT(body))
	if got := crc16ITUT(framed); got != 0 {
		t.Fatalf("crc over body+crc = %#04x, want 0", got)
	}
}

// TestCborGolden pins the encoder against hand-computed bytes.
func TestCborGolden(t *testing.T) {
	var got []byte
	got = append(got, cborMapHeader(2)...)
	got = append(got, cborText("off")...)
	got = append(got, cborUint(0x00, 0)...)
	got = append(got, cborText("data")...)
	got = append(got, cborBytes([]byte{0xAA, 0xBB})...)

	want := []byte{
		0xA2,                // map(2)
		0x63, 'o', 'f', 'f', // "off"
		0x00,                     // 0
		0x64, 'd', 'a', 't', 'a', // "data"
		0x42, 0xAA, 0xBB, // bytes(2)
	}
	if !bytes.Equal(got, want) {
		t.Fatalf("encoded %x, want %x", got, want)
	}
}

func TestCborFindInt(t *testing.T) {
	// {"rc": 0, "off": 4096} - the shape of an upload response.
	rsp := append(cborMapHeader(2), cborText("rc")...)
	rsp = append(rsp, cborUint(0x00, 0)...)
	rsp = append(rsp, cborText("off")...)
	rsp = append(rsp, cborUint(0x00, 4096)...)

	if v, ok := cborFindInt(rsp, "rc"); !ok || v != 0 {
		t.Fatalf("rc = %d, %v; want 0, true", v, ok)
	}
	if v, ok := cborFindInt(rsp, "off"); !ok || v != 4096 {
		t.Fatalf("off = %d, %v; want 4096, true", v, ok)
	}
	if _, ok := cborFindInt(rsp, "absent"); ok {
		t.Fatal("found a key that is not there")
	}

	// A negative rc, as mcumgr encodes error codes in some responses, and a
	// preceding byte string that has to be skipped over.
	rsp2 := append(cborMapHeader(2), cborText("data")...)
	rsp2 = append(rsp2, cborBytes([]byte{1, 2, 3})...)
	rsp2 = append(rsp2, cborText("rc")...)
	rsp2 = append(rsp2, cborUint(0x20, 2)...) // -3
	if v, ok := cborFindInt(rsp2, "rc"); !ok || v != -3 {
		t.Fatalf("rc = %d, %v; want -3, true", v, ok)
	}
}

// fakeBootloader is a stand-in for MCUboot in serial recovery. Its decoder
// mirrors boot_serial_in_dec()/boot_serial_input(): base64 lines with NLIP
// markers, a big-endian length covering body+CRC, and a CRC that must zero out
// over the pair.
type fakeBootloader struct {
	t *testing.T

	// what it received, in order
	image    []byte
	imageNum int
	declared int
	resets   int

	// injected behaviour
	shortWrite int // if non-zero, accept only this many bytes of each chunk
}

func (f *fakeBootloader) serve(conn net.Conn) {
	defer conn.Close()
	in := bufio.NewReader(conn)

	var frame []byte
	for {
		line, err := in.ReadBytes('\n')
		if err != nil {
			return
		}
		line = bytes.TrimRight(line, "\r\n")
		if len(line) < 2 {
			continue
		}
		if len(line) > 128 {
			f.t.Errorf("line of %d bytes exceeds CONFIG_BOOT_MAX_LINE_INPUT_LEN (128)", len(line))
			return
		}

		switch {
		case line[0] == 0x06 && line[1] == 0x09:
			frame = nil
		case line[0] == 0x04 && line[1] == 0x14:
			if frame == nil {
				f.t.Error("continuation line before a packet start")
				return
			}
		default:
			f.t.Errorf("line with bad marker %#02x %#02x", line[0], line[1])
			return
		}

		chunk, err := base64.StdEncoding.DecodeString(string(line[2:]))
		if err != nil {
			f.t.Errorf("undecodable base64: %v", err)
			return
		}
		frame = append(frame, chunk...)

		if len(frame) <= 2 {
			continue
		}
		want := int(binary.BigEndian.Uint16(frame))
		if want != len(frame)-2 {
			continue // more fragments expected
		}
		if len(frame) > 1024 {
			f.t.Errorf("frame of %d bytes exceeds CONFIG_BOOT_SERIAL_MAX_RECEIVE_SIZE", len(frame))
			return
		}
		if crc16ITUT(frame[2:]) != 0 {
			f.t.Error("received frame failed its CRC check")
			return
		}

		body := frame[2 : len(frame)-2]
		f.handle(conn, body)
		frame = nil
	}
}

func (f *fakeBootloader) handle(conn net.Conn, body []byte) {
	if len(body) < smpHeaderLen {
		f.t.Errorf("body of %d bytes is shorter than an SMP header", len(body))
		return
	}
	op := body[0] & 0x07
	plen := int(binary.BigEndian.Uint16(body[2:]))
	group := binary.BigEndian.Uint16(body[4:])
	seq := body[6]
	id := body[7]
	payload := body[smpHeaderLen:]

	if op != smpOpWrite {
		f.t.Errorf("op = %d, want %d", op, smpOpWrite)
		return
	}
	if plen != len(payload) {
		f.t.Errorf("header says %d payload bytes, got %d", plen, len(payload))
		return
	}

	if group == smpGroupOS && id == smpIDReset {
		f.resets++
		f.reply(conn, seq, group, id, append(cborMapHeader(1),
			append(cborText("rc"), cborUint(0x00, 0)...)...))
		return
	}
	if group != smpGroupImage || id != smpIDUpload {
		f.t.Errorf("unexpected group/id %d/%d", group, id)
		return
	}

	off, ok := cborFindInt(payload, "off")
	if !ok {
		f.t.Error("upload request without an offset")
		return
	}
	if off == 0 {
		f.image = nil
		if n, ok := cborFindInt(payload, "image"); ok {
			f.imageNum = int(n)
		} else {
			f.t.Error("first request carried no image number")
		}
		if n, ok := cborFindInt(payload, "len"); ok {
			f.declared = int(n)
		} else {
			f.t.Error("first request carried no total length")
		}
	} else {
		// boot_serial only reads these from the off==0 packet, so sending
		// them later would be wasted payload.
		if _, ok := cborFindInt(payload, "image"); ok {
			f.t.Errorf("request at offset %d repeated the image number", off)
		}
		if _, ok := cborFindInt(payload, "len"); ok {
			f.t.Errorf("request at offset %d repeated the total length", off)
		}
	}

	if int(off) != len(f.image) {
		f.t.Errorf("request at offset %d, but %d bytes have been received", off, len(f.image))
		return
	}

	data := cborFindBytes(payload, "data")
	if data == nil {
		f.t.Error("upload request without data")
		return
	}
	if f.shortWrite > 0 && len(data) > f.shortWrite {
		data = data[:f.shortWrite]
	}
	f.image = append(f.image, data...)

	rsp := append(cborMapHeader(2), cborText("rc")...)
	rsp = append(rsp, cborUint(0x00, 0)...)
	rsp = append(rsp, cborText("off")...)
	rsp = append(rsp, cborUint(0x00, uint64(len(f.image)))...)
	f.reply(conn, seq, group, id, rsp)
}

// reply frames a response the way boot_serial's bs_rsp_send does.
func (f *fakeBootloader) reply(conn net.Conn, seq uint8, group uint16, id uint8, payload []byte) {
	hdr := make([]byte, smpHeaderLen)
	hdr[0] = smpOpWrite + 1 // boot_serial does nh_op++
	binary.BigEndian.PutUint16(hdr[2:], uint16(len(payload)))
	binary.BigEndian.PutUint16(hdr[4:], group)
	hdr[6] = seq
	hdr[7] = id

	body := append(hdr, payload...)
	frame := binary.BigEndian.AppendUint16(nil, uint16(len(body)+2))
	frame = append(frame, body...)
	frame = binary.BigEndian.AppendUint16(frame, crc16ITUT(body))

	enc := []byte(base64.StdEncoding.EncodeToString(frame))
	for off := 0; off < len(enc); off += 124 {
		end := off + 124
		if end > len(enc) {
			end = len(enc)
		}
		var line []byte
		if off == 0 {
			line = []byte{0x06, 0x09}
		} else {
			line = []byte{0x04, 0x14}
		}
		line = append(line, enc[off:end]...)
		line = append(line, '\n')
		if _, err := conn.Write(line); err != nil {
			return
		}
	}
}

// cborFindBytes is the test's own byte-string lookup, so the fake does not
// depend on smp.go for anything but the CRC.
func cborFindBytes(data []byte, key string) []byte {
	if len(data) == 0 || data[0]&0xE0 != 0xA0 {
		return nil
	}
	n := int(data[0] & 0x1F)
	p := 1
	for i := 0; i < n; i++ {
		k, next, ok := cborString(data, p)
		if !ok {
			return nil
		}
		p = next
		if k == key {
			if p >= len(data) || data[p]&0xE0 != 0x40 {
				return nil
			}
			l, hp, ok := cborArg(data, p)
			if !ok || hp+int(l) > len(data) {
				return nil
			}
			return data[hp : hp+int(l)]
		}
		if p, ok = cborSkip(data, p); !ok {
			return nil
		}
	}
	return nil
}

func runFakeUpload(t *testing.T, image []byte, chunk int, f *fakeBootloader) error {
	t.Helper()

	client, device := net.Pipe()
	_ = client.SetDeadline(time.Now().Add(20 * time.Second))
	_ = device.SetDeadline(time.Now().Add(20 * time.Second))

	go f.serve(device)

	c := newSMPConn(client)
	defer c.Close()
	if err := smpUpload(c, image, 2, chunk); err != nil {
		return err
	}
	return smpReset(c)
}

// TestUploadDeliversWholeImage is the end-to-end check: every byte arrives, in
// order, exactly once.
func TestUploadDeliversWholeImage(t *testing.T) {
	image := make([]byte, 5000)
	for i := range image {
		image[i] = byte(i * 7)
	}

	f := &fakeBootloader{t: t}
	if err := runFakeUpload(t, image, smpDefaultChunk, f); err != nil {
		t.Fatalf("upload: %v", err)
	}

	if !bytes.Equal(f.image, image) {
		t.Fatalf("device received %d bytes, want %d (equal=%v)",
			len(f.image), len(image), bytes.Equal(f.image, image))
	}
	if f.declared != len(image) {
		t.Errorf("declared length %d, want %d", f.declared, len(image))
	}
	if f.imageNum != 2 {
		t.Errorf("image number %d, want 2", f.imageNum)
	}
	if f.resets != 1 {
		t.Errorf("device saw %d resets, want 1 - the upload is only useful if the "+
			"board is told to boot what it just received", f.resets)
	}
}

// TestUploadResyncsOnShortWrite covers the case the client is written for: the
// bootloader accepting less than was sent and reporting the shortfall in "off".
func TestUploadResyncsOnShortWrite(t *testing.T) {
	image := make([]byte, 3000)
	for i := range image {
		image[i] = byte(i)
	}

	f := &fakeBootloader{t: t, shortWrite: 100}
	if err := runFakeUpload(t, image, smpDefaultChunk, f); err != nil {
		t.Fatalf("upload: %v", err)
	}
	if !bytes.Equal(f.image, image) {
		t.Fatalf("device received %d bytes, want %d", len(f.image), len(image))
	}
}

// TestSendRejectsOversizedFrame makes sure a chunk that would overflow the
// bootloader's receive buffer fails here rather than there.
func TestSendRejectsOversizedFrame(t *testing.T) {
	c := newSMPConn(struct {
		io.Reader
		io.Writer
	}{bytes.NewReader(nil), io.Discard})

	err := c.send(make([]byte, smpMaxFrameLen))
	if err == nil {
		t.Fatal("oversized frame was accepted")
	}
	if want := fmt.Sprintf("%d-byte receive buffer", smpMaxFrameLen); !bytes.Contains([]byte(err.Error()), []byte(want)) {
		t.Fatalf("unhelpful error: %v", err)
	}
}

// worstCaseFrameLen is the largest frame smpDefaultChunk can produce: the
// offset-0 request, which carries the two extra keys, with every value at its
// widest encoding.
func worstCaseFrameLen() int {
	payload := append(cborMapHeader(4), cborKeyImage...)
	payload = append(payload, cborUint(0x00, 255)...)
	payload = append(payload, cborKeyLen...)
	payload = append(payload, cborUint(0x00, 0xFFFFFFFF)...)
	payload = append(payload, cborKeyOff...)
	payload = append(payload, cborUint(0x00, 0xFFFFFFFF)...)
	payload = append(payload, cborKeyData...)
	payload = append(payload, cborBytes(make([]byte, smpDefaultChunk))...)

	return 2 + smpHeaderLen + len(payload) + 2
}

// TestDefaultChunkFits guards the chunk size against both limits a frame has to
// respect. The line count is the one that matters: a frame taking more lines
// than the bootloader has spare receive buffers is dropped mid-frame and the
// board goes silent. See the comment on smpStockLineBufs.
func TestDefaultChunkFits(t *testing.T) {
	frame := worstCaseFrameLen()

	if frame > smpMaxFrameLen {
		t.Errorf("worst-case frame is %d bytes, over the %d-byte receive buffer; "+
			"lower smpDefaultChunk", frame, smpMaxFrameLen)
	}

	// base64 expands 3 bytes to 4, rounded up to a 4-char group.
	encoded := ((frame + 2) / 3) * 4
	lines := (encoded + smpFrameMTU - 1) / smpFrameMTU

	// Not smpStockLineBufs: the consumer permanently holds one buffer, see
	// the comment on smpUsableLines.
	if lines > smpUsableLines {
		t.Errorf("worst-case frame takes %d lines but only %d of a stock "+
			"bootloader's %d receive buffers are ever free; lower smpDefaultChunk",
			lines, smpUsableLines, smpStockLineBufs)
	}
}
