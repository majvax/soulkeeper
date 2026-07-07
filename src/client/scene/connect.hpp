#pragma once
#include "client/engine.hpp"
#include "client/scene.hpp"
#include "client/scene/lobby.hpp" // Join transitions to the lobby
#include <algorithm>
#include <imgui.h>

// The entry menu: pick a server (IP + port) and a username, then Join. Replaces
// the old argv host/name. Join pushes the LobbyScene on top; the lobby's "Back"
// pops back here (this scene persists underneath with its fields intact).
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
        // The lobby stacks on top of us. ImGui windows composite after ALL SDL,
        // so drawing while covered would float this menu over the lobby — skip.
        if (!engine_->scenes().is_top(this)) { return Continue; }

        SDL_Renderer* r = engine_->renderer();
        SDL_SetRenderDrawColor(r, 20, 22, 28, 255);
        SDL_RenderClear(r);

        ImGui::Begin("Connect to server");
        ImGui::InputText("IP", ip_, sizeof(ip_));
        ImGui::InputInt("Port", &port_);
        ImGui::InputText("Username", name_, sizeof(name_));
        ImGui::Separator();
        if (ImGui::Button("Join", ImVec2(120, 0))) { join(); }
        ImGui::End();
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
        const char* ip = (ip_[0] != '\0') ? ip_ : "127.0.0.1";
        const char* name = (name_[0] != '\0') ? name_ : "Player";
        const auto port = static_cast<std::uint16_t>(std::clamp(port_, 1, 65535));

        client::Session& session = engine_->session();
        session.configure(ip, port, name);
        session.connect();
        engine_->scenes().push<LobbyScene>(engine_); // lobby on top; Back pops it
    }

    char ip_[64] = "127.0.0.1";
    char name_[32] = "Player";
    int port_ = proto::default_port;
};
