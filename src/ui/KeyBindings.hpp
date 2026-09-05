#pragma once

#include "../core/Config.hpp"

#include <algorithm>
#include <initializer_list>
#include <string>

#include <ftxui/component/event.hpp>

namespace ui {

inline std::string normalizedKeyName(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (value.size() > 1) {
        std::replace(value.begin(), value.end(), '-', '_');
    }
    return value;
}

inline bool eventMatchesKey(const ftxui::Event& event, std::string key)
{
    key = normalizedKeyName(std::move(key));
    if (key == "enter" || key == "return") return event == ftxui::Event::Return;
    if (key == "esc" || key == "escape") return event == ftxui::Event::Escape;
    if (key == "space") return event == ftxui::Event::Character(' ');
    if (key == "tab") return event == ftxui::Event::Tab;
    if (key == "shift_tab" || key == "tab_reverse") return event == ftxui::Event::TabReverse;
    if (key == "up" || key == "arrow_up") return event == ftxui::Event::ArrowUp;
    if (key == "down" || key == "arrow_down") return event == ftxui::Event::ArrowDown;
    if (key == "left" || key == "arrow_left") return event == ftxui::Event::ArrowLeft;
    if (key == "right" || key == "arrow_right") return event == ftxui::Event::ArrowRight;
    if (key == "delete" || key == "del") return event == ftxui::Event::Delete;
    if (key == "backspace") return event == ftxui::Event::Backspace;
    if (key == "page_up" || key == "pageup") return event == ftxui::Event::PageUp;
    if (key == "page_down" || key == "pagedown") return event == ftxui::Event::PageDown;
    return event == ftxui::Event::Character(key);
}

inline bool keyMatches(const ftxui::Event& event,
                       std::initializer_list<std::string> keys)
{
    for (const auto& key : keys) {
        if (eventMatchesKey(event, key)) {
            return true;
        }
    }
    return false;
}

inline bool bindingMatches(const ftxui::Event& event,
                           const KeyBindingMap& bindings,
                           const std::string& action,
                           std::initializer_list<std::string> defaults)
{
    auto found = bindings.find(action);
    if (found != bindings.end()) {
        for (const auto& key : found->second) {
            if (eventMatchesKey(event, key)) {
                return true;
            }
        }
        return false;
    }
    return keyMatches(event, defaults);
}

}  // namespace ui
