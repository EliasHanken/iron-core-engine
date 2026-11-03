"""The M1911 (Stein Games, CC0): a two-handed pistol grip, the support hand wrapped
round the firing hand with both thumbs forward; fired semi-auto, the slide
cycling; reloaded by dropping the magazine and seating a fresh one with the palm,
and from empty, the slide racked overhand.

Space: the pistol's own, +Y out of the muzzle, +Z up, the origin at the trigger.
Measured off the model: the grip runs from z -0.095 up under the slide (z 0.013),
raked, its front strap near y -0.035 and back strap near y -0.08, 3.4 cm wide;
the trigger guard's bottom is at z -0.03; the slide spans z 0.013..0.064, from
y -0.1 to the muzzle at 0.136, its rear sight at z 0.064.
"""
import math
import os

from mathutils import Vector

import rig
from clips import Hand, Place, Pose, ease_in, ease_out, mix_hand, mix_place, smooth, slide_bone, track, turn_bone

NAME = "m1911"
FBX = os.path.join("1911", "SKM_1911.fbx")
TEXTURES = {"color": "T_1911_C.png", "rmao": "T_1911_RMAO.png", "normal": "T_1991_N.png"}
PARTS = ["SLIDE", "TRIGGER", "HAMMER", "MAGAZINE", "RELEASE", "LEVER"]
MAGAZINE = "MAGAZINE"
TRIGGER_TRAVEL = 0.004  # A 1911's trigger slides straight back.
SUPPORT_ON_HAND = True  # The support hand wraps the firing hand.
SLIDE_TRAVEL = 0.034


def load(pack):
    return rig.import_gun(os.path.join(pack, FBX), keep_rounds=True)


def move_parts(pose, weapon, p):
    """The slide runs back on its rails, the trigger slides straight back, the
    hammer falls and is cocked again by the slide, the release button is pressed
    in and the slide stop rises when the slide locks back."""
    slide_bone(pose, "SLIDE", weapon, Vector((0.0, -p.bolt, 0.0)))
    slide_bone(pose, "TRIGGER", weapon, Vector((0.0, -TRIGGER_TRAVEL * p.trigger, 0.0)))
    # The hammer stands cocked at rest; it falls forward when the shot breaks, and
    # the slide coming back rides over it and cocks it again.
    cocked_by_slide = min(1.0, p.bolt / 0.012)
    fallen = p.parts.get("hammer", 0.0) * (1.0 - cocked_by_slide)
    turn_bone(pose, "HAMMER", weapon, "X", -58.0 * fallen)
    slide_bone(pose, "RELEASE", weapon, Vector((0.003 * p.parts.get("release", 0.0), 0.0, 0.0)))
    turn_bone(pose, "LEVER", weapon, "X", 7.0 * p.parts.get("lever", 0.0))


# --- The hold ---
# The firing hand high on the grip, the web of the hand up under the beavertail:
# fingers wrap the front strap, the index finger on the trigger, the thumb forward
# along the frame above the support thumb.
FIST = {"index": (0.2, 0.3, 0.2), "middle": (1.1, 1.2, 0.7), "ring": (1.2, 1.2, 0.7), "pinky": (1.2, 1.2, 0.7)}
GRIP = Hand(Vector((-0.004, -0.05, -0.035)), Vector((-1.0, 0.25, 0.0)), Vector((-0.25, 0.9, -0.3)), FIST,
            thumb=(1.05, 0.5, 0.3), contact=1.0, spin=20.0, yaw=20.0, together=0.9,
            trigger=Vector((0.0, -0.004, -0.008)), thumb_aim=Vector((-0.3, 1.0, 0.05)))
# The support hand: its palm on the grip's left panel, in front of the firing
# palm's heel; its fingers wrap over the firing fingers under the trigger guard;
# its thumb lies forward along the frame, under the slide.
# Its fingers keep their authored wrap (they lie over the firing hand's, which
# a grasp onto the gun can't see well); its palm and thumb fit onto the gun.
WRAP = {"index": (1.0, 1.0, 0.55), "middle": (1.1, 1.05, 0.55), "ring": (1.15, 1.05, 0.55), "pinky": (1.2, 1.0, 0.55)}
SUPPORT = Hand(Vector((-0.024, -0.052, -0.066)), Vector((1.0, 0.1, 0.3)), Vector((0.55, 0.8, -0.2)), WRAP,
               thumb=(0.1, 0.15, 0.1), contact=1.0, spin=15.0, yaw=20.0, together=0.9,
               thumb_aim=Vector((0.1, 1.0, 0.1)), free=("index", "middle", "ring", "pinky"))
POLES = {"r": Vector((0.4, 0.05, -0.55)), "l": Vector((-0.4, 0.1, -0.55))}

# Where the pistol sits, in the eye's space: out in front at the hip, the arms
# most of the way out; aimed, the sights' line (0.064 above the trigger) through
# the eye.
HIP = Place(Vector((0.11, 0.44, -0.075)), pitch=4.0, yaw=7.0, roll=-5.0)
AIM = Place(Vector((0.0, 0.42, -0.064)))


def held(place, **kw):
    return Pose(place, GRIP, SUPPORT, **kw)


def mix_vector(a, b, s):
    return a.lerp(b, s)


def lin(a, b, s):
    return a + (b - a) * s


# --- Clips ---

def idle(t):
    """Breathing: the pistol rises and settles every 3.2 seconds."""
    b = math.sin(t / 3.2 * 2.0 * math.pi)
    return held(HIP.moved(Vector((0.0, 0.0, 0.003 * b)), pitch=0.7 * b, roll=0.4 * b))


def aim(t):
    """Aimed, the arms out, breathing less."""
    b = math.sin(t / 3.2 * 2.0 * math.pi)
    return held(AIM.moved(Vector((0.0, 0.0, 0.0012 * b)), pitch=0.15 * b), shoulders={"l": 30.0, "r": 14.0})


def fire(t):
    """One shot's moving parts: the trigger breaks, the hammer falls, the slide
    slams back and returns, cocking it. Additive over whatever's playing."""
    slide = track([(0.0, 0.0), (0.008, 0.0), (0.03, SLIDE_TRAVEL), (0.07, 0.0)], t, lin, ease=lambda s: s)
    trigger = track([(0.0, 0.0), (0.01, 1.0), (0.07, 1.0), (0.12, 0.0)], t, lin)
    hammer = track([(0.0, 0.0), (0.006, 1.0), (0.2, 1.0)], t, lin, ease=lambda s: s)
    return held(HIP, bolt=slide, trigger=trigger, parts={"hammer": hammer})


def draw(t):
    """Up from the holster at the right hip, the support hand meeting it."""
    low = Place(Vector((0.22, 0.1, -0.5)), pitch=-50.0, yaw=25.0, roll=40.0)
    place = track([(0.0, low), (0.35, HIP.moved(Vector((-0.01, -0.02, 0.01)), pitch=6.0, roll=-5.0), ease_out),
                   (0.6, HIP, smooth)], t, mix_place)
    away = Hand(Vector((-0.12, -0.15, -0.2)), Vector((1.0, 0.0, 0.0)), Vector((0.2, 1.0, -0.3)),
                {k: (0.3, 0.4, 0.3) for k in WRAP}, thumb=(0.2, 0.3, 0.2))
    left = track([(0.0, away), (0.2, away), (0.45, SUPPORT, ease_out)], t, mix_hand)
    return Pose(place, GRIP, left)


# --- Reload: the right thumb drops the magazine, the left hand strips it out and
# brings the "new" one up into the well, seating it with the heel of the palm. ---
CUP = {"index": (1.1, 1.2, 0.6), "middle": (1.15, 1.2, 0.6), "ring": (1.2, 1.2, 0.6), "pinky": (1.2, 1.15, 0.6)}
MAG_DROP = 0.025
# The magazine's foot, out of the grip once released: pinched from below.
MAG_GRAB = Hand(Vector((-0.002, -0.058, -0.12 - MAG_DROP * 0.0)), Vector((0.25, 0.0, 1.0)),
                Vector((0.0, 1.0, -0.2)), CUP, thumb=(0.15, 0.2, 0.1), contact=1.0)
MAG_PULLED = Hand(Vector((0.0, -0.05, -0.2)), Vector((0.25, 0.0, 1.0)), Vector((0.0, 1.0, -0.4)), CUP,
                  thumb=(0.15, 0.2, 0.1), contact=1.0, seat=False)
MAG_AWAY = Hand(Vector((-0.06, -0.12, -0.42)), Vector((0.4, 0.0, 1.0)), Vector((0.0, 0.7, -0.7)), CUP,
                thumb=(0.15, 0.2, 0.1), contact=1.0, seat=False)
MAG_RISING = Hand(Vector((-0.01, -0.07, -0.2)), Vector((0.25, 0.0, 1.0)), Vector((0.0, 0.95, -0.35)), CUP,
                  thumb=(0.15, 0.2, 0.1), contact=1.0, seat=False)
# Seated: where it was taken, the drop pushed back up.
MAG_SEAT = Hand(MAG_GRAB.socket + Vector((0.0, 0.0, MAG_DROP)), MAG_GRAB.palm, MAG_GRAB.forward, CUP,
                thumb=(0.15, 0.2, 0.1), contact=1.0, seat=False)
UNDER = Hand(Vector((-0.04, -0.05, -0.11)), Vector((0.6, 0.0, 0.8)), Vector((0.1, 1.0, -0.3)),
             {k: (0.3, 0.4, 0.3) for k in CUP}, thumb=(0.2, 0.3, 0.2))
# The right thumb reaching the release button behind the trigger, on the left.
THUMB_PRESS = Hand(GRIP.socket, GRIP.palm, GRIP.forward, FIST, thumb=(0.75, 0.6, 0.35), contact=1.0, spin=20.0,
                   yaw=20.0, together=0.9, trigger=GRIP.trigger)
# The overhand rack: the left hand over the slide's rear, gripping its serrations.
RACK = {"index": (1.0, 1.2, 0.8), "middle": (1.05, 1.2, 0.8), "ring": (1.1, 1.2, 0.8), "pinky": (1.1, 1.15, 0.8)}
OVER = Hand(Vector((-0.03, -0.07, 0.07)), Vector((0.7, 0.0, -0.7)), Vector((0.9, 0.0, 0.3)),
            {k: (0.4, 0.5, 0.3) for k in RACK}, thumb=(0.2, 0.3, 0.2))
SLIDE_GRIP = Hand(Vector((-0.02, -0.075, 0.045)), Vector((0.6, 0.0, -0.8)), Vector((1.0, 0.0, -0.1)), RACK,
                  thumb=(0.3, 0.4, 0.3), contact=1.0)
RELOAD_TILT = Place(Vector((0.08, 0.34, -0.12)), pitch=10.0, yaw=8.0, roll=35.0)


def reload_pose(t, empty):
    end = 1.9 if not empty else 2.5
    tilt = RELOAD_TILT
    place_keys = [
        (0.0, HIP),
        (0.08, HIP.moved(Vector((0.0, 0.0, -0.006)), pitch=-2.0), smooth),
        (0.3, tilt, ease_out),
        (0.45, tilt.moved(Vector((0.0, 0.0, 0.004)), roll=-3.0), smooth),   # Shaken free.
        (0.55, tilt, smooth),
        (1.3, tilt, smooth),
        (1.38, tilt.moved(Vector((0.0, 0.0, 0.012)), pitch=3.0), ease_out),  # The slap seats it.
        (1.5, tilt, smooth),
    ]
    if empty:
        rack = tilt.moved(Vector((-0.02, 0.02, 0.02)), roll=-15.0, yaw=-6.0)
        place_keys += [(1.7, rack, smooth), (1.92, rack.moved(Vector((0.0, 0.015, 0.0)), pitch=-3.0), ease_out),
                       (2.05, rack, smooth)]
    place_keys += [(end - 0.3, HIP.moved(Vector((0.0, 0.0, 0.006)), pitch=2.0, roll=-3.0), smooth), (end, HIP, smooth)]
    place = track(place_keys, t, mix_place)

    right = track([(0.0, GRIP), (0.18, GRIP), (0.26, THUMB_PRESS, ease_out), (0.36, THUMB_PRESS),
                   (0.46, GRIP, smooth)], t, mix_hand)
    release = track([(0.0, 0.0), (0.24, 0.0), (0.28, 1.0, ease_out), (0.38, 1.0), (0.44, 0.0)], t, lin)
    drop = track([(0.0, 0.0), (0.28, 0.0), (0.4, MAG_DROP, ease_in)], t, lin)
    left_keys = [(0.0, SUPPORT), (0.12, SUPPORT), (0.3, UNDER, smooth), (0.44, MAG_GRAB, ease_out), (0.5, MAG_GRAB),
                 (0.6, MAG_PULLED, ease_in), (0.8, MAG_AWAY, ease_in), (0.9, MAG_AWAY),
                 (1.15, MAG_RISING, ease_out), (1.3, MAG_SEAT, ease_out), (1.4, MAG_SEAT),
                 (1.55, UNDER, smooth)]
    bolt = SLIDE_TRAVEL if empty and t < 1.95 else 0.0
    lever = 1.0 if empty and t < 1.95 else 0.0
    if empty:
        left_keys += [(1.7, OVER, smooth), (1.8, SLIDE_GRIP, ease_out),
                      (1.92, Hand(SLIDE_GRIP.socket - Vector((0.0, 0.01, 0.0)), SLIDE_GRIP.palm, SLIDE_GRIP.forward,
                                  RACK, thumb=(0.3, 0.4, 0.3), contact=1.0), ease_out),
                      (2.02, OVER, smooth), (end - 0.1, SUPPORT, ease_out)]
        # Locked back until the hand pulls it off the stop, then it slams home.
        bolt = track([(0.0, SLIDE_TRAVEL), (1.82, SLIDE_TRAVEL), (1.92, SLIDE_TRAVEL + 0.004, ease_out),
                      (1.95, 0.0, ease_in)], t, lin)
    else:
        left_keys += [(end - 0.15, SUPPORT, ease_out)]
    left = track(left_keys, t, mix_hand)
    magazine = "hand" if 0.47 <= t < 1.36 else "gun"
    # Out of the grip it hangs free below until the hand has it; seated, it's home.
    drop = drop if t < 0.47 else 0.0
    lp = POLES["l"]
    left_pole = track([(0.0, lp), (0.4, Vector((-0.45, 0.0, -0.55))), (0.85, Vector((-0.4, -0.05, -0.75))),
                       (1.3, Vector((-0.45, 0.0, -0.55))), (end, lp)], t, mix_vector)
    return Pose(place, right, left, bolt=bolt, magazine=magazine,
                parts={"release": release, "mag_drop": drop, "lever": lever},
                poles={"l": left_pole})


def reload(t):
    return reload_pose(t, empty=False)


def reload_empty(t):
    return reload_pose(t, empty=True)


def sprint(t):
    """Running: the pistol pulled in low at the chest, muzzle down, bouncing."""
    phase = t / 0.62 * 2.0 * math.pi
    base = Place(Vector((0.08, 0.2, -0.26)), pitch=-35.0, yaw=20.0, roll=-15.0)
    return held(base.moved(Vector((0.012 * math.sin(phase), 0.0, 0.014 * abs(math.sin(phase)) - 0.007)),
                           pitch=3.0 * math.sin(2.0 * phase), roll=3.0 * math.sin(phase)),
                poles={"l": Vector((-0.25, 0.05, -0.7)), "r": Vector((0.3, -0.05, -0.7))})


def inspect(t):
    """Turned to look at: the left side, then over to the right."""
    left_side = Place(Vector((0.03, 0.3, -0.12)), pitch=10.0, yaw=-15.0, roll=60.0)
    right_side = Place(Vector((0.07, 0.3, -0.13)), pitch=8.0, yaw=30.0, roll=-50.0)
    place = track([(0.0, HIP), (0.6, left_side, smooth), (1.6, left_side.moved(Vector((0.0, 0.01, 0.006)), roll=5.0),
                                                          smooth),
                   (2.2, right_side, smooth), (3.1, right_side.moved(Vector((0.0, 0.0, 0.006)), pitch=3.0), smooth),
                   (3.7, HIP, smooth)], t, mix_place)
    return held(place)


CLIPS = [
    ("idle", 3.2, idle, True, []),
    ("aim", 3.2, aim, True, []),
    ("fire", 0.2, fire, False, []),
    ("draw", 0.6, draw, False, [(0.0, "draw")]),
    ("reload", 1.9, reload, False, [(0.26, "mag_release"), (0.4, "mag_out"), (1.36, "mag_in")]),
    ("reload_empty", 2.5, reload_empty, False,
     [(0.26, "mag_release"), (0.4, "mag_out"), (1.36, "mag_in"), (1.84, "bolt_back"), (1.95, "bolt_forward")]),
    ("sprint", 0.62, sprint, True, []),
    ("inspect", 3.7, inspect, False, [(0.2, "grab"), (1.8, "grab")]),
]

PREVIEW = {"draw": (0.15, 0.35), "reload": (0.27, 0.45, 0.6, 1.2, 1.36), "reload_empty": (1.8, 1.9),
           "sprint": (0.15,), "inspect": (1.0, 2.6)}
GRAB_TIME = 0.48


# The grenade thrown with the left hand (throwing.py).
from weapons.throwing import make_throw  # noqa: E402

CLIPS.append(make_throw(HIP, GRIP, SUPPORT, lambda place, right, left, **kw: Pose(place, right, left, **kw)))
PREVIEW["throw"] = (0.15, 0.3, 0.4, 0.5, 0.7)
