"""Mantling onto a waist-high ledge (about a metre): palms planted, pushed up onto it."""
from weapons.mantle import *  # noqa: F401,F403
from weapons.mantle import build

HEIGHT = 1.0
globals().update(build("mantle_low", HEIGHT))
