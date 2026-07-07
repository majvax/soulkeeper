#pragma once
#include "client/engine.hpp"
#include "client/scene.hpp"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <deque>
#include <imgui.h>
#include <string>
#include <vector>

// Developer console. It only exists on the stack while open: GameScene pushes it
// on TAB; it pops itself on ESC (always) or TAB (only while the input line is
// empty — with text typed, TAB autocompletes). Local commands (help/clear/quit) run
// on the client; /pause and /resume go to the server as engine commands; every
// other /name is a MOD command (mod:command) sent as a LuaCommand line and run
// server-side, host-only. The render VM registered the same commands from the
// same mod.lua, so their names + usage feed completion and the live suggestion
// list without any networking.
class ConsoleScene final : public client::Scene
{
public:
    explicit ConsoleScene(client::Engine* engine) : Scene(engine)
    {
        input_buffer_[0] = '\0';
        engine_->audio().play("click");
    }

    auto handle_event(const SDL_Event& event) -> Propagation override
    {
        if (event.type == SDL_EVENT_KEY_DOWN) {
            // ESC always closes; TAB only while the input is empty (with text
            // typed it autocompletes instead).
            if (event.key.key == SDLK_ESCAPE
                || (event.key.key == SDLK_TAB && input_buffer_[0] == '\0')) {
                engine_->scenes().pop(); // close: pop ourselves
            }
        }
        return Stop;
    }

    auto update(float) -> Propagation override { return Continue; }

    auto render(float) -> Propagation override
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y * 0.4f));

        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                                     | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
        ImGui::SetNextWindowBgAlpha(0.85f);

        if (ImGui::Begin("Console", nullptr, flags)) {
            ImGui::TextUnformatted("Developer Console");
            ImGui::Separator();

            // Leave room for the suggestion rows + the input line.
            const float footer = ImGui::GetFrameHeightWithSpacing()
                               + (static_cast<float>(suggestions().size()) * ImGui::GetTextLineHeightWithSpacing());
            ImGui::BeginChild("ConsoleScrollRegion", ImVec2(0.0f, -footer), false);
            for (const std::string& line : log_) { ImGui::TextUnformatted(line.c_str()); }
            if (scroll_to_bottom_) {
                ImGui::SetScrollHereY(1.0f);
                scroll_to_bottom_ = false;
            }
            ImGui::EndChild();

            // Live suggestions while a /command is being typed (usage included).
            for (const std::string& hint : suggestions()) {
                ImGui::TextDisabled("%s", hint.c_str());
            }

            ImGui::Separator();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::SetKeyboardFocusHere();
            if (ImGui::InputText("##console_input", input_buffer_, sizeof(input_buffer_),
                                 ImGuiInputTextFlags_EnterReturnsTrue
                                   | ImGuiInputTextFlags_CallbackCompletion
                                   | ImGuiInputTextFlags_CallbackHistory,
                                 &ConsoleScene::text_callback, this)) {
                submit_command(input_buffer_);
                input_buffer_[0] = '\0';
                ImGui::SetKeyboardFocusHere(-1);
            }
        }
        ImGui::End();
        return Stop;
    }

    ConsoleScene(const ConsoleScene&) = delete;
    ConsoleScene(ConsoleScene&&) = delete;
    ConsoleScene& operator=(const ConsoleScene&) = delete;
    ConsoleScene& operator=(ConsoleScene&&) = delete;
    ~ConsoleScene() override = default;

private:
    // Everything completable: local verbs, engine /commands, mod /commands.
    [[nodiscard]] std::vector<std::string> command_names() const
    {
        std::vector<std::string> names{ "help",    "clear",   "quit", "/pause",
                                        "/resume", "/volume", "/sfx", "/music" };
        for (const mod::ModState::ConsoleCommand& cmd : engine_->mods().state().commands) {
            names.push_back("/" + cmd.name);
        }
        return names;
    }

    // Usage lines for commands matching the current input prefix (≤ 6 shown).
    [[nodiscard]] std::vector<std::string> suggestions() const
    {
        std::vector<std::string> out;
        const std::string_view typed{ input_buffer_ };
        if (typed.empty() || typed.front() != '/' || typed.find(' ') != std::string_view::npos) {
            return out;
        }
        const std::string_view prefix = typed.substr(1);
        if (std::string_view("pause").starts_with(prefix)) { out.emplace_back("/pause  -- freeze the sim (host)"); }
        if (std::string_view("resume").starts_with(prefix)) { out.emplace_back("/resume  -- unfreeze (host)"); }
        if (std::string_view("volume").starts_with(prefix)) { out.emplace_back("/volume <0..1>  -- master volume (local)"); }
        if (std::string_view("sfx").starts_with(prefix)) { out.emplace_back("/sfx <0..1>  -- effects volume (local)"); }
        if (std::string_view("music").starts_with(prefix)) { out.emplace_back("/music <0..1>  -- music volume (local)"); }
        for (const mod::ModState::ConsoleCommand& cmd : engine_->mods().state().commands) {
            if (out.size() >= 6) { break; }
            if (std::string_view(cmd.name).starts_with(prefix)) { out.push_back(cmd.usage); }
        }
        return out;
    }

    // TAB: complete to the longest common prefix of the matching commands
    // (single match also appends a space). Up/Down: walk the input history.
    static int text_callback(ImGuiInputTextCallbackData* data)
    {
        auto* self = static_cast<ConsoleScene*>(data->UserData);
        if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
            const std::string_view typed{ data->Buf, static_cast<std::size_t>(data->BufTextLen) };
            std::vector<std::string> matches;
            for (const std::string& name : self->command_names()) {
                if (std::string_view(name).starts_with(typed)) { matches.push_back(name); }
            }
            if (matches.empty()) { return 0; }
            std::string common = matches.front();
            for (const std::string& m : matches) {
                const auto mismatch = std::mismatch(common.begin(), common.end(), m.begin(), m.end());
                common.erase(mismatch.first, common.end());
            }
            if (matches.size() == 1) { common += ' '; }
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, common.c_str());
            if (matches.size() > 1) { // ambiguous: list the candidates
                std::string row;
                for (const std::string& m : matches) { row += m + "  "; }
                self->log_.push_back(row);
                self->scroll_to_bottom_ = true;
            }
        } else if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
            if (self->history_.empty()) { return 0; }
            int& pos = self->history_pos_;
            const int last = static_cast<int>(self->history_.size()) - 1;
            if (data->EventKey == ImGuiKey_UpArrow) {
                pos = pos < 0 ? last : std::max(0, pos - 1);
            } else if (data->EventKey == ImGuiKey_DownArrow && pos >= 0) {
                ++pos;
                if (pos > last) { pos = -1; }
            }
            const std::string line = pos >= 0 ? self->history_[static_cast<std::size_t>(pos)] : "";
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, line.c_str());
        }
        return 0;
    }

    void submit_command(std::string command)
    {
        // Trim: TAB completion leaves a trailing space after a full match.
        const std::size_t first = command.find_first_not_of(' ');
        if (first == std::string::npos) { return; }
        command = command.substr(first, command.find_last_not_of(' ') - first + 1);
        log_.push_back("> " + command);
        history_.push_back(command);
        history_pos_ = -1;
        engine_->audio().play("click");

        // Match builtins on the first token so stray arguments don't misroute.
        const std::string name = command.substr(0, command.find(' '));
        if (name == "help") {
            log_.emplace_back("commands: help, clear, quit, /pause, /resume");
            for (const mod::ModState::ConsoleCommand& cmd : engine_->mods().state().commands) {
                log_.push_back("  " + cmd.usage);
            }
        } else if (name == "clear") {
            log_.clear();
        } else if (name == "quit") {
            engine_->quit();
        } else if (name == "/pause") {
            engine_->session().send_command(proto::Command::Pause);
        } else if (name == "/resume") {
            engine_->session().send_command(proto::Command::Resume);
        } else if (name == "/volume" || name == "/sfx" || name == "/music") {
            // Local audio verbs — never leave the client.
            const std::size_t sp = command.find(' ');
            float v = -1.0f;
            if (sp != std::string::npos) {
                const std::string arg = command.substr(sp + 1);
                std::from_chars(arg.data(), arg.data() + arg.size(), v);
            }
            client::Audio& audio = engine_->audio();
            if (v < 0.0f || v > 1.0f) {
                log_.push_back("usage: " + name + " <0..1>");
            } else if (name == "/volume") {
                audio.set_master(v);
            } else if (name == "/sfx") {
                audio.set_sfx(v);
            } else {
                audio.set_music_volume(v);
            }
        } else if (command.front() == '/') {
            // A mod command: "/name args..." -> LuaCommand "name args...".
            const std::string line = command.substr(1);
            const std::string name = line.substr(0, line.find(' '));
            const auto& commands = engine_->mods().state().commands;
            const bool known = std::any_of(commands.begin(), commands.end(),
                                           [&](const auto& cmd) { return cmd.name == name; });
            if (known) {
                engine_->session().send_lua_command(line);
                if (!engine_->session().is_host()) {
                    log_.emplace_back("(sent - note: mod commands are host-only)");
                }
            } else {
                log_.push_back("unknown command: " + command);
            }
        } else {
            log_.push_back("unknown command: " + command);
        }
        scroll_to_bottom_ = true;
    }

    bool scroll_to_bottom_ = false;
    char input_buffer_[256]{};
    std::deque<std::string> log_;
    std::vector<std::string> history_;
    int history_pos_ = -1; // -1 = not browsing; reset on submit
};
