#pragma once
#include "client/engine.hpp"
#include "client/scene.hpp"
#include "client/scene/lobby.hpp" // Join transitions to the lobby
#include <algorithm>
#include <charconv>
#include <string>

// The entry menu: pick a server (IP + port) and a username, then Join. Drawn
// with the game's own widget kit (client/gui.hpp) — no ImGui. Join pushes the
// LobbyScene on top; the lobby's "Back" pops back here (fields intact).
class ConnectScene : public client::Scene
{
public:
    explicit ConnectScene(client::Engine* engine) : Scene(engine) {}

    auto handle_event(const SDL_Event&) -> Propagation override { return Continue; }
    auto update(float) -> Propagation override
    {
        engine_->audio().music("music_lobby"); // idempotent (lobby stacks over us)
        return Continue;
    }

    auto render(float) -> Propagation override
    {
        // The lobby stacks on top of us; skip drawing while covered.
        if (!engine_->scenes().is_top(this)) { return Continue; }

        SDL_Renderer* r = engine_->renderer();
        SDL_SetRenderDrawColor(r, 20, 22, 28, 255);
        SDL_RenderClear(r);

        client::Gui& ui = engine_->gui();
        const float s = ui.scale();
        const float w = static_cast<float>(engine_->width());
        const float h = static_cast<float>(engine_->height());

        const float pw = 150.0f * s;
        const float ph = 120.0f * s;
        const float px = (w - pw) * 0.5f;
        const float py = (h - ph) * 0.5f;

        ui.text_centered(w * 0.5f, py - (26.0f * s), "SOULKEEPER", client::colors::accent,
                         16.0f * s);
        ui.panel(px, py, pw, ph);

        const float inner_x = px + (14.0f * s);
        const float inner_w = pw - (28.0f * s);
        float y = py + (14.0f * s);
        const float row = ui.input_h() + (10.0f * s);

        ui.text(inner_x, y, "SERVER IP", client::colors::dim);
        y += ui.line_h() * 0.8f;
        bool submit = ui.input("connect_ip", ip_, inner_x, y, inner_w * 0.62f);
        submit = ui.input("connect_port", port_, inner_x + (inner_w * 0.66f), y,
                          inner_w * 0.34f, /*numeric=*/true)
                 || submit;
        y += row;

        ui.text(inner_x, y, "USERNAME", client::colors::dim);
        y += ui.line_h() * 0.8f;
        submit = ui.input("connect_name", name_, inner_x, y, inner_w) || submit;
        y += row + (4.0f * s);

        const float bw = 60.0f * s;
        if (ui.button("JOIN", px + ((pw - bw) * 0.5f), y, bw, ui.button_h()) || submit) { join(); }

        return Continue;
    }

    ConnectScene(const ConnectScene&) = delete;
    ConnectScene(ConnectScene&&) = delete;
    ConnectScene& operator=(const ConnectScene&) = delete;
    ConnectScene& operator=(ConnectScene&&) = delete;
    ~ConnectScene() override = default;

private:
    void join()
    {
        engine_->audio().play("select");
        int port = proto::default_port;
        if (!port_.empty()) {
            int parsed = 0;
            std::from_chars(port_.data(), port_.data() + port_.size(), parsed);
            if (parsed > 0) { port = std::clamp(parsed, 1, 65535); }
        }
        const std::string ip = ip_.empty() ? "127.0.0.1" : ip_;
        const std::string name = name_.empty() ? "Player" : name_;

        client::Session& session = engine_->session();
        session.configure(ip.c_str(), static_cast<std::uint16_t>(port), name.c_str());
        session.connect();
        engine_->scenes().push<LobbyScene>(engine_); // lobby on top; Back pops it
    }

    std::string ip_ = "127.0.0.1";
    std::string name_ = "Player";
    std::string port_ = std::to_string(proto::default_port);
};
