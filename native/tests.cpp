#include "core.hpp"
#include <fstream>
#include <iostream>
#include <functional>
#include <stdexcept>
using namespace light;

static int passed=0;
#define CHECK(condition) do {if (!(condition)) throw std::runtime_error(#condition);} while(0)
static Json metadata(std::string id="a",bool child=false) {
    Json p={{"id",id},{"cwd","C:\\测试项目"},{"source","vscode"}};
    if (child) p["source"]={{"subagent",{{"parent_thread_id","parent"}}}};
    return {{"type","session_meta"},{"payload",p}};
}
static Json event(std::string type,std::string turn="t",std::string thread="a") {
    return {{"type","event_msg"},{"timestamp","2026-11-13T08:30:00.123Z"},
            {"payload",{{"type",type},{"turn_id",turn},{"thread_id",thread}}}};
}
static Json question(std::string call="q",int count=1) {
    auto r=event("item_completed");Json questions=Json::array();
    for(int i=0;i<count;++i) questions.push_back({{"title","确认？"}});
    r["payload"]["item"]={{"type","AgentMessage"},{"id",call},{"delivery","async"},{"questions",questions}};
    return r;
}
static Json reply(std::string call="q",int index=0) {
    auto r=event("item_completed");
    Json key=Json::array({"request_user_input_async",call,index});
    Json answers=Json::array({{{"questionItemId",key.dump()},{"answer","确认"}}});
    auto text="<send_user_message_question_reply>\n"+answers.dump()+"\n</send_user_message_question_reply>";
    r["payload"]["item"]={{"type","UserMessage"},{"content",Json::array({{{"type","text"},{"text",text}}})}};
    return r;
}
static Lifecycle live() {Lifecycle l;l.consume(metadata());l.consume(event("task_started"));return l;}
static void test(const char* name,std::function<void()> body) {
    body();++passed;std::cout<<"PASS "<<name<<"\n";
}
static void write(const fs::path& p,const std::string& data,bool append=false) {
    fs::create_directories(p.parent_path());std::ofstream o(p,std::ios::binary|(append?std::ios::app:std::ios::trunc));o<<data;
}
static std::string lines(std::initializer_list<Json> records) {std::string s;for(const auto&r:records)s+=r.dump()+"\n";return s;}
struct Fixture {
    fs::path root;
    Fixture() {root=fs::absolute(fs::path(L"testdata")/(L"case-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())));fs::create_directories(root);}
    ~Fixture() {
        // Only this test's explicitly created child of the testdata directory.
        if(root.parent_path()==fs::absolute(L"testdata") && root.filename().wstring().find(L"case-")==0) {
            std::error_code ec;fs::remove_all(root,ec);
        }
    }
};
static Candidate candidate(std::string id,std::string state,Time start=200) {
    Candidate c;c.id=id;c.state.kind=state;c.state.started=start;return c;
}

int main() {
    try {
        test("UTC fractional timestamps",[]{CHECK(parseTime("2026-11-13T08:30:00.123Z")-parseTime("2026-11-13T08:30:00Z")==123);CHECK(parseTime("bad")==0);});
        test("start and completion",[]{auto l=live();CHECK(l.state.kind=="running");l.consume(event("task_complete"));CHECK(l.state.kind=="completed");});
        test("late completion cannot finish newer turn",[]{auto l=live();l.consume(event("task_started","new"));l.consume(event("task_complete"));CHECK(l.state.kind=="running");CHECK(l.state.turn=="new");});
        test("wrong thread ignored",[]{auto l=live();l.consume(event("task_complete","t","other"));CHECK(l.state.kind=="running");});
        test("orphan completion unknown",[]{Lifecycle l;l.consume(metadata());l.consume(event("task_complete"));CHECK(l.state.kind=="unknown");});
        test("abort never green",[]{auto l=live();l.consume(event("turn_aborted"));l.consume(event("task_complete"));CHECK(l.state.kind=="interrupted");});
        test("tool completion not task completion",[]{auto l=live();l.consume(event("item_completed"));CHECK(l.state.kind=="running");});
        test("question yellow answer red complete green",[]{auto l=live();l.consume(question());CHECK(l.state.kind=="waiting");l.consume(reply());CHECK(l.state.kind=="running");l.consume(event("task_complete"));CHECK(l.state.kind=="completed");});
        test("all questions answered",[]{auto l=live();l.consume(question("q",2));l.consume(reply());CHECK(l.state.pending==1);CHECK(l.state.kind=="waiting");l.consume(reply("q",1));CHECK(l.state.kind=="running");});
        test("unanswered question survives turn completion",[]{auto l=live();l.consume(question());l.consume(event("task_complete"));CHECK(l.state.kind=="waiting");l.consume(reply());CHECK(l.state.kind=="completed");});
        test("wrong question cannot clear waiting",[]{auto l=live();l.consume(question());l.consume(reply("wrong"));CHECK(l.state.kind=="waiting");});
        test("duplicate question does not reopen answered card",[]{auto l=live();l.consume(question());l.consume(reply());l.consume(question());CHECK(l.state.kind=="running");});
        test("new turn clears old questions",[]{auto l=live();l.consume(question());l.consume(event("task_started","new"));l.consume(reply());CHECK(l.state.pending==0);CHECK(l.state.kind=="running");});
        test("interrupt clears waiting",[]{auto l=live();l.consume(question());l.consume(event("turn_aborted"));l.consume(reply());CHECK(l.state.kind=="interrupted");});
        test("child sessions identified",[]{Lifecycle l;l.consume(metadata("a",true));CHECK(!l.root);});
        test("partial JSON and UTF8 preserve state",[]{
            Fixture f;auto p=f.root/L"s.jsonl";write(p,lines({metadata()}));Reader r(p);r.poll();
            auto start=event("task_started");start["payload"]["note"]="中文";auto bytes=start.dump()+"\n";
            auto cut=bytes.find("中")+1;write(p,bytes.substr(0,cut),true);r.poll();CHECK(r.lifecycle.state.kind=="unknown");
            write(p,bytes.substr(cut),true);r.poll();CHECK(r.lifecycle.state.kind=="running");
            auto end=event("task_complete").dump();write(p,end,true);r.poll();CHECK(r.lifecycle.state.kind=="running");
            write(p,"\n",true);r.poll();CHECK(r.lifecycle.state.kind=="completed");
        });
        test("corrupt records fail closed then new start recovers",[]{Fixture f;auto p=f.root/L"s";write(p,lines({metadata(),event("task_started")}));Reader r(p);r.poll();write(p,"broken\n",true);r.poll();CHECK(!r.error.empty());write(p,lines({event("task_started","new")}),true);r.poll();CHECK(r.error.empty());});
        test("truncation resets old green",[]{Fixture f;auto p=f.root/L"s";write(p,lines({metadata(),event("task_started"),event("task_complete")}));Reader r(p);r.poll();CHECK(r.lifecycle.state.kind=="completed");write(p,lines({metadata()}));r.poll();CHECK(r.lifecycle.state.kind=="unknown");});
        test("file replacement resets old green",[]{Fixture f;auto p=f.root/L"s",q=f.root/L"new";write(p,lines({metadata(),event("task_started"),event("task_complete")}));Reader r(p);r.poll();write(q,lines({metadata(),event("task_started","new")}));CHECK(MoveFileExW(q.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING));r.poll();CHECK(r.lifecycle.state.kind=="running");CHECK(r.lifecycle.state.turn=="new");});
        test("long active session streamed after tail miss",[]{Fixture f;auto p=f.root/L"s";write(p,lines({metadata(),event("task_started")}));Json noise={{"type","response_item"},{"payload",{{"text",std::string(400000,'x')}}}};write(p,lines({noise,noise,noise}),true);Reader r(p);r.poll();CHECK(r.catchingUp);r.poll();CHECK(!r.catchingUp);CHECK(r.lifecycle.state.kind=="running");});
        test("single active task auto selected",[]{Selector s(100);auto v=s.choose({candidate("a","running")},1000);CHECK(v.id=="a");});
        test("another task cannot steal active lamp",[]{Selector s(100);s.choose({candidate("a","running")},1000);auto v=s.choose({candidate("a","running"),candidate("b","running",300)},2000);CHECK(v.id=="a");});
        test("initial multiple tasks require explicit selection",[]{Selector s(100);auto c=std::vector<Candidate>{candidate("a","running"),candidate("b","running")};CHECK(s.choose(c,1000).id.empty());CHECK(s.choose(c,2000,"b").id=="b");});
        test("finished lamp held before switching",[]{Selector s(100);s.choose({candidate("a","running")},1000);auto c=std::vector<Candidate>{candidate("a","completed"),candidate("b","running")};CHECK(s.choose(c,2000).id=="a");CHECK(s.choose(c,4500).id=="b");});
        test("unknown selected task does not switch unexpectedly",[]{Selector s(100);s.choose({candidate("a","running")},1000);CHECK(s.choose({candidate("a","unknown"),candidate("b","running")},5000).id=="a");});
        test("old modified history not selected",[]{Selector s(100);CHECK(s.choose({candidate("old","completed",50)},1000).id.empty());});
        test("fast task completed between polls retained",[]{Selector s(100);CHECK(s.choose({candidate("fast","completed",500)},1000).id=="fast");});
        test("discovery root relocation and stale process guard",[]{
            Fixture f;auto p=f.root/L"sessions"/L"2026"/L"s.jsonl";write(p,lines({metadata(),event("task_started")}));
            auto t=parseTime("2026-11-13T08:30:00.123Z");Engine engine(f.root,t);Runtime rt{true,t-1000,"online"};
            auto v=engine.tick(rt,"",t);CHECK(v.id=="a");CHECK(v.state.kind=="running");
            rt.started=t+1;v=engine.tick(rt,"",t+1);CHECK(v.state.kind=="unknown");
            rt.online=false;v=engine.tick(rt,"",t+2);CHECK(v.state.kind=="unknown");
        });
        test("new session discovered without restarting",[]{
            Fixture f;fs::create_directories(f.root/L"sessions");auto t=parseTime("2026-11-13T08:30:00.123Z");Engine engine(f.root,t);Runtime rt{true,t-1000,"online"};CHECK(engine.tick(rt,"",t).id.empty());
            write(f.root/L"sessions"/L"new.jsonl",lines({metadata(),event("task_started")}));CHECK(engine.tick(rt,"",t+3100).id=="a");
        });
        test("subagent cannot make startup ambiguous",[]{Fixture f;auto t=parseTime("2026-11-13T08:30:00.123Z");write(f.root/L"sessions"/L"root.jsonl",lines({metadata(),event("task_started")}));write(f.root/L"sessions"/L"child.jsonl",lines({metadata("child",true),event("task_started","x","child")}));Engine engine(f.root,t);Runtime rt{true,t-1000,"online"};auto v=engine.tick(rt,"",t);CHECK(v.id=="a");CHECK(v.choices.size()==1);});
        std::cout<<"ALL "<<passed<<" TESTS PASSED\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL after "<<passed<<": "<<e.what()<<"\n";return 1;}
}
