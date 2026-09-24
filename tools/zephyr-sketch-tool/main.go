// Copyright (c) Arduino s.r.l. and/or its affiliated companies
// SPDX-License-Identifier: Apache-2.0

package main

import (
	"bytes"
	"encoding/binary"
	"flag"
	"fmt"
	"os"
	"time"
)

func main() {
	var output = flag.String("output", "", "Output to a specific file (default: add -zsk.bin suffix)")
	var debug = flag.Bool("debug", false, "Enable debugging mode")
	var immediate = flag.Bool("immediate", false, "Start sketch immediately [UNO Q]")
	var wait_for_app = flag.Bool("wait_for_app", false, "Wait for the app to start [UNO Q]")
	var linked = flag.Bool("prelinked", false, "Provided file has already been linked to Zephyr")
	var force = flag.Bool("force", false, "Ignore safety checks and overwrite the header")
	var add_header = flag.Bool("add_header", false, "Add space for the header to the file")

	// OTA mode: produce .ota update files from a mangled sketch + loader.
	var ota = flag.Bool("ota", false, "OTA mode: produce .ota update files")
	var otaLoader = flag.String("ota-loader", "", "[ota] loader binary path")
	var otaSketch = flag.String("ota-sketch", "", "[ota] sketch binary path (the mangled -zsk.bin artifact)")
	var otaOffset = flag.String("ota-offset", "", "[ota] sketch offset in merged binary (hex)")
	var otaMagic = flag.String("ota-magic", "", "[ota] board magic number (hex, 32-bit)")

	// SMP mode: push an image to a board sitting in MCUboot serial recovery.
	var smp = flag.Bool("smp", false, "SMP mode: upload an image over mcumgr/SMP")
	var smpPort = flag.String("smp-port", "", "[smp] serial port of the board in recovery mode")
	var smpImage = flag.String("smp-image", "", "[smp] image to upload")
	var smpSlot = flag.Int("smp-slot", 0, "[smp] target slot (0/1 = primary, 2 = secondary, 3 = image 1 primary)")
	var smpChunk = flag.Int("smp-chunk", smpDefaultChunk, "[smp] payload bytes per request")
	var smpDebug = flag.Bool("smp-debug", false, "[smp] dump every protocol line to stderr")
	var smpNoReset = flag.Bool("smp-no-reset", false, "[smp] leave the board in recovery instead of resetting")
	var smpResetOnly = flag.Bool("smp-reset-only", false, "[smp] send only a reset command, no upload")
	var smpTimeout = flag.Int("smp-timeout", 2, "[smp] seconds to wait for each response, per attempt")

	flag.Parse()

	if *smp {
		timeout := time.Duration(*smpTimeout) * time.Second
		if *smpResetOnly {
			if *smpPort == "" {
				fmt.Printf("SMP reset error: -smp-port is required\n")
				os.Exit(1)
			}
			if err := runSMPResetOnly(*smpPort, timeout, *smpDebug); err != nil {
				fmt.Printf("SMP reset error: %v\n", err)
				os.Exit(1)
			}
			return
		}
		if *smpPort == "" || *smpImage == "" {
			fmt.Printf("SMP upload error: -smp-port and -smp-image are required\n")
			os.Exit(1)
		}
		if err := runSMPUpload(*smpPort, "", *smpImage, *smpSlot, *smpChunk, !*smpNoReset, false, timeout, *smpDebug); err != nil {
			fmt.Printf("SMP upload error: %v\n", err)
			os.Exit(1)
		}
		return
	}

	if *ota {
		if err := runOTA(*otaLoader, *otaSketch, *otaOffset, *otaMagic, *otaSketch); err != nil {
			fmt.Printf("OTA error: %v\n", err)
			os.Exit(1)
		}
		return
	}

	if flag.NArg() != 1 {
		fmt.Printf("Usage: %s [flags] <filename>\n", os.Args[0])
		flag.PrintDefaults()
		return
	}
	filename := flag.Arg(0)

	// Read the file content
	content, err := os.ReadFile(filename)
	if err != nil {
		fmt.Printf("Error reading file: %v\n", err)
		return
	}

	var ELF_HEADER = []byte { 0x7f, 0x45, 0x4c, 0x46 }
	var elf_header_found = bytes.Compare(ELF_HEADER, content[0:4]) == 0
	if *add_header || (!*force && !elf_header_found) {
		fmt.Printf("File does not have an ELF header, adding empty space\n")

		var newContent = make([]byte, len(content)+16)
		copy(newContent[16:], content)
		content = newContent
	}

	// Create and fill custom header
	var header struct {
		ver uint8       // @ 0x07
		len uint32      // @ 0x08
		magic uint16    // @ 0x0c
		flags uint8     // @ 0x0e
	}

	header.ver = 1
	header.magic = 0x2341 // Arduino USB VID
	header.len = uint32(len(content))

	header.flags = 0
	if *debug {
		header.flags |= 0x01
	}
	if *linked {
		header.flags |= 0x02
	}
	if *immediate {
		header.flags |= 0x04
	}
	if *wait_for_app {
		header.flags |= 0x08
	}

	var bytes = make([]byte, 9)
	_, err = binary.Encode(bytes, binary.LittleEndian, header)
	if err != nil {
		fmt.Printf("Error encoding header: %v\n", err)
		return
	}

	// Bytes 7 to 15 are free to use in current ELF specification. We will
	// use them to store the debug and linked flags.
	// Check if the target area is empty
	if !*force {
		for i := 7; i < 16; i++ {
			if content[i] != 0 {
				fmt.Printf("Target ELF header area is not empty. Use --force to overwrite\n")
				return
			}
		}
	}

	// Change the header bytes in the content
	copy(content[7:16], bytes)

	// Create a new filename for the copy
	newFilename := *output
	if newFilename == "" {
		newFilename = filename + "-zsk.bin"
	}

	// Write the new content to the new file
	err = os.WriteFile(newFilename, content, 0644)
	if err != nil {
		fmt.Printf("Error writing to file: %v\n", err)
		return
	}

	fmt.Printf("File %s saved as %s\n", filename, newFilename)
}
