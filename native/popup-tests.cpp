#include "popup-menu.hpp"
#include <thread>
#include <iostream>
using namespace light;
int main(){int checks=0;auto check=[&](bool ok,const char* label){if(!ok)throw std::runtime_error(label);++checks;};
 HWND owner=CreateWindowExW(0,L"STATIC",L"Popup test owner",0,0,0,0,0,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 try{
  std::vector<popup::Item> items{{0,L"Header"},{11,L"Disabled",false},{12,L"First"},{0,L"Submenu"},{13,L"Last"}};
  items[3].children={{20,L"Disabled child",false},{21,L"Child one"},{22,L"Child two"}};
  auto run=[&](const std::vector<popup::Item>& data,std::vector<UINT> keys,bool invoke=false){
   std::thread input([&,keys,invoke]{HWND root=nullptr;for(int n=0;n<300 && !root;++n){for(HWND h=nullptr;(h=FindWindowExW(nullptr,h,L"CodexLight.CompactMenu",nullptr));){DWORD pid=0;GetWindowThreadProcessId(h,&pid);if(pid==GetCurrentProcessId() && GetWindow(h,GW_OWNER)==owner){root=h;break;}}Sleep(10);}
    if(!root){PostThreadMessageW(GetWindowThreadProcessId(owner,nullptr),WM_QUIT,0,0);return;}
    Sleep(50);if(invoke)PostMessageW(root,WM_COMMAND,MAKEWPARAM(3,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(root,3)));
    else for(auto key:keys){PostMessageW(root,WM_KEYDOWN,key,0);Sleep(15);}
    for(int n=0;n<200 && IsWindow(root);++n)Sleep(10);if(IsWindow(root))PostMessageW(root,WM_CLOSE,0,0);
   });
   auto result=popup::show(owner,data,{100,100});input.join();return result;
  };
  check(run(items,{VK_ESCAPE})==0,"Escape dismisses");
  check(run(items,{VK_HOME,VK_RETURN})==12,"Home skips header and disabled row");
  check(run(items,{VK_HOME,VK_RIGHT,VK_DOWN,VK_RIGHT,VK_DOWN,VK_RETURN})==22,"Right opens child only and child selection works");
  check(run(items,{VK_HOME,VK_DOWN,VK_RIGHT,VK_LEFT,VK_DOWN,VK_RETURN})==13,"Left returns to parent");
  check(run(items,{},true)==12,"native button invocation selects command");
  std::vector<popup::Item> many;for(UINT i=1;i<=100;++i)many.emplace_back(i,L"Long menu row");
  check(run(many,{VK_END,VK_RETURN})==100,"keyboard scrolls long menu to final item");
  check(run(items,{VK_END,VK_UP,VK_RIGHT,VK_ESCAPE})==0,"Escape closes nested menu");
  bool leaked=false;for(HWND h=nullptr;(h=FindWindowExW(nullptr,h,L"CodexLight.CompactMenu",nullptr));){DWORD pid=0;GetWindowThreadProcessId(h,&pid);if(pid==GetCurrentProcessId())leaked=true;}
  check(!leaked,"all popup windows released");DestroyWindow(owner);std::cout<<checks<<" popup interaction checks passed\n";return 0;
 }catch(const std::exception& e){DestroyWindow(owner);std::cerr<<e.what()<<'\n';return 1;}
}
