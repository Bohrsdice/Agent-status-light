#include "deepseek.hpp"
#include <fstream>
#include <iostream>
using namespace light;
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);++checks;}while(0)
int main(int argc,char** argv){int checks=0;auto temp=fs::temp_directory_path()/(L"deepseek-light-test-"+std::to_wstring(GetCurrentProcessId()));
try{
 fs::create_directories(temp);Json header={{"type","session"},{"version",4},{"id","test"},{"cwd","C:\\中文"},{"isSeeded",false}};
 auto event=[](long long seq,const char* type,Json data){return Json{{"seq",seq},{"type",type},{"time",Time(1000+seq)},{"data",data}};};
 deepseek::Lifecycle life;life.consume(header);CHECK(life.identity && life.root);
 life.consume(event(0,"turn/start",{{"turn",1}}));CHECK(life.state.kind=="running");
 life.consume(event(1,"tool/call",{{"turn",1},{"name","ask_user_question"},{"callId","q"}}));CHECK(life.state.kind=="waiting");
 life.consume(event(2,"approval/asked",{{"id","p"}}));CHECK(life.state.pending==2);
 life.consume(event(3,"tool/result",{{"turn",1},{"message",{{"toolCallId","wrong"}}}}));CHECK(life.state.pending==2);
 life.consume(event(4,"tool/result",{{"turn",1},{"message",{{"toolCallId","q"}}}}));CHECK(life.state.kind=="waiting" && life.state.pending==1);
 life.consume(event(5,"approval/decided",{{"id","p"},{"outcome","rejected"}}));CHECK(life.state.kind=="running");
 life.consume(event(6,"turn/end",{{"turn",1},{"reason",{{"kind","completed"}}}}));CHECK(life.state.kind=="completed");
 life.consume(event(7,"tool/call",{{"turn",1},{"name","ask_user_question"},{"callId","late"}}));CHECK(life.state.kind=="completed");
 life.consume(event(8,"turn/start",{{"turn",2}}));life.consume(event(9,"turn/end",{{"turn",1},{"reason",{{"kind","completed"}}}}));CHECK(life.state.kind=="running");
 life.consume(event(10,"turn/end",{{"turn",2},{"reason",{{"kind","aborted"}}}}));CHECK(life.state.kind=="interrupted");
 life.consume(event(12,"turn/start",{{"turn",3}}));CHECK(life.damaged);
 deepseek::Lifecycle child;auto sub=header;sub["origin"]="subagent";child.consume(sub);child.consume(event(0,"turn/start",{{"turn",1}}));CHECK(!child.root && !child.state.active());
 deepseek::Lifecycle fork;auto seeded=header;seeded["isSeeded"]=true;fork.consume(seeded);fork.consume(event(0,"turn/start",{{"turn",1}}));CHECK(!fork.state.active());
 fork.consume(event(1,"session/end-seed",Json::object()));fork.consume(event(2,"turn/start",{{"turn",2}}));CHECK(fork.state.kind=="running");
 deepseek::Lifecycle unsupported;auto future=header;future["version"]=5;unsupported.consume(future);CHECK(unsupported.damaged);
 deepseek::Lifecycle orphan;orphan.consume(header);orphan.consume(event(0,"turn/end",{{"turn",1},{"reason",{{"kind","completed"}}}}));CHECK(orphan.state.kind=="unknown");
 CHECK(deepseek::generation(L"session.v4.jsonl.zstd")==4);CHECK(deepseek::generation(L"session.v10.jsonl")==10);CHECK(deepseek::generation(L"not-session.jsonl")==-1);
 if(argc<2)throw std::runtime_error("fixture directory required");auto fixtures=fs::path(argv[1]);
 auto bytes=[](const fs::path& path){std::ifstream in(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{});};
 auto append=[](const fs::path& path,const std::string& value,bool replace=false){std::ofstream out(path,std::ios::binary|(replace?std::ios::trunc:std::ios::app));out.write(value.data(),value.size());};
 auto file=temp/L"session.v4.jsonl.zstd";deepseek::Reader reader;
 append(file,bytes(fixtures/L"start.zstd"),true);reader.poll(file);CHECK(reader.lifecycle.state.kind=="running" && !reader.catchingUp);
 auto wait=bytes(fixtures/L"wait.zstd");append(file,wait.substr(0,wait.size()/2));reader.poll(file);CHECK(reader.catchingUp && reader.lifecycle.state.kind=="running");
 append(file,wait.substr(wait.size()/2));reader.poll(file);CHECK(!reader.catchingUp && reader.lifecycle.state.kind=="waiting");
 append(file,bytes(fixtures/L"answer.zstd"));reader.poll(file);CHECK(reader.lifecycle.state.kind=="running");
 append(file,bytes(fixtures/L"complete.zstd"));reader.poll(file);CHECK(reader.lifecycle.state.kind=="completed");
 reader.poll(file);CHECK(!reader.lifecycle.damaged && reader.lifecycle.state.kind=="completed");
 auto bad=bytes(fixtures/L"start.zstd");bad.back()^=1;auto corrupt=temp/L"bad.zstd";append(corrupt,bad,true);deepseek::Reader broken;
 bool rejected=false;try{broken.poll(corrupt);}catch(...){rejected=true;}CHECK(rejected && broken.lifecycle.damaged);
 append(file,bytes(fixtures/L"start.zstd"),true);reader.poll(file);CHECK(!reader.lifecycle.damaged && reader.lifecycle.state.kind=="running");
 auto raw=temp/L"raw.jsonl";append(raw,header.dump()+"\n"+event(0,"turn/start",{{"turn",1}}).dump()+"\n",true);deepseek::Reader plain;plain.poll(raw);CHECK(plain.lifecycle.state.kind=="running");
 auto home=temp/L"home";auto session=home/L"sessions"/L"project"/L"test";fs::create_directories(session);append(session/L"session.v4.jsonl.zstd",bytes(fixtures/L"start.zstd"),true);
 Runtime runtime;runtime.online=true;runtime.started=1;deepseek::Engine engine(home);CHECK(engine.tick(runtime).state.kind=="running");
 runtime.started=999999;CHECK(engine.tick(runtime).state.kind=="unknown");runtime.online=false;CHECK(!engine.tick(runtime).online);
 fs::remove_all(temp);std::cout<<checks<<" DeepSeek checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
