#pragma once
#include "client/engine.hpp"
#include "client/scene.hpp"
#include <imgui.h>

// Modal end-of-run screen: VICTORY or GAME OVER + final stats over the frozen
// world (the server keeps streaming it, paused). Returns Stop everywhere so the
// GameScene below can't be controlled. The host returns everyone to the lobby
// (Command::BackToLobby -> server reset); when the Lobby state arrives, THIS
// scene rebuilds the pre-game stack (GameScene::update is blocked below us, so
// the transition has to live here) — the next run then flows through the normal
// Lobby -> Playing path with a fresh GameScene.
class GameOverScene final : public client::Scene
{
public:
    explicit GameOverScene(client::Engine* engine) : Scene(engine) {}

    auto handle_event(const SDL_Event& event) -> Propagation override
    {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_RETURN) { back_to_lobby(); }
        return Stop;
    }

    auto update(float) -> Propagation override
    {
        // The server reset us to the lobby: swap the whole stack back to the
        // pre-game shape [Lobby over Connect] (deferred, applied after update).
        if (engine_->session().game_state() == proto::GameState::Lobby) {
            engine_->reset_to_lobby();
        }
        return Stop;
    }

    auto render(float) -> Propagation override
    {
        SDL_Renderer* r = engine_->renderer();
        const float w = static_cast<float>(engine_->width());
        const float h = static_cast<float>(engine_->height());

        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, 0, 0, 0, 200);
        const SDL_FRect full{ .x = 0, .y = 0, .w = w, .h = h };
        SDL_RenderFillRect(r, &full);

        const client::Session& session = engine_->session();
        const proto::GameOverMsg& stats = session.game_over_stats();
        const bool won = stats.won != 0;

        ImDrawList* draw = ImGui::GetForegroundDrawList();
        const char* title = won ? "VICTORY" : "GAME OVER";
        const ImU32 title_col = won ? IM_COL32(255, 215, 80, 255) : IM_COL32(230, 70, 70, 255);
        constexpr float title_px = 48.0f;
        ImFont* font = ImGui::GetFont();
        const ImVec2 title_size = font->CalcTextSizeA(title_px, FLT_MAX, 0.0f, title);
        draw->AddText(font, title_px, ImVec2((w - title_size.x) * 0.5f, (h * 0.5f) - 160.0f),
                      title_col, title);

        ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::Begin("##game_over", nullptr,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
                       | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
        ImGui::Text("%s", won ? "The Soulkeeper prevails." : "The horde has won.");
        ImGui::Separator();
        ImGui::Text("Reached wave %u at team level %u", static_cast<unsigned>(stats.final_wave),
                    static_cast<unsigned>(stats.final_level));
        ImGui::Spacing();
        ImGui::TextUnformatted("Party:");
        for (const client::RosterRow& row : session.roster()) {
            ImGui::BulletText("%s%s%s", row.name.c_str(), row.is_host ? "  [host]" : "",
                              row.connected ? "" : "  (disconnected)");
        }
        ImGui::Separator();
        if (session.is_host()) {
            if (ImGui::Button("Return to lobby (ENTER)", ImVec2(220, 0))) { back_to_lobby(); }
        } else {
            ImGui::TextDisabled("waiting for the host to return to the lobby...");
        }
        ImGui::End();
        return Stop;
    }

    GameOverScene(const GameOverScene&) = delete;
    GameOverScene(GameOverScene&&) = delete;
    GameOverScene& operator=(const GameOverScene&) = delete;
    GameOverScene& operator=(GameOverScene&&) = delete;
    ~GameOverScene() override = default;

private:
    void back_to_lobby()
    {
        if (!engine_->session().is_host()) { return; }
        engine_->session().send_command(proto::Command::BackToLobby);
        // Don't pop here — update() swaps the stack once the Lobby state lands,
        // for the host and everyone else alike.
    }
};
