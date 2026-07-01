#pragma once
#include <SDL3/SDL.h>
#include <stb_image.h>

#include <memory>
#include <string>
#include <unordered_map>

namespace client {

struct TextureDeleter
{
    void operator()(SDL_Texture* tex) const noexcept
    {
        if (tex != nullptr) { SDL_DestroyTexture(tex); }
    }
};
using TexturePtr = std::unique_ptr<SDL_Texture, TextureDeleter>;

// Loads and caches PNG textures (decoded with stb_image) for one renderer.
// A failed load is cached as null so we don't retry a missing file every frame.
class Textures
{
public:
    explicit Textures(SDL_Renderer* renderer) : renderer_{ renderer } {}

    [[nodiscard]] SDL_Texture* get(const std::string& path)
    {
        if (const auto it = cache_.find(path); it != cache_.end()) { return it->second.get(); }

        int width = 0;
        int height = 0;
        int channels = 0;
        unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
        if (pixels == nullptr) {
            cache_.emplace(path, TexturePtr{ nullptr });
            return nullptr;
        }

        SDL_Texture* tex =
          SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
        if (tex != nullptr) {
            SDL_UpdateTexture(tex, nullptr, pixels, width * 4);
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
        }
        stbi_image_free(pixels);
        cache_.emplace(path, TexturePtr{ tex });
        return tex;
    }

private:
    SDL_Renderer* renderer_;
    std::unordered_map<std::string, TexturePtr> cache_;
};

// Draw a texture centered on (cx, cy) at the given size.
inline void draw_centered(SDL_Renderer* renderer, SDL_Texture* tex, float cx, float cy, float w, float h)
{
    const SDL_FRect dst{ .x = cx - (w * 0.5f), .y = cy - (h * 0.5f), .w = w, .h = h };
    SDL_RenderTexture(renderer, tex, nullptr, &dst);
}

} // namespace client
