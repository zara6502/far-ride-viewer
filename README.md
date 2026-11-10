# FarRideViewer

Independent Far Manager image viewer with dynamically loaded decoder modules.

The host uses the stable C ABI in `include/ride_codec_api.h`. Codec DLLs are discovered in the sibling `Codecs` directory. The first module is standard QOI, implemented independently in `codecs/qoi/qoi_codec.cpp`.

## Layout

```
FarRideViewer.dll
Codecs/
  qoi.dll
```

## Build (MSYS2 UCRT64)

Clone Far Manager to obtain `far/plugin.hpp`, then:

```sh
cmake -S . -B build -G "MSYS Makefiles" -DCMAKE_BUILD_TYPE=Release -DFAR_SOURCE_DIR="C:/src/FarManager"
cmake --build build --parallel
```

Install `build/FarRideViewer.dll` in a Far plugin directory and `build/Codecs/qoi.dll` in its `Codecs` subdirectory. Use `ride: path\\to\\image.qoi` or select an image and press F3.

## Decoder ABI

Modules export `RideCodecGetInfo`, `RideCodecProbe`, `RideCodecDecode`, and `RideCodecFreeImage`. API version 1 returns top-down RGBA8 pixels. Memory is released by the allocating module. The host rejects modules with missing entry points or incompatible API versions.

## Status

Initial implementation scaffold. QOI decoding is implemented in its own DLL; the host discovers modules at runtime and dispatches by file signature rather than extension. The Windows/Far SDK build still needs to be run in the target MSYS2 UCRT64 environment before this should be treated as a verified release.
