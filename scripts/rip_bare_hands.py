#!/usr/bin/env python3
"""Split HL2VR player_alt.mdl (bare citizen hands) into independent L/R GLBs.

Same pipeline as rip_hev_gloves.py: wrist+finger ValveBiped subtrees, meters,
SteamVR joint names so BMVR's summary-curl palette still works. Used before
the HEV suit is equipped.
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rip_hev_gloves as hev

ROOT = hev.ROOT
SRC = os.path.join(ROOT, "assets", "bare hands")
MDL = os.path.join(SRC, "player.mdl")
VVD = os.path.join(SRC, "player.vvd")
VTX = os.path.join(SRC, "player.dx90.vtx")
HL2VR_VTF = os.path.join(
    r"C:\Program Files (x86)\Steam\steamapps\common\Half-Life 2 VR",
    "hlvr",
    "materials",
    "models",
    "player",
    "player_alt.vtf",
)


def resolve_vtf() -> str:
    local = os.path.join(SRC, "player_alt.vtf")
    nested = os.path.join(SRC, "materials", "models", "player", "player_alt.vtf")
    for p in (local, nested, HL2VR_VTF):
        if os.path.isfile(p):
            return p
    return local


def main():
    vtf = resolve_vtf()
    return hev.run_rip(
        MDL,
        VVD,
        VTX,
        vtf,
        [0],
        "bare_hand",
        "player_alt",
        "rip_bare_hands.py",
        1024,
        1024,
    )


if __name__ == "__main__":
    sys.exit(main())
