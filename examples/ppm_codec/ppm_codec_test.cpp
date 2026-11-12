#include "ride_codec_api.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      std::fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__,        \
                   #condition);                                              \
      return 1;                                                              \
    }                                                                        \
  } while (false)

int main() {
  RideCodecInfo info{};
  info.struct_size = sizeof(info);
  CHECK(RideCodecGetInfo(&info) != 0);
  CHECK(info.api_version == RIDE_CODEC_API_VERSION);
  CHECK(std::strcmp(info.name, "PPM P6 example") == 0);
  CHECK(std::strcmp(info.extensions, "ppm;pnm") == 0);

  const uint8_t ppm[] = {
      'P', '6', '\n', '2', ' ', '1', '\n', '2', '5', '5', '\n',
      255, 0, 0, 0, 255, 0
  };

  CHECK(RideCodecProbe(ppm, sizeof(ppm)) != 0);

  RideImage image{};
  image.struct_size = sizeof(image);
  RideCodecError error{};
  CHECK(RideCodecDecode(ppm, sizeof(ppm), &image, &error) != 0);
  CHECK(image.width == 2);
  CHECK(image.height == 1);
  CHECK(image.stride == 8);
  CHECK(image.pixel_format == 1);
  CHECK(image.data_size == 8);
  CHECK(image.pixels != nullptr);

  CHECK(image.pixels[0] == 255 && image.pixels[1] == 0 &&
        image.pixels[2] == 0 && image.pixels[3] == 255);
  CHECK(image.pixels[4] == 0 && image.pixels[5] == 255 &&
        image.pixels[6] == 0 && image.pixels[7] == 255);

  RideCodecFreeImage(&image);
  CHECK(image.pixels == nullptr);
  RideCodecFreeImage(&image);

  const uint8_t truncated[] = {
      'P', '6', '\n', '1', ' ', '1', '\n', '2', '5', '5', '\n'
  };
  CHECK(RideCodecProbe(truncated, sizeof(truncated)) != 0);

  RideImage failed{};
  failed.struct_size = sizeof(failed);
  CHECK(RideCodecDecode(truncated, sizeof(truncated), &failed, &error) == 0);
  CHECK(error.code != 0);
  CHECK(failed.pixels == nullptr);
  CHECK(failed.width == 0 && failed.height == 0);
  RideCodecFreeImage(&failed);

  const uint8_t not_ppm[] = {'B', 'M', 0, 0};
  CHECK(RideCodecProbe(not_ppm, sizeof(not_ppm)) == 0);
  return 0;
}
