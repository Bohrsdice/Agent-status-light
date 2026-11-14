#pragma once
#include "core.hpp"
#include <functional>
#include <cstring>
#include <cmath>

namespace light {
// Restricted, read-only decoders for the JSON-like cache values observed in
// Doubao. Unsupported serialization tags fail closed; no browser is embedded.
namespace doubao {
constexpr size_t maxBytes=16*1024*1024;
struct Bytes {
    const std::string& b; size_t p=0;
    uint8_t byte(){if(p>=b.size())throw std::runtime_error("truncated");return (uint8_t)b[p++];}
    uint64_t little(size_t n){uint64_t v=0;for(size_t i=0;i<n;++i)v|=uint64_t(byte())<<(i*8);return v;}
    uint32_t var(){uint32_t v=0;for(int i=0;i<5;++i){auto c=byte();if(i==4 && c>15)throw std::runtime_error("varint");v|=uint32_t(c&127)<<(7*i);if(!(c&128))return v;}throw std::runtime_error("varint");}
    std::string take(size_t n){if(n>b.size()-p)throw std::runtime_error("length");auto r=b.substr(p,n);p+=n;return r;}
};
inline std::string unsnappy(const std::string& input) {
    Bytes r{input};auto n=r.var();if(n>maxBytes)throw std::runtime_error("size");
    std::string out;out.reserve(n);
    while(r.p<input.size() && out.size()<n){auto t=r.byte();size_t len=0,offset=0;
        if((t&3)==0){len=t>>2;len=len<60?len+1:size_t(r.little(len-59))+1;
            if(len>n-out.size())throw std::runtime_error("literal");out+=r.take(len);
        }else{if((t&3)==1){len=4+((t>>2)&7);offset=((t&224)<<3)|r.byte();}
            else{len=1+(t>>2);offset=size_t(r.little((t&3)==2?2:4));}
            if(!offset||offset>out.size()||len>n-out.size())throw std::runtime_error("copy");
            for(size_t i=0;i<len;++i)out+=out[out.size()-offset];
        }
    }
    if(out.size()!=n||r.p!=input.size())throw std::runtime_error("snappy tail");return out;
}
class V8 {
    Bytes r;std::vector<Json> refs;size_t nodes=0;
    uint8_t peek(){while(r.p<r.b.size() && !r.b[r.p])++r.p;if(r.p>=r.b.size())throw std::runtime_error("tag");return (uint8_t)r.b[r.p];}
    std::string key(const Json& x){if(x.is_string())return x.get<std::string>();if(x.is_number_unsigned()||x.is_number_integer())return x.dump();throw std::runtime_error("key");}
    Json value(int depth=0){
        if(depth>100||++nodes>250000)throw std::runtime_error("complexity");auto t=peek();++r.p;
        if(t=='?' ){r.var();return value(depth);}
        if(t=='_'||t=='0'||t=='-')return nullptr;if(t=='T')return true;if(t=='F')return false;
        if(t=='I'){auto n=r.var();return int64_t(n>>1)^-int64_t(n&1);}if(t=='U')return r.var();
        if(t=='N'){auto bits=r.little(8);double d;memcpy(&d,&bits,8);if(!std::isfinite(d))return nullptr;return d;}
        if(t=='S'||t=='"'||t=='c'){
            auto bytes=r.take(r.var());if(t=='S')return bytes;
            std::wstring w;if(t=='c'){if(bytes.size()%2)throw std::runtime_error("utf16");for(size_t i=0;i<bytes.size();i+=2)w+=wchar_t((uint8_t)bytes[i]|((uint8_t)bytes[i+1]<<8));}
            else for(unsigned char c:bytes)w+=wchar_t(c);return utf8(w);
        }
        if(t=='^'){auto id=r.var();if(id>=refs.size()||refs[id].is_discarded())throw std::runtime_error("reference");return refs[id];}
        if(t=='o'||t=='A'||t=='a'){
            auto id=refs.size();refs.push_back(Json(Json::value_t::discarded));
            bool array=t!='o';uint32_t length=array?r.var():0;if(length>250000)throw std::runtime_error("array");
            Json out=array?Json::array():Json::object();
            if(array)for(uint32_t i=0;i<length;++i)out.push_back(t=='A'?value(depth+1):Json(nullptr));
            uint8_t end=t=='o'?'{':t=='A'?'$':'@';uint32_t properties=0;
            while(peek()!=end){auto k=key(value(depth+1));auto v=value(depth+1);++properties;
                if(array){size_t index=0;try{size_t used=0;index=std::stoull(k,&used);if(used!=k.size())continue;}catch(...){continue;}if(index<length)out[index]=std::move(v);}
                else out[k]=std::move(v);
            }
            ++r.p;if(r.var()!=properties)throw std::runtime_error("properties");if(array&&r.var()!=length)throw std::runtime_error("array length");refs[id]=out;return out;
        }
        throw std::runtime_error("unsupported V8 tag");
    }
public:
    V8(const std::string& b,size_t p):r{b,p}{}
    Json read(){if(r.byte()!=255||r.var()!=15)throw std::runtime_error("V8 version");return value();}
};
inline Json decode(std::string value){
    auto compressed=value.find(std::string("\xff\x11\x02",3));
    if(compressed<10)value=unsnappy(value.substr(compressed+3));
    // Blink envelope precedes the V8 stream. Only the observed v15 format is
    // supported. Protobuf journal values and host objects are intentionally ignored.
    auto begin=value.find(std::string("\xff\x0f",2));
    if(begin>=32)return nullptr;
    return V8(value,begin).read();
}
inline uint32_t crc(const char* b,size_t n){
    static const auto table=[](){std::array<uint32_t,256> t{};for(uint32_t i=0;i<256;++i){auto c=i;for(int j=0;j<8;++j)c=(c>>1)^((c&1)?0x82f63b78u:0);t[i]=c;}return t;}();
    uint32_t c=~0u;for(size_t i=0;i<n;++i)c=table[(c^(uint8_t)b[i])&255]^(c>>8);return ~c;
}
struct Log {
    uint64_t offset=0;std::string fragments;bool damaged=false;
    void poll(const fs::path& path,const std::function<void(uint64_t,const Json&)>& accept){
        HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE)throw std::runtime_error("open cache");
        LARGE_INTEGER size{};if(!GetFileSizeEx(h,&size)){CloseHandle(h);throw std::runtime_error("cache size");}
        if(uint64_t(size.QuadPart)<offset){offset=0;fragments.clear();damaged=true;}
        size_t length=size_t(std::min<uint64_t>(uint64_t(size.QuadPart)-offset,8*1024*1024));
        std::string data(length,0);LARGE_INTEGER seek{};seek.QuadPart=offset;SetFilePointerEx(h,seek,nullptr,FILE_BEGIN);DWORD got=0;
        bool ok=ReadFile(h,data.data(),(DWORD)data.size(),&got,nullptr)!=FALSE;CloseHandle(h);if(!ok)throw std::runtime_error("read cache");data.resize(got);
        size_t p=0;while(p<data.size()){
            auto remaining=32768-size_t((offset+p)%32768);
            if(remaining<7){if(data.size()-p<remaining)break;p+=remaining;continue;}
            if(data.size()-p<7)break;Bytes header{data,p};auto checksum=(uint32_t)header.little(4);auto n=header.little(2);auto type=header.byte();
            if(!n&&!type){if(data.size()-p<remaining)break;p+=remaining;continue;}
            if(n+7>remaining){damaged=true;fragments.clear();p+=std::min(remaining,data.size()-p);continue;}
            if(n+7>data.size()-p)break;
            auto c=crc(data.data()+p+6,size_t(n)+1);auto masked=((c>>15)|(c<<17))+0xa282ead8u;
            if(masked!=checksum){damaged=true;fragments.clear();p+=size_t(n)+7;continue;}
            auto part=data.substr(p+7,size_t(n));p+=size_t(n)+7;std::string batch;
            if(type==1)batch=std::move(part);
            else if(type==2)fragments=std::move(part);
            else if(type==3||type==4){if(fragments.empty())continue;fragments+=part;if(type==4)batch.swap(fragments);}
            else{damaged=true;fragments.clear();}
            if(fragments.size()>maxBytes){damaged=true;fragments.clear();}
            if(batch.empty())continue;
            try{Bytes r{batch};auto seq=r.little(8);auto count=r.little(4);if(count>250000)throw std::runtime_error("batch");
                for(uint64_t i=0;i<count;++i){auto tag=r.byte();r.take(r.var());if(tag==0)continue;if(tag!=1)throw std::runtime_error("batch tag");auto val=r.take(r.var());
                    try{auto j=decode(std::move(val));if(j.is_object())accept(seq+i,j);}catch(const std::exception&){}
                }
            }catch(const std::exception&){damaged=true;}
        }
        offset+=p;
    }
};
struct Session {
    uint64_t loadingSeq=0;bool loading=false;Time started=0;std::set<std::string> running;
    struct Question {uint64_t seq=0;bool waiting=false;};std::map<std::string,Question> questions;
};
class Engine {
    fs::path folder;std::map<fs::path,Log> logs;std::map<std::string,Session> sessions;
    std::string selected;
    void conversation(uint64_t seq,const Json& x){
        if(!x.is_object())return;auto id=field(x,"conversation_id");if(id.empty()||!x.contains("extra"))return;
        auto extra=x["extra"];if(extra.is_string())extra=parse(extra.get<std::string>());if(!extra.is_object()||!extra.contains("is_loading"))return;
        auto& s=sessions[id];if(seq<=s.loadingSeq)return;
        auto flag=field(extra,"is_loading");if(flag!="0"&&flag!="1")return;
        s.loadingSeq=seq;s.loading=flag=="1";s.running.clear();auto running=parse(field(extra,"running_messages"));
        if(running.is_object())for(auto it=running.begin();it!=running.end();++it){s.running.insert(it.key());if(it.value().is_object()&&it.value().contains("started_at_ms")&&it.value()["started_at_ms"].is_number_integer())s.started=std::max(s.started,it.value()["started_at_ms"].get<Time>());}
    }
public:
    explicit Engine(fs::path path):folder(std::move(path)){}
    void accept(uint64_t seq,const Json& obj){
        if(obj.contains("data")&&obj["data"].is_object()){
            const auto& d=obj["data"];
            if(d.contains("conversations")&&d["conversations"].is_array())for(const auto& c:d["conversations"])conversation(seq,c);
            if(d.contains("devices")&&d["devices"].is_array())for(const auto& device:d["devices"])if(device.is_object()&&device.contains("conversations")&&device["conversations"].is_array())for(const auto& c:device["conversations"])conversation(seq,c);
        }
        if(!obj.contains("state")||!obj["state"].is_object())return;const auto& state=obj["state"];
        if(!state.contains("mainTaskDataMap")||!state["mainTaskDataMap"].is_object())return;
        for(const auto& task:state["mainTaskDataMap"]){if(!task.is_object()||!task.contains("receivedMessages")||!task["receivedMessages"].is_object())continue;
            for(auto msg=task["receivedMessages"].begin();msg!=task["receivedMessages"].end();++msg){auto m=msg.value();if(!m.is_object()||!m.contains("extra")||!m["extra"].is_object())continue;auto& extra=m["extra"];
                auto id=field(extra,"conversation_id");if(id.empty()||!extra.contains("content_blocks_v2")||!extra["content_blocks_v2"].is_array())continue;
                bool waiting=false;for(const auto& b:extra["content_blocks_v2"]){
                    if(!b.is_object()||!b.contains("content")||!b["content"].is_object())continue;auto& c=b["content"];
                    if(c.contains("interaction_ask_block")&&c["interaction_ask_block"].is_object()){auto& q=c["interaction_ask_block"];if(q.contains("status")&&q["status"].is_number_integer()&&q["status"].get<int>()==1)waiting=true;}
                }
                auto& q=sessions[id].questions[msg.key()];if(seq>q.seq)q={seq,waiting};
            }
        }
    }
    View tick(const std::string& manual="",bool online=true,Time runtimeStarted=0){
        View v;v.online=online;std::error_code ec;
        if(!fs::exists(folder,ec)){v.state.reason="等待豆包创建本地任务缓存";return v;}
        std::set<fs::path> present;bool damaged=false;
        for(fs::directory_iterator it(folder,ec),end;!ec&&it!=end;it.increment(ec))if(it->path().extension()==L".log"){
            auto p=it->path();present.insert(p);auto& log=logs[p];log.poll(p,[this](uint64_t seq,const Json& x){accept(seq,x);});damaged=damaged||log.damaged;
        }
        if(ec)throw std::runtime_error("cache scan");for(auto i=logs.begin();i!=logs.end();)if(!present.count(i->first))i=logs.erase(i);else ++i;
        // Keep bounded metadata only, never message contents or titles.
        for(auto& pair:sessions){auto& s=pair.second;for(auto q=s.questions.begin();q!=s.questions.end();)if(!s.running.count(q->first))q=s.questions.erase(q);else ++q;}
        if(sessions.size()>2048)throw std::runtime_error("too many sessions");
        std::vector<std::string> active;for(const auto& pair:sessions)if(pair.second.loading&&pair.second.started>=runtimeStarted)active.push_back(pair.first);
        if(!manual.empty()&&sessions.count(manual))selected=manual;
        if(selected.empty()||!sessions[selected].loading){if(active.size()==1)selected=active[0];}
        if(selected.empty()&&active.empty()){
            Time newest=runtimeStarted;
            for(const auto& pair:sessions)if(pair.second.started>=newest&&pair.second.started>0){selected=pair.first;newest=pair.second.started;}
        }
        if(!online){v.state.reason="等待豆包启动";return v;}
        if(damaged){v.state.reason="豆包缓存校验失败，请重启指示灯后重新测试";return v;}
        for(const auto& id:active){Candidate c;c.id=id;c.project="豆包";c.state.kind="running";v.choices.push_back(c);}
        if(selected.empty()){v.state.reason=active.size()>1?"多个豆包任务，请右键选择":"豆包已连接，请开始一个新任务";return v;}
        auto& s=sessions[selected];v.id=selected;v.project="豆包";v.source=utf8(folder.wstring());v.state.started=s.started;v.state.turn=s.running.empty()?"":*s.running.begin();
        if(s.loading&&s.started<runtimeStarted){v.state.reason="旧任务缓存，等待新的豆包任务";return v;}
        for(const auto& q:s.questions)if(s.running.count(q.first)&&q.second.waiting)++v.state.pending;
        v.state.kind=s.loading?(v.state.pending?"waiting":"running"):"idle";
        v.state.event=s.loading?(v.state.pending?"interaction_ask_pending":"is_loading_1"):"is_loading_0";
        v.state.reason=s.loading?(v.state.pending?"豆包正在等待回答原生提问卡片":"豆包任务正在运行"):"当前未运行（正常结束或手动停止）";
        return v;
    }
};
}
}
