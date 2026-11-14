#pragma once
#include "core.hpp"
#include "vendor/zstd.h"
#include "vendor/zstd_errors.h"
#include <algorithm>

namespace light::deepseek {
class Lifecycle {
    long long sequence_=-1,turn_=0;bool seeded_=false;
    std::set<std::string> questions_,approvals_;
    void pending(){state.pending=questions_.size()+approvals_.size();state.kind=state.pending?"waiting":"running";}
public:
    std::string id,project;State state;
    bool identity=false,root=true,damaged=false;
    void consume(const Json& e) {
        if(damaged)return;
        if(!e.is_object()){damaged=true;return;}
        auto type=field(e,"type");
        if(!identity) {
            if(type!="session" || e.value("version",0)!=4){damaged=true;return;}
            id=field(e,"id");project=field(e,"cwd").substr(0,4096);
            if(id.empty() || id.size()>256){damaged=true;return;}
            root=field(e,"origin")!="subagent" && e.value("delegationDepth",0)==0;
            seeded_=e.value("isSeeded",false);identity=true;return;
        }
        if(!e.contains("seq") || !e["seq"].is_number_integer() || e["seq"].get<long long>()!=sequence_+1 ||
           !e.contains("time") || !e["time"].is_number_integer() || !e.contains("data") || !e["data"].is_object()){damaged=true;return;}
        ++sequence_;const auto& d=e["data"];auto time=e["time"].get<Time>();
        if(seeded_) {if(type=="session/end-seed")seeded_=false;return;}
        if(!root)return;
        if(type=="turn/start") {
            auto turn=d.value("turn",0LL);if(turn<=turn_){damaged=true;return;}
            turn_=turn;questions_.clear();approvals_.clear();state=State{};
            state.turn=std::to_string(turn);state.started=time;state.updated=time;state.event=type;state.kind="running";return;
        }
        if(!state.active())return;
        if(d.contains("turn") && d.value("turn",0LL)!=turn_)return;
        bool changed=false;
        if(type=="turn/end") {
            auto reason=d.find("reason");auto kind=reason==d.end()?"":field(*reason,"kind");
            state.kind=kind=="completed"?"completed":kind=="aborted" || kind=="error" || kind=="interrupted" || kind=="blocked" || kind=="max-tokens"?"interrupted":"unknown";
            questions_.clear();approvals_.clear();state.pending=0;changed=true;
        } else if(type=="tool/call" && field(d,"name")=="ask_user_question") {
            auto call=field(d,"callId");if(call.empty() || call.size()>256 || questions_.size()>=32){damaged=true;return;}
            questions_.insert(call);pending();changed=true;
        } else if(type=="tool/result") {
            auto message=d.find("message");auto call=message==d.end()?"":field(*message,"toolCallId");
            if(questions_.erase(call)){pending();changed=true;}
        } else if(type=="approval/asked") {
            auto request=field(d,"id");if(request.empty() || request.size()>256 || approvals_.size()>=32){damaged=true;return;}
            approvals_.insert(request);pending();changed=true;
        } else if(type=="approval/decided") {
            if(approvals_.erase(field(d,"id"))){pending();changed=true;}
        }
        if(changed){state.updated=time;state.event=type;}
    }
};
class Reader {
    uint64_t offset_=0,identity_=0;DWORD volume_=0;bool initialized_=false;
    std::string compressed_,line_;
    void text(const char* data,size_t size) {
        size_t begin=0;
        while(begin<size) {
            auto end=begin;while(end<size && data[end]!='\n')++end;
            if(line_.size()+end-begin>4*1024*1024)throw std::runtime_error("DeepSeek record too large");
            line_.append(data+begin,end-begin);
            if(end<size){lifecycle.consume(parse(line_));line_.clear();}
            begin=end+1;
        }
    }
    void decode() {
        size_t consumed=0;
        while(consumed<compressed_.size()) {
            auto data=compressed_.data()+consumed;auto size=compressed_.size()-consumed;
            auto frame=ZSTD_findFrameCompressedSize(data,size);
            if(ZSTD_isError(frame)) {
                if(ZSTD_getErrorCode(frame)==ZSTD_error_srcSize_wrong)break;
                throw std::runtime_error("DeepSeek compressed log invalid");
            }
            constexpr size_t limit=16*1024*1024;
            auto content=ZSTD_getFrameContentSize(data,frame);
            if(content==ZSTD_CONTENTSIZE_ERROR || (content!=ZSTD_CONTENTSIZE_UNKNOWN && content>limit))throw std::runtime_error("DeepSeek frame too large");
            std::string output(content==ZSTD_CONTENTSIZE_UNKNOWN?limit:(size_t)content,'\0');
            auto got=ZSTD_decompress(output.data(),output.size(),data,frame);
            if(ZSTD_isError(got))throw std::runtime_error("DeepSeek checksum/decompression failed");
            text(output.data(),got);consumed+=frame;
        }
        compressed_.erase(0,consumed);
        if(compressed_.size()>16*1024*1024)throw std::runtime_error("DeepSeek compressed frame too large");
    }
public:
    Lifecycle lifecycle;bool catchingUp=false;
    void poll(const fs::path& path) {
        HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("DeepSeek log unavailable");
        struct Close{HANDLE h;~Close(){CloseHandle(h);}}close{file};
        BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(file,&info))throw std::runtime_error("DeepSeek log identity");
        uint64_t identity=((uint64_t)info.nFileIndexHigh<<32)|info.nFileIndexLow,size=((uint64_t)info.nFileSizeHigh<<32)|info.nFileSizeLow;
        if(!initialized_ || identity_!=identity || volume_!=info.dwVolumeSerialNumber || size<offset_) {
            offset_=0;compressed_.clear();line_.clear();lifecycle=Lifecycle{};identity_=identity;volume_=info.dwVolumeSerialNumber;initialized_=true;
        }
        if(lifecycle.damaged)return;
        try {
            if(size>128*1024*1024)throw std::runtime_error("DeepSeek log exceeds read limit");
            LARGE_INTEGER position;position.QuadPart=offset_;if(!SetFilePointerEx(file,position,nullptr,FILE_BEGIN))throw std::runtime_error("DeepSeek log seek");
            auto remaining=std::min<uint64_t>(size-offset_,2*1024*1024);char block[65536];
            while(remaining) {
                DWORD got=0;if(!ReadFile(file,block,(DWORD)std::min<uint64_t>(remaining,sizeof(block)),&got,nullptr))throw std::runtime_error("DeepSeek log read");
                if(!got)break;remaining-=got;offset_+=got;
                if(path.extension()==L".zstd"){compressed_.append(block,got);decode();}else text(block,got);
            }
            catchingUp=offset_<size || !compressed_.empty() || !line_.empty();
        }catch(...){lifecycle.damaged=true;throw;}
    }
};
inline int generation(const fs::path& path) {
    auto name=path.filename().wstring();
    if(name==L"session.jsonl" || name==L"session.jsonl.zstd")return 0;
    if(name.find(L"session.v")!=0)return -1;
    size_t pos=9;int version=0;bool digit=false;
    while(pos<name.size() && name[pos]>=L'0' && name[pos]<=L'9'){digit=true;version=version*10+(name[pos++]-L'0');if(version>10000)return -1;}
    auto suffix=name.substr(pos);return digit && (suffix==L".jsonl" || suffix==L".jsonl.zstd")?version:-1;
}
class Engine {
    fs::path home_;std::map<fs::path,Reader> readers_;Time scan_=0;Selector selector_{0};
public:
    explicit Engine(fs::path home):home_(std::move(home)){}
    View tick(const Runtime& runtime,const std::string& manual="") {
        View view;if(!runtime.online){view.state.reason="等待 DeepSeek Harness 启动";return view;}
        if(!scan_ || now()-scan_>=2000) {
            std::error_code ec;std::map<fs::path,fs::path> canonical;size_t visited=0;
            for(fs::recursive_directory_iterator it(home_/L"sessions",fs::directory_options::skip_permission_denied,ec),end;it!=end && !ec;it.increment(ec)) {
                if(++visited>12000)break;if(it.depth()>2)it.disable_recursion_pending();
                auto path=it->path();auto version=generation(path);if(version<0 || !it->is_regular_file(ec))continue;
                auto parent=path.parent_path();if(!canonical.count(parent) || version>generation(canonical[parent]))canonical[parent]=path;
            }
            if(ec)throw std::runtime_error("DeepSeek session discovery failed");
            std::vector<std::pair<fs::file_time_type,fs::path>> recent;
            for(const auto& c:canonical){auto modified=fs::last_write_time(c.second,ec);if(!ec)recent.push_back({modified,c.second});}
            std::sort(recent.rbegin(),recent.rend());if(recent.size()>64)recent.resize(64);
            std::set<fs::path> paths;for(const auto& c:recent)paths.insert(c.second);
            for(auto it=readers_.begin();it!=readers_.end();)if(!paths.count(it->first))it=readers_.erase(it);else ++it;
            for(const auto& path:paths)readers_.try_emplace(path);scan_=now();
        }
        std::vector<Candidate> candidates;bool uncertain=false;
        for(auto& pair:readers_) {
            auto& r=pair.second;r.poll(pair.first);auto& l=r.lifecycle;
            if(l.identity && !l.root)continue;
            if(l.damaged || r.catchingUp){uncertain=true;continue;}
            if(!l.identity || l.state.started<runtime.started)continue;
            Candidate c;c.id=l.id;c.project=l.project;c.source="deepseek-session-v4";c.state=l.state;
            c.state.reason=c.state.kind=="running"?"DeepSeek 正在运行":c.state.kind=="waiting"?"DeepSeek 等待回答或确认":c.state.kind=="completed"?"DeepSeek 本轮已结束":"DeepSeek 已停止或出错";
            candidates.push_back(std::move(c));
        }
        view.online=true;
        if(uncertain){view.state.reason="DeepSeek 日志不完整或格式不受支持";return view;}
        view=selector_.choose(candidates,now(),manual);view.online=true;
        if(candidates.empty())view.state.reason="等待发现 DeepSeek 新任务";
        return view;
    }
};
}
