#include "config.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>

using nlohmann::json;

namespace tuki {

namespace {

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

std::wstring ensureDir(std::wstring dir) {
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool readFile(const std::wstring& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool writeFileAtomic(const std::wstring& path, const std::string& data) {
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

template <typename T>
void readField(const json& obj, const char* key, T& target) {
    if (!obj.is_object()) return;
    auto it = obj.find(key);
    if (it == obj.end()) return;
    try {
        target = it->get<T>();
    } catch (...) {
        // keep previous value on type mismatch
    }
}

const json& child(const json& j, const char* key) {
    static const json empty = json::object();
    if (!j.is_object()) return empty;
    auto it = j.find(key);
    return it != j.end() && it->is_object() ? *it : empty;
}

}  // namespace

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring toLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

std::wstring appDataDir() { return ensureDir(knownFolder(FOLDERID_RoamingAppData) + L"\\Tuki"); }
std::wstring localDataDir() { return ensureDir(knownFolder(FOLDERID_LocalAppData) + L"\\Tuki"); }
std::wstring configPath() { return appDataDir() + L"\\config.json"; }

json configToJson(const Config& c) {
    json apps = json::array();
    for (const auto& a : c.excludedApps) apps.push_back(toUtf8(a));
    return {
        {"enabled", c.enabled},
        {"scroll",
         {{"mode", scrollModeName(c.scrollMode)},
          {"speed", c.scrollSpeed},
          {"accel", c.scrollAccel},
          {"accelStrength", c.scrollAccelStrength},
          {"shiftHorizontal", c.shiftHorizontal},
          {"invertHorizontal", c.invertHorizontal},
          {"jitterFilter", c.wheelJitterFilter},
          {"jitterMs", c.wheelJitterMs}}},
        {"buttons",
         {{"swapLeftRight", c.swapLeftRight},
          {"middle", actionName(c.buttonActions[static_cast<size_t>(Button::Middle)])},
          {"x1", actionName(c.buttonActions[static_cast<size_t>(Button::X1)])},
          {"x2", actionName(c.buttonActions[static_cast<size_t>(Button::X2)])}}},
        {"debounce",
         {{"left", c.debounce[static_cast<size_t>(Button::Left)]},
          {"right", c.debounce[static_cast<size_t>(Button::Right)]},
          {"middle", c.debounce[static_cast<size_t>(Button::Middle)]},
          {"ms", c.debounceMs}}},
        {"apps", {{"excluded", apps}}},
        {"general", {{"startWithWindows", c.startWithWindows}, {"hotkey", c.hotkeyEnabled}}},
    };
}

Config configFromJson(const json& j, const Config& base) {
    Config c = base;
    readField(j, "enabled", c.enabled);

    const json& s = child(j, "scroll");
    std::string mode = scrollModeName(c.scrollMode);
    readField(s, "mode", mode);
    c.scrollMode = scrollModeFromName(mode);
    readField(s, "speed", c.scrollSpeed);
    readField(s, "accel", c.scrollAccel);
    readField(s, "accelStrength", c.scrollAccelStrength);
    readField(s, "shiftHorizontal", c.shiftHorizontal);
    readField(s, "invertHorizontal", c.invertHorizontal);
    readField(s, "jitterFilter", c.wheelJitterFilter);
    readField(s, "jitterMs", c.wheelJitterMs);

    const json& b = child(j, "buttons");
    readField(b, "swapLeftRight", c.swapLeftRight);
    const std::pair<const char*, Button> remappable[] = {
        {"middle", Button::Middle}, {"x1", Button::X1}, {"x2", Button::X2}};
    for (const auto& [key, btn] : remappable) {
        std::string name = actionName(c.buttonActions[static_cast<size_t>(btn)]);
        readField(b, key, name);
        c.buttonActions[static_cast<size_t>(btn)] = actionFromName(name);
    }

    const json& d = child(j, "debounce");
    readField(d, "left", c.debounce[static_cast<size_t>(Button::Left)]);
    readField(d, "right", c.debounce[static_cast<size_t>(Button::Right)]);
    readField(d, "middle", c.debounce[static_cast<size_t>(Button::Middle)]);
    readField(d, "ms", c.debounceMs);

    const json& a = child(j, "apps");
    auto ex = a.find("excluded");
    if (ex != a.end() && ex->is_array()) {
        c.excludedApps.clear();
        for (const auto& item : *ex) {
            if (!item.is_string()) continue;
            std::wstring name = toLower(fromUtf8(item.get<std::string>()));
            if (name.empty() || name.size() > 260) continue;
            if (std::find(c.excludedApps.begin(), c.excludedApps.end(), name) == c.excludedApps.end())
                c.excludedApps.push_back(name);
        }
    }

    const json& g = child(j, "general");
    readField(g, "startWithWindows", c.startWithWindows);
    readField(g, "hotkey", c.hotkeyEnabled);

    sanitize(c);
    return c;
}

json statsToJson(const Stats& s) {
    return {{"clicks", s.clicks},
            {"notches", s.notches},
            {"corrected", s.corrected},
            {"ghostClicks", s.ghostClicks},
            {"jitterDropped", s.jitterDropped}};
}

Config loadConfig() {
    Config c;
    std::string text;
    if (readFile(configPath(), text)) {
        json j = json::parse(text, nullptr, false);
        if (!j.is_discarded()) c = configFromJson(j, c);
    }
    sanitize(c);
    return c;
}

bool saveConfig(const Config& cfg) { return writeFileAtomic(configPath(), configToJson(cfg).dump(2)); }

Stats loadStats() {
    Stats s;
    std::string text;
    if (!readFile(appDataDir() + L"\\stats.json", text)) return s;
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) return s;
    readField(j, "clicks", s.clicks);
    readField(j, "notches", s.notches);
    readField(j, "corrected", s.corrected);
    readField(j, "ghostClicks", s.ghostClicks);
    readField(j, "jitterDropped", s.jitterDropped);
    return s;
}

bool saveStats(const Stats& s) { return writeFileAtomic(appDataDir() + L"\\stats.json", statsToJson(s).dump(2)); }

}  // namespace tuki
