// src/client/audio.cpp
#include "client/audio.hpp"

#include <algorithm>
#include <cstdio>
#include <random>

// Declarations only — the implementation TU is stb_vorbis_impl.cpp (both are
// C++ so the linkage matches).
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#undef STB_VORBIS_HEADER_ONLY

namespace client {

Audio::Audio()
{
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "[audio] init failed (%s) — running silent\n", SDL_GetError());
        return;
    }
    device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (device_ == 0) {
        std::fprintf(stderr, "[audio] no playback device (%s) — running silent\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return;
    }
    // Voices declare a nominal source format up front; each play() re-points
    // the format at the clip actually queued.
    const SDL_AudioSpec nominal{ .format = SDL_AUDIO_S16LE, .channels = 2, .freq = 44100 };
    for (SDL_AudioStream*& v : voices_) {
        v = SDL_CreateAudioStream(&nominal, nullptr);
        if (v != nullptr) { SDL_BindAudioStream(device_, v); }
    }
    music_stream_ = SDL_CreateAudioStream(&nominal, nullptr);
    if (music_stream_ != nullptr) { SDL_BindAudioStream(device_, music_stream_); }
}

Audio::~Audio()
{
    if (device_ == 0) { return; }
    for (SDL_AudioStream* v : voices_) {
        if (v != nullptr) { SDL_DestroyAudioStream(v); } // auto-unbinds
    }
    if (music_stream_ != nullptr) { SDL_DestroyAudioStream(music_stream_); }
    SDL_CloseAudioDevice(device_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool Audio::load_wav(const std::string& path, Clip& out)
{
    SDL_AudioSpec spec{};
    Uint8* buf = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV(path.c_str(), &spec, &buf, &len)) { return false; }
    out.spec = spec;
    out.pcm.assign(buf, buf + len);
    SDL_free(buf);
    return true;
}

bool Audio::load_ogg(const std::string& path, Clip& out)
{
    int channels = 0;
    int rate = 0;
    short* samples = nullptr;
    const int frames = stb_vorbis_decode_filename(path.c_str(), &channels, &rate, &samples);
    if (frames <= 0 || samples == nullptr) { return false; }
    out.spec = SDL_AudioSpec{ .format = SDL_AUDIO_S16LE, .channels = channels, .freq = rate };
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(samples);
    out.pcm.assign(bytes, bytes + (static_cast<std::size_t>(frames) * channels * sizeof(short)));
    free(samples); // stb_vorbis allocates with malloc
    return true;
}

Audio::Clip* Audio::clip(const std::string& name, bool quiet)
{
    if (const auto it = clips_.find(name); it != clips_.end()) { return &it->second; }

    Clip c;
    bool loaded = false;
    if (const auto ov = overrides_.find(name); ov != overrides_.end()) {
        loaded = ov->second.ends_with(".ogg") ? load_ogg(ov->second, c) : load_wav(ov->second, c);
        if (!loaded) { std::fprintf(stderr, "[audio] '%s': can't load %s\n", name.c_str(), ov->second.c_str()); }
    } else {
        loaded = load_wav("assets/sound/" + name + ".wav", c)
                 || load_ogg("assets/sound/" + name + ".ogg", c);
        if (!loaded && !quiet) {
            std::fprintf(stderr, "[audio] no assets/sound/%s.{wav,ogg} — silent\n", name.c_str());
        }
    }
    return &clips_.emplace(name, std::move(c)).first->second;
}

void Audio::play(const std::string& name, float volume, bool jitter)
{
    if (device_ == 0 || volume <= 0.0f) { return; }
    Clip* c = clip(name);
    if (c->pcm.empty()) { return; }

    const std::uint64_t now = SDL_GetTicks();
    if (auto [it, fresh] = last_play_ms_.try_emplace(name, now); !fresh) {
        if (now - it->second < throttle_ms) { return; }
        it->second = now;
    }

    // Prefer a fully drained voice; otherwise steal the first one (oldest
    // sounds die first only by luck, but 16 voices make collisions rare).
    SDL_AudioStream* voice = nullptr;
    for (SDL_AudioStream* v : voices_) {
        if (v != nullptr && SDL_GetAudioStreamQueued(v) == 0 && SDL_GetAudioStreamAvailable(v) == 0) {
            voice = v;
            break;
        }
    }
    if (voice == nullptr) {
        voice = voices_[0];
        if (voice == nullptr) { return; }
        SDL_ClearAudioStream(voice);
    }

    SDL_SetAudioStreamFormat(voice, &c->spec, nullptr);
    SDL_SetAudioStreamGain(voice, std::clamp(volume, 0.0f, 1.0f) * sfx_ * master_);
    if (jitter) {
        static std::minstd_rand rng{ std::random_device{}() };
        const float ratio = 0.94f + (static_cast<float>(rng() % 1000) / 1000.0f) * 0.12f;
        SDL_SetAudioStreamFrequencyRatio(voice, ratio);
    } else {
        SDL_SetAudioStreamFrequencyRatio(voice, 1.0f);
    }
    SDL_PutAudioStreamData(voice, c->pcm.data(), static_cast<int>(c->pcm.size()));
    SDL_FlushAudioStream(voice); // one-shot: let the resampler drain the tail
}

void Audio::play_at(const std::string& name, float wx, float wy, float lx, float ly, float volume)
{
    const float dx = wx - lx;
    const float dy = wy - ly;
    const float d = SDL_sqrtf((dx * dx) + (dy * dy));
    const float att = std::clamp(1.0f - (d / full_silence_dist_), 0.0f, 1.0f);
    if (att <= 0.0f) { return; }
    play(name, volume * att * att);
}

void Audio::music(const std::string& name)
{
    if (device_ == 0 || music_stream_ == nullptr) { return; }
    if (name == music_name_ && !music_fading_out_) { return; }  // already on it
    if (name == music_name_ && music_fading_out_ && music_pending_ == name) { return; }
    if (music_name_.empty()) {
        start_track(name); // nothing playing: no fade-out leg needed
        return;
    }
    music_pending_ = name;
    music_fading_out_ = true;
}

void Audio::start_track(const std::string& name)
{
    music_name_ = name;
    music_pending_.clear();
    music_fading_out_ = false;
    music_pos_ = 0;
    music_clip_ = nullptr;
    SDL_ClearAudioStream(music_stream_);
    if (name.empty()) { return; }
    Clip* c = clip(name);
    if (c->pcm.empty()) {
        music_name_.clear();
        return;
    }
    music_clip_ = c;
    SDL_SetAudioStreamFormat(music_stream_, &c->spec, nullptr);
    music_fade_ = 0.0f; // ramp in from silence
}

void Audio::update(float dt)
{
    if (device_ == 0 || music_stream_ == nullptr) { return; }

    if (music_fading_out_) {
        music_fade_ -= fade_speed * dt;
        if (music_fade_ <= 0.0f) { start_track(music_pending_); }
    } else if (music_fade_ < 1.0f) {
        music_fade_ = std::min(1.0f, music_fade_ + (fade_speed * dt));
    }

    if (music_clip_ == nullptr) { return; }
    SDL_SetAudioStreamGain(music_stream_, music_fade_ * music_vol_ * master_);

    // Keep ~2 s queued; wrap at the end of the clip = seamless loop.
    const std::size_t low_water =
      static_cast<std::size_t>(music_clip_->spec.freq) * static_cast<std::size_t>(music_clip_->spec.channels)
      * sizeof(short) * 2;
    while (static_cast<std::size_t>(SDL_GetAudioStreamQueued(music_stream_)) < low_water) {
        const std::size_t remain = music_clip_->pcm.size() - music_pos_;
        const std::size_t chunk = std::min(remain, low_water);
        SDL_PutAudioStreamData(music_stream_, music_clip_->pcm.data() + music_pos_,
                               static_cast<int>(chunk));
        music_pos_ = (music_pos_ + chunk) % music_clip_->pcm.size();
    }
}

void Audio::set_master(float v) { master_ = std::clamp(v, 0.0f, 1.0f); }
void Audio::set_sfx(float v) { sfx_ = std::clamp(v, 0.0f, 1.0f); }
void Audio::set_music_volume(float v) { music_vol_ = std::clamp(v, 0.0f, 1.0f); }

} // namespace client
