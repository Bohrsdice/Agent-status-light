#pragma once
#include "core.hpp"
#include <algorithm>
#include <cmath>

namespace light::lamps {
inline const std::vector<std::string>& keys(){static const std::vector<std::string> v={"codex","cursor","doubao","workbuddy","deepseek"};return v;}
inline int agentIndex(const std::string& key){auto& v=keys();auto it=std::find(v.begin(),v.end(),key);return it==v.end()?-1:(int)(it-v.begin());}
inline const std::vector<std::string>& styles(){static const std::vector<std::string> v={"circle","ring","glass","porous","arcane"};return v;}
inline const std::vector<std::wstring>& styleLabels(){static const std::vector<std::wstring> v={L"标准圆灯",L"圆圈灯",L"拟态圆灯",L"多孔圆灯",L"法阵圆灯"};return v;}
inline bool validStyle(const std::string& s){auto& v=styles();return std::find(v.begin(),v.end(),s)!=v.end();}
struct Config {unsigned id=1;std::string agent="auto",style="circle";};
struct Settings {
    std::vector<Config> items{{}};unsigned nextId=2;
    static Settings read(const Json& value) {
        Settings result;
        // Previous automatic multi-icon settings deliberately migrate to one lamp.
        if(!value.is_object() || !value.contains("version") || value["version"]!=2 || !value.contains("lamps") || !value["lamps"].is_array())return result;
        std::vector<Config> items;std::set<unsigned> ids;
        for(const auto& item:value["lamps"]) {
            if(items.size()>=8)break;
            if(!item.is_object() || !item.contains("id") || !item["id"].is_number_unsigned())continue;
            auto id=item["id"].get<uint64_t>();auto agent=field(item,"agent"),style=field(item,"style");
            if(!id || id>60000 || !ids.insert((unsigned)id).second || (agent!="auto" && agentIndex(agent)<0))continue;
            if(!validStyle(style))style="circle";
            items.push_back({(unsigned)id,agent,style});
        }
        if(!items.empty())result.items=std::move(items);
        unsigned greatest=0;for(auto& item:result.items)greatest=std::max(greatest,item.id);
        result.nextId=greatest+1;
        if(value.contains("next_id") && value["next_id"].is_number_unsigned()) {
            auto next=value["next_id"].get<uint64_t>();if(next>greatest && next<=60000)result.nextId=(unsigned)next;
        }
        return result;
    }
    Json json() const {
        Json value={{"version",2},{"next_id",nextId},{"lamps",Json::array()}};
        for(const auto& item:items)value["lamps"].push_back({{"id",item.id},{"agent",item.agent},{"style",item.style}});
        return value;
    }
    bool add(const std::string& agent) {
        if(items.size()>=8 || (agent!="auto" && agentIndex(agent)<0))return false;
        if(nextId>60000){nextId=1;while(std::any_of(items.begin(),items.end(),[this](const Config& c){return c.id==nextId;}))++nextId;}
        items.push_back({nextId++,agent,"circle"});return true;
    }
    bool remove(unsigned id){if(items.size()<=1)return false;auto it=std::find_if(items.begin(),items.end(),[id](const Config& c){return c.id==id;});if(it==items.end())return false;items.erase(it);return true;}
};
struct Candidate {bool present=false;State state;};
struct Follow {
    int selected=-1;Time terminalSince=0;bool wasActive=false;
    int choose(const std::vector<Candidate>& candidates,int foreground,const std::set<int>& reserved,Time time) {
        auto available=[&](int i){return i>=0 && i<(int)candidates.size() && candidates[i].present && !reserved.count(i);};
        if(available(selected)) {
            if(candidates[selected].state.active()){wasActive=true;terminalSince=0;return selected;}
            if(wasActive && candidates[selected].state.kind=="unknown")return selected;
            if(wasActive){terminalSince=time;wasActive=false;}
            if(terminalSince && time-terminalSince<2000)return selected;
        }else {selected=-1;terminalSince=0;wasActive=false;}
        int best=-1;
        // A task already running keeps its lamp. Initial discovery prefers the
        // foreground task; otherwise waiting questions, then the newest task.
        if(available(foreground) && candidates[foreground].state.active())best=foreground;
        else for(int i=0;i<(int)candidates.size();++i)if(available(i) && candidates[i].state.active()) {
            if(best<0 || (candidates[i].state.kind=="waiting" && candidates[best].state.kind!="waiting") ||
                (candidates[i].state.kind==candidates[best].state.kind && candidates[i].state.started>candidates[best].state.started))best=i;
        }
        if(best<0 && available(foreground))best=foreground;
        if(best<0 && available(selected))best=selected;
        if(best<0)for(int i=0;i<(int)candidates.size();++i)if(available(i) && (best<0 || candidates[i].state.updated>candidates[best].state.updated))best=i;
        if(best!=selected)terminalSince=0;
        selected=best;wasActive=best>=0 && candidates[best].state.active();return selected;
    }
};
inline double clamp(double value,double low=0,double high=1){return std::max(low,std::min(high,value));}
inline std::vector<DWORD> renderIcon(const std::string& style,const std::string& state,int size=32) {
    COLORREF color=state=="running"?RGB(244,62,58):state=="waiting"?RGB(255,196,30):
        state=="completed" || state=="idle"?RGB(30,212,100):RGB(138,150,162);
    std::vector<DWORD> pixels(size*size);
    // Supersampling keeps the bezel and circular status surround readable at
    // native 16/20/24px tray sizes. Render once, never animate or poll graphics.
    for(int y=0;y<size;++y)for(int x=0;x<size;++x) {
        double alpha=0,red=0,green=0,blue=0;
        for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx) {
            double px=(x+(sx+.5)/4)*32/size,py=(y+(sy+.5)/4)*32/size;
            double dx=px-16,dy=py-16,r=std::sqrt(dx*dx+dy*dy);
            double a=0,rr=0,gg=0,bb=0;
            if(style!="glass") {
                bool inside=r<=12.75;
                if(style=="ring")inside=r<=13.25 && r>=9.2;
                if(style=="porous") {
                    // Seven round perforations remain distinct at 16px.
                    if(r<2.0)inside=false;
                    for(int hole=0;hole<6;++hole){double angle=hole*3.141592653589793/3;
                        double hx=dx-7*std::cos(angle),hy=dy-7*std::sin(angle);
                        if(hx*hx+hy*hy<4.4)inside=false;}
                }
                if(style=="arcane") {
                    // Two inscribed rings, six radial seals and a small center
                    // star. No rotation or transport-control silhouettes.
                    inside=(r>=12.1 && r<=13.5) || (r>=7.7 && r<=9.1) || r<1.6;
                    for(int ray=0;ray<6;++ray){double angle=ray*3.141592653589793/3;
                        double along=dx*std::cos(angle)+dy*std::sin(angle),across=-dx*std::sin(angle)+dy*std::cos(angle);
                        if(along>=3.1 && along<=12.7 && std::abs(across)<.65)inside=true;
                        double hx=dx-12.8*std::cos(angle),hy=dy-12.8*std::sin(angle);
                        if(hx*hx+hy*hy<2.6)inside=true;}
                }
                if(inside){a=1;rr=GetRValue(color);gg=GetGValue(color);bb=GetBValue(color);}
            }else if(r<=14) {
                a=1;
                if(r>12.35) {
                    double metal=clamp(.40-.30*dy/14+.10*dx/14);
                    rr=gg=bb=255*metal;
                    if(r>13.5)rr=gg=bb=55;
                }else if(r>11.7){rr=18;gg=22;bb=26;}
                else {
                    double nz=std::sqrt(clamp(1-(r*r)/(11.8*11.8)));
                    double light=clamp(-.25*dx/11.8-.38*dy/11.8+.62*nz);
                    double intensity=.55+.55*light;
                    rr=GetRValue(color)*intensity;gg=GetGValue(color)*intensity;bb=GetBValue(color)*intensity;
                    double gloss=.60*std::exp(-((dx+3)*(dx+3)/28+(dy+5)*(dy+5)/7));
                    rr=rr*(1-gloss)+255*gloss;gg=gg*(1-gloss)+255*gloss;bb=bb*(1-gloss)+255*gloss;
                }
            }else {
                double shadow=std::sqrt(dx*dx+(py-17)*(py-17));
                if(shadow<14.7){a=.20*clamp((14.7-shadow)/1.5);rr=gg=bb=12;}
            }
            alpha+=a;red+=clamp(rr,0,255)*a;green+=clamp(gg,0,255)*a;blue+=clamp(bb,0,255)*a;
        }
        pixels[y*size+x]=((DWORD)std::lround(alpha*255/16)<<24)|((DWORD)std::lround(red/16)<<16)|((DWORD)std::lround(green/16)<<8)|(DWORD)std::lround(blue/16);
    }
    return pixels;
}
inline HICON createIcon(const std::string& style,const std::string& state) {
    auto pixels=renderIcon(style,state);BYTE mask[128]{};
    for(int y=0;y<32;++y)for(int x=0;x<32;++x)if(!(pixels[y*32+x]>>24))mask[y*4+x/8]|=BYTE(1<<(7-x%8));
    return CreateIcon(GetModuleHandleW(nullptr),32,32,1,32,mask,reinterpret_cast<BYTE*>(pixels.data()));
}
}
