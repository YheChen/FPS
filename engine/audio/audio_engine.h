#pragma once

#include <filesystem>
#include <memory>
#include <optional>

namespace eng {

// Fire-and-forget sound playback on miniaudio. Decoded sounds are cached by
// path; each play() spawns an independent instance. miniaudio mixes on its
// own audio thread; this class must only be used from the main thread.
class AudioEngine {
public:
    static std::optional<AudioEngine> create();

    ~AudioEngine();
    AudioEngine(AudioEngine&&) noexcept;
    AudioEngine& operator=(AudioEngine&&) noexcept;
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Plays a WAV/MP3/FLAC file at the given volume (0..1). Loads and caches
    // on first use; failures are logged and swallowed (sound is never fatal).
    void play(const std::filesystem::path& path, float volume = 1.0f);

    // Reclaims finished sound instances. Call once per frame.
    void update();

    // Master volume multiplier (0..1) applied to all sounds.
    void set_master_volume(float volume);

private:
    AudioEngine();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace eng
