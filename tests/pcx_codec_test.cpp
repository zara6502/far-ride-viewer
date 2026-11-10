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

void put_le16(std::vector<uint8_t> &data, size_t offset, uint16_t value) {
  data[offset] = static_cast<uint8_t>(value & 0xFFu);
  data[offset + 1] = static_cast<uint8_t>(value >> 8u);
}

std::vector<uint8_t> make_header(uint8_t bits_per_plane, uint8_t planes,
                                 uint16_t width, uint16_t height,
                                 uint16_t bytes_per_line) {
  std::vector<uint8_t> data(128, 0);
  data[0] = 0x0A;
  data[1] = 5;
  data[2] = 1;
  data[3] = bits_per_plane;
  put_le16(data, 4, 0);
  put_le16(data, 6, 0);
  put_le16(data, 8, static_cast<uint16_t>(width - 1));
  put_le16(data, 10, static_cast<uint16_t>(height - 1));
  data[65] = planes;
  put_le16(data, 66, bytes_per_line);
  put_le16(data, 68, 1);
  return data;
}

void set_header_color(std::vector<uint8_t> &data, size_t index,
                      uint8_t r, uint8_t g, uint8_t b) {
  const size_t offset = 16 + index * 3;
  data[offset + 0] = r;
  data[offset + 1] = g;
  data[offset + 2] = b;
}

void append_encoded(std::vector<uint8_t> &data,
                    const std::vector<uint8_t> &decoded) {
  for (uint8_t value : decoded) {
    if (value >= 0xC0) {
      data.push_back(0xC1);
      data.push_back(value);
    } else {
      data.push_back(value);
    }
  }
}

bool pixel_is(const RideImage &image, uint32_t x, uint32_t y,
              uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
  const size_t offset = size_t(y) * image.stride + size_t(x) * 4u;
  return image.pixels[offset + 0] == r &&
         image.pixels[offset + 1] == g &&
         image.pixels[offset + 2] == b &&
         image.pixels[offset + 3] == a;
}

int test_monochrome_fixture(const char *path,
                            uint32_t expected_width,
                            uint32_t expected_height) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    std::fprintf(stderr, "Cannot open PCX fixture: %s\n", path);
    return 1;
  }

  const std::vector<uint8_t> data{
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>()};

  RideImage image{};
  image.struct_size = sizeof(image);
  RideCodecError error{};
  if (!RideCodecProbe(data.data(), data.size()) ||
      !RideCodecDecode(data.data(), data.size(), &image, &error)) {
    std::fprintf(stderr, "Cannot decode PCX fixture %s: %s\n",
                 path, error.message);
    return 1;
  }

  bool has_black = false;
  bool has_white = false;
  bool valid = image.width == expected_width &&
               image.height == expected_height &&
               pixel_is(image, 0, 0, 255, 255, 255);

  for (uint32_t y = 0; y < image.height && valid; ++y) {
    for (uint32_t x = 0; x < image.width; ++x) {
      const size_t offset = size_t(y) * image.stride + size_t(x) * 4u;
      const uint8_t r = image.pixels[offset + 0];
      const uint8_t g = image.pixels[offset + 1];
      const uint8_t b = image.pixels[offset + 2];
      const uint8_t a = image.pixels[offset + 3];

      if (r != g || g != b || a != 255 || (r != 0 && r != 255)) {
        valid = false;
        break;
      }
      has_black = has_black || r == 0;
      has_white = has_white || r == 255;
    }
  }

  RideCodecFreeImage(&image);
  if (!valid || !has_black || !has_white) {
    std::fprintf(stderr,
                 "PCX fixture is not decoded as black-and-white: %s\n",
                 path);
    return 1;
  }
  // Real PaintBrush and independently created PCX files must decode as
  // pure monochrome regardless of the non-black/white header palette.
  CHECK(test_monochrome_fixture("bunny.pcx", 2080, 2778) == 0);
  CHECK(test_monochrome_fixture("2026-11-11_000052.pcx", 132, 74) == 0);

  return 0;
}

} // namespace

int main() {
  RideCodecInfo info{};
  info.struct_size = sizeof(info);
  CHECK(RideCodecGetInfo(&info));
  CHECK(info.struct_size == sizeof(RideCodecInfo));
  CHECK(info.api_version == RIDE_CODEC_API_VERSION);
  CHECK(std::strcmp(info.name, "PCX") == 0);
  CHECK(std::strcmp(info.extensions, "pcx") == 0);

  RideCodecError error{};

  // 1-bit monochrome ignores the header palette; MSB is the leftmost pixel.
  std::vector<uint8_t> mono = make_header(1, 1, 2, 1, 2);
  set_header_color(mono, 0, 15, 15, 15);
  set_header_color(mono, 1, 14, 14, 14);
  append_encoded(mono, {0x40, 0x00});

  CHECK(RideCodecProbe(mono.data(), mono.size()));
  RideImage image{};
  image.struct_size = sizeof(image);
  CHECK(RideCodecDecode(mono.data(), mono.size(), &image, &error));
  CHECK(image.width == 2 && image.height == 1);
  CHECK(image.stride == 8 && image.data_size == 8);
  CHECK(image.pixel_format == 1);
  CHECK(pixel_is(image, 0, 0, 0, 0, 0));
  CHECK(pixel_is(image, 1, 0, 255, 255, 255));
  RideCodecFreeImage(&image);
  CHECK(image.pixels == nullptr && image.data_size == 0);

  // Packed 4-bit indexed pixels, high nibble first.
  std::vector<uint8_t> packed4 = make_header(4, 1, 2, 1, 2);
  set_header_color(packed4, 1, 255, 0, 0);
  set_header_color(packed4, 2, 0, 255, 0);
  append_encoded(packed4, {0x12, 0x00});
  image = {};
  image.struct_size = sizeof(image);
  CHECK(RideCodecDecode(packed4.data(), packed4.size(), &image, &error));
  CHECK(pixel_is(image, 0, 0, 255, 0, 0));
  CHECK(pixel_is(image, 1, 0, 0, 255, 0));
  RideCodecFreeImage(&image);

  // 8-bit indexed image with the 256-entry palette trailer.
  std::vector<uint8_t> indexed8 = make_header(8, 1, 2, 1, 2);
  append_encoded(indexed8, {1, 2});
  indexed8.push_back(0x0C);
  std::vector<uint8_t> palette(768, 0);
  palette[1 * 3 + 0] = 255;
  palette[2 * 3 + 1] = 255;
  indexed8.insert(indexed8.end(), palette.begin(), palette.end());
  image = {};
  image.struct_size = sizeof(image);
  CHECK(RideCodecDecode(indexed8.data(), indexed8.size(), &image, &error));
  CHECK(pixel_is(image, 0, 0, 255, 0, 0));
  CHECK(pixel_is(image, 1, 0, 0, 255, 0));
  RideCodecFreeImage(&image);

  // 24-bit RGB: three 8-bit planes stored separately in each row.
  std::vector<uint8_t> rgb24 = make_header(8, 3, 2, 1, 2);
  append_encoded(rgb24, {255, 0, 0, 255, 0, 0});
  image = {};
  image.struct_size = sizeof(image);
  CHECK(RideCodecDecode(rgb24.data(), rgb24.size(), &image, &error));
  CHECK(pixel_is(image, 0, 0, 255, 0, 0));
  CHECK(pixel_is(image, 1, 0, 0, 255, 0));
  RideCodecFreeImage(&image);

  // 32-bit RGBA is a commonly used four-plane extension.
  std::vector<uint8_t> rgba32 = make_header(8, 4, 1, 1, 2);
  append_encoded(rgba32, {10, 0, 20, 0, 30, 0, 128, 0});
  image = {};
  image.struct_size = sizeof(image);
  CHECK(RideCodecDecode(rgba32.data(), rgba32.size(), &image, &error));
  CHECK(pixel_is(image, 0, 0, 10, 20, 30, 128));
  RideCodecFreeImage(&image);

  // A run may not spill into the next scanline.
  std::vector<uint8_t> bad_run = make_header(8, 1, 2, 1, 2);
  bad_run.push_back(0xC3);
  bad_run.push_back(7);
  image = {};
  image.struct_size = sizeof(image);
  CHECK(!RideCodecDecode(bad_run.data(), bad_run.size(), &image, &error));
  CHECK(image.pixels == nullptr);
  CHECK(error.code != 0);

  // Truncated header and invalid signature.
  CHECK(!RideCodecProbe(mono.data(), 64));
  std::vector<uint8_t> bad_signature = mono;
  bad_signature[0] = 'X';
  CHECK(!RideCodecProbe(bad_signature.data(), bad_signature.size()));
  image = {};
  image.struct_size = sizeof(image);
  CHECK(!RideCodecDecode(bad_signature.data(), bad_signature.size(),
                         &image, &error));

  return 0;
}
