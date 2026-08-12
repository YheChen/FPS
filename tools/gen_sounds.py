#!/usr/bin/env python3
"""Generates the sound effects in assets/sounds/ (16-bit mono WAV).

Pure stdlib (wave + math + random with fixed seed) so the sounds are
reproducible anywhere. The feedback beeps are still deliberately simple
synthesized placeholders; the four weapon reports are not, because a player
has to tell the guns apart blind and that is a property of the SET rather
than of any one file -- see the table above rifle_fire(). The footsteps are
not either, for the same reason and one more: they are the sound a player is
expected to LOCALISE, and localisation is a spectral property -- see the
table above footstep().

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


# --- spectral shaping ------------------------------------------------------
# One-pole RC filters. 6 dB/octave is coarse, but it has no ringing to smear a
# transient, and what the weapons need is to be placed in a frequency BAND --
# that is the axis they are told apart on. Cascade a call for 12 dB/oct where
# a harder edge is wanted.


def low_pass(track, cutoff_start, cutoff_end=None):
    """One-pole low-pass. The cutoff glides start -> end across the track when
    `cutoff_end` is given, which is how a report losing its highs to air
    absorption is modelled: it does not just get quieter, it gets duller."""
    cutoff_end = cutoff_start if cutoff_end is None else cutoff_end
    out = []
    y = 0.0
    last = max(1, len(track) - 1)
    for i, x in enumerate(track):
        fc = cutoff_start + (cutoff_end - cutoff_start) * (i / last)
        a = 1.0 - math.exp(-2.0 * math.pi * fc / RATE)
        y += a * (x - y)
        out.append(y)
    return out


def high_pass(track, cutoff):
    """One-pole high-pass: whatever the low-pass did not keep."""
    return [x - y for x, y in zip(track, low_pass(track, cutoff))]


def band_pass(track, low_hz, high_hz):
    return high_pass(low_pass(track, high_hz), low_hz)


def resonant_noise(duration, freq, q, decay, volume, rng):
    """Decaying noise through a two-pole resonator. Bandwidth is freq/q, so a
    low q is a broad spectral tilt and a high q is a metallic ring. Gain is
    compensated so `volume` stays roughly comparable to noise_burst's."""
    r = math.exp(-math.pi * (freq / q) / RATE)
    c = 2.0 * r * math.cos(2.0 * math.pi * freq / RATE)
    g = (1.0 - r) * math.sqrt(1.0 - r * r)
    out, y1, y2 = [], 0.0, 0.0
    for i in range(seconds(duration)):
        x = (rng.random() * 2 - 1) * math.exp(-(i / RATE) * decay)
        y = g * x + c * y1 - r * r * y2
        y2, y1 = y1, y
        out.append(volume * y)
    return out


def white_noise(duration, rng):
    """Raw un-enveloped noise, so a caller can filter BEFORE shaping.
    noise_burst() envelopes first, which is right when the filter is there to
    colour the decay and wrong when it is there to shape the attack."""
    return [rng.random() * 2 - 1 for _ in range(seconds(duration))]


def decay_envelope(track, decay, volume=1.0):
    return [volume * s * math.exp(-(i / RATE) * decay) for i, s in enumerate(track)]


def sweep(duration, freq_start, freq_end, decay, volume):
    """Sine with an EXPONENTIAL pitch glide. tone() interpolates frequency
    linearly, which spends far too long up at the starting pitch to read as a
    falling boom; pitch is perceived logarithmically."""
    out, phase = [], 0.0
    n = seconds(duration)
    for i in range(n):
        f = freq_start * (freq_end / freq_start) ** (i / max(1, n - 1))
        phase += 2 * math.pi * f / RATE
        out.append(volume * math.sin(phase) * math.exp(-(i / RATE) * decay))
    return out


def scatter(track, count, max_delay, rng):
    """Smears one transient into `count` irregular taps inside `max_delay`,
    modelling a shot column leaving the bore. Spacing is deliberately
    aperiodic: a regular comb would ring at a pitch, and a pitch heard
    thousands of times is the definition of listening fatigue."""
    out = list(track) + [0.0] * seconds(max_delay)
    for _ in range(count - 1):
        offset = rng.uniform(0.0004, max_delay)
        # Taper by delay so the dry onset keeps the peak: this must widen the
        # attack, not push it later.
        gain = rng.uniform(0.20, 0.45) * (1.0 - offset / max_delay)
        start = seconds(offset)
        for i, s in enumerate(track):
            out[i + start] += s * gain
    return out


# --- envelope hygiene ------------------------------------------------------
# A shot that starts on a full-amplitude sample is a DC step, and one that
# stops on a non-zero sample is a click. Fired ten times a second, those
# artifacts stop being subtle.


def normalize(track, peak):
    """Scales to an exact peak. Used instead of hand-balanced layer volumes,
    both so a layer's weight is readable straight off the call site and so the
    finished mix can never reach write_wav's silent clamp."""
    loudest = max((abs(s) for s in track), default=0.0)
    return [s * (peak / loudest) for s in track] if loudest > 1e-9 else list(track)


def attack_ramp(track, ramp_seconds):
    """Raised-cosine onset over a fraction of a millisecond: removes the step
    at sample 0 while staying far faster than the ~1 ms the ear uses to place
    an onset, so the transient is still razor sharp and still localises."""
    n = max(1, seconds(ramp_seconds))
    out = list(track)
    for i in range(min(n, len(out))):
        out[i] *= 0.5 - 0.5 * math.cos(math.pi * i / n)
    return out


def fade_out(track, fade_seconds):
    """Raised-cosine tail-out, so truncating a still-decaying tail cannot tick."""
    n = max(1, seconds(fade_seconds))
    out = list(track)
    for i in range(min(n, len(out))):
        out[len(out) - n + i] *= 0.5 + 0.5 * math.cos(math.pi * i / n)
    return out


def soft_clip(track, drive):
    """tanh saturation. When every layer peaks in the same millisecond,
    rounding the overshoot reads as 'loud'; hard clipping reads as 'broken'."""
    limit = math.tanh(drive)
    return [math.tanh(s * drive) / limit for s in track]


# --- weapon fire -----------------------------------------------------------
# Four guns that must be told apart BLIND, in a fraction of a second, while
# other things are also making noise. What separates them is not "quality" but
# their position on three measurable axes -- spectral tilt, crest factor and
# duration -- and duration is not a free choice: a fire sound longer than
# roughly a third of the weapon's shot interval smears its own automatic fire
# into a drone. So the rate of fire in each .cfg sets the length budget, and
# the tactical meaning of the weapon sets the tilt.
#
#   weapon   rpm   shot interval   length   peak   the sound means
#   smg      900     66.7 ms        49 ms   0.82   close, fast, low per shot
#   rifle    600    100.0 ms       115 ms   0.80   mid range, trade or break off
#   shotgun   75    800.0 ms       240 ms   0.89   he is already on top of me
#   sniper    45   1333.3 ms       252 ms   0.95   get behind something, now
#
# Peaks are the cross-weapon loudness knob and are ordered by per-shot threat.
# Nothing reaches 1.0: the old shared fire.wav peaked at full scale with 24
# samples pinned to the rail, so any two simultaneous shots clipped the mix.
#
# Measured on the rendered files (FFT band shares, % of total energy). No two
# weapons are close on BOTH duration and spectrum, which is what makes the set
# work -- any one file heard alone is meaningless, the contrast is the product:
#
#   weapon    <400 Hz   centroid   crest    separated from its nearest twin by
#   smg          10%     3012 Hz   16.0 dB  5.1x duration (vs sniper)
#   rifle        46%     3296 Hz   17.0 dB  36 pts of low end (vs smg)
#   shotgun      75%     1661 Hz   19.7 dB  2.5x centroid (vs sniper)
#   sniper       15%     4150 Hz   20.8 dB  60 pts of low end (vs shotgun)
#
# The shotgun and sniper are near-identical in LENGTH (240 vs 252 ms) and are
# told apart purely by tilt; the smg and sniper are near-identical in TILT and
# are told apart purely by length. Changing one weapon's length or band without
# re-checking its twin is how this set quietly stops working.
#
# Every weapon seeds its OWN random.Random. The rest of this file threads one
# shared stream through the sounds in call order, which is fine when they are
# written once -- but editing one gun's recipe must not silently change
# another gun's bytes.


def rifle_fire():
    """600 rpm, 25 damage, 100 m. The reference sound: the one the other three
    are heard as deviations from. Mid-forward with almost no sub, and over
    before the next round at 100 ms so a burst reads as a countable 10 Hz grid
    rather than a rising noise bed."""
    rng = random.Random(0x8151)

    # 0-6 ms. Broadband above 1.8 kHz -- a crack, not a thud -- and the part
    # the positional panner has real detail to localise on. Rolled off at
    # 9 kHz because a rifle 30 m away has already lost its top octave to air,
    # and because unfiltered hiss ten times a second is fatiguing.
    crack = normalize(band_pass(noise_burst(0.010, 620.0, 1.0, rng), 1800.0, 9000.0), 0.85)
    # 0-45 ms. Midband pressure, parked between the crack's band and the
    # thump's so the three layers do not mask each other.
    blast = normalize(band_pass(noise_burst(0.045, 90.0, 1.0, rng), 400.0, 2600.0), 0.62)
    # 0-70 ms. The audible 300 -> 105 Hz glide is what makes this a gun with a
    # chamber rather than a click. Deliberately clear of the sub, which
    # belongs to the shotgun.
    thump = tone(0.070, 300.0, 105.0, decay=34.0, volume=0.26)
    # 2-32 ms. A short metallic pitch where the ear is most sensitive. Starts
    # 2 ms in so it colours the decay instead of blunting the transient.
    ring = delayed(normalize(tone(0.030, 1480.0, 1240.0, decay=120.0), 0.16), 0.002)
    # +28 ms. The signature: a second DISCRETE event inside one shot. It lands
    # where the blast has already gone, so it reads as mechanism rather than
    # as more blast, and gives sustained fire a "ta-tk" texture.
    bolt = delayed(mix(normalize(high_pass(noise_burst(0.006, 500.0, 1.0, rng), 2500.0), 0.30),
                       normalize(tone(0.006, 3100.0, 2700.0, decay=260.0), 0.135)), 0.028)
    # Dark, quiet room slap so the shot does not stop dead -- but down near 1%
    # of peak by 90 ms, because the 100 ms shot grid has to stay audible.
    tail = normalize(low_pass(noise_burst(0.115, 30.0, 1.0, rng), 900.0), 0.10)

    shot = mix(crack, blast, thump, ring, bolt, tail)[:seconds(0.115)]
    return normalize(fade_out(attack_ramp(soft_clip(shot, 1.5), 0.0003), 0.005), 0.80)


def smg_fire():
    """900 rpm, 14 damage, 60 m. Its identity is RATE, not weight. At 49 ms
    against a 66.7 ms shot interval consecutive rounds never overlap, so
    sustained fire is a countable 15 Hz rattle instead of a wall of noise --
    and it is thin on purpose, because bass is what sums into mud."""
    rng = random.Random(4242)

    # 0-4 ms. Essentially an impulse (43 dB down by 4 ms): the only full-
    # bandwidth layer, and the one the panner works from.
    strike = high_pass(attack_ramp(noise_burst(0.004, 950.0, 0.85, rng), 0.0002), 900.0)
    # 0-24 ms. The voice: a BROAD tilt at 2.9 kHz, not a whistle. A pitched
    # resonance repeated 15 times a second would fatigue in seconds.
    crack = resonant_noise(0.024, 2900.0, 1.4, 150.0, 5.6, rng)
    # 0-20 ms. Just enough low end not to sound like a cap gun, and no more:
    # this is the only layer that sums coherently across overlapping shots.
    body = tone(0.020, 200.0, 90.0, decay=130.0, volume=0.16)
    # 8-22 ms. Narrow and metallic, reading as the bolt cycling. 8 ms is
    # inside the ear's fusion window, so it thickens the transient rather than
    # detaching from it -- that is what makes the weapon chatter, not buzz,
    # and it is deliberately NOT the rifle's separate 28 ms tick.
    bolt = delayed(resonant_noise(0.014, 3800.0, 4.0, 260.0, 2.4, rng), 0.008)
    # 9-49 ms. Band-limited air, well under the crack: enough that the shot
    # is not a click in a vacuum, not enough to add hiss or mud.
    air = delayed(band_pass(noise_burst(0.040, 80.0, 0.42, rng), 450.0, 1800.0), 0.009)

    # Shave sub-content no laptop speaker reproduces and that only eats
    # headroom, then seal the tail to zero -- a truncated decay is a click,
    # and 15 clicks a second is exactly the fatigue this design avoids.
    shot = high_pass(mix(strike, crack, body, bolt, air), 150.0)
    return normalize(fade_out(shot, 0.010), 0.82)


def shotgun_fire():
    """75 rpm, 8 pellets, 40 m. A proximity alarm. The engine's only distance
    cue is a volume ramp, and volume alone cannot say 'close' -- so timbre
    must: this is the darkest and lowest sound of the four, with no supersonic
    crack at all, because pellets do not go supersonic and a sound with no
    highs to lose is heard as near and large."""
    rng = random.Random(0x5867)

    # Transient, band-limited on BOTH ends. The 6.5 kHz ceiling is the
    # deliberate absence of the rifle/sniper crack; that omission is the
    # fastest cue in the set, decided inside the first 5 ms. Then scattered
    # into 8 taps under the ~30 ms fusion threshold, so it is heard as one
    # gritty wide shot rather than as eight.
    slap = scatter(decay_envelope(band_pass(white_noise(0.03, rng), 1200.0, 6500.0), 620.0, 1.25),
                   8, 0.0042, rng)
    # The mass. The low-pass sweeps SHUT: broadband for an instant, then only
    # the low end survives. It also guarantees no static resonant peak, which
    # is what actually causes fatigue in a sound played thousands of times.
    body = decay_envelope(low_pass(white_noise(0.12, rng), 1100.0, 320.0), 34.0, 0.64)
    # The pitch identity, an octave under the rifle. `boom` is the part a
    # laptop speaker can still reproduce, so it carries the weight; `sub` is
    # the headphone reward and is deliberately faint, because energy below
    # 150 Hz is energy most players will never hear. Balanced by measurement,
    # not by taste: at the designer's original levels 68% of this weapon's
    # energy sat under 150 Hz, and running the mix through a 200 Hz rolloff
    # showed it dropping 8.8 dB -- from the loudest of the four to the
    # quietest -- the moment it left a subwoofer. Neither sweep ends below
    # 45 Hz; nothing can play that, so it is only headroom burnt.
    boom = sweep(0.15, 182.0, 68.0, decay=28.0, volume=0.27)
    sub = sweep(0.18, 92.0, 46.0, decay=15.0, volume=0.045)
    # The pump rack, inside the fire event because a pump gun racks after
    # every shot. It is the unmistakable blind tell -- nothing else in the
    # game emits a mechanical clack after a shot -- and it doubles as a clock:
    # it finishes at ~238 ms, and the next shot cannot come before 800 ms.
    # Both strokes live above 1.4 kHz, so they never join the low-end pile-up.
    # The forward (chambering) stroke is louder and brighter, as a real one is.
    back = mix(decay_envelope(band_pass(white_noise(0.02, rng), 1400.0, 5200.0), 300.0, 0.15),
               sweep(0.02, 2600.0, 2600.0, decay=260.0, volume=0.06))
    forward = mix(decay_envelope(band_pass(white_noise(0.02, rng), 1600.0, 6000.0), 260.0, 0.17),
                  sweep(0.02, 3900.0, 3900.0, decay=240.0, volume=0.07))

    blast = mix(slap, body, boom, sub, delayed(back, 0.162), delayed(forward, 0.218))
    n = seconds(0.240)
    blast = blast[:n] + [0.0] * max(0, n - len(blast))
    # High-passing before the normalise spends the headroom on content that
    # can actually be heard rather than on sub-audible rumble. The 0.3 ms ramp
    # is the same envelope hygiene the other three get: the scattered
    # transient starts at 0.59 of full scale on sample 0, and a step that size
    # is a driver artifact on cheap speakers rather than part of the shot.
    return normalize(attack_ramp(high_pass(blast, 35.0), 0.0003), 0.89)


def sniper_fire():
    """45 rpm, 75 damage, 200 m. The only weapon that threatens a player from
    outside the range they can see, so it must be identifiable at the moment
    of onset, from a direction, and QUIETLY: at 200 m the client's distance
    ramp has bottomed out, so loudness carries no information and timbre is
    the only channel left. Equal-loudness contours steepen at low frequency as
    level drops, which is why the identity is a bright 1-5 kHz crack and not
    weight. At 1.33 s between shots it is also the only gun that can afford a
    tail without smearing itself."""
    rng = random.Random(4501)

    # 0-10 ms. The razor edge the ear time-stamps and the panner localises on.
    # Doubled filters for 12 dB/oct: this one wants a hard-edged band.
    snap = normalize(band_pass(band_pass(noise_burst(0.010, 470.0, 1.0, rng), 2000.0, 9000.0),
                               2000.0, 9000.0), 0.55)
    # 0-38 ms. The bulk of the energy, put where hearing is most sensitive so
    # the shot is perceptually loud without spending peak headroom. The
    # 5.2 kHz ceiling is what keeps it a crack rather than a hiss.
    crack = normalize(band_pass(band_pass(noise_burst(0.038, 90.0, 1.0, rng), 1100.0, 5200.0),
                                1100.0, 5200.0), 0.80)
    # Weight, not boom. 75 damage is precision, not mass -- and the low end is
    # the shotgun's, so the body is gone in ~45 ms.
    body = tone(0.042, 300.0, 85.0, decay=70.0, volume=0.15)
    # Receiver ring: a genuinely pitched component in an otherwise all-noise
    # family. The slight downward glide makes it metal rather than a beep.
    ring = tone(0.055, 2680.0, 2390.0, decay=105.0, volume=0.10)
    # First reflection at 34 ms: past echo fusion for a click but well inside
    # the precedence window, so the listener still localises on the direct
    # wavefront and hears one event -- with open space around it.
    slap = delayed(normalize(band_pass(band_pass(noise_burst(0.016, 240.0, 1.0, rng), 900.0,
                                                 4200.0), 900.0, 4200.0), 0.15), 0.034)
    # The report rolling away. The gliding 2800 -> 560 Hz cutoff is the whole
    # trick: the sound does not merely get quieter, it gets duller, which is
    # what distance actually does to a gunshot. This says "large rifle, fired
    # in the open, a long way off" -- weapon class, which is the thing that
    # changes what the listener should do.
    tail = high_pass(noise_burst(0.240, 13.0, 0.5, rng), 600.0)
    tail = low_pass(low_pass(tail, 2800.0, 560.0), 2800.0, 560.0)
    tail = delayed(normalize(tail, 0.17), 0.012)

    # Ramped after the mix, not just over snap+crack: the pitched layers start
    # mid-cycle and would otherwise leave a step on sample 0 by themselves.
    shot = attack_ramp(mix(snap, crack, body, ring, slap, tail), 0.0004)
    # Saturation, for loudness rather than for colour. Peak-normalising alone
    # left this the quietest of the four in short-window RMS -- the loudest
    # single sample but the least sound -- which is backwards for the weapon
    # that does 75 damage and whose whole job is to make a player take cover.
    # Rounding the isolated noise spikes buys ~4 dB of level under the same
    # 0.95 ceiling and costs nothing audible; the crack stays a crack.
    return fade_out(normalize(soft_clip(shot, 1.9), 0.95), 0.020)


# --- footsteps -------------------------------------------------------------
# The most-played sound in the game by a wide margin: one moving player emits
# ~2.5 a second, eight of them emit twenty. Everything below follows from that
# number rather than from what a single footstep "should" sound like.
#
#  - Under 100 ms, so consecutive steps never overlap even at the sprint
#    cadence (a step every 258 ms at 9.3 m/s).
#  - No pitched resonator anywhere. A narrow peak heard twenty times a second
#    stops being a texture and becomes a tone, and a tone that never resolves
#    is the definition of listening fatigue. Everything here is either noise
#    or a glide that never sits still.
#  - Peak 0.55, well under the weapons (0.80-0.95). A footstep must be
#    information you can hear THROUGH gunfire, not something that competes
#    with it; the client's own gain ramp does the rest.
#  - Sealed at both ends (attack_ramp/fade_out). A truncated 90 ms tail is a
#    click, and twenty clicks a second is all anyone would hear.
#
# The spectral balance is the part that is easy to get wrong, and it was:
# built by ear-free intuition the heel dominated so completely that 96% of a
# step's energy sat under 400 Hz. That is not merely dull, it is USELESS --
# below ~500 Hz a wavelength is longer than a head, interaural level
# difference collapses, and the listener cannot tell which side the sound came
# from. A footstep nobody can point at is not worth playing. So each layer is
# peak-normalised to a stated level (the volume arguments of a sine and of
# band-passed noise are not comparable: one-pole filters throw most of the
# noise away, so equal "volume" is nowhere near equal loudness), and the
# finished step is measured, not assumed. Target: roughly half the energy
# under 400 Hz for weight, a quarter above 2 kHz for direction.
#
# The four variants are one pair of boots on one floor, not four different
# events, so they share the recipe and differ only where two real consecutive
# steps differ: the heel pitch (a few percent), how much of the sole caught,
# and the balance between the two. The client also jitters level and playback
# pitch per step, so the audible period is far longer than four.
#
# Measured on the rendered files (FFT band shares, % of total energy). The
# four steps are a tight cluster and the landing is nowhere near it -- that
# separation is the point: a landing must never be mistaken for a step, and it
# is told apart by TILT (1.8 kHz centroid against ~4.5 kHz) rather than by
# level, because level is what distance already spends.
#
#   sound     length   peak   <400 Hz   >2 kHz   centroid
#   step1      65 ms   0.55     46%       44%     4518 Hz
#   step2      65 ms   0.55     58%       37%     3783 Hz
#   step3      65 ms   0.55     48%       42%     5096 Hz
#   step4      65 ms   0.55     51%       43%     4770 Hz
#   land      200 ms   0.78     75%       18%     1800 Hz
STEP_VARIANTS = [
    # seed,   heel Hz, scuff band,      heel, scuff
    (0x57A1, 132.0, (2100.0, 7000.0), 0.38, 0.85),
    (0x57A2, 121.0, (1900.0, 6400.0), 0.42, 0.80),
    (0x57A3, 142.0, (2500.0, 7600.0), 0.35, 0.90),
    (0x57A4, 126.0, (2000.0, 6800.0), 0.40, 0.82),
]


def footstep(seed, heel_hz, scuff_band, heel_level, scuff_level):
    """One footfall on concrete: a heel, a sole, and the room noticing."""
    rng = random.Random(seed)

    # 0-40 ms. The heel. A falling glide rather than a fixed pitch, because a
    # boot on concrete is a struck plate whose contact patch grows as it
    # flattens -- and because a fixed pitch is exactly the thing that would
    # ring after the twentieth repeat.
    heel = sweep(0.040, heel_hz, heel_hz * 0.58, decay=58.0, volume=heel_level)
    # 0-20 ms. The sole. Band-limited noise well above the heel, so the two
    # layers occupy different bands and the ear hears one event with weight
    # AND edge instead of two stacked sounds. This is also the layer with the
    # high-frequency detail the panner localises on, which is the entire
    # reason a footstep is worth hearing -- hence the peak ABOVE the heel's.
    scuff = normalize(decay_envelope(band_pass(white_noise(0.020, rng), scuff_band[0],
                                               scuff_band[1]), 200.0), scuff_level)
    # 3-28 ms. Grit under the boot, bridging the gap between heel and scuff so
    # the step is not two disconnected bands. Delayed a few ms so it colours
    # the decay rather than blunting the transient.
    grit = delayed(normalize(decay_envelope(band_pass(white_noise(0.025, rng), 700.0, 2600.0),
                                            150.0), 0.34), 0.003)
    # A dark, very quiet room slap. Without it a step is a click in a vacuum;
    # any more of it and eight players moving turn the arena into a swamp.
    tail = delayed(normalize(low_pass(noise_burst(0.055, 42.0, 1.0, rng), 1200.0), 0.05), 0.010)

    step = mix(heel, scuff, grit, tail)[:seconds(0.095)]
    return normalize(fade_out(attack_ramp(step, 0.0004), 0.012), 0.55)


def landing():
    """Arriving from a jump or a drop. Deliberately not "a louder footstep":
    it is a whole body stopping, so it is an octave lower, three times as long
    and carries gear rattle -- and it is a discrete event rather than one of a
    stream, which is what buys it the length and the 0.78 peak."""
    rng = random.Random(0x1A2D)

    # 0-200 ms. The mass. An octave under a footstep's heel and five times as
    # long; this is what says "person", not "pebble".
    thud = sweep(0.200, 96.0, 44.0, decay=17.0, volume=0.55)
    # The floor answering. Sweeping the cutoff shut turns a broadband impact
    # into low-end residue over 150 ms, which is what a real room does and
    # what no static filter can imitate.
    body = normalize(decay_envelope(low_pass(white_noise(0.150, rng), 900.0, 260.0), 26.0), 0.45)
    # Both boots, 12 ms apart. Nobody lands in unison, and 12 ms is inside the
    # fusion window, so it widens the transient instead of reading as two.
    slap = mix(normalize(decay_envelope(band_pass(white_noise(0.030, rng), 1400.0, 6500.0), 210.0),
                         0.80),
               delayed(normalize(decay_envelope(band_pass(white_noise(0.030, rng), 1200.0, 5600.0),
                                                230.0), 0.55), 0.012))
    # 40-140 ms. Kit settling after the impact. It is the cue that separates a
    # landing from a footstep at a distance, where the falloff curve and the
    # air have eaten the low end and only the top of the sound survives.
    rattle = delayed(normalize(decay_envelope(band_pass(white_noise(0.100, rng), 2400.0, 9000.0),
                                              34.0), 0.22), 0.040)

    out = mix(thud, body, slap, rattle)[:seconds(0.210)]
    # High-passed before the normalise for the same reason the shotgun is:
    # sub-40 Hz content nothing can reproduce is pure burnt headroom.
    return normalize(fade_out(attack_ramp(high_pass(out, 35.0), 0.0004), 0.025), 0.78)


def main():
    root = Path(__file__).resolve().parent.parent / "assets" / "sounds"
    rng = random.Random(42)

    # Per-weapon fire. Each owns its RNG seed, so re-cutting one gun cannot
    # change another gun's bytes.
    write_wav(root / "fire_rifle.wav", rifle_fire())
    write_wav(root / "fire_smg.wav", smg_fire())
    write_wav(root / "fire_shotgun.wav", shotgun_fire())
    write_wav(root / "fire_sniper.wav", sniper_fire())

    # Generic shot, kept as the fallback for a weapon whose config names no
    # sound of its own (the knife, today) and for an out-of-range slot in a
    # fire event. Peak-normalised because the hand-balanced version clipped:
    # its layers summed past 1.0 and write_wav silently clamped 24 samples
    # flat against the rail. Same length, so the file size does not move.
    write_wav(root / "fire.wav",
              normalize(mix(noise_burst(0.09, 55.0, 0.9, rng),
                            tone(0.12, 140, 60, decay=25, volume=0.7)), 0.85))

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

    # Footsteps and the landing that ends a jump. Each owns its RNG seed for
    # the same reason the weapons do: re-cutting one variant must not shift
    # the other three, or a one-line tweak becomes a five-file binary diff.
    for index, (seed, heel_hz, band, heel_level, scuff_level) in enumerate(STEP_VARIANTS, start=1):
        write_wav(root / f"step{index}.wav",
                  footstep(seed, heel_hz, band, heel_level, scuff_level))
    write_wav(root / "land.wav", landing())


if __name__ == "__main__":
    main()
