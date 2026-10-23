#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace geode::prelude;

struct ClickEvent {
    double time;  // seconds since level start
    bool down;    // true = press, false = release
};

static std::vector<ClickEvent> g_events;
static size_t g_next = 0;
static double g_time = 0.0;

// Macro format: one event per line, "seconds,1" = press, "seconds,0" = release
static void loadMacro() {
    g_events.clear();
    g_next = 0;
    g_time = 0.0;

    auto mod = Mod::get();
    if (!mod->getSettingValue<bool>("play-macro")) return;

    auto path = mod->getSettingValue<std::filesystem::path>("macro-file");
    if (path.empty()) return;

    std::ifstream in(path);
    if (!in) {
        log::warn("Could not open macro file: {}", path.string());
        return;
    }

    double offset = mod->getSettingValue<int64_t>("offset-ms") / 1000.0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto comma = line.find(',');
        if (comma == std::string::npos) continue;
        try {
            double t = std::stod(line.substr(0, comma));
            int d = std::stoi(line.substr(comma + 1));
            g_events.push_back({ t + offset, d != 0 });
        } catch (...) {}
    }
    std::sort(g_events.begin(), g_events.end(),
        [](auto& a, auto& b) { return a.time < b.time; });

    log::info("Loaded {} click events", g_events.size());
}

// Reload and rewind the macro every time the level (re)starts
class $modify(ClickbotPlayLayer, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        loadMacro();
    }
};

class $modify(ClickbotBaseLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!g_events.empty() && static_cast<GJBaseGameLayer*>(PlayLayer::get()) == this) {
            while (g_next < g_events.size() && g_events[g_next].time <= g_time) {
                this->handleButton(g_events[g_next].down, 1, true);
                ++g_next;
            }
            g_time += dt;
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};
