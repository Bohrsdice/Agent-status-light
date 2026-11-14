// Development-only ICO generator. No GDI+ dependency in the shipped EXE.
#include "lamps.hpp"
#include <gdiplus.h>
#include <objidl.h>
#include <fstream>
#pragma comment(lib,"gdiplus.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"user32.lib")
static void little(std::ofstream& out,uint32_t n,int bytes){for(int i=0;i<bytes;++i)out.put(char(n>>(8*i)));}
int wmain(int argc,wchar_t** argv){if(argc!=2)return 1;ULONG_PTR token;Gdiplus::GdiplusStartupInput input;if(Gdiplus::GdiplusStartup(&token,&input,nullptr)!=Gdiplus::Ok)return 1;
 const int sizes[]={16,24,32,48,64,128,256};std::vector<std::vector<char>> frames;
 CLSID png={0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
 for(int size:sizes){auto pixels=light::lamps::renderIcon("glass","idle",size,RGB(14,190,175));
  for(auto& p:pixels){unsigned a=p>>24;if(a){DWORD straight=a<<24;for(int shift:{0,8,16})straight|=std::min<DWORD>(255,((p>>shift)&255)*255/a)<<shift;p=straight;}}
  Gdiplus::Bitmap bitmap(size,size,size*4,PixelFormat32bppARGB,reinterpret_cast<BYTE*>(pixels.data()));IStream* stream=nullptr;
  if(FAILED(CreateStreamOnHGlobal(nullptr,TRUE,&stream)))return 1;
  if(bitmap.Save(stream,&png,nullptr)!=Gdiplus::Ok){stream->Release();return 1;}
  STATSTG stat{};stream->Stat(&stat,STATFLAG_NONAME);HGLOBAL memory;GetHGlobalFromStream(stream,&memory);auto bytes=static_cast<char*>(GlobalLock(memory));frames.emplace_back(bytes,bytes+stat.cbSize.LowPart);GlobalUnlock(memory);stream->Release();
 }
 Gdiplus::GdiplusShutdown(token);std::ofstream out(argv[1],std::ios::binary);little(out,0,2);little(out,1,2);little(out,7,2);uint32_t offset=6+7*16;
 for(int i=0;i<7;++i){little(out,sizes[i]==256?0:sizes[i],1);little(out,sizes[i]==256?0:sizes[i],1);little(out,0,2);little(out,1,2);little(out,32,2);little(out,(uint32_t)frames[i].size(),4);little(out,offset,4);offset+=(uint32_t)frames[i].size();}
 for(const auto& frame:frames)out.write(frame.data(),frame.size());return out?0:1;
}
