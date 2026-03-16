// Copyright (c) Arduino s.r.l. and/or its affiliated companies
// SPDX-License-Identifier: Apache-2.0

// ota-merge: Create OTA update files for Arduino boards
//
// Usage:
//
//	ota-merge [flags]           Create an OTA update file
package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"hash/crc32"
	"os"
	"strconv"
	"strings"
)

// LZSS parameters matching the Arduino OTA decoder (Arduino_Portenta_OTA)
const (
	lzssEI = 11                    // match position bits
	lzssEJ = 4                     // match length bits
	lzssN  = 1 << lzssEI           // ring buffer size = 2048
	lzssF  = (1 << lzssEJ) + 1     // max match length = 17
	lzssMask = lzssN - 1           // ring buffer index mask
)

// OTA header constants
const (
	otaHeaderSize = 20
)

func main() {
	doMerge()
}

func doMerge() {
	sfuPath := flag.String("sfu", "", "SFU binary to prepend before loader (for C33)")
	sfuSizeStr := flag.String("sfu-size", "0x20000", "SFU partition size in hex (default 128KB)")
	loaderPath := flag.String("loader", "", "loader binary path")
	sketchPath := flag.String("sketch", "", "sketch binary path")
	offsetStr := flag.String("offset", "", "sketch offset in merged binary (hex)")
	outputPath := flag.String("output", "", "output .ota file path")
	magicStr := flag.String("magic", "0x00000000", "board magic number (hex)")
	noCompress := flag.Bool("no-compress", false, "disable LZSS compression")
	sketchOnly := flag.Bool("sketch-only", false, "sketch-only mode: wrap raw sketch in OTA header (no loader merge)")

	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Usage: %s [--sketch-only] -sketch <file> -output <file> [options]\n\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "Creates an OTA update file with header and LZSS compression.\n\n")
		fmt.Fprintf(os.Stderr, "In sketch-only mode, the payload is the raw sketch file.\n")
		fmt.Fprintf(os.Stderr, "In merge mode, the payload is loader + sketch merged at offset.\n\n")
		flag.PrintDefaults()
	}

	flag.Parse()

	magic, err := parseHex(*magicStr)
	fatal(err, "parse magic")

	var payload_input []byte

	if *sketchOnly {
		// Sketch-only mode: reject merge-only flags
		if *loaderPath != "" || *offsetStr != "" || *sfuPath != "" {
			fatalf("--sketch-only: -loader, -offset, and -sfu are not allowed")
		}
		if *sketchPath == "" || *outputPath == "" {
			flag.Usage()
			os.Exit(1)
		}

		sketch, err := os.ReadFile(*sketchPath)
		fatal(err, "read sketch")
		payload_input = sketch

		fmt.Printf("Sketch-only mode: %d bytes\n", len(sketch))
	} else {
		// Merge mode: require loader, sketch, offset, output
		if *loaderPath == "" || *sketchPath == "" || *offsetStr == "" || *outputPath == "" {
			flag.Usage()
			os.Exit(1)
		}

		offset, err := parseHex(*offsetStr)
		fatal(err, "parse offset")

		loader, err := os.ReadFile(*loaderPath)
		fatal(err, "read loader")

		sketch, err := os.ReadFile(*sketchPath)
		fatal(err, "read sketch")

		// If SFU is specified, prepend it before the loader with 0xFF padding
		if *sfuPath != "" {
			sfuSize, err := parseHex(*sfuSizeStr)
			fatal(err, "parse sfu-size")

			sfu, err := os.ReadFile(*sfuPath)
			fatal(err, "read sfu")

			if int64(len(sfu)) > sfuSize {
				fatalf("SFU binary size (%d bytes) exceeds SFU partition size (0x%X)", len(sfu), sfuSize)
			}

			combined := make([]byte, sfuSize+int64(len(loader)))
			copy(combined, sfu)
			for i := len(sfu); i < int(sfuSize); i++ {
				combined[i] = 0xFF
			}
			copy(combined[sfuSize:], loader)

			fmt.Printf("SFU: %d bytes (padded to 0x%X), loader: %d bytes\n", len(sfu), sfuSize, len(loader))
			loader = combined
		}

		if int64(len(loader)) > offset {
			fatalf("loader size (%d bytes) exceeds offset (0x%X)", len(loader), offset)
		}

		// Merge: loader + 0xFF padding + sketch
		merged := make([]byte, offset+int64(len(sketch)))
		copy(merged, loader)
		for i := len(loader); i < int(offset); i++ {
			merged[i] = 0xFF
		}
		copy(merged[offset:], sketch)
		payload_input = merged

		fmt.Printf("  merged binary: %d bytes (loader %d + sketch %d)\n", len(merged), len(loader), len(sketch))
	}

	// Compress
	var payload []byte
	compressed := !*noCompress
	if compressed {
		payload = lzssEncode(payload_input)
		ratio := 100.0 * float64(len(payload)) / float64(len(payload_input))
		fmt.Printf("LZSS: %d -> %d bytes (%.1f%%)\n", len(payload_input), len(payload), ratio)
	} else {
		payload = payload_input
	}

	// Build 8-byte header version field
	var version [8]byte
	version[0] = 1 // header version 1
	if compressed {
		version[0] |= 0x40 // bit 6: compression
	}

	// Magic as little-endian bytes
	var magicBytes [4]byte
	binary.LittleEndian.PutUint32(magicBytes[:], uint32(magic))

	// Assemble file body (everything after len+crc32 fields)
	// CRC covers: magic(4) + version(8) + payload
	var body []byte
	body = append(body, magicBytes[:]...)
	body = append(body, version[:]...)
	body = append(body, payload...)

	// CRC-32 (IEEE, same as Arduino OTA format)
	crcVal := crc32.ChecksumIEEE(body)

	// Build complete file: len(4) + crc32(4) + body
	var out []byte
	var lenBytes [4]byte
	binary.LittleEndian.PutUint32(lenBytes[:], uint32(len(body)))
	var crcBytes [4]byte
	binary.LittleEndian.PutUint32(crcBytes[:], crcVal)

	out = append(out, lenBytes[:]...)
	out = append(out, crcBytes[:]...)
	out = append(out, body...)

	err = os.WriteFile(*outputPath, out, 0644)
	fatal(err, "write output")

	fmt.Printf("OTA file: %s (%d bytes)\n", *outputPath, len(out))
	fmt.Printf("  input: %d bytes\n", len(payload_input))
	fmt.Printf("  payload: %d bytes\n", len(payload))
	fmt.Printf("  magic: 0x%08X\n", magic)
}

// --- LZSS Encoder ---

type bitWriter struct {
	data  []byte
	accum uint32
	nbits int
}

func (w *bitWriter) write(value, bits int) {
	w.accum = (w.accum << uint(bits)) | uint32(value&((1<<uint(bits))-1))
	w.nbits += bits
	for w.nbits >= 8 {
		w.nbits -= 8
		w.data = append(w.data, byte(w.accum>>uint(w.nbits)))
		w.accum &= (1 << uint(w.nbits)) - 1
	}
}

func (w *bitWriter) flush() {
	if w.nbits > 0 {
		w.data = append(w.data, byte(w.accum<<uint(8-w.nbits)))
		w.nbits = 0
		w.accum = 0
	}
}

func lzssEncode(input []byte) []byte {
	var ring [lzssN]byte
	// Initialize with spaces, matching the decoder
	for i := range ring {
		ring[i] = ' '
	}

	w := &bitWriter{}
	r := lzssN - lzssF // write position = 2031
	pos := 0

	for pos < len(input) {
		bestLen := 1 // minimum useful match is 2
		bestPos := 0

		maxLen := lzssF
		if rem := len(input) - pos; rem < maxLen {
			maxLen = rem
		}

		// Search ring buffer for longest match
		for i := 0; i < lzssN; i++ {
			// Quick first-byte check
			if ring[i] != input[pos] {
				continue
			}
			// Compute safe match distance: match must not extend past the
			// write cursor r, otherwise the decoder would read bytes that
			// it has already overwritten during this match operation.
			safeDist := (r - i) & lzssMask
			if safeDist == 0 {
				safeDist = lzssN // i == r: full buffer is safe
			}
			safeLen := maxLen
			if safeDist < safeLen {
				safeLen = safeDist
			}
			matchLen := 1
			for matchLen < safeLen && ring[(i+matchLen)&lzssMask] == input[pos+matchLen] {
				matchLen++
			}
			if matchLen > bestLen {
				bestLen = matchLen
				bestPos = i
			}
		}

		if bestLen >= 2 {
			// Match: 0-bit + EI-bit position + EJ-bit (length-2)
			w.write(0, 1)
			w.write(bestPos, lzssEI)
			w.write(bestLen-2, lzssEJ)
		} else {
			bestLen = 1
			// Literal: 1-bit + 8-bit byte
			w.write(1, 1)
			w.write(int(input[pos]), 8)
		}

		// Update ring buffer
		for k := 0; k < bestLen; k++ {
			ring[r] = input[pos+k]
			r = (r + 1) & lzssMask
		}
		pos += bestLen
	}

	w.flush()
	return w.data
}

// --- Helpers ---

func parseHex(s string) (int64, error) {
	s = strings.TrimSpace(s)
	if strings.HasPrefix(s, "0x") || strings.HasPrefix(s, "0X") {
		return strconv.ParseInt(s[2:], 16, 64)
	}
	return strconv.ParseInt(s, 0, 64)
}

func fatal(err error, context string) {
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: %s: %v\n", context, err)
		os.Exit(1)
	}
}

func fatalf(format string, args ...interface{}) {
	fmt.Fprintf(os.Stderr, "Error: "+format+"\n", args...)
	os.Exit(1)
}
