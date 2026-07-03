#pragma once
#include "client/renderer.hpp"
#include <SDL3/SDL.h>
#include <charconv>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace client {

// One animation strip: N frames laid out horizontally in a single texture.
// The pack convention makes the filename the metadata: <Clip>_<N>x1.png.
struct AnimClip
{
    SDL_Texture* tex = nullptr;
    int frames = 1;
    float frame_w = 0.0f;
    float frame_h = 0.0f;
};

// An animation pack = one character folder of clip strips. Some packs prefix
// clips with the character name (Goblin_Regular_01_Move), some don't (Move),
// so lookup matches exact first, then by "_<name>" suffix.
struct SpritePack
{
    std::vector<std::pair<std::string, AnimClip>> clips;

    [[nodiscard]] const AnimClip* clip(std::string_view name) const
    {
        for (const auto& [stem, c] : clips) {
            if (stem == name) { return &c; }
        }
        for (const auto& [stem, c] : clips) {
            if (stem.size() > name.size() + 1 && stem.ends_with(name)
                && stem[stem.size() - name.size() - 1] == '_') {
                return &c;
            }
        }
        return nullptr;
    }
};

// Discovers and caches animation packs. A pack path is a directory; every
// *_<N>x1.png inside becomes a clip (frame width = texture width / N). Misses
// (not a directory, no strips) cache as null so callers can probe every frame
// and fall back to a static texture for plain .png sprite paths.
class SpritePacks
{
public:
    explicit SpritePacks(Textures* textures) : textures_{ textures } {}

    [[nodiscard]] const SpritePack* get(const std::string& dir)
    {
        if (const auto it = cache_.find(dir); it != cache_.end()) { return it->second.get(); }

        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) {
            cache_.emplace(dir, nullptr);
            return nullptr;
        }

        auto pack = std::make_unique<SpritePack>();
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".png") { continue; }
            const std::string stem = entry.path().stem().string();
            const auto [clip_name, frames] = parse_strip(stem);
            if (frames <= 0) { continue; }
            SDL_Texture* tex = textures_->get(entry.path().string());
            if (tex == nullptr) { continue; }
            float w = 0.0f;
            float h = 0.0f;
            SDL_GetTextureSize(tex, &w, &h);
            pack->clips.emplace_back(
              clip_name, AnimClip{ .tex = tex, .frames = frames,
                                   .frame_w = w / static_cast<float>(frames), .frame_h = h });
        }
        if (pack->clips.empty()) { pack = nullptr; }
        return cache_.emplace(dir, std::move(pack)).first->second.get();
    }

private:
    // "Goblin_Regular_01_Move_10x1" -> { "Goblin_Regular_01_Move", 10 }.
    // Only horizontal strips (Nx1) qualify; anything else returns frames 0.
    [[nodiscard]] static std::pair<std::string, int> parse_strip(const std::string& stem)
    {
        const std::size_t us = stem.rfind('_');
        if (us == std::string::npos) { return { {}, 0 }; }
        const std::string_view tail{ stem.data() + us + 1, stem.size() - us - 1 };
        const std::size_t x = tail.find('x');
        if (x == std::string_view::npos || tail.substr(x + 1) != "1") { return { {}, 0 }; }
        int frames = 0;
        const auto [ptr, err] = std::from_chars(tail.data(), tail.data() + x, frames);
        if (err != std::errc{} || ptr != tail.data() + x || frames <= 0) { return { {}, 0 }; }
        return { stem.substr(0, us), frames };
    }

    Textures* textures_;
    std::unordered_map<std::string, std::unique_ptr<SpritePack>> cache_;
};

// Draw one animated clip centered on (cx, cy), scaled to target_h keeping the
// frame's pixel aspect. Packs face RIGHT; flip mirrors for leftward movement.
inline void draw_clip(SDL_Renderer* r, const AnimClip& c, float cx, float cy, float target_h,
                      float time, bool flip, SDL_Color tint = { 255, 255, 255, 255 })
{
    constexpr float fps = 12.0f;
    const int frame = c.frames > 1 ? static_cast<int>(time * fps) % c.frames : 0;
    const SDL_FRect src{ .x = static_cast<float>(frame) * c.frame_w, .y = 0.0f,
                         .w = c.frame_w, .h = c.frame_h };
    const float w = c.frame_w * (target_h / c.frame_h);
    const SDL_FRect dst{ .x = cx - (w * 0.5f), .y = cy - (target_h * 0.5f), .w = w, .h = target_h };
    SDL_SetTextureColorMod(c.tex, tint.r, tint.g, tint.b);
    SDL_RenderTextureRotated(r, c.tex, &src, &dst, 0.0, nullptr,
                             flip ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    SDL_SetTextureColorMod(c.tex, 255, 255, 255);
}

} // namespace client
