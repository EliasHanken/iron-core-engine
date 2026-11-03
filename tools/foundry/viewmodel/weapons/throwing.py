"""A grenade thrown with the left hand, for every gun: the gun dips to the right
in the firing hand while the support hand leaves it, takes a grenade from the
chest, cocks it back past the shoulder, throws it overhand, follows through
and comes back to the gun. The game draws the grenade in the hand from "grab"
until "release", and lets the real one fly from there.

Hands are authored in the gun's space like every clip; the throwing hand's path
is set relative to the eye (through the gun's place), so it's the same throw
whichever gun is held.
"""
import math

from mathutils import Vector

from clips import Hand, Pose, ease_in, ease_out, mix_hand, mix_place, smooth, track

GRENADE = {"index": (0.9, 1.0, 0.7), "middle": (1.0, 1.1, 0.75), "ring": (1.05, 1.1, 0.75),
           "pinky": (1.1, 1.05, 0.7)}
OPEN = {k: (0.15, 0.2, 0.1) for k in GRENADE}
LENGTH = 1.05
RELEASE = 0.5


def make_throw(hip, grip, support, held_pose):
    """A "throw" clip for a gun held at `hip` by `grip` and `support`; `held_pose`
    builds that gun's Pose from (place, right, left, **kw)."""
    dipped = hip.moved(Vector((0.07, -0.03, -0.09)), pitch=-10.0, yaw=-10.0, roll=-22.0)

    def at_eye(place, socket, palm, forward, curl, thumb=(0.5, 0.5, 0.35)):
        # A hand placed relative to the eye, written in the gun's space.
        inv = place.matrix().inverted()
        turn = inv.to_3x3()
        return Hand(inv @ Vector(socket), turn @ Vector(palm), turn @ Vector(forward), curl, thumb=thumb)

    def left_at(t, place):
        chest = at_eye(place, (-0.12, 0.2, -0.32), (0.3, -0.3, 0.9), (0.4, 0.8, 0.2), GRENADE)
        cocked = at_eye(place, (-0.26, 0.1, -0.08), (0.5, 0.85, -0.1), (0.1, 0.1, 1.0), GRENADE)
        thrown = at_eye(place, (-0.12, 0.45, 0.02), (0.2, 0.8, -0.5), (0.1, 0.9, 0.4), OPEN, thumb=(0.15, 0.2, 0.1))
        follow = at_eye(place, (-0.05, 0.38, -0.3), (0.3, 0.3, -0.9), (0.1, 0.9, -0.4), OPEN, thumb=(0.15, 0.2, 0.1))
        keys = [(0.0, support), (0.12, chest, ease_out), (0.22, chest), (0.4, cocked, smooth),
                (RELEASE, thrown, ease_in), (0.7, follow, ease_out), (LENGTH, support, smooth)]
        return track(keys, t, mix_hand)

    def throw(t):
        place = track([(0.0, hip), (0.12, dipped, ease_out), (0.45, dipped.moved(Vector((0.0, 0.0, -0.01)),
                                                                                  roll=-4.0), smooth),
                       (0.55, dipped.moved(Vector((0.0, -0.01, -0.02)), pitch=-3.0), ease_out),
                       (LENGTH, hip, smooth)], t, mix_place)
        return held_pose(place, grip, left_at(t, place), shoulders={"l": 30.0 + 25.0 * math.sin(
            min(t / RELEASE, 1.0) * math.pi), "r": 8.0})

    return ("throw", LENGTH, throw, False, [(0.06, "grab"), (0.2, "pin"), (RELEASE, "release")])
