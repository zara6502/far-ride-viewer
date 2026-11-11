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

int base64_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

std::vector<uint8_t> decode_base64(const char *encoded) {
  std::vector<uint8_t> decoded;
  uint32_t bits = 0;
  unsigned bit_count = 0;

  for (const char *p = encoded; *p; ++p) {
    if (*p == '=') break;
    const int value = base64_value(*p);
    if (value < 0) continue;

    bits = (bits << 6u) | static_cast<uint32_t>(value);
    bit_count += 6u;
    if (bit_count >= 8u) {
      bit_count -= 8u;
      decoded.push_back(static_cast<uint8_t>((bits >> bit_count) & 0xFFu));
    }
  }
  return decoded;
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
  std::memcpy(data.data() + 54, pixels, sizeof(pixels));
  return data;
}

std::vector<uint8_t> make_bmp8_indexed() {
  // 2x1, 8-bit BI_RGB with a two-entry BGRA palette.
  std::vector<uint8_t> data(66u, 0);
  put_le16(data, 0, 0x4D42u);
  put_le32(data, 2, static_cast<uint32_t>(data.size()));
  put_le32(data, 10, 62);
  put_le32(data, 14, 40);
  put_le32(data, 18, 2);
  put_le32(data, 22, 1);
  put_le16(data, 26, 1);
  put_le16(data, 28, 8);
  put_le32(data, 34, 4);
  put_le32(data, 46, 2); // colors used

  // Palette entries are stored as B,G,R,reserved.
  const uint8_t palette_and_pixels[] = {
      0, 0, 255, 0, // red
      0, 255, 0, 0, // green
      0, 1, 0, 0    // pixel indices and row padding
  };
  std::memcpy(data.data() + 54, palette_and_pixels,
              sizeof(palette_and_pixels));
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

std::vector<uint8_t> make_tga8_indexed() {
  auto data = make_tga_header(1, 2, 1, 8, 0x20);
  data[1] = 1; // color-mapped image
  put_le16(data, 5, 2); // two palette entries
  data[7] = 24; // 24-bit palette entries
  data.insert(data.end(), {
      0, 0, 255, // palette index 0: red, BGR
      0, 255, 0  // palette index 1: green, BGR
  });
  data.insert(data.end(), {0, 1}); // indices
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

std::vector<uint8_t> make_jpeg_rgb_baseline() {
  // 4x4 solid RGB JPEG, baseline sequential, 4:4:4 sampling.
  return decode_base64(
      "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAIBAQEBAQIBAQECAgICAgQDAgICAgUEBAMEBgUGBgYFBgYGBwkIBgcJBwYGCAsICQoKCgoKBggLDAsKDAkKCgr/2wBDAQICAgICAgUDAwUKBwYHCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgr/wAARCAAEAAQDAREAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD5zr+cz/Ww/9k=");
}

std::vector<uint8_t> make_jpeg_grayscale() {
  // 4x4 grayscale JPEG.
  return decode_base64(
      "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAIBAQEBAQIBAQECAgICAgUEBAMEBgUGBgYFBgYGBwkIBgcJBwYGCAsICQoKCgoKBggLDAsKDAkKCgr/wAALCAAEAAQBAREA/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/9oACAEBAAA/ACv/2Q==");
}

std::vector<uint8_t> make_jpeg_rgb_progressive() {
  // 4x4 solid RGB progressive JPEG.
  return decode_base64(
      "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wgARCAAEAAQDAREAAhEBAxEB/8QAFAABAAAAAAAAAAAAAAAAAAAAA//EABUBAQEAAAAAAAAAAAAAAAAAAAYH/9oADAMBAAIQAxAAAAEC8n//xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAEFAn//xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAECAQE/AX//xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAECAQE/AX//xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAY/An//xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAE/IX//2gAMAwEAAgADAAAAEJ//xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAECAQE/EH//xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAECAQE/EH//xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAE/EH//2Q==");
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

bool channel_near(uint8_t actual, uint8_t expected, int tolerance) {
  const int delta = static_cast<int>(actual) - static_cast<int>(expected);
  return delta >= -tolerance && delta <= tolerance;
}

bool decode_jpeg_and_check(const std::vector<uint8_t> &data, Pixel expected,
                           int tolerance, const char *name) {
  RideCodecError error{};
  RideImage image{};
  image.struct_size = sizeof(image);

  if (!RideCodecProbe(data.data(), data.size()) ||
      !RideCodecDecode(data.data(), data.size(), &image, &error)) {
    std::fprintf(stderr, "%s decode failed: %s\n", name, error.message);
    return false;
  }

  bool ok = image.pixels != nullptr && image.width == 4 &&
            image.height == 4 && image.stride == 16u &&
            image.data_size == 64u;
  for (uint32_t y = 0; y < image.height && ok; ++y) {
    for (uint32_t x = 0; x < image.width; ++x) {
      const size_t offset = size_t(y) * image.stride + size_t(x) * 4u;
      if (!channel_near(image.pixels[offset + 0], expected.r, tolerance) ||
          !channel_near(image.pixels[offset + 1], expected.g, tolerance) ||
          !channel_near(image.pixels[offset + 2], expected.b, tolerance) ||
          image.pixels[offset + 3] != 255) {
        std::fprintf(stderr, "%s pixel mismatch at (%u,%u): %u,%u,%u,%u\n",
                     name, x, y,
                     image.pixels[offset + 0], image.pixels[offset + 1],
                     image.pixels[offset + 2], image.pixels[offset + 3]);
        ok = false;
        break;
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
  CHECK(std::strcmp(info.extensions, "bmp;tga;jpg;jpeg") == 0);

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

  const auto bmp8 = make_bmp8_indexed();
  CHECK(decode_and_check(bmp8, 2, 1,
                         {{255, 0, 0, 255}, {0, 255, 0, 255}},
                         "BMP 8-bit indexed"));

  const auto tga8 = make_tga8_indexed();
  CHECK(decode_and_check(tga8, 2, 1,
                         {{255, 0, 0, 255}, {0, 255, 0, 255}},
                         "TGA 8-bit indexed"));

  const auto tga_alpha = make_tga32_alpha();
  CHECK(decode_and_check(tga_alpha, 1, 1,
                         {{10, 20, 30, 128}}, "TGA 32-bit alpha"));

  const auto tga_gray = make_tga8_gray();
  CHECK(decode_and_check(tga_gray, 3, 1,
                         {{0x20, 0x20, 0x20, 255},
                          {0x80, 0x80, 0x80, 255},
                          {0xE0, 0xE0, 0xE0, 255}},
                         "TGA 8-bit grayscale"));

  // This module recognizes BMP, TGA, and JPEG at this stage.
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

  const auto jpeg_rgb = make_jpeg_rgb_baseline();
  CHECK(jpeg_rgb.size() == 632u);
  CHECK(decode_jpeg_and_check(jpeg_rgb, {220, 40, 30, 255}, 8,
                              "JPEG baseline RGB"));

  const auto jpeg_gray = make_jpeg_grayscale();
  CHECK(jpeg_gray.size() == 331u);
  CHECK(decode_jpeg_and_check(jpeg_gray, {128, 128, 128, 255}, 3,
                              "JPEG grayscale"));

  const auto jpeg_progressive = make_jpeg_rgb_progressive();
  CHECK(jpeg_progressive.size() == 520u);
  CHECK(decode_jpeg_and_check(jpeg_progressive, {30, 180, 70, 255}, 16,
                              "JPEG progressive RGB"));

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
