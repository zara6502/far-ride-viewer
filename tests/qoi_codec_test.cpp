#include "ride_codec_api.h"

#include <cassert>
#include <cstdint>
#include <cstring>

int main() {
  const uint8_t sample[] = {
      'q', 'o', 'i', 'f', 0, 0, 0, 1, 0, 0, 0, 1, 4, 0,
      0xfe, 0xff, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 1};

  RideCodecInfo info{};
  info.struct_size = sizeof(info);
  assert(RideCodecGetInfo(&info));
  assert(info.struct_size == sizeof(RideCodecInfo));
  assert(info.api_version == RIDE_CODEC_API_VERSION);
  assert(std::strcmp(info.name, "QOI/QOIMT") == 0);
  assert(RideCodecProbe(sample, sizeof(sample)));

  RideImage image{};
  image.struct_size = sizeof(image);
  RideCodecError error{};
  assert(RideCodecDecode(sample, sizeof(sample), &image, &error));
  assert(image.width == 1 && image.height == 1);
  assert(image.stride == 4 && image.data_size == 4);
  assert(image.pixel_format == 1);
  assert(image.pixels[0] == 255 && image.pixels[1] == 0);
  assert(image.pixels[2] == 0 && image.pixels[3] == 255);
  RideCodecFreeImage(&image);
  assert(!image.pixels && image.data_size == 0);

  // QOIMT: two independently decodable one-row strips, with two workers requested.
  const uint8_t qoimt[] = {
      'q', 'o', 'i', 'f',
      0, 0, 0, 1,  // width
      0, 0, 0, 2,  // height
      3, 0,        // RGB, sRGB
      0xfe, 255, 0, 0,  // strip 1: red
      0xfe, 0, 0, 255,  // strip 2: blue
      1, 2, 0, 0, 0, 0, 0, 1  // strip height 1, 2 workers, end marker
  };
  assert(RideCodecProbe(qoimt, sizeof(qoimt)));
  image = {};
  image.struct_size = sizeof(image);
  assert(RideCodecDecode(qoimt, sizeof(qoimt), &image, &error));
  assert(image.width == 1 && image.height == 2);
  assert(image.pixels[0] == 255 && image.pixels[1] == 0 &&
         image.pixels[2] == 0 && image.pixels[3] == 255);
  assert(image.pixels[4] == 0 && image.pixels[5] == 0 &&
         image.pixels[6] == 255 && image.pixels[7] == 255);
  RideCodecFreeImage(&image);

  uint8_t bad[sizeof(sample)];
  std::memcpy(bad, sample, sizeof(sample));
  bad[0] = 'x';
  RideImage invalid{};
  invalid.struct_size = sizeof(invalid);
  assert(!RideCodecDecode(bad, sizeof(bad), &invalid, &error));
  return 0;
}
