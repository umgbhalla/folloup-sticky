#!/usr/bin/env python3
"""Render a small Cuelume-derived preview set as 16 kHz mono PCM WAV."""

import math
import random
import subprocess
import wave
from pathlib import Path

RATE = 16_000
REPO = Path(__file__).resolve().parents[1]
WAV_OUT = REPO / "emulator/web/audio"
MP3_OUT = REPO / "components/system_sound_service/sounds"
CUE_NAMES = {
    "navigation_move": "select",
    "button_activate": "tap",
    "complete": "complete",
    "interrupt": "interrupt",
    "online": "online",
    "speaking": "speaking",
    "startup": "startup",
}

# Parameters are copied from Cuelume v0.2.2 recipes. Noise layers use a simple
# deterministic filtered approximation because this is a local preview export.
RECIPES = {
    # Cuelume's short, crisp tick fits repeated rocker navigation.
    "navigation_move": (0.4, [("noise", 5400, 0.0, 0.001, 0.018, 0.14), ("tone", 2600, 0.0, 0.001, 0.012, 0.018)], 0.0),
    "button_activate": (0.42, [("noise", 1700, 0.0, 0.001, 0.02, 0.13)], 0.0),
    "complete": (0.5, [("tone", 880, 0.0, 0.004, 0.09, 0.06), ("tone", 1108.73, 0.06, 0.004, 0.1, 0.06), ("tone", 1318.51, 0.12, 0.004, 0.18, 0.07)], 0.1),
    "interrupt": (0.42, [("noise", 850, 0.0, 0.001, 0.035, 0.13), ("triangle", 440, 0.025, 0.004, 0.09, 0.045), ("triangle", 349.23, 0.1, 0.004, 0.14, 0.04)], 0.0),
    "online": (0.48, [("noise", 3600, 0.0, 0.001, 0.02, 0.11), ("triangle_glide", 330, 0.012, 0.004, 0.16, 0.055, 660), ("tone", 990, 0.13, 0.004, 0.22, 0.06)], 0.1),
    "speaking": (0.42, [("tone_glide", 420, 0.0, 0.025, 0.18, 0.05, 630), ("noise", 1400, 0.0, 0.035, 0.14, 0.035)], 0.11),
    "startup": (0.44, [("tone_glide", 220, 0.0, 0.04, 0.34, 0.055, 440), ("tone", 659.25, 0.12, 0.045, 0.32, 0.04), ("tone", 987.77, 0.19, 0.045, 0.34, 0.032)], 0.16),
}


def env(t, start, attack, decay, peak):
    if t < start or t > start + attack + decay:
        return 0.0
    if t <= start + attack:
        return peak * max(0.0001, (t - start) / max(attack, 1e-9))
    return peak * max(0.0001, 1 - (t - start - attack) / max(decay, 1e-9))


def render(name, master, layers, shimmer):
    end = max(layer[2] + layer[3] + layer[4] for layer in layers) + (0.55 if shimmer else 0.05)
    samples = [0.0] * math.ceil(end * RATE)
    rng = random.Random(name)
    for layer in layers:
        kind, freq, offset, attack, decay, peak, *rest = layer
        glide_to = rest[0] if rest else None
        noise = [rng.uniform(-1, 1) for _ in samples] if kind == "noise" else None
        for i in range(len(samples)):
            t = i / RATE
            a = env(t, offset, attack, decay, peak)
            if not a:
                continue
            local = max(0.0, t - offset)
            f = freq + (glide_to - freq) * min(1.0, local / max(attack + decay, 1e-9)) if glide_to else freq
            if noise is not None:
                # Moving average is enough for a preview of the filtered noise layer.
                value = sum(noise[max(0, i - 3):i + 1]) / len(noise[max(0, i - 3):i + 1])
            elif kind.startswith("triangle"):
                value = 2 * abs(2 * ((local * f) % 1.0) - 1) - 1
            else:
                value = math.sin(2 * math.pi * f * local)
            samples[i] += value * a
    samples = [max(-1.0, min(1.0, x * master)) for x in samples]
    if shimmer:
        delay = round(shimmer * RATE)
        for i in range(delay, len(samples)):
            samples[i] = max(-1.0, min(1.0, samples[i] + samples[i - delay] * 0.12))
    path = WAV_OUT / f"{CUE_NAMES[name]}.wav"
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(b"".join(int(x * 32767).to_bytes(2, "little", signed=True) for x in samples))
    subprocess.run(
        ["ffmpeg", "-loglevel", "error", "-y", "-i", str(path), "-codec:a", "libmp3lame",
         "-ar", str(RATE), "-ac", "1", "-b:a", "32k", str(MP3_OUT / f"{CUE_NAMES[name]}.mp3")],
        check=True,
    )


WAV_OUT.mkdir(parents=True, exist_ok=True)
MP3_OUT.mkdir(parents=True, exist_ok=True)
for cue, (master, layers, shimmer) in RECIPES.items():
    render(cue, master, layers, shimmer)
