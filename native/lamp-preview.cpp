#include "lamps.hpp"
#include <fstream>
// Development-only contact sheet; renderer is exactly the one shipped in EXE.
int main(){
 const int width=750,height=640;std::vector<DWORD> canvas(width*height);
 const char* states[]={"running","waiting","completed","unknown"};
 for(int y=0;y<height;++y)for(int x=0;x<width;++x)canvas[y*width+x]=y<320?0xffeff1f5:0xff1d232b;
 for(int theme=0;theme<2;++theme)for(int row=0;row<4;++row)for(int column=0;column<5;++column) {
    int offset=0;for(int size:{16,24,32,48}) {
      auto pixels=light::lamps::renderIcon(light::lamps::styles()[column],states[row],size);
      int left=column*150+8+offset,top=theme*320+38+row*68+(48-size)/2;offset+=size+4;
      for(int y=0;y<size;++y)for(int x=0;x<size;++x){auto p=pixels[y*size+x],background=canvas[(top+y)*width+left+x];double a=(p>>24)/255.;
        DWORD value=0xff000000;for(int shift:{0,8,16})value|=(DWORD)(((p>>shift)&255)+((background>>shift)&255)*(1-a))<<shift;
        canvas[(top+y)*width+left+x]=value;}
    }
 }
 BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);file.bfSize=file.bfOffBits+(DWORD)canvas.size()*4;
 BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=width;info.biHeight=-height;info.biPlanes=1;info.biBitCount=32;
 std::ofstream out("lamp-preview.bmp",std::ios::binary);out.write((char*)&file,sizeof(file));out.write((char*)&info,sizeof(info));out.write((char*)canvas.data(),canvas.size()*4);return out?0:1;
}
