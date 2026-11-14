#include "cursor.hpp"
#include <iostream>
using namespace light;
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);++checks;}while(0)
int main(){int checks=0;fs::path temp=fs::temp_directory_path()/(L"cursor-light-test-"+std::to_wstring(GetCurrentProcessId()));
try {
 Json all=Json::object();Time t=now();
 Json start={{"conversation_id","a"},{"generation_id","g1"},{"hook_event_name","beforeSubmitPrompt"},{"prompt","SECRET"},{"workspace_roots",{"C:\\test"}}};
 CHECK(cursor::consume(all,start,t));CHECK(all.dump().find("SECRET")==std::string::npos);
 CHECK(!cursor::consume(all,start,t+1));
 auto q=start;q["hook_event_name"]="preToolUse";q["tool_name"]="AskQuestion";q["tool_use_id"]="q1";q["tool_input"]={{"run_async",false}};
 CHECK(cursor::consume(all,q,t+2));CHECK(all["sessions"]["a"]["state"]=="waiting");
 auto q2=q;q2["tool_use_id"]="q2";cursor::consume(all,q2,t+3);
 auto answer=q;answer["hook_event_name"]="postToolUse";cursor::consume(all,answer,t+4);
 CHECK(all["sessions"]["a"]["state"]=="waiting");
 CHECK(!cursor::consume(all,q,t+5));
 answer["tool_use_id"]="q2";cursor::consume(all,answer,t+6);CHECK(all["sessions"]["a"]["state"]=="running");
 q["tool_use_id"]="async";q["tool_input"]["run_async"]=true;CHECK(!cursor::consume(all,q,t+7));
 auto stop=start;stop["hook_event_name"]="stop";stop["status"]="completed";cursor::consume(all,stop,t+8);CHECK(all["sessions"]["a"]["state"]=="completed");
 CHECK(!cursor::consume(all,answer,t+9));
 start["generation_id"]="g2";cursor::consume(all,start,t+10);CHECK(!cursor::consume(all,stop,t+11));
 stop["generation_id"]="g2";stop["status"]="aborted";cursor::consume(all,stop,t+12);CHECK(all["sessions"]["a"]["state"]=="interrupted");
 auto config=temp/L"hooks.json";auto exe=temp/L"a'b 空格.exe";auto data=temp/L"data";
 Json original={{"version",1},{"custom",true},{"hooks",{{"stop",Json::array({{{"command","existing-hook"}}})}}}};
 atomicWrite(config,original);CHECK(cursor::configure(config,exe,data));CHECK(!cursor::configure(config,exe,data));
 auto installed=cursor::read(config);CHECK(installed["hooks"]["stop"][0]["command"]=="existing-hook");
 CHECK(installed["hooks"]["stop"][1]["command"].get<std::string>().find("a''b")!=std::string::npos);
 CHECK(installed["hooks"]["preToolUse"][0]["matcher"]=="^AskQuestion$");
 CHECK(cursor::configure(config,exe,data,true));CHECK(cursor::read(config)==original);
 atomicWrite(config,{{"version",2}});bool rejected=false;try{cursor::configure(config,exe,data);}catch(...){rejected=true;}CHECK(rejected);CHECK(cursor::read(config)["version"]==2);
 atomicWrite(data/L"cursor-state.json",all);cursor::Engine engine(data);Runtime runtime;runtime.online=true;runtime.started=t-1;
 CHECK(engine.tick(runtime).state.kind=="interrupted");runtime.started=t+20;CHECK(engine.tick(runtime).state.kind=="unknown");runtime.online=false;CHECK(!engine.tick(runtime).online);
 auto bounded=Json::object();for(int i=0;i<150;++i){start["conversation_id"]=std::to_string(i);cursor::consume(bounded,start,t+i);}CHECK(bounded["sessions"].size()==128);
 // Real Cursor hook-log framing, including a split write and original UTF-8.
 Json input={{"conversation_id","native"},{"generation_id","native-g"},{"hook_event_name","beforeSubmitPrompt"},{"prompt","中文 SECRET"},{"workspace_roots",{"C:\\项目"}}};
 auto framing="[2026-11-14T03:20:19.073Z] Executing hook 1/1\nCommand: example\nINPUT:\n"+input.dump(2)+"\nOUTPUT:\n{}\n";
 auto log=temp/L"native.log";
 {std::ofstream out(log,std::ios::binary);out<<framing.substr(0,framing.size()/2);}
 cursorNative::Log nativeLog;std::vector<cursorNative::Event> events;nativeLog.poll(log,events);CHECK(events.empty());
 {std::ofstream out(log,std::ios::binary|std::ios::app);out<<framing.substr(framing.size()/2);}
 nativeLog.poll(log,events);CHECK(events.size()==1);CHECK(events[0].time==parseTime("2026-11-14T03:20:19.073Z"));
 CHECK(events[0].value.dump().find("SECRET")==std::string::npos);CHECK(events[0].value["workspace_roots"][0]=="C:\\项目");
 events.clear();nativeLog.poll(log,events);CHECK(events.empty());
 {std::ofstream out(log,std::ios::binary);out<<"[2026-11-14T03:21:00.000Z] hook\nINPUT:\n{\n bad json\n}\n";}
 nativeLog.poll(log,events);CHECK(events.empty());
 Json composer={{"latestChatGenerationUUID","native-g"},{"fullConversationHeadersOnly",Json::array({
  {{"bubbleId","old-question"},{"type",2},{"grouping",{{"toolCallCase","askQuestionToolCall"}}}},
  {{"bubbleId","human"},{"type",1}},
  {{"bubbleId","question"},{"type",2},{"grouping",{{"toolCallCase","askQuestionToolCall"}}}}
 })}};
 CHECK(cursorNative::questionBubbles(composer,"native-g")==std::vector<std::string>{"question"});
 CHECK(cursorNative::questionBubbles(composer,"old-g").empty());
 Json bubble={{"toolFormerData",{{"name","ask_question"},{"status","loading"},{"params","{}"},{"additionalData",{{"status","pending"}}}}}};
 CHECK(cursorNative::pendingQuestion(bubble));bubble["toolFormerData"]["status"]="completed";CHECK(cursorNative::pendingQuestion(bubble));
 bubble["toolFormerData"]["params"]="{\"run_async\":true}";CHECK(!cursorNative::pendingQuestion(bubble));bubble["toolFormerData"]["params"]="{}";
 bubble["toolFormerData"]["userDecision"]="accepted";CHECK(!cursorNative::pendingQuestion(bubble));bubble["toolFormerData"].erase("userDecision");
 bubble["toolFormerData"]["additionalData"]["status"]="submitted";CHECK(!cursorNative::pendingQuestion(bubble));bubble["toolFormerData"]["additionalData"]["status"]="pending";
 bubble["toolFormerData"]["result"]="{}";CHECK(!cursorNative::pendingQuestion(bubble));bubble["toolFormerData"].erase("result");
 // Exercise Windows' system SQLite against a writable fixture; production opens read-only.
 auto root=temp/L"Cursor";auto database=root/L"User"/L"globalStorage"/L"state.vscdb";fs::create_directories(database.parent_path());
 cursorNative::Sqlite sql;CHECK(sql.available());void* db=nullptr;
 CHECK(sql.open(utf8(database.wstring()).c_str(),&db,6,nullptr)==0);
 auto execute=[&](const char* query,const std::string& key="",const std::string& value=""){
  void* statement=nullptr;if(sql.prepare(db,query,-1,&statement,nullptr)!=0)throw std::runtime_error("fixture prepare");
  if(!key.empty()){sql.bind(statement,1,key.c_str(),(int)key.size(),nullptr);sql.bind(statement,2,value.c_str(),(int)value.size(),nullptr);}
  int result=sql.step(statement);sql.finalize(statement);if(result!=101)throw std::runtime_error("fixture step");
 };
 execute("CREATE TABLE cursorDiskKV(key TEXT PRIMARY KEY,value BLOB)");
 execute("INSERT INTO cursorDiskKV VALUES(?1,?2)","composerData:native",composer.dump());
 execute("INSERT INTO cursorDiskKV VALUES(?1,?2)","bubbleId:native:question",bubble.dump());
 cursorNative::Questions questions(root);CHECK(questions.pending("native","native-g")==1);CHECK(questions.pending("native","old-g")==0);
 bubble["toolFormerData"]["additionalData"]["status"]="submitted";
 execute("UPDATE cursorDiskKV SET value=?2 WHERE key=?1","bubbleId:native:question",bubble.dump());
 CHECK(questions.pending("native","native-g")==0);sql.close(db);
 std::cout<<checks<<" Cursor checks passed\n";
 fs::remove_all(temp);return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
