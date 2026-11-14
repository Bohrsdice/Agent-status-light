#include "workbuddy.hpp"
#include <fstream>
#include <iostream>
using namespace light;
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);++checks;}while(0)
std::string event(const char* signal,const char* to,int seq=1,int second=1){return "[2026/11/14 10:50:"+std::string(second<10?"0":"")+std::to_string(second)+".123] [Info] [SessionManager] task state transition "+Json({{"sessionId","test"},{"signal",signal},{"to",to},{"turnSeq",seq}}).dump()+"\n";}
int main(){int checks=0;auto temp=fs::temp_directory_path()/(L"workbuddy-light-test-"+std::to_wstring(GetCurrentProcessId()));
try {
 workbuddy::Lifecycle state;auto start=event("turn_started","planning");auto time=workbuddy::timestamp(start);CHECK(time>0);
 state.consume(start,time+1,"test");CHECK(state.candidates.empty());
 state.consume(event("created","pending",0),0,"test");CHECK(state.candidates.empty());
 state.consume(start,0,"test");CHECK(state.candidates["test"].state.kind=="running");
 state.consume(event("awaiting_input","pending",1,2),0,"test");CHECK(state.candidates["test"].state.kind=="waiting");
 state.consume(event("input_resolved","working",1,3),0,"test");CHECK(state.candidates["test"].state.kind=="running");
 state.consume(event("turn_completed","completed",1,4),0,"test");CHECK(state.candidates["test"].state.kind=="completed");
 state.consume(event("turn_started","planning",2,5),0,"test");state.consume(event("turn_completed","completed",1,6),0,"test");CHECK(state.candidates["test"].state.kind=="running");
 state.consume(event("terminated","terminated",2,7),0,"test");CHECK(state.candidates["test"].state.kind=="interrupted");
 state.consume(start,0,"test");CHECK(state.candidates["test"].state.kind=="interrupted");
 state.consume("[2026/11/14 10:50:08.123] [Info] [SessionManager] task state transition BROKEN",0,"test");CHECK(state.damaged);
 state.consume(event("turn_started","planning",3,9),0,"test");CHECK(!state.damaged);
 auto log=temp/L"logs"/L"2026-11-14"/L"workbuddyMainThread__test.log";fs::create_directories(log.parent_path());
 auto put=[&](const std::string& value,bool append){std::ofstream out(log,std::ios::binary|(append?std::ios::app:std::ios::trunc));out<<value;};
 put(start.substr(0,start.size()/2),false);workbuddy::Log reader;reader.poll(log,0);CHECK(reader.lifecycle.candidates.empty());
 put(start.substr(start.size()/2),true);reader.poll(log,0);CHECK(reader.lifecycle.candidates["test"].state.kind=="running");
 put(event("awaiting_input","pending",1,2),true);reader.poll(log,0);CHECK(reader.lifecycle.candidates["test"].state.kind=="waiting");
 put("",false);reader.poll(log,0);CHECK(reader.lifecycle.candidates.empty());
 put(start+event("turn_completed","completed",1,4),true);
 workbuddy::Engine engine(temp);Runtime runtime;runtime.online=true;runtime.started=time-1;
 CHECK(engine.tick(runtime).state.kind=="completed");runtime.started=time+10000;CHECK(engine.tick(runtime).state.kind=="unknown");runtime.online=false;CHECK(!engine.tick(runtime).online);
 auto data=temp/L"data";Json hook={{"session_id","hook-test"},{"hook_event_name","UserPromptSubmit"},{"prompt","SECRET"}};
 workbuddy::record(data,hook);auto readState=[&](){return cursor::read(data/L"workbuddy-hooks-state.json");};
 CHECK(readState()["sessions"]["hook-test"]["state"]=="running");CHECK(readState().dump().find("SECRET")==std::string::npos);
 hook["hook_event_name"]="PreToolUse";hook["tool_name"]="AskUserQuestion";workbuddy::record(data,hook);CHECK(readState()["sessions"]["hook-test"]["state"]=="waiting");
 hook["hook_event_name"]="PostToolUse";workbuddy::record(data,hook);CHECK(readState()["sessions"]["hook-test"]["state"]=="running");
 hook["hook_event_name"]="PreToolUse";workbuddy::record(data,hook);CHECK(readState()["sessions"]["hook-test"]["state"]=="waiting");
 hook["hook_event_name"]="Stop";workbuddy::record(data,hook);CHECK(readState()["sessions"]["hook-test"]["state"]=="completed");
 hook["session_id"]="hook-test";hook["agent_id"]="child";hook["hook_event_name"]="UserPromptSubmit";workbuddy::record(data,hook);CHECK(readState()["sessions"].size()==1);
 hook["agent_id"]="hook-test";workbuddy::record(data,hook);CHECK(readState()["sessions"]["hook-test"]["state"]=="running");
 hook["hook_event_name"]="Notification";hook["notification_type"]="permission_prompt";hook["message"]="needs your permission to use AskUserQuestion";workbuddy::record(data,hook);CHECK(readState()["sessions"]["hook-test"]["state"]=="waiting");
 hook["hook_event_name"]="PostToolUse";hook["tool_use_id"]="modern-call-id";workbuddy::record(data,hook);CHECK(readState()["sessions"]["hook-test"]["state"]=="running");
 auto config=temp/L"settings.json";auto exe=temp/L"a'b 空格.exe";
 Json original={{"sandbox",true},{"hooks",{{"Stop",Json::array({{{"hooks",Json::array({{{"command","existing"},{"type","command"}}})}}})}}}};
 atomicWrite(config,original);CHECK(workbuddy::configure(config,exe,data));CHECK(!workbuddy::configure(config,exe,data));CHECK(cursor::read(config)["sandbox"]==true);
 CHECK(workbuddy::configure(config,exe,data,true));CHECK(cursor::read(config)==original);
 fs::remove_all(temp);std::cout<<checks<<" WorkBuddy checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
