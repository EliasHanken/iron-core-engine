"""Builds Kindle's sound set from CC0 recordings, so that better source files can be
dropped in later and everything rebuilt the same way.

    python build_audio.py <folder of downloads> <games/19-kindle/assets/audio>

Needs ffmpeg and numpy. Loops get a crossfaded seam, and one-shots get trimmed at
their tail. Every source is listed here with its author and page, and CREDITS.md
is written from this list.
"""
import os
import subprocess
import sys

import numpy as np

RATE = 44100

# name, source file, Freesound id, author, what it is, start, length, target level
LOOPS = [
    ("room", "5a_grain_silo_wind_interior__felix.blume_667664__PREVIEW.ogg", 667664, "felix.blume",
     "wind inside a grain silo", 200.0, 44.0, -21.0, 2),
    ("roar", "2a_roar_bronze_furnace__iankath_173991__PREVIEW.ogg", 173991, "iankath",
     "bronze furnace", 20.0, 30.0, -20.0, 1),
    ("crackle", "1a_crackle_loop__NickTayloe_813328__PREVIEW.ogg", 813328, "NickTayloe",
     "burning wicks", 8.0, 26.0, -24.0, 1),
    ("rush", "2b_roar_flamethrower_loop__Nox_Sound_563138__PREVIEW.ogg", 563138, "Nox_Sound",
     "flamethrower", 1.0, 10.5, -20.0, 1),
]

# name, source file, id, author, what it is, start, longest length, channels
SHOTS = [
    ("ignite", "4c_deep_blasts_pitched_fireworks__DylanTheFish_455513__PREVIEW.ogg", 455513, "DylanTheFish",
     "pitched-down firework blast", 47.3, 6.0, 1),
    ("finale", "4c_deep_blasts_pitched_fireworks__DylanTheFish_455513__PREVIEW.ogg", 455513, "DylanTheFish",
     "pitched-down firework blast", 130.8, 12.0, 2),
    ("spool", "7a_J79_jet_start_cart__C-V_704945__PREVIEW.ogg", 704945, "C-V",
     "J79 jet engine starting", 2.0, 14.0, 2),
    ("wake", "4a_bunker_doors_reverberant__SecureSubset_845666__PREVIEW.ogg", 845666, "SecureSubset",
     "bunker door in a reverberant space", 9.1, 7.0, 1),
    ("gate", "4b_big_metal_door__kikorurelas_608396__PREVIEW.ogg", 608396, "kikorurelas",
     "big metal door", 0.0, 13.0, 1),
    ("groan", "6b_bunker_door_groan__hinchinbrook_496836__PREVIEW.ogg", 496836, "hinchinbrook",
     "bunker door groan", 0.0, 9.0, 1),
    ("creak1", "6a_rotting_ferry_creaks__allthingssound_425183__PREVIEW.ogg", 425183, "allthingssound",
     "creaks in a rotting ferry", 3.2, 4.0, 1),
    ("creak2", "6a_rotting_ferry_creaks__allthingssound_425183__PREVIEW.ogg", 425183, "allthingssound",
     "creaks in a rotting ferry", 139.4, 4.0, 1),
    ("creak3", "6a_rotting_ferry_creaks__allthingssound_425183__PREVIEW.ogg", 425183, "allthingssound",
     "creaks in a rotting ferry", 435.0, 4.0, 1),
    ("chain", "6d_iron_chain__cabled_mess_571375__PREVIEW.ogg", 571375, "cabled_mess",
     "iron chain", 50.7, 3.0, 1),
    ("bend", "6c_bending_metal__NachtmahrTV_556714__PREVIEW.ogg", 556714, "NachtmahrTV",
     "bending metal", 0.0, 5.9, 1),
    ("whoosh1", "3b_swish_x12__hz37_463763__PREVIEW.ogg", 463763, "hz37", "swish", 0.0, 1.3, 1),
    ("whoosh2", "3b_swish_x12__hz37_463763__PREVIEW.ogg", 463763, "hz37", "swish", 1.65, 1.3, 1),
    ("whoosh3", "3b_swish_x12__hz37_463763__PREVIEW.ogg", 463763, "hz37", "swish", 3.3, 1.3, 1),
    ("whoosh4", "3b_swish_x12__hz37_463763__PREVIEW.ogg", 463763, "hz37", "swish", 4.95, 1.3, 1),
]


def read(path, start, length, channels):
    pcm = subprocess.run(
        ["ffmpeg", "-v", "error", "-ss", str(start), "-t", str(length), "-i", path, "-ac", str(channels),
         "-ar", str(RATE), "-f", "f32le", "-"], capture_output=True, check=True).stdout
    return np.frombuffer(pcm, dtype=np.float32).reshape(-1, channels).copy()


def write(path, x):
    x16 = (np.clip(x, -1, 1) * 32767).astype(np.int16)
    subprocess.run(
        ["ffmpeg", "-v", "error", "-y", "-f", "s16le", "-ac", str(x.shape[1]), "-ar", str(RATE), "-i", "-",
         "-c:a", "pcm_s16le", path], input=x16.tobytes(), check=True)


def level(x):
    return 20 * np.log10(np.sqrt(np.mean(x ** 2)) + 1e-9)


def seamless(x, fade_seconds):
    """Crossfades the tail into the head, at equal power, so nothing dips at the seam."""
    n = int(fade_seconds * RATE)
    t = np.linspace(0, 1, n)[:, None]
    head = np.sqrt(t)
    tail = np.sqrt(1 - t)
    out = x[:-n].copy()
    out[:n] = x[:n] * head + x[-n:] * tail
    return out


def trim_tail(x):
    """Ends a one-shot where it falls quiet, and fades the last moment."""
    win = RATE // 50
    frames = x.shape[0] // win
    env = np.array([np.sqrt(np.mean(x[i * win:(i + 1) * win] ** 2)) for i in range(frames)])
    floor = env.max() * 10 ** (-42 / 20)
    loud = np.nonzero(env > floor)[0]
    end = min((int(loud[-1]) + 1) * win, x.shape[0])
    x = x[:end].copy()
    fade = min(int(0.25 * RATE), end // 3)
    x[-fade:] *= np.linspace(1, 0, fade)[:, None]
    start = min(int(0.004 * RATE), end // 4)
    x[:start] *= np.linspace(0, 1, start)[:, None]
    return x


def seam_report(x):
    """A click shows as a jump at the wrap far bigger than the sound's usual step."""
    steps = np.abs(np.diff(x, axis=0)).max(axis=1)
    jump = np.abs(x[0] - x[-1]).max()
    return jump / (np.percentile(steps, 99.9) + 1e-9)


def main():
    raw, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    credits = [
        "# Kindle sound credits\n\n",
        "Every recording here was marked CC0 on its Freesound page when downloaded, so no credit is\n",
        "required. These are listed anyway, with thanks. Built by `tools/kindle/build_audio.py`.\n\n",
        "These come from Freesound's Ogg previews, not the uploaders' original files.\n\n",
        "| File | Recording | Author | Source |\n|---|---|---|---|\n",
    ]
    for name, source, sid, author, what, start, length, target, channels in LOOPS:
        x = read(os.path.join(raw, source), start, length, channels)
        x = seamless(x, min(2.5, length * 0.2))
        x *= 10 ** ((target - level(x)) / 20)
        peak = np.abs(x).max()
        if peak > 0.89:
            x *= 0.89 / peak
        write(os.path.join(out, name + ".wav"), x)
        credits.append(f"| {name}.wav | {what} | {author} | https://freesound.org/s/{sid}/ |\n")
        print(f"{name:8} loop {x.shape[0] / RATE:5.1f}s {x.shape[1]}ch level {level(x):6.1f} "
              f"peak {20 * np.log10(np.abs(x).max()):6.1f} seam {seam_report(x):5.2f}")
    for name, source, sid, author, what, start, length, channels in SHOTS:
        x = trim_tail(read(os.path.join(raw, source), start, length, channels))
        x *= 0.71 / (np.abs(x).max() + 1e-9)
        write(os.path.join(out, name + ".wav"), x)
        credits.append(f"| {name}.wav | {what} | {author} | https://freesound.org/s/{sid}/ |\n")
        print(f"{name:8} shot {x.shape[0] / RATE:5.1f}s {x.shape[1]}ch level {level(x):6.1f}")
    with open(os.path.join(out, "CREDITS.md"), "w", encoding="utf-8", newline="\n") as f:
        f.writelines(credits)


if __name__ == "__main__":
    main()
