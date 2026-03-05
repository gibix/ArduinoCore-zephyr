// Copyright (c) Arduino s.r.l. and/or its affiliated companies
// SPDX-License-Identifier: Apache-2.0

package main

import (
	"bufio"
	"debug/elf"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
)

type sectionInfo struct {
	address    uint64
	size       uint64
	region     string
	writable   bool
	executable bool
}

type partitionInfo struct {
	address uint64
	size    uint64
}

func main() {
	if len(os.Args) != 5 {
		fmt.Fprintf(os.Stderr, "Usage: %s <link_mode> <config_path> <upload_file> <elf_file>\n\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "Generate memory diagram for ArduinoCore-zephyr variant.\n\n")
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
	uploadSize, err := fileSize(uploadFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error reading upload file: %v\n", err)
		os.Exit(1)
	}

	// Config values from Zephyr .config
	config := parseConfFile(configPath)

	// NO_RELOC flag (only relevant in dynamic mode)
	noReloc := linkMode == "dynamic" && checkConfigFlag(configPath, "CONFIG_LLEXT_RODATA_NO_RELOC")

	// Parse ELF sections
	elfSections, totalFlash, totalRAM, elfFileSize, err := parseElfFile(elfFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error parsing ELF file: %v\n", err)
		os.Exit(1)
	}

	// Compute LLEXT heap usage based on link mode
	heapUsage, err := computeHeapUsage(elfFile, linkMode, noReloc)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error computing heap usage: %v\n", err)
		os.Exit(1)
	}

	// Infer variant from config path (zephyr-<variant>.config) or ELF path
	variantName := inferVariantFromPath(configPath)
	if variantName == "" || !strings.Contains(variantName, "_") {
		variantName = inferVariantFromPath(elfFile)
	}
	repoRoot := findRepoRoot()

	variantDir := filepath.Join(repoRoot, "variants", variantName)
	overlayPath := filepath.Join(variantDir, variantName+".overlay")
	partitions := parseOverlayFile(overlayPath)

	printMemoryDiagram(variantName, linkMode, config, partitions, elfSections,
		totalFlash, totalRAM, elfFileSize, uploadSize, heapUsage, noReloc)
}

func fileSize(path string) (int64, error) {
	info, err := os.Stat(path)
	if err != nil {
		return 0, err
	}
	return info.Size(), nil
}

func findRepoRoot() string {
	if exe, err := os.Executable(); err == nil {
		if root := walkUpForRoot(filepath.Dir(exe)); root != "" {
			return root
		}
	}
	if cwd, err := os.Getwd(); err == nil {
		if root := walkUpForRoot(cwd); root != "" {
			return root
		}
	}
	if exe, err := os.Executable(); err == nil {
		return filepath.Dir(filepath.Dir(exe))
	}
	return "."
}

func walkUpForRoot(dir string) string {
	dir = filepath.Clean(dir)
	for {
		if _, err := os.Stat(filepath.Join(dir, "loader", "prj.conf")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return ""
		}
		dir = parent
	}
}

func parseConfFile(path string) map[string]interface{} {
	config := make(map[string]interface{})
	f, err := os.Open(path)
	if err != nil {
		return config
	}
	defer f.Close()

	notSetRe := regexp.MustCompile(`^#\s*(CONFIG_\w+) is not set`)
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}
		// Handle Kconfig "# CONFIG_X is not set" as false
		if strings.HasPrefix(line, "#") {
			if m := notSetRe.FindStringSubmatch(line); m != nil {
				config[m[1]] = false
			}
			continue
		}
		idx := strings.Index(line, "=")
		if idx < 0 {
			continue
		}
		key := strings.TrimSpace(line[:idx])
		value := strings.TrimSpace(line[idx+1:])
		value = strings.Trim(value, "\"")

		switch value {
		case "y":
			config[key] = true
		case "n":
			config[key] = false
		default:
			if n, err := strconv.Atoi(value); err == nil {
				config[key] = n
			} else {
				config[key] = value
			}
		}
	}
	return config
}

func configInt(config map[string]interface{}, key string, def int) int {
	if v, ok := config[key]; ok {
		if n, ok := v.(int); ok {
			return n
		}
	}
	return def
}

func configBool(config map[string]interface{}, key string, def bool) bool {
	if v, ok := config[key]; ok {
		if b, ok := v.(bool); ok {
			return b
		}
	}
	return def
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

func computeHeapUsage(elfFile, linkMode string, noReloc bool) (uint64, error) {
	f, err := elf.Open(elfFile)
	if err != nil {
		return 0, err
	}
	defer f.Close()

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

var (
	partitionRe = regexp.MustCompile(`(\w+):\s*partition@([0-9a-fA-F]+)\s*\{[^}]*reg\s*=\s*<\s*(0x[0-9a-fA-F]+|[0-9]+)\s+([^>]+)\s*>`)
	dtSizeKRe   = regexp.MustCompile(`DT_SIZE_K\s*\(\s*(\d+)\s*\)`)
)

func parseUint(s string) uint64 {
	s = strings.TrimSpace(s)
	if strings.HasPrefix(s, "0x") || strings.HasPrefix(s, "0X") {
		n, _ := strconv.ParseUint(s[2:], 16, 64)
		return n
	}
	n, _ := strconv.ParseUint(s, 10, 64)
	return n
}

func parseOverlayFile(path string) map[string]partitionInfo {
	partitions := make(map[string]partitionInfo)
	data, err := os.ReadFile(path)
	if err != nil {
		return partitions
	}
	for _, match := range partitionRe.FindAllStringSubmatch(string(data), -1) {
		name := match[1]
		addr := parseUint(match[3])
		sizeStr := strings.TrimSpace(match[4])

		var size uint64
		if strings.Contains(sizeStr, "DT_SIZE_K") {
			if m := dtSizeKRe.FindStringSubmatch(sizeStr); m != nil {
				n, _ := strconv.ParseUint(m[1], 10, 64)
				size = n * 1024
			}
		} else {
			size = parseUint(sizeStr)
		}
		partitions[name] = partitionInfo{address: addr, size: size}
	}
	return partitions
}

func parseElfFile(path string) (map[string]sectionInfo, uint64, uint64, int64, error) {
	sections := make(map[string]sectionInfo)
	var totalFlash, totalRAM uint64

	stat, err := os.Stat(path)
	if err != nil {
		return nil, 0, 0, 0, err
	}
	elfFileSize := stat.Size()

	f, err := elf.Open(path)
	if err != nil {
		return nil, 0, 0, 0, err
	}
	defer f.Close()

	for _, sec := range f.Sections {
		if sec.Size == 0 || sec.Flags&elf.SHF_ALLOC == 0 {
			continue
		}

		isWritable := sec.Flags&elf.SHF_WRITE != 0
		isExecutable := sec.Flags&elf.SHF_EXECINSTR != 0

		var region string
		switch sec.Name {
		case ".text", ".rodata", ".ARM.exidx", ".ARM.extab":
			region = "flash"
			totalFlash += sec.Size
		case ".data", ".bss", ".noinit":
			region = "ram"
			totalRAM += sec.Size
		default:
			if isExecutable || (!isWritable && sec.Type != elf.SHT_NOBITS) {
				region = "flash"
				totalFlash += sec.Size
			} else {
				region = "ram"
				totalRAM += sec.Size
			}
		}

		sections[sec.Name] = sectionInfo{
			address:    sec.Addr,
			size:       sec.Size,
			region:     region,
			writable:   isWritable,
			executable: isExecutable,
		}
	}

	return sections, totalFlash, totalRAM, elfFileSize, nil
}

func formatSize(size uint64) string {
	if size >= 1024*1024 {
		return fmt.Sprintf("%.1f MB", float64(size)/(1024*1024))
	}
	if size >= 1024 {
		return fmt.Sprintf("%.1f KB", float64(size)/1024)
	}
	return fmt.Sprintf("%d B", size)
}

func center(s string, width int) string {
	if len(s) >= width {
		return s
	}
	pad := width - len(s)
	left := pad / 2
	right := pad - left
	return strings.Repeat(" ", left) + s + strings.Repeat(" ", right)
}

func drawMemoryBar(used, total uint64, width int) string {
	if total == 0 {
		return "[" + strings.Repeat("?", width-2) + "]"
	}
	ratio := float64(used) / float64(total)
	if ratio > 1.0 {
		ratio = 1.0
	}
	filled := int(ratio * float64(width-2))
	empty := width - 2 - filled
	return "[" + strings.Repeat("#", filled) + strings.Repeat(".", empty) + "]"
}

func printMemoryDiagram(variantName, linkMode string, config map[string]interface{},
	partitions map[string]partitionInfo, elfSections map[string]sectionInfo,
	totalFlash, totalRAM uint64, elfFileSize, uploadSize int64,
	heapUsage uint64, noReloc bool) {

	width := 78

	heapSize := configInt(config, "CONFIG_HEAP_MEM_POOL_SIZE", 32768)
	stackSize := configInt(config, "CONFIG_MAIN_STACK_SIZE", 32768)
	llextHeapSize := configInt(config, "CONFIG_LLEXT_HEAP_SIZE", 32) * 1024
	storageWritable := configBool(config, "CONFIG_LLEXT_STORAGE_WRITABLE", true)

	sketchPartition := partitions["user_sketch"]
	sketchAddr := sketchPartition.address
	sketchSize := sketchPartition.size

	fmt.Println()
	fmt.Println(strings.Repeat("=", width))
	fmt.Println(center(fmt.Sprintf("  Memory Diagram for: %s", variantName), width))
	fmt.Println(strings.Repeat("=", width))
	fmt.Println()

	// Configuration summary
	fmt.Println("CONFIGURATION SUMMARY")
	fmt.Println(strings.Repeat("-", width))
	fmt.Printf("  Link Mode:             %10s\n", linkMode)
	fmt.Printf("  Heap Pool Size:        %10s\n", formatSize(uint64(heapSize)))
	fmt.Printf("  Main Stack Size:       %10s\n", formatSize(uint64(stackSize)))
	fmt.Printf("  LLEXT Heap Size:       %10s\n", formatSize(uint64(llextHeapSize)))
	if storageWritable {
		fmt.Println("  Storage Mode:          RAM Copy (writable)")
	} else {
		fmt.Println("  Storage Mode:          Direct Flash (persistent)")
	}
	if noReloc {
		fmt.Println("  RODATA NO_RELOC:       yes (.rodata without relocs stays in flash)")
	}
	if sketchSize > 0 {
		fmt.Printf("  Sketch Partition:      %10s @ 0x%06X\n", formatSize(sketchSize), sketchAddr)
	}
	fmt.Println()

	// ELF summary
	fmt.Println("ELF FILE ANALYSIS")
	fmt.Println(strings.Repeat("-", width))
	fmt.Printf("  ELF File Size:         %10s\n", formatSize(uint64(elfFileSize)))
	fmt.Printf("  Upload File Size:      %10s\n", formatSize(uint64(uploadSize)))
	fmt.Printf("  Flash Sections:        %10s (.text, .rodata, etc.)\n", formatSize(totalFlash))
	fmt.Printf("  RAM Sections:          %10s (.data, .bss, etc.)\n", formatSize(totalRAM))
	fmt.Printf("  LLEXT Heap Usage:      %10s (%s mode)\n", formatSize(heapUsage), linkMode)
	fmt.Println()

	// Section breakdown
	if len(elfSections) > 0 {
		type namedSection struct {
			name string
			info sectionInfo
		}

		fmt.Println("  Sections:")
		var flashSections, ramSections []namedSection
		for name, sec := range elfSections {
			ns := namedSection{name, sec}
			if sec.region == "flash" {
				flashSections = append(flashSections, ns)
			} else {
				ramSections = append(ramSections, ns)
			}
		}

		if len(flashSections) > 0 {
			sort.Slice(flashSections, func(i, j int) bool {
				return flashSections[i].info.size > flashSections[j].info.size
			})
			fmt.Println("    Flash:")
			for _, ns := range flashSections {
				fmt.Printf("      %-20s %10s @ 0x%08X\n", ns.name, formatSize(ns.info.size), ns.info.address)
			}
		}

		if len(ramSections) > 0 {
			sort.Slice(ramSections, func(i, j int) bool {
				return ramSections[i].info.size > ramSections[j].info.size
			})
			fmt.Println("    RAM:")
			for _, ns := range ramSections {
				fmt.Printf("      %-20s %10s @ 0x%08X\n", ns.name, formatSize(ns.info.size), ns.info.address)
			}
		}
	}
	fmt.Println()

	// ASCII Memory Diagram
	fmt.Println("MEMORY LAYOUT")
	fmt.Println(strings.Repeat("-", width))
	fmt.Println()

	// Flash Memory Diagram
	fmt.Println("  FLASH MEMORY")
	fmt.Printf("  +%s+\n", strings.Repeat("-", 70))
	fmt.Printf("  |%-70s|\n", " Loader Firmware")
	fmt.Printf("  +%s+\n", strings.Repeat("-", 70))
	fmt.Printf("  |%-70s|\n", " user_sketch partition")
	if sketchSize > 0 {
		fmt.Printf("  |%-70s|\n", fmt.Sprintf("   Address: 0x%06X  Size: %s", sketchAddr, formatSize(sketchSize)))
	}
	fmt.Printf("  |+%s+  |\n", strings.Repeat("-", 66))
	fmt.Printf("  ||%-66s|  |\n", " Header (16B) | Sketch ELF Data")
	if sketchSize > 0 && uploadSize > 0 {
		bar := drawMemoryBar(uint64(uploadSize), sketchSize, 50)
		pct := float64(uploadSize) / float64(sketchSize) * 100
		fmt.Printf("  ||%-66s|  |\n", fmt.Sprintf("   %s %5.1f%%", bar, pct))
	}
	fmt.Printf("  |+%s+  |\n", strings.Repeat("-", 66))
	fmt.Printf("  +%s+\n", strings.Repeat("-", 70))
	fmt.Println()

	// RAM Memory Diagram
	fmt.Println("  RAM MEMORY")
	fmt.Printf("  +%s+\n", strings.Repeat("-", 70))

	// Heap Pool
	fmt.Printf("  |%-70s|\n", " CONFIG_HEAP_MEM_POOL_SIZE")
	fmt.Printf("  |%-70s|\n", fmt.Sprintf("   Size: %s", formatSize(uint64(heapSize))))
	fmt.Printf("  |+%s+  |\n", strings.Repeat("-", 66))
	fmt.Printf("  ||%-66s|  |\n", " Main Heap Pool (k_malloc, malloc, etc.)")
	fmt.Printf("  ||%-66s|  |\n", " Used by loader for:")
	if storageWritable {
		elfBufferSize := ((uploadSize + 4095) / 4096) * 4096
		fmt.Printf("  ||%-66s|  |\n", fmt.Sprintf("   - Sketch Buffer: %s (4KB aligned)", formatSize(uint64(elfBufferSize))))
	} else {
		fmt.Printf("  ||%-66s|  |\n", "   - (no sketch buffer in persistent mode)")
	}
	fmt.Printf("  |+%s+  |\n", strings.Repeat("-", 66))
	fmt.Printf("  +%s+\n", strings.Repeat("-", 70))

	// LLEXT Heap
	fmt.Printf("  |%-70s|\n", " CONFIG_LLEXT_HEAP_SIZE")
	fmt.Printf("  |%-70s|\n", fmt.Sprintf("   Size: %s", formatSize(uint64(llextHeapSize))))
	fmt.Printf("  |+%s+  |\n", strings.Repeat("-", 66))
	fmt.Printf("  ||%-66s|  |\n", " LLEXT Internal Heap")
	if linkMode == "static" {
		fmt.Printf("  ||%-66s|  |\n", "   - Sketch .data and .bss sections")
		fmt.Printf("  ||%-66s|  |\n", "   - .text and .rodata remain in flash (static link)")
		fmt.Printf("  ||%-66s|  |\n", "   - Symbol table, metadata")
	} else {
		fmt.Printf("  ||%-66s|  |\n", "   - Sketch .text, .data, .bss sections")
		if noReloc {
			fmt.Printf("  ||%-66s|  |\n", "   - Sketch .rodata (with relocations only)")
			fmt.Printf("  ||%-66s|  |\n", "   - .rodata without relocs stays in flash (NO_RELOC=y)")
		} else {
			fmt.Printf("  ||%-66s|  |\n", "   - Sketch .rodata (all, including .llext.rodata.noreloc)")
		}
		fmt.Printf("  ||%-66s|  |\n", "   - Symbol table, metadata, relocations")
	}
	heapBar := drawMemoryBar(heapUsage, uint64(llextHeapSize), 50)
	heapPct := float64(0)
	if llextHeapSize > 0 {
		heapPct = float64(heapUsage) / float64(llextHeapSize) * 100
	}
	fmt.Printf("  ||%-66s|  |\n", fmt.Sprintf("   %s %5.1f%%", heapBar, heapPct))
	fmt.Printf("  |+%s+  |\n", strings.Repeat("-", 66))
	fmt.Printf("  +%s+\n", strings.Repeat("-", 70))

	// Main Stack
	fmt.Printf("  |%-70s|\n", " CONFIG_MAIN_STACK_SIZE")
	fmt.Printf("  |%-70s|\n", fmt.Sprintf("   Size: %s", formatSize(uint64(stackSize))))
	fmt.Printf("  |+%s+  |\n", strings.Repeat("-", 66))
	fmt.Printf("  ||%-66s|  |\n", " LLEXT Thread Stack")
	fmt.Printf("  ||%-66s|  |\n", "   - Sketch execution context")
	fmt.Printf("  ||%-66s|  |\n", "   - Local variables & function calls")
	fmt.Printf("  |+%s+  |\n", strings.Repeat("-", 66))
	fmt.Printf("  +%s+\n", strings.Repeat("-", 70))

	// Sketch RAM
	fmt.Printf("  |%-70s|\n", " Sketch RAM (remaining memory)")
	fmt.Printf("  |+%s+  |\n", strings.Repeat("-", 66))
	fmt.Printf("  ||%-66s|  |\n", " Dynamic allocations from sketch")
	fmt.Printf("  ||%-66s|  |\n", "   - Sketch .data and .bss sections")
	fmt.Printf("  ||%-66s|  |\n", "   - Runtime heap allocations (malloc/free)")
	fmt.Printf("  |+%s+  |\n", strings.Repeat("-", 66))
	fmt.Printf("  +%s+\n", strings.Repeat("-", 70))
	fmt.Println()

	// Memory usage summary
	fmt.Println("MEMORY USAGE SUMMARY")
	fmt.Println(strings.Repeat("-", width))

	if sketchSize > 0 {
		flashPct := float64(uploadSize) / float64(sketchSize) * 100
		fmt.Printf("  Flash Partition:    %s\n", drawMemoryBar(uint64(uploadSize), sketchSize, 40))
		fmt.Printf("                      %10s / %-10s (%.1f%%)\n", formatSize(uint64(uploadSize)), formatSize(sketchSize), flashPct)
	} else {
		fmt.Println("  Flash Partition:    (partition size unknown)")
		fmt.Printf("                      Upload size: %s\n", formatSize(uint64(uploadSize)))
	}

	fmt.Println()
	llextPct := float64(0)
	if llextHeapSize > 0 {
		llextPct = float64(heapUsage) / float64(llextHeapSize) * 100
	}
	fmt.Printf("  LLEXT Heap:         %s\n", drawMemoryBar(heapUsage, uint64(llextHeapSize), 40))
	fmt.Printf("                      %10s / %-10s (%.1f%%)\n", formatSize(heapUsage), formatSize(uint64(llextHeapSize)), llextPct)

	fmt.Println()
	if storageWritable {
		elfBufferSize := ((uploadSize + 4095) / 4096) * 4096
		heapRemaining := int64(heapSize) - elfBufferSize
		var bufPct float64
		if heapSize > 0 {
			bufPct = float64(elfBufferSize) / float64(heapSize) * 100
		}
		fmt.Printf("  Heap (ELF buffer):  %s\n", drawMemoryBar(uint64(elfBufferSize), uint64(heapSize), 40))
		fmt.Printf("                      %10s / %-10s (%.1f%%)\n", formatSize(uint64(elfBufferSize)), formatSize(uint64(heapSize)), bufPct)
		if heapRemaining < 0 {
			heapRemaining = 0
		}
		fmt.Printf("                      Remaining for malloc: %s\n", formatSize(uint64(heapRemaining)))
	} else {
		fmt.Printf("  Heap Pool:          %s available (no ELF buffer needed)\n", formatSize(uint64(heapSize)))
	}

	fmt.Printf("  Stack:              %s available\n", formatSize(uint64(stackSize)))

	fmt.Println()
	fmt.Println(strings.Repeat("=", width))
}

func inferVariantFromPath(elfPath string) string {
	base := filepath.Base(elfPath)
	ext := filepath.Ext(base)
	stem := base[:len(base)-len(ext)]

	// Pattern: zephyr-<variant>
	if strings.HasPrefix(stem, "zephyr-") {
		return stem[7:]
	}

	// Pattern: build/<variant>/zephyr/zephyr.elf
	parts := strings.Split(filepath.ToSlash(elfPath), "/")
	for i, p := range parts {
		if p == "build" && i+1 < len(parts) {
			for _, later := range parts[i+2:] {
				if later == "zephyr" {
					return parts[i+1]
				}
			}
		}
	}

	return stem
}
