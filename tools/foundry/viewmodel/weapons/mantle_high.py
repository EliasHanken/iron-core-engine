"""Mantling onto a head-high ledge (about 1.6 m): reached for, hauled up over the hands."""
from weapons.mantle import *  # noqa: F401,F403
from weapons.mantle import build

HEIGHT = 1.6
globals().update(build("mantle_high", HEIGHT))
