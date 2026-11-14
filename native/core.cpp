#include "core.hpp"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <limits>

namespace light {
Time now() {
    FILETIME value; GetSystemTimeAsFileTime(&value);
    ULARGE_INTEGER ticks; ticks.LowPart=value.dwLowDateTime; ticks.HighPart=value.dwHighDateTime;
    return static_cast<Time>(ticks.QuadPart / 10000 - 11644473600000ULL);
}
Time parseTime(const std::string& text) {
    if (text.size() < 20) return 0;
    unsigned y,m,d,h,mi,s;
    if (sscanf_s(text.c_str(), "%u-%u-%uT%u:%u:%u", &y,&m,&d,&h,&mi,&s) != 6) return 0;
    // Codex lifecycle timestamps are UTC. Refuse unfamiliar timezone formats.
    auto zone = text.find('Z',19);
    if (zone == std::string::npos && text.find("+00:00",19) == std::string::npos) return 0;
    SYSTEMTIME st{}; st.wYear=(WORD)y; st.wMonth=(WORD)m; st.wDay=(WORD)d;
    st.wHour=(WORD)h; st.wMinute=(WORD)mi; st.wSecond=(WORD)s;
    if (text[19] == '.') {
        unsigned ms = 0, factor=100;
        for (size_t i=20; i<text.size() && factor && text[i]>='0' && text[i]<='9'; ++i, factor/=10)
            ms += (text[i]-'0')*factor;
        st.wMilliseconds=(WORD)ms;
    }
    FILETIME ft{}; if (!SystemTimeToFileTime(&st,&ft)) return 0;
    ULARGE_INTEGER n; n.LowPart=ft.dwLowDateTime; n.HighPart=ft.dwHighDateTime;
    return static_cast<Time>(n.QuadPart/10000 - 11644473600000ULL);
}
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int count=WideCharToMultiByte(CP_UTF8,0,text.data(),(int)text.size(),nullptr,0,nullptr,nullptr);
    std::string result(count,0);
    WideCharToMultiByte(CP_UTF8,0,text.data(),(int)text.size(),result.data(),count,nullptr,nullptr);
    return result;
}
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    int count=MultiByteToWideChar(CP_UTF8,0,text.data(),(int)text.size(),nullptr,0);
    std::wstring result(count,0);
    MultiByteToWideChar(CP_UTF8,0,text.data(),(int)text.size(),result.data(),count);
    return result;
}
std::string field(const Json& v,const char* key) {
    if (!v.is_object()) return {};
    auto i=v.find(key); return i!=v.end() && i->is_string() ? i->get<std::string>() : "";
}
Json parse(const std::string& text) { return Json::parse(text,nullptr,false); }
void atomicWrite(const fs::path& path,const Json& data) {
    fs::create_directories(path.parent_path());
    fs::path temporary=path; temporary += L".tmp";
    { std::ofstream out(temporary,std::ios::binary|std::ios::trunc);
      auto text=data.dump(2); out.write(text.data(),text.size());
      if (!out) throw std::runtime_error("write failed"); }
    if (!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING))
        throw std::runtime_error("replace failed");
}

void Lifecycle::item(const Json& v,Time time) {
    auto type=field(v,"type");
    if ((type=="AgentMessage" || type=="agentMessage") && field(v,"delivery")=="async") {
        auto call=field(v,"id");
        if (call.empty() || !v.contains("questions") || !v["questions"].is_array()) return;
        for (size_t i=0;i<v["questions"].size();++i)
            if (v["questions"][i].is_object() && !answered_.count({call,i})) questions_.insert({call,i});
    } else if (type=="UserMessage" || type=="userMessage") {
        if (!v.contains("content") || !v["content"].is_array()) return;
        const std::string begin="<send_user_message_question_reply>", end="</send_user_message_question_reply>";
        for (const auto& part:v["content"]) {
            auto text=field(part,"text");
            auto a=text.find_first_not_of(" \r\n\t"), b=text.find_last_not_of(" \r\n\t");
            if (a==std::string::npos) continue;
            text=text.substr(a,b-a+1);
            if (text.size()<begin.size()+end.size() || text.compare(0,begin.size(),begin)!=0 ||
                text.compare(text.size()-end.size(),end.size(),end)!=0) continue;
            auto replies=parse(text.substr(begin.size(),text.size()-begin.size()-end.size()));
            if (!replies.is_array()) continue;
            for (const auto& reply:replies) {
                auto key=parse(field(reply,"questionItemId"));
                if (!key.is_array() || key.size()!=3 || !key[0].is_string() ||
                    key[0]!="request_user_input_async" || !key[1].is_string() || !key[2].is_number_unsigned()) continue;
                auto pair=std::make_pair(key[1].get<std::string>(),key[2].get<size_t>());
                if (questions_.erase(pair)) answered_.insert(pair);
            }
        }
    } else return;
    state.pending=questions_.size();
    if (!questions_.empty()) {
        state.kind="waiting"; state.event="question_pending"; state.reason="等待你回答或确认"; state.updated=time;
    } else if (state.kind=="waiting") {
        state.kind=finished_ ? "completed" : "running";
        state.event="question_answered"; state.reason=finished_ ? "确认已收到；本轮已结束" : "确认已收到；继续运行";
        state.updated=time;
    }
}
void Lifecycle::consume(const Json& record) {
    if (!record.is_object() || !record.contains("payload") || !record["payload"].is_object()) return;
    const auto& p=record["payload"]; auto type=field(record,"type");
    if (type=="session_meta") {
        auto incoming=field(p,"id");
        if (!id.empty() && incoming!=id) { identity=false; state=State{}; return; }
        id=incoming; identity=!id.empty(); project=field(p,"cwd");
        root=!(p.contains("source") && p["source"].is_object());
        return;
    }
    if (!identity || type!="event_msg") return;
    auto thread=field(p,"thread_id"); if (!thread.empty() && thread!=id) return;
    auto turn=field(p,"turn_id"), kind=field(p,"type");
    if (turn.empty()) return;
    Time time=parseTime(field(record,"timestamp"));
    if (kind=="task_started") {
        questions_.clear(); answered_.clear(); finished_=false;
        state=State{}; state.kind="running"; state.turn=turn; state.event=kind;
        state.started=time; state.updated=time; state.reason="任务正在运行";
    } else if (turn==state.turn && kind=="item_completed" && (state.active() || state.kind=="completed")) {
        if (p.contains("item")) item(p["item"],time);
    } else if (turn==state.turn && state.active() && (kind=="task_complete" || kind=="turn_aborted")) {
        state.event=kind; state.updated=time;
        if (kind=="turn_aborted") {
            questions_.clear(); state.pending=0; state.kind="interrupted"; state.reason="任务已中断";
        } else {
            finished_=true; state.kind=questions_.empty() ? "completed" : "waiting";
            state.reason=questions_.empty() ? "本轮已结束" : "本轮已结束，仍等待你确认";
        }
    }
}

struct File {
    HANDLE h=INVALID_HANDLE_VALUE;
    explicit File(const fs::path& p) { h=CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                                   nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr); }
    ~File() { if (h!=INVALID_HANDLE_VALUE) CloseHandle(h); }
};
static std::string readBytes(HANDLE h,uint64_t offset,size_t length) {
    LARGE_INTEGER pos; pos.QuadPart=offset;
    if (!SetFilePointerEx(h,pos,nullptr,FILE_BEGIN)) throw std::runtime_error("seek");
    std::string data(length,0); DWORD count=0;
    if (length && !ReadFile(h,data.data(),(DWORD)length,&count,nullptr)) throw std::runtime_error("read");
    data.resize(count); return data;
}
void Reader::initialize(HANDLE h,uint64_t size) {
    lifecycle=Lifecycle{}; offset_=0; pending_.clear(); damaged_=false;
    auto head=readBytes(h,0,(size_t)std::min<uint64_t>(size,1024*1024));
    auto end=head.find('\n');
    if (end==std::string::npos) { initialized_=true; return; }
    lifecycle.consume(parse(head.substr(0,end)));
    if (!lifecycle.identity) { initialized_=true; return; }
    offset_=end+1;
    // Seek straight to the latest start in a small tail window when possible.
    // If a long live turn has no start in the window, bounded streaming replay follows.
    uint64_t start=size>256*1024 ? size-256*1024 : 0;
    auto tail=readBytes(h,start,(size_t)(size-start));
    size_t at=start ? tail.find('\n') : 0;
    if (at==std::string::npos) { initialized_=true; return; }
    if (start) ++at;
    bool found=false, terminal=false;
    size_t complete=at;
    while (at<tail.size()) {
        auto next=tail.find('\n',at); if (next==std::string::npos) break;
        auto line=tail.substr(at,next-at);
        if (line.find("task_started")!=std::string::npos || line.find("task_complete")!=std::string::npos ||
            line.find("turn_aborted")!=std::string::npos) {
            auto r=parse(line);
            if (r.is_object() && field(r,"type")=="event_msg" && r.contains("payload")) {
                const auto& p=r["payload"]; auto thread=field(p,"thread_id");
                auto k=field(p,"type");
                if ((thread.empty() || thread==lifecycle.id) && !field(p,"turn_id").empty()) {
                    if (k=="task_started") { offset_=start+at; found=true; terminal=false; }
                    else if (k=="task_complete" || k=="turn_aborted") terminal=true;
                }
            }
        }
        at=next+1; complete=at;
    }
    if (!found && terminal) offset_=start+complete;
    initialized_=true;
}
void Reader::poll() {
    try {
        File file(path); if (file.h==INVALID_HANDLE_VALUE) throw std::runtime_error("open");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file.h,&info)) throw std::runtime_error("stat");
        uint64_t size=(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
        uint64_t id=(uint64_t(info.nFileIndexHigh)<<32)|info.nFileIndexLow;
        if (!initialized_ || id!=fileId_ || volume_!=info.dwVolumeSerialNumber || size<offset_) {
            initialize(file.h,size); fileId_=id; volume_=info.dwVolumeSerialNumber;
        }
        auto chunk=readBytes(file.h,offset_,(size_t)std::min<uint64_t>(size-offset_,1024*1024));
        offset_+=chunk.size(); pending_+=chunk;
        size_t at=0;
        while (at<pending_.size()) {
            auto end=pending_.find('\n',at); if (end==std::string::npos) break;
            auto line=pending_.substr(at,end-at); at=end+1;
            if (line.empty() || line=="\r") continue;
            auto r=parse(line);
            if (r.is_discarded()) { damaged_=true; continue; }
            lifecycle.consume(r);
            if (field(r,"type")=="event_msg" && r.contains("payload") &&
                field(r["payload"],"type")=="task_started" && lifecycle.identity &&
                lifecycle.state.event=="task_started" && lifecycle.state.turn==field(r["payload"],"turn_id") &&
                (field(r["payload"],"thread_id").empty() || field(r["payload"],"thread_id")==lifecycle.id)) damaged_=false;
        }
        pending_.erase(0,at);
        // A single oversized record is incompatible; keep memory bounded and fail closed.
        if (pending_.size()>16*1024*1024) { pending_.clear(); damaged_=true; }
        catchingUp=offset_<size;
        error=!lifecycle.identity ? "会话格式无法识别" : catchingUp ? "正在读取任务记录" : damaged_ ? "任务记录损坏，等待新的开始事件" : "";
    } catch (const std::exception&) { error="任务记录暂时不可读"; catchingUp=false; }
}

Json View::json() const {
    Json value=nullptr; if (state.kind=="running") value=1; else if (state.kind=="completed" || state.kind=="idle") value=0;
    Json options=Json::array();
    for (const auto& c:choices) options.push_back({{"thread_id",c.id},{"project",c.project},{"state",c.state.kind}});
    return {{"state",state.kind},{"value",value},{"reason",state.reason},{"thread_id",id},{"turn_id",state.turn},
            {"event",state.event},{"started_at_ms",state.started},{"event_at_ms",state.updated},
            {"pending_count",state.pending},{"source",source},{"project",project},
            {"runtime_connected",online},{"choices",options},{"mode","auto"}};
}
static View from(const Candidate& c) { View v; v.state=c.state; v.id=c.id; v.project=c.project; v.source=c.source; return v; }
View Selector::choose(const std::vector<Candidate>& candidates,Time time,const std::string& manual) {
    const Candidate* selected=nullptr; std::vector<Candidate> active;
    for (const auto& c:candidates) {
        if (c.state.active()) active.push_back(c);
        if (c.id==selected_) selected=&c;
    }
    if (!manual.empty()) {
        for (const auto& c:active) if (c.id==manual) { selected_ = c.id; terminalSince_=0; auto v=from(c); v.choices=active; return v; }
    }
    if (selected && selected->state.active()) { terminalSince_=0; auto v=from(*selected); v.choices=active; return v; }
    if (selected && selected->state.kind=="unknown") { auto v=from(*selected); v.choices=active; return v; }
    if (selected && (selected->state.kind=="completed" || selected->state.kind=="interrupted")) {
        if (!terminalSince_) terminalSince_=time;
        if (time-terminalSince_<2000 || active.empty()) { auto v=from(*selected); v.choices=active; return v; }
    }
    if (active.size()==1) {
        selected_=active[0].id; terminalSince_=0; auto v=from(active[0]); v.choices=active; return v;
    }
    View v; v.choices=active;
    if (active.size()>1) { v.state.reason="多个任务正在运行，请右键选择一个"; return v; }
    // Catch tasks that started and completed between two polling intervals.
    const Candidate* newest=nullptr;
    for (const auto& c:candidates)
        if ((c.state.kind=="completed" || c.state.kind=="interrupted") && c.state.started>=boot_ &&
            (!newest || c.state.started>newest->state.started)) newest=&c;
    if (newest) { selected_=newest->id; terminalSince_=time; return from(*newest); }
    if (selected) return from(*selected);
    return v;
}
View Engine::tick(const Runtime& runtime,const std::string& manual,Time time) {
    auto directory=home_/L"sessions";
    if (!lastScan_ || time-lastScan_>=3000) {
        lastScan_=time; scanFailed_=false;
        std::error_code ec;
        if (!fs::exists(directory,ec)) scanFailed_=true;
        else {
            std::set<fs::path> present;
            fs::recursive_directory_iterator end;
            for (fs::recursive_directory_iterator it(directory,fs::directory_options::skip_permission_denied,ec); !ec && it!=end; it.increment(ec)) {
                if (it->is_regular_file(ec) && it->path().extension()==L".jsonl") {
                    auto p=it->path(); present.insert(p);
                    if (!readers_.count(p)) readers_.emplace(p,Reader(p));
                }
            }
            scanFailed_=bool(ec);
            if (!scanFailed_) for (auto it=readers_.begin(); it!=readers_.end();) {
                if (!present.count(it->first)) it=readers_.erase(it); else ++it;
            }
        }
    }
    std::vector<Candidate> candidates;
    bool catching=false;
    for (auto& pair:readers_) {
        auto& r=pair.second; r.poll();
        if (!r.lifecycle.root || !r.lifecycle.identity) continue;
        Candidate c{r.lifecycle.id,r.lifecycle.project,utf8(r.path.wstring()),r.lifecycle.state};
        catching=catching || r.catchingUp;
        if (!r.error.empty()) { c.state.kind="unknown"; c.state.reason=r.error; }
        else if (c.state.active() && (c.state.started==0 || c.state.started<runtime.started)) {
            c.state.kind="unknown"; c.state.reason="旧任务没有结束记录，等待新任务";
        }
        candidates.push_back(std::move(c));
    }
    View v;
    // Do not auto-bind the first parsed file while another live candidate is still loading.
    if (catching && selector_.selected().empty()) v.state.reason="正在发现已有任务";
    else v=selector_.choose(candidates,time,manual);
    v.online=runtime.online;
    if (!runtime.online) { v.state.kind="unknown"; v.state.reason=runtime.reason; }
    else if (scanFailed_ && v.id.empty()) { v.state.kind="unknown"; v.state.reason="未找到 Codex 会话目录，等待 Codex 启动"; }
    return v;
}
}
