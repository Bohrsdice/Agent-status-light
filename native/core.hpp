#pragma once
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "vendor/json.hpp"

namespace light {
using Json = nlohmann::json;
namespace fs = std::filesystem;
using Time = long long;
Time now();
Time parseTime(const std::string& text);
std::string utf8(const std::wstring& text);
std::wstring wide(const std::string& text);
std::string field(const Json& value, const char* key);
Json parse(const std::string& text);
void atomicWrite(const fs::path& path, const Json& data);

struct State {
    std::string kind = "unknown", turn, event, reason = "等待发现任务";
    Time started = 0, updated = 0;
    size_t pending = 0;
    bool active() const { return kind == "running" || kind == "waiting"; }
};

class Lifecycle {
    std::set<std::pair<std::string, size_t>> questions_, answered_;
    bool finished_ = false;
    void item(const Json&, Time);
public:
    std::string id, project;
    bool identity = false, root = true;
    State state;
    void consume(const Json& record);
};

class Reader {
    uint64_t offset_ = 0, fileId_ = 0;
    DWORD volume_ = 0;
    std::string pending_;
    bool initialized_ = false, damaged_ = false;
    void initialize(HANDLE file, uint64_t size);
public:
    fs::path path;
    Lifecycle lifecycle;
    std::string error;
    bool catchingUp = false;
    explicit Reader(fs::path p) : path(std::move(p)) {}
    void poll();
};

struct Runtime {
    bool online = false;
    Time started = 0;
    std::string reason = "正在查找 Codex 后台服务";
};

struct Candidate {
    std::string id, project, source;
    State state;
};

struct View {
    State state;
    std::string id, project, source;
    std::vector<Candidate> choices;
    bool online = false;
    Json json() const;
};

class Selector {
    std::string selected_;
    Time boot_, terminalSince_ = 0;
public:
    explicit Selector(Time boot) : boot_(boot) {}
    View choose(const std::vector<Candidate>& candidates, Time time,
                const std::string& manual = "");
    const std::string& selected() const { return selected_; }
};

class Engine {
    fs::path home_;
    std::map<fs::path, Reader> readers_;
    Selector selector_;
    Time lastScan_ = 0;
    bool scanFailed_ = false;
public:
    explicit Engine(fs::path home, Time boot = now()) : home_(std::move(home)), selector_(boot) {}
    View tick(const Runtime& runtime, const std::string& manual = "", Time time = now());
};
}
