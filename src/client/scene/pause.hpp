#pragma once
#include "client/engine.hpp"
#include "client/scene.hpp"
#include <string>

// ESC menu over the running game: volume sliders (local), a host-only run
// pause toggle, and disconnect. update() returns Continue — in co-op the
// world keeps simulating (and rendering) beneath; only INPUT is modal, so a
// player fiddling with sliders never freezes their teammates' game unless the
// host explicitly pauses the run.
class PauseScene final : public client::Scene
{
public:
    explicit PauseScene(client::Engine* engine) : Scene(engine) {}

    auto handle_event(const SDL_Event& event) -> Propagation override
    {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
            engine_->scenes().pop();
        }
        return Stop; // gameplay input stays blocked while the menu is up
    }

    auto update(float) -> Propagation override
    {
        // The run ended (or the host reset to lobby) while the menu was open:
        // get out of the way — GameScene below owns those transitions.
        if (engine_->session().game_over()
            || engine_->session().game_state() == proto::GameState::Lobby) {
            engine_->scenes().pop();
        }
        return Continue;
    }

    auto render(float) -> Propagation override
    {
        const float w = static_cast<float>(engine_->width());
        const float h = static_cast<float>(engine_->height());
        client::Gui& ui = engine_->gui();
        client::Audio& audio = engine_->audio();
        client::Session& session = engine_->session();
        const float s = ui.scale();

        ui.dim_overlay(0.55f);

        const bool host = session.is_host();
        const float pw = 190.0f * s;
        const float row = ui.line_h() * 1.55f;
        const float ph = (34.0f * s) + (3.0f * row) + ((host ? 3.0f : 2.0f) * (ui.button_h() + (6.0f * s)))
                       + (14.0f * s);
        const float px = (w - pw) * 0.5f;
        const float py = (h - ph) * 0.5f;

        ui.text_centered(w * 0.5f, py - (26.0f * s), "MENU", client::colors::accent, 16.0f * s);
        ui.panel(px, py, pw, ph);

        const float inner_x = px + (16.0f * s);
        const float inner_w = pw - (32.0f * s);
        float y = py + (18.0f * s);

        const auto volume_row = [&](std::string_view label, float value) {
            ui.text(inner_x, y, label, client::colors::dim);
            const float track_w = inner_w * 0.62f;
            const float v = ui.slider(inner_x + inner_w - track_w,
                                      y - ((ui.slider_h() - ui.body_px()) * 0.5f), track_w, value);
            y += row;
            return v;
        };
        audio.set_master(volume_row("VOLUME", audio.master()));
        audio.set_sfx(volume_row("SFX", audio.sfx()));
        audio.set_music_volume(volume_row("MUSIC", audio.music_volume()));

        y += 6.0f * s;
        const float bw = inner_w;
        if (ui.button("RESUME", inner_x, y, bw, ui.button_h())) { engine_->scenes().pop(); }
        y += ui.button_h() + (6.0f * s);
        if (host) {
            // Blind toggle, same contract as the console verbs: the server has
            // no paused flag on the wire, so track what WE last sent.
            if (ui.button(sent_pause_ ? "RESUME RUN" : "PAUSE RUN", inner_x, y, bw, ui.button_h())) {
                sent_pause_ = !sent_pause_;
                session.send_command(sent_pause_ ? proto::Command::Pause : proto::Command::Resume);
            }
            y += ui.button_h() + (6.0f * s);
        }
        if (ui.button("DISCONNECT", inner_x, y, bw, ui.button_h())) {
            session.disconnect();
            engine_->reset_to_connect();
        }
        return Stop;
    }

    PauseScene(const PauseScene&) = delete;
    PauseScene(PauseScene&&) = delete;
    PauseScene& operator=(const PauseScene&) = delete;
    PauseScene& operator=(PauseScene&&) = delete;
    ~PauseScene() override = default;

private:
    bool sent_pause_ = false;
};
