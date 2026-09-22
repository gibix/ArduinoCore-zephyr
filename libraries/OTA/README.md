# OTA

Downloads and installs a sketch update on an Arduino board running the Zephyr
core.

The examples fetch a raw sketch binary over plain HTTP and write it to the
loader's staging area.  On the next boot the loader installs it in place of
the running sketch.

## Requirements

The loader must be built with `CONFIG_OTA=y`.

## Producing the update image

Compile the sketch you want to install and take the plain binary:

```
<sketch>.elf-zsk.bin
```

Serve it with any HTTP server:

```
python3 -m http.server 8000
```

## Examples

- **OTAEthernet** — downloads the binary over Ethernet (DHCP).
- **OTAWiFi** — downloads the binary over WiFi (edit `arduino_secrets.h`).
