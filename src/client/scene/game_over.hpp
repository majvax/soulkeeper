#pragma once
#include "client/engine.hpp"
#include "client/scene.hpp"
#include <string>

// Modal end-of-run screen: VICTORY or GAME OVER + final stats over the frozen
// world (the server keeps streaming it, paused). Drawn with the widget kit.
// Returns Stop everywhere so the GameScene below can't be controlled. The host
// returns everyone to the lobby (Command::BackToLobby -> server reset); when
// the Lobby state arrives, THIS scene rebuilds the pre-game stack
// (GameScene::update is blocked below us, so the transition has to live here).
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
        const float w = static_cast<float>(engine_->width());
        const float h = static_cast<float>(engine_->height());

        client::Gui& ui = engine_->gui();
        ui.dim_overlay(); // scrim over the frozen world beneath us
        const client::Session& session = engine_->session();
        const proto::GameOverMsg& stats = session.game_over_stats();
        const bool won = stats.won != 0;
        const float s = ui.scale();

        const float pw = 180.0f * s;
        const float ph = 110.0f * s;
        const float px = (w - pw) * 0.5f;
        const float py = (h - ph) * 0.5f;

        ui.text_centered(w * 0.5f, py - (30.0f * s), won ? "VICTORY" : "GAME OVER",
                         won ? client::colors::accent : client::colors::danger, 20.0f * s);
        ui.panel(px, py, pw, ph);

        const float inner_x = px + (14.0f * s);
        const float inner_w = pw - (28.0f * s);
        float y = py + (14.0f * s);
        ui.text_clipped(inner_x, y, won ? "THE SOULKEEPER PREVAILS." : "THE HORDE HAS WON.",
                        inner_w, client::colors::text);
        y += ui.line_h();
        ui.text_clipped(inner_x, y,
                        "WAVE " + std::to_string(stats.final_wave) + "  LEVEL "
                          + std::to_string(stats.final_level),
                        inner_w, client::colors::dim);
        y += ui.line_h() * 1.2f;

        ui.text(inner_x, y, "PARTY", client::colors::dim);
        y += ui.line_h() * 0.9f;
        for (const client::RosterRow& row : session.roster()) {
            std::string line = row.name;
            if (row.is_host) { line += " [HOST]"; }
            if (!row.connected) { line += " (LOST)"; }
            ui.text_clipped(inner_x + (6.0f * s), y, line, inner_w - (6.0f * s),
                            row.connected ? client::colors::text : client::colors::dim);
            y += ui.line_h() * 0.9f;
        }

        const float by = py + ph - ui.button_h() - (10.0f * s);
        if (session.is_host()) {
            const float bw = 130.0f * s;
            if (ui.button("RETURN TO LOBBY", px + ((pw - bw) * 0.5f), by, bw, ui.button_h())) {
                back_to_lobby();
            }
            ui.text_centered(w * 0.5f, py + ph + (8.0f * s), "OR PRESS ENTER",
                             client::colors::dim, 6.0f * s);
        } else {
            ui.text_centered(w * 0.5f, py + ph + (8.0f * s), "WAITING FOR THE HOST...",
                             client::colors::dim, 6.0f * s);
        }
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
