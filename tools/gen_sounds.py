#!/usr/bin/env python3
"""Generates placeholder sound effects into assets/sounds/ (16-bit mono WAV).

Pure stdlib (wave + math + random with fixed seed) so the sounds are
reproducible anywhere. These are deliberately simple synthesized effects;
replace with real recordings whenever.

Usage: python3 tools/gen_sounds.py
"""

import math
import random
import struct
import wave
from pathlib import Path

RATE = 44100


def write_wav(path: Path, samples):
    path.parent.mkdir(parents=True, exist_ok=True)
    clipped = [max(-1.0, min(1.0, s)) for s in samples]
    with wave.open(str(path), "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(b"".join(struct.pack("<h", int(s * 32767)) for s in clipped))
    print(f"wrote {path} ({len(samples) / RATE * 1000:.0f} ms)")


def seconds(n):
    return int(n * RATE)


def noise_burst(duration, decay, volume=1.0, rng=None):
    rng = rng or random.Random(1234)
    out = []
    for i in range(seconds(duration)):
        t = i / RATE
        out.append(volume * (rng.random() * 2 - 1) * math.exp(-t * decay))
    return out


def tone(duration, freq_start, freq_end=None, decay=8.0, volume=0.8):
    freq_end = freq_end if freq_end is not None else freq_start
    out = []
    phase = 0.0
    n = seconds(duration)
    for i in range(n):
        t = i / RATE
        f = freq_start + (freq_end - freq_start) * (i / max(1, n - 1))
        phase += 2 * math.pi * f / RATE
        out.append(volume * math.sin(phase) * math.exp(-t * decay))
    return out


def mix(*tracks):
    n = max(len(t) for t in tracks)
    out = [0.0] * n
    for track in tracks:
        for i, s in enumerate(track):
            out[i] += s
    return out


def delayed(track, delay):
    return [0.0] * seconds(delay) + track


def main():
    root = Path(__file__).resolve().parent.parent / "assets" / "sounds"
    rng = random.Random(42)

    # Rifle shot: sharp noise crack + low thump.
    write_wav(root / "fire.wav",
              mix(noise_burst(0.09, 55.0, 0.9, rng), tone(0.12, 140, 60, decay=25, volume=0.7)))

    # Dry fire: tiny click.
    write_wav(root / "dry.wav", noise_burst(0.02, 220.0, 0.5, rng))

    # Reload: two mechanical clicks.
    click = mix(noise_burst(0.03, 160.0, 0.6, rng), tone(0.03, 2400, decay=90, volume=0.2))
    write_wav(root / "reload.wav", mix(click, delayed(click, 0.28)))

    # Hit confirm: short high blip.
    write_wav(root / "hit.wav", tone(0.07, 1300, decay=40, volume=0.5))

    # Kill confirm: descending two-tone.
    write_wav(root / "kill.wav", mix(tone(0.25, 880, 440, decay=10, volume=0.5),
                                     delayed(tone(0.18, 587, 294, decay=12, volume=0.4), 0.08)))

    # Death: low descending rumble.
    write_wav(root / "death.wav",
              mix(tone(0.5, 220, 60, decay=6, volume=0.7), noise_burst(0.4, 12.0, 0.25, rng)))

    # Jump: soft thump.
    write_wav(root / "jump.wav", tone(0.08, 180, 120, decay=30, volume=0.4))


if __name__ == "__main__":
    main()
