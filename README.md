# Far Ride Viewer

[Русская версия](readme-rus.md)

A modular image-viewer plugin for Far Manager 3.x. It opens supported images in a separate native Windows window and uses independently loadable codec DLLs.

The viewer is based on the windowing and image-viewing functionality developed in [FarQoiViewer](https://github.com/zara6502/far-qoi-viewer). Image decoding is provided by codec modules through the project's versioned C ABI.

## Features

- Press `F3` on a supported file to open it in the viewer.
- Detects image data through each codec's `Probe` function. A filename extension is only a prioritization hint; it does not override content validation.
- Loads codec DLLs from the `Codecs` directory next to `FarRideViewer.dll`.
- Reads files and decodes images in a background thread.
- Preserves the image aspect ratio.
- Provides fit-to-window and 1:1 display modes.
- Supports zooming with the mouse wheel and `+` / `-` keys.
- Supports panning with the arrow keys and centering with `Home`.
- Displays RGBA transparency using alpha compositing over a selectable background.
- Provides checkerboard, black, and white backgrounds.
- Shows viewer version, codec API version, and registered codec information on the `F1` screen.
- Displays diagnostic information for file I/O, codec, and decoded-output validation errors.
- Keeps at most one viewer window open; a repeated invocation focuses the existing window.
- Leaves files that are not recognized by an installed codec to Far Manager.

## Included codecs and supported formats

The repository includes the following codec DLLs. They are built as separate modules and discovered at runtime.

| DLL | Codec | File extensions / formats |
|---|---|---|
| `qoi.dll` | QOI / QOIMT | `.qoi` — standard QOI (RGB/RGBA) and the project's QOIMT variant |
| `pcx.dll` | PCX | `.pcx` |
| `stb_image.dll` | stb_image | `.bmp`, `.tga`, `.jpg`, `.jpeg`, `.png`, `.gif` |
| `zx_spectrum.dll` | ZX Spectrum SCR | `.scr` — 6144-byte and 6912-byte screen files |

### Format notes

- **QOI / QOIMT:** QOI files are identified by the `qoif` signature. The codec also supports the project's QOIMT format with independently decodable strips.
- **PCX:** recognized by its PCX header and decoded by the dedicated PCX module.
- **BMP, TGA, JPEG, PNG, GIF:** decoded by the bundled `stb_image` implementation. Animated GIF files display their first frame only; animation playback is not implemented. JPEG support is limited to the capabilities of stb_image; arithmetic-coded and 12-bit JPEG are not supported.
- **ZX Spectrum SCR:** both variants are 256×192. A 6144-byte file contains the bitmap only (monochrome rendering); a 6912-byte file contains the bitmap plus the 768-byte attribute area (colour rendering). SCR has no magic signature, so the codec recognizes it by exact file size. FLASH animation is not implemented.

All codecs return top-down RGBA8 pixels to the viewer.

## Controls

| Key / Input | Action |
|---|---|
| `0` | Fit image to window |
| `1` | Display at 100% (1:1) |
| `+` / `-` | Zoom in / out |
| Mouse wheel | Zoom in / out |
| Arrow keys | Pan the image |
| `Home` | Center the image |
| `Space` / Left mouse button | Toggle fit-to-window and zoom mode |
| `F1` | Show / hide viewer and registered-codec information |
| `Esc` | Close the viewer (also closes the F1 screen) |
| `w` | White background |
| `b` | Black background |
| `c` | Checkerboard background |

On the `F1` screen, use the Up/Down keys or mouse wheel to scroll the codec list. Press `F1` again to return to the image.

## Open an image from the Far command line

Use the plugin command prefix `ride:` followed by the image path:

```text
ride: C:\\path\\to\\image.qoi
ride: "C:\\path with spaces\\image.png"
```

You can also select a supported file in a Far Manager panel and press `F3`.

## Build

### Requirements

- Windows
- [MSYS2 UCRT64](https://www.msys2.org/) with MinGW-w64 GCC
- CMake 3.20 or later
- Ninja
- Git
- Far Manager source code containing `far/plugin.hpp`

The project uses C++17 and the Far Manager SDK header from the source tree.

Assuming the Far Manager source is located next to this repository in `../FarManager`:

```bash
git clone --depth 1 https://github.com/FarGroup/FarManager.git ../FarManager

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFAR_SOURCE_DIR="$PWD/../FarManager"

cmake --build build
ctest --test-dir build --output-on-failure
```

The build produces `FarRideViewer.dll` and the codec DLLs in `build/Codecs/`.

## Install

Copy the plugin DLL and the codec directory while preserving this layout:

```text
Plugins/
\`-- FarRideViewer/
    |-- FarRideViewer.dll
    \`-- Codecs/
        |-- qoi.dll
        |-- pcx.dll
        |-- stb_image.dll
        \`-- zx_spectrum.dll
```

Restart Far Manager after installing or replacing the plugin or codec DLLs. The codec list is loaded once per Far Manager session.

## Codec API

Codec modules implement **Ride Codec API v1**. The host looks for four exported functions:

- `RideCodecGetInfo`
- `RideCodecProbe`
- `RideCodecDecode`
- `RideCodecFreeImage`

The API defines codec metadata, content probing, decoding to top-down RGBA8, error reporting, and module-owned image-memory release.

- [Codec API reference (English)](docs/CODEC_API.md)
- [Справочник Codec API (русский)](docs/CODEC_API.ru.md)
- [Codec developer guide](docs/CODEC_DEVELOPER_GUIDE.md) (currently available in Russian)

## License

This project is licensed under the Apache License 2.0. See [LICENSE](LICENSE) for details.
