// src/client/audio.hpp
//
// The client's audio mixer, built directly on SDL3 audio streams: the device
// natively mixes every bound stream, so "the mixer" is a small pool of one-shot
// SFX streams plus one dedicated looping music stream. No SDL_mixer.
//
// Philosophy matches Textures: audio is best-effort cosmetics. A missing
// device, driver, or file degrades to silence (logged once), never to an error
// the game has to handle. Clips load lazily by canonical name from
// assets/sound/<name>.wav|.ogg (WAV via SDL, OGG via stb_vorbis); mods can
// rebind any name to their own file (see set_override / mod:sound).
#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace client {

class Audio
{
public:
    // Brings up SDL_INIT_AUDIO + the default playback device. Failure leaves
    // the object in silent mode (every call a no-op) — never fatal.
    Audio();
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    // One-shot SFX. `jitter` adds ±6% playback-rate variance so rapid repeats
    // (shots, hits) don't machine-gun the exact same sample. Same-name plays
    // are throttled to >= 40 ms apart.
    void play(const std::string& name, float volume = 1.0f, bool jitter = true);

    // Positional one-shot: quadratic falloff with distance from the listener
    // (the local player), silent past `full_silence_dist_`.
    void play_at(const std::string& name, float wx, float wy, float lx, float ly, float volume = 1.0f);

    // Looping music. Idempotent (same track = no-op); switching cross-fades
    // ~0.4 s out, swaps, fades back in. Empty name == stop_music().
    void music(const std::string& name);
    void stop_music() { music(""); }

    // Per-frame housekeeping: music fade ramp + loop-queue refill.
    void update(float dt);

    // Master/category volumes (clamped 0..1). Applied at play time (SFX) or
    // every frame (music), so changes take effect immediately.
    void set_master(float v);
    void set_sfx(float v);
    void set_music_volume(float v);
    [[nodiscard]] float master() const noexcept { return master_; }
    [[nodiscard]] float sfx() const noexcept { return sfx_; }
    [[nodiscard]] float music_volume() const noexcept { return music_vol_; }

    // Rebind a canonical (or brand-new) sound name to a file path — the
    // mod:sound verb. Must land before the name's first play (clips cache).
    void set_override(const std::string& name, std::string path) { overrides_[name] = std::move(path); }

    // True if `name` resolves to a playable sound (override or assets file).
    // Quiet — for OPTIONAL names (per-variant "shoot_<N>") where absence is the
    // normal case and means "use the fallback", not a broken asset.
    [[nodiscard]] bool has(const std::string& name)
    {
        return device_ != 0 && !clip(name, /*quiet=*/true)->pcm.empty();
    }

    [[nodiscard]] bool ok() const noexcept { return device_ != 0; }

private:
    // Decoded PCM + its format. Empty pcm = missing/failed file (cached so a
    // missing sound is one log line, not a per-frame stat()).
    struct Clip
    {
        std::vector<std::uint8_t> pcm;
        SDL_AudioSpec spec{};
    };

    Clip* clip(const std::string& name, bool quiet = false); // lazy load + cache
    [[nodiscard]] bool load_wav(const std::string& path, Clip& out);
    [[nodiscard]] bool load_ogg(const std::string& path, Clip& out);
    void start_track(const std::string& name);

    SDL_AudioDeviceID device_ = 0;

    static constexpr std::size_t voice_count = 16;
    std::array<SDL_AudioStream*, voice_count> voices_{};

    std::unordered_map<std::string, Clip> clips_;
    std::unordered_map<std::string, std::string> overrides_;
    std::unordered_map<std::string, std::uint64_t> last_play_ms_;

    // Music state: one stream, whole-clip PCM looped by refilling the queue.
    SDL_AudioStream* music_stream_ = nullptr;
    std::string music_name_;    // currently playing (or fading out)
    std::string music_pending_; // switch target once the fade-out lands
    Clip* music_clip_ = nullptr;
    std::size_t music_pos_ = 0; // byte offset into music_clip_->pcm
    float music_fade_ = 0.0f;   // current fade gain 0..1
    bool music_fading_out_ = false;

    float master_ = 1.0f;
    float sfx_ = 1.0f;
    float music_vol_ = 0.6f; // music sits under the SFX by default

    static constexpr float fade_speed = 2.5f;             // full fade in ~0.4 s
    static constexpr float full_silence_dist_ = 1000.0f;  // play_at falloff reach
    static constexpr std::uint64_t throttle_ms = 40;      // same-clip spam guard
};

} // namespace client
