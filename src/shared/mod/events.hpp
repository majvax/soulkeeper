// src/shared/mod/events.hpp
//
// A tiny event bus for the modding layer: plugins `subscribe(name, fn)` and the
// engine `emit(name, args...)`. Handlers run as protected calls so a broken mod
// callback is logged, never fatal. SDL-free (lives in shared/); each VM (sim on
// the server, render on the client) owns its own bus.
#pragma once

#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sol/sol.hpp>

namespace mod {

class EventBus
{
public:
    void subscribe(std::string name, sol::protected_function handler)
    {
        if (handler.valid()) { handlers_[std::move(name)].push_back(std::move(handler)); }
    }

    // Invoke every handler registered for `name` with the given args. Errors are
    // logged and swallowed so one bad mod can't break the loop.
    template <typename... Args>
    void emit(const std::string& name, Args&&... args) const
    {
        const auto it = handlers_.find(name);
        if (it == handlers_.end()) { return; }
        for (const sol::protected_function& fn : it->second) {
            sol::protected_function_result res = fn(args...);
            if (!res.valid()) {
                const sol::error err = res;
                std::fprintf(stderr, "[mod] event '%s' handler error: %s\n", name.c_str(), err.what());
            }
        }
    }

    // Variadic form for Lua-originated events (mod:emit) — forwards the caller's
    // arguments to every handler unchanged.
    void emit_variadic(const std::string& name, sol::variadic_args args) const
    {
        const auto it = handlers_.find(name);
        if (it == handlers_.end()) { return; }
        for (const sol::protected_function& fn : it->second) {
            sol::protected_function_result res = fn(args);
            if (!res.valid()) {
                const sol::error err = res;
                std::fprintf(stderr, "[mod] event '%s' handler error: %s\n", name.c_str(), err.what());
            }
        }
    }

    [[nodiscard]] bool has(const std::string& name) const { return handlers_.contains(name); }

private:
    std::unordered_map<std::string, std::vector<sol::protected_function>> handlers_;
};

} // namespace mod
