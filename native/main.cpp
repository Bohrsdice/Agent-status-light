#include "core.hpp"
#include "doubao.hpp"
#include "cursor.hpp"
#include "workbuddy.hpp"
#include "deepseek.hpp"
#include "agent-windows.hpp"
#include "lamps.hpp"
#include "popup-menu.hpp"
#include <shellapi.h>
#include <shlobj.h>
#include <wbemidl.h>
#include <wrl/client.h>
#include <atomic>
#include <fstream>
#include <mutex>
#include <thread>
#include <tlhelp32.h>
#include <memory>

#pragma comment(lib,"user32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"oleaut32.lib")
#pragma comment(lib,"wbemuuid.lib")
#pragma comment(lib,"advapi32.lib")

using namespace light;
using Microsoft::WRL::ComPtr;
namespace {
struct BStr {
    BSTR p;
    explicit BStr(const wchar_t* s):p(SysAllocString(s)){}
    ~BStr(){SysFreeString(p);}
    operator BSTR() const {return p;}
};
struct Variant {
    VARIANT v;
    Variant(){VariantInit(&v);}
    ~Variant(){VariantClear(&v);}
};
class RuntimeProbe {
    bool doubao_,cursor_,workbuddy_,deepseek_;
    ComPtr<IWbemServices> service_;
    Time last_=0;
    Runtime value_;
public:
    explicit RuntimeProbe(bool doubao=false,bool cursor=false,bool workbuddy=false,bool deepseek=false):doubao_(doubao),cursor_(cursor),workbuddy_(workbuddy),deepseek_(deepseek){}
    Runtime poll() {
        if (last_ && now()-last_<5000) return value_;
        last_=now(); value_=Runtime{};
        if(cursor_ || workbuddy_ || deepseek_) {
            // Electron roots can be found without querying WMI command lines.
            // WMI can time out while an editor starts many language servers.
            const wchar_t* executable=deepseek_?L"DeepSeek Harness.exe":cursor_?L"Cursor.exe":L"WorkBuddy.exe";
            HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
            if(snapshot==INVALID_HANDLE_VALUE){value_.reason="无法读取应用进程";return value_;}
            std::map<DWORD,DWORD> processes;PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
            if(Process32FirstW(snapshot,&entry))do {
                if(_wcsicmp(entry.szExeFile,executable)==0)processes[entry.th32ProcessID]=entry.th32ParentProcessID;
            }while(Process32NextW(snapshot,&entry));
            CloseHandle(snapshot);int count=0;
            std::set<DWORD> windowOwners;
            EnumWindows([](HWND window,LPARAM context)->BOOL {
                if(IsWindowVisible(window) && !GetWindow(window,GW_OWNER)) {
                    DWORD pid=0;GetWindowThreadProcessId(window,&pid);
                    reinterpret_cast<std::set<DWORD>*>(context)->insert(pid);
                }
                return TRUE;
            },reinterpret_cast<LPARAM>(&windowOwners));
            bool hasWindow=false;
            for(const auto& process:processes)if(windowOwners.count(process.first))hasWindow=true;
            for(const auto& process:processes) {
                // Extensions may launch an orphaned Electron/Node worker with
                // the same executable name. Prefer the actual window owner;
                // minimized windows remain visible to EnumWindows.
                if(hasWindow && !windowOwners.count(process.first))continue;
                if(processes.count(process.second))continue;
                HANDLE handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,process.first);
                if(!handle)continue;
                FILETIME created,exited,kernel,user;
                if(GetProcessTimes(handle,&created,&exited,&kernel,&user)) {
                    ULARGE_INTEGER ticks;ticks.LowPart=created.dwLowDateTime;ticks.HighPart=created.dwHighDateTime;
                    value_.started=(Time)(ticks.QuadPart/10000-11644473600000ULL);++count;
                }
                CloseHandle(handle);
            }
            value_.online=count==1;
            value_.reason=count==1?"应用进程在线":count==0?"等待应用启动":"发现多个独立应用进程，暂不判断";
            return value_;
        }
        if (!service_) {
            ComPtr<IWbemLocator> locator;
            if (FAILED(CoCreateInstance(CLSID_WbemLocator,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&locator))) ||
                FAILED(locator->ConnectServer(BStr(L"ROOT\\CIMV2"),nullptr,nullptr,nullptr,0,nullptr,nullptr,&service_))) {
                value_.reason="无法读取 Codex 后台服务状态"; return value_;
            }
            CoSetProxyBlanket(service_.Get(),RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,nullptr,
                              RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE);
        }
        ComPtr<IEnumWbemClassObject> rows;
        if (FAILED(service_->ExecQuery(BStr(L"WQL"),BStr(workbuddy_?L"SELECT ProcessId,ParentProcessId,CommandLine FROM Win32_Process WHERE Name='WorkBuddy.exe'":cursor_?L"SELECT ProcessId,ParentProcessId,CommandLine FROM Win32_Process WHERE Name='Cursor.exe'":doubao_?L"SELECT ProcessId,ParentProcessId,CommandLine FROM Win32_Process WHERE Name='Doubao.exe'":L"SELECT ProcessId,ParentProcessId,CommandLine FROM Win32_Process WHERE Name='codex.exe'"),
                                      WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,&rows))) {
            service_.Reset(); value_.reason="无法读取 Codex 后台服务状态"; return value_;
        }
        int count=0,desktopCount=0;
        Time desktopStarted=0;
        while (true) {
            ComPtr<IWbemClassObject> row; ULONG fetched=0;
            HRESULT hr=rows->Next(2000,1,&row,&fetched);
            if (FAILED(hr) || hr==WBEM_S_TIMEDOUT) { value_.reason="Codex 服务查询超时"; return value_; }
            if (!fetched) break;
            Variant command,pid,parentPid;
            row->Get(L"CommandLine",0,&command.v,nullptr,nullptr);
            row->Get(L"ProcessId",0,&pid.v,nullptr,nullptr);
            row->Get(L"ParentProcessId",0,&parentPid.v,nullptr,nullptr);
            if (command.v.vt!=VT_BSTR || !command.v.bstrVal) continue;
            std::wstring line=command.v.bstrVal;
            if ((doubao_ || cursor_ || workbuddy_) && line.find(L"--type=")!=std::wstring::npos) continue;
            if (!doubao_ && !cursor_ && !workbuddy_ && line.find(L" app-server ")==std::wstring::npos &&
                !(line.size()>=11 && line.substr(line.size()-11)==L" app-server")) continue;
            DWORD process=pid.v.vt==VT_I4 ? (DWORD)pid.v.lVal : pid.v.ulVal;
            // Electron's Node workers can also be called Cursor.exe without
            // --type=. They are children of another Cursor process, not apps.
            if(cursor_ || workbuddy_) {
                DWORD parent=parentPid.v.vt==VT_I4?(DWORD)parentPid.v.lVal:parentPid.v.ulVal;
                HANDLE owner=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parent);
                bool child=false;
                if(owner) {wchar_t path[32768]{};DWORD length=32768;
                    child=QueryFullProcessImageNameW(owner,0,path,&length) && _wcsicmp(fs::path(path).filename().c_str(),workbuddy_?L"WorkBuddy.exe":L"Cursor.exe")==0;
                    CloseHandle(owner);}
                if(child)continue;
            }
            HANDLE handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,process);
            if (!handle) continue;
            FILETIME created,exited,kernel,user;
            if (GetProcessTimes(handle,&created,&exited,&kernel,&user)) {
                ULARGE_INTEGER ticks; ticks.LowPart=created.dwLowDateTime; ticks.HighPart=created.dwHighDateTime;
                value_.started=(Time)(ticks.QuadPart/10000-11644473600000ULL); ++count;
                // Tool runtimes may launch their own app-server. Prefer the
                // service owned directly by the desktop app when available.
                DWORD parent=parentPid.v.vt==VT_I4 ? (DWORD)parentPid.v.lVal : parentPid.v.ulVal;
                HANDLE owner=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parent);
                if (owner) {
                    wchar_t path[32768]{};DWORD length=32768;
                    if (QueryFullProcessImageNameW(owner,0,path,&length) &&
                        _wcsicmp(fs::path(path).filename().c_str(),L"ChatGPT.exe")==0) {
                        ++desktopCount;desktopStarted=value_.started;
                    }
                    CloseHandle(owner);
                }
            }
            CloseHandle(handle);
        }
        if (!doubao_ && !cursor_ && !workbuddy_ && desktopCount) {count=desktopCount;value_.started=desktopStarted;}
        value_.online=count==1;
        value_.reason=count==1 ? "Codex 后台服务在线" : count==0 ? "等待 Codex 启动" : "多个 Codex 后台服务，暂不判断";
        return value_;
    }
};

fs::path environment(const wchar_t* name) {
    DWORD count=GetEnvironmentVariableW(name,nullptr,0); if (!count) return {};
    std::wstring text(count,0); GetEnvironmentVariableW(name,text.data(),count); text.resize(count-1); return text;
}
fs::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR path=nullptr;
    if (FAILED(SHGetKnownFolderPath(id,0,nullptr,&path))) throw std::runtime_error("known folder");
    fs::path result(path); CoTaskMemFree(path); return result;
}
std::wstring stateName(const std::string& state) {
    if (state=="running") return L"运行中";
    if (state=="waiting") return L"等待确认";
    if (state=="completed") return L"本轮已结束";
    if (state=="idle") return L"当前未运行";
    if (state=="interrupted") return L"已中断";
    return L"未连接 / 待发现";
}
void appendBounded(const fs::path& path,const Json& value) {
    std::error_code ec;
    if (fs::exists(path,ec) && fs::file_size(path,ec)>256*1024) {
        auto previous=path; previous+=L".previous";
        MoveFileExW(path.c_str(),previous.c_str(),MOVEFILE_REPLACE_EXISTING);
    }
    std::ofstream out(path,std::ios::binary|std::ios::app); out<<value.dump()<<'\n';
}
Json readSmall(const fs::path& path) {
    std::error_code ec;
    if (fs::file_size(path,ec)>4096 || ec) return nullptr;
    std::ifstream in(path,std::ios::binary);
    return parse(std::string(std::istreambuf_iterator<char>(in),{}));
}

GUID stableIconGuid(const fs::path& data) {
    // Windows binds unsigned notification GUIDs to the executable path. Include
    // that path so moving this portable EXE creates a new valid identity.
    std::wstring executable(32768,L'\0');
    DWORD size=GetModuleFileNameW(nullptr,executable.data(),(DWORD)executable.size());
    executable.resize(size);
    auto name=executable+L"|"+fs::absolute(data).lexically_normal().wstring();
    uint64_t a=1469598103934665603ULL,b=0x9e3779b97f4a7c15ULL;
    for (wchar_t c:name) {auto n=(unsigned)towlower(c);a=(a^n)*1099511628211ULL;b=(b+n)*0x100000001b3ULL;}
    GUID id{};memcpy(&id,&a,8);memcpy(reinterpret_cast<BYTE*>(&id)+8,&b,8);
    id.Data3=(id.Data3&0x0fff)|0x8000;id.Data4[0]=(id.Data4[0]&0x3f)|0x80;
    return id;
}

struct App {
    fs::path home,data,hookData;
    bool isDoubao=false,isCursor=false,isWorkbuddy=false,isDeepseek=false;
    std::wstring agentName() const {return isDeepseek?L"DeepSeek":isWorkbuddy?L"WorkBuddy":isCursor?L"Cursor":isDoubao?L"豆包（测试）":L"Codex";}
    std::string agentKey() const {return isDeepseek?"deepseek":isWorkbuddy?"workbuddy":isCursor?"cursor":isDoubao?"doubao":"codex";}
    bool managed=false;
    std::atomic<bool> enabled{true},present{false};
    HWND window=nullptr;
    HANDLE stopEvent=nullptr;
    NOTIFYICONDATAW icon{};
    std::map<std::string,HICON> icons;
    UINT taskbarCreated=0;
    std::mutex mutex;
    View latest;
    std::string manual;
    std::atomic<bool> stopping{false};
    std::thread worker;
    Json previous, previousDisplay;
    Time lastWrite=0;
    bool registered=false;

    explicit App(fs::path h,fs::path d,HANDLE event,bool db=false,bool cur=false,bool wb=false,bool ds=false):home(std::move(h)),data(std::move(d)),hookData(data),isDoubao(db),isCursor(cur),isWorkbuddy(wb),isDeepseek(ds),stopEvent(event){}
    void work() {
        HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        {
            RuntimeProbe runtime(isDoubao,isCursor,isWorkbuddy,isDeepseek); Engine engine(home);doubao::Engine doubaoEngine(home);cursor::Engine cursorEngine(hookData,isCursor?home:fs::path{});workbuddy::Engine workbuddyEngine(home,hookData);deepseek::Engine deepseekEngine(home);
            bool configured=false;Time configureAttempt=0;
            while (!stopping) {
                std::string selection;
                { std::lock_guard<std::mutex> lock(mutex); selection.swap(manual); }
                View view;
                try {
                    if(managed && (!enabled || !present))view.state.reason=enabled?"等待应用启动":"已关闭此应用监控";
                    else {
                        if(managed && !configured && (isCursor || isWorkbuddy) && now()-configureAttempt>=10000) {
                            configureAttempt=now();
                            try {
                                wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
                                if(isCursor)cursor::configure(knownFolder(FOLDERID_Profile)/L".cursor"/L"hooks.json",executable,hookData,false);
                                else workbuddy::configure(home/L"settings.json",executable,hookData,false);
                                configured=true;
                            } catch(const std::exception&) {atomicWrite(data/L"hook-setup-error.json",{{"reason","无法配置回调，将重试"},{"time_ms",now()}});}
                        }
                        auto connection=runtime.poll();view=isDeepseek?deepseekEngine.tick(connection,selection):isWorkbuddy?workbuddyEngine.tick(connection,selection):isCursor?cursorEngine.tick(connection,selection):isDoubao?doubaoEngine.tick(selection,connection.online,connection.started):engine.tick(connection,selection);
                    }
                }
                catch (const std::exception&) { view.state.reason="读取任务失败，将自动重试"; }
                { std::lock_guard<std::mutex> lock(mutex); latest=std::move(view); }
                if (WaitForSingleObject(stopEvent,500)==WAIT_OBJECT_0) break;
            }
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }
    void makeIcons() {
        for(const auto& state:{"running","waiting","completed","idle","interrupted","unknown"}) {
            auto graphic=lamps::createIcon("circle",state);
            if(!graphic)throw std::runtime_error("icon");icons[state]=graphic;
        }
    }
    void update() {
        if (WaitForSingleObject(stopEvent,0)==WAIT_OBJECT_0) { DestroyWindow(window); return; }
        View view;
        { std::lock_guard<std::mutex> lock(mutex); view=latest; }
        auto raw=view.json();raw["agent"]=isDeepseek?"deepseek":isWorkbuddy?"workbuddy":isCursor?"cursor":isDoubao?"doubao":"codex"; Time time=now();
        bool changed=raw!=previous;
        try {
            if (changed || time-lastWrite>=2000) {
                auto output=raw; output["observed_at_ms"]=time; output["monitor_pid"]=GetCurrentProcessId(); output["implementation"]="native-cpp";
                atomicWrite(data/L"status.json",output);
                if (changed) appendBounded(data/L"transitions.jsonl",output);
                previous=raw; lastWrite=time;
            }
        } catch (const std::exception&) {}
        std::string state=view.state.kind;
        auto preview=readSmall(data/L"display-request.json");
        bool testing=false;
        if (preview.is_object() && field(preview,"action")=="test-yellow" && preview.contains("expires_at_ms") && preview["expires_at_ms"].is_number_integer()) {
            auto until=preview["expires_at_ms"].get<Time>(); testing=until>time && until-time<=10000;
        }
        if (testing) state="waiting";
        auto label=wide(view.project.empty()?view.id:utf8(fs::path(wide(view.project)).filename().wstring()));
        std::wstring tip=agentName()+L" · "+stateName(state)+L"\n"+(testing ? L"黄灯演示（8秒）" : label+L"\n"+wide(view.state.reason));
        if (tip.size()>127) tip.resize(127);
        Json display={{"state",state},{"is_test",testing},{"thread_id",view.id},{"tip",utf8(tip)},
                      {"display_mode","tray"}};
        bool show=!managed;display["visible"]=show;
        if (display!=previousDisplay || (show && !registered)) {
            if(show)icon.hIcon=icons.at(state); wcsncpy_s(icon.szTip,tip.c_str(),_TRUNCATE);
            if(!show){if(registered)Shell_NotifyIconW(NIM_DELETE,&icon);registered=false;}
            else if (registered) registered=Shell_NotifyIconW(NIM_MODIFY,&icon)!=FALSE;
            if (show && !registered) {
                registered=Shell_NotifyIconW(NIM_ADD,&icon)!=FALSE;
                if (registered) {icon.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&icon);}
            }
            previousDisplay=display;
            try {
                display["icon_registered"]=registered;
                wchar_t guid[40]{};StringFromGUID2(icon.guidItem,guid,40);display["icon_guid"]=utf8(guid);
                display["updated_at_ms"]=time;
                atomicWrite(data/L"tray-display.json",display);
                appendBounded(data/L"tray-display-events.jsonl",display);
            } catch (const std::exception&) {}
        }
    }
    void openAgent() {
        auto targets=agentWindows(agentKey());if(targets.empty())return;
        size_t index=0;
        if(targets.size()>1) {
            std::vector<popup::Item> rows;
            for(size_t i=0;i<std::min<size_t>(targets.size(),30);++i)
                rows.emplace_back(1+(UINT)i,targets[i].title.empty()?agentName():targets[i].title);
            POINT point;GetCursorPos(&point);UINT choice=popup::show(window,rows,point);
            if(!choice || choice>targets.size())return;index=choice-1;
        }
        bool result=activateAgentWindow(agentKey(),targets[index]);
        try {atomicWrite(data/L"activation.json",{{"agent",agentKey()},{"window",(uint64_t)(uintptr_t)targets[index].handle},
            {"process_id",targets[index].process},{"foreground_requested",result},{"time_ms",now()}});}catch(const std::exception&){}
    }
    void menu() {
        View view; {std::lock_guard<std::mutex> lock(mutex);view=latest;}
        std::vector<popup::Item> rows;
        rows.emplace_back(0,agentName()+L" · "+stateName(view.state.kind),false);
        rows.emplace_back(2,L"打开对应窗口");
        if(!view.choices.empty()) {
            rows.push_back(popup::Item::line());
            for(size_t i=0;i<std::min<size_t>(view.choices.size(),30);++i) {
                const auto& c=view.choices[i];auto project=fs::path(wide(c.project)).filename().wstring();
                rows.emplace_back(100+(UINT)i,stateName(c.state.kind)+L" · "+project+L" · "+wide(c.id.substr(0,8)),true,c.id==view.id);
            }
        }
        rows.push_back(popup::Item::line());rows.emplace_back(1,L"退出");
        POINT point;GetCursorPos(&point);UINT choice=popup::show(window,rows,point);
        if (choice==1) SetEvent(stopEvent);
        else if(choice==2)openAgent();
        else if (choice>=100 && choice-100<view.choices.size()) {
            std::lock_guard<std::mutex> lock(mutex);manual=view.choices[choice-100].id;
        }
    }
    static LRESULT CALLBACK procedure(HWND h,UINT message,WPARAM wp,LPARAM lp) {
        auto app=reinterpret_cast<App*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if (message==WM_NCCREATE) {
            app=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));
            app->window=h;
        }
        if (app) {
            try {
                if (message==WM_TIMER) {app->update();return 0;}
                if (message==WM_APP+1 && (LOWORD(lp)==WM_RBUTTONUP || LOWORD(lp)==WM_CONTEXTMENU)) {app->menu();return 0;}
                if (message==WM_APP+1 && (LOWORD(lp)==NIN_SELECT || LOWORD(lp)==NIN_KEYSELECT)) {app->openAgent();return 0;}
                if (message==app->taskbarCreated) {app->registered=false;app->update();return 0;}
                if (message==WM_CLOSE) {SetEvent(app->stopEvent);return 0;}
                if (message==WM_DESTROY) {if(!app->managed)PostQuitMessage(0);return 0;}
            } catch (const std::exception&) {}
        }
        return DefWindowProcW(h,message,wp,lp);
    }
    void start() {
        fs::create_directories(data);if(!managed)makeIcons();
        taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
        WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"CodexLight.Native.Window";
        if (!RegisterClassW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("window class");
        window=CreateWindowExW(0,cls.lpszClassName,L"",0,0,0,0,0,nullptr,nullptr,cls.hInstance,this);
        if (!window) throw std::runtime_error("window");
        icon.cbSize=sizeof(icon);icon.hWnd=window;icon.uID=1;
        icon.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP|NIF_GUID|NIF_SHOWTIP;icon.uCallbackMessage=WM_APP+1;
        icon.guidItem=stableIconGuid(data);
        worker=std::thread([this]{work();});
        update();SetTimer(window,1,500,nullptr);
    }
    void finish() {
        stopping=true;SetEvent(stopEvent);
        if (worker.joinable()) worker.join();
        KillTimer(window,1);Shell_NotifyIconW(NIM_DELETE,&icon);
        for (const auto& pair:icons) DestroyIcon(pair.second);
        icons.clear();if(IsWindow(window))DestroyWindow(window);
        try {auto output=latest.json();output["state"]="unknown";output["value"]=nullptr;output["reason"]="监控器已关闭";
             output["observed_at_ms"]=now();atomicWrite(data/L"status.json",output);} catch (const std::exception&) {}
    }
    ~App(){if(worker.joinable())finish();}
    int run() {
        start();MSG message{};
        while (GetMessageW(&message,nullptr,0,0)>0) {TranslateMessage(&message);DispatchMessageW(&message);}
        finish();return 0;
    }
};

#include "lamp-manager.hpp"

std::wstring instanceName(const fs::path& directory) {
    auto text=fs::absolute(directory).lexically_normal().wstring();
    unsigned long long hash=1469598103934665603ULL;
    for (wchar_t ch:text) {hash^=(unsigned)towlower(ch);hash*=1099511628211ULL;}
    return L"Local\\CodexLight.Native."+std::to_wstring(hash);
}
}

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    fs::path data;
    try {
        fs::path home=environment(L"CODEX_HOME");
        if (home.empty()) home=knownFolder(FOLDERID_Profile)/L".codex";
        data=knownFolder(FOLDERID_LocalAppData)/L"CodexLight";
        int count=0;auto args=CommandLineToArgvW(GetCommandLineW(),&count);
        bool quit=false,test=false,isDoubao=false,isCursor=false,isWorkbuddy=false,isDeepseek=false,isCodex=false,all=false,hook=false,wbHook=false,wbInstall=false,wbRemove=false,install=false,remove=false,explicitHome=false;fs::path snapshotPath;
        for (int i=1;i<count;++i) {
            std::wstring arg=args[i];
            if (arg==L"--home" && i+1<count) {home=args[++i];explicitHome=true;}
            else if (arg==L"--doubao") isDoubao=true;
            else if (arg==L"--cursor") isCursor=true;
            else if (arg==L"--workbuddy") isWorkbuddy=true;
            else if (arg==L"--deepseek") isDeepseek=true;
            else if (arg==L"--codex") isCodex=true;
            else if (arg==L"--all") all=true;
            else if (arg==L"--cursor-light-hook-v1") hook=true;
            else if (arg==L"--workbuddy-light-hook-v1") wbHook=true;
            else if (arg==L"--install-workbuddy") wbInstall=true;
            else if (arg==L"--uninstall-workbuddy") wbRemove=true;
            else if (arg==L"--install-cursor") install=true;
            else if (arg==L"--uninstall-cursor") remove=true;
            else if (arg==L"--data-dir" && i+1<count) data=args[++i];
            else if (arg==L"--snapshot" && i+1<count) snapshotPath=args[++i];
            else if (arg==L"--quit") quit=true;
            else if (arg==L"--test-yellow") test=true;
            else {LocalFree(args);throw std::runtime_error("invalid arguments");}
        }
        LocalFree(args);
        bool multi=all || (!isCodex && !isDoubao && !isCursor && !isWorkbuddy && !isDeepseek && !explicitHome && snapshotPath.empty());
        if(all && (isCodex || isDoubao || isCursor || isWorkbuddy || isDeepseek || explicitHome || !snapshotPath.empty()))throw std::runtime_error("--all cannot be combined with a single agent/home/snapshot");
        if(hook)return cursor::hook(data);
        if(wbHook)return cursor::hook(data,workbuddy::record,true);
        if((isWorkbuddy && !quit && !test && snapshotPath.empty()) || wbInstall || wbRemove) {
            wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
            workbuddy::configure((explicitHome?home:knownFolder(FOLDERID_Profile)/L".workbuddy")/L"settings.json",executable,data,wbRemove);
            if(wbInstall || wbRemove)return 0;
        }
        if((isCursor && !quit && !test && snapshotPath.empty()) || install || remove) {
            wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
            cursor::configure(knownFolder(FOLDERID_Profile)/L".cursor"/L"hooks.json",executable,data,remove);
            if(install || remove)return 0;
        }
        if(isWorkbuddy && !explicitHome)home=knownFolder(FOLDERID_Profile)/L".workbuddy";
        if(isDeepseek && !explicitHome){home=environment(L"DSH_HOME");if(home.empty())home=knownFolder(FOLDERID_Profile)/L".dsh";}
        if(isCursor && !explicitHome)home=knownFolder(FOLDERID_RoamingAppData)/L"Cursor";
        if(isDoubao && !explicitHome)home=knownFolder(FOLDERID_LocalAppData)/L"Doubao"/L"User Data"/L"Default"/L"IndexedDB"/L"chrome_doubao-chat_0.indexeddb.leveldb";
        auto name=instanceName(data),eventName=name+L".Stop";
        if (quit) {
            HANDLE event=OpenEventW(EVENT_MODIFY_STATE,FALSE,eventName.c_str());
            if (event) {SetEvent(event);CloseHandle(event);return 0;}
            return 1;
        }
        if (test) {atomicWrite(data/L"display-request.json",{{"action","test-yellow"},{"expires_at_ms",now()+8000}});return 0;}
        if (!snapshotPath.empty()) {
            CoInitializeEx(nullptr,COINIT_MULTITHREADED);View view;
            {RuntimeProbe probe(isDoubao,isCursor,isWorkbuddy,isDeepseek);Engine engine(home);doubao::Engine db(home);cursor::Engine cur(data,isCursor?home:fs::path{});workbuddy::Engine wb(home,data);deepseek::Engine ds(home);
             for(int i=0;i<40;++i) {auto connection=probe.poll();view=isDeepseek?ds.tick(connection):isWorkbuddy?wb.tick(connection):isCursor?cur.tick(connection):isDoubao?db.tick("",connection.online,connection.started):engine.tick(connection);if(view.state.reason!="正在发现已有任务")break;Sleep(50);}}
            atomicWrite(snapshotPath,view.json());CoUninitialize();return 0;
        }
        HANDLE mutex=CreateMutexW(nullptr,FALSE,name.c_str());
        if (!mutex || GetLastError()==ERROR_ALREADY_EXISTS) {if(mutex)CloseHandle(mutex);return 0;}
        HANDLE event=CreateEventW(nullptr,TRUE,FALSE,eventName.c_str());
        if (!event) {CloseHandle(mutex);throw std::runtime_error("event");}
        ResetEvent(event);
        CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        CoInitializeSecurity(nullptr,-1,nullptr,nullptr,RPC_C_AUTHN_LEVEL_DEFAULT,RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE,nullptr);
        int result=multi?MultiApp(data,event,home).run():App(home,data,event,isDoubao,isCursor,isWorkbuddy,isDeepseek).run();
        CoUninitialize();CloseHandle(event);CloseHandle(mutex);return result;
    } catch (const std::exception& error) {
        try {atomicWrite(data/L"startup-error.json",{{"error",error.what()},{"time_ms",now()}});} catch (...) {}
        MessageBoxW(nullptr,L"启动失败。请查看本机 AppData\\Local\\CodexLight\\startup-error.json。",L"Codex 红绿灯",MB_OK|MB_ICONERROR);
        return 1;
    }
}
