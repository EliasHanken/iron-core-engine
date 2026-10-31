"""Gives a Kindle recording its sound. The capture path only records pictures, so a
scripted run logs what played, and this mixes that log and adds it to the video.

    python mix_audio.py <video.mp4> <assets/audio folder> <output.mp4>

The log is <video.mp4>.sound.csv. Needs ffmpeg and numpy.
"""
import csv
import os
import subprocess
import sys

import numpy as np

RATE = 44100


def load(path):
    pcm = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "2", "-ar", str(RATE), "-f", "f32le", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(pcm, dtype=np.float32).reshape(-1, 2)


def mix_bed(out, sound, times, gains, pitches):
    """Reads through a loop at a speed that follows the log, so pitch changes smoothly."""
    n = out.shape[0]
    t = np.arange(n) / RATE
    gain = np.interp(t, times, gains)
    pitch = np.interp(t, times, pitches)
    phase = np.cumsum(pitch)
    low = phase.astype(np.int64)
    frac = (phase - low)[:, None]
    a = sound[low % sound.shape[0]]
    b = sound[(low + 1) % sound.shape[0]]
    out += (a * (1 - frac) + b * frac) * gain[:, None]


def main():
    video, folder, target = sys.argv[1], sys.argv[2], sys.argv[3]
    beds = {}
    shots = []
    with open(video + ".sound.csv", newline="") as f:
        for time, kind, name, gain, pitch in csv.reader(f):
            if kind == "bed":
                beds.setdefault(name, []).append((float(time), float(gain), float(pitch)))
            else:
                shots.append((float(time), name, float(gain), float(pitch)))

    seconds = float(subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", video],
                                   capture_output=True, text=True, check=True).stdout)
    out = np.zeros((int(seconds * RATE), 2), dtype=np.float32)

    cache = {}
    sound = lambda name: cache.setdefault(name, load(os.path.join(folder, name + ".wav")))

    for name, rows in beds.items():
        rows = np.array(rows)
        mix_bed(out, sound(name), rows[:, 0], rows[:, 1], rows[:, 2])
    for time, name, gain, pitch in shots:
        x = sound(name)
        if pitch != 1.0:
            # Reads faster or slower through the sound, as a pitched voice does.
            read = np.arange(0, x.shape[0] - 1, pitch)
            x = np.stack([np.interp(read, np.arange(x.shape[0]), x[:, c]) for c in range(2)], axis=1)
        start = int(time * RATE)
        end = min(start + x.shape[0], out.shape[0])
        if start < end:
            out[start:end] += x[: end - start] * gain

    peak = np.abs(out).max()
    print(f"peak before limiting {20 * np.log10(peak + 1e-9):.1f} dB, {len(shots)} shots")
    # A soft limit, so that the finale can be loud without clipping.
    out = np.tanh(out * 1.1) * 0.9

    wav = target + ".mix.wav"
    x16 = (out * 32767).astype(np.int16)
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "s16le", "-ac", "2", "-ar", str(RATE), "-i", "-", wav],
                   input=x16.tobytes(), check=True)
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", video, "-i", wav, "-c:v", "copy", "-c:a", "aac", "-b:a", "256k",
                    "-movflags", "+faststart", "-shortest", target], check=True)
    os.remove(wav)


if __name__ == "__main__":
    main()
