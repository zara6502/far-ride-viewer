#include "ride_codec_api.h"

#include <cstdio>
#include <new>

namespace {

constexpr uint32_t kWidth = 256u;
constexpr uint32_t kHeight = 192u;
constexpr uint32_t kStride = kWidth * 4u;
constexpr size_t kBitmapSize = 6144u;
constexpr size_t kAttributesSize = 768u;
constexpr size_t kScreenSize = kBitmapSize + kAttributesSize;
constexpr size_t kOutputSize = size_t(kStride) * kHeight;

enum ErrorCode : uint32_t {
  kInvalidOutput = 1,
  kNullInput,
  kInvalidSize,
  kAllocationFailed
};

void set_error(RideCodecError *error, ErrorCode code, size_t offset,
               const char *message) {
  if (!error)
    return;

  error->code = static_cast<uint32_t>(code);
  error->byte_offset = offset;
  std::snprintf(error->message, sizeof(error->message), "%s",
                message ? message : "ZX Spectrum SCR decode failed");
}

void clear_output(RideImage *out, uint32_t caller_size) {
  *out = {};
  out->struct_size = caller_size;
}

/*
 * ZX Spectrum bitmap addressing and palette are adapted from
 * zara6502/scr2atari (Apache-2.0).
 */
size_t bitmap_offset(uint32_t x, uint32_t y) {
  return size_t((y & 0xC0u) << 5u) |
         size_t((y & 0x07u) << 8u) |
         size_t((y & 0x38u) << 2u) |
         size_t(x >> 3u);
}

size_t attribute_offset(uint32_t x, uint32_t y) {
  return kBitmapSize + size_t(y >> 3u) * 32u + size_t(x >> 3u);
}

constexpr uint8_t kPalette[16][3] = {
    // Normal brightness.
    {0, 0, 0},       {0, 0, 205},       {205, 0, 0},     {205, 0, 205},
    {0, 205, 0},     {0, 205, 205},     {205, 205, 0},   {205, 205, 205},
    // Bright.
    {0, 0, 0},       {0, 0, 255},       {255, 0, 0},     {255, 0, 255},
    {0, 255, 0},     {0, 255, 255},     {255, 255, 0},   {255, 255, 255}
};

bool valid_scr_size(size_t size) {
  return size == kBitmapSize || size == kScreenSize;
}

bool decode_scr(const uint8_t *data, size_t size, RideImage *out,
                RideCodecError *error) {
  if (!data) {
    set_error(error, kNullInput, 0, "Input buffer is null");
    return false;
  }

  if (!valid_scr_size(size)) {
    set_error(error, kInvalidSize, size,
              "ZX Spectrum SCR must be exactly 6144 or 6912 bytes");
    return false;
  }

  const bool has_attributes = size == kScreenSize;
  uint8_t *pixels = new (std::nothrow) uint8_t[kOutputSize];
  if (!pixels) {
    set_error(error, kAllocationFailed, 0,
              "Memory allocation failed for ZX Spectrum image");
    return false;
  }

  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      const uint8_t packed = data[bitmap_offset(x, y)];
      const bool set = (packed & (0x80u >> (x & 7u))) != 0;

      uint8_t r = 0;
      uint8_t g = 0;
      uint8_t b = 0;

      if (!has_attributes) {
        // 6144-byte bitmap-only variant: set bits are white, clear bits black.
        r = g = b = set ? 255u : 0u;
      } else {
        const uint8_t attr = data[attribute_offset(x, y)];
        const uint32_t ink = attr & 7u;
        const uint32_t paper = (attr >> 3u) & 7u;
        const uint32_t bright = (attr >> 6u) & 1u;
        const uint32_t color = (set ? ink : paper) | (bright << 3u);

        r = kPalette[color][0];
        g = kPalette[color][1];
        b = kPalette[color][2];
        // The FLASH attribute bit is ignored for a static decoded image.
      }

      const size_t dst = size_t(y) * kStride + size_t(x) * 4u;
      pixels[dst + 0] = r;
      pixels[dst + 1] = g;
      pixels[dst + 2] = b;
      pixels[dst + 3] = 255u;
    }
  }

  out->width = kWidth;
  out->height = kHeight;
  out->stride = kStride;
  out->pixel_format = 1; // top-down RGBA8
  out->pixels = pixels;
  out->data_size = kOutputSize;
  return true;
}

} // namespace

extern "C" RIDE_CODEC_EXPORT uint32_t RIDE_CODEC_CALL
RideCodecGetInfo(RideCodecInfo *info) {
  if (!info || info->struct_size < sizeof(RideCodecInfo))
    return 0;

  RideCodecInfo value{};
  value.struct_size = sizeof(value);
  value.api_version = RIDE_CODEC_API_VERSION;
  std::snprintf(value.name, sizeof(value.name), "%s", "ZX Spectrum SCR");
  std::snprintf(value.extensions, sizeof(value.extensions), "%s", "scr");
  *info = value;
  return 1;
}

extern "C" RIDE_CODEC_EXPORT uint32_t RIDE_CODEC_CALL
RideCodecProbe(const uint8_t *data, size_t size) {
  // SCR has no magic bytes: its two supported variants are identified by size.
  return data && valid_scr_size(size) ? 1u : 0u;
}

extern "C" RIDE_CODEC_EXPORT uint32_t RIDE_CODEC_CALL
RideCodecDecode(const uint8_t *data, size_t size, RideImage *out,
                RideCodecError *error) {
  if (error) {
    error->code = 0;
    error->byte_offset = 0;
    error->message[0] = '\0';
  }

  if (!out || out->struct_size < sizeof(RideImage)) {
    set_error(error, kInvalidOutput, 0, "Invalid output structure");
    return 0;
  }

  const uint32_t caller_size = out->struct_size;
  clear_output(out, caller_size);

  if (!decode_scr(data, size, out, error)) {
    clear_output(out, caller_size);
    return 0;
  }

  return 1;
}

extern "C" RIDE_CODEC_EXPORT void RIDE_CODEC_CALL
RideCodecFreeImage(RideImage *image) {
  if (!image)
    return;

  delete[] image->pixels;
  image->pixels = nullptr;
  image->data_size = 0;
  image->width = 0;
  image->height = 0;
  image->stride = 0;
  image->pixel_format = 0;
}
