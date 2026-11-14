#pragma once
#include "core.hpp"
#include <dwmapi.h>
#pragma comment(lib,"dwmapi.lib")

namespace light {
struct AgentWindow { HWND handle=nullptr; DWORD process=0; std::wstring title; };
inline bool matchesAgentImage(const std::string& agent,const std::wstring& image) {
    if(agent!="codex" && agent!="cursor" && agent!="doubao" && agent!="workbuddy" && agent!="deepseek")return false;
    const wchar_t* wanted=agent=="cursor"?L"Cursor.exe":agent=="doubao"?L"Doubao.exe":
        agent=="workbuddy"?L"WorkBuddy.exe":agent=="deepseek"?L"DeepSeek Harness.exe":L"ChatGPT.exe";
    return _wcsicmp(image.c_str(),wanted)==0 || (agent=="codex" && _wcsicmp(image.c_str(),L"Codex.exe")==0);
}
inline bool agentOwnsWindow(const std::string& agent,HWND window,DWORD expected=0) {
    DWORD pid=0;GetWindowThreadProcessId(window,&pid);
    if(!pid || (expected && expected!=pid))return false;
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!process)return false;
    wchar_t path[32768]{};DWORD size=32768;
    bool found=QueryFullProcessImageNameW(process,0,path,&size) && matchesAgentImage(agent,fs::path(path).filename().wstring());
    CloseHandle(process);return found;
}
inline std::vector<AgentWindow> agentWindows(const std::string& agent) {
    struct Search {const std::string& agent;std::vector<AgentWindow> result;} search{agent,{}};
    EnumWindows([](HWND window,LPARAM value)->BOOL {
        auto& s=*reinterpret_cast<Search*>(value);
        if(!IsWindowVisible(window) || GetWindow(window,GW_OWNER) || (GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_TOOLWINDOW))return TRUE;
        DWORD cloaked=0;DwmGetWindowAttribute(window,DWMWA_CLOAKED,&cloaked,sizeof(cloaked));
        if(cloaked || !agentOwnsWindow(s.agent,window))return TRUE;
        wchar_t title[512]{};GetWindowTextW(window,title,512);
        DWORD pid=0;GetWindowThreadProcessId(window,&pid);
        s.result.push_back({window,pid,title});return TRUE;
    },reinterpret_cast<LPARAM>(&search));
    return search.result;
}
inline bool activateAgentWindow(const std::string& agent,const AgentWindow& target) {
    if(!IsWindow(target.handle) || !agentOwnsWindow(agent,target.handle,target.process))return false;
    if(IsIconic(target.handle))ShowWindowAsync(target.handle,SW_RESTORE);
    HWND popup=GetLastActivePopup(target.handle);
    HWND chosen=popup && IsWindowVisible(popup) && agentOwnsWindow(agent,popup,target.process)?popup:target.handle;
    bool activated=SetForegroundWindow(chosen)!=FALSE;
    if(!activated){FLASHWINFO flash{sizeof(flash),target.handle,FLASHW_TRAY,2,0};FlashWindowEx(&flash);}
    return activated;
}
}
