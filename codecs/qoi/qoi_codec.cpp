#include "ride_codec_api.h"
#include "qoi_legacy.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <exception>

namespace {

const char *error_text(QoiErrorCode code) {
  switch (code) {
  case QoiErrorCode::FileTooLarge: return "File is too large";
  case QoiErrorCode::TooShort: return "File is too short";
  case QoiErrorCode::BadMagic: return "Invalid QOI signature";
  case QoiErrorCode::InvalidDimensions: return "Invalid image dimensions";
  case QoiErrorCode::InvalidChannels: return "Invalid channel count";
  case QoiErrorCode::InvalidColorSpace: return "Invalid colorspace";
  case QoiErrorCode::PixelLimitExceeded: return "Pixel limit exceeded";
  case QoiErrorCode::InvalidEndMarker: return "Invalid end marker or QOIMT trailer";
  case QoiErrorCode::InvalidStripHeight: return "Invalid QOIMT strip height";
  case QoiErrorCode::UnexpectedEnd: return "Unexpected end of data";
  case QoiErrorCode::TruncatedRgb: return "Truncated RGB command";
  case QoiErrorCode::TruncatedRgba: return "Truncated RGBA command";
  case QoiErrorCode::TruncatedLuma: return "Truncated LUMA command";
  case QoiErrorCode::InvalidOpcode: return "Invalid opcode";
  case QoiErrorCode::RunExceedsImage: return "RUN exceeds image bounds";
  case QoiErrorCode::RunCrossesStripBoundary: return "RUN crosses strip boundary";
  case QoiErrorCode::StripMissingAbsolutePixel: return "QOIMT strip lacks RGB/RGBA start";
  case QoiErrorCode::ExtraEncodedBytes: return "Unexpected extra encoded bytes";
  case QoiErrorCode::WrongStripCount: return "Wrong number of QOIMT strips";
  case QoiErrorCode::InvalidDecodeRange: return "Invalid decoder range";
  case QoiErrorCode::PixelCountOverflow: return "Pixel count overflow";
  case QoiErrorCode::AllocationFailed: return "Memory allocation failed";
  case QoiErrorCode::UnknownDecodeFailure: return "Unknown decode failure";
  default: return "QOI decode failed";
  }
}

void set_error(RideCodecError *error, uint32_t code, size_t offset,
               const char *message) {
  if (!error)
    return;
  error->code = code;
  error->byte_offset = offset;
  std::snprintf(error->message, sizeof(error->message), "%s",
                message ? message : "Unknown codec error");
}

} // namespace

extern "C" RIDE_CODEC_EXPORT uint32_t RIDE_CODEC_CALL
RideCodecGetInfo(RideCodecInfo *info) {
  if (!info || info->struct_size < sizeof(RideCodecInfo))
    return 0;

  RideCodecInfo value{};
  value.struct_size = sizeof(value);
  value.api_version = RIDE_CODEC_API_VERSION;
  std::snprintf(value.name, sizeof(value.name), "%s", "QOI/QOIMT");
  std::snprintf(value.extensions, sizeof(value.extensions), "%s", "qoi");
  *info = value;
  return 1;
}

extern "C" RIDE_CODEC_EXPORT uint32_t RIDE_CODEC_CALL
RideCodecProbe(const uint8_t *data, size_t size) {
  return data && size >= 4 && std::memcmp(data, "qoif", 4) == 0;
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
    set_error(error, 1, 0, "Invalid output structure");
    return 0;
  }

  const uint32_t callerSize = out->struct_size;
  *out = {};
  out->struct_size = callerSize;

  if (!data) {
    set_error(error, 2, 0, "Input buffer is null");
    return 0;
  }

  QoiImage decoded{};
  QoiDecodeError decodeError{};
  bool ok = false;
  try {
    ok = qoi_decode(data, size, decoded, &decodeError);
  } catch (const std::bad_alloc &) {
    set_error(error, static_cast<uint32_t>(QoiErrorCode::AllocationFailed),
              0, "Memory allocation failed");
    return 0;
  } catch (...) {
    set_error(error,
              static_cast<uint32_t>(QoiErrorCode::UnknownDecodeFailure),
              0, "Unexpected exception in QOI decoder");
    return 0;
  }

  if (!ok) {
    char message[160]{};
    std::snprintf(message, sizeof(message),
                  "%s (stage=%u, strip=%u)",
                  error_text(decodeError.code),
                  static_cast<unsigned>(decodeError.stage),
                  static_cast<unsigned>(decodeError.strip));
    set_error(error, static_cast<uint32_t>(decodeError.code),
              decodeError.offset, message);
    return 0;
  }

  const uint64_t stride64 = uint64_t(decoded.width) * 4u;
  const uint64_t bytes64 = stride64 * uint64_t(decoded.height);
  if (!decoded.width || !decoded.height || stride64 > UINT32_MAX ||
      bytes64 > std::numeric_limits<size_t>::max() ||
      decoded.rgba.size() != static_cast<size_t>(bytes64)) {
    set_error(error, 20, 0, "Decoded image has invalid dimensions or size");
    return 0;
  }

  auto *pixels = new (std::nothrow) uint8_t[static_cast<size_t>(bytes64)];
  if (!pixels) {
    set_error(error, static_cast<uint32_t>(QoiErrorCode::AllocationFailed),
              0, "Memory allocation failed");
    return 0;
  }

  std::memcpy(pixels, decoded.rgba.data(), static_cast<size_t>(bytes64));
  out->width = decoded.width;
  out->height = decoded.height;
  out->stride = static_cast<uint32_t>(stride64);
  out->pixel_format = 1; // top-down RGBA8
  out->pixels = pixels;
  out->data_size = static_cast<size_t>(bytes64);
  return 1;
}

extern "C" RIDE_CODEC_EXPORT void RIDE_CODEC_CALL
RideCodecFreeImage(RideImage *image) {
  if (!image)
    return;
  delete[] image->pixels;
  image->pixels = nullptr;
  image->data_size = 0;
  image->width = image->height = image->stride = image->pixel_format = 0;
}
