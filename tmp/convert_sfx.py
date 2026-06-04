"""Convert downloaded OGG previews to mono 16-bit PCM WAV @ 44.1 kHz for net-shooter SFX."""
import os
import numpy as np
import soundfile as sf
import scipy.signal as sig

STAGING = r"C:\Users\elias\Documents\_dev\iron-core-engine\tmp\sfx-staging"
TARGET_DIR = r"C:\Users\elias\Documents\_dev\iron-core-engine\games\07-net-shooter\assets\sfx"
TARGET_SR = 44100


def load_mono(path: str) -> tuple[np.ndarray, int]:
    data, sr = sf.read(path)
    if data.ndim > 1:
        data = data.mean(axis=1)
    return data.astype(np.float64), sr


def resample(data: np.ndarray, sr: int, target_sr: int = TARGET_SR) -> np.ndarray:
    if sr == target_sr:
        return data
    n = int(round(len(data) * target_sr / sr))
    return sig.resample(data, n)


def apply_fade(data: np.ndarray, in_ms: float = 5.0, out_ms: float = 30.0, sr: int = TARGET_SR) -> np.ndarray:
    n_in = max(1, int(in_ms * sr / 1000))
    n_out = max(1, int(out_ms * sr / 1000))
    n_in = min(n_in, len(data) // 2)
    n_out = min(n_out, len(data) // 2)
    if n_in > 0:
        data[:n_in] *= np.linspace(0.0, 1.0, n_in)
    if n_out > 0:
        data[-n_out:] *= np.linspace(1.0, 0.0, n_out)
    return data


def normalize(data: np.ndarray, target_peak: float = 0.891) -> np.ndarray:
    """Normalize to ~-1 dBFS."""
    peak = float(np.max(np.abs(data))) if len(data) else 0.0
    if peak > 0:
        data = data / peak * target_peak
    return data


def write_wav(path: str, data: np.ndarray) -> int:
    data16 = np.clip(data * 32767, -32768, 32767).astype(np.int16)
    sf.write(path, data16, TARGET_SR, subtype="PCM_16")
    return os.path.getsize(path)


def trim_to_window(data: np.ndarray, target_sec: float, sr: int = TARGET_SR,
                   pre_ms: float = 30.0) -> np.ndarray:
    """Centre window on peak with `pre_ms` of head-room."""
    target_samples = int(target_sec * sr)
    if len(data) <= target_samples:
        return data
    peak_idx = int(np.argmax(np.abs(data)))
    start = max(0, peak_idx - int(pre_ms * sr / 1000))
    end = min(len(data), start + target_samples)
    # If we ran off the end, slide window back
    if end - start < target_samples and start > 0:
        start = max(0, end - target_samples)
    return data[start:end]


def detect_silence_segments(data: np.ndarray, sr: int, silence_db: float = -40.0,
                            min_silence_ms: float = 100.0, min_segment_ms: float = 80.0) -> list[tuple[int, int]]:
    """Find non-silent segments. Returns list of (start, end) sample indices."""
    win = max(1, int(0.005 * sr))  # 5 ms RMS window
    # RMS envelope
    sq = data ** 2
    kernel = np.ones(win) / win
    rms = np.sqrt(np.convolve(sq, kernel, mode="same"))
    peak = np.max(rms) if len(rms) else 0.0
    if peak <= 0:
        return []
    threshold = peak * (10 ** (silence_db / 20))
    above = rms > threshold

    segments: list[tuple[int, int]] = []
    in_seg = False
    seg_start = 0
    min_silence_samples = int(min_silence_ms * sr / 1000)
    silence_run = 0
    for i, a in enumerate(above):
        if a:
            if not in_seg:
                in_seg = True
                seg_start = i
            silence_run = 0
        else:
            if in_seg:
                silence_run += 1
                if silence_run >= min_silence_samples:
                    seg_end = i - silence_run
                    segments.append((seg_start, seg_end))
                    in_seg = False
                    silence_run = 0
    if in_seg:
        segments.append((seg_start, len(above)))
    # Filter very short segments
    min_segment_samples = int(min_segment_ms * sr / 1000)
    return [(s, e) for s, e in segments if (e - s) >= min_segment_samples]


def process_simple(src: str, dst: str, target_sec: float, fade_out_ms: float = 30.0) -> int:
    data, sr = load_mono(src)
    data = resample(data, sr)
    data = trim_to_window(data, target_sec)
    data = apply_fade(data, in_ms=3.0, out_ms=fade_out_ms)
    data = normalize(data)
    return write_wav(dst, data)


def process_footsteps(src: str, dst_paths: list[str], target_sec: float = 0.2) -> list[int]:
    """Slice multi-footstep file into individual variants."""
    data, sr = load_mono(src)
    data = resample(data, sr)
    segs = detect_silence_segments(data, TARGET_SR, silence_db=-30.0,
                                   min_silence_ms=100.0, min_segment_ms=80.0)
    print(f"  detected {len(segs)} footstep segments")
    if len(segs) < len(dst_paths):
        # Fallback: slice into N equal chunks if detection failed
        n = len(dst_paths)
        chunk = len(data) // n
        segs = [(i * chunk, (i + 1) * chunk) for i in range(n)]
        print(f"  fallback to {n} equal chunks")
    sizes: list[int] = []
    target_samples = int(target_sec * TARGET_SR)
    for i, dst in enumerate(dst_paths):
        s, e = segs[i]
        # Pad pre with ~10 ms head-room
        pre = int(0.010 * TARGET_SR)
        s = max(0, s - pre)
        # Take target window
        e = min(len(data), s + target_samples)
        seg = data[s:e].copy()
        seg = apply_fade(seg, in_ms=3.0, out_ms=20.0)
        seg = normalize(seg)
        sizes.append(write_wav(dst, seg))
    return sizes


def main() -> None:
    os.makedirs(TARGET_DIR, exist_ok=True)
    results = []

    # gunshot-rifle: sharp transient, ~0.3s
    src = os.path.join(STAGING, "gunshot-rifle.ogg")
    dst = os.path.join(TARGET_DIR, "gunshot-rifle.wav")
    sz = process_simple(src, dst, target_sec=0.3, fade_out_ms=80.0)
    results.append(("gunshot-rifle.wav", sz))

    # rocket-launch: whoosh / thrust, ~0.5s
    src = os.path.join(STAGING, "rocket-launch.ogg")
    dst = os.path.join(TARGET_DIR, "rocket-launch.wav")
    sz = process_simple(src, dst, target_sec=0.5, fade_out_ms=80.0)
    results.append(("rocket-launch.wav", sz))

    # footsteps: split into 4
    src = os.path.join(STAGING, "footsteps-pack.ogg")
    dsts = [os.path.join(TARGET_DIR, f"footstep-0{i}.wav") for i in (1, 2, 3, 4)]
    sizes = process_footsteps(src, dsts, target_sec=0.2)
    for d, sz in zip(dsts, sizes):
        results.append((os.path.basename(d), sz))

    # hit: ~0.2s
    src = os.path.join(STAGING, "hit.ogg")
    dst = os.path.join(TARGET_DIR, "hit.wav")
    sz = process_simple(src, dst, target_sec=0.2, fade_out_ms=40.0)
    results.append(("hit.wav", sz))

    # jump: ~0.3s
    src = os.path.join(STAGING, "jump.ogg")
    dst = os.path.join(TARGET_DIR, "jump.wav")
    sz = process_simple(src, dst, target_sec=0.3, fade_out_ms=40.0)
    results.append(("jump.wav", sz))

    # death (fox yelp): ~0.5s
    src = os.path.join(STAGING, "death.ogg")
    dst = os.path.join(TARGET_DIR, "death.wav")
    sz = process_simple(src, dst, target_sec=0.5, fade_out_ms=80.0)
    results.append(("death.wav", sz))

    print("\n=== Results ===")
    for name, sz in results:
        print(f"  {name}: {sz} bytes ({sz/1024:.1f} KB)")


if __name__ == "__main__":
    main()
