#pragma once
#include "client/engine.hpp"
#include "client/scene.hpp"
#include "client/scene/game.hpp"
#include <imgui.h>

// Pre-game lobby: shows the connected players and lets the host start. When the
// game starts it swaps itself for the GameScene.
class LobbyScene : public client::Scene
{
public:
    explicit LobbyScene(client::Engine* engine) : Scene(engine) {}

    auto handle_event(const SDL_Event& event) -> Propagation override
    {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_RETURN
            && engine_->session().is_host() && engine_->session().connected()) {
            engine_->audio().play("select");
            engine_->session().send_start();
        }
        return Continue;
    }

    auto update(float) -> Propagation override
    {
        engine_->audio().music("music_lobby"); // idempotent; fades in on entry
        if (engine_->session().game_state() == proto::GameState::Playing) {
            engine_->scenes().clear();
            engine_->scenes().push<GameScene>(engine_);
        }
        return Continue;
    }

    auto render(float) -> Propagation override
    {
        SDL_Renderer* r = engine_->renderer();
        SDL_SetRenderDrawColor(r, 20, 22, 28, 255);
        SDL_RenderClear(r);

        client::Session& session = engine_->session();
        ImGui::Begin("Lobby");
        if (session.join_denied()) {
            ImGui::TextColored(ImVec4(1, 0.35f, 0.35f, 1), "Mod set mismatch — join refused.");
            ImGui::Text("server mods: %016llx", static_cast<unsigned long long>(session.server_mods_hash()));
            ImGui::Text("your mods:   %016llx", static_cast<unsigned long long>(session.mods_hash()));
            ImGui::TextUnformatted("Your mods/ folder must match the server's.");
            if (ImGui::Button("Back")) { go_back(); }
            ImGui::End();
            return Continue;
        }
        if (!session.connected()) {
            ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "connecting...");
            if (ImGui::Button("Back")) { go_back(); }
        }
        ImGui::Text("Players (%zu):", session.roster().size());
        ImGui::Separator();
        for (const client::RosterRow& row : session.roster()) {
            ImGui::Text("%s%s%s", row.name.c_str(), row.is_host ? "  [host]" : "",
                        row.connected ? "" : "  (disconnected)");
        }
        ImGui::Separator();
        ImGui::TextUnformatted(session.is_host() ? "Press ENTER to start" : "Waiting for host to start...");
        ImGui::End();
        return Continue;
    }

private:
    // Return to the Connect menu: drop the connection and pop ourselves, which
    // reveals the ConnectScene that pushed us (its fields intact underneath).
    void go_back()
    {
        engine_->session().disconnect();
        engine_->scenes().pop();
    }

public:
    LobbyScene(const LobbyScene&) = delete;
    LobbyScene(LobbyScene&&) = delete;
    LobbyScene& operator=(const LobbyScene&) = delete;
    LobbyScene& operator=(LobbyScene&&) = delete;
    ~LobbyScene() override = default;
};
