#include "doubao.hpp"
#include <iostream>
#include <fstream>
using namespace light;
// Synthetic V8 / LevelDB records only: no captured chats or account IDs.
static void var(std::string& out,uint32_t n){do{auto b=n&127;n>>=7;out+=char(b|(n?128:0));}while(n);}
static void little(std::string& out,uint64_t n,int count){for(int i=0;i<count;++i)out+=char(n>>(8*i));}
static void value(std::string& out,const Json& j){
 if(j.is_string()){auto s=j.get<std::string>();out+='S';var(out,(uint32_t)s.size());out+=s;}
 else if(j.is_number_integer()){out+='U';var(out,j.get<uint32_t>());}
 else if(j.is_object()){out+='o';for(auto i=j.begin();i!=j.end();++i){value(out,i.key());value(out,i.value());}out+='{';var(out,(uint32_t)j.size());}
 else if(j.is_array()){out+='A';var(out,(uint32_t)j.size());for(const auto& x:j)value(out,x);out+='$';var(out,0);var(out,(uint32_t)j.size());}
 else throw std::runtime_error("unsupported fixture value");
}
static std::string record(uint64_t sequence,const Json& data){
 std::string encoded("\xff\x0f",2);value(encoded,data);
 std::string batch;little(batch,sequence,8);little(batch,1,4);batch+='\1';var(batch,4);batch+="test";var(batch,(uint32_t)encoded.size());batch+=encoded;
 std::string payload(1,'\1');payload+=batch;auto c=doubao::crc(payload.data(),payload.size());
 std::string out;little(out,((c>>15)|(c<<17))+0xa282ead8u,4);little(out,batch.size(),2);out+=payload;return out;
}
int main(){
 auto fixture=fs::temp_directory_path()/(L"doubao-synthetic-"+std::to_wstring(GetCurrentProcessId()));
 try{
  fs::create_directories(fixture);doubao::Engine engine(fixture);uint64_t seq=1;
  auto conversation=[](bool running){auto j=parse(R"({"data":{"conversations":[{"conversation_id":"test-conversation","extra":{"is_loading":"1","running_messages":"{\"test-message\":{\"started_at_ms\":1000}}"}}]}})");j["data"]["conversations"][0]["extra"]["is_loading"]=running?"1":"0";return j;};
  auto question=[](int status){auto j=parse(R"({"state":{"mainTaskDataMap":{"test-task":{"receivedMessages":{"test-message":{"extra":{"conversation_id":"test-conversation","content_blocks_v2":[{"content":{"interaction_ask_block":{"status":1}}}]}}}}}}})");j["state"]["mainTaskDataMap"]["test-task"]["receivedMessages"]["test-message"]["extra"]["content_blocks_v2"][0]["content"]["interaction_ask_block"]["status"]=status;return j;};
  std::string raw;
  for(int stage=0;stage<6;++stage){bool running=stage==0||stage==3;raw=record(seq++,conversation(running));if(stage>=3)raw+=record(seq++,question(stage==3?1:2));
   {std::ofstream out(fixture/L"000001.log",std::ios::binary|std::ios::app);out.write(raw.data(),raw.size());}
   auto view=engine.tick("",true,1);std::string expected=stage==0?"running":stage==3?"waiting":"idle";
   if(view.id!="test-conversation"||view.state.kind!=expected)throw std::runtime_error("synthetic lifecycle mismatch");
  }
  raw=record(seq++,conversation(true));auto split=fixture/L"split.bin";
  {std::ofstream out(split,std::ios::binary);out.write(raw.data(),raw.size()-3);}
  doubao::Log log;uint64_t maxSeq=0;auto accept=[&](uint64_t n,const Json&){maxSeq=std::max(maxSeq,n);};log.poll(split,accept);
  {std::ofstream out(split,std::ios::binary|std::ios::app);out.write(raw.data()+raw.size()-3,3);}log.poll(split,accept);
  if(log.offset!=raw.size()||log.damaged||!maxSeq)throw std::runtime_error("partial WAL replay");
  raw[7]^=1;auto bad=fixture/L"corrupt.bin";{std::ofstream out(bad,std::ios::binary);out.write(raw.data(),raw.size());}
  doubao::Log corrupt;corrupt.poll(bad,accept);if(!corrupt.damaged)throw std::runtime_error("checksum corruption ignored");
  if(engine.tick("",false).state.kind!="unknown")throw std::runtime_error("offline state");
  fs::remove_all(fixture);std::cout<<"9 synthetic Doubao checks passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
