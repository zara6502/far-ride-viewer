#include "ride_codec_api.h"

#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_FAILURE_USERMSG
#define STBI_MAX_DIMENSIONS 32768
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <climits>
#include <cstdint>
#include <cstdio>
#include <limits>

namespace {

enum ErrorCode : uint32_t {
  kInvalidOutput = 1,
  kNullInput,
  kUnsupportedImage,
  kInvalidDimensions,
  kPixelLimitExceeded,
  kDecodeFailed
};

constexpr uint32_t kMaxPixels = 100000000u;
constexpr uint32_t kRgbaBytesPerPixel = 4u;

void set_error(RideCodecError *error, ErrorCode code, size_t offset,
               const char *message) {
  if (!error)
    return;

  error->code = static_cast<uint32_t>(code);
  error->byte_offset = offset;
  std::snprintf(error->message, sizeof(error->message), "%s",
                message ? message : "stb_image decode failed");
}

void set_stbi_error(RideCodecError *error, ErrorCode code,
                    const char *fallback) {
  const char *reason = stbi_failure_reason();
  set_error(error, code, 0,
            reason && reason[0] ? reason : fallback);
}

void clear_output(RideImage *out, uint32_t caller_size) {
  *out = {};
  out->struct_size = caller_size;
}

bool get_image_info(const uint8_t *data, size_t size,
                    int &width, int &height, int &channels) {
  if (!data || size == 0 || size > static_cast<size_t>(INT_MAX))
    return false;

  width = 0;
  height = 0;
  channels = 0;

  if (!stbi_info_from_memory(data, static_cast<int>(size),
                             &width, &height, &channels))
    return false;

  return width > 0 && height > 0;
}

bool decode_image(const uint8_t *data, size_t size, RideImage *out,
                  RideCodecError *error) {
  int info_width = 0;
  int info_height = 0;
  int info_channels = 0;

  if (!get_image_info(data, size, info_width, info_height, info_channels)) {
    set_stbi_error(error, kUnsupportedImage,
                   "Input is not a supported BMP, TGA, JPEG, or PNG image");
    return false;
  }

  const uint64_t width = static_cast<uint64_t>(info_width);
  const uint64_t height = static_cast<uint64_t>(info_height);
  const uint64_t pixel_count = width * height;

  if (pixel_count > kMaxPixels) {
    set_error(error, kPixelLimitExceeded, 0,
              "Image exceeds the 100-megapixel limit");
    return false;
  }

  const uint64_t stride = width * kRgbaBytesPerPixel;
  const uint64_t output_size = stride * height;
  if (stride > std::numeric_limits<uint32_t>::max() ||
      output_size > std::numeric_limits<size_t>::max()) {
    set_error(error, kInvalidDimensions, 0,
              "Decoded image size overflows the host address space");
    return false;
  }

  int decoded_width = 0;
  int decoded_height = 0;
  int decoded_channels = 0;
  stbi_uc *pixels = stbi_load_from_memory(
      data, static_cast<int>(size), &decoded_width, &decoded_height,
      &decoded_channels, STBI_rgb_alpha);

  if (!pixels) {
    set_stbi_error(error, kDecodeFailed,
                   "stb_image could not decode this BMP, TGA, JPEG, or PNG image");
    return false;
  }

  if (decoded_width != info_width || decoded_height != info_height) {
    stbi_image_free(pixels);
    set_error(error, kDecodeFailed, 0,
              "Image dimensions changed between header parsing and decoding");
    return false;
  }

  out->width = static_cast<uint32_t>(decoded_width);
  out->height = static_cast<uint32_t>(decoded_height);
  out->stride = static_cast<uint32_t>(stride);
  out->pixel_format = 1; // top-down RGBA8
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
  std::snprintf(value.name, sizeof(value.name), "%s", "stb_image");
  std::snprintf(value.extensions, sizeof(value.extensions), "%s", "bmp;tga;jpg;jpeg;png");
  *info = value;
  return 1;
}

extern "C" RIDE_CODEC_EXPORT uint32_t RIDE_CODEC_CALL
RideCodecProbe(const uint8_t *data, size_t size) {
  // FAR probes only the first 4096 bytes. JPEG metadata may push the frame
  // header beyond that prefix, and PNG ancillary chunks (iCCP/iTXt/eXIf) may
  // push IDAT beyond it. Use each format's mandatory leading signature/header
  // for selection; the complete file is validated by RideCodecDecode.
  if (data && size >= 3 &&
      data[0] == 0xFF &&
      data[1] == 0xD8 &&
      data[2] == 0xFF) {
    return 1u;
  }

  static constexpr uint8_t png_signature[8] = {
      0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
  };
  if (data && size >= 24 &&
      std::memcmp(data, png_signature, sizeof(png_signature)) == 0 &&
      data[8] == 0 && data[9] == 0 &&
      data[10] == 0 && data[11] == 13 &&
      std::memcmp(data + 12, "IHDR", 4) == 0) {
    return 1u;
  }

  int width = 0;
  int height = 0;
  int channels = 0;
  return get_image_info(data, size, width, height, channels) ? 1u : 0u;
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

  if (size > static_cast<size_t>(INT_MAX)) {
    set_error(error, kUnsupportedImage, 0,
              "Input is too large for stb_image memory API");
    return 0;
  }

  if (size == 0) {
    set_error(error, kUnsupportedImage, 0, "Input image is empty");
    return 0;
  }

  if (!decode_image(data, size, out, error)) {
    clear_output(out, caller_size);
    return 0;
  }

  return 1;
}

extern "C" RIDE_CODEC_EXPORT void RIDE_CODEC_CALL
RideCodecFreeImage(RideImage *image) {
  if (!image)
    return;

  stbi_image_free(image->pixels);
  image->pixels = nullptr;
  image->data_size = 0;
  image->width = 0;
  image->height = 0;
  image->stride = 0;
  image->pixel_format = 0;
}
