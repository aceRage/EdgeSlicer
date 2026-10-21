"""Printer personalities. Each one = a state model + a request router + a command interpreter.

E1 ships the generic Moonraker/Klipper personality and the QIDI one on top of it. E2 adds Elegoo
SDCP and the Snapmaker U1, E3 Creality, Flashforge, OctoPrint and PrusaLink (design section 7).
"""
from .moonraker import Moonraker
from .qidi import Qidi

PERSONALITIES = {
    "moonraker": Moonraker,
    "qidi": Qidi,
}


def personality_class(name):
    cls = PERSONALITIES.get(name)
    if cls is None:
        raise ValueError("unknown personality '%s' (E1 has: %s)" % (name, ", ".join(sorted(PERSONALITIES))))
    return cls
