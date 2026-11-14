// Exercise the actual menu command handlers and Windows tray registration using
// isolated synthetic states. No agent configuration, prompts or startup keys.
#include "main.cpp"
#include <iostream>
int main(){int checks=0;auto check=[&](bool condition,const char* message){if(!condition)throw std::runtime_error(message);++checks;};
 HANDLE stop=CreateEventW(nullptr,TRUE,FALSE,nullptr);fs::path temp=fs::temp_directory_path()/(L"CodexLight-lamps-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(now()));
 try {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);MultiApp app(temp,stop,temp/L"codex");
    app.window=CreateWindowExW(0,L"STATIC",L"Lamp integration",0,0,0,0,0,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    for(const auto& style:lamps::styles())for(auto state:{"running","waiting","completed","idle","interrupted","unknown"})app.icons[std::string(style)+":"+state]=lamps::createIcon(style,state);
    app.lastScan=now()+60000;app.foreground=0;
    for(auto& agent:app.agents)agent->present=false;
    app.agents[0]->present=true;app.agents[0]->latest.state.kind="running";
    app.agents[4]->present=true;app.agents[4]->latest.state.kind="waiting";
    app.syncLights();app.update();
    check(app.lights.size()==1 && app.lights[1].registered && app.lights[1].selected==0,"one automatic tray icon by default");
    auto original=app.lights[1].icon.guidItem;
    app.command(1,121,0,{});check(app.settings.items[0].style=="ring" && app.lights[1].registered,"style menu updates real icon");
    check(app.settings.items[0].agent=="auto" && app.lights[1].selected==0,"style preserves automatic binding and active task");
    app.command(1,145,0,{});check(app.lights.size()==2 && app.settings.items[1].style=="circle" && app.settings.items[1].agent=="deepseek","add named lamp with default circle");
    unsigned added=app.settings.items[1].id;
    check(app.lights[added].registered && app.lights[added].selected==4 && app.lights[1].selected==0,"manual second lamp leaves first task alone");
    app.command(added,104,4,{});check(app.lights[added].selected==3,"manual binding does not silently follow another app");
    auto display=readSmall(temp/L"tray-display.json");check(display["lamps"][1]["state"]=="unknown","offline manual target is gray");
    app.command(added,105,3,{});app.command(added,124,4,{});
    MultiApp restored(temp,stop,temp/L"codex");
    check(restored.settings.items.size()==2 && restored.settings.items[1].agent=="deepseek" && restored.settings.items[1].style=="arcane","menu bindings and style survive reload");
    check(memcmp(&original,&app.lights[1].icon.guidItem,sizeof(GUID))==0,"binding and style keep stable tray identity");
    app.command(added,160,4,{});check(app.lights.size()==1 && app.lights[1].registered,"delete only selected lamp");
    app.command(1,160,0,{});check(app.lights.size()==1,"cannot delete last lamp");
    app.command(1,140,0,{});unsigned spare=app.settings.items.back().id;
    check(app.lights[spare].selected==4 && app.lights[1].selected==0,"additional automatic lamp avoids assigned task");
    app.command(spare,122,4,{});check(app.settings.items[0].style=="ring" && app.settings.items.back().style=="glass","styles are independent");
    app.command(spare,100,1,{});check(app.settings.items.back().style=="glass" && app.settings.items.back().agent=="auto","automatic follow preserves chosen style");
    app.command(spare,124,4,{});app.command(spare,101,4,{});
    check(app.settings.items.back().style=="arcane" && app.settings.items.back().agent=="codex","changing binding preserves chosen style");
    app.command(spare,120,0,{});check(app.settings.items.back().style=="circle" && app.settings.items.back().agent=="codex","generic lamp retains explicit binding");
    for(auto& lamp:app.lights)Shell_NotifyIconW(NIM_DELETE,&lamp.second.icon);
    for(auto& icon:app.icons)DestroyIcon(icon.second);DestroyWindow(app.window);
    CoUninitialize();CloseHandle(stop);std::cout<<checks<<" native lamp UI checks passed\nFixture: "<<utf8(temp.wstring())<<'\n';return 0;
 }catch(const std::exception& e){CloseHandle(stop);std::cerr<<e.what()<<'\n';return 1;}}
