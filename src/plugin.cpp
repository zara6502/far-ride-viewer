#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>
#include "plugin.hpp"
#include "ride_codec_api.h"
static PluginStartupInfo g_far{};
static const GUID kPluginGuid={0x9bba2c10,0x2f13,0x4c41,{0x9a,0x77,0x52,0x49,0x44,0x45,0x56,0x31}};
static const GUID kMenuGuid={0x9bba2c11,0x2f13,0x4c41,{0x9a,0x77,0x52,0x49,0x44,0x45,0x56,0x31}};
struct Codec { HMODULE module{}; RideCodecInfo info{}; RideCodecProbeFn probe{}; RideCodecDecodeFn decode{}; RideCodecFreeImageFn release{}; };
static std::vector<Codec> g_codecs;
static std::wstring module_dir(){wchar_t p[MAX_PATH]{};GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase),p,MAX_PATH);std::wstring s=p;auto n=s.find_last_of(L"\\/");return n==s.npos?L".":s.substr(0,n);}
extern "C" IMAGE_DOS_HEADER __ImageBase;
static void load_codecs(){
 if(!g_codecs.empty())return;std::wstring dir=module_dir()+L"\\Codecs";WIN32_FIND_DATAW fd{};HANDLE f=FindFirstFileW((dir+L"\\*.dll").c_str(),&fd);if(f==INVALID_HANDLE_VALUE)return;
 do{HMODULE m=LoadLibraryW((dir+L"\\"+fd.cFileName).c_str());if(!m)continue;auto gi=(RideCodecGetInfoFn)GetProcAddress(m,"RideCodecGetInfo");auto pr=(RideCodecProbeFn)GetProcAddress(m,"RideCodecProbe");auto de=(RideCodecDecodeFn)GetProcAddress(m,"RideCodecDecode");auto re=(RideCodecFreeImageFn)GetProcAddress(m,"RideCodecFreeImage");if(!gi||!pr||!de||!re){FreeLibrary(m);continue;}Codec c{};c.module=m;c.info.struct_size=sizeof(c.info);if(!gi(&c.info)||c.info.api_version!=RIDE_CODEC_API_VERSION){FreeLibrary(m);continue;}c.probe=pr;c.decode=de;c.release=re;g_codecs.push_back(c);}while(FindNextFileW(f,&fd));FindClose(f);
}
static bool read_file(const std::wstring& p,std::vector<uint8_t>& b){std::ifstream f(p,std::ios::binary);if(!f)return false;b.assign(std::istreambuf_iterator<char>(f),{});return !b.empty();}
static std::wstring error_text(const RideCodecError& e){wchar_t b[256]{};swprintf(b,256,L"Codec error %u at byte %llu: %S",e.code,(unsigned long long)e.byte_offset,e.message);return b;}
static void view_file(const std::wstring& path){
 std::vector<uint8_t> bytes;if(!read_file(path,bytes)){MessageBoxW(nullptr,L"Cannot read file.",L"FarRideViewer",MB_ICONERROR);return;}load_codecs();Codec* chosen=nullptr;for(auto& c:g_codecs)if(c.probe(bytes.data(),bytes.size())){chosen=&c;break;}
 if(!chosen){MessageBoxW(nullptr,L"No installed codec recognizes this image.",L"FarRideViewer",MB_ICONWARNING);return;}
 RideImage image{};image.struct_size=sizeof(image);RideCodecError err{};if(!chosen->decode(bytes.data(),bytes.size(),&image,&err)){auto s=error_text(err);MessageBoxW(nullptr,s.c_str(),L"FarRideViewer",MB_ICONERROR);return;}
 if(image.width>32767||image.height>32767||image.pixel_format!=1||image.stride<image.width*4){chosen->release(&image);MessageBoxW(nullptr,L"Codec returned an unsupported image.",L"FarRideViewer",MB_ICONERROR);return;}
 BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=(LONG)image.width;bi.bmiHeader.biHeight=-(LONG)image.height;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;void* bits=nullptr;HBITMAP bmp=CreateDIBSection(nullptr,&bi,DIB_RGB_COLORS,&bits,nullptr,0);if(bmp&&bits){for(uint32_t y=0;y<image.height;y++){auto* d=(uint8_t*)bits+size_t(y)*image.width*4;auto* s=image.pixels+size_t(y)*image.stride;for(uint32_t x=0;x<image.width;x++){d[x*4]=s[x*4+2];d[x*4+1]=s[x*4+1];d[x*4+2]=s[x*4];d[x*4+3]=s[x*4+3];}}}
 chosen->release(&image);if(!bmp)return;
 HWND w=CreateWindowExW(WS_EX_APPWINDOW,L"STATIC",L"FarRideViewer",WS_OVERLAPPEDWINDOW|WS_VISIBLE,CW_USEDEFAULT,CW_USEDEFAULT,900,700,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 if(w){HDC dc=GetDC(w);HDC mem=CreateCompatibleDC(dc);auto old=SelectObject(mem,bmp);RECT r{};GetClientRect(w,&r);SetStretchBltMode(dc,HALFTONE);StretchBlt(dc,0,0,r.right,r.bottom,mem,0,0,(int)image.width,(int)image.height,SRCCOPY);SelectObject(mem,old);DeleteDC(mem);ReleaseDC(w,dc);MSG msg{};while(IsWindow(w)&&GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}}DeleteObject(bmp);
}
static bool current_path(std::wstring& p){if(!g_far.PanelControl)return false;PanelInfo pi{};pi.StructSize=sizeof(pi);if(!g_far.PanelControl(PANEL_ACTIVE,FCTL_GETPANELINFO,0,&pi)||pi.PanelType!=PTYPE_FILEPANEL)return false;FarGetPluginPanelItem it{};it.StructSize=sizeof(it);intptr_t n=g_far.PanelControl(PANEL_ACTIVE,FCTL_GETCURRENTPANELITEM,0,&it);if(n<=0)return false;std::vector<unsigned char>b((size_t)n);it.StructSize=sizeof(it);it.Size=b.size();it.Item=(PluginPanelItem*)b.data();if(!g_far.PanelControl(PANEL_ACTIVE,FCTL_GETCURRENTPANELITEM,0,&it)||!it.Item||!it.Item->FileName)return false;std::wstring name=it.Item->FileName;intptr_t z=g_far.PanelControl(PANEL_ACTIVE,FCTL_GETPANELDIRECTORY,0,nullptr);if(z<=0)return false;std::vector<unsigned char>d((size_t)z);auto* dir=(FarPanelDirectory*)d.data();dir->StructSize=sizeof(*dir);if(!g_far.PanelControl(PANEL_ACTIVE,FCTL_GETPANELDIRECTORY,z,dir)||!dir->Name)return false;p=dir->Name;if(!p.empty()&&p.back()!=L'\\')p+=L'\\';p+=name;return true;}
extern "C" __declspec(dllexport) void WINAPI GetGlobalInfoW(GlobalInfo* i){if(!i)return;i->StructSize=sizeof(*i);i->MinFarVersion=MAKEFARVERSION(3,0,0,0,VS_RELEASE);i->Version=MAKEFARVERSION(0,1,0,1,VS_ALPHA);i->Guid=kPluginGuid;i->Title=L"Far Ride Viewer";i->Description=L"Modular image viewer";i->Author=L"Far Ride Viewer";i->Instance=nullptr;}
extern "C" __declspec(dllexport) void WINAPI SetStartupInfoW(const PluginStartupInfo* i){if(i)g_far=*i;}
extern "C" __declspec(dllexport) void WINAPI GetPluginInfoW(PluginInfo* i){if(!i)return;static const wchar_t* names[]={L"Far Ride Viewer"};static const UUID ids[]={kMenuGuid};i->StructSize=sizeof(*i);i->Flags=PF_PRELOAD;i->PluginMenu.Guids=ids;i->PluginMenu.Strings=names;i->PluginMenu.Count=1;i->CommandPrefix=L"ride";i->DiskMenu={};i->PluginConfig={};i->Instance=nullptr;}
extern "C" __declspec(dllexport) HANDLE WINAPI OpenW(const OpenInfo* i){if(!i)return nullptr;if(i->OpenFrom==OPEN_PLUGINSMENU){std::wstring p;if(current_path(p))view_file(p);return INVALID_HANDLE_VALUE;}if(i->OpenFrom==OPEN_COMMANDLINE){auto*c=(const OpenCommandLineInfo*)i->Data;if(c&&c->CommandLine){std::wstring p=c->CommandLine;while(!p.empty()&&iswspace(p.front()))p.erase(p.begin());while(!p.empty()&&iswspace(p.back()))p.pop_back();if(p.size()>1&&p.front()==L'"'&&p.back()==L'"')p=p.substr(1,p.size()-2);view_file(p);}return INVALID_HANDLE_VALUE;}return nullptr;}
extern "C" __declspec(dllexport) intptr_t WINAPI ProcessConsoleInputW(ProcessConsoleInputInfo* i){if(!i||i->StructSize<sizeof(*i)||i->Rec.EventType!=KEY_EVENT||!i->Rec.Event.KeyEvent.bKeyDown||i->Rec.Event.KeyEvent.wVirtualKeyCode!=VK_F3)return 0;std::wstring p;if(!current_path(p))return 0;std::vector<uint8_t>b;if(!read_file(p,b))return 0;load_codecs();for(auto& c:g_codecs)if(c.probe(b.data(),b.size())){view_file(p);return 1;}return 0;}
