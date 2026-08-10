#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include <miniaudio.h>

#include "engine/audio/audio_engine.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "engine/audio/audio_listener.h"
#include "engine/core/log.h"

namespace eng {

namespace {

// Attenuation policy for a ~45 m arena (arena01 is 40x40, arena02 48x32).
//
// Inverse-distance is the model to reach for: it is the physical law for a
// point source, and unlike the linear model it never reaches zero, so a shot
// across the map stays audible instead of dropping off a cliff at some
// arbitrary radius. The other two knobs are what adapt it to a room this
// size:
//
//  - min distance 5 m: no falloff at all inside that radius. A rifle going
//    off at arm's length should not be louder than one across the room by a
//    factor of fifty, and it also keeps the 1/d curve away from its
//    singularity at zero.
//  - max distance 60 m: past the longest diagonal of either arena (56.6 m and
//    57.7 m), so the curve is never clipped anywhere a player can stand.
//  - rolloff 0.5: half the physical rate. A true 1/d law puts the far corner
//    of the map at -21 dB, which is technically audible and practically
//    ignorable; halved, the far corner sits at 0.16 gain -- within a few
//    percent of the 0.15 floor the old hand-faked distance curve used, so
//    perceived loudness is roughly preserved while direction becomes real.
constexpr float kMinDistanceMeters = 5.0f;
constexpr float kMaxDistanceMeters = 60.0f;
constexpr float kRolloff = 0.5f;

}  // namespace

struct AudioEngine::Impl {
    ma_engine engine{};
    bool engine_ready = false;

    // Mirrors what was last pushed into miniaudio's listener. Kept so a
    // spatial play can report where the sound landed relative to the ears
    // without reading back through miniaudio's atomics.
    glm::vec3 listener_position{0.0f};
    glm::vec3 listener_forward{0.0f, 0.0f, -1.0f};
    glm::vec3 listener_up{0.0f, 1.0f, 0.0f};

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

void AudioEngine::set_listener(const glm::vec3& position, const glm::vec3& forward,
                               const glm::vec3& up) {
    if (!impl_->engine_ready) {
        return;
    }
    impl_->listener_position = position;
    impl_->listener_forward = forward;
    impl_->listener_up = up;
    // No axis flip: miniaudio's listener is right-handed with -Z forward by
    // default (ma_handedness_right), the same convention as eng::Camera.
    ma_engine_listener_set_position(&impl_->engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&impl_->engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&impl_->engine, 0, up.x, up.y, up.z);
}

void AudioEngine::play(const std::filesystem::path& path, float volume) {
    start(path, volume, nullptr);
}

void AudioEngine::play_at(const std::filesystem::path& path, const glm::vec3& position,
                          float volume) {
    start(path, volume, &position);
}

void AudioEngine::start(const std::filesystem::path& path, float volume,
                        const glm::vec3* position) {
    if (!impl_->engine_ready) {
        return;
    }
    const std::string key = path.string();

    ma_sound* source = nullptr;
    if (const auto it = impl_->templates.find(key); it != impl_->templates.end()) {
        source = it->second.get();
    } else {
        // The template is never started, so its own spatialization flag is
        // irrelevant; ma_sound_init_copy takes the instance's flags.
        auto loaded = std::make_unique<ma_sound>();
        const ma_result result = ma_sound_init_from_file(
            &impl_->engine, key.c_str(), MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION,
            nullptr, nullptr, loaded.get());
        if (result != MA_SUCCESS) {
            log::error("Audio: failed to load '{}' (ma_result {})", key, static_cast<int>(result));
            return;
        }
        source = loaded.get();
        impl_->templates.emplace(key, std::move(loaded));
    }

    // The cast is not decoration: miniaudio's sound flags are enumerators,
    // and GCC's -Wextra rejects a conditional whose arms are an enum and a
    // plain integer. Both arms have to be ma_uint32.
    const ma_uint32 no_spatialization = static_cast<ma_uint32>(MA_SOUND_FLAG_NO_SPATIALIZATION);
    const ma_uint32 flags = position != nullptr ? ma_uint32{0} : no_spatialization;
    auto instance = std::make_unique<ma_sound>();
    if (ma_sound_init_copy(&impl_->engine, source, flags, nullptr, instance.get()) != MA_SUCCESS) {
        log::warn("Audio: failed to instance '{}'", key);
        return;
    }
    if (position != nullptr) {
        // Position before start, so the very first mixed frame is already
        // panned; a shot that begins centred and slides into place clicks.
        ma_sound_set_position(instance.get(), position->x, position->y, position->z);
        ma_sound_set_attenuation_model(instance.get(), ma_attenuation_model_inverse);
        ma_sound_set_min_distance(instance.get(), kMinDistanceMeters);
        ma_sound_set_max_distance(instance.get(), kMaxDistanceMeters);
        ma_sound_set_rolloff(instance.get(), kRolloff);

        // Trace, not debug: one line per gunshot is far too much for normal
        // play, but it is the only way to check the left/right sign of a
        // running client from the outside. The local coordinates come from
        // the same maths miniaudio pans with (audio_listener.h), so "right"
        // here is what the right speaker gets.
        const glm::vec3 local = listener_space(impl_->listener_position, impl_->listener_forward,
                                               impl_->listener_up, *position);
        log::trace(
            "Audio 3D '{}' emitter ({:.2f}, {:.2f}, {:.2f}) listener ({:.2f}, {:.2f}, {:.2f}) "
            "forward ({:.2f}, {:.2f}, {:.2f}) -> local ({:.2f}, {:.2f}, {:.2f}) {} {} {:.2f} m",
            key, position->x, position->y, position->z, impl_->listener_position.x,
            impl_->listener_position.y, impl_->listener_position.z, impl_->listener_forward.x,
            impl_->listener_forward.y, impl_->listener_forward.z, local.x, local.y, local.z,
            local.x >= 0.0f ? "right" : "left", local.z <= 0.0f ? "front" : "behind",
            glm::length(local));
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
