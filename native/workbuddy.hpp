#pragma once
#include "cursor.hpp"
#include <cstdio>

namespace light::workbuddy {
inline void record(const fs::path& data,const Json& e) {
    auto id=field(e,"session_id"),event=field(e,"hook_event_name");
    auto agent=field(e,"agent_id");
    if(id.empty() || (!agent.empty() && agent!=id) || !field(e,"parent_session_id").empty())return;
    auto path=data/L"workbuddy-hooks-state.json";cursor::Lock lock(path);auto all=cursor::read(path);
    auto toolId=field(e,"tool_use_id");bool legacyId=toolId.empty();if(legacyId)toolId=field(e,"tool_name");
    Json normalized={{"conversation_id",id},{"tool_name","AskQuestion"},{"tool_use_id",toolId}};
    auto time=now();
    if(event=="UserPromptSubmit") {
        normalized["hook_event_name"]="beforeSubmitPrompt";normalized["generation_id"]=std::to_string(time);
        normalized["workspace_roots"]=Json::array({"WorkBuddy"});
    } else {
        if(!all.contains("sessions") || !all["sessions"].contains(id))return;
        normalized["generation_id"]=field(all["sessions"][id],"generation");
        if(event=="Stop"){normalized["hook_event_name"]="stop";normalized["status"]="completed";}
        else if(event=="SessionEnd"){normalized["hook_event_name"]="stop";normalized["status"]="aborted";}
        else if(event=="PreToolUse" && field(e,"tool_name")=="AskUserQuestion")normalized["hook_event_name"]="preToolUse";
        else if(event=="PermissionRequest")normalized["hook_event_name"]="preToolUse";
        else if(event=="Notification" && field(e,"notification_type")=="permission_prompt" && field(e,"message")=="needs your permission to use AskUserQuestion") {
            normalized["hook_event_name"]="preToolUse";toolId="AskUserQuestion";normalized["tool_use_id"]=toolId;legacyId=true;
        }
        else if(event=="PostToolUse" || event=="PostToolUseFailure")normalized["hook_event_name"]="postToolUse";
        else return;
    }
    if(legacyId && field(normalized,"hook_event_name")=="preToolUse")all["sessions"][id]["answered"].erase(toolId);
    if((event=="PostToolUse" || event=="PostToolUseFailure") && field(e,"tool_name")=="AskUserQuestion")
        all["sessions"][id]["pending"].erase("AskUserQuestion");
    if(cursor::consume(all,normalized,time))atomicWrite(path,all);
}
inline std::string bashQuote(const fs::path& path) {
    std::string out="'";for(char c:utf8(fs::absolute(path).generic_wstring()))out+=c=='\''?"'\\''":std::string(1,c);return out+"'";
}
inline bool configure(const fs::path& config,const fs::path& exe,const fs::path& data,bool remove=false) {
    cursor::Lock lock(config);auto root=cursor::read(config);auto previous=root;
    if(!root.contains("hooks"))root["hooks"]=Json::object();
    if(!root["hooks"].is_object())throw std::runtime_error("Invalid WorkBuddy hooks");
    auto command=bashQuote(exe)+" --workbuddy-light-hook-v1 --data-dir "+bashQuote(data);
    for(const char* event:{"UserPromptSubmit","Stop","SessionEnd","PreToolUse","PostToolUse","PostToolUseFailure","PermissionRequest","Notification"}) {
        auto& groups=root["hooks"][event];if(groups.is_null())groups=Json::array();
        if(!groups.is_array())throw std::runtime_error("Invalid WorkBuddy hook groups");
        for(auto g=groups.begin();g!=groups.end();) {
            if(!g->is_object() || !g->contains("hooks") || !(*g)["hooks"].is_array())throw std::runtime_error("Invalid WorkBuddy hook group");
            auto& hooks=(*g)["hooks"];
            for(auto it=hooks.begin();it!=hooks.end();)if(field(*it,"command").find(" --workbuddy-light-hook-v1 --data-dir ")!=std::string::npos)it=hooks.erase(it);else ++it;
            if(hooks.empty())g=groups.erase(g);else ++g;
        }
        if(!remove) {
            Json group={{"hooks",Json::array({{{"type","command"},{"command",command},{"timeout",5}}})}};
            if(std::string(event)=="PreToolUse")group["matcher"]="^AskUserQuestion$";
            if(std::string(event)=="Notification")group["matcher"]="^permission_prompt$";
            groups.push_back(group);
        }
        if(groups.empty())root["hooks"].erase(event);
    }
    if(root==previous)return false;
    std::error_code ec;if(fs::exists(config,ec)){auto backup=config;backup+=L".codexlight-"+std::to_wstring(now())+L".bak";fs::copy_file(config,backup);}
    atomicWrite(config,root);return true;
}
inline Time timestamp(const std::string& line) {
    unsigned y=0,m=0,d=0,h=0,n=0,s=0,ms=0;
    if(sscanf_s(line.c_str(),"[%u/%u/%u %u:%u:%u.%u]",&y,&m,&d,&h,&n,&s,&ms)!=7)return 0;
    if(y<2020 || y>2100 || m>12 || d>31 || h>23 || n>59 || s>59 || ms>999)return 0;
    SYSTEMTIME local{},utc{};local.wYear=(WORD)y;local.wMonth=(WORD)m;local.wDay=(WORD)d;
    local.wHour=(WORD)h;local.wMinute=(WORD)n;local.wSecond=(WORD)s;local.wMilliseconds=(WORD)ms;
    FILETIME file{};
    if(!TzSpecificLocalTimeToSystemTime(nullptr,&local,&utc) || !SystemTimeToFileTime(&utc,&file))return 0;
    ULARGE_INTEGER ticks;ticks.LowPart=file.dwLowDateTime;ticks.HighPart=file.dwHighDateTime;
    return (Time)(ticks.QuadPart/10000-11644473600000ULL);
}
class Lifecycle {
public:
    std::map<std::string,Candidate> candidates;
    bool damaged=false;
    void consume(const std::string& line,Time runtimeStart,const std::string& source) {
        const std::string marker="[Info] [SessionManager] task state transition ";
        auto at=line.find(marker);if(at==std::string::npos)return;
        auto time=timestamp(line);if(time<runtimeStart || !time)return;
        auto e=parse(line.substr(at+marker.size()));
        auto id=field(e,"sessionId"),signal=field(e,"signal"),to=field(e,"to");
        if(id.empty() || id.size()>256 || !e.contains("turnSeq") || !e["turnSeq"].is_number_integer()) {damaged=true;return;}
        auto seq=e["turnSeq"].get<long long>();if(seq<=0)return; // blank chats are also 'pending'
        auto found=candidates.find(id);
        if(found!=candidates.end() && found->second.state.updated>time)return;
        if(found!=candidates.end() && std::stoll(found->second.state.turn)>seq)return;
        std::string kind;
        if(to=="planning" || to=="working")kind="running";
        else if(to=="pending" && signal=="awaiting_input")kind="waiting";
        else if(to=="completed")kind="completed";
        else if(to=="terminated" || to=="error" || to=="failed")kind="interrupted";
        else if(to=="archived"){candidates.erase(id);return;}
        else {damaged=true;return;}
        if(found==candidates.end() && candidates.size()>=256) {
            auto oldest=candidates.begin();for(auto it=candidates.begin();it!=candidates.end();++it)if(it->second.state.updated<oldest->second.state.updated)oldest=it;
            candidates.erase(oldest);
        }
        auto& c=candidates[id];c.id=id;c.project="WorkBuddy";c.source=source;
        if(c.state.turn!=std::to_string(seq) || signal=="turn_started" || !c.state.started)c.state.started=time;
        c.state.turn=std::to_string(seq);c.state.kind=kind;c.state.updated=time;c.state.event=signal;c.state.pending=kind=="waiting"?1:0;
        c.state.reason=kind=="running"?"WorkBuddy 正在运行":kind=="waiting"?"WorkBuddy 等待回答或确认":kind=="completed"?"WorkBuddy 本轮已结束":"WorkBuddy 已停止或出错";
        damaged=false;
    }
};
class Log {
    uint64_t offset_=0,identity_=0;DWORD volume_=0;
    std::string partial_;
    bool initialized_=false,discard_=false;
public:
    bool catchingUp=false;
    Lifecycle lifecycle;
    void poll(const fs::path& path,Time runtimeStart) {
        HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("WorkBuddy log unavailable");
        struct Close{HANDLE h;~Close(){CloseHandle(h);}}close{file};
        BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(file,&info))throw std::runtime_error("WorkBuddy log identity");
        uint64_t identity=((uint64_t)info.nFileIndexHigh<<32)|info.nFileIndexLow;
        uint64_t size=((uint64_t)info.nFileSizeHigh<<32)|info.nFileSizeLow;
        constexpr uint64_t budget=8*1024*1024;
        if(!initialized_ || identity!=identity_ || volume_!=info.dwVolumeSerialNumber || size<offset_) {
            offset_=size>budget?size-budget:0;discard_=offset_!=0;partial_.clear();lifecycle=Lifecycle{};
            initialized_=true;identity_=identity;volume_=info.dwVolumeSerialNumber;
        }
        LARGE_INTEGER position;position.QuadPart=offset_;if(!SetFilePointerEx(file,position,nullptr,FILE_BEGIN))throw std::runtime_error("WorkBuddy log seek");
        uint64_t remaining=std::min<uint64_t>(size-offset_,budget);char block[65536];
        while(remaining) {
            DWORD got=0;if(!ReadFile(file,block,(DWORD)std::min<uint64_t>(remaining,sizeof(block)),&got,nullptr))throw std::runtime_error("WorkBuddy log read");
            if(!got)break;remaining-=got;offset_+=got;partial_.append(block,got);
            size_t used=0,end=0;
            while((end=partial_.find('\n',used))!=std::string::npos) {
                if(!discard_)lifecycle.consume(partial_.substr(used,end-used),runtimeStart,utf8(path.wstring()));
                discard_=false;used=end+1;
            }
            partial_.erase(0,used);
            if(partial_.size()>1024*1024){partial_.clear();discard_=true;lifecycle.damaged=true;}
        }
        catchingUp=offset_<size;
    }
};
class Engine {
    fs::path home_,data_;std::map<fs::path,Log> logs_;Selector selector_{0};Time lastScan_=0,runtimeStart_=0;
public:
    explicit Engine(fs::path home,fs::path data={}):home_(std::move(home)),data_(std::move(data)){}
    View tick(const Runtime& runtime,const std::string& manual="") {
        View view;
        if(!runtime.online){view.state.reason="等待 WorkBuddy 启动";return view;}
        if(runtime.started!=runtimeStart_){logs_.clear();lastScan_=0;runtimeStart_=runtime.started;selector_=Selector(0);}
        if(!lastScan_ || now()-lastScan_>=2000) {
            std::vector<fs::path> dates;std::error_code ec;
            for(fs::directory_iterator it(home_/L"logs",ec),end;it!=end && !ec;it.increment(ec))
                if(it->is_directory() && it->path().filename().wstring().size()==10)dates.push_back(it->path());
            if(ec)throw std::runtime_error("WorkBuddy logs unavailable");
            std::sort(dates.rbegin(),dates.rend());if(dates.size()>8)dates.resize(8);
            std::set<fs::path> paths;
            for(const auto& dir:dates)for(fs::directory_iterator it(dir,ec),end;it!=end && !ec;it.increment(ec)) {
                auto name=it->path().filename().wstring();
                if(name.find(L"workbuddyMainThread__")==0 && it->path().extension()==L".log" && paths.size()<32)paths.insert(it->path());
            }
            if(ec)throw std::runtime_error("WorkBuddy log discovery");
            for(auto it=logs_.begin();it!=logs_.end();)if(!paths.count(it->first))it=logs_.erase(it);else ++it;
            for(const auto& path:paths)logs_.try_emplace(path);
            lastScan_=now();
        }
        std::map<std::string,Candidate> merged;bool damaged=false,catchingUp=false;
        for(auto& pair:logs_) {
            pair.second.poll(pair.first,runtime.started);damaged|=pair.second.lifecycle.damaged;catchingUp|=pair.second.catchingUp;
            for(const auto& c:pair.second.lifecycle.candidates)if(!merged.count(c.first) || merged[c.first].state.updated<c.second.state.updated)merged[c.first]=c.second;
        }
        if(!data_.empty()) {
            auto all=cursor::read(data_/L"workbuddy-hooks-state.json");
            if(all.contains("sessions") && all["sessions"].is_object())for(auto it=all["sessions"].begin();it!=all["sessions"].end();++it) {
                const auto& s=it.value();auto time=s.value("updated",Time(0));
                if(s.value("started",Time(0))<runtime.started || (merged.count(it.key()) && merged[it.key()].state.updated>=time))continue;
                Candidate c;c.id=it.key();c.project="WorkBuddy";c.source="workbuddy-hooks";
                c.state.kind=field(s,"state");c.state.turn=field(s,"generation");c.state.event=field(s,"event");c.state.started=s.value("started",Time(0));c.state.updated=time;
                c.state.pending=s.contains("pending")?s["pending"].size():0;
                c.state.reason=c.state.kind=="running"?"WorkBuddy 正在运行":c.state.kind=="waiting"?"WorkBuddy 等待回答或确认":c.state.kind=="completed"?"WorkBuddy 本轮已结束":"WorkBuddy 已停止或出错";
                merged[c.id]=std::move(c);
            }
        }
        view.online=true;
        if(damaged || catchingUp){view.state.reason="WorkBuddy 日志不完整，等待有效状态";return view;}
        std::vector<Candidate> candidates;for(auto& c:merged)candidates.push_back(std::move(c.second));
        view=selector_.choose(candidates,now(),manual);view.online=true;
        if(candidates.empty())view.state.reason="等待发现 WorkBuddy 新任务";
        return view;
    }
};
}
