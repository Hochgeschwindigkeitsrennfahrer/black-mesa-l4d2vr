#!/usr/bin/env python3
"""Map IClientMode / ClientModeShared / ClientModeBlackMesaNormal vtables offline.

Anchors on known CreateMove (0x110310) and AdjustEngineViewport (0x1102C0),
walks back to COL, assigns Source-like slot names from BMSVR/sdk/sdk.h IClientMode.
Also: OverrideView hunt, CalcViewModelView owner, engine BackgroundMaps.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path
from typing import List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).parent))
from bm_offset_scan import MODULES, PE, hexdump, parse_pe, walk_back_to_prologue  # noqa: E402
from bm_offset_deep import dump_rtti_names, find_col_and_vtable, read_u32  # noqa: E402

# Source IClientMode order (PE32 MSVC: slot 0 = scalar deleting dtor).
# Matches BMSVR/sdk/sdk.h — BM may insert slots after CanRecordDemo.
ICLIENTMODE_NAMES = [
    "dtor",  # 0
    "InitViewport",  # 1
    "Init",  # 2
    "VGui_Shutdown",  # 3
    "Shutdown",  # 4
    "Enable",  # 5
    "Disable",  # 6
    "Layout",  # 7
    "GetViewport",  # 8
    "GetViewportAnimationController",  # 9
    "ProcessInput",  # 10
    "ShouldDrawDetailObjects",  # 11
    "ShouldDrawEntity",  # 12
    "ShouldDrawLocalPlayer",  # 13
    "ShouldDrawParticles",  # 14
    "ShouldDrawFog",  # 15
    "OverrideView",  # 16
    "KeyInput",  # 17
    "StartMessageMode",  # 18
    "GetMessagePanel",  # 19
    "OverrideMouseInput",  # 20
    "CreateMove",  # 21
    "LevelInit",  # 22
    "LevelShutdown",  # 23
    "ShouldDrawViewModel",  # 24
    "ShouldDrawCrosshair",  # 25
    "AdjustEngineViewport",  # 26
    "PreRender",  # 27
    "PostRender",  # 28
    "PostRenderVGui",  # 29
    "ActivateInGameVGuiContext",  # 30
    "DeactivateInGameVGuiContext",  # 31
    "GetViewModelFOV",  # 32
    "CanRecordDemo",  # 33
    "ComputeVguiResConditions",  # 34
    "GetServerName",  # 35
    "SetServerName",  # 36
    "GetMapName",  # 37
    "SetMapName",  # 38
    "DoPostScreenSpaceEffects",  # 39
    "DisplayReplayMessage",  # 40
    "Update",  # 41
    "ShouldBlackoutAroundHUD",  # 42
    "ShouldOverrideHeadtrackControl",  # 43
]


def is_code_ptr(pe: PE, va: int) -> bool:
    fo = pe.va_to_off(va)
    if fo is None:
        return False
    # executable sections typically .text
    rva = pe.off_to_rva(fo)
    if rva is None:
        return False
    for s in pe.sections:
        if s.name.startswith(".text") and s.virt_addr <= rva < s.virt_addr + max(s.virt_size, s.raw_size):
            return True
    # also accept any mapped RVA with plausible prologue
    b0 = pe.data[fo]
    return b0 in (0x55, 0x56, 0x53, 0x57, 0x8B, 0x83, 0xC2, 0xC3, 0xB0, 0xB8, 0x33, 0xE9, 0xEB, 0xA1)


def is_col_ptr(pe: PE, va: int) -> bool:
    """COL lives in .rdata; signature dword 0 or 1, type-desc ptr in-module."""
    fo = pe.va_to_off(va)
    if fo is None or fo + 0x14 > len(pe.data):
        return False
    sig = read_u32(pe, fo)
    if sig not in (0, 1):
        return False
    td_va = read_u32(pe, fo + 0x0C)
    return pe.va_to_off(td_va) is not None


def find_vtable_start(pe: PE, known_slot_file_off: int, known_slot_index: int) -> Optional[int]:
    """Given file offset of a known slot and its expected index, verify COL at vt-4."""
    vt = known_slot_file_off - known_slot_index * 4
    if vt < 4:
        return None
    col_va = read_u32(pe, vt - 4)
    if is_col_ptr(pe, col_va):
        return vt
    # fallback: scan backward for COL
    for back in range(0, 80):
        cand = known_slot_file_off - back * 4
        if cand < 4:
            break
        col_va = read_u32(pe, cand - 4)
        if not is_col_ptr(pe, col_va):
            continue
        # next slots should be code
        ok = True
        for i in range(8):
            va = read_u32(pe, cand + i * 4)
            if not is_code_ptr(pe, va):
                ok = False
                break
        if ok:
            return cand
    return None


def slot_rva(pe: PE, vt_off: int, idx: int) -> Optional[int]:
    o = vt_off + idx * 4
    if o + 4 > len(pe.data):
        return None
    va = read_u32(pe, o)
    fo = pe.va_to_off(va)
    if fo is None:
        return None
    return pe.off_to_rva(fo)


def dump_named_vtable(pe: PE, vt_off: int, label: str, count: int = 55) -> List[Tuple[int, Optional[int], str]]:
    print(f"\n=== {label} vtable @ file+0x{vt_off:X} RVA=0x{pe.off_to_rva(vt_off):X} ===")
    col_va = read_u32(pe, vt_off - 4)
    col_fo = pe.va_to_off(col_va)
    td_name = "?"
    if col_fo is not None:
        td_va = read_u32(pe, col_fo + 0x0C)
        td_fo = pe.va_to_off(td_va)
        if td_fo is not None:
            # type descriptor: name at +0x08
            td_name = pe.read_cstr(td_fo + 8) or "?"
    print(f"  COL VA=0x{col_va:X} type={td_name}")
    rows = []
    for i in range(count):
        rva = slot_rva(pe, vt_off, i)
        name = ICLIENTMODE_NAMES[i] if i < len(ICLIENTMODE_NAMES) else f"slot_{i}"
        if rva is None:
            va = read_u32(pe, vt_off + i * 4)
            print(f"  [{i:02d}] {name:32} VA=0x{va:08X} (unmapped)")
            rows.append((i, None, name))
            continue
        fo = pe.rva_to_off(rva)
        print(f"  [{i:02d}] {name:32} RVA=0x{rva:06X}  {hexdump(pe.data, fo, 20)}")
        rows.append((i, rva, name))
    return rows


def compare_vtables(shared: List[Tuple[int, Optional[int], str]], bm: List[Tuple[int, Optional[int], str]]):
    print("\n=== Shared vs BlackMesaNormal diffs ===")
    n = min(len(shared), len(bm))
    for i in range(n):
        si, sr, sn = shared[i]
        bi, br, bn = bm[i]
        if sr != br:
            ss = f"0x{sr:X}" if sr is not None else "None"
            bs = f"0x{br:X}" if br is not None else "None"
            print(f"  [{i:02d}] {sn}: Shared={ss}  BM={bs}")


def analyze_func(pe: PE, rva: int, label: str, peek: int = 48):
    fo = pe.rva_to_off(rva)
    if fo is None:
        print(f"  {label} RVA=0x{rva:X} UNMAPPED")
        return
    print(f"  {label} RVA=0x{rva:X}: {hexdump(pe.data, fo, peek)}")
    # first retn
    ch = bytes(pe.data[fo : fo + 0x800])
    for i in range(8, len(ch) - 2):
        if ch[i] == 0xC2 and ch[i + 2] == 0x00 and ch[i - 1] in (0x5D, 0xC9, 0x5E, 0x5F, 0x5B, 0xC3):
            print(f"    retn 0x{ch[i+1]:X} at +0x{i:X}")
            break
        if ch[i] == 0xC3 and i > 0x10 and ch[i - 1] in (0x5D, 0xC9, 0x5E, 0x5F, 0x5B):
            print(f"    ret at +0x{i:X}")
            break


def find_rtti_vtable(pe: PE, needle: bytes) -> List[Tuple[str, int, int]]:
    out = []
    for off, name in dump_rtti_names(pe, needle, limit=8):
        for col, vt in find_col_and_vtable(pe, off)[:3]:
            out.append((name, col, vt))
    return out


def xref_string(pe: PE, needle: bytes, max_xrefs: int = 10):
    blob = bytes(pe.data)
    idx = blob.find(needle + b"\x00")
    if idx < 0:
        idx = blob.find(needle)
    if idx < 0:
        print(f"  MISS {needle!r}")
        return
    rva = pe.off_to_rva(idx)
    va = pe.image_base + rva
    print(f"  STR {needle!r} RVA=0x{rva:X} VA=0x{va:X}")
    needle_va = struct.pack("<I", va)
    start = 0
    hits = []
    while len(hits) < max_xrefs:
        i = blob.find(needle_va, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
    print(f"    xrefs ({len(hits)}): " + ", ".join(f"file+0x{x:X}" for x in hits))
    for xo in hits[:max_xrefs]:
        prol = walk_back_to_prologue(pe.data, xo, 0x600)
        print(f"    xref file+0x{xo:X} ctx={hexdump(pe.data, max(0, xo - 8), 28)}")
        if prol is not None:
            print(f"      prol RVA=0x{pe.off_to_rva(prol):X} {hexdump(pe.data, prol, 36)}")


def resolve_type_at_col(pe: PE, col_va: int) -> str:
    fo = pe.va_to_off(col_va)
    if fo is None:
        return "?"
    td_va = read_u32(pe, fo + 0x0C)
    td_fo = pe.va_to_off(td_va)
    if td_fo is None:
        return "?"
    return pe.read_cstr(td_fo + 8) or "?"


def main() -> int:
    client = parse_pe(MODULES["client.dll"])
    engine = parse_pe(MODULES["engine.dll"])

    create_move_va = client.image_base + 0x110310
    aev_va = client.image_base + 0x1102C0
    cm_needle = struct.pack("<I", create_move_va)
    aev_needle = struct.pack("<I", aev_va)
    raw = bytes(client.data)

    cm_hits = []
    start = 0
    while True:
        i = raw.find(cm_needle, start)
        if i < 0:
            break
        cm_hits.append(i)
        start = i + 1

    aev_hits = []
    start = 0
    while True:
        i = raw.find(aev_needle, start)
        if i < 0:
            break
        aev_hits.append(i)
        start = i + 1

    print(f"CreateMove VA hits: {[hex(h) for h in cm_hits]}")
    print(f"AEV VA hits: {[hex(h) for h in aev_hits]}")

    # Expected: CreateMove at IClientMode index 21, AEV at 26 -> delta 5*4=20
    CREATEMOVE_IDX = 21
    AEV_IDX = 26

    shared_vt = None
    bm_vt = None

    for fo in cm_hits:
        vt = find_vtable_start(client, fo, CREATEMOVE_IDX)
        if vt is None:
            print(f"  CreateMove @ file+0x{fo:X}: could not find COL start")
            continue
        col_va = read_u32(client, vt - 4)
        tname = resolve_type_at_col(client, col_va)
        print(f"  CreateMove @ file+0x{fo:X} -> vt file+0x{vt:X} type={tname}")
        if "Shared" in tname:
            shared_vt = vt
        elif "BlackMesa" in tname or "Normal" in tname:
            bm_vt = vt

    for fo in aev_hits:
        vt = find_vtable_start(client, fo, AEV_IDX)
        if vt is None:
            # try relative: AEV should be CreateMove+5
            continue
        col_va = read_u32(client, vt - 4)
        tname = resolve_type_at_col(client, col_va)
        print(f"  AEV @ file+0x{fo:X} -> vt file+0x{vt:X} type={tname}")
        if "Shared" in tname and shared_vt is None:
            shared_vt = vt
        if ("BlackMesa" in tname or "Normal" in tname) and bm_vt is None:
            bm_vt = vt

    # RTTI fallback
    print("\n=== RTTI ClientMode* ===")
    for needle in (b"ClientModeShared", b"ClientModeBlackMesaNormal", b"IClientMode"):
        for name, col, vt in find_rtti_vtable(client, needle):
            print(f"  {name} COL+0x{col:X} VT+0x{vt:X}")
            if "Shared" in name and "BlackMesa" not in name:
                shared_vt = shared_vt or vt
            if "BlackMesaNormal" in name:
                bm_vt = bm_vt or vt

    if shared_vt is None and cm_hits:
        # force: CreateMove is index 21
        shared_vt = cm_hits[0] - CREATEMOVE_IDX * 4
        print(f"  FALLBACK Shared vt = file+0x{shared_vt:X}")

    if bm_vt is None and len(aev_hits) >= 2:
        # second AEV hit is BM (first is Shared)
        bm_vt = aev_hits[1] - AEV_IDX * 4
        print(f"  FALLBACK BM vt = file+0x{bm_vt:X}")

    shared_rows = dump_named_vtable(client, shared_vt, "ClientModeShared") if shared_vt else []
    bm_rows = dump_named_vtable(client, bm_vt, "ClientModeBlackMesaNormal") if bm_vt else []
    if shared_rows and bm_rows:
        compare_vtables(shared_rows, bm_rows)

    print("\n=== OverrideView analysis ===")
    for label, rows in (("Shared", shared_rows), ("BM", bm_rows)):
        if len(rows) > 16 and rows[16][1] is not None:
            analyze_func(client, rows[16][1], f"{label} OverrideView")
    # BM CreateMove / LevelInit / LevelShutdown
    print("\n=== BM key overrides ===")
    for idx, name in ((16, "OverrideView"), (21, "CreateMove"), (22, "LevelInit"), (23, "LevelShutdown"),
                      (25, "ShouldDrawCrosshair"), (32, "GetViewModelFOV")):
        if bm_rows and idx < len(bm_rows) and bm_rows[idx][1] is not None:
            analyze_func(client, bm_rows[idx][1], f"BM {name}")
        if shared_rows and idx < len(shared_rows) and shared_rows[idx][1] is not None:
            analyze_func(client, shared_rows[idx][1], f"Shared {name}")

    print("\n=== CalcViewModelView vtable owner ===")
    cvm_va = client.image_base + 0xF090
    cvm_needle = struct.pack("<I", cvm_va)
    start = 0
    while True:
        i = raw.find(cvm_needle, start)
        if i < 0:
            break
        # scan back for COL
        vt = None
        for back in range(0, 120):
            cand = i - back * 4
            if cand < 4:
                break
            col_va = read_u32(client, cand - 4)
            if is_col_ptr(client, col_va):
                # verify consecutive code slots
                ok = sum(1 for k in range(6) if is_code_ptr(client, read_u32(client, cand + k * 4))) >= 5
                if ok:
                    vt = cand
                    break
        slot = (i - vt) // 4 if vt else None
        tname = resolve_type_at_col(client, read_u32(client, vt - 4)) if vt else "?"
        print(f"  hit file+0x{i:X} vt={('file+0x%X' % vt) if vt else '?'} slot={slot} type={tname}")
        if vt is not None:
            for s in range(max(0, slot - 3), slot + 5):
                rva = slot_rva(client, vt, s)
                mark = " <<" if s == slot else ""
                print(f"    [{s:02d}] RVA=0x{rva:X}{mark}")
        start = i + 1

    # Also dump C_BaseViewModel RTTI
    print("\n=== RTTI *ViewModel* ===")
    for off, name in dump_rtti_names(client, b"ViewModel", limit=15):
        print(f"  {name} @ +0x{off:X}")
        if b"C_BaseViewModel" in name.encode() or "C_BaseViewModel" in name:
            for col, vt in find_col_and_vtable(client, off)[:1]:
                # find which slot is 0xF090
                for si in range(80):
                    rva = slot_rva(client, vt, si)
                    if rva == 0xF090:
                        print(f"    CalcViewModelView at slot [{si}] on {name}")
                        break

    print("\n=== Engine BackgroundMaps / level helpers ===")
    for s in (
        b"BackgroundMaps",
        b"Backgrounds.txt",
        b"IsLevelMainMenuBackground",
        b"m_bLevelMainMenuBackground",
        b"GetLevelNameShort",
        b"GetLevelName",
        b"IsDrawingLoadingImage",
        b"mapname",
    ):
        xref_string(engine, s, max_xrefs=6)

    # Client LevelInit string wrappers (known not on ClientMode vt)
    print("\n=== Client LevelInitPre/PostEntity (entity system, not ClientMode) ===")
    for s in (b"LevelInitPreEntity", b"LevelInitPostEntity"):
        xref_string(client, s, max_xrefs=4)

    # Summary candidates
    print("\n=== CANDIDATE SUMMARY ===")
    if bm_rows:
        for idx in (16, 21, 22, 23, 25, 26, 32):
            if idx < len(bm_rows) and bm_rows[idx][1] is not None:
                print(f"  BM [{idx}] {bm_rows[idx][2]} = 0x{bm_rows[idx][1]:X}")
    if shared_rows:
        for idx in (16, 21, 22, 23, 26, 32):
            if idx < len(shared_rows) and shared_rows[idx][1] is not None:
                print(f"  Shared [{idx}] {shared_rows[idx][2]} = 0x{shared_rows[idx][1]:X}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
