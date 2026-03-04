// Copyright (c) Arduino s.r.l. and/or its affiliated companies
// SPDX-License-Identifier: Apache-2.0

package main

import (
	"bufio"
	"debug/elf"
	"fmt"
	"os"
	"strings"
)

func main() {
	if len(os.Args) != 5 {
		fmt.Fprintf(os.Stderr, "Usage: %s <link_mode> <config_path> <upload_file> <elf_file>\n\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "Computes sketch size for Arduino IDE.\n\n")
		fmt.Fprintf(os.Stderr, "Arguments:\n")
		fmt.Fprintf(os.Stderr, "  link_mode    'static' or 'dynamic'\n")
		fmt.Fprintf(os.Stderr, "  config_path  Path to Zephyr .config file\n")
		fmt.Fprintf(os.Stderr, "  upload_file  Path to upload binary (for flash size)\n")
		fmt.Fprintf(os.Stderr, "  elf_file     Path to ELF file (for section analysis)\n")
		os.Exit(1)
	}

	linkMode := os.Args[1]
	configPath := os.Args[2]
	uploadFile := os.Args[3]
	elfFile := os.Args[4]

	// Flash size = filesystem size of the upload file
	flashSize, err := fileSize(uploadFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error reading upload file: %v\n", err)
		os.Exit(1)
	}

	// Heap size = sum of relevant ELF sections
	heapSize, err := computeHeapSize(elfFile, linkMode, configPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error computing heap size: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf(".text %12d %12d\n", flashSize, 0)
	fmt.Printf(".data %12d %12d\n", heapSize, 0)
}

func fileSize(path string) (int64, error) {
	info, err := os.Stat(path)
	if err != nil {
		return 0, err
	}
	return info.Size(), nil
}

func computeHeapSize(elfFile, linkMode, configPath string) (uint64, error) {
	f, err := elf.Open(elfFile)
	if err != nil {
		return 0, err
	}
	defer f.Close()

	noReloc := false
	if linkMode == "dynamic" {
		noReloc = checkConfigFlag(configPath, "CONFIG_LLEXT_RODATA_NO_RELOC")
	}

	var total uint64
	for _, section := range f.Sections {
		name := section.Name

		if linkMode == "static" {
			if name == ".data" || name == ".bss" {
				total += section.Size
			}
		} else {
			// Dynamic mode: all code/data sections go to LLEXT heap.
			// .llext.rodata.noreloc stays in flash when NO_RELOC=y.
			switch {
			case name == ".data" || name == ".bss" || name == ".text":
				total += section.Size
			case strings.HasPrefix(name, ".rodata"):
				total += section.Size
			case noReloc && strings.HasPrefix(name, ".llext.rodata.noreloc"):
				// stays in flash, skip
			case strings.HasPrefix(name, ".llext."):
				total += section.Size
			}
		}
	}

	return total, nil
}

func checkConfigFlag(configPath, key string) bool {
	f, err := os.Open(configPath)
	if err != nil {
		return false
	}
	defer f.Close()

	target := key + "=y"
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		if strings.TrimSpace(scanner.Text()) == target {
			return true
		}
	}
	return false
}
