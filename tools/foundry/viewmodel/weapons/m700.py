"""The M700 (Stein Games, CC0) with a scope modelled for it (scope.py): held by its
grip and forend, fired a round at a time, the bolt worked by hand between shots
(lifted, drawn back, run home, turned down), and reloaded a round at a time
through the open action: reload_start opens it, reload_round is played once a
round, reload_end closes it.

Space: the rifle's own, +Y out of the muzzle, +Z up, the origin at the trigger.
Measured off the model: the stock's grip runs from the trigger guard back and
down, its front edge from y -0.04 (z 0) to -0.09 (z -0.05), 3 cm wide; the forend
spans y 0.15..0.45, z 0..0.05, 3.4 cm wide; the bolt turns about z 0.044 and its
knob sits at (0.036, -0.004, 0.007), out to the right; the scope rail's top is at
z 0.065 over y 0.013..0.172; the trigger's blade hangs to z -0.031.
"""
import math
import os

from mathutils import Matrix, Vector

import rig
import scope
from clips import Hand, Place, Pose, ease_in, ease_out, mix_hand, mix_place, smooth, track, turn_bone

NAME = "m700"
FBX = os.path.join("M700", "SKM_M700.fbx")
TEXTURES = {"color": "T_M700_C.png", "rmao": "T_M700_RMAO.png", "normal": "T_M700_N.png"}
PARTS = ["BOLT", "TRIGGER", "BULLET_01"]
MAGAZINE = None  # Its magazine is inside the stock: rounds go in through the action.
TRIGGER_TRAVEL = 0.004

BOLT_PIVOT = Vector((0.0, 0.092, 0.044))  # On the bolt's axis.
BOLT_LIFT = 62.0     # Degrees the handle turns up to unlock.
BOLT_TRAVEL = 0.09   # How far it draws back, in metres.
KNOB = Vector((0.036, -0.004, 0.007))
SCOPE_AXIS = 0.097   # Set by load(): the scope's axis height above the trigger.


def load(pack):
    global SCOPE_AXIS
    skeleton, gun = rig.import_gun(os.path.join(pack, FBX), keep_unbound=True, keep_rounds=True)
    SCOPE_AXIS = scope.attach_scope(gun, Vector((0.0, 0.0925, 0.065)), centre_y=0.09, height=0.032)
    return skeleton, gun


def bolt_matrix(lift, back):
    """The bolt's move from rest: turned up about its axis by `lift` (0..1), drawn
    back `back` metres."""
    return (Matrix.Translation((0.0, -back, 0.0)) @ Matrix.Translation(BOLT_PIVOT) @
            Matrix.Rotation(math.radians(-BOLT_LIFT * lift), 4, "Y") @ Matrix.Translation(-BOLT_PIVOT))


def move_parts(pose, weapon, p):
    """The bolt turns and slides; the trigger swings back on its pin; a round
    being loaded rides from the hand down into the magazine."""
    lift = p.parts.get("lift", 0.0)
    turn_bone(pose, "BOLT", weapon, "Y", -BOLT_LIFT * lift, pivot=BOLT_PIVOT, offset=Vector((0.0, -p.bolt, 0.0)))
    turn_bone(pose, "TRIGGER", weapon, "X", -12.0 * p.trigger)
    bullet = pose["BULLET_01"]
    seated = weapon @ bullet.bone.matrix_local
    loading = p.parts.get("round")
    if loading is None:
        bullet.matrix = seated
    else:
        # Held between the thumb and fingers, then pressed down into the action.
        held_at = pose["socket.r"].matrix @ Matrix.Translation((0.0, 0.03, -0.01))
        at = held_at.translation.lerp(seated.translation, loading)
        bullet.matrix = Matrix.LocRotScale(at, seated.to_quaternion(), None)


# --- The hold ---
FIST = {"index": (0.2, 0.3, 0.2), "middle": (1.1, 1.2, 0.7), "ring": (1.2, 1.2, 0.7), "pinky": (1.2, 1.2, 0.7)}
WRAP = {"index": (0.65, 1.15, 0.85), "middle": (0.7, 1.2, 0.85), "ring": (0.75, 1.2, 0.85), "pinky": (0.8, 1.15, 0.85)}
# The firing hand round the grip, as on the AK's, the index finger on the trigger.
GRIP = Hand(Vector((0.004, -0.095, -0.03)), Vector((-1.0, 0.4, 0.0)), Vector((-0.3, 0.9, -0.35)), FIST,
            thumb=(0.5, 0.6, 0.4), contact=1.0, spin=20.0, yaw=20.0, together=0.85,
            trigger=Vector((0.0, -0.007, -0.018)), thumb_aim=Vector((-0.5, 0.5, -0.7)))
# The support hand cradling the forend from below and the left.
SUPPORT = Hand(Vector((0.03, 0.28, 0.029)), Vector((0.5, 0.0, 1.0)), Vector((1.0, 0.15, -0.3)), WRAP,
               thumb=(0.1, 0.2, 0.1), contact=1.0, spin=12.0, thumb_aim=Vector((0.0, 1.0, 0.1)))
POLES = {"r": Vector((0.45, 0.05, -0.55)), "l": Vector((-0.35, 0.25, -0.6))}

HIP = Place(Vector((0.14, 0.3, -0.205)), pitch=1.5, yaw=4.0, roll=-4.0)
# Aimed: the scope's axis through the eye, its eyepiece a hand's breadth ahead.
AIM = Place(Vector((0.0, 0.22, -0.097)))


def held(place, **kw):
    return Pose(place, GRIP, SUPPORT, **kw)


def mix_vector(a, b, s):
    return a.lerp(b, s)


def lin(a, b, s):
    return a + (b - a) * s


# The right hand on the bolt's knob: over it from the right, fingers round it,
# following it as it turns and slides.
PINCH = {"index": (0.9, 1.1, 0.7), "middle": (1.0, 1.15, 0.7), "ring": (1.1, 1.15, 0.7), "pinky": (1.15, 1.1, 0.7)}
KNOB_OFFSET = Vector((0.012, -0.004, 0.016))


def on_knob(lift, back, curl=PINCH):
    m = bolt_matrix(lift, back)
    turn = m.to_3x3()
    return Hand(m @ (KNOB + KNOB_OFFSET), turn @ Vector((-0.6, 0.0, -0.8)), turn @ Vector((0.1, 1.0, -0.1)), curl,
                thumb=(0.4, 0.5, 0.4), contact=1.0, spin=15.0, yaw=20.0)


OFF_KNOB = Hand(Vector((0.07, -0.05, 0.05)), Vector((-0.6, 0.0, -0.8)), Vector((0.1, 1.0, -0.1)),
                {k: (0.4, 0.5, 0.3) for k in PINCH}, thumb=(0.3, 0.4, 0.3))


# --- Clips ---

def idle(t):
    b = math.sin(t / 3.2 * 2.0 * math.pi)
    return held(HIP.moved(Vector((0.0, 0.0, 0.003 * b)), pitch=0.6 * b, roll=0.4 * b))


def aim(t):
    """Behind the scope: breathing, barely moving."""
    b = math.sin(t / 3.2 * 2.0 * math.pi)
    return held(AIM.moved(Vector((0.0, 0.0, 0.0008 * b)), pitch=0.1 * b), shoulders={"l": 46.0, "r": 8.0})


def fire(t):
    trigger = track([(0.0, 0.0), (0.01, 1.0), (0.08, 1.0), (0.14, 0.0)], t, lin)
    return held(HIP, trigger=trigger)


def bolt_pose(t, lift, back, place, right):
    return Pose(place, right, SUPPORT, bolt=back, parts={"lift": lift},
                poles={"r": track([(0.0, POLES["r"]), (0.15, Vector((0.55, -0.05, -0.1))),
                                   (0.7, Vector((0.55, -0.05, -0.1))), (0.85, POLES["r"])], t, mix_vector)})


def bolt(t):
    """Working the bolt after a shot: the hand off the grip onto the knob, up,
    back (the case flies), home, down, and back to the grip. The rifle rolls a
    little towards the hand and settles."""
    lift = track([(0.0, 0.0), (0.16, 0.0), (0.24, 1.0, ease_out), (0.52, 1.0), (0.6, 0.0, ease_in)], t, lin)
    back = track([(0.0, 0.0), (0.25, 0.0), (0.36, BOLT_TRAVEL, ease_out), (0.42, BOLT_TRAVEL),
                  (0.5, 0.0, ease_in)], t, lin)
    tilt = HIP.moved(Vector((0.0, 0.0, 0.01)), roll=-12.0, yaw=-3.0)
    place = track([(0.0, HIP), (0.15, tilt, smooth), (0.36, tilt.moved(Vector((0.0, -0.006, 0.0)), pitch=1.5)),
                   (0.5, tilt.moved(Vector((0.0, 0.006, 0.0)), pitch=-1.0), ease_out), (0.62, tilt, smooth),
                   (0.85, HIP, smooth)], t, mix_place)
    if t < 0.08:
        right = mix_hand(GRIP, OFF_KNOB, smooth(t / 0.08))
    elif t < 0.15:
        right = mix_hand(OFF_KNOB, on_knob(0.0, 0.0), ease_out((t - 0.08) / 0.07))
    elif t < 0.64:
        right = on_knob(lift, back)
    elif t < 0.72:
        right = mix_hand(on_knob(0.0, 0.0), OFF_KNOB, smooth((t - 0.64) / 0.08))
    else:
        right = mix_hand(OFF_KNOB, GRIP, ease_out((t - 0.72) / 0.13))
    return bolt_pose(t, lift, back, place, right)


RELOAD_TILT = Place(Vector((0.12, 0.27, -0.16)), pitch=6.0, yaw=4.0, roll=-25.0)
# A round from the pouch: the hand comes up under the open action with it and the
# thumb presses it down into the magazine.
ROUND = {"index": (0.6, 0.8, 0.5), "middle": (0.9, 1.0, 0.6), "ring": (1.1, 1.1, 0.7), "pinky": (1.2, 1.1, 0.7)}
POUCH = Hand(Vector((0.12, -0.05, -0.25)), Vector((-0.5, 0.0, 0.85)), Vector((0.0, 1.0, 0.2)), ROUND,
             thumb=(0.4, 0.5, 0.3))
BELOW_PORT = Hand(Vector((0.05, 0.05, 0.06)), Vector((-0.6, 0.0, -0.8)), Vector((0.0, 1.0, 0.0)), ROUND,
                  thumb=(0.4, 0.5, 0.3))
PRESS = Hand(Vector((0.03, 0.07, 0.065)), Vector((-0.6, 0.0, -0.8)), Vector((0.0, 1.0, 0.0)), ROUND,
             thumb=(0.8, 0.7, 0.5))


def reload_start(t):
    """The rifle cants to show the action; the bolt opens; the hand goes for a round."""
    lift = track([(0.0, 0.0), (0.22, 0.0), (0.3, 1.0, ease_out)], t, lin)
    back = track([(0.0, 0.0), (0.3, 0.0), (0.42, BOLT_TRAVEL, ease_out)], t, lin)
    place = track([(0.0, HIP), (0.3, RELOAD_TILT, smooth)], t, mix_place)
    if t < 0.1:
        right = mix_hand(GRIP, OFF_KNOB, smooth(t / 0.1))
    elif t < 0.2:
        right = mix_hand(OFF_KNOB, on_knob(0.0, 0.0), ease_out((t - 0.1) / 0.1))
    elif t < 0.45:
        right = on_knob(lift, back)
    else:
        right = mix_hand(on_knob(1.0, BOLT_TRAVEL), POUCH, smooth((t - 0.45) / 0.25))
    return Pose(place, right, SUPPORT, bolt=back, parts={"lift": lift},
                poles={"r": Vector((0.55, -0.05, -0.2))})


def reload_round(t):
    """One round: up from the pouch, pressed into the open action, back down."""
    right = track([(0.0, POUCH), (0.2, BELOW_PORT, ease_out), (0.3, PRESS, smooth), (0.38, PRESS),
                   (0.6, POUCH, ease_in)], t, mix_hand)
    loading = track([(0.0, 0.0), (0.2, 0.0), (0.33, 1.0, ease_in)], t, lin)
    place = track([(0.0, RELOAD_TILT), (0.32, RELOAD_TILT.moved(Vector((0.0, 0.0, -0.004)), pitch=-1.0), ease_out),
                   (0.45, RELOAD_TILT, smooth)], t, mix_place)
    parts = {"lift": 1.0, "round": loading} if t < 0.34 else {"lift": 1.0}
    return Pose(place, right, SUPPORT, bolt=BOLT_TRAVEL, parts=parts, poles={"r": Vector((0.55, -0.05, -0.2))})


def reload_end(t):
    """Back to the knob, the bolt run home and turned down, the hand to the grip."""
    lift = track([(0.0, 1.0), (0.3, 1.0), (0.38, 0.0, ease_in)], t, lin)
    back = track([(0.0, BOLT_TRAVEL), (0.18, BOLT_TRAVEL), (0.28, 0.0, ease_in)], t, lin)
    place = track([(0.0, RELOAD_TILT), (0.4, RELOAD_TILT, smooth), (0.7, HIP, smooth)], t, mix_place)
    if t < 0.16:
        right = mix_hand(POUCH, on_knob(1.0, BOLT_TRAVEL), ease_out(t / 0.16))
    elif t < 0.42:
        right = on_knob(lift, back)
    else:
        right = mix_hand(on_knob(0.0, 0.0), GRIP, smooth((t - 0.42) / 0.28))
    return Pose(place, right, SUPPORT, bolt=back, parts={"lift": lift},
                poles={"r": track([(0.0, Vector((0.55, -0.05, -0.2))), (0.4, Vector((0.55, -0.05, -0.1))),
                                   (0.7, POLES["r"])], t, mix_vector)})


def draw(t):
    low = Place(Vector((0.26, 0.06, -0.55)), pitch=-50.0, yaw=35.0, roll=65.0)
    place = track([(0.0, low), (0.5, HIP.moved(Vector((-0.01, 0.01, 0.012)), pitch=5.0, roll=-6.0), ease_out),
                   (0.85, HIP, smooth)], t, mix_place)
    return held(place)


def sprint(t):
    phase = t / 0.62 * 2.0 * math.pi
    base = Place(Vector((0.11, 0.2, -0.24)), pitch=-12.0, yaw=30.0, roll=-24.0)
    return held(base.moved(Vector((0.018 * math.sin(phase), 0.0, 0.016 * abs(math.sin(phase)) - 0.008)),
                           pitch=2.5 * math.sin(2.0 * phase), roll=4.0 * math.sin(phase)),
                poles={"l": Vector((-0.2, 0.1, -0.7)), "r": Vector((0.3, -0.05, -0.7))})


def inspect(t):
    left_side = Place(Vector((0.04, 0.26, -0.15)), pitch=10.0, yaw=-12.0, roll=60.0)
    right_side = Place(Vector((0.09, 0.25, -0.16)), pitch=8.0, yaw=30.0, roll=-50.0)
    place = track([(0.0, HIP), (0.7, left_side, smooth), (1.9, left_side.moved(Vector((0.0, 0.01, 0.006)), roll=5.0),
                                                          smooth),
                   (2.6, right_side, smooth), (3.6, right_side.moved(Vector((0.0, 0.0, 0.006)), pitch=3.0), smooth),
                   (4.3, HIP, smooth)], t, mix_place)
    lp = POLES["l"]
    left_pole = track([(0.0, lp), (0.7, Vector((-0.45, 0.2, -0.25))), (1.9, Vector((-0.45, 0.2, -0.25))),
                       (2.6, Vector((-0.3, 0.2, -0.75))), (3.6, Vector((-0.3, 0.2, -0.75))), (4.3, lp)], t, mix_vector)
    return held(place, poles={"l": left_pole})


CLIPS = [
    ("idle", 3.2, idle, True, []),
    ("aim", 3.2, aim, True, []),
    ("fire", 0.2, fire, False, []),
    ("bolt", 0.85, bolt, False, [(0.2, "bolt_up"), (0.33, "bolt_back"), (0.48, "bolt_forward"), (0.58, "bolt_down")]),
    ("draw", 0.85, draw, False, [(0.0, "draw")]),
    ("reload_start", 0.7, reload_start, False, [(0.27, "bolt_up"), (0.4, "bolt_back")]),
    ("reload_round", 0.6, reload_round, False, [(0.32, "round_in")]),
    ("reload_end", 0.7, reload_end, False, [(0.26, "bolt_forward"), (0.37, "bolt_down")]),
    ("sprint", 0.62, sprint, True, []),
    ("inspect", 4.3, inspect, False, [(0.2, "grab"), (2.0, "grab")]),
]

PREVIEW = {"bolt": (0.2, 0.36, 0.55), "reload_start": (0.4,), "reload_round": (0.2, 0.3), "reload_end": (0.3,),
           "draw": (0.3,), "sprint": (0.15,), "inspect": (1.2, 3.0)}


# The grenade thrown with the left hand (throwing.py).
from weapons.throwing import make_throw  # noqa: E402

CLIPS.append(make_throw(HIP, GRIP, SUPPORT, lambda place, right, left, **kw: Pose(place, right, left, **kw)))
PREVIEW["throw"] = (0.15, 0.3, 0.4, 0.5, 0.7)
