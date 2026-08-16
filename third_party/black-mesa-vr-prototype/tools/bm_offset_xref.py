#!/usr/bin/env python3
"""Xref view-render strings to find real RenderView / SetUpView / CreateMove / etc."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from bm_offset_scan import (
    MODULES, parse_pe, find_bytes, hexdump, walk_back_to_prologue, find_all, dump_rtti_names
)


def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def func_size(data, prol, limit=0x20000):
    for j in range(prol + 8, min(len(data) - 1, prol + limit)):
        if data[j] == 0xCC and data[j + 1] == 0xCC:
            return j - prol
    return -1


def xref_string(pe, needle: bytes, max_back=0x10000):
    print(f"\n==== {needle!r} ====")
    # exact cstring
    offs = find_bytes(pe.data, needle + b"\x00", limit=8)
    if not offs:
        offs = find_bytes(pe.data, needle, limit=5)
    if not offs:
        print("  not found")
        return set()
    prol_set = set()
    for so in offs[:3]:
        rva = pe.off_to_rva(so)
        if rva is None:
            continue
        va = pe.image_base + rva
        # show string
        s = pe.read_cstr(so, 80)
        print(f"  str +0x{so:X} VA=0x{va:X} {s!r}")
        xrefs = find_bytes(pe.data, struct.pack("<I", va), limit=30)
        print(f"  xrefs ({len(xrefs)}): {[hex(x) for x in xrefs[:20]]}")
        for xo in xrefs:
            p = walk_back_to_prologue(pe.data, xo, max_back)
            if p is None:
                print(f"    xref+0x{xo:X} no prologue  {hexdump(pe.data, max(0,xo-4), 24)}")
                continue
            sz = func_size(pe.data, p)
            prol_set.add(p)
            print(f"    xref+0x{xo:X} -> +0x{p:X} size=0x{sz:X}  {hexdump(pe.data, p, 36)}")
    return prol_set


def dump_fn(pe, off, n=64):
    print(f"\n-- fn +0x{off:X} size=0x{func_size(pe.data, off):X}")
    print(hexdump(pe.data, off, n))
    # find retn
    sz = func_size(pe.data, off)
    if sz > 0:
        chunk = bytes(pe.data[off : off + sz])
        for i, b in enumerate(chunk):
            if b == 0xC2 and i + 2 < len(chunk):
                imm = chunk[i + 1] | (chunk[i + 2] << 8)
                # only near end
                if i > sz - 0x40:
                    print(f"  retn 0x{imm:X} at +0x{off+i:X}")
            if b == 0xC3 and i > sz - 0x20:
                print(f"  ret at +0x{off+i:X}")


def main():
    pe = parse_pe(MODULES["client.dll"])
    eng = parse_pe(MODULES["engine.dll"])
    mat = parse_pe(MODULES["materialsystem.dll"])
    srv = parse_pe(MODULES["server.dll"])

    # --- Render path strings ---
    rv_candidates = set()
    for s in [
        b"DrawWorld",
        b"3D Skybox",
        b"DrawTranslucentWorldInLeaves",
        b"DrawOpaqueRenderables",
        b"DrawTranslucentRenderables",
        b"CViewRender::SetUpView",
        b"CViewRender::SetUpView->OnRenderEnd",
        b"CViewRender::Render",
        b"CViewRender::RenderShadows",
        b"dump position and angles to the console",
        b"cl_leveloverview",
    ]:
        rv_candidates |= xref_string(pe, s)

    print("\n*** Unique prologues referenced by view strings ***")
    for p in sorted(rv_candidates):
        dump_fn(pe, p, 48)

    # Search for functions that look like RenderView with 3 or 4 args
    # Pattern from 1F1D40 area - also search callers of that
    print("\n=== Who calls CViewRender::Render @ 0x1F1D40? ===")
    # E8 rel32 call
    target = 0x1F1D40
    callers = []
    for i in range(len(pe.data) - 5):
        if pe.data[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", pe.data, i + 1)[0]
        dest = i + 5 + rel
        if dest == target:
            callers.append(i)
            if len(callers) >= 20:
                break
    print(f"direct E8 callers: {[hex(c) for c in callers]}")
    for c in callers[:10]:
        p = walk_back_to_prologue(pe.data, c, 0x2000)
        print(f"  call@+0x{c:X} prol={hex(p) if p else None} {hexdump(pe.data, c, 8)}")

    # Also search FF 15 / vtable calls - harder

    # --- CreateMove: ClientModeBlackMesaNormal ---
    # Look for string related to createmove
    print("\n=== CreateMove related strings ===")
    for s in [b"CreateMove", b"ClientMode", b"OverrideView", b"AdjustEngineViewport", b"GetViewModelFOV"]:
        offs = find_bytes(pe.data, s + b"\x00", limit=5)
        print(f"  {s!r}: {[hex(o) for o in offs]}")

    # Signature hunt for ClientModeShared::CreateMove from Source:
    # bool CreateMove(float flInputSampleTime, CUserCmd *cmd)
    # Often starts checking cmd null / gpGlobals
    for name, sig in [
        ("CM_check_cmd", "55 8B EC 56 8B 75 0C 85 F6 74 ? 8B 06"),
        ("CM_float_cmd", "55 8B EC 83 EC ? 56 8B 75 0C 57 8B F9 85 F6"),
        ("CM_portal_like", "55 8B EC A1 ? ? ? ? 83 EC ? 83 78 30 00 56 8B 75 0C"),
        ("AEV_cdecl", "55 8B EC 8B 0D ? ? ? ? 85 C9 74 ? 8B 01 FF 50"),
        ("AEV_refs", "55 8B EC 8B 45 08 8B 4D 0C 8B 55 10"),
        ("OverrideView", "55 8B EC 8B 45 08 83 EC ? 56"),
    ]:
        hits = find_all(pe.data, sig, limit=15)
        print(f"  {name}: {[hex(h) for h in hits[:10]]}")

    # g_pClientMode: find assignments / usages with ClientModeBlackMesa
    print("\n=== g_pClientMode pointer usages (8B 0D ... near mode) ===")
    # From known site imm = 0x106D96B0
    gva = u32(pe.data, 0x16A152)
    print(f"pointer VA from known site: 0x{gva:X}")
    g_off = pe.va_to_off(gva)
    print(f"pointer file off: {hex(g_off) if g_off else None}")
    # find all instructions referencing this VA
    refs = find_bytes(pe.data, struct.pack("<I", gva), limit=40)
    print(f"refs to g_pClientMode storage VA: {[hex(r) for r in refs[:30]]}")
    for r in refs[:15]:
        # show if 8B 0D / 89 0D / A1 / A3
        pref = bytes(pe.data[max(0, r - 2) : r])
        print(f"  +0x{r:X} prefix={pref.hex()} ctx={hexdump(pe.data, max(0,r-2), 16)}")

    # --- CalcViewModelView: find via 'viewmodel_offset' or similar ---
    print("\n=== CalcViewModelView strings / patterns ===")
    for s in [b"viewmodel_offset_x", b"viewmodel_offset_y", b"viewmodel_offset_z", b"ViewModel", b"cl_viewmodel"]:
        offs = find_bytes(pe.data, s + b"\x00", limit=5)
        if offs:
            print(f"  {s!r} @ {[hex(o) for o in offs]}")
            for o in offs[:1]:
                rva = pe.off_to_rva(o)
                va = pe.image_base + rva
                xrefs = find_bytes(pe.data, struct.pack("<I", va), limit=15)
                for xo in xrefs[:8]:
                    p = walk_back_to_prologue(pe.data, xo, 0x800)
                    print(f"    xref+0x{xo:X} prol={hex(p) if p else None} {hexdump(pe.data, p or xo, 28)}")

    # Compare E490 with viewmodel_offset xrefs
    dump_fn(pe, 0xE490, 80)

    # --- Engine Paint ---
    print("\n=== Engine CEngineVGui::Paint ===")
    xref_string(eng, b"CEngineVGui::Paint")
    # Also search PaintMode
    for s in [b"CEngineVGui::Paint", b"VRAD ClearBuffers", b"Paint UI"]:
        xref_string(eng, s)

    # VGui_Paint often registered as Con/ exported - look for function taking enum
    # Candidate 115CE0 - check if Paint xrefs land there
    dump_fn(eng, 0x115CE0, 64)
    dump_fn(eng, 0xF5E20, 64)

    # --- materialsystem: functions around GetRT ---
    print("\n=== matsys function list near GetRT/PopRT ===")
    for off in range(0x67B00, 0x69800):
        if off > 0 and mat.data[off - 1] == 0xCC and mat.data[off] in (0x55, 0x56, 0x57, 0x8B, 0x83):
            # skip if previous was also start
            print(f"  +0x{off:X}: {hexdump(mat.data, off, 40)}")

    # PushRT: look for call pattern with texture + viewport ints
    print("\n=== PushRT via 'inc dword [reg+4C]' prologues ===")
    for h in find_all(mat.data, "FF 46 4C", limit=20):
        p = walk_back_to_prologue(mat.data, h, 0x150)
        print(f"  inc@+0x{h:X} prol={hex(p) if p else None}")
        if p:
            print(f"    {hexdump(mat.data, p, 48)}")
    for h in find_all(mat.data, "FF 41 4C", limit=20):
        p = walk_back_to_prologue(mat.data, h, 0x150)
        print(f"  inc ecx@+0x{h:X} prol={hex(p) if p else None}")
        if p:
            print(f"    {hexdump(mat.data, p, 48)}")

    # GetViewport often right after GetRenderTarget in source - check 0x67C40 area
    print("\nGetRT+nearby:")
    print(hexdump(mat.data, 0x67C20, 0x100))
    print("PopRT region:")
    print(hexdump(mat.data, 0x69600, 0x80))

    # Viewport setter - L4D2 sig was 55 8B EC 83 EC 28 8B C1 - BM has 83 EC 18 at 1BCD0
    dump_fn(mat, 0x1BCD0, 48)
    # Is 1BCD0 really Viewport? Check args - reads [this+4], [this+8] vs args - looks like bounds check for something else maybe

    # --- EyePosition: map which hit is CBaseEntity ---
    print("\n=== EyePosition hits disasm ends ===")
    for off in [0x317260, 0x3DDBE0, 0x4B4840]:
        dump_fn(srv, off, 48)
    xref_string(srv, b"EyePosition")

    # Find CBasePlayer::EyePosition via string in comments? 
    # Compare: CBaseEntity EyePosition in SDK returns Vector and uses EyePosition() virtual
    # The __thiscall returning Vector* via hidden ptr (arg0) matches the sig with [ebp+8]

    print("\nDone.")


if __name__ == "__main__":
    main()
