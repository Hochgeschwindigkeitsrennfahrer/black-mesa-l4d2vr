#!/usr/bin/env python3
"""Locate vtables containing known function RVAs; dump ClientMode / ViewRender slots."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from bm_offset_scan import MODULES, hexdump, parse_pe  # noqa: E402


def find_vtables_containing(pe, func_rva: int, label: str, slots_before: int = 16, slots_after: int = 32):
    func_va = pe.image_base + func_rva
    needle = struct.pack("<I", func_va)
    raw = bytes(pe.data)
    hits = []
    start = 0
    while True:
        i = raw.find(needle, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
        if len(hits) >= 30:
            break

    print(f"\n=== refs to {label} RVA=0x{func_rva:X} VA=0x{func_va:X} ({len(hits)} hits) ===")
    for fo in hits:
        # treat fo as a vtable slot
        slot0 = fo - slots_before * 4
        if slot0 < 0:
            continue
        # heuristic: previous dword looks like COL or another code ptr in module
        print(f"  slot @ file+0x{fo:X} (as index {slots_before} if vtable starts -{slots_before})")
        print(f"  dump [{slots_before} before .. {slots_after} after]:")
        for s in range(-slots_before, slots_after + 1):
            o = fo + s * 4
            if o < 0 or o + 4 > len(raw):
                continue
            va = struct.unpack_from("<I", raw, o)[0]
            fr = pe.va_to_off(va)
            mark = " <<" if s == 0 else ""
            if fr is None:
                print(f"    [{s:+03d}] VA=0x{va:08X} (data/outside){mark}")
            else:
                rva = pe.off_to_rva(fr)
                print(f"    [{s:+03d}] RVA=0x{rva:X}  {hexdump(pe.data, fr, 16)}{mark}")


def main() -> int:
    client = parse_pe(MODULES["client.dll"])
    engine = parse_pe(MODULES["engine.dll"])

    # Full named ClientMode maps: tools/bm_clientmode_map.py
    # CalcViewModelView owner / BackgroundMaps: tools/bm_cvm_bgmaps.py

    # Known good hooks
    find_vtables_containing(client, 0x110310, "CreateMove", 20, 40)
    find_vtables_containing(client, 0x207730, "RenderView", 12, 20)
    find_vtables_containing(client, 0xF090, "CalcViewModelView", 8, 8)
    find_vtables_containing(client, 0x1102C0, "AdjustEngineViewport stub", 8, 16)

    # LevelInit candidates from string xrefs
    find_vtables_containing(client, 0x16E840, "LevelInitPreEntity xref prol", 8, 16)
    find_vtables_containing(client, 0x16E7D0, "LevelInitPostEntity xref prol", 8, 16)

    # Engine VGui_Paint / DME
    find_vtables_containing(engine, 0x238C50, "VGui_Paint", 8, 12)
    find_vtables_containing(engine, 0xF6A20, "DrawModelExecute", 8, 12)

    # Scan for "background" / menu map related in engine more loosely
    print("\n=== engine loose string hits ===")
    blob = bytes(engine.data)
    for n in (
        b"background",
        b"Background",
        b"mainmenu",
        b"MainMenu",
        b"loading",
        b"Loading",
        b"level name",
        b"LevelName",
        b"m_bLoadGame",
        b"IsMapValid",
    ):
        idx = blob.find(n)
        if idx >= 0:
            print(f"  {n!r} @ RVA 0x{engine.off_to_rva(idx):X} ctx={blob[idx:idx+40]!r}")

    print("\n=== client loose string hits ===")
    blob = bytes(client.data)
    for n in (
        b"OverrideView",
        b"override view",
        b"GetViewModelFOV",
        b"viewmodel_fov",
        b"ShouldDrawViewModel",
        b"CalcDefaultViewAngles",
        b"GetDeathMessageStartHeight",
    ):
        idx = blob.find(n)
        if idx >= 0:
            print(f"  {n!r} @ RVA 0x{client.off_to_rva(idx):X}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
