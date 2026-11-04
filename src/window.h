// Settings window: a native frame hosting the WebView2 UI from the ui/ folder.
#pragma once

#include <windows.h>

#include <functional>

#include "json.hpp"

namespace tuki::ui {

using MessageHandler = std::function<void(const nlohmann::json&)>;

void init(HINSTANCE inst, MessageHandler onMessage);
void show();                            // create the window, or bring it to front
void close();
bool isOpen();
void send(const nlohmann::json& msg);  // dropped when the page is not loaded

}  // namespace tuki::ui
