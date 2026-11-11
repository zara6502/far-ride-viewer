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

size_t bitmap_offset(uint32_t x, uint32_t y) {
  return size_t((y & 0xC0u) << 5u) |
         size_t((y & 0x07u) << 8u) |
         size_t((y & 0x38u) << 2u) |
         size_t(x >> 3u);
}

void set_pixel_bit(std::vector<uint8_t> &data,
                   uint32_t x, uint32_t y) {
  data[bitmap_offset(x, y)] |=
      static_cast<uint8_t>(0x80u >> (x & 7u));
}

bool pixel_is(const RideImage &image, uint32_t x, uint32_t y,
              uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255u) {
  const size_t offset = size_t(y) * image.stride + size_t(x) * 4u;
  return image.pixels[offset + 0] == r &&
         image.pixels[offset + 1] == g &&
         image.pixels[offset + 2] == b &&
         image.pixels[offset + 3] == a;
}

} // namespace

int main() {
  RideCodecInfo info{};
  info.struct_size = sizeof(info);
  CHECK(RideCodecGetInfo(&info));
  CHECK(info.api_version == RIDE_CODEC_API_VERSION);
  CHECK(std::strcmp(info.name, "ZX Spectrum SCR") == 0);
  CHECK(std::strcmp(info.extensions, "scr") == 0);

  // A size-only probe: no signature bytes are required.
  std::vector<uint8_t> probe_data(8192u, 0);
  CHECK(!RideCodecProbe(probe_data.data(), 4096u));
  CHECK(RideCodecProbe(probe_data.data(), 6144u));
  CHECK(RideCodecProbe(probe_data.data(), 6912u));
  CHECK(!RideCodecProbe(probe_data.data(), 6143u));
  CHECK(!RideCodecProbe(probe_data.data(), 6145u));
  CHECK(!RideCodecProbe(probe_data.data(), 6911u));
  CHECK(!RideCodecProbe(probe_data.data(), 6913u));
  CHECK(!RideCodecProbe(nullptr, 6144u));

  RideCodecError error{};
  RideImage image{};

  // 6144-byte bitmap-only SCR: classic ZX Spectrum non-linear row layout.
  std::vector<uint8_t> mono(6144u, 0);
  set_pixel_bit(mono, 0, 0);
  set_pixel_bit(mono, 7, 0);
  set_pixel_bit(mono, 8, 0);
  set_pixel_bit(mono, 0, 1);
  set_pixel_bit(mono, 0, 8);
  set_pixel_bit(mono, 0, 64);
  set_pixel_bit(mono, 255, 191);

  CHECK(RideCodecProbe(mono.data(), mono.size()));
  image.struct_size = sizeof(image);
  CHECK(RideCodecDecode(mono.data(), mono.size(), &image, &error));
  CHECK(image.width == 256u && image.height == 192u);
  CHECK(image.stride == 1024u && image.data_size == 256u * 192u * 4u);
  CHECK(image.pixel_format == 1u);
  CHECK(pixel_is(image, 0, 0, 255, 255, 255));
  CHECK(pixel_is(image, 1, 0, 0, 0, 0));
  CHECK(pixel_is(image, 7, 0, 255, 255, 255));
  CHECK(pixel_is(image, 8, 0, 255, 255, 255));
  CHECK(pixel_is(image, 0, 1, 255, 255, 255));
  CHECK(pixel_is(image, 0, 8, 255, 255, 255));
  CHECK(pixel_is(image, 0, 64, 255, 255, 255));
  CHECK(pixel_is(image, 255, 191, 255, 255, 255));
  CHECK(pixel_is(image, 254, 191, 0, 0, 0));
  RideCodecFreeImage(&image);
  CHECK(image.pixels == nullptr && image.data_size == 0);

  // 6912-byte color SCR: bitmap plus 32x24 attribute table.
  std::vector<uint8_t> color(6912u, 0);
  for (size_t i = 6144u; i < color.size(); ++i)
    color[i] = 0x4Eu; // BRIGHT, blue paper, yellow ink.

  set_pixel_bit(color, 0, 0);
  set_pixel_bit(color, 8, 0);
  set_pixel_bit(color, 0, 8);

  color[6144u + 0u * 32u + 1u] = 0x02u; // normal red ink, black paper.
  color[6144u + 1u * 32u + 0u] = 0xC4u; // FLASH + BRIGHT green ink.

  CHECK(RideCodecProbe(color.data(), color.size()));
  image = {};
  image.struct_size = sizeof(image);
  CHECK(RideCodecDecode(color.data(), color.size(), &image, &error));
  CHECK(image.width == 256u && image.height == 192u);
  CHECK(image.stride == 1024u && image.data_size == 256u * 192u * 4u);
  CHECK(pixel_is(image, 0, 0, 255, 255, 0)); // bright yellow ink.
  CHECK(pixel_is(image, 1, 0, 0, 0, 255));   // bright blue paper.
  CHECK(pixel_is(image, 8, 0, 205, 0, 0));   // normal red ink.
  CHECK(pixel_is(image, 0, 8, 0, 255, 0));   // bright green; FLASH ignored.
  RideCodecFreeImage(&image);

  // Invalid lengths are rejected by both probe and decoder.
  std::vector<uint8_t> invalid(6145u, 0);
  CHECK(!RideCodecProbe(invalid.data(), invalid.size()));
  image = {};
  image.struct_size = sizeof(image);
  CHECK(!RideCodecDecode(invalid.data(), invalid.size(), &image, &error));
  CHECK(image.pixels == nullptr);
  CHECK(error.code != 0);

  // The bitmap-only image must not be accepted when only FAR's old 4096-byte
  // prefix is available.
  CHECK(!RideCodecProbe(mono.data(), 4096u));

  return 0;
}
