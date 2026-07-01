#pragma once
#include "client/engine.hpp"
#include "client/scene.hpp"
#include <deque>
#include <imgui.h>
#include <string>

// Developer console. It only exists on the stack while open: GameScene pushes it
// on TAB, and it pops itself on TAB. Local commands (help/clear/quit) run on the
// client; server commands (/pause, /resume) go to the server.
//
// handle_event/render return Stop (it eats input and draws on top); update
// returns Continue so the game keeps ticking behind it.
class ConsoleScene final : public client::Scene
{
public:
    explicit ConsoleScene(client::Engine* engine) : Scene(engine) { input_buffer_[0] = '\0'; }

    auto handle_event(const SDL_Event& event) -> Propagation override
    {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_TAB) {
            engine_->scenes().pop(); // close: pop ourselves
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

            ImGui::BeginChild("ConsoleScrollRegion", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()), false);
            for (const std::string& line : log_) { ImGui::TextUnformatted(line.c_str()); }
            if (scroll_to_bottom_) {
                ImGui::SetScrollHereY(1.0f);
                scroll_to_bottom_ = false;
            }
            ImGui::EndChild();

            ImGui::Separator();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::SetKeyboardFocusHere();
            if (ImGui::InputText("##console_input", input_buffer_, sizeof(input_buffer_),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
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
    void submit_command(const std::string& command)
    {
        if (command.empty()) { return; }
        log_.push_back("> " + command);

        if (command == "help") {
            log_.emplace_back("commands: help, clear, quit, /pause, /resume");
        } else if (command == "clear") {
            log_.clear();
        } else if (command == "quit") {
            engine_->quit();
        } else if (command == "/pause") {
            engine_->session().send_command(proto::Command::Pause);
        } else if (command == "/resume") {
            engine_->session().send_command(proto::Command::Resume);
        } else {
            log_.push_back("unknown command: " + command);
        }
        scroll_to_bottom_ = true;
    }

    bool scroll_to_bottom_ = false;
    char input_buffer_[256]{};
    std::deque<std::string> log_;
};
