// Copyright (c) Arduino s.r.l. and/or its affiliated companies
// SPDX-License-Identifier: Apache-2.0

package main

import (
	"flag"
	"fmt"
	"os"
	"strconv"
	"strings"
)

func main() {
	loaderPath := flag.String("loader", "", "path to loader binary (e.g. firmwares/zephyr-variant.bin)")
	sketchPath := flag.String("sketch", "", "path to sketch upload file (e.g. sketch.elf-zsk.bin)")
	offsetStr := flag.String("offset", "", "sketch offset within merged binary (e.g. 0xA0000)")
	outputPath := flag.String("output", "", "output .ota file path")

	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Usage: %s -loader <loader.bin> -sketch <sketch.bin> -offset <hex> -output <out.ota>\n\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "Merges loader and sketch binaries into a single OTA update file.\n")
		fmt.Fprintf(os.Stderr, "The loader is placed at offset 0, padded with 0xFF to the given offset,\n")
		fmt.Fprintf(os.Stderr, "then the sketch is appended.\n\n")
		flag.PrintDefaults()
	}

	flag.Parse()

	if *loaderPath == "" || *sketchPath == "" || *offsetStr == "" || *outputPath == "" {
		flag.Usage()
		os.Exit(1)
	}

	offset, err := parseHex(*offsetStr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: invalid offset %q: %v\n", *offsetStr, err)
		os.Exit(1)
	}

	loader, err := os.ReadFile(*loaderPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: cannot read loader %q: %v\n", *loaderPath, err)
		os.Exit(1)
	}

	sketch, err := os.ReadFile(*sketchPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: cannot read sketch %q: %v\n", *sketchPath, err)
		os.Exit(1)
	}

	if int64(len(loader)) > offset {
		fmt.Fprintf(os.Stderr, "Error: loader size (%d bytes) exceeds offset (0x%X = %d bytes)\n",
			len(loader), offset, offset)
		os.Exit(1)
	}

	// Build merged binary: loader + 0xFF padding + sketch
	merged := make([]byte, offset+int64(len(sketch)))

	// Copy loader at offset 0
	copy(merged, loader)

	// Fill gap with 0xFF (erased flash value)
	for i := len(loader); i < int(offset); i++ {
		merged[i] = 0xFF
	}

	// Copy sketch at offset
	copy(merged[offset:], sketch)

	if err := os.WriteFile(*outputPath, merged, 0644); err != nil {
		fmt.Fprintf(os.Stderr, "Error: cannot write output %q: %v\n", *outputPath, err)
		os.Exit(1)
	}

	fmt.Printf("OTA file created: %s (%d bytes)\n", *outputPath, len(merged))
	fmt.Printf("  loader: %d bytes at offset 0x0\n", len(loader))
	fmt.Printf("  sketch: %d bytes at offset 0x%X\n", len(sketch), offset)
}

func parseHex(s string) (int64, error) {
	s = strings.TrimSpace(s)
	if strings.HasPrefix(s, "0x") || strings.HasPrefix(s, "0X") {
		return strconv.ParseInt(s[2:], 16, 64)
	}
	return strconv.ParseInt(s, 0, 64)
}
