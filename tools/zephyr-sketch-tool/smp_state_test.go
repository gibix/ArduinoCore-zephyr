package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"testing"
)

// Build a minimal image with the same header layout mcuboot.go writes.
func fakeImage(hdrSize, imgSize int) []byte {
	img := make([]byte, hdrSize+imgSize+64)
	binary.LittleEndian.PutUint32(img[0:], imageMagic)
	binary.LittleEndian.PutUint16(img[8:], uint16(hdrSize))
	binary.LittleEndian.PutUint32(img[12:], uint32(imgSize))
	for i := hdrSize; i < hdrSize+imgSize; i++ {
		img[i] = byte(i)
	}
	return img
}

// The hash img_mgmt identifies an image by covers header+payload and stops
// before the TLVs - hashing the whole file instead would silently never match
// anything the board has.
func TestImageHashCoversHeaderAndPayloadOnly(t *testing.T) {
	const hdrSize, imgSize = 0x400, 5000
	img := fakeImage(hdrSize, imgSize)

	got, err := imageHash(img)
	if err != nil {
		t.Fatalf("imageHash: %v", err)
	}
	want := sha256.Sum256(img[:hdrSize+imgSize])
	if !bytes.Equal(got, want[:]) {
		t.Fatalf("hash covers the wrong range")
	}
	if h, _ := imageHash(img[:hdrSize+imgSize]); bytes.Equal(got, h) == false {
		t.Fatal("trailing TLVs changed the hash; they must not be covered")
	}
}

// The secondary slot also stages bootloader packages and sketch deltas. Marking
// one of those pending would have MCUboot try to swap something that is not an
// image, so they must be refused before the request is sent.
func TestImageHashRejectsNonImages(t *testing.T) {
	for name, blob := range map[string][]byte{
		"sketch delta":       append([]byte{0x53, 0x4b, 0x55, 0x50}, make([]byte, 64)...),
		"bootloader package": append([]byte{0x42, 0x46, 0x4d, 0x55}, make([]byte, 64)...),
		"too short":          {0x3d, 0xb8},
	} {
		if _, err := imageHash(blob); err == nil {
			t.Errorf("%s: expected a refusal, got none", name)
		}
	}
}

// A header claiming more than the file holds must not read past the end.
func TestImageHashRejectsOversizeClaim(t *testing.T) {
	img := fakeImage(0x400, 100)
	binary.LittleEndian.PutUint32(img[12:], 1<<20)
	if _, err := imageHash(img); err == nil {
		t.Fatal("expected a refusal when the header claims more than the file holds")
	}
}
