#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include <miniaudio.h>

#include "engine/audio/audio_engine.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "engine/core/log.h"

namespace eng {

struct AudioEngine::Impl {
    ma_engine engine{};
    bool engine_ready = false;

    // Decoded templates, keyed by path string.
    std::unordered_map<std::string, std::unique_ptr<ma_sound>> templates;
    // Live one-shot instances; culled in update().
    std::vector<std::unique_ptr<ma_sound>> playing;

    ~Impl() {
        // Instances must die before their templates, templates before engine.
        for (auto& sound : playing) {
            ma_sound_uninit(sound.get());
        }
        playing.clear();
        for (auto& [path, sound] : templates) {
            ma_sound_uninit(sound.get());
        }
        templates.clear();
        if (engine_ready) {
            ma_engine_uninit(&engine);
        }
    }
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {}

std::optional<AudioEngine> AudioEngine::create() {
    AudioEngine audio;
    if (ma_engine_init(nullptr, &audio.impl_->engine) != MA_SUCCESS) {
        log::error("miniaudio engine init failed (no audio device?)");
        return std::nullopt;
    }
    audio.impl_->engine_ready = true;
    log::info("Audio engine initialized");
    return audio;
}

AudioEngine::~AudioEngine() = default;
AudioEngine::AudioEngine(AudioEngine&&) noexcept = default;
AudioEngine& AudioEngine::operator=(AudioEngine&&) noexcept = default;

void AudioEngine::play(const std::filesystem::path& path, float volume) {
    if (!impl_->engine_ready) {
        return;
    }
    const std::string key = path.string();

    ma_sound* source = nullptr;
    if (const auto it = impl_->templates.find(key); it != impl_->templates.end()) {
        source = it->second.get();
    } else {
        auto loaded = std::make_unique<ma_sound>();
        const ma_result result =
            ma_sound_init_from_file(&impl_->engine, key.c_str(),
                                    MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION,
                                    nullptr, nullptr, loaded.get());
        if (result != MA_SUCCESS) {
            log::error("Audio: failed to load '{}' (ma_result {})", key,
                       static_cast<int>(result));
            return;
        }
        source = loaded.get();
        impl_->templates.emplace(key, std::move(loaded));
    }

    auto instance = std::make_unique<ma_sound>();
    if (ma_sound_init_copy(&impl_->engine, source, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr,
                           instance.get()) != MA_SUCCESS) {
        log::warn("Audio: failed to instance '{}'", key);
        return;
    }
    ma_sound_set_volume(instance.get(), volume);
    ma_sound_start(instance.get());
    impl_->playing.push_back(std::move(instance));
}

void AudioEngine::set_master_volume(float volume) {
    if (impl_->engine_ready) {
        ma_engine_set_volume(&impl_->engine, volume);
    }
}

void AudioEngine::update() {
    std::erase_if(impl_->playing, [](const std::unique_ptr<ma_sound>& sound) {
        if (ma_sound_at_end(sound.get()) != 0) {
            ma_sound_uninit(sound.get());
            return true;
        }
        return false;
    });
}

}  // namespace eng
