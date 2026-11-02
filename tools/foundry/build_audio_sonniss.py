"""Builds Foundry's sound set from a local Sonniss GDC library.

    python -I build_audio_sonniss.py <sonniss-kit folder> <games/20-foundry/assets/audio-hq>

Every sound is cut from real recordings: no synthesis. The Sonniss licence lets a
finished game ship these sounds, but not give them away as sound effects. This repo is
public, so the output folder is ignored by git, and the sounds only reach people
inside a packaged game, scrambled into audio.pak. Without the folder the game falls
back to the CC0 shooter set. Needs ffmpeg and numpy.

The rifle follows Ashe's method: the shot is layered from real 5.56 mm recordings
(the crack from 1 m, the action from a mic on the gun, the body from a second rifle),
and the room isn't baked in. Each space has its own recorded tail instead, a real gun
heard in a large warehouse or across an open field, played when a burst ends.
"""
import csv
import os
import subprocess
import sys

import numpy as np

RATE = 48000

# --- Recipes ------------------------------------------------------------------------

# The rifle is built the way AAA weapons are: layers recorded apart, each kept to its
# own job and lined up on the shot's first transient.
#   base   the shove below 150 Hz: a .338 Lapua's on-gun mic, and a synthesized sub;
#   punch  the body, 90 Hz-3.5 kHz: a .30-06 BAR 10 cm from the muzzle, a full-power
#          rifle round like the AK's 7.62, with the .338's mids for length;
#   high   the crack, kept bright but short: an L85 a metre off axis, shaped to a
#          snap, with a narrow notch where it bites instead of a lowpass;
#   mech   the action, from the L85's on-gun mic;
#   tail   the space, per shot (tailshot-*) and longer when a burst ends (tail-*).
L85_1M = "Enfield_L85_rifle_5.56mm_1m_left_off_axis"
L85_GUN = "Enfield_L85_rifle_5.56mm_on_gun"
BAR_CLOSE = "M1918_Browning_Automatic_Rifle_.30cal_0.1m"
BARRETT_GUN = "Barrett_M98B"

# Five variants, so that automatic fire never repeats one sample. Measured onsets:
# the L85 shots sit about 0.23 s apart; the BAR fires two, 0.105 s apart, so its
# takes end before the next and alternate, pitched a little apart; the Barrett's are
# single shots seconds apart.
SHOT_VARIANTS = 5
CRACK_TAKES = [0.023, 0.266, 0.518, 0.741, 0.963]
MECH_TAKES = [0.013, 0.256, 0.508, 0.733, 0.955]
PUNCH_TAKES = [(0.408, 0.0), (0.513, 0.0), (0.408, -0.6), (0.513, 0.5), (0.408, 0.4)]
BASE_TAKES = [0.046, 3.049, 7.044, 15.043, 23.042]
TAKE_LENGTH = 0.24

# A short tail with every shot, from rifles in the two spaces: a long corridor and an
# open field (onsets measured); the direct sound is skipped, the reflections kept.
SHOT_TAILS = [("tailshot-hall", "AK4_long_corridor", [0.452, 7.713, 31.453]),
              ("tailshot-yard", "AK47_big_open_area", [0.835, 9.555, 13.860])]

# One-shots: name, channels, then layers of (file, start, length, gain[, semitones]).
ONE_SHOTS = [
    # A real rifle in a large warehouse, and one across an open field: the reflections
    # only, starting just after the direct sound.
    ("tail-hall", 2, [("Large_Warehouse_Colt_M1911", 6.885 + 0.09, 2.8, 1.0),
                      ("Large_Warehouse_Colt_M1911", 15.710 + 0.09, 2.8, 0.6)]),
    ("tail-yard", 2, [("AK47_big_open_area", 0.835 + 0.07, 1.7, 1.0)]),

    # Handling, from a G36C and a Steyr TMP recorded close.
    ("mag-out", 1, [("G36C_5.56mm_foley", 4.10, 0.45, 1.0)]),
    ("mag-in", 1, [("G36C_5.56mm_foley", 6.30, 0.55, 1.0)]),
    ("charge", 1, [("Steyr_TMP_9mm_foley", 4.10, 0.65, 1.0)]),
    ("dry", 1, [("Steyr_TMP_9mm_foley", 0.48, 0.25, 1.0)]),

    # Brass: a .50 casing on dirt, pitched up towards a 5.56 one, and a shell on concrete.
    ("casing1", 1, [("Casing 50BMG Dirt", 0.98, 0.45, 1.0, 5)]),
    ("casing2", 1, [("Casing 50BMG Dirt", 15.98, 0.55, 1.0, 6)]),
    ("casing3", 1, [("ShotgunShell_Land_Concrete", 0.0, 0.88, 1.0, 3)]),

    # What a round hits.
    ("hit-metal1", 1, [("bullet_impact_metal_heavy", 0.0, 0.82, 1.0)]),
    ("hit-metal2", 1, [("Metal,Hits,Pipe,Mixed", 0.0, 0.7, 1.0)]),
    ("hit-concrete1", 1, [("bullet_impact_concrete_brick", 0.0, 0.35, 1.0)]),
    ("hit-concrete2", 1, [("Bullet Impact 22", 0.0, 0.9, 1.0)]),
    ("hit-concrete3", 1, [("Concrete_Impact_B02", 0.0, 0.7, 0.9)]),
    ("hit-wood1", 1, [("Impacts Hard - Short Wobbly", 0.0, 0.63, 1.0)]),
    ("hit-wood2", 1, [("Impacts Soft - Short, Crack", 0.0, 0.45, 1.0)]),
    ("ricochet", 1, [("bullet_flyby_fast_05", 0.0, 0.51, 1.0)]),

    # Steel targets ringing.
    ("plate1", 1, [("Metal_Plate_Hit_02", 0.0, 2.5, 1.0)]),
    ("plate2", 1, [("metal_plate_medium", 0.0, 2.6, 1.0)]),
    ("plate3", 1, [("Plate_Impact_Hard_02", 0.0, 0.76, 1.0)]),

    # Footsteps: dirt in the yard, concrete in the hall, steel on the catwalk.
    ("step-ground1", 1, [("FS Ground Civilian Walk N03", 0.0, 0.7, 1.0)]),
    ("step-ground2", 1, [("FS Ground Civilian Walk N05", 0.0, 0.7, 1.0)]),
    ("step-concrete1", 1, [("FS Concrete Soldier Crouch N02", 0.0, 0.8, 1.0)]),
    ("step-concrete2", 1, [("FS Concrete Soldier Crouch N05", 0.0, 0.8, 1.0)]),
    ("step-metal1", 1, [("FS Metal Soldier Walk N01", 0.0, 0.7, 1.0)]),
    ("step-metal2", 1, [("FS Metal Soldier Walk N05", 0.0, 0.7, 1.0)]),

    # A grenade: the crack and blast of a large charge, with a mine's longer rolling
    # body under it from after its warning beep; then what it throws about landing.
    ("blast", 2, [("explosion_large_07", 0.0, 2.6, 1.0),
                  ("beep-mine-explosion-002", 0.52, 5.4, 0.55)]),
    ("debris", 2, [("Debris,Stones,Soil", 0.05, 1.0, 1.0),
                   ("Pile of Scrap Metal", 0.45, 1.8, 0.7)]),
    ("bounce", 1, [("Rock Hit and Bounce Dirt_RBDII 06", 0.0, 0.5, 1.0, 4)]),

    # The works, now and then somewhere in the hall: a steam blast, a power hammer,
    # a compressor cycling.
    ("works1", 1, [("Steam Blast,Tube or Pipe,Distant", 0.0, 4.0, 1.0)]),
    ("works2", 1, [("Power hammer,Drops,Impacts", 2.0, 6.0, 1.0)]),
    ("works3", 1, [("Air Compressor_Stress Adjustment", 0.0, 6.0, 1.0)]),
]

# Loops: name, channels, target level, crossfade, then layers.
BEDS = [
    # A cold winter morning at a working steelworks: wind through the yard, crows,
    # cranes loading at the docks beyond; inside, the line running and the air
    # handling; at the furnace, hot gas venting.
    ("bed-yard", 2, -24.0, 3.0, [("Wind,Ext,Alley,Urban,Howl", 4.0, 58.0, 1.0),
                                 ("Cranes, Chains, Loading, Iron", 20.0, 58.0, 0.5),
                                 ("WINTER Forest morning crow calls", 2.0, 58.0, 0.45)]),
    ("bed-hall", 2, -26.0, 3.0, [("Factory drone,Conveyor belt,Clack", 30.0, 42.0, 1.0),
                                 ("Hvac,Ventilation,Exhaust,Industrial", 4.0, 42.0, 0.5)]),
    ("furnace", 1, -20.0, 2.0, [("Steam release hot with medium pressure", 1.0, 18.0, 1.0)]),
]

# Low and high cuts, from Ashe's method: each sound keeps only the band it's for.
BANDS = {
    "tail-hall": (90, 9000), "tail-yard": (90, 8000),
    "mag-out": (150, 14000), "mag-in": (120, 14000), "charge": (120, 14000), "dry": (300, 14000),
    "casing1": (500, 16000), "casing2": (500, 16000), "casing3": (400, 16000),
    "ricochet": (300, 14000),
    "plate1": (120, 14000), "plate2": (120, 14000), "plate3": (150, 14000),
    "step-ground1": (60, 12000), "step-ground2": (60, 12000),
    "step-concrete1": (60, 12000), "step-concrete2": (60, 12000),
    "step-metal1": (80, 14000), "step-metal2": (80, 14000),
    "blast": (25, 14000), "debris": (80, 14000), "bounce": (250, 14000),
    "works1": (60, 12000), "works2": (40, 10000), "works3": (50, 12000),
    "bed-yard": (30, 11000), "bed-hall": (25, 9000), "furnace": (35, 10000),
}
DEFAULT_BAND = (40, 16000)


# --- Audio helpers ---------------------------------------------------------------------

def run_ffmpeg(args, data=None):
    return subprocess.run(["ffmpeg", "-v", "error", *args], input=data, capture_output=True, check=True).stdout


def read(path, start, length, channels, semitones=0):
    # Pitching by resampling also changes speed, so a raised layer reads further.
    ratio = 2 ** (semitones / 12)
    pcm = run_ffmpeg(["-ss", str(max(start, 0.0)), "-t", str(length * ratio), "-i", path,
                      "-af", f"aresample={RATE},asetrate={RATE * ratio},aresample={RATE}",
                      "-ac", str(channels), "-ar", str(RATE), "-f", "f32le", "-"])
    return np.frombuffer(pcm, dtype=np.float32).reshape(-1, channels).copy()


def filtered(x, low, high):
    channels = x.shape[1]
    out = run_ffmpeg(["-f", "f32le", "-ac", str(channels), "-ar", str(RATE), "-i", "-",
                      "-af", f"highpass=f={low},lowpass=f={high}", "-f", "f32le", "-"],
                     x.astype(np.float32).tobytes())
    return np.frombuffer(out, dtype=np.float32).reshape(-1, channels).copy()


def eq(x, chain):
    """Runs an ffmpeg audio filter chain over `x`."""
    channels = x.shape[1]
    out = run_ffmpeg(["-f", "f32le", "-ac", str(channels), "-ar", str(RATE), "-i", "-", "-af", chain,
                      "-f", "f32le", "-"], x.astype(np.float32).tobytes())
    return np.frombuffer(out, dtype=np.float32).reshape(-1, channels).copy()


def thump(length, channels):
    """A sub-bass kick: a sine falling from 68 to 42 Hz, gone in about 150 ms.
    It's the weight a rifle has at the shoulder that a microphone barely takes."""
    t = np.arange(int(length * RATE)) / RATE
    freq = 42.0 + 26.0 * np.exp(-t / 0.03)
    phase = 2.0 * np.pi * np.cumsum(freq) / RATE
    x = np.sin(phase) * np.exp(-t / 0.055) * np.minimum(t / 0.002, 1.0)
    return np.repeat(x[:, None], channels, axis=1).astype(np.float32)


def band_limit(x, name):
    low, high = BANDS.get(name, DEFAULT_BAND)
    return filtered(x, low, high)


def write(path, x):
    x16 = (np.clip(x, -1, 1) * 32767).astype(np.int16)
    run_ffmpeg(["-y", "-f", "s16le", "-ac", str(x.shape[1]), "-ar", str(RATE), "-i", "-", "-c:a", "pcm_s16le", path],
               x16.tobytes())


def level(x):
    return 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-9)


def fade(x, seconds_in, seconds_out):
    x = x.copy()
    n_in = min(int(seconds_in * RATE), x.shape[0] // 4)
    n_out = min(int(seconds_out * RATE), x.shape[0] // 2)
    if n_in > 0:
        x[:n_in] *= np.linspace(0, 1, n_in)[:, None]
    if n_out > 0:
        x[-n_out:] *= np.linspace(1, 0, n_out)[:, None] ** 2
    return x


def trim_tail(x, floor_db=-50.0):
    win = RATE // 100
    frames = max(x.shape[0] // win, 1)
    env = np.array([np.sqrt(np.mean(x[i * win:(i + 1) * win] ** 2)) for i in range(frames)])
    loud = np.nonzero(env > env.max() * 10 ** (floor_db / 20))[0]
    return x[: min((int(loud[-1]) + 1) * win, x.shape[0])] if loud.size else x


def soft_clip(x, drive):
    return np.tanh(x * drive) / np.tanh(drive)


def mix_layers(find, layers, channels, match_db=-20.0):
    parts = []
    for query, start, length, gain, *pitch in layers:
        x = read(find(query), start, length, channels, *pitch)
        parts.append(x * (10 ** ((match_db - level(x)) / 20)) * gain)
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


# --- The rifle -----------------------------------------------------------------------

def build_shots(find, out):
    length = TAKE_LENGTH
    early = int(0.06 * RATE)

    def norm(x, db):
        # Balanced by their first 60 ms, where the shot lives.
        return x * (10 ** ((db - level(x[:early])) / 20))

    def snap(x, decay):
        # Keeps a layer's transient and lets it die fast after it: the attack
        # carries the detail, a sustained fizz only tires the ear.
        t = np.arange(x.shape[0]) / RATE
        return x * np.maximum(np.exp(-np.maximum(t - 0.004, 0.0) / decay), 0.0)[:, None]

    for k in range(SHOT_VARIANTS):
        # Each layer starts 3 ms before its own transient, so they line up.
        base = eq(read(find(BARRETT_GUN), BASE_TAKES[k] - 0.003, length, 2), "highpass=f=28,lowpass=f=150")
        punch_at, semis = PUNCH_TAKES[k]
        punch = eq(read(find(BAR_CLOSE), punch_at - 0.003, 0.1, 2, semis), "highpass=f=90,lowpass=f=3500")
        punch = np.concatenate([fade(punch, 0.0, 0.03), np.zeros((int(length * RATE) - punch.shape[0], 2), np.float32)])
        body = eq(read(find(BARRETT_GUN), BASE_TAKES[k] - 0.003, length, 2), "highpass=f=150,lowpass=f=2200")
        high = eq(read(find(L85_1M), CRACK_TAKES[k] - 0.003, length, 2),
                  "highpass=f=1800,equalizer=f=3200:t=q:w=2.0:g=-5,highshelf=f=9000:g=-2")
        high = snap(high, 0.022)
        mech = snap(eq(read(find(L85_GUN), MECH_TAKES[k] - 0.003, length, 2), "highpass=f=700,lowpass=f=9000"), 0.05)
        n = min(p.shape[0] for p in (base, punch, body, high, mech))
        x = (norm(base[:n], -18.0) + norm(punch[:n], -13.0) + norm(body[:n], -20.0) + norm(high[:n], -15.5) +
             norm(mech[:n], -22.0) + norm(thump(n / RATE, 2), -20.0))
        x = soft_clip(x * 1.2, 1.2)
        x = fade(trim_tail(x, -46.0), 0.0015, 0.07)
        x *= 0.89 / (np.abs(x).max() + 1e-9)
        write(os.path.join(out, f"shot{k + 1}.wav"), x)
        print(f"shot{k + 1}       {x.shape[0] / RATE:5.2f}s level {level(x):6.1f}")

    for name, source, onsets in SHOT_TAILS:
        for i, at in enumerate(onsets):
            x = eq(read(find(source), at + 0.05, 0.9, 2), "highpass=f=90,lowpass=f=9000")
            x = fade(trim_tail(x, -45.0), 0.015, 0.3)
            x *= 0.79 / (np.abs(x).max() + 1e-9)
            write(os.path.join(out, f"{name}{i + 1}.wav"), x)
        print(f"{name:14} x{len(onsets)}")


def main():
    kit, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(kit, "sounds-index.tsv"), encoding="utf-8", errors="replace") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))

    used = set()

    def find(query):
        for row in rows:
            if query.lower() in row["file"].lower():
                used.add(f"{row['library']} / {row['file']}")
                return os.path.join(kit, row["path"])
        raise SystemExit(f"not found: {query}")

    build_shots(find, out)

    for name, channels, layers in ONE_SHOTS:
        x = band_limit(mix_layers(find, layers, channels), name)
        x = fade(trim_tail(x), 0.002 if not name.startswith("tail") else 0.03, min(0.25, x.shape[0] / RATE * 0.3))
        x *= 0.79 / (np.abs(x).max() + 1e-9)
        write(os.path.join(out, name + ".wav"), x)
        print(f"{name:14} {x.shape[0] / RATE:5.2f}s {channels}ch level {level(x):6.1f}")

    for name, channels, target, crossfade, layers in BEDS:
        x = seamless(band_limit(mix_layers(find, layers, channels), name), crossfade)
        x *= 10 ** ((target - level(x)) / 20)
        peak = np.abs(x).max()
        if peak > 0.89:
            x *= 0.89 / peak
        write(os.path.join(out, name + ".wav"), x)
        print(f"{name:14} {x.shape[0] / RATE:5.2f}s {channels}ch level {level(x):6.1f} loop")

    with open(os.path.join(out, "SOURCES.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(sorted(used)) + "\n")


if __name__ == "__main__":
    main()
