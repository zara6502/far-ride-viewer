# Ride Codec API v1

[Русская версия](CODEC_API.ru.md)

This document describes the C ABI used by Far Ride Viewer to load codec DLLs and decode image files.

## 1. Overview

Each codec is a separate Windows DLL placed in the `Codecs` directory next to `FarRideViewer.dll`.

At startup, the host scans that directory for `*.dll` files. A module is accepted only if it exports all four required functions, reports the supported API version, and returns valid codec metadata.

The API is intentionally small:

1. **GetInfo** — reports the codec name and supported filename extensions.
2. **Probe** — determines whether a byte buffer belongs to the codec's format.
3. **Decode** — decodes a complete file into a standard pixel buffer.
4. **FreeImage** — releases memory allocated by the codec.

The current API version is `1` (`RIDE_CODEC_API_VERSION`).

## 2. Header and platform ABI

Include:

```cpp
#include "ride_codec_api.h"
```

The header defines the Windows calling convention and exports:

- `RIDE_CODEC_EXPORT` — `__declspec(dllexport)` when building a codec DLL, otherwise `__declspec(dllimport`.
- `RIDE_CODEC_CALL` — `__cdecl` on Windows.
- `RIDE_CODEC_API_VERSION` — currently `1u`.
- `RIDE_CODEC_NAME_CAPACITY` — 64 bytes.
- `RIDE_CODEC_EXTENSIONS_CAPACITY` — 128 bytes.

The public functions use C linkage. Build codec modules with a compatible architecture and ABI (the current project targets Windows with MinGW-w64).

## 3. Data structures

### `RideCodecInfo`

```cpp
typedef struct RideCodecInfo {
    uint32_t struct_size;
    uint32_t api_version;
    char name[64];
    char extensions[128];
} RideCodecInfo;
```

| Field | Meaning |
|---|---|
| `struct_size` | Size of the structure in bytes. The caller sets the available size before calling `RideCodecGetInfo`; the codec returns its structure size. |
| `api_version` | API version implemented by the codec. It must equal `RIDE_CODEC_API_VERSION`. |
| `name` | Human-readable codec name, encoded as a null-terminated UTF-8 string. |
| `extensions` | Semicolon-separated filename extensions, without leading dots; for example `bmp;tga;jpg`. |

The host uses the name and extensions on the F1 information screen. Extensions are compared case-insensitively and are used only to prioritize codec probing.

### `RideImage`

```cpp
typedef struct RideImage {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
    uint8_t* pixels;
    size_t data_size;
} RideImage;
```

| Field | Meaning |
|---|---|
| `struct_size` | Structure size. The host initializes it before calling `RideCodecDecode`; the codec must preserve the caller-provided value. |
| `width`, `height` | Decoded image dimensions in pixels; both must be non-zero. |
| `stride` | Distance in bytes between the starts of consecutive rows. |
| `pixel_format` | Must be `1`, meaning top-down RGBA8. |
| `pixels` | Pointer to the first pixel (top-left corner). |
| `data_size` | Size in bytes of the allocated pixel buffer. |

The required number of bytes in each row is `width * 4`. The stride must be at least that value. The buffer must contain at least `stride * (height - 1) + width * 4` bytes.

RGBA8 means four 8-bit channels in R, G, B, A order. Rows are stored top-down; the first row is the top row of the image. Alpha is straight (not premultiplied).

### `RideCodecError`

```cpp
typedef struct RideCodecError {
    uint32_t code;
    size_t byte_offset;
    char message[160];
} RideCodecError;
```

| Field | Meaning |
|---|---|
| `code` | Codec-defined error code. |
| `byte_offset` | Byte offset associated with the error, when known. |
| `message` | Null-terminated UTF-8 diagnostic message. |

The host displays these fields when decoding fails. Error codes are defined by each codec; there is no shared enumeration in API v1.

## 4. Required exported functions

All four functions must be exported with C linkage and the declared calling convention.

```cpp
uint32_t RIDE_CODEC_CALL RideCodecGetInfo(RideCodecInfo* info);

uint32_t RIDE_CODEC_CALL RideCodecProbe(
    const uint8_t* data,
    size_t size
);

uint32_t RIDE_CODEC_CALL RideCodecDecode(
    const uint8_t* data,
    size_t size,
    RideImage* out,
    RideCodecError* error
);

void RIDE_CODEC_CALL RideCodecFreeImage(RideImage* image);
```

### 4.1 `RideCodecGetInfo`

Reports codec metadata.

- The host initializes `info->struct_size` to `sizeof(RideCodecInfo)`.
- Return non-zero on success and zero on failure.
- Reject a null pointer or a structure smaller than the version expected by the codec.
- Set `struct_size` to `sizeof(RideCodecInfo)` and `api_version` to `RIDE_CODEC_API_VERSION`.
- Terminate both strings with a null byte.
- List extensions without dots, separated by semicolons.

The host rejects modules whose reported API version does not exactly match its own.

### 4.2 `RideCodecProbe`

Checks whether the supplied bytes appear to belong to the codec's format.

- Return non-zero if the data is recognized; otherwise return zero.
- The input pointer is read-only and may be null when `size == 0`.
- Never read beyond `size`.
- The function must safely handle short and truncated prefixes.
- The function receives no filename or extension; it must inspect the bytes only.

The host uses `Probe` twice:

1. **F3 pre-check:** it reads up to the first 8192 bytes and calls the probes. If no codec recognizes that prefix, a matching declared extension may still allow the viewer to open. This is only a preliminary decision.
2. **Actual loading:** after reading the complete file, the host calls the probes again with the full buffer. A codec is selected only if its `Probe` succeeds.

Codecs whose declared extension matches the filename are probed first. If none recognizes the data, the host probes the remaining codecs. The extension never overrides a failed full-file probe.

This two-stage process also allows formats whose identifying structure is not fully available in the first 8192 bytes. For example, the JPEG frame header or PNG image data may occur later in a file with extensive metadata.

### 4.3 `RideCodecDecode`

Decodes a complete file from memory.

- `data` points to the complete file contents and `size` is its exact length.
- `out` is initialized by the host with `out->struct_size = sizeof(RideImage)`.
- Return non-zero on success and zero on failure.
- On success, fill all `RideImage` fields and return a valid top-down RGBA8 buffer.
- On failure, populate `RideCodecError` when `error` is not null.
- Preserve the caller-provided `out->struct_size` value when clearing or filling the output structure.
- Check arithmetic for overflow before calculating dimensions, stride, or allocation sizes.

The host validates the returned structure, dimensions, pixel format, stride, and buffer size before using the image.

### 4.4 `RideCodecFreeImage`

Releases the image buffer allocated by the codec.

- The host calls this function from the same DLL that decoded the image.
- It may be called after either a successful or a failed `RideCodecDecode`.
- It must safely accept a null pointer and an empty/partially initialized image.
- It must release only memory owned by that codec.
- Clear the image's pointer and size fields after release where practical.

Do not use the host's allocator or another DLL's allocator for memory that this function must release.

## 5. Memory ownership and lifetime

The codec owns the memory returned in `RideImage::pixels`. The host copies the validated RGBA8 pixels into its own storage and then calls `RideCodecFreeImage`.

The codec DLL must remain loaded until its returned image has been released. The host retains the codec module for the lifetime of the registered codec.

Do not return pointers to stack storage, temporary buffers, or memory that becomes invalid when `RideCodecDecode` returns.

## 6. Minimal implementation outline

```cpp
#include "ride_codec_api.h"

#include <cstring>
#include <new>

extern "C" RIDE_CODEC_EXPORT uint32_t RIDE_CODEC_CALL
RideCodecGetInfo(RideCodecInfo* info)
{
    if (!info || info->struct_size < sizeof(RideCodecInfo))
        return 0;

    RideCodecInfo value{};
    value.struct_size = sizeof(value);
    value.api_version = RIDE_CODEC_API_VERSION;
    std::strncpy(value.name, "Example", sizeof(value.name) - 1);
    std::strncpy(value.extensions, "example;ex", sizeof(value.extensions) - 1);
    *info = value;
    return 1;
}

extern "C" RIDE_CODEC_EXPORT uint32_t RIDE_CODEC_CALL
RideCodecProbe(const uint8_t* data, size_t size)
{
    return data && size >= 4 &&
           data[0] == 'E' && data[1] == 'X' &&
           data[2] == 'A' && data[3] == 'M';
}

extern "C" RIDE_CODEC_EXPORT uint32_t RIDE_CODEC_CALL
RideCodecDecode(const uint8_t* data, size_t size,
                RideImage* out, RideCodecError* error)
{
    // Validate input and output structure, decode the complete file,
    // allocate a top-down RGBA8 buffer, and fill every RideImage field.
    // Return 1 on success or 0 on failure.
    (void)data; (void)size; (void)out; (void)error;
    return 0;
}

extern "C" RIDE_CODEC_EXPORT void RIDE_CODEC_CALL
RideCodecFreeImage(RideImage* image)
{
    if (!image)
        return;
    delete[] image->pixels;
    image->pixels = nullptr;
    image->data_size = 0;
}
```

The example is a structural outline, not a complete decoder.

## 7. Host loading and selection rules

- The host scans `Codecs\\*.dll` beside the plugin DLL.
- A DLL must export all four required symbols.
- `RideCodecGetInfo` must succeed and report API version 1.
- The codec name and extension list are displayed on the F1 screen.
- Extension matching is case-insensitive.
- A matching extension affects probe order only.
- Full-file recognition by `RideCodecProbe` is mandatory before decoding.
- The order in which multiple codecs recognize the same data is not a separately configurable priority contract in API v1.

The codec list is cached for the current Far Manager session. Restart Far Manager after adding, removing, or replacing a codec DLL.

## 8. Current bundled codecs

| DLL | Name reported by API | Extensions |
|---|---|---|
| `qoi.dll` | `QOI/QOIMT` | `qoi` |
| `pcx.dll` | `PCX` | `pcx` |
| `stb_image.dll` | `stb_image` | `bmp;tga;jpg;jpeg;png;gif` |
| `zx_spectrum.dll` | `ZX Spectrum SCR` | `scr` |

## 9. Compatibility notes

API v1 is a fixed C ABI contract. A host rejects a codec that reports a different API version. If the structures, calling convention, pixel format contract, or required exports change incompatibly, introduce a new API version rather than silently changing version 1.

For practical integration instructions and the standalone PPM example, see the [codec developer guide](CODEC_DEVELOPER_GUIDE.md).
