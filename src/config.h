#pragma once

#include <cstdint>
#include <string>

#include "engine.h"
#include "json.hpp"

namespace tuki {

constexpr char kVersion[] = "0.1.0";

struct Stats {
    uint64_t clicks = 0;          // button presses seen
    uint64_t notches = 0;         // wheel events seen
    uint64_t corrected = 0;       // wheel events whose direction Tuki changed
    uint64_t ghostClicks = 0;     // presses removed by the debounce filter
    uint64_t jitterDropped = 0;   // stray wheel notches removed
};

std::wstring appDataDir();        // %APPDATA%\Tuki (created on demand)
std::wstring localDataDir();      // %LOCALAPPDATA%\Tuki (created on demand)
std::wstring configPath();

Config loadConfig();
bool saveConfig(const Config& cfg);
Stats loadStats();
bool saveStats(const Stats& s);

nlohmann::json configToJson(const Config& cfg);
// Reads fields present in `j`, keeping `base` for anything missing or invalid.
Config configFromJson(const nlohmann::json& j, const Config& base);
nlohmann::json statsToJson(const Stats& s);

std::string toUtf8(const std::wstring& w);
std::wstring fromUtf8(const std::string& s);
std::wstring toLower(std::wstring s);

}  // namespace tuki
