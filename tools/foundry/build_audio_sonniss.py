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

# The pistol (an M1911, .45 ACP), the same five layers, smaller and rounder:
#   base   a lighter synthesized kick: a pistol shoves the hands, not the shoulder;
#   punch  the body: the Colt M1911 itself, 10 m off in a warehouse, its first
#          90 ms before the room answers;
#   body   a 9 mm on the gun (the HK VP70), for the mids right at the hands;
#   high   a designed pistol crack, snapped short (a .45 is subsonic: less crack);
#   mech   the slide, from the VP70's on-gun mic.
COLT_WAREHOUSE = "Large_Warehouse_Colt_M1911"
VP70_GUN = "HK_VP70_9mm_on_gun"
PISTOL_CRACK = "gun_revolver_pistol_shot_04"
PISTOL_VARIANTS = 4

# The sniper (an M700 in .308, scoped), heavier than the rifle in every layer:
#   base   the .338 Lapua's on-gun low end, with a deep kick under it;
#   punch  the BAR at 10 cm, pitched down: a full-power round's body;
#   body   the .338's mids, held longer;
#   high   a designed sniper crack, snapped;
#   mech   the .338's on-gun action;
#   tail   a long roll across open ground (tail-sniper), on top of the space's own.
SNIPER_CRACK = "gun_rifle_sniper_shot_01"
SNIPER_VARIANTS = 3

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
    # A sniper's report rolling away over open ground, lower and longer.
    ("tail-sniper", 2, [("AK47_big_open_area", 0.835 + 0.05, 3.2, 1.0, -3),
                        ("AK47_big_open_area", 9.555 + 0.05, 3.2, 0.5, -4)]),

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
    ("debris", 2, [("Debris,Stones,Soil", 0.05, 1.0, 1.0),
                   ("Pile of Scrap Metal", 0.45, 1.8, 0.7)]),
    ("bounce", 1, [("Rock Hit and Bounce Dirt_RBDII 06", 0.0, 0.5, 1.0, 4)]),
    # A smoke canister igniting, layered: the spoon's click on top, a burst of gas,
    # the hiss of it pouring, hot steam underneath for weight.
    ("smoke", 1, [("Metal,Hits,Pipe,Mixed", 0.0, 0.15, 0.5, 5),
                  ("Steam Blast,Tube or Pipe,Distant", 0.0, 1.6, 1.0),
                  ("Vent Rattle,Pipe Hiss", 5.0, 8.0, 0.8),
                  ("Steam release hot with medium pressure", 8.0, 8.0, 0.55)]),

    # The works, now and then somewhere in the hall: a steam blast, a power hammer,
    # a compressor cycling.
    ("works1", 1, [("Steam Blast,Tube or Pipe,Distant", 0.0, 4.0, 1.0)]),
    ("works2", 1, [("Power hammer,Drops,Impacts", 2.0, 6.0, 1.0)]),
    ("works3", 1, [("Air Compressor_Stress Adjustment", 0.0, 6.0, 1.0)]),
]

# Layered one-shots, the way AAA builds them: each layer kept to its own job (a
# transient on top, a body, low weight, a tail), lined up and balanced on its
# first 60 ms. A layer: (source, start, length, level dB, band, semitones, delay,
# snap). `snap` keeps a layer's attack and lets it die in that many seconds;
# "SUB" as a source is a synthesized low thump (start = from Hz, length = to Hz).
def impact(crack, body, extra, semis):
    return [crack[:3] + (crack[3], crack[4], semis, 0.0, crack[5]),
            body[:3] + (body[3], body[4], semis, 0.0, None),
            extra[:3] + (extra[3], extra[4], semis, extra[5], None)]


LAYERED = [
    # Concrete and brick: the round's crack, the stone's crunch, grit coming down.
    *[(f"hit-concrete{i + 1}", 1, impact(
        ("bullet_impact_concrete_brick", 0.0, 0.25, -15.0, (1500, 14000), 0.02),
        ("Concrete_Impact_B02", 0.0, 0.5, -15.0, (120, 6000)),
        ("Debris,Stones,Soil", d, 0.6, -25.0, (300, 9000), 0.04), s))
      for i, (d, s) in enumerate([(0.08, 0.0), (2.65, -1.0), (5.15, 1.5)])],
    # Metal: a sharp strike, the pipe's body, a short ring.
    *[(f"hit-metal{i + 1}", 1, impact(
        ("bullet_impact_metal_heavy", 0.0, 0.3, -14.0, (1000, 15000), 0.03),
        ("Metal,Hits,Pipe,Mixed", b, 0.35, -18.0, (150, 8000)),
        ("Metal_Plate_Hit_02", 0.0, 0.9, -24.0, (400, 12000), 0.0), s))
      for i, (b, s) in enumerate([(0.0, 0.0), (0.0, -1.5), (0.0, 2.0)])],
    # Wood: the crack, a hollow knock, splinters.
    *[(f"hit-wood{i + 1}", 1, impact(
        ("Impacts Soft - Short, Crack", 0.0, 0.3, -15.0, (800, 14000), 0.03),
        ("Impacts Hard - Short Wobbly", 0.0, 0.6, -15.0, (100, 6000)),
        ("Rustles - Pile, Debris 22", 0.0, 0.6, -24.0, (500, 12000), 0.03), s))
      for i, s in enumerate([0.0, -1.5, 1.0])],
    # Enemy rounds going past the head: a supersonic round's whip through the air,
    # with the crack of its shock wave on the front of it.
    *[(f"flyby{i + 1}", 1, [(f"bullet_flyby_fast_0{src}", 0.0, 0.5, -14.0, (300, 15000), semis, 0.0, None),
                            (L85_1M, f"onset:{k}", 0.06, -21.0, (2500, 16000), semis + 3.0, 0.0, 0.015)])
      for i, (src, semis, k) in enumerate([(6, 0.0, 0), (5, 1.0, 1), (6, -1.5, 2)])],
    # A round into a body: the slap of the strike, the thump through it, something wet.
    *[(f"hit-body{i + 1}", 1, [("bullet_impact_body_thump_02", 0.0, 0.45, -14.0, (150, 12000), s, 0.0, None),
                               ("punch_general_body_impact_03", 0.0, 0.4, -17.0, (60, 3000), s, 0.0, None),
                               ("GORE Impact stab dagger", "onset:0", 0.3, -25.0, (800, 12000), s, 0.0, 0.05)])
      for i, s in enumerate([0.0, -1.5, 1.5])],
    # The works' alarm: a hand-cranked siren winding up, and the yard throwing it
    # back a moment later, duller.
    ("alarm", 1, [("Sherman_M4A1_t11_foley_siren_long", 0.0, 12.0, -14.0, (150, 9000), 0.0, 0.0, None),
                  ("Sherman_M4A1_t11_foley_siren_long", 0.0, 12.0, -25.0, (200, 2500), -0.3, 0.2, None)]),
    # A body coming down on concrete, with a dirt fall's weight under it.
    *[(f"bodyfall{i + 1}", 1, [("RL_bodyfall_Concrete_Generic_Feet_Mid_Mono_Med_Impact_02", 0.0, 1.1, -14.0,
                                (60, 12000), s, 0.0, None),
                               ("RL_bodyfall_Dirt_M4_Close_Stereo_Hard_Impact_10", 0.0, 1.1, -19.0, (40, 4000),
                                s - 1.0, 0.0, None)])
      for i, s in enumerate([0.0, -2.0])],
    # Handling for the pistol and the bolt-action, layered the same way; a start of
    # "onset:k" is the source's k-th transient, found by onsets().
    # The pistol's slide: racked back, and let fly home.
    ("slide-back", 1, [("Steyr_TMP_9mm_foley", "onset:2", 0.25, -16.0, (300, 12000), 2.0, 0.0, 0.08),
                       ("gun_semi_auto_rifle_cock", "onset:0", 0.3, -18.0, (200, 12000), 3.0, 0.0, None)]),
    ("slide-forward", 1, [("gun_semi_auto_rifle_cock", "onset:1", 0.3, -15.0, (200, 14000), 3.0, 0.0, None),
                          ("Steyr_TMP_9mm_foley", "onset:3", 0.2, -18.0, (500, 14000), 2.0, 0.0, 0.05)]),
    # The magazine release's click.
    ("mag-release", 1, [("Steyr_TMP_9mm_foley", "onset:0", 0.15, -16.0, (800, 14000), 4.0, 0.0, 0.03),
                        ("Smith_&_Wesson_M29", "onset:2", 0.12, -22.0, (1500, 14000), 3.0, 0.0, 0.02)]),
    # The bolt: lifted (the lugs unlock), drawn back, run home, turned down.
    ("bolt-up", 1, [("Smith_&_Wesson_M29", "onset:0", 0.2, -16.0, (400, 14000), -2.0, 0.0, 0.05),
                    ("Metal_Barrel_Bolt_Lock_Loose_Handling_03", "onset:0", 0.35, -19.0, (150, 10000), 0.0, 0.0,
                     None)]),
    ("bolt-back", 1, [("Metal_Barrel_Bolt_Lock_Loose_Handling_03", "onset:1", 0.4, -15.0, (150, 12000), -1.0, 0.0,
                       None),
                      ("SAIGA-12", "onset:0", 0.3, -18.0, (300, 12000), 0.0, 0.0, 0.08)]),
    ("bolt-forward", 1, [("SAIGA-12", "onset:1", 0.35, -14.0, (120, 12000), 0.0, 0.0, None),
                         ("Metal_Barrel_Bolt_Lock_Loose_Handling_03", "onset:2", 0.3, -18.0, (300, 12000), 0.0, 0.0,
                          0.06)]),
    ("bolt-down", 1, [("Smith_&_Wesson_M29", "onset:1", 0.25, -15.0, (300, 14000), -3.0, 0.0, 0.06),
                      ("SUB", 110.0, 70.0, -26.0, (40, 220), 0.0, 0.0, 0.05)]),
    # A steel door: the lever's latch, then the heavy leaf swinging open on dry
    # hinges, with a low thump of its weight.
    ("door-latch", 1, [("Lock03_outdoor_opening_solid03", "onset:0", 0.4, -15.0, (300, 14000), 0.0, 0.0, None),
                       ("door_open_004", "onset:0", 0.3, -19.0, (200, 12000), 0.0, 0.0, 0.08)]),
    ("door-open", 1, [("WR_EXT_Troop Door Open_Mono", "onset:0", 1.6, -15.0, (80, 10000), -1.0, 0.0, None),
                      ("MR - Metal Shelf D, Bright Tonal Creak", "onset:1", 1.4, -21.0, (300, 9000), -2.0, 0.1,
                       None),
                      ("SUB", 70.0, 45.0, -27.0, (30, 150), 0.0, 0.0, 0.2)]),
    # A round pressed down into the magazine.
    ("round-in", 1, [("Shell Load_Round Eject", "onset:0", 0.3, -15.0, (300, 14000), 4.0, 0.0, None),
                     ("Steyr_TMP_9mm_foley", "onset:1", 0.12, -20.0, (1000, 14000), 5.0, 0.0, 0.02)]),
    # A grenade: a synthesized sub boom, a mine's low body, a large charge's punch,
    # its crack on top, and a designed sweetener for the bloom after.
    ("blast", 2, [("SUB", 58.0, 28.0, -13.0, (20, 160), 0.0, 0.0, None),
                  ("beep-mine-explosion-002", 0.52, 4.5, -14.0, (25, 300), 0.0, 0.0, None),
                  ("explosion_large_07", 0.0, 2.6, -12.0, (120, 5000), 0.0, 0.0, None),
                  ("explosion_large_07", 0.0, 0.4, -13.0, (3000, 16000), 0.0, 0.0, 0.05),
                  ("Explosives - Sweetners_07", 0.0, 3.5, -19.0, (200, 12000), 0.0, 0.0, None)]),
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
    "tail-hall": (90, 9000), "tail-yard": (90, 8000), "tail-sniper": (60, 6000),
    "mag-out": (150, 14000), "mag-in": (120, 14000), "charge": (120, 14000), "dry": (300, 14000),
    "casing1": (500, 16000), "casing2": (500, 16000), "casing3": (400, 16000),
    "ricochet": (300, 14000),
    "plate1": (120, 14000), "plate2": (120, 14000), "plate3": (150, 14000),
    "step-ground1": (60, 12000), "step-ground2": (60, 12000),
    "step-concrete1": (60, 12000), "step-concrete2": (60, 12000),
    "step-metal1": (80, 14000), "step-metal2": (80, 14000),
    "blast": (25, 14000), "debris": (80, 14000), "bounce": (250, 14000), "smoke": (60, 12000),
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


_OUT = "."


def out_dir_of(name):
    return _OUT


def boom(length, channels, start_hz, end_hz):
    """A synthesized low boom: a sine falling from start_hz to end_hz, gone in
    about half a second."""
    t = np.arange(int(length * RATE)) / RATE
    freq = end_hz + (start_hz - end_hz) * np.exp(-t / 0.08)
    x = np.sin(2.0 * np.pi * np.cumsum(freq) / RATE) * np.exp(-t / 0.22) * np.minimum(t / 0.003, 1.0)
    return np.repeat(x[:, None], channels, axis=1).astype(np.float32)


_ONSETS = {}


def onsets(path, threshold=0.35, gap=0.12):
    """The times (s) a recording's transients start: where its 5 ms envelope jumps
    past `threshold` of its loudest after quieter, at least `gap` s apart."""
    key = (path, threshold, gap)
    if key not in _ONSETS:
        x = read(path, 0.0, 600.0, 1)[:, 0]
        win = RATE // 200
        env = np.array([np.abs(x[i:i + win]).max() for i in range(0, len(x) - win, win)])
        top = env.max() + 1e-9
        found, last = [], -1e9
        for i in range(1, len(env)):
            t = i * win / RATE
            if env[i] > threshold * top and env[i - 1] < 0.6 * env[i] and t - last > gap:
                found.append(t)
                last = t
        _ONSETS[key] = found or [0.0]
    return _ONSETS[key]


def build_layered(find, name, channels, layers):
    early = int(0.06 * RATE)
    parts = []
    for source, start, length, db, band, semis, delay, snap in layers:
        if source == "SUB":
            x = boom(1.2, channels, start, length)
        else:
            path = find(source)
            if isinstance(start, str):
                # "onset:k": the k-th transient, a few milliseconds early.
                takes = onsets(path)
                start = max(takes[min(int(start.split(":")[1]), len(takes) - 1)] - 0.003, 0.0)
            x = read(path, start, length, channels, semis)
        x = eq(x, f"highpass=f={band[0]},lowpass=f={band[1]}")
        if snap is not None and snap > 0.0:
            t = np.arange(x.shape[0]) / RATE
            x = x * np.exp(-np.maximum(t - 0.004, 0.0) / snap)[:, None]
        # Balanced on its loudest 60 ms (a delayed layer may open quiet), never
        # boosted past 24 dB, so a near-silent take can't flood the mix with hiss.
        loudest = max(level(x[i:i + early]) for i in range(0, max(x.shape[0] - early, 1), early // 2))
        x = x * min(10 ** ((db - loudest) / 20), 10 ** (24 / 20))
        parts.append(np.concatenate([np.zeros((int(delay * RATE), channels), np.float32), x]))
    out = np.zeros((max(p.shape[0] for p in parts), channels), dtype=np.float32)
    for p in parts:
        out[: p.shape[0]] += p
    out = soft_clip(out * 1.1, 1.1)
    out = fade(trim_tail(out, -50.0), 0.001, min(0.25, out.shape[0] / RATE * 0.3))
    out *= 0.89 / (np.abs(out).max() + 1e-9)
    write(os.path.join(out_dir_of(name), name + ".wav"), out)
    print(f"{name:14} {out.shape[0] / RATE:5.2f}s {channels}ch level {level(out):6.1f} layered x{len(layers)}")


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
        x = (norm(base[:n], -21.0) + norm(punch[:n], -13.0) + norm(body[:n], -20.0) + norm(high[:n], -15.5) +
             norm(mech[:n], -22.0) + norm(thump(n / RATE, 2), -23.0))
        x = soft_clip(x * 1.2, 1.2)
        # Mastered like a game weapon: compressed so the body holds up right behind
        # the crack, then limited, so it's loud and dense rather than just peaky.
        x = eq(x, "acompressor=threshold=-26dB:ratio=6:attack=0.5:release=80:makeup=12dB,"
                  "alimiter=limit=0.89:attack=0.3:release=20")
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


def kick(length, channels, high_hz, low_hz, decay):
    """A synthesized kick like thump(), tuned: falling from high_hz to low_hz."""
    t = np.arange(int(length * RATE)) / RATE
    freq = low_hz + (high_hz - low_hz) * np.exp(-t / 0.03)
    x = np.sin(2.0 * np.pi * np.cumsum(freq) / RATE) * np.exp(-t / decay) * np.minimum(t / 0.002, 1.0)
    return np.repeat(x[:, None], channels, axis=1).astype(np.float32)


def master(x, threshold_db):
    """Mastered like a game weapon: compressed so the body holds up behind the
    crack, then limited, so it's loud and dense rather than peaky."""
    return eq(soft_clip(x * 1.2, 1.2),
              f"acompressor=threshold={threshold_db}dB:ratio=6:attack=0.5:release=80:makeup=12dB,"
              "alimiter=limit=0.89:attack=0.3:release=20")


def _norm(x, db):
    # Balanced by their first 60 ms, where the shot lives.
    early = int(0.06 * RATE)
    return x * (10 ** ((db - level(x[:early])) / 20))


def _snap(x, decay):
    # Keeps a layer's transient and lets it die fast after it.
    t = np.arange(x.shape[0]) / RATE
    return x * np.exp(-np.maximum(t - 0.004, 0.0) / decay)[:, None]


def _fit(x, n):
    return np.concatenate([x, np.zeros((max(n - x.shape[0], 0), x.shape[1]), np.float32)])[:n]


def build_pistol(find, out):
    length = 0.22
    n = int(length * RATE)
    colt, vp70, crack = find(COLT_WAREHOUSE), find(VP70_GUN), find(PISTOL_CRACK)
    colt_at, vp70_at, crack_at = onsets(colt, 0.5, 1.0), onsets(vp70, 0.4, 0.08), onsets(crack, 0.4, 0.3)
    for k in range(PISTOL_VARIANTS):
        semis = [0.0, -0.5, 0.6, -1.0][k]

        def take(path, at, band, keep, pitch=semis):
            x = eq(read(path, max(at - 0.003, 0.0), keep, 2, pitch), band)
            return _fit(fade(x, 0.0, min(0.03, keep * 0.4)), n)

        punch = take(colt, colt_at[k % len(colt_at)], "highpass=f=120,lowpass=f=3000", 0.09)
        body = take(vp70, vp70_at[k % len(vp70_at)], "highpass=f=150,lowpass=f=2500", 0.12)
        high = _snap(take(crack, crack_at[0], "highpass=f=2000,equalizer=f=3500:t=q:w=2.0:g=-4", 0.2,
                          semis + 1.0), 0.018)
        mech = _snap(take(vp70, vp70_at[k % len(vp70_at)], "highpass=f=1200,lowpass=f=9000", 0.15), 0.035)
        x = (_norm(kick(n / RATE, 2, 85.0, 52.0, 0.04), -24.0) + _norm(punch, -14.0) + _norm(body, -19.0) +
             _norm(high, -16.0) + _norm(mech, -21.0))
        x = fade(trim_tail(master(x, -24), -46.0), 0.0015, 0.05)
        x *= 0.89 / (np.abs(x).max() + 1e-9)
        write(os.path.join(out, f"pistol{k + 1}.wav"), x)
        print(f"pistol{k + 1}     {x.shape[0] / RATE:5.2f}s level {level(x):6.1f}")


def build_sniper(find, out):
    length = 0.6
    n = int(length * RATE)
    barrett, bar, crack = find(BARRETT_GUN), find(BAR_CLOSE), find(SNIPER_CRACK)
    crack_at = onsets(crack, 0.4, 0.3)
    for k in range(SNIPER_VARIANTS):
        at = BASE_TAKES[[1, 3, 4][k]] - 0.003
        semis = [0.0, -0.6, 0.5][k]
        base = _fit(eq(read(barrett, at, length, 2, semis), "highpass=f=25,lowpass=f=150"), n)
        body = _fit(eq(read(barrett, at, length, 2, semis), "highpass=f=150,lowpass=f=2500"), n)
        punch_at, _ = PUNCH_TAKES[k]
        punch = _fit(fade(eq(read(bar, punch_at - 0.003, 0.12, 2, semis - 3.0), "highpass=f=80,lowpass=f=3000"),
                          0.0, 0.04), n)
        high = _snap(_fit(eq(read(crack, max(crack_at[0] - 0.003, 0.0), 0.3, 2, semis),
                             "highpass=f=1800,equalizer=f=3200:t=q:w=2.0:g=-4,highshelf=f=9000:g=-2"), n), 0.03)
        mech = _snap(_fit(eq(read(barrett, at, 0.3, 2, semis), "highpass=f=1500,lowpass=f=8000"), n), 0.06)
        x = (_norm(base, -19.0) + _norm(kick(length, 2, 70.0, 38.0, 0.09), -20.0) + _norm(punch, -13.0) +
             _norm(body, -18.0) + _norm(high, -15.0) + _norm(mech, -23.0))
        x = fade(trim_tail(master(x, -26), -48.0), 0.0015, 0.12)
        x *= 0.89 / (np.abs(x).max() + 1e-9)
        write(os.path.join(out, f"sniper{k + 1}.wav"), x)
        print(f"sniper{k + 1}     {x.shape[0] / RATE:5.2f}s level {level(x):6.1f}")


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
    build_pistol(find, out)
    build_sniper(find, out)

    for name, channels, layers in ONE_SHOTS:
        x = band_limit(mix_layers(find, layers, channels), name)
        x = fade(trim_tail(x), 0.002 if not name.startswith("tail") else 0.03, min(0.25, x.shape[0] / RATE * 0.3))
        x *= 0.79 / (np.abs(x).max() + 1e-9)
        write(os.path.join(out, name + ".wav"), x)
        print(f"{name:14} {x.shape[0] / RATE:5.2f}s {channels}ch level {level(x):6.1f}")

    global _OUT
    _OUT = out
    for name, channels, layers in LAYERED:
        build_layered(find, name, channels, layers)

    for name, channels, target, crossfade, layers in BEDS:
        x = seamless(band_limit(mix_layers(find, layers, channels), name), crossfade)
        x *= 10 ** ((target - level(x)) / 20)
        peak = np.abs(x).max()
        if peak > 0.89:
            x *= 0.89 / peak
        write(os.path.join(out, name + ".wav"), x)
        print(f"{name:14} {x.shape[0] / RATE:5.2f}s {channels}ch level {level(x):6.1f} loop")

    # Ringing ears after a blast: two close tones near 3.6 kHz beating slowly,
    # in fast and fading over five seconds (synthesized, not from the kit).
    t = np.arange(int(5.0 * RATE)) / RATE
    ring = (np.sin(2 * np.pi * 3580.0 * t) + 0.6 * np.sin(2 * np.pi * 3617.0 * t)) / 1.6
    ring *= np.minimum(t / 0.08, 1.0) * np.exp(-t / 1.6)
    write(os.path.join(out, "ring.wav"), (ring * 0.6)[:, None].astype(np.float32))
    print("ring           5.00s 1ch synthesized")

    with open(os.path.join(out, "SOURCES.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(sorted(used)) + "\n")


if __name__ == "__main__":
    main()
