"""Paints Foundry's impact decals: bullet holes in concrete, metal and wood, and a
scorch mark, as RGBA images (straight alpha) the engine projects onto surfaces.

    python make_decals.py <out dir>

Everything is drawn from noise and simple shapes, so the images are ours (Apache-2.0
with the rest of the repo). Each is 256 x 256, centred, with alpha fading to nothing
before the edge so the decal's box never shows.
"""
import os
import sys

import numpy as np
from PIL import Image

SIZE = 256
rng = np.random.default_rng(7)

y, x = np.mgrid[0:SIZE, 0:SIZE].astype(np.float32)
u = (x + 0.5) / SIZE * 2.0 - 1.0
v = (y + 0.5) / SIZE * 2.0 - 1.0
r = np.sqrt(u * u + v * v)
angle = np.arctan2(v, u)


def noise(cells, seed):
    """Smooth value noise, 0..1, from a coarse random grid scaled up."""
    g = np.random.default_rng(seed).random((cells, cells)).astype(np.float32)
    img = Image.fromarray((g * 255).astype(np.uint8)).resize((SIZE, SIZE), Image.BICUBIC)
    return np.asarray(img).astype(np.float32) / 255.0


def fbm(seed, octaves=4):
    total, amp, norm = np.zeros((SIZE, SIZE), np.float32), 1.0, 0.0
    for o in range(octaves):
        total += noise(4 * 2 ** o, seed + o) * amp
        norm += amp
        amp *= 0.5
    return total / norm


def smooth(e0, e1, t):
    t = np.clip((t - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def rays(count, seed, width, length):
    """Thin radial streaks: cracks or splinters."""
    g = np.random.default_rng(seed)
    out = np.zeros((SIZE, SIZE), np.float32)
    for _ in range(count):
        a = g.uniform(-np.pi, np.pi)
        lng = length * g.uniform(0.5, 1.0)
        d = np.abs(np.angle(np.exp(1j * (angle - a))))
        streak = smooth(width, 0.0, d * r) * smooth(lng, lng * 0.6, r)
        out = np.maximum(out, streak)
    return out


def save(name, rgb, alpha, out_dir):
    alpha = np.clip(alpha, 0.0, 1.0) * smooth(1.0, 0.85, r)  # Nothing at the box's edge.
    img = np.dstack([np.clip(rgb, 0.0, 1.0), alpha[..., None]])
    Image.fromarray((img * 255 + 0.5).astype(np.uint8), "RGBA").save(os.path.join(out_dir, name + ".png"))
    print("wrote", name)


def concrete(out_dir):
    n = fbm(11)
    edge = r + (n - 0.5) * 0.12
    hole = smooth(0.13, 0.09, edge)
    crater = smooth(0.42, 0.18, edge)
    cracks = rays(9, 3, 0.012, 0.6) * smooth(0.12, 0.2, r)
    dust = 0.62 + 0.15 * fbm(21)
    rgb = np.dstack([dust, dust * 0.97, dust * 0.93])
    rgb = rgb * (1.0 - 0.35 * crater[..., None])               # The crater's broken face.
    rgb = rgb * (1.0 - 0.75 * hole[..., None])                 # The hole.
    rgb = rgb * (1.0 - 0.5 * cracks[..., None])
    alpha = np.maximum(np.maximum(hole, crater * (0.55 + 0.4 * n)), cracks * 0.8)
    alpha = np.maximum(alpha, smooth(0.62, 0.3, edge) * 0.25)  # A faint dust halo.
    save("concrete_hole", rgb, alpha, out_dir)


def metal(out_dir):
    n = fbm(31)
    edge = r + (n - 0.5) * 0.05
    hole = smooth(0.09, 0.07, edge)
    bare = smooth(0.22, 0.1, edge)
    scratches = rays(14, 5, 0.008, 0.45) * smooth(0.08, 0.12, r)
    steel = 0.72 + 0.12 * fbm(41)
    rgb = np.dstack([steel, steel * 1.0, steel * 1.03])
    rgb = rgb * (1.0 - 0.94 * hole[..., None])
    rgb = rgb * (1.0 - 0.25 * smooth(0.075, 0.1, edge)[..., None] * (1 - hole[..., None]))
    alpha = np.maximum(np.maximum(hole, bare * 0.9), scratches * 0.6)
    save("metal_hole", rgb, alpha, out_dir)


def wood(out_dir):
    n = fbm(51)
    stretched = np.sqrt((u * 0.7) ** 2 + (v * 1.35) ** 2)  # The grain runs across the image.
    edge = stretched + (n - 0.5) * 0.1
    hole = smooth(0.13, 0.09, edge)
    torn = smooth(0.36, 0.14, edge)
    grain = np.abs(np.sin((v * 40.0) + n * 6.0)) ** 6
    splinters = rays(10, 9, 0.02, 0.7) * smooth(0.1, 0.18, r) * (np.abs(np.cos(angle)) > 0.6)
    raw = 0.78 + 0.12 * fbm(61)
    rgb = np.dstack([raw, raw * 0.74, raw * 0.5])
    rgb = rgb * (1.0 - 0.15 * grain[..., None])
    rgb = rgb * (1.0 - 0.85 * hole[..., None])
    alpha = np.maximum(np.maximum(hole, torn * (0.6 + 0.35 * n)), splinters * 0.85)
    save("wood_hole", rgb, alpha, out_dir)


def scorch(out_dir):
    n = fbm(71, 5)
    edge = r + (n - 0.5) * 0.45
    soot = smooth(0.85, 0.15, edge)
    rgb = np.dstack([np.full_like(r, 0.06), np.full_like(r, 0.05), np.full_like(r, 0.045)])
    rgb = rgb + 0.08 * fbm(81)[..., None]
    alpha = soot * (0.65 + 0.3 * fbm(91))
    save("scorch", rgb, alpha, out_dir)


def puddle(out_dir):
    # Standing water: an uneven pool, darker and smooth (the decal makes it glossy),
    # thinning out at a ragged rim.
    n = fbm(131, 5)
    edge = r + (n - 0.5) * 0.7
    wet = smooth(0.95, 0.35, edge)
    rgb = np.dstack([np.full_like(r, 0.12), np.full_like(r, 0.125), np.full_like(r, 0.13)])
    alpha = wet * (0.75 + 0.2 * fbm(141))
    save("puddle", rgb, alpha, out_dir)


if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    concrete(out)
    metal(out)
    wood(out)
    scorch(out)
    puddle(out)
