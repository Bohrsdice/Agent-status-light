#pragma once
#include "core.hpp"
#include <windowsx.h>
#include <memory>
#include <algorithm>
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"advapi32.lib")

namespace light::popup {
struct Item {
    UINT id=0;std::wstring text;
    bool enabled=true,checked=false,separator=false;
    HICON preview=nullptr;
    std::vector<Item> children;
    Item()=default;
    Item(UINT command,std::wstring label,bool active=true,bool selected=false,HICON graphic=nullptr):id(command),text(std::move(label)),enabled(active),checked(selected),preview(graphic){}
    static Item line(){Item value;value.separator=true;return value;}
    bool selectable() const{return enabled && !separator && (id || !children.empty());}
};
class Menu {
    struct Pane {
        Menu* menu;const std::vector<Item>* items;HWND window=nullptr;
        int level=0,hover=-1,parentRow=-1,scroll=0,content=0,width=0,height=0;
        std::vector<HWND> buttons;
    };
    HWND owner_;std::vector<std::unique_ptr<Pane>> panes_;HFONT font_=nullptr,small_=nullptr;
    int dpi_=96;bool dark_=false,done_=false,closing_=false;UINT result_=0;
    COLORREF background_,text_,muted_,disabled_,border_;
    int scale(int n) const{return MulDiv(n,dpi_,96);}
    int rowHeight(const Item& item) const{return scale(item.separator?8:26);}
    int itemTop(const Pane& pane,int index) const{int y=scale(6)-pane.scroll;for(int i=0;i<index;++i)y+=rowHeight((*pane.items)[i]);return y;}
    void fill(HDC dc,const RECT& rect,COLORREF color){auto brush=CreateSolidBrush(color);FillRect(dc,&rect,brush);DeleteObject(brush);}
    void round(HDC dc,const RECT& rect,COLORREF color,int radius){auto brush=CreateSolidBrush(color);auto oldBrush=SelectObject(dc,brush);auto oldPen=SelectObject(dc,GetStockObject(NULL_PEN));RoundRect(dc,rect.left,rect.top,rect.right,rect.bottom,radius,radius);SelectObject(dc,oldPen);SelectObject(dc,oldBrush);DeleteObject(brush);}
    void drawRow(Pane& pane,int index,HDC dc,RECT rect) {
        const auto& item=(*pane.items)[index];bool selected=pane.hover==index && item.selectable();
        fill(dc,rect,background_);if(selected)round(dc,rect,RGB(38,112,232),scale(9));
        SetBkMode(dc,TRANSPARENT);SelectObject(dc,font_);
        auto color=selected?RGB(255,255,255):item.enabled?text_:disabled_;SetTextColor(dc,color);
        RECT label=rect;label.left+=scale(8);label.right-=scale(10);
        if(item.checked || !item.children.empty() || item.preview)label.right-=scale(20);
        if(item.preview) {
            int side=scale(19),x=label.right-side;DrawIconEx(dc,x,rect.top+(rect.bottom-rect.top-side)/2,item.preview,side,side,0,nullptr,DI_NORMAL);label.right=x-scale(8);
        }
        DrawTextW(dc,item.text.c_str(),-1,&label,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        if(item.checked) {
            RECT check=rect;check.left=check.right-scale(27);check.right-=scale(7);
            DrawTextW(dc,L"✓",-1,&check,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }else if(!item.children.empty()) {
            int x=rect.right-scale(13),y=(rect.top+rect.bottom)/2;
            auto pen=CreatePen(PS_SOLID,scale(1),color);auto old=SelectObject(dc,pen);
            MoveToEx(dc,x-scale(2),y-scale(3),nullptr);LineTo(dc,x+scale(1),y);LineTo(dc,x-scale(2),y+scale(3));SelectObject(dc,old);DeleteObject(pen);
        }
    }
    void paint(Pane& pane) {
        PAINTSTRUCT ps;HDC dc=BeginPaint(pane.window,&ps);RECT client;GetClientRect(pane.window,&client);fill(dc,client,background_);
        auto pen=CreatePen(PS_SOLID,1,border_);auto oldPen=SelectObject(dc,pen);auto oldBrush=SelectObject(dc,GetStockObject(HOLLOW_BRUSH));
        RoundRect(dc,0,0,pane.width,pane.height,scale(18),scale(18));SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(pen);
        SaveDC(dc);IntersectClipRect(dc,scale(6),scale(6),pane.width-scale(6),pane.height-scale(6));
        for(int i=0;i<(int)pane.items->size();++i) {
            const auto& item=(*pane.items)[i];int y=itemTop(pane,i);
            if(item.separator){RECT line{scale(13),y+scale(4),pane.width-scale(13),y+scale(4)+1};fill(dc,line,border_);}
            else if(!item.id && item.children.empty()) {
                RECT label{scale(14),y,pane.width-scale(14),y+rowHeight(item)};
                SelectObject(dc,small_);SetTextColor(dc,muted_);SetBkMode(dc,TRANSPARENT);
                DrawTextW(dc,item.text.c_str(),-1,&label,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
            }
        }
        RestoreDC(dc,-1);EndPaint(pane.window,&ps);
    }
    void layout(Pane& pane) {
        for(int i=0;i<(int)pane.buttons.size();++i)if(pane.buttons[i]) {
            int y=itemTop(pane,i),height=rowHeight((*pane.items)[i]);
            SetWindowPos(pane.buttons[i],nullptr,scale(6),y,pane.width-scale(12),height,SWP_NOZORDER|SWP_NOACTIVATE);
            ShowWindow(pane.buttons[i],y>=scale(6) && y+height<=pane.height-scale(6)?SW_SHOWNOACTIVATE:SW_HIDE);
        }
        InvalidateRect(pane.window,nullptr,FALSE);
    }
    void trim(int count) {
        while((int)panes_.size()>count){auto pane=std::move(panes_.back());panes_.pop_back();if(IsWindow(pane->window))DestroyWindow(pane->window);}
    }
    void ensureVisible(Pane& pane,int row) {
        int y=itemTop(pane,row),height=rowHeight((*pane.items)[row]);
        if(y<scale(6))pane.scroll-=scale(6)-y;
        if(y+height>pane.height-scale(6))pane.scroll+=y+height-(pane.height-scale(6));
        pane.scroll=std::max(0,std::min(pane.scroll,pane.content-pane.height));layout(pane);
    }
    void openPane(const std::vector<Item>& items,POINT position,int parentRow=-1) {
        auto pane=std::make_unique<Pane>();pane->menu=this;pane->items=&items;pane->level=(int)panes_.size();pane->parentRow=parentRow;
        HDC dc=GetDC(owner_);auto oldFont=SelectObject(dc,font_);int width=scale(206),height=scale(12);
        for(const auto& item:items){SIZE size{};GetTextExtentPoint32W(dc,item.text.c_str(),(int)item.text.size(),&size);width=std::max(width,(int)size.cx+scale(item.preview?74:48));height+=rowHeight(item);}
        SelectObject(dc,oldFont);ReleaseDC(owner_,dc);pane->width=std::min(width,scale(360));pane->content=height;
        MONITORINFO monitor{sizeof(monitor)};GetMonitorInfoW(MonitorFromPoint(position,MONITOR_DEFAULTTONEAREST),&monitor);auto work=monitor.rcWork;
        pane->height=std::min(height,(int)(work.bottom-work.top)-scale(12));
        if(pane->level && position.x+pane->width>work.right){RECT parent;GetWindowRect(panes_.back()->window,&parent);position.x=parent.left-pane->width+scale(3);}
        position.x=std::max(work.left+scale(4),std::min(position.x,work.right-pane->width-scale(4)));
        position.y=std::max(work.top+scale(4),std::min(position.y,work.bottom-pane->height-scale(4)));
        auto raw=pane.get();panes_.push_back(std::move(pane));
        raw->window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,L"CodexLight.CompactMenu",L"红绿灯菜单",WS_POPUP|WS_CLIPCHILDREN,
            position.x,position.y,raw->width,raw->height,panes_.size()==1?owner_:panes_[0]->window,nullptr,GetModuleHandleW(nullptr),raw);
        if(!raw->window){done_=true;return;}
        SetWindowRgn(raw->window,CreateRoundRectRgn(0,0,raw->width+1,raw->height+1,scale(18),scale(18)),TRUE);
        for(int i=0;i<(int)items.size();++i) {
            HWND button=nullptr;
            if(!items[i].separator && (items[i].id || !items[i].children.empty())) {
                auto accessible=items[i].text+(items[i].checked?L"，已选中":L"");
                button=CreateWindowExW(0,L"BUTTON",accessible.c_str(),WS_CHILD|WS_VISIBLE|BS_OWNERDRAW|(items[i].enabled?0:WS_DISABLED),
                    0,0,0,0,raw->window,reinterpret_cast<HMENU>((INT_PTR)i+1),GetModuleHandleW(nullptr),nullptr);
                SendMessageW(button,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
            }
            raw->buttons.push_back(button);
        }
        layout(*raw);ShowWindow(raw->window,SW_SHOWNOACTIVATE);UpdateWindow(raw->window);
    }
    void hover(Pane& pane,int row,bool openChildren) {
        if(row<0 || row>=(int)pane.items->size() || !(*pane.items)[row].selectable())return;
        int previous=pane.hover;pane.hover=row;
        if(previous>=0 && pane.buttons[previous])InvalidateRect(pane.buttons[previous],nullptr,FALSE);
        if(pane.buttons[row])InvalidateRect(pane.buttons[row],nullptr,FALSE);
        bool sameChild=(int)panes_.size()>pane.level+1 && panes_[pane.level+1]->parentRow==row;
        if(sameChild)return;
        trim(pane.level+1);
        const auto& item=(*pane.items)[row];
        if(openChildren && !item.children.empty()) {
            RECT rect;GetWindowRect(pane.window,&rect);POINT point{rect.right-scale(3),rect.top+itemTop(pane,row)-scale(6)};openPane(item.children,point,row);
        }
    }
    void choose(Pane& pane,int row) {
        if(row<0 || row>=(int)pane.items->size())return;
        const auto& item=(*pane.items)[row];if(!item.selectable())return;
        if(!item.children.empty()){hover(pane,row,true);return;}
        result_=item.id;done_=true;
    }
    void pointer(bool click) {
        POINT point;GetCursorPos(&point);
        for(int p=(int)panes_.size()-1;p>=0;--p) {
            auto& pane=*panes_[p];RECT rect;GetWindowRect(pane.window,&rect);
            if(!PtInRect(&rect,point))continue;
            int local=point.y-rect.top;if(local<scale(6) || local>=pane.height-scale(6))return;
            for(int row=0;row<(int)pane.items->size();++row) {
                int top=itemTop(pane,row);if(local>=top && local<top+rowHeight((*pane.items)[row])){if(click)choose(pane,row);else hover(pane,row,true);return;}
            }
            return;
        }
        if(click)done_=true;
    }
    void key(UINT key) {
        if(panes_.empty())return;auto& pane=*panes_.back();
        if(key==VK_ESCAPE){done_=true;return;}
        if(key==VK_LEFT){if(panes_.size()>1)trim((int)panes_.size()-1);return;}
        if(key==VK_RETURN || key==VK_SPACE || key==VK_RIGHT){if(key==VK_RIGHT && (pane.hover<0 || (*pane.items)[pane.hover].children.empty()))return;int level=pane.level;choose(pane,pane.hover);if((int)panes_.size()>level+1){auto& child=*panes_.back();for(int row=0;row<(int)child.items->size();++row)if((*child.items)[row].selectable()){hover(child,row,false);break;}}return;}
        if(key==VK_UP || key==VK_DOWN || key==VK_HOME || key==VK_END) {
            int direction=key==VK_UP || key==VK_END?-1:1;
            int row=key==VK_HOME?-1:key==VK_END?(int)pane.items->size():pane.hover;
            for(size_t n=0;n<pane.items->size();++n){row=(row+direction+(int)pane.items->size())%(int)pane.items->size();if((*pane.items)[row].selectable()){hover(pane,row,false);ensureVisible(pane,row);return;}}
        }
    }
    static LRESULT CALLBACK procedure(HWND h,UINT message,WPARAM wp,LPARAM lp) {
        auto pane=reinterpret_cast<Pane*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(message==WM_NCCREATE){pane=static_cast<Pane*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);pane->window=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(pane));}
        if(pane){auto& menu=*pane->menu;
            if(message==WM_PAINT){menu.paint(*pane);return 0;}
            if(message==WM_ERASEBKGND)return 1;
            if(message==WM_DRAWITEM){auto draw=reinterpret_cast<DRAWITEMSTRUCT*>(lp);int row=(int)draw->CtlID-1;if(row>=0 && row<(int)pane->items->size())menu.drawRow(*pane,row,draw->hDC,draw->rcItem);return TRUE;}
            if(message==WM_COMMAND && HIWORD(wp)==BN_CLICKED){menu.choose(*pane,(int)LOWORD(wp)-1);return 0;}
            if(message==WM_MOUSEMOVE){menu.pointer(false);return 0;}
            if(message==WM_LBUTTONUP){menu.pointer(true);return 0;}
            if(message==WM_RBUTTONUP){menu.pointer(false);return 0;}
            if(message==WM_KEYDOWN){menu.key((UINT)wp);return 0;}
            if(message==WM_MOUSEWHEEL){auto& target=*menu.panes_.back();target.scroll=std::max(0,std::min(target.content-target.height,target.scroll-GET_WHEEL_DELTA_WPARAM(wp)*menu.scale(52)/WHEEL_DELTA));menu.layout(target);return 0;}
            if(message==WM_MOUSEACTIVATE)return pane->level?MA_NOACTIVATE:MA_ACTIVATE;
            if(message==WM_CANCELMODE || message==WM_CLOSE){menu.done_=true;return 0;}
            if(message==WM_CAPTURECHANGED && !menu.closing_){menu.done_=true;return 0;}
            if(message==WM_ACTIVATEAPP && !wp && !menu.closing_){menu.done_=true;return 0;}
            if(message==WM_DESTROY && pane->level==0)menu.done_=true;
        }
        return DefWindowProcW(h,message,wp,lp);
    }
public:
    explicit Menu(HWND owner):owner_(owner) {
        dpi_=(int)GetDpiForWindow(owner);if(!dpi_)dpi_=96;
        DWORD light=1,bytes=sizeof(light);RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",L"AppsUseLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&bytes);dark_=light==0;
        background_=dark_?RGB(39,40,44):RGB(249,249,251);text_=dark_?RGB(239,239,242):RGB(35,35,39);
        muted_=dark_?RGB(167,168,174):RGB(121,121,129);disabled_=dark_?RGB(104,105,112):RGB(178,178,185);border_=dark_?RGB(68,69,75):RGB(218,218,223);
        font_=CreateFontW(-scale(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        small_=CreateFontW(-scale(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    }
    ~Menu(){closing_=true;if(!panes_.empty() && GetCapture()==panes_[0]->window)ReleaseCapture();trim(0);DeleteObject(font_);DeleteObject(small_);}
    UINT show(const std::vector<Item>& items,POINT position) {
        WNDCLASSW cls{};cls.style=CS_DROPSHADOW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=procedure;cls.lpszClassName=L"CodexLight.CompactMenu";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        if(!RegisterClassW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return 0;
        openPane(items,position);if(done_ || panes_.empty())return 0;
        SetForegroundWindow(panes_[0]->window);SetFocus(panes_[0]->window);SetCapture(panes_[0]->window);
        MSG message{};while(!done_) {
            auto got=GetMessageW(&message,nullptr,0,0);if(got<=0){if(!got)PostQuitMessage((int)message.wParam);break;}
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        closing_=true;ReleaseCapture();trim(0);PostMessageW(owner_,WM_NULL,0,0);return result_;
    }
};
inline UINT show(HWND owner,const std::vector<Item>& items,POINT point){Menu menu(owner);return menu.show(items,point);}
}
