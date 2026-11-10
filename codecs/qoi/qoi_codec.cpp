#include "ride_codec_api.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <vector>
namespace {
static uint32_t be32(const uint8_t* p) { return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3]; }
static uint8_t hash(uint8_t r,uint8_t g,uint8_t b,uint8_t a){return uint8_t((r*3u+g*5u+b*7u+a*11u)&63u);}
static void fail(RideCodecError* e,uint32_t c,size_t off,const char* m){if(!e)return;e->code=c;e->byte_offset=off;std::snprintf(e->message,sizeof(e->message),"%s",m);}
}
extern "C" uint32_t RIDE_CODEC_CALL RideCodecGetInfo(RideCodecInfo* i) {
 if(!i || i->struct_size<sizeof(RideCodecInfo)) return 0;
 i->api_version=RIDE_CODEC_API_VERSION; std::snprintf(i->name,sizeof(i->name),"QOI"); std::snprintf(i->extensions,sizeof(i->extensions),"qoi"); return 1;
}
extern "C" uint32_t RIDE_CODEC_CALL RideCodecProbe(const uint8_t* p,size_t n){return p&&n>=4&&std::memcmp(p,"qoif",4)==0;}
extern "C" uint32_t RIDE_CODEC_CALL RideCodecDecode(const uint8_t* d,size_t n,RideImage* out,RideCodecError* e) {
 if(e){e->code=0;e->byte_offset=0;e->message[0]=0;}
 if(!out || out->struct_size<sizeof(RideImage)){fail(e,1,0,"Invalid output structure");return 0;}
 out->pixels=nullptr;out->data_size=0;out->width=out->height=out->stride=out->pixel_format=0;
 if(!d||n<22||std::memcmp(d,"qoif",4)){fail(e,2,0,"Invalid QOI header");return 0;}
 const uint32_t w=be32(d+4),h=be32(d+8);const uint8_t channels=d[12],cs=d[13];
 if(!w||!h||w>0x7fffffffU||h>0x7fffffffU||(channels!=3&&channels!=4)||cs>1){fail(e,3,4,"Invalid QOI dimensions or header fields");return 0;}
 const uint64_t count64=uint64_t(w)*h;if(count64>SIZE_MAX/4){fail(e,4,4,"Image is too large");return 0;}
 if(std::memcmp(d+n-8,"\0\0\0\0\0\0\0\1",8)){fail(e,5,n-8,"Invalid QOI end marker");return 0;}
 const size_t count=size_t(count64),bytes=count*4;uint8_t* pixels=new(std::nothrow) uint8_t[bytes];if(!pixels){fail(e,6,0,"Out of memory");return 0;}
 uint8_t index[64][4]={};uint8_t r=0,g=0,b=0,a=255;size_t p=14,run=0;const size_t end=n-8;
 for(size_t px=0;px<count;++px){
  if(run) --run; else {
   if(p>=end){delete[] pixels;fail(e,7,p,"Unexpected end of QOI stream");return 0;}
   uint8_t op=d[p++];
   if(op==0xfe){if(end-p<3){delete[] pixels;fail(e,8,p-1,"Truncated RGB opcode");return 0;}r=d[p++];g=d[p++];b=d[p++];}
   else if(op==0xff){if(end-p<4){delete[] pixels;fail(e,9,p-1,"Truncated RGBA opcode");return 0;}r=d[p++];g=d[p++];b=d[p++];a=d[p++];}
   else switch(op&0xc0){
    case 0x00:{auto*q=index[op&63];r=q[0];g=q[1];b=q[2];a=q[3];break;}
    case 0x40:r=uint8_t(int(r)+int((op>>4)&3)-2);g=uint8_t(int(g)+int((op>>2)&3)-2);b=uint8_t(int(b)+int(op&3)-2);break;
    case 0x80:{if(p>=end){delete[] pixels;fail(e,10,p-1,"Truncated LUMA opcode");return 0;}uint8_t q=d[p++];int dg=int(op&63)-32,dr=int(q>>4)-8,db=int(q&15)-8;g=uint8_t(int(g)+dg);r=uint8_t(int(r)+dg+dr);b=uint8_t(int(b)+dg+db);break;}
    case 0xc0:run=op&63;if(run>count-px-1){delete[] pixels;fail(e,11,p-1,"RUN exceeds image");return 0;}break;
   }
  }
  uint8_t* slot=index[hash(r,g,b,a)];slot[0]=r;slot[1]=g;slot[2]=b;slot[3]=a;
  size_t o=px*4;pixels[o]=r;pixels[o+1]=g;pixels[o+2]=b;pixels[o+3]=a;
 }
 if(p!=end||run){delete[] pixels;fail(e,12,p,"Unexpected trailing QOI data");return 0;}
 out->width=w;out->height=h;out->stride=w*4;out->pixel_format=1;out->pixels=pixels;out->data_size=bytes;return 1;
}
extern "C" void RIDE_CODEC_CALL RideCodecFreeImage(RideImage* i){if(!i)return;delete[] i->pixels;i->pixels=nullptr;i->data_size=0;}
