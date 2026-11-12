#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
# ifdef RIDE_CODEC_BUILD
#  define RIDE_CODEC_EXPORT __declspec(dllexport)
# else
#  define RIDE_CODEC_EXPORT __declspec(dllimport)
# endif
# define RIDE_CODEC_CALL __cdecl
#else
# define RIDE_CODEC_EXPORT
# define RIDE_CODEC_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define RIDE_CODEC_API_VERSION 1u
#define RIDE_CODEC_NAME_CAPACITY 64u
#define RIDE_CODEC_EXTENSIONS_CAPACITY 128u
typedef struct RideCodecInfo {
  uint32_t struct_size;
  uint32_t api_version;
  char name[RIDE_CODEC_NAME_CAPACITY];
  char extensions[RIDE_CODEC_EXTENSIONS_CAPACITY]; /* semicolon separated */
} RideCodecInfo;
typedef struct RideImage {
  uint32_t struct_size;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t pixel_format; /* 1 = RGBA8, top-down */
  uint8_t* pixels;
  size_t data_size;
} RideImage;
typedef struct RideCodecError {
  uint32_t code;
  size_t byte_offset;
  char message[160];
} RideCodecError;
typedef uint32_t (RIDE_CODEC_CALL *RideCodecGetInfoFn)(RideCodecInfo*);
typedef uint32_t (RIDE_CODEC_CALL *RideCodecProbeFn)(const uint8_t*, size_t);
typedef uint32_t (RIDE_CODEC_CALL *RideCodecDecodeFn)(const uint8_t*, size_t, RideImage*, RideCodecError*);
typedef void (RIDE_CODEC_CALL *RideCodecFreeImageFn)(RideImage*);
RIDE_CODEC_EXPORT uint32_t RIDE_CODEC_CALL RideCodecGetInfo(RideCodecInfo*);
RIDE_CODEC_EXPORT uint32_t RIDE_CODEC_CALL RideCodecProbe(const uint8_t*, size_t);
RIDE_CODEC_EXPORT uint32_t RIDE_CODEC_CALL RideCodecDecode(const uint8_t*, size_t, RideImage*, RideCodecError*);
RIDE_CODEC_EXPORT void RIDE_CODEC_CALL RideCodecFreeImage(RideImage*);
#ifdef __cplusplus
}
#endif
