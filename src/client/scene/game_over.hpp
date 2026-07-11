#pragma once
#include "client/engine.hpp"
#include "client/scene.hpp"
#include <SDL3/SDL.h>
#include <cstdio>
#include <fstream>
#include <string>

// Modal end-of-run screen: VICTORY or GAME OVER + the party SCOREBOARD
// (per-player kills/damage/revives from the server's RunStats block) over the
// frozen world (the server keeps streaming it, paused). Drawn with the widget
// kit. Also owns the LOCAL BESTS file (best wave / wins / runs, in SDL's
// pref-path) — updated once per run on construction, "NEW BEST!" flash when
// the wave record falls. Returns Stop everywhere so the GameScene below can't
// be controlled. The host returns everyone to the lobby (Command::BackToLobby
// -> server reset); when the Lobby state arrives, THIS scene rebuilds the
// pre-game stack (GameScene::update is blocked below us, so the transition has
// to live here).
class GameOverScene final : public client::Scene
{
public:
    explicit GameOverScene(client::Engine* engine) : Scene(engine)
    {
        load_records();
        const proto::GameOverMsg& stats = engine_->session().game_over_stats();
        new_best_ = stats.final_wave > best_wave_;
        best_wave_ = std::max(best_wave_, stats.final_wave);
        runs_ += 1;
        if (stats.won != 0) { wins_ += 1; }
        save_records();
    }

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
        const float row_h = ui.line_h() * 0.9f;

        const float pw = 250.0f * s;
        const float ph = (78.0f * s) + (row_h * (2.0f + static_cast<float>(session.roster().size())))
                       + ui.button_h();
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
        std::string headline = "WAVE " + std::to_string(stats.final_wave) + "  LEVEL "
                             + std::to_string(stats.final_level);
        if (new_best_) { headline += "  - NEW BEST!"; }
        ui.text_clipped(inner_x, y, headline, inner_w,
                        new_best_ ? client::colors::accent : client::colors::dim);
        y += ui.line_h() * 1.2f;

        // Scoreboard: name + the run's RunStats per player. Fixed right-aligned
        // numeric columns; the name column clips into whatever is left.
        const float col_w = 34.0f * s;
        const float col_kills = inner_x + inner_w - (3.0f * col_w);
        const float col_dmg = inner_x + inner_w - (2.0f * col_w);
        const float col_rev = inner_x + inner_w - col_w;
        ui.text(inner_x, y, "PARTY", client::colors::dim);
        ui.text(col_kills, y, "KILLS", client::colors::dim);
        ui.text(col_dmg, y, "DMG", client::colors::dim);
        ui.text(col_rev, y, "REV", client::colors::dim);
        y += row_h;
        for (const client::RosterRow& row : session.roster()) {
            std::string line = row.name;
            if (row.is_host) { line += " [HOST]"; }
            if (!row.connected) { line += " (LOST)"; }
            const client::GuiColor col = row.connected ? client::colors::text : client::colors::dim;
            ui.text_clipped(inner_x, y, line, col_kills - inner_x - (4.0f * s), col);
            for (const proto::GameOverEntry& entry : session.game_over_entries()) {
                if (entry.net_id != row.net_id) { continue; }
                ui.text(col_kills, y, std::to_string(entry.kills), col);
                ui.text(col_dmg, y, compact(entry.damage), col);
                ui.text(col_rev, y, std::to_string(entry.revives), col);
                break;
            }
            y += row_h;
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
        // Lifetime footer from the local records file.
        ui.text_centered(w * 0.5f, py + ph + (20.0f * s),
                         "BEST WAVE " + std::to_string(best_wave_) + "  -  WINS "
                           + std::to_string(wins_) + "  -  RUNS " + std::to_string(runs_),
                         client::colors::dim, 6.0f * s);
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

    // Thousands read badly in a pixel font: 12874 -> "12.8K".
    static std::string compact(std::uint32_t v)
    {
        if (v < 10000) { return std::to_string(v); }
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1fK", static_cast<double>(v) / 1000.0);
        return buf;
    }

    // Local records: a tiny "key value" per line file in SDL's per-user pref
    // dir (the client runs from the repo root, but SAVES must not live there).
    [[nodiscard]] static std::string records_path()
    {
        char* pref = SDL_GetPrefPath("soulkeeper", "soulkeeper");
        if (pref == nullptr) { return {}; }
        std::string path = std::string(pref) + "records.txt";
        SDL_free(pref);
        return path;
    }

    void load_records()
    {
        const std::string path = records_path();
        if (path.empty()) { return; }
        std::ifstream in(path);
        std::string key;
        long value = 0;
        while (in >> key >> value) {
            if (key == "best_wave") { best_wave_ = static_cast<std::uint16_t>(value); }
            if (key == "wins") { wins_ = static_cast<int>(value); }
            if (key == "runs") { runs_ = static_cast<int>(value); }
        }
    }

    void save_records() const
    {
        const std::string path = records_path();
        if (path.empty()) { return; }
        std::ofstream out(path, std::ios::trunc);
        out << "best_wave " << best_wave_ << "\nwins " << wins_ << "\nruns " << runs_ << "\n";
    }

    std::uint16_t best_wave_ = 0;
    int wins_ = 0;
    int runs_ = 0;
    bool new_best_ = false;
};
