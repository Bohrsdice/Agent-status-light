#pragma once
#include "core.hpp"
#include <algorithm>

namespace light::cursorNative {
struct Event {Time time=0;Json value;};
// Cursor writes the original, UTF-8 hook INPUT before interpreting hook output.
// Keep only lifecycle metadata; never persist prompts or the raw log fragment.
class InputParser {
    Time time_=0;bool input_=false;std::string json_;
public:
    void line(std::string text,std::vector<Event>& events) {
        if(!text.empty() && text.back()=='\r')text.pop_back();
        if(text.size()>25 && text[0]=='[') {
            auto end=text.find(']');auto t=parseTime(text.substr(1,end-1));
            if(t){time_=t;input_=false;json_.clear();}
        }
        if(text=="INPUT:"){input_=true;json_.clear();return;}
        if(!input_)return;
        if(json_.size()+text.size()>1024*1024){input_=false;json_.clear();return;}
        json_+=text;json_+='\n';
        if(text!="}")return;
        auto value=parse(json_);input_=false;json_.clear();
        if(!value.is_object() || !time_)return;
        auto kind=field(value,"hook_event_name");
        if(kind!="beforeSubmitPrompt" && kind!="stop" && kind!="sessionEnd")return;
        Json metadata=Json::object();
        for(auto key:{"conversation_id","generation_id","hook_event_name","status","reason"})metadata[key]=field(value,key);
        if(value.contains("workspace_roots") && value["workspace_roots"].is_array())metadata["workspace_roots"]=value["workspace_roots"];
        events.push_back({time_,std::move(metadata)});
    }
};
class Log {
    uint64_t offset_=0,identity_=0;DWORD volume_=0;bool initialized_=false,discard_=false;
    std::string partial_;InputParser parser_;
public:
    void poll(const fs::path& path,std::vector<Event>& events) {
        HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cursor hook log unavailable");
        struct Close{HANDLE h;~Close(){CloseHandle(h);}}close{file};
        BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(file,&info))throw std::runtime_error("Cursor log identity");
        uint64_t identity=((uint64_t)info.nFileIndexHigh<<32)|info.nFileIndexLow,size=((uint64_t)info.nFileSizeHigh<<32)|info.nFileSizeLow;
        constexpr uint64_t budget=4*1024*1024;
        if(!initialized_ || identity_!=identity || volume_!=info.dwVolumeSerialNumber || size<offset_) {
            offset_=size>budget?size-budget:0;discard_=offset_!=0;partial_.clear();parser_=InputParser{};
            identity_=identity;volume_=info.dwVolumeSerialNumber;initialized_=true;
        }
        LARGE_INTEGER position;position.QuadPart=offset_;if(!SetFilePointerEx(file,position,nullptr,FILE_BEGIN))throw std::runtime_error("Cursor log seek");
        auto remaining=std::min<uint64_t>(size-offset_,budget);char block[65536];
        while(remaining) {
            DWORD got=0;if(!ReadFile(file,block,(DWORD)std::min<uint64_t>(remaining,sizeof(block)),&got,nullptr))throw std::runtime_error("Cursor log read");
            if(!got)break;remaining-=got;offset_+=got;partial_.append(block,got);
            size_t used=0,end=0;
            while((end=partial_.find('\n',used))!=std::string::npos) {
                if(!discard_)parser_.line(partial_.substr(used,end-used),events);
                discard_=false;used=end+1;
            }
            partial_.erase(0,used);
            if(partial_.size()>1024*1024){partial_.clear();discard_=true;parser_=InputParser{};}
        }
    }
};
class Logs {
    fs::path root_;std::map<fs::path,Log> logs_;Time scan_=0;
public:
    explicit Logs(fs::path root):root_(std::move(root)){}
    std::vector<Event> poll() {
        std::vector<Event> events;if(root_.empty())return events;
        if(!scan_ || now()-scan_>2000) {
            std::error_code ec;std::vector<fs::path> dates;
            for(fs::directory_iterator it(root_/L"logs",ec),end;it!=end && !ec;it.increment(ec))
                if(it->is_directory(ec) && it->path().filename().wstring().size()==15)dates.push_back(it->path());
            if(ec)throw std::runtime_error("Cursor log directory unavailable");
            std::sort(dates.rbegin(),dates.rend());if(dates.size()>4)dates.resize(4);
            std::set<fs::path> paths;
            for(const auto& dir:dates) {
                size_t visited=0;
                for(fs::recursive_directory_iterator it(dir,fs::directory_options::skip_permission_denied,ec),end;it!=end && !ec;it.increment(ec)) {
                    if(++visited>4096)break;if(it.depth()>3)it.disable_recursion_pending();
                    auto name=it->path().filename().wstring();
                    if(name.find(L"cursor.hooks.")==0 && it->path().extension()==L".log" && paths.size()<32)paths.insert(it->path());
                }
            }
            for(auto it=logs_.begin();it!=logs_.end();)if(!paths.count(it->first))it=logs_.erase(it);else ++it;
            for(const auto& path:paths)logs_.try_emplace(path);
            scan_=now();
        }
        for(auto& pair:logs_)pair.second.poll(pair.first,events);
        std::stable_sort(events.begin(),events.end(),[](const Event& a,const Event& b){return a.time<b.time;});return events;
    }
};

// Load the Windows system SQLite library, not an extra bundled runtime.
struct Sqlite {
    HMODULE library=nullptr;
    int (__cdecl *open)(const char*,void**,int,const char*)=nullptr;
    int (__cdecl *close)(void*)=nullptr;
    int (__cdecl *prepare)(void*,const char*,int,void**,const char**)=nullptr;
    int (__cdecl *bind)(void*,int,const char*,int,void(__cdecl *)(void*))=nullptr;
    int (__cdecl *step)(void*)=nullptr;
    int (__cdecl *finalize)(void*)=nullptr;
    const void* (__cdecl *blob)(void*,int)=nullptr;
    int (__cdecl *bytes)(void*,int)=nullptr;
    int (__cdecl *timeout)(void*,int)=nullptr;
    Sqlite() {
        library=LoadLibraryExW(L"winsqlite3.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!library)return;
#define LIGHT_SQL(member,name) member=reinterpret_cast<decltype(member)>(GetProcAddress(library,name))
        LIGHT_SQL(open,"sqlite3_open_v2");LIGHT_SQL(close,"sqlite3_close");LIGHT_SQL(prepare,"sqlite3_prepare_v2");
        LIGHT_SQL(bind,"sqlite3_bind_text");LIGHT_SQL(step,"sqlite3_step");LIGHT_SQL(finalize,"sqlite3_finalize");
        LIGHT_SQL(blob,"sqlite3_column_blob");LIGHT_SQL(bytes,"sqlite3_column_bytes");LIGHT_SQL(timeout,"sqlite3_busy_timeout");
#undef LIGHT_SQL
    }
    bool available()const{return open&&close&&prepare&&bind&&step&&finalize&&blob&&bytes&&timeout;}
    ~Sqlite(){if(library)FreeLibrary(library);}
    Sqlite(const Sqlite&)=delete;Sqlite& operator=(const Sqlite&)=delete;
};
inline std::vector<std::string> questionBubbles(const Json& composer,const std::string& generation) {
    std::vector<std::string> ids;
    if(field(composer,"latestChatGenerationUUID")!=generation)return ids;
    auto it=composer.find("fullConversationHeadersOnly");if(it==composer.end() || !it->is_array())return ids;
    bool human=false;
    for(auto h=it->rbegin();h!=it->rend();++h) {
        if(h->value("type",0)==1){human=true;break;}
        auto group=h->find("grouping");
        if(group!=h->end() && field(*group,"toolCallCase")=="askQuestionToolCall") {
            auto id=field(*h,"bubbleId");if(!id.empty() && id.size()<=256 && ids.size()<32)ids.push_back(id);
        }
    }
    if(!human)ids.clear();return ids;
}
inline bool pendingQuestion(const Json& bubble) {
    auto it=bubble.find("toolFormerData");if(it==bubble.end() || !it->is_object())return false;
    // Cursor may persist the outer tool as completed while its card is still
    // pending. The card's own status/result/decision is the authoritative data.
    auto status=field(*it,"status");
    if(field(*it,"name")!="ask_question" || (status!="loading" && status!="completed"))return false;
    if(!field(*it,"result").empty() || !field(*it,"userDecision").empty())return false;
    auto additional=it->find("additionalData");if(additional==it->end() || field(*additional,"status")!="pending")return false;
    auto params=parse(field(*it,"params"));
    if(!params.is_object() || params.value("run_async",false) || params.value("runAsync",false))return false;
    return true;
}
class Questions {
    Sqlite api_;fs::path path_;
public:
    explicit Questions(fs::path root):path_(root.empty()?fs::path{}:root/L"User"/L"globalStorage"/L"state.vscdb"){}
    size_t pending(const std::string& conversation,const std::string& generation) {
        if(path_.empty())return 0;
        if(!api_.available())throw std::runtime_error("Windows SQLite unavailable");
        void* db=nullptr;
        int opened=api_.open(utf8(path_.wstring()).c_str(),&db,1,nullptr); // SQLITE_OPEN_READONLY
        struct Close{Sqlite& a;void* p;~Close(){if(p)a.close(p);}}close{api_,db};
        if(opened!=0)throw std::runtime_error("Cursor state database unavailable");
        api_.timeout(db,50);
        auto get=[&](const std::string& key)->Json {
            void* statement=nullptr;
            auto rc=api_.prepare(db,"SELECT value FROM cursorDiskKV WHERE key=?1",-1,&statement,nullptr);
            struct Final{Sqlite& a;void* p;~Final(){if(p)a.finalize(p);}}final{api_,statement};
            if(rc!=0 || api_.bind(statement,1,key.c_str(),(int)key.size(),nullptr)!=0)throw std::runtime_error("Cursor state schema unavailable");
            rc=api_.step(statement);if(rc==101)return Json::object();
            if(rc!=100)throw std::runtime_error("Cursor state database busy");
            auto length=api_.bytes(statement,0);auto data=api_.blob(statement,0);
            if(length<=0 || length>2*1024*1024 || !data)throw std::runtime_error("Cursor state record unavailable");
            auto value=parse(std::string((const char*)data,length));
            if(!value.is_object())throw std::runtime_error("Cursor state JSON invalid");return value;
        };
        auto composer=get("composerData:"+conversation);size_t count=0;
        for(const auto& id:questionBubbles(composer,generation))if(pendingQuestion(get("bubbleId:"+conversation+":"+id)))++count;
        return count;
    }
};
}
