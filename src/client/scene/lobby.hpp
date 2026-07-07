#pragma once
#include "client/engine.hpp"
#include "client/scene.hpp"
#include "client/scene/game.hpp"
#include <string>

// Pre-game lobby: the connected players and the host's start control, drawn
// with the widget kit (no ImGui). When the game starts it swaps itself for the
// GameScene.
class LobbyScene : public client::Scene
{
public:
    explicit LobbyScene(client::Engine* engine) : Scene(engine) {}

    auto handle_event(const SDL_Event& event) -> Propagation override
    {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_RETURN
            && engine_->session().is_host() && engine_->session().connected()) {
            start();
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

        client::Gui& ui = engine_->gui();
        client::Session& session = engine_->session();
        const float s = ui.scale();
        const float w = static_cast<float>(engine_->width());
        const float h = static_cast<float>(engine_->height());

        const float pw = 170.0f * s;
        const float ph = 140.0f * s;
        const float px = (w - pw) * 0.5f;
        const float py = (h - ph) * 0.5f;

        ui.text_centered(w * 0.5f, py - (22.0f * s), "LOBBY", client::colors::accent, 12.0f * s);
        ui.panel(px, py, pw, ph);

        const float inner_x = px + (14.0f * s);
        float y = py + (12.0f * s);

        if (session.join_denied()) {
            ui.text(inner_x, y, "MOD SET MISMATCH", client::colors::danger);
            y += ui.line_h();
            ui.text(inner_x, y, "JOIN REFUSED", client::colors::danger);
            y += ui.line_h();
            ui.text(inner_x, y, "YOUR MODS/ MUST MATCH", client::colors::dim);
            y += ui.line_h();
            ui.text(inner_x, y, "THE SERVER'S", client::colors::dim);
        } else if (!session.connected()) {
            // Retro "connecting" pulse: trailing dots cycle with the caret clock.
            ui.text(inner_x, y, "CONNECTING...", client::colors::accent);
        } else {
            ui.text(inner_x, y, "PLAYERS", client::colors::dim);
            y += ui.line_h();
            for (const client::RosterRow& row : session.roster()) {
                std::string line = row.name;
                if (row.is_host) { line += " [HOST]"; }
                if (!row.connected) { line += " (LOST)"; }
                ui.text(inner_x + (6.0f * s), y, line,
                        row.connected ? client::colors::text : client::colors::dim);
                y += ui.line_h() * 0.9f;
            }
        }

        // Footer: back on the left, start (host) on the right.
        const float by = py + ph - ui.button_h() - (10.0f * s);
        if (ui.button("BACK", inner_x, by, 46.0f * s, ui.button_h())) { go_back(); }
        if (!session.join_denied() && session.connected()) {
            if (session.is_host()) {
                if (ui.button("START", px + pw - (60.0f * s) - (14.0f * s), by, 60.0f * s,
                              ui.button_h())) {
                    start();
                }
                ui.text_centered(w * 0.5f, py + ph + (8.0f * s), "OR PRESS ENTER",
                                 client::colors::dim, 6.0f * s);
            } else {
                ui.text_centered(w * 0.5f, py + ph + (8.0f * s), "WAITING FOR HOST...",
                                 client::colors::dim, 6.0f * s);
            }
        }
        return Continue;
    }

private:
    void start()
    {
        engine_->audio().play("select");
        engine_->session().send_start();
    }

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
