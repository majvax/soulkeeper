#pragma once
#include <SDL3/SDL.h>
#include <concepts>
#include <deque>
#include <functional>
#include <memory>
#include <ranges>
#include <vector>

namespace client {

class Engine; // forward-declared: Scene only stores an Engine* (breaks the engine<->scene cycle)

class Scene
{
protected:
    Engine* engine_;

public:
    using Propagation = bool;
    static constexpr Propagation Continue = true;
    static constexpr Propagation Stop = false;

    explicit Scene(Engine* engine) : engine_{ engine } {}
    Scene(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene& operator=(Scene&&) = delete;
    virtual ~Scene() = default;

    virtual auto handle_event(const SDL_Event& event) -> Propagation = 0;
    virtual auto update(float dt) -> Propagation = 0;
    virtual auto render(float alpha) -> Propagation = 0;
};

template<typename T>
concept scene = std::derived_from<T, Scene>;

// A stack of scenes. Mutations (push/pop/clear) are DEFERRED into a command
// queue so a scene can request a transition from inside its own update/
// handle_event without invalidating the in-progress iteration; the Engine calls
// apply_pending() at safe points (after the event and update phases).
class SceneManager
{
public:
    template<scene T>
    void push(Engine* engine)
    {
        pending_.emplace_back([this, engine] { scenes_.emplace_front(std::make_unique<T>(engine)); });
    }

    void pop()
    {
        pending_.emplace_back([this] {
            if (!scenes_.empty()) { scenes_.pop_front(); }
        });
    }

    void clear()
    {
        pending_.emplace_back([this] { scenes_.clear(); });
    }

    void apply_pending()
    {
        for (auto& command : pending_) { command(); }
        pending_.clear();
    }

    [[nodiscard]] bool empty() const { return scenes_.empty(); }

    void handle_event(const SDL_Event& event)
    {
        for (auto& scene : scenes_) {
            if (!scene->handle_event(event)) { return; }
        }
    }

    void update(float dt)
    {
        for (auto& scene : scenes_) {
            if (!scene->update(dt)) { return; }
        }
    }

    void render(float alpha) const
    {
        for (const auto& scene : std::views::reverse(scenes_)) {
            if (!scene->render(alpha)) { return; }
        }
    }

private:
    std::deque<std::unique_ptr<Scene>> scenes_;
    std::vector<std::function<void()>> pending_;
};

} // namespace client
