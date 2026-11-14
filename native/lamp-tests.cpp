#include "lamps.hpp"
#include <iostream>
using namespace light;
int main(){int count=0;auto check=[&](bool ok,const char* label){if(!ok)throw std::runtime_error(label);++count;};try {
 auto old=lamps::Settings::read(parse(R"({"version":1,"enabled":{"codex":true,"deepseek":true}})"));
 check(old.items.size()==1 && old.items[0].agent=="auto" && old.items[0].style=="circle","old settings migrate to one plain lamp");
 old.items[0].style="glass";old.items[0].agent="auto";check(old.add("deepseek") && old.items[1].style=="circle","new lamp does not inherit custom style");
 auto saved=lamps::Settings::read(parse(old.json().dump()));
 check(saved.items.size()==2 && saved.items[0].style=="glass" && saved.items[0].agent=="auto" && saved.items[1].agent=="deepseek","persist binding and style");
 auto id=saved.items[1].id;check(saved.remove(id) && saved.add("cursor") && saved.items[1].id!=id,"deleted identities are not immediately reused");
 check(!saved.add("invalid"),"invalid binding rejected");
 while(saved.items.size()<8)saved.add("auto");check(!saved.add("auto"),"bounded icon count");
 lamps::Settings only;check(!only.remove(1),"last lamp remains available");
 auto bad=lamps::Settings::read(parse(R"({"version":2,"lamps":[{"id":1,"agent":"invalid"},{"id":2,"agent":"codex","style":"bad"},{"id":2,"agent":"cursor"},{"id":999999,"agent":"auto"}]})"));
 check(bad.items.size()==1 && bad.items[0].style=="circle" && bad.items[0].agent=="codex","malformed records and duplicate ids ignored");
 std::vector<lamps::Candidate> c(5);for(auto& item:c)item.present=true;
 c[0].state.kind="running";c[0].state.started=10;c[1].state.kind="waiting";c[1].state.started=20;
 lamps::Follow f;check(f.choose(c,0,{},100)==0,"initial foreground running task wins");
 check(f.choose(c,1,{},200)==0,"foreground change cannot steal active lamp");
 c[0].state.kind="unknown";check(f.choose(c,1,{},300)==0,"temporary read failure does not abandon active task");
 c[0].state.kind="completed";check(f.choose(c,1,{},400)==0,"completion visible before switching");
 check(f.choose(c,1,{},2399)==0,"completion held two seconds");
 check(f.choose(c,1,{},2401)==1,"follows next waiting task after hold");
 c[1].present=false;check(f.choose(c,1,{},2500)!=1,"closed application releases automatic lamp");
 lamps::Follow spare;c[1].present=true;check(spare.choose(c,1,{1},2600)!=1,"manual reservation prevents duplicate automatic assignment");
 lamps::Follow pending;check(pending.choose(c,-1,{},2700)==1,"waiting task preferred on discovery");
 lamps::Follow empty;std::vector<lamps::Candidate> none(5);check(empty.choose(none,0,{},2800)==-1,"no applications leaves unbound gray lamp");
 c[0].state.kind="completed";c[1].state.kind="completed";
 lamps::Follow idle;check(idle.choose(c,4,{},2900)==4,"idle follows foreground agent");
 check(idle.choose(c,3,{},3000)==3,"idle can follow a different foreground agent");
 for(const auto& style:lamps::styles()) {auto icon=lamps::createIcon(style,"waiting");check(icon!=nullptr,"native style icon can be created");DestroyIcon(icon);}
 auto migrated=lamps::Settings::read(parse(R"({"version":2,"lamps":[{"id":1,"agent":"auto","style":"square"},{"id":2,"agent":"deepseek","style":"diamond"},{"id":3,"agent":"cursor","style":"ring"}]})"));
 check(migrated.items.size()==3 && migrated.items[0].style=="circle" && migrated.items[1].style=="circle" && migrated.items[2].style=="ring","old geometric styles migrate without removing lamps");
 check(migrated.items[0].agent=="auto" && migrated.items[1].agent=="deepseek","migration preserves existing bindings");
 auto identity=lamps::Settings::read(parse(R"({"version":2,"lamps":[{"id":1,"agent":"cursor","style":"codex"}]})"));
 check(identity.items[0].agent=="cursor" && identity.items[0].style=="circle","retired brand style preserves explicit binding");
 auto glossy=lamps::renderIcon("glass","running",16);
 check(glossy.size()==256 && glossy[8*16+8]!=glossy[4*16+6],"small glossy lamp has shaped lighting");
 std::cout<<count<<" lamp checks passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
