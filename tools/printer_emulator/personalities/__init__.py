"""Printer personalities. Each one = a state model + a request router + a command interpreter.

E1: the generic Moonraker/Klipper personality and the QIDI one on top of it. E2: the Elegoo
Centauri Carbon 1 (SDCP, CANVAS, camera) and the Snapmaker U1. E3: Creality, Flashforge, OctoPrint
and PrusaLink (design section 7).
"""
from .elegoo_cc1 import ElegooCC1
from .moonraker import Moonraker
from .qidi import Qidi
from .u1 import U1

PERSONALITIES = {
    "moonraker": Moonraker,
    "qidi": Qidi,
    "elegoo_cc1": ElegooCC1,
    "u1": U1,
}


def personality_class(name):
    cls = PERSONALITIES.get(name)
    if cls is None:
        raise ValueError("unknown personality '%s' (this emulator has: %s)" % (name, ", ".join(sorted(PERSONALITIES))))
    return cls
