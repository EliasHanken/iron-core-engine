"""Vaulting, at a run: a low, thin obstacle (a barrier, a crate) taken in one
motion instead of climbed. The left palm plants on its top, the body swings over
leaning on it, and drops down the far side still running. Built like the mantles
(mantle.py): the obstacle is a prop only for the hand to land on; the game plays
the clip against the real one and carries the run on after.

Space: the obstacle's own, +Y over it, +Z up, the origin on its near top edge.
"""
import math

from mathutils import Vector

from clips import Hand, Pose, ease_in, ease_out, mix_hand, smooth, track
from weapons.door import Viewer, mix_viewer
from weapons.mantle import EYE, make_ledge

NAME = "vault"
PARTS = []
MAGAZINE = None
EYE_FOV = 60.0
HEIGHT = 1.0   # Its top above the feet.
DEPTH = 0.5    # Front to back.
LENGTH = 0.62


def load(pack):
    del pack
    return make_ledge(HEIGHT, DEPTH)


def move_parts(pose, weapon, p):
    del pose, weapon, p


PLANT = {k: (0.3, 0.4, 0.25) for k in ("index", "middle", "ring", "pinky")}
LOOSE = {k: (0.4, 0.5, 0.3) for k in PLANT}
# The left palm on the top, a little left of the line, fingers turned to the right
# as the body swings past it.
PLANTED = Hand(Vector((-0.16, 0.14, 0.03)), Vector((0.0, 0.0, -1.0)), Vector((0.45, 1.0, 0.0)), PLANT,
               thumb=(0.25, 0.3, 0.2), contact=1.0, spin=20.0, yaw=30.0)

START = Viewer((0.0, -1.0, EYE - HEIGHT), pitch=-14.0)
END = Viewer((0.0, DEPTH + 1.05, EYE - HEIGHT), pitch=-3.0)


def viewer_at(t):
    """Fast over: up as the hand lands, leaning over it (rolled towards the hand),
    a dip on landing, and on at a run."""
    keys = [(0.0, START),
            (0.13, Viewer((0.0, -0.42, EYE - HEIGHT + 0.1), pitch=-34.0, roll=-3.0), smooth),
            (0.3, Viewer((0.12, 0.2, EYE - HEIGHT + 0.26), pitch=-24.0, roll=-9.0, yaw=3.0), smooth),
            (0.45, Viewer((0.06, DEPTH + 0.45, EYE - HEIGHT - 0.12), pitch=-9.0, roll=-2.0), ease_in),  # Landing.
            (LENGTH, END, ease_out)]
    return track(keys, t, mix_viewer)


def from_eye(viewer, side, up=False):
    x = 0.3 if side == "r" else -0.3
    m = viewer.camera()
    turn = m.to_3x3()
    # Down at the side, or (the free hand over the obstacle) out for balance.
    socket = (x, 0.12, -0.62) if not up else (x * 1.1, 0.25, -0.35)
    palm = (-1.0 if side == "r" else 1.0, 0.1, 0.15) if not up else (-0.6, 0.3, -0.7)
    forward = (0.0, 0.35, -0.95) if not up else (0.4, 0.8, -0.3)
    return Hand(m @ Vector(socket), turn @ Vector(palm), turn @ Vector(forward), LOOSE, thumb=(0.3, 0.4, 0.3))


def vault(t):
    v = viewer_at(t)
    away_l = from_eye(v, "l")
    if t < 0.11:
        left = mix_hand(away_l, PLANTED, ease_out(t / 0.11))
    elif t < 0.34:
        left = PLANTED
    else:
        left = mix_hand(PLANTED, away_l, ease_in(min((t - 0.34) / 0.2, 1.0)))
    # The right arm swings out for balance as the body goes over.
    swing = math.sin(min(t / LENGTH, 1.0) * math.pi)
    right = mix_hand(from_eye(v, "r"), from_eye(v, "r", up=True), smooth(swing))
    return Pose(v, right, left, shoulders={"l": 28.0, "r": 12.0},
                poles={"r": Vector((0.5, -0.1, -0.4)), "l": Vector((-0.5, -0.2, -0.3))})


def standing(viewer):
    def still(t):
        del t
        return Pose(viewer, from_eye(viewer, "r"), from_eye(viewer, "l"))
    return still


CLIPS = [
    ("before", 0.2, standing(START), True, []),
    ("vault", LENGTH, vault, False, [(0.11, "plant"), (0.45, "step"), (0.58, "step")]),
    ("after", 0.2, standing(END), True, []),
]

HIP = START
AIM = START
GRIP = PLANTED
SUPPORT = PLANTED
POLES = {"r": Vector((0.5, -0.1, -0.4)), "l": Vector((-0.5, -0.2, -0.3))}


def held(place, **kw):
    return Pose(place, PLANTED, PLANTED, **kw)


PREVIEW = {"vault": (0.1, 0.22, 0.32, 0.45)}
