#pragma once
#include "core.hpp"
#include "cursor-native.hpp"
#include <fstream>

namespace light::cursor {
inline Json read(const fs::path& path, size_t limit=1024*1024) {
    std::error_code ec;
    if (!fs::exists(path,ec)) return Json::object();
    if (fs::file_size(path,ec)>limit || ec) throw std::runtime_error("Cursor file too large");
    std::ifstream in(path,std::ios::binary);
    auto result=parse(std::string(std::istreambuf_iterator<char>(in),{}));
    if (!result.is_object()) throw std::runtime_error("Invalid Cursor JSON");
    return result;
}
inline std::wstring lockName(const fs::path& path) {
    uint64_t h=1469598103934665603ULL;
    for(auto c:fs::absolute(path).lexically_normal().wstring()) h=(h^towlower(c))*1099511628211ULL;
    return L"Local\\CodexLight.Cursor."+std::to_wstring(h);
}
struct Lock {
    HANDLE handle;
    explicit Lock(const fs::path& path):handle(CreateMutexW(nullptr,FALSE,lockName(path).c_str())) {
        auto result=handle?WaitForSingleObject(handle,1500):WAIT_FAILED;
        if(result!=WAIT_OBJECT_0 && result!=WAIT_ABANDONED) {
            if(handle)CloseHandle(handle);handle=nullptr;throw std::runtime_error("Cursor lock timeout");
        }
    }
    ~Lock(){if(handle){ReleaseMutex(handle);CloseHandle(handle);}}
};
// Only metadata is retained. Prompt, tool arguments/results and email never reach disk.
inline bool consume(Json& all,const Json& e,Time time) {
    auto id=field(e,"conversation_id"),gen=field(e,"generation_id"),event=field(e,"hook_event_name");
    if(id.empty() || id.size()>256 || gen.size()>256) return false;
    if(!all.contains("sessions"))all["sessions"]=Json::object();
    auto& sessions=all["sessions"];
    if(!sessions.is_object())throw std::runtime_error("Invalid Cursor state");
    if(event=="beforeSubmitPrompt") {
        if(gen.empty())return false;
        if(sessions.contains(id) && field(sessions[id],"generation")==gen)return false;
        if(!sessions.contains(id) && sessions.size()>=128) {
            auto oldest=sessions.end();
            for(auto it=sessions.begin();it!=sessions.end();++it)
                if(oldest==sessions.end() || it.value().value("updated",Time(0))<oldest.value().value("updated",Time(0)))oldest=it;
            sessions.erase(oldest);
        }
        std::string project;
        if(e.contains("workspace_roots") && e["workspace_roots"].is_array() && !e["workspace_roots"].empty() && e["workspace_roots"][0].is_string())
            project=e["workspace_roots"][0].get<std::string>().substr(0,4096);
        sessions[id]={{"generation",gen},{"state","running"},{"started",time},{"updated",time},
                      {"event",event},{"project",project},{"pending",Json::object()},{"answered",Json::object()}};
        return true;
    }
    if(!sessions.contains(id))return false;
    auto& s=sessions[id];
    // No generation means we cannot safely associate an out-of-order callback.
    if(gen.empty() || field(s,"generation")!=gen)return false;
    auto state=field(s,"state");
    if(state!="running" && state!="waiting")return false;
    if(event=="stop" || event=="sessionEnd") {
        auto status=field(e,event=="stop"?"status":"reason");
        if(status=="completed")s["state"]="completed";
        else if(status=="aborted" || status=="error" || status=="window_close" || status=="user_close")s["state"]="interrupted";
        else return false;
        s["pending"]=Json::object();s["answered"]=Json::object();
    } else if(field(e,"tool_name")=="AskQuestion") {
        auto tool=field(e,"tool_use_id");
        if(tool.empty() || tool.size()>256)return false;
        if(event=="preToolUse") {
            // Async cards do not mean the agent is blocked; their answer callback
            // is not exposed by the public hook contract. Do not guess yellow.
            auto input=e.value("tool_input",Json::object());
            if(!input.is_object() || input.value("run_async",false) || input.value("runAsync",false))return false;
            if(s["answered"].contains(tool))return false;
            if(s["pending"].size()>=32)return false;
            s["pending"][tool]=true;
        } else if(event=="postToolUse" || event=="postToolUseFailure") {
            s["pending"].erase(tool);
            if(s["answered"].size()<128)s["answered"][tool]=true;
        } else return false;
        s["state"]=s["pending"].empty()?"running":"waiting";
    } else return false;
    s["updated"]=time;s["event"]=event;
    return true;
}
inline void record(const fs::path& data,const Json& event) {
    auto path=data/L"cursor-state.json";Lock lock(path);
    auto all=read(path);
    bool applied=consume(all,event,now());
    if(applied)atomicWrite(path,all);
    Json diagnostic={{"received_at_ms",now()},{"applied",applied}};
    for(const char* key:{"hook_event_name","conversation_id","generation_id","status","reason","tool_name","tool_use_id"})
        diagnostic[key]=field(event,key).substr(0,256);
    auto log=data/L"cursor-hooks.jsonl";std::error_code ec;
    if(fs::exists(log,ec) && fs::file_size(log,ec)>128*1024){auto previous=log;previous+=L".previous";MoveFileExW(log.c_str(),previous.c_str(),MOVEFILE_REPLACE_EXISTING);}
    std::ofstream out(log,std::ios::binary|std::ios::app);out<<diagnostic.dump()<<'\n';out.flush();
    if(!out)throw std::runtime_error("Cursor event log write failed");
}
inline int hook(const fs::path& data,void(*save)(const fs::path&,const Json&)=record,bool silent=false) noexcept {
    Json event;
    try {
        std::string input;char buffer[8192];DWORD got=0;
        HANDLE handle=GetStdHandle(STD_INPUT_HANDLE);
        while(ReadFile(handle,buffer,sizeof(buffer),&got,nullptr) && got) {
            if(input.size()+got>8*1024*1024) return 1; // fail open, never block an agent
            input.append(buffer,got);
        }
        event=parse(input);
        if(!event.is_object())return 1;
    } catch(...) {return 1;}
    try{save(data,event);}catch(const std::exception& e){
        // Keep the hook fail-open, but make storage failures diagnosable.
        std::string error=std::string("CodexLight: ")+e.what()+"\n";DWORD written=0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE),error.data(),(DWORD)error.size(),&written,nullptr);
    }catch(...){} // monitor errors must not break the agent
    if(silent)return 0;
    std::string response="{}\n";
    auto eventName=field(event,"hook_event_name");
    if(eventName=="beforeSubmitPrompt")response="{\"continue\":true}\n";
    if(eventName=="preToolUse")response="{\"permission\":\"allow\"}\n";
    DWORD written=0;
    return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),response.data(),(DWORD)response.size(),&written,nullptr) && written==response.size()?0:1;
}
inline std::string quote(const fs::path& path) {
    std::string out="'";
    for(char c:utf8(fs::absolute(path).wstring())) {out+=c;if(c=='\'')out+='\'';}
    return out+"'";
}
inline bool configure(const fs::path& config,const fs::path& exe,const fs::path& data,bool remove=false) {
    Lock lock(config);auto root=read(config);auto previous=root;
    if(root.empty())root={{"version",1},{"hooks",Json::object()}};
    if(root.value("version",1)!=1)throw std::runtime_error("Unsupported Cursor hooks version");
    if(!root.contains("hooks"))root["hooks"]=Json::object();
    if(!root["hooks"].is_object())throw std::runtime_error("Invalid Cursor hooks");
    // Windows PowerShell 5 may pipe native input using a non-UTF-8 code page.
    // A script block owns the incoming pipeline and explicitly re-encodes it.
    // Out-String also forces PowerShell to wait for this Windows-subsystem EXE.
    auto command="& { $global:OutputEncoding = New-Object System.Text.UTF8Encoding $false; [Console]::OutputEncoding = $global:OutputEncoding; $input | & "+quote(exe)+" --cursor-light-hook-v1 --data-dir "+quote(data)+" | Out-String }";
    for(const char* event:{"beforeSubmitPrompt","stop","sessionEnd","preToolUse","postToolUse","postToolUseFailure"}) {
        auto& hooks=root["hooks"][event];
        if(hooks.is_null())hooks=Json::array();
        if(!hooks.is_array())throw std::runtime_error("Invalid Cursor hook array");
        for(auto it=hooks.begin();it!=hooks.end();) {
            if(field(*it,"command").find(" --cursor-light-hook-v1 --data-dir ")!=std::string::npos)it=hooks.erase(it);else ++it;
        }
        if(!remove) {
            Json entry={{"command",command},{"timeout",5}};
            if(std::string(event).find("ToolUse")!=std::string::npos)entry["matcher"]="^AskQuestion$";
            hooks.push_back(entry);
        }
        if(hooks.empty())root["hooks"].erase(event);
    }
    if(root==previous)return false;
    std::error_code ec;
    if(fs::exists(config,ec)) {
        auto backup=config;backup+=L".codexlight-"+std::to_wstring(now())+L".bak";
        fs::copy_file(config,backup); // preserve exact original before changing config
    }
    atomicWrite(config,root);return true;
}
class Engine {
    fs::path data_;
    bool native_=false;
    cursorNative::Logs logs_;
    cursorNative::Questions questions_;
    Json nativeState_=Json::object();Time runtimeStart_=0;
    Selector selector_{0};
public:
    explicit Engine(fs::path data,fs::path cursorRoot={}):data_(std::move(data)),native_(!cursorRoot.empty()),logs_(cursorRoot),questions_(cursorRoot){}
    View tick(const Runtime& runtime,const std::string& manual="") {
        View view;
        if(!runtime.online){view.state.reason="Cursor："+runtime.reason;return view;}
        auto all=read(data_/L"cursor-state.json");std::vector<Candidate> candidates;
        if(native_) {
            if(runtimeStart_!=runtime.started){nativeState_=Json::object();runtimeStart_=runtime.started;selector_=Selector(0);}
            for(const auto& event:logs_.poll()) {
                if(event.time<runtime.started)continue;
                auto id=field(event.value,"conversation_id");
                if(nativeState_.contains("sessions") && nativeState_["sessions"].contains(id) &&
                   nativeState_["sessions"][id].value("updated",Time(0))>event.time)continue;
                consume(nativeState_,event.value,event.time);
            }
            if(nativeState_.contains("sessions"))for(auto it=nativeState_["sessions"].begin();it!=nativeState_["sessions"].end();++it) {
                if(!all.contains("sessions"))all["sessions"]=Json::object();
                auto& sessions=all["sessions"];
                if(!sessions.contains(it.key()) || sessions[it.key()].value("updated",Time(0))<=it.value().value("updated",Time(0))) {
                    sessions[it.key()]=it.value();sessions[it.key()]["native_log"]=true;
                }
            }
        }
        if(all.contains("sessions") && all["sessions"].is_object())for(auto it=all["sessions"].begin();it!=all["sessions"].end();++it) {
            const auto& s=it.value();Candidate c;c.id=it.key();c.project=field(s,"project");c.source="cursor-hooks";
            c.state.kind=field(s,"state");c.state.turn=field(s,"generation");c.state.event=field(s,"event");
            c.state.started=s.value("started",Time(0));c.state.updated=s.value("updated",Time(0));
            if(c.state.started<runtime.started)continue;
            c.state.pending=s.contains("pending")?s["pending"].size():0;
            if(s.value("native_log",false))c.source="cursor-native-log";
            if(native_ && c.state.active()) {
                c.state.pending=std::max(c.state.pending,questions_.pending(c.id,c.state.turn));
                if(c.state.pending){c.state.kind="waiting";c.state.event="native_question_pending";c.source+="+sqlite";}
            }
            c.state.reason=c.state.kind=="waiting"?"Cursor 原生提问等待回答":c.state.kind=="running"?"Cursor 正在运行":c.state.kind=="completed"?"Cursor 本轮已结束":"Cursor 已取消或出错";
            candidates.push_back(std::move(c));
        }
        view=selector_.choose(candidates,now(),manual);view.online=true;
        if(candidates.empty())view.state.reason="等待 Cursor 发送新任务（Hooks）";
        return view;
    }
};
}
