#include "ride_codec_api.h"
#include <cassert>
#include <cstdint>
#include <cstring>
int main(){
 const uint8_t sample[]={ 'q','o','i','f',0,0,0,1,0,0,0,1,4,0,0xfe,0xff,0x00,0x00,0,0,0,0,0,0,0,1 };
 RideCodecInfo info{};info.struct_size=sizeof(info);assert(RideCodecGetInfo(&info));assert(info.api_version==RIDE_CODEC_API_VERSION);assert(std::strcmp(info.name,"QOI")==0);assert(RideCodecProbe(sample,sizeof(sample)));
 RideImage image{};image.struct_size=sizeof(image);RideCodecError error{};assert(RideCodecDecode(sample,sizeof(sample),&image,&error));assert(image.width==1&&image.height==1&&image.stride==4&&image.data_size==4);assert(image.pixels[0]==255&&image.pixels[1]==0&&image.pixels[2]==0&&image.pixels[3]==255);RideCodecFreeImage(&image);assert(!image.pixels);
 uint8_t bad[sizeof(sample)];std::memcpy(bad,sample,sizeof(sample));bad[0]='x';RideImage invalid{};invalid.struct_size=sizeof(invalid);assert(!RideCodecDecode(bad,sizeof(bad),&invalid,&error));return 0;
}
