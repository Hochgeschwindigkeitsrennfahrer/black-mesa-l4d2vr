#!/usr/bin/env python3
"""Verify BMSVR offsets.h against on-disk Black Mesa DLLs; hunt new hook targets."""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from bm_offset_scan import MODULES, find_all, hexdump, parse_pe, parse_sig  # noqa: E402

CHECKS = [
    ("client.dll", "RenderView", 0x207730, "55 8B EC 83 EC 08 A1 ? ? ? ? 53 8B D9 89 45 F8"),
    ("client.dll", "g_pClientMode", 0x16AD50, "56 57 8B F9 8B 0D ? ? ? ? 8B 01 FF 50 24"),
    ("client.dll", "CreateMove", 0x110310, "55 8B EC E8 ? ? ? ? 8B C8 85 C9 75 06 B0 01 5D C2 08 00"),
    ("client.dll", "CalcViewModelView", 0x29D930, "55 8B EC 83 EC 24 53 56 8B 75 08 57 8B F9 85 F6"),
    ("client.dll", "CalcViewModelView_Shared", 0x7CF60, "55 8B EC 83 EC 24 8B 55 10 56 57 8B F9 8B 4D 0C"),
    ("client.dll", "AdjustEngineViewport", 0x1102C0, "C2 10 00 CC CC CC CC CC CC CC CC CC CC CC CC CC B0 01 C2 08 00"),
    ("engine.dll", "DrawModelExecute", 0xF6A20, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 56 8B 75 08 57 8B"),
    ("engine.dll", "VGui_Paint", 0x238C50, "55 8B EC 83 EC 18 53 8B D9 8B 0D ? ? ? ? FF 15"),
    ("materialsystem.dll", "GetRenderTarget", 0x68820, "83 79 4C 00 7E 0E 8B 41 4C 8D 14 C0"),
    ("materialsystem.dll", "GetViewport", 0x68A70, "55 8B EC 8B 41 4C 56 8D 14 C0 8B 41 40 83 7C 90 F8 00"),
    ("materialsystem.dll", "Viewport", 0x69F30, "55 8B EC 56 FF 75 14 8B F1 FF 75 10 FF 75 0C FF 75 08 E8"),
    ("materialsystem.dll", "PushRT", 0x6A3D0, "55 8B EC 83 EC 24 8B 45 08 89 45 DC 8B 45 0C 89 45 EC"),
    ("materialsystem.dll", "PopRT", 0x6A250, "56 8B F1 83 7E 4C 00 74 15 8B 06 6A 00 FF 50 10 FF 4E 4C"),
    ("server.dll", "EyePosition", 0x317E60, "55 8B EC 56 8B F1 8B 86 04 01 00 00 C1 E8 0B A8 01 74 05 E8"),
    ("server.dll", "ProcessUsercmds", 0x5320F0, "55 8B EC B8 ? ? ? ? E8 ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 0C 8B 55 08"),
]

# Offline RE candidates (not hooked) — prologue must still match on disk
CANDIDATES = [
    ("client.dll", "Shared_LevelInit", 0x110A80, "55 8B EC 83 EC 20 56 8B F1 6A 01 68 ? ? ? ?"),
    ("client.dll", "Shared_LevelShutdown", 0x110B30, "55 8B EC 83 EC 20 56 8B F1 B9 ? ? ? ? E8"),
    ("client.dll", "Shared_OverrideView", 0x110BE0, "55 8B EC 83 EC 4C E8 ? ? ? ? 85 C0 0F 84"),
    ("client.dll", "BM_LevelInit", 0x216B90, "55 8B EC 53 56 57 FF 75 08 8B D9 E8 ? ? ? ? BF 04 00 00 00 8D B3 FC 04 00 00"),
    ("client.dll", "BM_LevelShutdown", 0x216C60, "53 56 57 8B D9 E8 ? ? ? ? BF 04 00 00 00"),
    ("client.dll", "BM_CreateMove", 0x216130, "55 8B EC FF 75 0C D9 45 08 51 D9 1C 24 E8"),
    ("client.dll", "BM_OverrideView", 0x216EB0, "55 8B EC 5D E9 27 9D EF FF CC CC CC CC CC CC CC"),
    ("client.dll", "BM_GetViewModelFOV", 0x216510, "55 8B EC 51 8B 0D ? ? ? ? 81 F9 ? ? ? ? 75 16 F3 0F 10 0D"),
    ("client.dll", "Shared_GetViewModelFOV", 0x110490, "55 8B EC 51 8B 0D ? ? ? ? 81 F9 ? ? ? ? 75 1B F3 0F 10 05"),
    ("client.dll", "BM_GetMapName", 0x2164B0, "8D 81 24 02 00 00 C3"),
]

# Legacy L4D leftovers — expected MISSING on BM (kept here only to document)
OPTIONAL_MISSING = [
    ("client.dll", "GetMeleeWeaponInfoClient", 0x30B570, "8B 81 ? ? ? ? 50 B9 ? ? ? ? E8 ? ? ? ? C3"),
    ("server.dll", "GetActiveWeapon", 0x464F0, "55 8B EC 8B 45 0C 56 8B 75 08 50 56 E8 ? ? ? ? 84 C0 74 47 8B"),
    ("client.dll", "WriteUsercmdDeltaToBuffer", 0x134790, "55 8B EC 83 EC 60 0F 57 C0 8B 55 0C"),
    ("client.dll", "WriteUsercmd", 0x1AAD50, "55 8B EC A1 ? ? ? ? 83 78 30 00 53 8B 5D 10 56 57"),
    ("server.dll", "ReadUserCmd", 0x205100, "55 8B EC 53 8B 5D 10 56 57 8B 7D 0C 53"),
]


def match_at(data, off, sig: str) -> bool:
    pat, mask = parse_sig(sig)
    if off is None or off < 0 or off + len(pat) > len(data):
        return False
    for i in range(len(pat)):
        if mask[i] and data[off + i] != pat[i]:
            return False
    return True


def main() -> int:
    pes = {n: parse_pe(p) for n, p in MODULES.items() if p.exists()}
    print("=== VERIFY offsets.h against on-disk DLLs ===")
    bad = []
    for row in CHECKS:
        mod, name, rva, sig = row[0], row[1], row[2], row[3]
        pe = pes[mod]
        fo = pe.rva_to_off(rva)
        sig_hits = find_all(pe.data, sig, limit=8)
        ok = match_at(pe.data, fo, sig)
        hit_rvas = []
        for h in sig_hits[:5]:
            hr = pe.off_to_rva(h)
            hit_rvas.append(f"0x{hr:X}" if hr is not None else f"file+0x{h:X}")
        status = "OK" if ok else "MISMATCH"
        if not ok:
            bad.append(name)
        print(
            f"{status:8} {name:28} {mod:20} RVA=0x{rva:X} "
            f"file={('0x%X' % fo) if fo is not None else 'NONE'} "
            f"sig_hits={hit_rvas}"
        )
        if not ok and sig_hits:
            h0 = sig_hits[0]
            print(f"         -> first hit: {hexdump(pe.data, h0, 32)}")
            hr0 = pe.off_to_rva(h0)
            if hr0 is not None and hr0 != rva:
                print(f"         -> SUGGEST RVA 0x{hr0:X}")

    print("\n=== CalcViewModelView stack args (retn) ===")
    pe = pes["client.dll"]
    fo = pe.rva_to_off(0x29D930)
    chunk = bytes(pe.data[fo : fo + 0x300])
    found = False
    for i in range(0x40, len(chunk) - 2):
        if chunk[i : i + 3] == b"\xC2\x0C\x00" and chunk[i - 1] in (0x5D, 0xC9):
            print(
                f"  BM retn 0x0C at +0x{i:X} "
                f"(stack=0x0C => 3 thiscall stack args + this)"
            )
            found = True
            break
    if not found:
        for i in range(len(chunk) - 2):
            if chunk[i] == 0xC2 and chunk[i + 2] == 0x00 and chunk[i - 1] in (0x5D, 0xC9):
                print(
                    f"  retn 0x{chunk[i+1]:X} at +0x{i:X} "
                    f"(stack={chunk[i+1]} => ~{(chunk[i+1]//4)+1} thiscall args incl this)"
                )
                break

    print("\n=== Useful strings (future hooks) ===")
    needles = [
        b"IsLevelMainMenuBackground",
        b"GetLevelNameShort",
        b"GetLevelName",
        b"IsDrawingLoadingImage",
        b"ClientModeBlackMesaNormal",
        b"OverrideView",
        b"GetViewModelFOV",
        b"LevelInitPreEntity",
        b"LevelInitPostEntity",
        b"CHudCrosshair",
        b"DrawCrosshair",
        b"FireBullets",
        b"ProcessUsercmds",
        b"WriteUsercmdDeltaToBuffer",
    ]
    for mod, pe in pes.items():
        blob = bytes(pe.data)
        for n in needles:
            idx = blob.find(n + b"\x00")
            if idx < 0:
                idx = blob.find(n)
            if idx >= 0:
                rva = pe.off_to_rva(idx)
                print(f"  {mod:20} {n.decode('ascii', 'ignore'):32} RVA=0x{rva:X}")

    print("\n=== CreateMove uniqueness ===")
    pe = pes["client.dll"]
    hits = find_all(
        pe.data,
        "55 8B EC E8 ? ? ? ? 8B C8 85 C9 75 06 B0 01 5D C2 08 00",
        limit=20,
    )
    print("  hits=" + ", ".join(f"0x{pe.off_to_rva(h):X}" for h in hits))

    try:
        from bm_offset_deep import dump_rtti_names, find_col_and_vtable, dump_vtable

        print("\n=== CViewRender vtable (first 24) ===")
        for off, name in dump_rtti_names(pe, b"CViewRender@@", limit=3):
            print(f"  type {name} @ +0x{off:X}")
            for col, vt in find_col_and_vtable(pe, off)[:1]:
                dump_vtable(pe, vt, 24, label=f"COL+0x{col:X}")
    except Exception as e:
        print(f"  (vtable dump skipped: {e})")

    print("\n=== Candidate offsets (not hooked) ===")
    cand_bad = []
    for mod, name, rva, sig in CANDIDATES:
        pe = pes[mod]
        fo = pe.rva_to_off(rva)
        ok = match_at(pe.data, fo, sig)
        status = "OK" if ok else "MISMATCH"
        if not ok:
            cand_bad.append(name)
        print(f"{status:8} {name:28} RVA=0x{rva:X}")

    print("\n=== Optional L4D leftovers (expect miss) ===")
    for mod, name, rva, sig in OPTIONAL_MISSING:
        pe = pes[mod]
        hits = find_all(pe.data, sig, limit=3)
        print(f"  {name:28} hits={len(hits)}")

    print("\n=== SUMMARY ===")
    if bad:
        print("CRITICAL FAIL:", ", ".join(bad))
        return 1
    if cand_bad:
        print("CANDIDATE MISMATCH:", ", ".join(cand_bad))
        return 1
    print("All critical offsets.h signatures match on-disk binaries.")
    print("All documented candidate RVAs match prologues.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
