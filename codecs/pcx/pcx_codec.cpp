#include "ride_codec_api.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace {

enum ErrorCode : uint32_t {
  kInvalidOutput = 1,
  kNullInput,
  kTruncatedHeader,
  kInvalidSignature,
  kUnsupportedVersion,
  kUnsupportedEncoding,
  kInvalidDimensions,
  kUnsupportedLayout,
  kInvalidScanlineSize,
  kPixelLimitExceeded,
  kTruncatedImageData,
  kInvalidRun,
  kAllocationFailed
};

constexpr uint32_t kMaxPixels = 100000000u;
constexpr uint32_t kRgbaBytesPerPixel = 4u;
constexpr size_t kHeaderSize = 128u;
constexpr size_t kPalette256Size = 769u;

void set_error(RideCodecError *error, ErrorCode code, size_t offset,
               const char *message) {
  if (!error)
    return;

  error->code = static_cast<uint32_t>(code);
  error->byte_offset = offset;
  std::snprintf(error->message, sizeof(error->message), "%s",
                message ? message : "PCX decode failed");
}

uint16_t read_le16(const uint8_t *p) {
  return static_cast<uint16_t>(uint16_t(p[0]) |
                               (uint16_t(p[1]) << 8));
}

bool supported_version(uint8_t version) {
  return version == 0 || version == 2 || version == 3 ||
         version == 4 || version == 5;
}

void set_ega_palette(std::array<uint8_t, 768> &palette) {
  static constexpr uint8_t ega[16][3] = {
      {0, 0, 0},       {0, 0, 170},       {0, 170, 0},
      {0, 170, 170},   {170, 0, 0},       {170, 0, 170},
      {170, 85, 0},    {170, 170, 170},   {85, 85, 85},
      {85, 85, 255},   {85, 255, 85},     {85, 255, 255},
      {255, 85, 85},   {255, 85, 255},    {255, 255, 85},
      {255, 255, 255}
  };

  for (size_t i = 0; i < 16; ++i) {
    palette[i * 3 + 0] = ega[i][0];
    palette[i * 3 + 1] = ega[i][1];
    palette[i * 3 + 2] = ega[i][2];
  }
}

bool decode_scanline(const uint8_t *data, size_t data_size,
                     size_t &offset, uint8_t encoding,
                     uint8_t *destination, size_t destination_size,
                     RideCodecError *error) {
  if (encoding == 0) {
    if (offset > data_size || destination_size > data_size - offset) {
      set_error(error, kTruncatedImageData, offset,
                "Truncated uncompressed PCX scanline");
      return false;
    }

    std::memcpy(destination, data + offset, destination_size);
    offset += destination_size;
    return true;
  }

  size_t written = 0;
  while (written < destination_size) {
    if (offset >= data_size) {
      set_error(error, kTruncatedImageData, offset,
                "Truncated PCX RLE data");
      return false;
    }

    const uint8_t value = data[offset++];
    size_t count = 1;
    uint8_t decoded_value = value;

    if ((value & 0xC0u) == 0xC0u) {
      count = static_cast<size_t>(value & 0x3Fu);
      if (count == 0) {
        set_error(error, kInvalidRun, offset - 1,
                  "PCX RLE run has a zero length");
        return false;
      }
      if (offset >= data_size) {
        set_error(error, kTruncatedImageData, offset,
                  "PCX RLE run is missing its value");
        return false;
      }
      decoded_value = data[offset++];
    }

    if (count > destination_size - written) {
      set_error(error, kInvalidRun, offset,
                "PCX RLE run exceeds scanline boundary");
      return false;
    }

    std::memset(destination + written, decoded_value, count);
    written += count;
  }

  return true;
}

void write_rgba(uint8_t *destination, uint8_t r, uint8_t g,
                uint8_t b, uint8_t a = 255) {
  destination[0] = r;
  destination[1] = g;
  destination[2] = b;
  destination[3] = a;
}

bool decode_pcx(const uint8_t *data, size_t size, RideImage *out,
                RideCodecError *error) {
  if (!data || size < kHeaderSize) {
    set_error(error, kTruncatedHeader, size,
              "PCX header is truncated");
    return false;
  }

  if (data[0] != 0x0A) {
    set_error(error, kInvalidSignature, 0,
              "Invalid PCX manufacturer byte");
    return false;
  }

  const uint8_t version = data[1];
  const uint8_t encoding = data[2];
  const uint8_t bits_per_plane = data[3];
  const uint16_t x_min = read_le16(data + 4);
  const uint16_t y_min = read_le16(data + 6);
  const uint16_t x_max = read_le16(data + 8);
  const uint16_t y_max = read_le16(data + 10);
  const uint8_t planes = data[65];
  const uint16_t bytes_per_line = read_le16(data + 66);
  const uint16_t palette_info = read_le16(data + 68);

  if (!supported_version(version)) {
    set_error(error, kUnsupportedVersion, 1,
              "Unsupported PCX version");
    return false;
  }

  if (encoding > 1) {
    set_error(error, kUnsupportedEncoding, 2,
              "Unsupported PCX encoding");
    return false;
  }

  if (x_max < x_min || y_max < y_min) {
    set_error(error, kInvalidDimensions, 4,
              "Invalid PCX image bounds");
    return false;
  }

  const uint32_t width =
      static_cast<uint32_t>(x_max) - static_cast<uint32_t>(x_min) + 1u;
  const uint32_t height =
      static_cast<uint32_t>(y_max) - static_cast<uint32_t>(y_min) + 1u;

  if (!width || !height ||
      uint64_t(width) * uint64_t(height) > kMaxPixels) {
    set_error(error, kPixelLimitExceeded, 4,
              "PCX image exceeds the pixel limit");
    return false;
  }

  // A single 1-bit plane is monochrome, not a two-color palette lookup.
  const bool monochrome_1bpp =
      bits_per_plane == 1 && planes == 1;
  const bool indexed_planar =
      bits_per_plane == 1 && planes >= 2 && planes <= 4;
  const bool indexed_packed =
      (bits_per_plane == 2 || bits_per_plane == 4) && planes == 1;
  const bool indexed_or_gray_8 =
      bits_per_plane == 8 && planes == 1;
  const bool rgb_24 = bits_per_plane == 8 && planes == 3;
  const bool rgba_32 = bits_per_plane == 8 && planes == 4;

  if (!(monochrome_1bpp || indexed_planar || indexed_packed ||
        indexed_or_gray_8 || rgb_24 || rgba_32)) {
    set_error(error, kUnsupportedLayout, 3,
              "Unsupported PCX bit depth and plane layout");
    return false;
  }

  const uint64_t minimum_line_bytes =
      (uint64_t(width) * uint64_t(bits_per_plane) + 7u) / 8u;
  if (!bytes_per_line || uint64_t(bytes_per_line) < minimum_line_bytes) {
    set_error(error, kInvalidScanlineSize, 66,
              "PCX bytes-per-line is smaller than the image width");
    return false;
  }

  const uint64_t row_size_64 =
      uint64_t(bytes_per_line) * uint64_t(planes);
  const uint64_t output_stride_64 = uint64_t(width) * kRgbaBytesPerPixel;
  const uint64_t output_size_64 = output_stride_64 * uint64_t(height);

  if (row_size_64 > std::numeric_limits<size_t>::max() ||
      output_stride_64 > std::numeric_limits<uint32_t>::max() ||
      output_size_64 > std::numeric_limits<size_t>::max()) {
    set_error(error, kInvalidDimensions, 66,
              "PCX image size overflows the host address space");
    return false;
  }

  const size_t row_size = static_cast<size_t>(row_size_64);
  const size_t output_size = static_cast<size_t>(output_size_64);
  const uint32_t output_stride =
      static_cast<uint32_t>(output_stride_64);

  const bool has_256_palette =
      indexed_or_gray_8 && size >= kHeaderSize + kPalette256Size &&
      data[size - kPalette256Size] == 0x0C;
  const size_t encoded_data_end =
      has_256_palette ? size - kPalette256Size : size;

  std::array<uint8_t, 768> palette{};
  std::memcpy(palette.data(), data + 16, 48);

  const bool header_palette_is_empty =
      std::all_of(palette.begin(), palette.begin() + 48,
                  [](uint8_t value) { return value == 0; });

  if ((version == 3 || version == 4) && header_palette_is_empty &&
      (indexed_planar || indexed_packed)) {
    set_ega_palette(palette);
  }

  if (has_256_palette) {
    std::memcpy(palette.data(), data + size - 768, 768);
  }

  std::vector<uint8_t> row;
  try {
    row.resize(row_size);
  } catch (const std::bad_alloc &) {
    set_error(error, kAllocationFailed, 0,
              "Memory allocation failed for PCX scanline");
    return false;
  }

  uint8_t *pixels = new (std::nothrow) uint8_t[output_size];
  if (!pixels) {
    set_error(error, kAllocationFailed, 0,
              "Memory allocation failed for PCX output");
    return false;
  }

  size_t offset = kHeaderSize;
  for (uint32_t y = 0; y < height; ++y) {
    if (!decode_scanline(data, encoded_data_end, offset, encoding,
                         row.data(), row.size(), error)) {
      delete[] pixels;
      return false;
    }

    for (uint32_t x = 0; x < width; ++x) {
      uint8_t *dst = pixels + size_t(y) * output_stride + size_t(x) * 4u;

      if (rgb_24 || rgba_32) {
        const uint8_t r = row[size_t(x)];
        const uint8_t g = row[size_t(bytes_per_line) + x];
        const uint8_t b = row[size_t(bytes_per_line) * 2u + x];
        const uint8_t a = rgba_32
            ? row[size_t(bytes_per_line) * 3u + x]
            : 255;
        write_rgba(dst, r, g, b, a);
        continue;
      }

      uint32_t index = 0;
      uint32_t maximum_index = 0;

      if (monochrome_1bpp || indexed_planar) {
        maximum_index = (1u << planes) - 1u;
        for (uint32_t plane = 0; plane < planes; ++plane) {
          const uint8_t packed = row[size_t(plane) * bytes_per_line + x / 8u];
          const uint32_t bit = (packed >> (7u - (x & 7u))) & 1u;
          index |= bit << plane;
        }
      } else if (bits_per_plane == 2) {
        maximum_index = 3;
        const uint8_t packed = row[x / 4u];
        const uint32_t shift = 6u - ((x & 3u) * 2u);
        index = (packed >> shift) & 3u;
      } else if (bits_per_plane == 4) {
        maximum_index = 15;
        const uint8_t packed = row[x / 2u];
        index = (x & 1u) ? (packed & 0x0Fu) : (packed >> 4u);
      } else {
        maximum_index = 255;
        index = row[x];
      }

      if (monochrome_1bpp) {
        const uint8_t gray = index ? 255u : 0u;
        write_rgba(dst, gray, gray, gray, 255);
      } else if (indexed_or_gray_8 && !has_256_palette) {
        const uint8_t gray = static_cast<uint8_t>(index);
        write_rgba(dst, gray, gray, gray, 255);
      } else if (palette_info == 2 && !has_256_palette) {
        const uint8_t gray = maximum_index
            ? static_cast<uint8_t>((index * 255u) / maximum_index)
            : 0;
        write_rgba(dst, gray, gray, gray, 255);
      } else {
        const size_t palette_offset = size_t(index) * 3u;
        write_rgba(dst, palette[palette_offset + 0],
                   palette[palette_offset + 1],
                   palette[palette_offset + 2], 255);
      }
    }
  }

  out->width = width;
  out->height = height;
  out->stride = output_stride;
  out->pixel_format = 1; // top-down RGBA8
  out->pixels = pixels;
  out->data_size = output_size;
  return true;
}

void clear_output(RideImage *out, uint32_t caller_size) {
  *out = {};
  out->struct_size = caller_size;
}

} // namespace

extern "C" RIDE_CODEC_EXPORT uint32_t RIDE_CODEC_CALL
RideCodecGetInfo(RideCodecInfo *info) {
  if (!info || info->struct_size < sizeof(RideCodecInfo))
    return 0;

  RideCodecInfo value{};
  value.struct_size = sizeof(value);
  value.api_version = RIDE_CODEC_API_VERSION;
  std::snprintf(value.name, sizeof(value.name), "%s", "PCX");
  std::snprintf(value.extensions, sizeof(value.extensions), "%s", "pcx");
  *info = value;
  return 1;
}

extern "C" RIDE_CODEC_EXPORT uint32_t RIDE_CODEC_CALL
RideCodecProbe(const uint8_t *data, size_t size) {
  return data && size >= kHeaderSize &&
         data[0] == 0x0A && supported_version(data[1]) &&
         data[2] <= 1;
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

  if (!data) {
    set_error(error, kNullInput, 0, "Input buffer is null");
    return 0;
  }

  try {
    if (!decode_pcx(data, size, out, error)) {
      clear_output(out, caller_size);
      return 0;
    }
  } catch (const std::bad_alloc &) {
    clear_output(out, caller_size);
    set_error(error, kAllocationFailed, 0, "Memory allocation failed");
    return 0;
  } catch (...) {
    clear_output(out, caller_size);
    set_error(error, kAllocationFailed, 0,
              "Unexpected exception in PCX decoder");
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
