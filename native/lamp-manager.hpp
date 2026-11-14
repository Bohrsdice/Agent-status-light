// Included after App inside main.cpp's anonymous namespace.
struct MultiApp {
    struct Lamp {NOTIFYICONDATAW icon{};bool registered=false;lamps::Follow follow;int selected=-1;Json previous;};
    fs::path data;HANDLE stopEvent;HWND window=nullptr;
    std::vector<std::unique_ptr<App>> agents;
    lamps::Settings settings;std::map<unsigned,Lamp> lights;
    std::map<std::string,HICON> icons;
    UINT taskbarCreated=0;Time lastScan=0,lastWrite=0;int foreground=-1;
    bool menuOpen=false;Json previous;
    std::wstring startupKey=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    explicit MultiApp(fs::path directory,HANDLE event,const fs::path& codexHome):data(std::move(directory)),stopEvent(event) {
        settings=lamps::Settings::read(readSmall(data/L"monitor-settings.json"));
        fs::path dsh=environment(L"DSH_HOME");if(dsh.empty())dsh=knownFolder(FOLDERID_Profile)/L".dsh";
        std::vector<fs::path> homes={codexHome,knownFolder(FOLDERID_RoamingAppData)/L"Cursor",
            knownFolder(FOLDERID_LocalAppData)/L"Doubao"/L"User Data"/L"Default"/L"IndexedDB"/L"chrome_doubao-chat_0.indexeddb.leveldb",
            knownFolder(FOLDERID_Profile)/L".workbuddy",dsh};
        for(size_t i=0;i<5;++i) {
            auto app=std::make_unique<App>(homes[i],data/L"agents"/lamps::keys()[i],event,i==2,i==1,i==3,i==4);
            app->managed=true;app->hookData=data;agents.push_back(std::move(app));
        }
    }
    std::wstring startupCommand() const {
        wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
        return L"\""+std::wstring(exe)+L"\" --all --data-dir \""+fs::absolute(data).wstring()+L"\"";
    }
    bool startupEnabled() const {
        wchar_t value[32768]{};DWORD bytes=sizeof(value),type=0;
        return RegGetValueW(HKEY_CURRENT_USER,startupKey.c_str(),L"CodexLight",RRF_RT_REG_SZ,&type,value,&bytes)==ERROR_SUCCESS && std::wstring(value)==startupCommand();
    }
    void setStartup(bool enabled) {
        HKEY key=nullptr;
        if(RegCreateKeyExW(HKEY_CURRENT_USER,startupKey.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)throw std::runtime_error("startup setting");
        auto command=startupCommand();
        auto code=enabled?RegSetValueExW(key,L"CodexLight",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),(DWORD)((command.size()+1)*sizeof(wchar_t))):RegDeleteValueW(key,L"CodexLight");
        RegCloseKey(key);if(code!=ERROR_SUCCESS && code!=ERROR_FILE_NOT_FOUND)throw std::runtime_error("startup setting");
    }
    void initializeStartup() {
        if(settings.startupConfigured)return;
        auto next=settings;next.startupConfigured=true;
        // Migrate existing installations without overriding a previous opt-out.
        next.startup=fs::exists(data/L"monitor-settings.json")?startupEnabled():true;
        bool changed=next.startup && !startupEnabled();if(changed)setStartup(true);
        try{save(std::move(next));}catch(...){if(changed)setStartup(false);throw;}
    }
    void syncLights() {
        std::set<unsigned> ids;for(auto& config:settings.items)ids.insert(config.id);
        for(auto it=lights.begin();it!=lights.end();) {
            if(!ids.count(it->first)){Shell_NotifyIconW(NIM_DELETE,&it->second.icon);it=lights.erase(it);}else ++it;
        }
        for(auto& config:settings.items)if(!lights.count(config.id)) {
            auto& lamp=lights[config.id];auto& icon=lamp.icon;
            icon.cbSize=sizeof(icon);icon.hWnd=window;icon.uID=config.id;icon.uCallbackMessage=WM_APP+1;
            icon.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP|NIF_GUID|NIF_SHOWTIP;
            icon.guidItem=stableIconGuid(data/L"lamps"/std::to_wstring(config.id));
        }
    }
    void save(lamps::Settings next) {
        // Write first: a failed save must not silently change the visible lamps.
        atomicWrite(data/L"monitor-settings.json",next.json());settings=std::move(next);syncLights();previous=nullptr;
    }
    void command(unsigned id,UINT choice,int target,const View& view) {
        try {
            if(choice==1){SetEvent(stopEvent);return;}
            if(choice==2){if(target>=0)agents[target]->openAgent();return;}
            if(choice==210){bool before=startupEnabled();auto next=settings;next.startupConfigured=true;next.startup=!before;setStartup(next.startup);try{save(std::move(next));}catch(...){setStartup(before);throw;}return;}
            if(choice>=500 && choice-500<view.choices.size() && target>=0) {
                std::lock_guard<std::mutex> lock(agents[target]->mutex);agents[target]->manual=view.choices[choice-500].id;return;
            }
            auto next=settings;auto found=std::find_if(next.items.begin(),next.items.end(),[id](const lamps::Config& c){return c.id==id;});
            if(found==next.items.end())return;
            if(choice>=100 && choice<=105){found->agent=choice==100?"auto":lamps::keys()[choice-101];}
            else if(choice>=120 && choice<=124){found->style=lamps::styles()[choice-120];}
            else if(choice>=140 && choice<=145){if(!next.add(choice==140?"auto":lamps::keys()[choice-141]))return;}
            else if(choice==160){if(!next.remove(id))return;}
            else return;
            save(std::move(next));
            if(choice>=100 && choice<=105 && lights.count(id))lights[id].follow=lamps::Follow{};
            update();
        }catch(const std::exception&){MessageBoxW(window,L"设置未能保存，请检查当前用户目录权限。",L"Agent 红绿灯",MB_OK|MB_ICONERROR);}
    }
    void menu(unsigned id) {
        auto config=std::find_if(settings.items.begin(),settings.items.end(),[id](const lamps::Config& c){return c.id==id;});
        if(config==settings.items.end() || !lights.count(id))return;
        menuOpen=true;int target=lights[id].selected;View view;
        if(target>=0){std::lock_guard<std::mutex> lock(agents[target]->mutex);view=agents[target]->latest;}
        using popup::Item;
        std::vector<Item> rows;
        rows.emplace_back(0,L"灯 "+std::to_wstring(id)+L" · "+(target>=0?agents[target]->agentName():L"等待匹配"),false);
        rows.emplace_back(2,L"打开对应窗口",target>=0);
        Item binding(0,L"对应 Agent"),appearance(0,L"样式"),add(0,L"添加灯",settings.items.size()<8);
        binding.children.emplace_back(100,L"自动跟随",true,config->agent=="auto");
        add.children.emplace_back(140,L"自动跟随");
        for(size_t i=0;i<agents.size();++i) {
            auto name=agents[i]->agentName()+(agents[i]->present?L"":L"（未打开）");
            binding.children.emplace_back(101+(UINT)i,name,true,config->agent==agents[i]->agentKey());
            add.children.emplace_back(141+(UINT)i,name);
        }
        for(size_t i=0;i<lamps::styles().size();++i)
            appearance.children.emplace_back(120+(UINT)i,lamps::styleLabels()[i],true,config->style==lamps::styles()[i],icons.at(lamps::styles()[i]+":"+view.state.kind));
        rows.push_back(std::move(binding));rows.push_back(std::move(appearance));rows.push_back(std::move(add));
        rows.emplace_back(160,L"删除这盏灯",settings.items.size()>1);
        if(!view.choices.empty()) {
            Item tasks(0,L"选择任务");
            for(size_t i=0;i<std::min<size_t>(30,view.choices.size());++i) {
                const auto& task=view.choices[i];auto text=stateName(task.state.kind)+L" · "+fs::path(wide(task.project)).filename().wstring()+L" · "+wide(task.id.substr(0,8));
                tasks.children.emplace_back(500+(UINT)i,text,true,task.id==view.id);
            }
            rows.push_back(std::move(tasks));
        }
        rows.push_back(Item::line());rows.emplace_back(210,L"开机启动",true,startupEnabled());rows.emplace_back(1,L"退出");
        POINT point;GetCursorPos(&point);
        auto choice=popup::show(window,rows,point);menuOpen=false;
        command(id,choice,target,view);
    }
    void update() {
        if(WaitForSingleObject(stopEvent,0)==WAIT_OBJECT_0){DestroyWindow(window);return;}
        auto time=now();
        if(!lastScan || time-lastScan>=2000) {
            HWND front=GetForegroundWindow();
            for(int i=0;i<(int)agents.size();++i) {
                agents[i]->present=!agentWindows(agents[i]->agentKey()).empty();
                if(agentOwnsWindow(agents[i]->agentKey(),front))foreground=i;
            }
            lastScan=time;
        }
        std::vector<View> views;std::vector<lamps::Candidate> candidates;Json states=Json::object();
        for(auto& app:agents) {
            View view;{std::lock_guard<std::mutex> lock(app->mutex);view=app->latest;}
            auto raw=view.json();raw["window_present"]=app->present.load();states[app->agentKey()]=raw;
            candidates.push_back({app->present.load(),view.state});views.push_back(std::move(view));
        }
        std::set<int> reserved;
        for(auto& config:settings.items)if(config.agent!="auto")reserved.insert(lamps::agentIndex(config.agent));
        // Existing auto assignments reserve their busy tasks before new lamps choose.
        for(auto& config:settings.items)if(config.agent=="auto") {
            int selected=lights[config.id].follow.selected;
            if(selected>=0 && candidates[selected].present && candidates[selected].state.active())reserved.insert(selected);
        }
        Json displays=Json::array();
        for(auto& config:settings.items) {
            auto& lamp=lights[config.id];
            if(!menuOpen) {
                if(config.agent=="auto") {
                    auto taken=reserved;
                    int old=lamp.follow.selected;
                    // Only release this slot's reservation if no manual slot owns it.
                    bool fixed=false;for(auto& other:settings.items)if(other.agent!="auto" && lamps::agentIndex(other.agent)==old)fixed=true;
                    if(!fixed)taken.erase(old);
                    lamp.selected=lamp.follow.choose(candidates,foreground,taken,time);
                    if(lamp.selected>=0)reserved.insert(lamp.selected);
                }else lamp.selected=lamps::agentIndex(config.agent);
            }
            View view;if(lamp.selected>=0)view=views[lamp.selected];
            auto agent=lamp.selected>=0?agents[lamp.selected]->agentKey():"";
            auto name=lamp.selected>=0?agents[lamp.selected]->agentName():L"等待匹配 Agent";
            if(lamp.selected<0)view.state.reason="右键选择对应 Agent 或等待新任务";
            if(lamp.selected>=0 && !candidates[lamp.selected].present){view=View{};view.state.reason="对应应用未打开";}
            auto tip=L"灯 "+std::to_wstring(config.id)+L" · "+name+L" · "+stateName(view.state.kind)+L"\n"+
                (config.agent=="auto"?L"自动跟随":L"手动指定")+L" · "+wide(view.state.reason);
            if(tip.size()>127)tip.resize(127);
            Json display={{"id",config.id},{"binding",config.agent},{"agent",agent},{"style",config.style},{"state",view.state.kind},{"thread_id",view.id},{"tip",utf8(tip)}};
            if(display!=lamp.previous || !lamp.registered) {
                lamp.icon.hIcon=icons.at(config.style+":"+view.state.kind);wcsncpy_s(lamp.icon.szTip,tip.c_str(),_TRUNCATE);
                if(lamp.registered)lamp.registered=Shell_NotifyIconW(NIM_MODIFY,&lamp.icon)!=FALSE;
                if(!lamp.registered){lamp.registered=Shell_NotifyIconW(NIM_ADD,&lamp.icon)!=FALSE;if(lamp.registered){lamp.icon.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&lamp.icon);}}
                lamp.previous=display;
            }
            wchar_t guid[40]{};StringFromGUID2(lamp.icon.guidItem,guid,40);display["icon_guid"]=utf8(guid);display["icon_registered"]=lamp.registered;displays.push_back(display);
        }
        Json status={{"mode","lamps"},{"agents",states},{"lamps",displays}};
        if(status!=previous || time-lastWrite>=2000) {
            auto output=status;output["observed_at_ms"]=time;output["monitor_pid"]=GetCurrentProcessId();output["implementation"]="native-cpp";
            try {
                atomicWrite(data/L"status.json",output);
                atomicWrite(data/L"tray-display.json",{{"mode","lamps"},{"lamps",displays},{"is_test",false},{"updated_at_ms",time}});
                if(status!=previous)appendBounded(data/L"transitions.jsonl",output);
                previous=status;lastWrite=time;
            }catch(const std::exception&){}
        }
    }
    static LRESULT CALLBACK procedure(HWND h,UINT message,WPARAM wp,LPARAM lp) {
        auto app=reinterpret_cast<MultiApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(message==WM_NCCREATE){app=static_cast<MultiApp*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);app->window=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}
        if(app)try {
            if(message==WM_TIMER){app->update();return 0;}
            if(message==WM_APP+1) {
                unsigned id=HIWORD(lp);auto found=app->lights.find(id);if(found==app->lights.end())return 0;
                if(LOWORD(lp)==WM_RBUTTONUP || LOWORD(lp)==WM_CONTEXTMENU)app->menu(id);
                else if(LOWORD(lp)==NIN_SELECT || LOWORD(lp)==NIN_KEYSELECT){int target=found->second.selected;if(target>=0)app->agents[target]->openAgent();else app->menu(id);}
                return 0;
            }
            if(message==app->taskbarCreated){for(auto& light:app->lights)light.second.registered=false;app->update();return 0;}
            if(message==WM_CLOSE){SetEvent(app->stopEvent);return 0;}
            if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
        }catch(const std::exception&){}
        return DefWindowProcW(h,message,wp,lp);
    }
    int run() {
        fs::create_directories(data);taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
        for(const auto& style:lamps::styles())for(const auto& state:{"running","waiting","completed","idle","interrupted","unknown"}) {
            auto icon=lamps::createIcon(style,state);if(!icon)throw std::runtime_error("lamp icon");icons[std::string(style)+":"+state]=icon;
        }
        WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"CodexLight.Native.Manager";cls.hIcon=LoadIconW(cls.hInstance,MAKEINTRESOURCEW(1));
        if(!RegisterClassW(&cls))throw std::runtime_error("manager window class");
        window=CreateWindowExW(0,cls.lpszClassName,L"",0,0,0,0,0,nullptr,nullptr,cls.hInstance,this);if(!window)throw std::runtime_error("manager window");
        try{initializeStartup();}catch(const std::exception&){try{atomicWrite(data/L"autostart-error.json",{{"reason","无法设置开机启动；监控继续运行，可从菜单重试"},{"time_ms",now()}});}catch(const std::exception&){} }
        for(auto& app:agents){app->present=!agentWindows(app->agentKey()).empty();app->start();}
        syncLights();update();SetTimer(window,1,500,nullptr);
        MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
        SetEvent(stopEvent);KillTimer(window,1);
        for(auto& light:lights)Shell_NotifyIconW(NIM_DELETE,&light.second.icon);
        for(auto& app:agents)app->finish();for(auto& icon:icons)DestroyIcon(icon.second);icons.clear();
        try {atomicWrite(data/L"status.json",{{"mode","lamps"},{"state","unknown"},{"reason","监控器已关闭"},{"observed_at_ms",now()}});}catch(const std::exception&){}
        return 0;
    }
};
