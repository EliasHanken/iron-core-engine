"""Builds Kindle's professional sound set from a local Sonniss GDC library.

    python build_audio_sonniss.py <sonniss-kit folder> <games/19-kindle/assets/audio> <games/19-kindle/assets/audio-hq>

The Sonniss licence lets a finished game ship these sounds, but not give them away as
sound effects. This repo is public, so the output folder is ignored by git, and the
sounds only reach people inside a packaged game. Without this folder the game falls
back to the CC0 set in assets/audio. Needs ffmpeg and numpy.
"""
import csv
import os
import shutil
import subprocess
import sys

import numpy as np

RATE = 48000


# name, channels, target level, crossfade, then layers of (file, start, length, gain).
BEDS = [
    ("room", 2, -22.0, 3.0, [("SPACESHIP-INTERIORS-LOW-53", 20.0, 44.0, 1.0),
                             ("Industrial_Ambient_Drones_02", 10.0, 44.0, 0.5)]),
    ("chamber", 2, -21.0, 3.0, [("Huge Planet, Rumble", 40.0, 44.0, 1.0)]),
    ("awake", 2, -20.0, 2.5, [("Distant - Machine Hall", 2.0, 22.0, 1.0),
                              ("Medium Distance - Machine Hall", 2.0, 22.0, 0.5)]),
    ("roar", 1, -19.0, 2.0, [("Firedrone 3", 0.6, 10.2, 1.0)]),
    ("drive", 1, -21.0, 3.0, [("spacecraft engine 2", 10.0, 34.0, 1.0)]),
    ("rush", 1, -21.0, 2.5, [("SPDR - Venus", 30.0, 26.0, 1.0)]),

    # The score. These are recorded tonal drones, not composed music. It sits on E
    # while the machine is cold, and resolves to A major as it wakes. Measured: Swells
    # holds C# and A, Solemn holds A and E, and the fifth value below pitches a layer
    # in semitones to fit: Breathy's B, G and D rise to C#, A and E, and Mystery's F
    # falls to E.
    ("score-cold", 2, -24.0, 5.0, [("dark_space_017", 20.0, 58.0, 1.0),
                                   ("Drone_Atmosphere_Mystery_06", 20.0, 58.0, 0.8, -1)]),
    ("score-rise", 2, -23.0, 5.0, [("Drone,Musical,Resonant,Swells", 10.0, 58.0, 1.0),
                                   ("Drone,Resonant,Bed,Solemn", 2.0, 58.0, 0.7)]),
    ("score-awake", 2, -22.0, 5.0, [("Drone,Musical,Airy,Breathy", 4.0, 58.0, 1.0, 2),
                                    ("Drone,Musical,Resonant,Swells", 50.0, 58.0, 0.6)]),
]

# name, channels, then layers of (file, start, length, gain). Layers start together.
SHOTS = [
    ("ignite", 2, [("Impact_Boom_Distorted", 0.0, 5.5, 1.0), ("Designed Fire - Impacts", 1.9, 3.0, 0.6)]),
    ("finale", 2, [("Explosion 8.wav", 0.0, 12.0, 1.0), ("PM_INT_HnT_18", 2.7, 9.0, 0.8),
                   ("6 Big Metal Hit", 0.0, 12.0, 0.7)]),
    ("spool", 2, [("PodRacer_Engine_Start", 8.5, 4.2, 1.0)]),  # Peaks as the core blows.
    ("wake", 2, [("heavy_metal_impact_factory_door", 0.0, 5.0, 1.0)]),
    ("gate", 2, [("heavy_metal_impact_large_tank", 0.0, 5.0, 0.8), ("steel frame dragged", 10.0, 11.0, 1.0)]),
    ("groan", 2, [("heavy sheet metal low groan", 0.0, 7.7, 1.0)]),
    ("creak1", 1, [("submarine deformation", 0.0, 10.0, 1.0)]),
    ("creak2", 1, [("ship scraping", 3.0, 9.0, 1.0)]),
    ("creak3", 1, [("Metal_Creak_13", 0.0, 5.4, 1.0)]),
    ("chain", 1, [("UCnS_BlockedDoor", 0.0, 5.9, 1.0)]),
    ("bend", 1, [("Large Metal Air Drum", 0.0, 5.0, 1.0)]),
    ("whoosh1", 2, [("WHOOSH_15_space_vehicle", 0.0, 4.0, 1.0)]),
    ("whoosh2", 2, [("metal_whoosh_021", 0.0, 3.4, 1.0)]),
    ("whoosh3", 2, [("metal_whoosh_037", 0.0, 4.3, 1.0)]),
    ("whoosh4", 2, [("Power Whoosh Large", 0.0, 2.7, 1.0)]),
    ("ui-move", 1, [("Metal_Ringout_MKH8060_14", 0.0, 0.5, 1.0)]),
    ("ui-confirm", 1, [("Metal_Ringout_DPA4060_10", 0.0, 2.4, 1.0)]),
]


# Low and high cuts, from Ashe's method: each sound keeps only the band it's for, so
# that rumble and hiss don't pile up when many play together. The default suits a hit.
BANDS = {
    "ui-move": (500, 12000), "ui-confirm": (300, 12000),
    "whoosh1": (60, 14000), "whoosh2": (60, 14000), "whoosh3": (60, 14000), "whoosh4": (60, 14000),
    "creak1": (50, 9000), "creak2": (50, 9000), "creak3": (80, 9000), "chain": (120, 10000), "bend": (120, 10000),
    "room": (25, 9000), "chamber": (22, 7000), "awake": (28, 10000), "roar": (35, 11000), "drive": (28, 8000),
    "rush": (120, 12000), "score-cold": (24, 9000), "score-rise": (28, 11000), "score-awake": (28, 13000),
}
DEFAULT_BAND = (24, 16000)


def band_limit(x, name):
    low, high = BANDS.get(name, DEFAULT_BAND)
    channels = x.shape[1]
    done = subprocess.run(
        ["ffmpeg", "-v", "error", "-f", "f32le", "-ac", str(channels), "-ar", str(RATE), "-i", "-",
         "-af", f"highpass=f={low},lowpass=f={high}", "-f", "f32le", "-"],
        input=x.astype(np.float32).tobytes(), capture_output=True, check=True).stdout
    return np.frombuffer(done, dtype=np.float32).reshape(-1, channels).copy()


def strongest_note(x):
    mono = x.mean(axis=1)[: RATE * 30]
    spectrum = np.abs(np.fft.rfft(mono * np.hanning(mono.size))) ** 2
    freqs = np.fft.rfftfreq(mono.size, 1 / RATE)
    band = (freqs > 55) & (freqs < 900)
    hz = freqs[band][np.argmax(spectrum[band])]
    return ["A", "A#", "B", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#"][int(round(12 * np.log2(hz / 440))) % 12], hz


def read(path, start, length, channels, semitones=0):
    # Pitching by resampling also changes speed, so a raised layer reads further.
    ratio = 2 ** (semitones / 12)
    pcm = subprocess.run(
        ["ffmpeg", "-v", "error", "-ss", str(start), "-t", str(length * ratio), "-i", path,
         "-af", f"aresample={RATE},asetrate={RATE * ratio},aresample={RATE}", "-ac", str(channels),
         "-ar", str(RATE), "-f", "f32le", "-"], capture_output=True, check=True).stdout
    return np.frombuffer(pcm, dtype=np.float32).reshape(-1, channels).copy()


def write(path, x):
    x16 = (np.clip(x, -1, 1) * 32767).astype(np.int16)
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "s16le", "-ac", str(x.shape[1]), "-ar", str(RATE), "-i", "-",
                    "-c:a", "pcm_s16le", path], input=x16.tobytes(), check=True)


def level(x):
    return 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-9)


def mix(find, layers, channels):
    parts = []
    for query, start, length, gain, *pitch in layers:
        x = read(find(query), start, length, channels, *pitch)
        # Each layer leaves as it would ring out, so a short one doesn't end on a cliff.
        fade = int(min(x.shape[0] * 0.35, RATE * 1.2))
        if x.shape[0] < RATE * 6:
            x[-fade:] *= np.linspace(1, 0, fade)[:, None] ** 2
        parts.append(x * (10 ** ((-20 - level(x)) / 20)) * gain)  # Matched first, then balanced.
    out = np.zeros((max(p.shape[0] for p in parts), channels), dtype=np.float32)
    for p in parts:
        out[: p.shape[0]] += p
    return out


def seamless(x, fade_seconds):
    n = int(fade_seconds * RATE)
    t = np.linspace(0, 1, n)[:, None]
    out = x[:-n].copy()
    out[:n] = x[:n] * np.sqrt(t) + x[-n:] * np.sqrt(1 - t)
    return out


def finish_shot(x):
    win = RATE // 50
    frames = x.shape[0] // win
    env = np.array([np.sqrt(np.mean(x[i * win:(i + 1) * win] ** 2)) for i in range(frames)])
    loud = np.nonzero(env > env.max() * 10 ** (-48 / 20))[0]
    end = min((int(loud[-1]) + 1) * win, x.shape[0])
    x = x[:end].copy()
    fade = min(int(0.4 * RATE), end // 3)
    x[-fade:] *= np.linspace(1, 0, fade)[:, None] ** 2
    start = min(int(0.003 * RATE), end // 4)
    x[:start] *= np.linspace(0, 1, start)[:, None]
    return x * (0.79 / (np.abs(x).max() + 1e-9))


def main():
    kit, cc0, out = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(kit, "sounds-index.tsv"), encoding="utf-8", errors="replace") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))

    used = []

    def find(query):
        for row in rows:
            if query.lower() in row["file"].lower():
                used.append(f"{row['library']} / {row['file']}")
                return os.path.join(kit, row["path"])
        raise SystemExit(f"not found: {query}")

    for name, channels, target, fade, layers in BEDS:
        x = seamless(band_limit(mix(find, layers, channels), name), fade)
        x *= 10 ** ((target - level(x)) / 20)
        peak = np.abs(x).max()
        if peak > 0.89:
            x *= 0.89 / peak
        write(os.path.join(out, name + ".wav"), x)
        jump = np.abs(x[0] - x[-1]).max() / (np.percentile(np.abs(np.diff(x, axis=0)).max(axis=1), 99.9) + 1e-9)
        note, hz = strongest_note(x)
        print(f"{name:11} bed  {x.shape[0] / RATE:5.1f}s {channels}ch level {level(x):6.1f} "
              f"peak {20 * np.log10(np.abs(x).max()):6.1f} seam {jump:5.2f} note {note} {hz:.0f} Hz")
    for name, channels, layers in SHOTS:
        x = finish_shot(band_limit(mix(find, layers, channels), name))
        write(os.path.join(out, name + ".wav"), x)
        print(f"{name:10} shot {x.shape[0] / RATE:5.1f}s {channels}ch level {level(x):6.1f}")

    # No fire crackle in the library beats the one we have, so it carries over.
    shutil.copy(os.path.join(cc0, "crackle.wav"), os.path.join(out, "crackle.wav"))
    with open(os.path.join(out, "SOURCES.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(sorted(set(used))) + "\n")


if __name__ == "__main__":
    main()
