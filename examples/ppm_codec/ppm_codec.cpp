#include "ride_codec_api.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

namespace {

constexpr uint32_t kMaxDimension = 32768u;
constexpr uint64_t kMaxPixels = 100000000ull;

enum ErrorCode : uint32_t {
  kInvalidOutput = 1,
  kNullInput,
  kInvalidHeader,
  kInvalidDimensions,
  kUnsupportedMaxValue,
  kPixelLimitExceeded,
  kTruncatedOrExtraRaster,
  kAllocationFailed
};

void set_error(RideCodecError *error, ErrorCode code,
               size_t offset, const char *message) {
  if (!error)
    return;
  error->code = static_cast<uint32_t>(code);
  error->byte_offset = offset;
  std::snprintf(error->message, sizeof(error->message), "%s",
                message ? message : "PPM decode failed");
}

void clear_output(RideImage *out, uint32_t caller_size) {
  *out = {};
  out->struct_size = caller_size;
}

bool is_space(uint8_t c) {
  return c == ' ' || c == '\t' || c == '\r' ||
         c == '\n' || c == '\v' || c == '\f';
}

/*
 * Reads one ASCII token from the PPM header.
 * Whitespace and comments are accepted before tokens. The terminating
 * whitespace is consumed; CRLF is treated as one line ending.
 *
 * This example intentionally implements a strict P6 subset: after the
 * max-value token, the next byte begins the binary raster.
 */
bool read_token(const uint8_t *data, size_t size, size_t &offset,
                const uint8_t *&token, size_t &token_size) {
  while (offset < size) {
    if (is_space(data[offset])) {
      ++offset;
      continue;
    }
    if (data[offset] == '#') {
      while (offset < size &&
             data[offset] != '\r' && data[offset] != '\n') {
        ++offset;
      }
      continue;
    }
    break;
  }

  if (offset >= size)
    return false;

  const size_t start = offset;
  while (offset < size && !is_space(data[offset]))
    ++offset;

  if (offset == start || offset >= size)
    return false;

  const size_t end = offset;
  const uint8_t separator = data[offset++];
  if (separator == '\r' && offset < size && data[offset] == '\n')
    ++offset;

  token = data + start;
  token_size = end - start;
  return token_size != 0;
}

bool parse_decimal(const uint8_t *token, size_t token_size,
                   uint32_t &value) {
  if (!token || token_size == 0)
    return false;

  uint64_t parsed = 0;
  for (size_t i = 0; i < token_size; ++i) {
    const uint8_t c = token[i];
    if (c < '0' || c > '9')
      return false;
    parsed = parsed * 10u + uint64_t(c - '0');
    if (parsed > std::numeric_limits<uint32_t>::max())
      return false;
  }

  value = static_cast<uint32_t>(parsed);
  return true;
}

bool parse_header(const uint8_t *data, size_t size,
                  uint32_t &width, uint32_t &height,
                  size_t &raster_offset) {
  if (!data || size < 3 ||
      data[0] != 'P' || data[1] != '6' || !is_space(data[2])) {
    return false;
  }

  size_t offset = 2;
  const uint8_t *token = nullptr;
  size_t token_size = 0;
  uint32_t max_value = 0;

  if (!read_token(data, size, offset, token, token_size) ||
      !parse_decimal(token, token_size, width)) {
    return false;
  }
  if (!read_token(data, size, offset, token, token_size) ||
      !parse_decimal(token, token_size, height)) {
    return false;
  }
  if (!read_token(data, size, offset, token, token_size) ||
      !parse_decimal(token, token_size, max_value)) {
    return false;
  }
  if (max_value != 255u)
    return false;

  raster_offset = offset;
  return true;
}

bool decode_ppm(const uint8_t *data, size_t size,
                RideImage *out, RideCodecError *error) {
  uint32_t width = 0;
  uint32_t height = 0;
  size_t raster_offset = 0;

  if (!parse_header(data, size, width, height, raster_offset)) {
    set_error(error, kInvalidHeader, 0,
              "Expected a valid binary PPM P6 header with max value 255");
    return false;
  }

  if (width == 0 || height == 0 ||
      width > kMaxDimension || height > kMaxDimension) {
    set_error(error, kInvalidDimensions, 0,
              "PPM dimensions are zero or exceed 32768");
    return false;
  }

  const uint64_t pixel_count = uint64_t(width) * uint64_t(height);
  if (pixel_count > kMaxPixels) {
    set_error(error, kPixelLimitExceeded, 0,
              "PPM image exceeds the 100-megapixel limit");
    return false;
  }

  const uint64_t source_size = pixel_count * 3u;
  const uint64_t stride = uint64_t(width) * 4u;
  const uint64_t output_size = stride * uint64_t(height);

  if (raster_offset > size ||
      source_size != uint64_t(size - raster_offset)) {
    set_error(error, kTruncatedOrExtraRaster, raster_offset,
              "PPM raster size does not match its dimensions");
    return false;
  }

  if (stride > std::numeric_limits<uint32_t>::max() ||
      output_size > std::numeric_limits<size_t>::max()) {
    set_error(error, kInvalidDimensions, 0,
              "Decoded PPM image size overflows the address space");
    return false;
  }

  uint8_t *pixels =
      new (std::nothrow) uint8_t[static_cast<size_t>(output_size)];
  if (!pixels) {
    set_error(error, kAllocationFailed, 0,
              "Unable to allocate the RGBA output buffer");
    return false;
  }

  const uint8_t *source = data + raster_offset;
  const size_t count = static_cast<size_t>(pixel_count);
  for (size_t i = 0; i < count; ++i) {
    pixels[i * 4u + 0u] = source[i * 3u + 0u];
    pixels[i * 4u + 1u] = source[i * 3u + 1u];
    pixels[i * 4u + 2u] = source[i * 3u + 2u];
    pixels[i * 4u + 3u] = 255u;
  }

  out->width = width;
  out->height = height;
  out->stride = static_cast<uint32_t>(stride);
  out->pixel_format = 1u; // top-down, non-premultiplied RGBA8
  out->pixels = pixels;
  out->data_size = static_cast<size_t>(output_size);
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
  std::snprintf(value.name, sizeof(value.name), "%s", "PPM P6 example");
  std::snprintf(value.extensions, sizeof(value.extensions), "%s", "ppm;pnm");
  *info = value;
  return 1;
}

extern "C" RIDE_CODEC_EXPORT uint32_t RIDE_CODEC_CALL
RideCodecProbe(const uint8_t *data, size_t size) {
  return data && size >= 3 &&
         data[0] == 'P' && data[1] == '6' && is_space(data[2])
             ? 1u : 0u;
}

extern "C" RIDE_CODEC_EXPORT uint32_t RIDE_CODEC_CALL
RideCodecDecode(const uint8_t *data, size_t size,
                RideImage *out, RideCodecError *error) {
  if (error) {
    error->code = 0;
    error->byte_offset = 0;
    error->message[0] = '\0';
  }

  if (!out || out->struct_size < sizeof(RideImage)) {
    set_error(error, kInvalidOutput, 0, "Invalid RideImage output structure");
    return 0;
  }

  const uint32_t caller_size = out->struct_size;
  clear_output(out, caller_size);

  if (!data) {
    set_error(error, kNullInput, 0, "Input buffer is null");
    return 0;
  }

  try {
    if (!decode_ppm(data, size, out, error)) {
      clear_output(out, caller_size);
      return 0;
    }
  } catch (...) {
    clear_output(out, caller_size);
    set_error(error, kAllocationFailed, 0,
              "Unexpected exception while decoding PPM");
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
