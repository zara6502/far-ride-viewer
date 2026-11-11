#include "ride_codec_api.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

#define CHECK(expr)                                                        \
  do {                                                                     \
    if (!(expr)) {                                                         \
      std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,       \
                   __LINE__, #expr);                                       \
      return 1;                                                            \
    }                                                                      \
  } while (0)

struct Pixel {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a;
};

void put_le16(std::vector<uint8_t> &data, size_t offset, uint16_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8u);
}

void put_le32(std::vector<uint8_t> &data, size_t offset, uint32_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8u);
  data[offset + 2] = static_cast<uint8_t>(value >> 16u);
  data[offset + 3] = static_cast<uint8_t>(value >> 24u);
}

std::vector<uint8_t> make_bmp24() {
  // 2x2, 24-bit BI_RGB, bottom-up, each row padded to 4-byte alignment.
  std::vector<uint8_t> data(54u + 16u, 0);
  put_le16(data, 0, 0x4D42u); // BM
  put_le32(data, 2, static_cast<uint32_t>(data.size()));
  put_le32(data, 10, 54);
  put_le32(data, 14, 40); // BITMAPINFOHEADER
  put_le32(data, 18, 2);
  put_le32(data, 22, 2);
  put_le16(data, 26, 1);
  put_le16(data, 28, 24);
  put_le32(data, 34, 16);

  const uint8_t pixels[] = {
      // Bottom row: blue, white.
      255, 0, 0,  255, 255, 255,  0, 0,
      // Top row: red, green.
      0, 0, 255,  0, 255, 0,  0, 0
  };
  data.insert(data.end(), pixels, pixels + sizeof(pixels));
  return data;
}

std::vector<uint8_t> make_tga_header(uint8_t image_type,
                                     uint16_t width, uint16_t height,
                                     uint8_t bits_per_pixel,
                                     uint8_t descriptor) {
  std::vector<uint8_t> data(18, 0);
  data[2] = image_type;
  put_le16(data, 12, width);
  put_le16(data, 14, height);
  data[16] = bits_per_pixel;
  data[17] = descriptor;
  return data;
}

std::vector<uint8_t> make_tga24_top_origin() {
  auto data = make_tga_header(2, 2, 2, 24, 0x20);
  const uint8_t pixels[] = {
      // Top row: red, green. Bottom row: blue, white.
      0, 0, 255,  0, 255, 0,
      255, 0, 0,  255, 255, 255
  };
  data.insert(data.end(), pixels, pixels + sizeof(pixels));
  return data;
}

std::vector<uint8_t> make_tga24_bottom_origin() {
  auto data = make_tga_header(2, 2, 2, 24, 0x00);
  const uint8_t pixels[] = {
      // Bottom row first: blue, white. Then top row: red, green.
      255, 0, 0,  255, 255, 255,
      0, 0, 255,  0, 255, 0
  };
  data.insert(data.end(), pixels, pixels + sizeof(pixels));
  return data;
}

std::vector<uint8_t> make_tga24_rle() {
  auto data = make_tga_header(10, 2, 2, 24, 0x20);
  // Two red pixels as a run packet, followed by two raw pixels.
  data.push_back(0x81);
  data.insert(data.end(), {0, 0, 255});
  data.push_back(0x01);
  data.insert(data.end(), {
      255, 0, 0,       // blue
      255, 255, 255    // white
  });
  return data;
}

std::vector<uint8_t> make_tga32_alpha() {
  auto data = make_tga_header(2, 1, 1, 32, 0x28);
  data.insert(data.end(), {30, 20, 10, 128}); // BGRA
  return data;
}

std::vector<uint8_t> make_tga8_gray() {
  auto data = make_tga_header(3, 3, 1, 8, 0x20);
  data.insert(data.end(), {0x20, 0x80, 0xE0});
  return data;
}

bool pixel_is(const RideImage &image, uint32_t x, uint32_t y,
              Pixel expected) {
  const size_t offset = size_t(y) * image.stride + size_t(x) * 4u;
  return image.pixels[offset + 0] == expected.r &&
         image.pixels[offset + 1] == expected.g &&
         image.pixels[offset + 2] == expected.b &&
         image.pixels[offset + 3] == expected.a;
}

bool decode_and_check(const std::vector<uint8_t> &data,
                      uint32_t width, uint32_t height,
                      const std::vector<Pixel> &expected,
                      const char *name) {
  RideCodecError error{};
  RideImage image{};
  image.struct_size = sizeof(image);

  if (!RideCodecProbe(data.data(), data.size()) ||
      !RideCodecDecode(data.data(), data.size(), &image, &error)) {
    std::fprintf(stderr, "%s decode failed: %s\n", name, error.message);
    return false;
  }

  bool ok = image.width == width && image.height == height &&
            image.stride == width * 4u &&
            image.data_size == size_t(width) * height * 4u;

  if (ok) {
    for (uint32_t y = 0; y < height && ok; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const size_t index = size_t(y) * width + x;
        if (!pixel_is(image, x, y, expected[index])) {
          std::fprintf(stderr,
                       "%s pixel mismatch at (%u,%u)\n", name, x, y);
          ok = false;
          break;
        }
      }
    }
  }

  RideCodecFreeImage(&image);
  return ok;
}

} // namespace

int main() {
  RideCodecInfo info{};
  info.struct_size = sizeof(info);
  CHECK(RideCodecGetInfo(&info));
  CHECK(info.struct_size == sizeof(RideCodecInfo));
  CHECK(info.api_version == RIDE_CODEC_API_VERSION);
  CHECK(std::strcmp(info.name, "stb_image") == 0);
  CHECK(std::strcmp(info.extensions, "bmp;tga") == 0);

  const std::vector<Pixel> four_colors = {
      {255, 0, 0, 255}, {0, 255, 0, 255},
      {0, 0, 255, 255}, {255, 255, 255, 255}
  };

  const auto bmp = make_bmp24();
  CHECK(RideCodecProbe(bmp.data(), bmp.size()));
  CHECK(decode_and_check(bmp, 2, 2, four_colors, "BMP 24-bit bottom-up"));

  const auto tga_top = make_tga24_top_origin();
  CHECK(RideCodecProbe(tga_top.data(), tga_top.size()));
  CHECK(decode_and_check(tga_top, 2, 2, four_colors, "TGA 24-bit top-origin"));

  const auto tga_bottom = make_tga24_bottom_origin();
  CHECK(decode_and_check(tga_bottom, 2, 2, four_colors,
                         "TGA 24-bit bottom-origin"));

  const std::vector<Pixel> rle_expected = {
      {255, 0, 0, 255}, {255, 0, 0, 255},
      {0, 0, 255, 255}, {255, 255, 255, 255}
  };
  const auto tga_rle = make_tga24_rle();
  CHECK(decode_and_check(tga_rle, 2, 2, rle_expected, "TGA RLE 24-bit"));

  const auto tga_alpha = make_tga32_alpha();
  CHECK(decode_and_check(tga_alpha, 1, 1,
                         {{10, 20, 30, 128}}, "TGA 32-bit alpha"));

  const auto tga_gray = make_tga8_gray();
  CHECK(decode_and_check(tga_gray, 3, 1,
                         {{0x20, 0x20, 0x20, 255},
                          {0x80, 0x80, 0x80, 255},
                          {0xE0, 0xE0, 0xE0, 255}},
                         "TGA 8-bit grayscale"));

  // This module intentionally recognizes only BMP and TGA at this stage.
  const std::vector<uint8_t> png_signature = {
      0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
      0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 1
  };
  const std::vector<uint8_t> jpeg_signature = {
      0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F',
      0, 1, 0, 0, 1, 0, 1, 0
  };
  CHECK(!RideCodecProbe(png_signature.data(), png_signature.size()));
  CHECK(!RideCodecProbe(jpeg_signature.data(), jpeg_signature.size()));

  // A truncated BMP may be recognized by its signature but must fail safely.
  std::vector<uint8_t> truncated_bmp = {'B', 'M', 0, 0, 0, 0};
  RideImage invalid{};
  invalid.struct_size = sizeof(invalid);
  RideCodecError error{};
  CHECK(!RideCodecDecode(truncated_bmp.data(), truncated_bmp.size(),
                         &invalid, &error));
  CHECK(invalid.pixels == nullptr && invalid.data_size == 0);
  CHECK(error.code != 0);

  return 0;
}
