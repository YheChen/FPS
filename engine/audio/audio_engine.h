#pragma once

#include <filesystem>
#include <memory>
#include <optional>

#include <glm/glm.hpp>

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

    // Where the player's ears are, in world space. `forward` and `up` use the
    // engine convention (+Y up, -Z forward, right-handed), which is also
    // miniaudio's default, so they go straight through unflipped -- see
    // audio_listener.h. Drive this from the camera every frame.
    void set_listener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);

    // Plays a WAV/MP3/FLAC file at the given volume (0..1), unspatialized:
    // same in both ears, no distance falloff. This is the right call for
    // sounds that happen TO the listener rather than somewhere near them --
    // their own weapon, their own death, hit confirmation, UI. Panning those
    // makes the player's own head seem to be somewhere else.
    // Loads and caches on first use; failures are logged and swallowed
    // (sound is never fatal).
    //
    // `pitch` resamples playback (1.0 = as recorded, 1.05 = 5% up and 5%
    // shorter). It exists for the sounds that repeat: a footstep set of four
    // samples gives itself away within seconds when the same file comes back
    // bit-identical, and a few percent of pitch either way costs nothing and
    // makes the repeat inaudible. Do not use it to transpose -- a sound
    // designed at one pitch stops being that sound.
    void play(const std::filesystem::path& path, float volume = 1.0f, float pitch = 1.0f);

    // Plays the same file positioned at `position` in world space: panned and
    // attenuated against the listener. For anything that happens elsewhere in
    // the arena, where the direction is the information.
    void play_at(const std::filesystem::path& path, const glm::vec3& position, float volume = 1.0f,
                 float pitch = 1.0f);

    // Reclaims finished sound instances. Call once per frame.
    void update();

    // Master volume multiplier (0..1) applied to all sounds.
    void set_master_volume(float volume);

private:
    AudioEngine();
    // Shared body of play()/play_at(); a non-null `position` is what makes an
    // instance spatial.
    void start(const std::filesystem::path& path, float volume, float pitch,
               const glm::vec3* position);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace eng
