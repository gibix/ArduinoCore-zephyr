package main

import (
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"os"
)

/*
 * Marking a staged image for swap.
 *
 * Uploading into the secondary slot is not enough on its own: MCUboot only
 * swaps an image whose pending flag is set, and img_mgmt sets that flag from
 * the image STATE command (group 1, id 0), never from the upload. Without this
 * step an OTA uploads happily, resets, and boots exactly what it booted before,
 * which looks like a silent failure and is the first thing to check if one
 * appears to do nothing.
 *
 * "test" rather than "confirm" is the point of doing it this way: MCUboot swaps
 * the image, and unless something marks it good it swaps back on the next boot.
 * loader/main.c does the marking, calling ota_confirm() only once the sketch
 * header in the freshly-swapped image validates - so an image that cannot run
 * is reverted without anyone having to notice.
 *
 * 🔴 Only ever mark a real MCUboot image. The secondary slot is also the
 * staging area for bootloader packages and sketch deltas, and marking one of
 * those pending would have MCUboot try to swap something that is not an image
 * at all.
 */

const (
	smpIDImageState = 0
	imageMagic      = 0x96f3b83d
	imageHeaderLen  = 32
)

// imageHash computes the SHA-256 that img_mgmt identifies an image by: the
// digest over the header and payload, exactly the range MCUboot itself hashes
// and the value that ends up in the image's SHA256 TLV.
func imageHash(image []byte) ([]byte, error) {
	if len(image) < imageHeaderLen {
		return nil, fmt.Errorf("image is %d bytes, too short for a header", len(image))
	}
	if binary.LittleEndian.Uint32(image[:4]) != imageMagic {
		return nil, fmt.Errorf("not an MCUboot image (bad magic) - a bootloader package " +
			"or a sketch delta cannot be marked for swap")
	}

	hdrSize := int(binary.LittleEndian.Uint16(image[8:10]))
	imgSize := int(binary.LittleEndian.Uint32(image[12:16]))
	if hdrSize+imgSize > len(image) {
		return nil, fmt.Errorf("image header claims %d+%d bytes but the file is %d",
			hdrSize, imgSize, len(image))
	}

	sum := sha256.Sum256(image[:hdrSize+imgSize])
	return sum[:], nil
}

// smpMarkPending asks the board to swap this image on the next boot. With
// permanent=false the swap is reverted unless the new image confirms itself.
func smpMarkPending(c *smpConn, hash []byte, permanent bool) error {
	payload := cborMapHeader(2)
	payload = append(payload, cborText("hash")...)
	payload = append(payload, cborBytes(hash)...)
	payload = append(payload, cborText("confirm")...)
	if permanent {
		payload = append(payload, 0xf5) // CBOR true
	} else {
		payload = append(payload, 0xf4) // CBOR false
	}

	rsp, err := c.request(smpGroupImage, smpIDImageState, payload, smpRetries)
	if err != nil {
		return fmt.Errorf("marking the image for swap: %w", err)
	}
	return smpCheckRC(rsp)
}

// markStagedImage is the post-upload half of an OTA.
func markStagedImage(c *smpConn, imagePath string, permanent bool) error {
	image, err := os.ReadFile(imagePath)
	if err != nil {
		return err
	}
	hash, err := imageHash(image)
	if err != nil {
		return err
	}
	if err := smpMarkPending(c, hash, permanent); err != nil {
		return err
	}

	what := "for test (reverts unless the new image confirms itself)"
	if permanent {
		what = "permanently (no rollback)"
	}
	fmt.Printf("Marked %x… %s\n", hash[:8], what)
	return nil
}
