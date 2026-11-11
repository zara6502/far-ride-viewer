#include "ride_codec_api.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
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

std::vector<uint8_t> make_jpeg_with_large_app1() {
  // Insert an 8 KiB APP1 segment before the JPEG frame header. This models
  // files where EXIF/metadata pushes the SOF marker beyond FAR's 4096-byte
  // signature-probe buffer. APP1 payload bytes are opaque to the JPEG decoder.
  const auto jpeg = make_jpeg_rgb_baseline();
  constexpr size_t kApp1PayloadSize = 8192u;
  constexpr uint16_t kApp1SegmentLength =
      static_cast<uint16_t>(kApp1PayloadSize + 2u);

  std::vector<uint8_t> data;
  data.reserve(jpeg.size() + kApp1PayloadSize + 4u);
  data.insert(data.end(), jpeg.begin(), jpeg.begin() + 2); // SOI
  data.push_back(0xFF);
  data.push_back(0xE1); // APP1
  data.push_back(static_cast<uint8_t>(kApp1SegmentLength >> 8u));
  data.push_back(static_cast<uint8_t>(kApp1SegmentLength));
  data.insert(data.end(), kApp1PayloadSize, 0xA5);
  data.insert(data.end(), jpeg.begin() + 2, jpeg.end());
  return data;
}

std::vector<uint8_t> make_png_rgb() {
  // 2x1, truecolor RGB8 PNG.
  return decode_base64(
      "iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAIAAAB7QOjdAAAAD0lEQVR42mO4oyHHdSIKAAh3Ak8JX2JVAAAAAElFTkSuQmCC");
}

std::vector<uint8_t> make_png_rgba() {
  // 2x2, truecolor RGBA8 PNG with opaque, semi-transparent, and transparent pixels.
  return decode_base64(
      "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFElEQVR42mP4z8DwHwgbGMA0EAAAP9cIeV/HbbEAAAAASUVORK5CYII=");
}

std::vector<uint8_t> make_png_indexed_trns() {
  // 2x1 indexed PNG with a palette and per-entry transparency (tRNS).
  return decode_base64(
      "iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAMAAADD/I+4AAAACVBMVEX/AAAA/wAAAP8tSs2KAAAAA3RSTlP/QAC0l782AAAAC0lEQVR42mNgYAQAAAQAAizeSK0AAAAASUVORK5CYII=");
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

bool decode_png_fixture_and_check(const char *path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::fprintf(stderr, "PNG fixture open failed: %s\\n", path);
    return false;
  }

  const std::vector<uint8_t> data(
      (std::istreambuf_iterator<char>(input)),
      std::istreambuf_iterator<char>());
  if (input.bad() || data.size() < 24u) {
    std::fprintf(stderr, "PNG fixture read failed or file is too short: %s\\n",
                 path);
    return false;
  }

  // Match FAR's host-side selection: only the first 4096 bytes are probed.
  const size_t probe_size = data.size() < 4096u ? data.size() : 4096u;
  if (!RideCodecProbe(data.data(), probe_size)) {
    std::fprintf(stderr, "PNG fixture was not recognized from its prefix: %s\\n",
                 path);
    return false;
  }

  RideImage image{};
  image.struct_size = sizeof(image);
  RideCodecError error{};
  if (!RideCodecDecode(data.data(), data.size(), &image, &error)) {
    error.message[sizeof(error.message) - 1] = '\\0';
    std::fprintf(stderr, "PNG fixture decode failed: %s (%s)\\n",
                 path, error.message);
    return false;
  }

  const uint64_t row_bytes = uint64_t(image.width) * 4u;
  const uint64_t required_bytes = uint64_t(image.stride) * image.height;
  const bool valid = image.pixels && image.width && image.height &&
                     image.pixel_format == 1u &&
                     image.stride >= row_bytes &&
                     image.data_size >= required_bytes;
  if (!valid) {
    std::fprintf(stderr, "PNG fixture returned invalid RGBA layout: %s\\n",
                 path);
  } else {
    std::fprintf(stdout, "PNG fixture OK: %s (%ux%u, %zu bytes)\\n",
                 path, image.width, image.height, data.size());
  }

  RideCodecFreeImage(&image);
  return valid;
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
  CHECK(std::strcmp(info.extensions, "bmp;tga;jpg;jpeg;png") == 0);

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

  // This module recognizes BMP, TGA, JPEG, and PNG.
  const std::vector<uint8_t> png_signature = {
      0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
      0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 1
  };
  const std::vector<uint8_t> jpeg_signature = {
      0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F',
      0, 1, 0, 0, 1, 0, 1, 0
  };
  CHECK(!RideCodecProbe(png_signature.data(), png_signature.size()));
  // Valid PNG headers are recognized from the file data.
  const auto png_rgb = make_png_rgb();
  CHECK(png_rgb.size() == 72u);
  CHECK(png_rgb.size() < 4096u);
  CHECK(RideCodecProbe(png_rgb.data(), png_rgb.size()));
  CHECK(decode_and_check(png_rgb, 2, 1,
                         {{220, 40, 30, 255}, {10, 200, 90, 255}},
                         "PNG RGB8"));

  const auto png_rgba = make_png_rgba();
  CHECK(png_rgba.size() == 77u);
  CHECK(RideCodecProbe(png_rgba.data(), png_rgba.size()));
  CHECK(decode_and_check(png_rgba, 2, 2,
                         {{255, 0, 0, 255}, {0, 255, 0, 128},
                          {0, 0, 255, 0}, {255, 255, 255, 255}},
                         "PNG RGBA8 alpha"));

  const auto png_indexed = make_png_indexed_trns();
  CHECK(png_indexed.size() == 104u);
  CHECK(RideCodecProbe(png_indexed.data(), png_indexed.size()));
  CHECK(decode_and_check(png_indexed, 2, 1,
                         {{255, 0, 0, 255}, {0, 255, 0, 64}},
                         "PNG indexed with tRNS"));

  // Real-world files added to tests/ to cover ancillary metadata before IDAT.
  CHECK(decode_png_fixture_and_check("snow-expansion.png"));
  CHECK(decode_png_fixture_and_check("p22.png"));
  CHECK(decode_png_fixture_and_check("p35.png"));

  // A short JPEG prefix is enough for host-side codec selection.
  CHECK(RideCodecProbe(jpeg_signature.data(), jpeg_signature.size()));

  const auto jpeg_rgb = make_jpeg_rgb_baseline();
  CHECK(jpeg_rgb.size() == 632u);
  CHECK(decode_jpeg_and_check(jpeg_rgb, {220, 40, 30, 255}, 8,
                              "JPEG baseline RGB"));

  const auto jpeg_large_app1 = make_jpeg_with_large_app1();
  CHECK(jpeg_large_app1.size() > 4096u);
  // Match plugin.cpp: probe only the first 4096 bytes of the file.
  CHECK(RideCodecProbe(jpeg_large_app1.data(), 4096u));
  CHECK(decode_jpeg_and_check(jpeg_large_app1, {220, 40, 30, 255}, 8,
                              "JPEG with large APP1 metadata"));

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
