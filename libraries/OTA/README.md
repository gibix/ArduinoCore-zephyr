# OTA

Applies a sketch update on an Arduino board running the Zephyr core.

The library stages an update image into the `/ota:` partition and asks the
loader to install it on the next boot. It does not care where the image comes
from: feed it bytes from an HTTP download, from a file already on the
filesystem, or from any `Stream`.

## Requirements

The loader on the board must be built with `CONFIG_OTA=y` — the library calls
`ota_sketch_ready()` / `ota_sketch_start()`, which the loader exports to
sketches. Variants without it fail at compile time with a clear message rather
than an undefined-symbol link error.

## Producing an update image

Compile the sketch you want to install; the build emits the update image next
to the other artifacts:

```
<sketch>.elf-zsk.bin.ota
```

This is the plain (uncompressed) container. The build also produces a
`.lzss.ota` variant and `-bundle.ota` files that carry the loader as well —
this library handles **only** the plain sketch-only `.elf-zsk.bin.ota`.

Boards opt into `.ota` generation with `build.ota.magic` and
`build.ota.sketch_offset` in `boards.txt`.

## Image format

```
offset  0   uint32  len           file size minus 8
offset  4   uint32  crc32         CRC-32 of the file from offset 8 onwards
offset  8   uint32  magic_number  board identifier, must match this board
offset 12   uint8   version[8]    bit 6 of version[0] marks LZSS compression
offset 20           payload       the sketch image
```

The library checks `magic_number`, `len` and `crc32`, rejects compressed
images, and writes only the payload to the staging file.

## API

```cpp
bool     isOtaCapable();
Error    begin();
size_t   write(const uint8_t *buf, size_t len);
int      readFrom(const char *path);
int      readFrom(Stream &stream, size_t len, uint32_t timeout_ms = 10000);
int      readFrom(Client &client, const char *host, uint16_t port,
                  const char *path, uint32_t timeout_ms = 10000);
Error    update();
void     reset();
void     abort();
uint32_t length() const;
Error    error() const;
const char *errorString();
void     setFeedWatchdogFunc(void (*func)(void));
```

`begin()` opens the staging file, `write()` (or one of the `readFrom()`
helpers) feeds it, `update()` verifies the CRC and commits, `reset()` reboots
into the new image and does not return.

The `Client` overload performs a minimal HTTP GET. It takes the client from the
caller, so the same code path serves WiFi and Ethernet without the library
depending on either. Redirects, chunked transfer encoding and TLS are not
supported.

Nothing is installed until `update()` succeeds: bytes go to a temporary file
and only the final rename makes the image visible to the loader, so an
interrupted transfer leaves the running sketch untouched.

## Example

```cpp
#include <OTA.h>

OTA.begin();
OTA.readFrom("/ota:/UPDATE.OTA");
if (OTA.update() == OTAClass::Error::None) {
  OTA.reset();
} else {
  Serial.println(OTA.errorString());
}
```

See `examples/` for the file, WiFi and Ethernet variants.
