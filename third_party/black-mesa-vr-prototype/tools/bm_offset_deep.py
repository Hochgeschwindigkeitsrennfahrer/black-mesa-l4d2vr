#!/usr/bin/env python3
"""Deep verification: RTTI vtables, string xrefs, CreateMove, AEV, PushRT, VGui_Paint."""
from __future__ import annotations

import struct
from pathlib import Path
from typing import List, Optional, Tuple

# reuse helpers by import
import sys
sys.path.insert(0, str(Path(__file__).parent))
from bm_offset_scan import (
    MODULES, PE, parse_pe, find_all, find_bytes, hexdump, walk_back_to_prologue,
    dump_rtti_names, parse_sig,
)

def read_u32(pe: PE, off: int) -> int:
    return struct.unpack_from("<I", pe.data, off)[0]


def find_col_and_vtable(pe: PE, type_desc_off: int) -> List[Tuple[int, int]]:
    """MSVC PE32: COL has pTypeDescriptor at +0x0C. Vtable is ptr to COL-followed-by-funcs... 
    Actually: complete object locator is referenced FROM vtable[-1].
    COL layout: signature, offset, cdOffset, pTypeDescriptor, pClassDescriptor
    Search for dword == type_desc VA at COL+0xC, then find refs to COL VA; those are vtable-4.
    """
    rva = pe.off_to_rva(type_desc_off)
    if rva is None:
        return []
    td_va = pe.image_base + rva
    results = []
    # find COL candidates: dword at position matching type descriptor
    for hit in find_bytes(pe.data, struct.pack("<I", td_va), limit=30):
        # likely at COL+0x0C
        col_off = hit - 0x0C
        if col_off < 0:
            continue
        # COL signature usually 0 for PE32
        sig = read_u32(pe.data, col_off) if col_off + 4 <= len(pe.data) else 0xFFFFFFFF
        if sig not in (0, 1):  # 0 = PE32, 1 = PE32+
            # still try
            pass
        col_rva = pe.off_to_rva(col_off)
        if col_rva is None:
            continue
        col_va = pe.image_base + col_rva
        for vref in find_bytes(pe.data, struct.pack("<I", col_va), limit=10):
            # vref points at slot before first virtfunc => vtable starts at vref+4
            vt_off = vref + 4
            results.append((col_off, vt_off))
    return results


def dump_vtable(pe: PE, vt_off: int, count: int = 40, label: str = "") -> None:
    print(f"  vtable {label} @ +0x{vt_off:X}:")
    for i in range(count):
        o = vt_off + i * 4
        if o + 4 > len(pe.data):
            break
        va = read_u32(pe.data, o)
        fo = pe.va_to_off(va)
        if fo is None:
            print(f"    [{i:02d}] VA=0x{va:X} (no map)")
            continue
        print(f"    [{i:02d}] VA=0x{va:X} file+0x{fo:X}  {hexdump(pe.data, fo, 24)}")


def xref_string(pe: PE, needle: bytes, max_xrefs: int = 12) -> None:
    print(f"\n=== STRING {needle!r} ===")
    for so in find_bytes(pe.data, needle + b"\x00", limit=5) or find_bytes(pe.data, needle, limit=5):
        rva = pe.off_to_rva(so)
        if rva is None:
            continue
        va = pe.image_base + rva
        print(f"  str @ +0x{so:X} VA=0x{va:X} ctx={bytes(pe.data[so:so+48])!r}")
        xrefs = find_bytes(pe.data, struct.pack("<I", va), limit=max_xrefs)
        print(f"  xrefs ({len(xrefs)}): " + ", ".join(f"+0x{x:X}" for x in xrefs))
        for xo in xrefs[:max_xrefs]:
            prol = walk_back_to_prologue(pe.data, xo, 0x400)
            # also show surrounding
            print(f"    xref+0x{xo:X} bytes={hexdump(pe.data, max(0,xo-8), 32)}")
            if prol is not None:
                # measure rough size to next CC CC or int3 pad
                end = prol
                for j in range(prol + 4, min(len(pe.data) - 1, prol + 0x8000)):
                    if pe.data[j] == 0xCC and pe.data[j + 1] == 0xCC:
                        end = j
                        break
                else:
                    end = prol + 0x40
                print(f"      prologue +0x{prol:X} ~size 0x{end-prol:X}: {hexdump(pe.data, prol, 40)}")


def main() -> int:
    pes = {n: parse_pe(p) for n, p in MODULES.items() if p.exists()}

    # --- CViewRender vtable ---
    pe = pes["client.dll"]
    print("=== CViewRender RTTI / vtable ===")
    for off, name in dump_rtti_names(pe, b"CViewRender@@", limit=5):
        if b"Beams" in name.encode() if False else "Beams" in name:
            continue
        print(f"TD {name} @ +0x{off:X}")
        for col, vt in find_col_and_vtable(pe, off):
            print(f"  COL +0x{col:X} VT +0x{vt:X}")
            dump_vtable(pe, vt, 50, "CViewRender")

    # Also search CViewRender::Render string (not Shadows)
    for needle in [b"CViewRender::Render\x00", b"CViewRender::RenderView", b"CViewRender::SetupRender"]:
        xref_string(pe, needle.rstrip(b"\x00"))

    # Render at string +0x4643E4 from prior scan
    xref_string(pe, b"CViewRender::Render")

    # Look for large functions matching stereo render patterns:
    # typical: many stack locals, references to view origin/angles offsets
    print("\n=== Large RenderView-like prologues (81 EC big stack) near view strings ===")
    # Search: 55 8B EC 81 EC ?? ?? 00 00 53 56 57
    hits = find_all(pe.data, "55 8B EC 81 EC ? ? 00 00 53 56 57", limit=40)
    print(f"big-frame prologues: {len(hits)}")
    for h in hits:
        size_imm = read_u32(pe.data, h + 4) & 0xFFFF  # low 16 of sub esp imm? actually 81 EC imm32
        imm = read_u32(pe.data, h + 2 + 2)  # after 81 EC
        # Actually bytes: 55 8B EC 81 EC xx xx xx xx
        imm = read_u32(pe.data, h + 4)
        if imm < 0x100 or imm > 0x2000:
            continue
        print(f"  +0x{h:X} stack=0x{imm:X} {hexdump(pe.data, h, 32)}")

    # Portal2-style: 55 8B EC 83 EC 2C 53 56 8B F1
    # BMS tiny at 76E10 - disasm more carefully
    print("\n=== Disasm context: candidate RenderView 0x76E10 (FULL) ===")
    print(hexdump(pe.data, 0x76E10, 0x80))
    # What string is at AEV's push 0x1041B648?
    aev_str_va = 0x1041B648
    aev_str_off = pe.va_to_off(aev_str_va)
    if aev_str_off:
        print(f"AEV pushed string VA 0x{aev_str_va:X} -> {pe.read_cstr(aev_str_off)}")

    # g_pClientMode absolute
    gpcm_insn = 0x16A150
    print(f"\n=== g_pClientMode site ===")
    print(hexdump(pe.data, gpcm_insn - 0x10, 0x40))
    ptr_va = read_u32(pe.data, 0x16A152)  # imm of 8B 0D xx xx xx xx
    print(f"g_pClientMode static VA from MOV = 0x{ptr_va:X} file={pe.va_to_off(ptr_va)}")

    # Find all "8B 0D xx xx xx xx 8B 01 FF 50" near ClientMode usage with AdjustEngineViewport-like
    # ClientModeShared::AdjustEngineViewport in SDK is often a real method - find via vtable
    print("\n=== ClientModeShared / ClientModeBlackMesaNormal vtables ===")
    for off, name in dump_rtti_names(pe, b"ClientMode", limit=10):
        print(f"TD {name} @ +0x{off:X}")
        pairs = find_col_and_vtable(pe, off)
        for col, vt in pairs[:2]:
            print(f"  COL +0x{col:X} VT +0x{vt:X}")
            dump_vtable(pe, vt, 55, name)

    # CreateMove patterns - look at ClientMode vtable slot historically ~index for CreateMove
    # In Source SDK IClientMode, CreateMove is around the middle of the vtable

    # CalcViewModelView - find C_BaseViewModel / CBaseViewModel RTTI
    print("\n=== ViewModel RTTI ===")
    for off, name in dump_rtti_names(pe, b"ViewModel", limit=20):
        print(f"  {name} @ +0x{off:X}")
    for off, name in dump_rtti_names(pe, b"C_BaseViewModel", limit=10):
        print(f"  {name} @ +0x{off:X}")
        for col, vt in find_col_and_vtable(pe, off)[:1]:
            dump_vtable(pe, vt, 80, name)

    # Check candidate CalcViewModelView 0xE490 size and args
    print("\n=== CalcViewModelView candidate 0xE490 ===")
    print(hexdump(pe.data, 0xE490, 0x80))
    # find end
    for j in range(0xE490 + 4, 0xE490 + 0x800):
        if pe.data[j] == 0xCC and pe.data[j+1] == 0xCC:
            print(f"  ends ~ +0x{j:X} size=0x{j-0xE490:X}")
            # look for retn
            chunk = bytes(pe.data[0xE490:j])
            for i, b in enumerate(chunk):
                if b == 0xC2:
                    print(f"  retn @ +0x{0xE490+i:X}: {hexdump(pe.data, 0xE490+i, 3)}")
                if b == 0xC3 and i > 0x20:
                    print(f"  ret @ +0x{0xE490+i:X}")
            break

    # Nearby functions at E750, EA40 also matched BMS CalcViewModel sig - compare
    for off in [0xE490, 0xE750, 0xEA40, 0xF670]:
        print(f"\n  cand +0x{off:X}: {hexdump(pe.data, off, 48)}")

    # --- engine CEngineVGui::Paint / CModelRender ---
    eng = pes["engine.dll"]
    xref_string(eng, b"CEngineVGui::Paint")
    print("\n=== CEngineVGui / CModelRender vtables ===")
    for off, name in dump_rtti_names(eng, b"CEngineVGui@@", limit=3):
        print(f"TD {name}")
        for col, vt in find_col_and_vtable(eng, off)[:2]:
            dump_vtable(eng, vt, 40, name)
    for off, name in dump_rtti_names(eng, b"CModelRender@@", limit=3):
        print(f"TD {name}")
        for col, vt in find_col_and_vtable(eng, off)[:2]:
            dump_vtable(eng, vt, 30, name)

    # DrawModelExecute - check size / uniqueness of nearby
    print("\n=== DrawModelExecute 0xF5E20 size ===")
    for j in range(0xF5E20 + 0x10, 0xF5E20 + 0x5000):
        if eng.data[j] == 0xCC and eng.data[j+1] == 0xCC:
            print(f"  ~size 0x{j-0xF5E20:X}")
            break
    print(hexdump(eng.data, 0xF5E20, 64))

    # VGui_Paint candidate - compare with Paint string xrefs
    print("\n=== VGui_Paint candidate 0x115CE0 ===")
    print(hexdump(eng.data, 0x115CE0, 64))
    for j in range(0x115CE0 + 0x10, 0x115CE0 + 0x2000):
        if eng.data[j] == 0xCC and eng.data[j+1] == 0xCC:
            print(f"  ~size 0x{j-0x115CE0:X}")
            # find retn
            break

    # Search better VGui_Paint: often takes PaintMode_t
    for sig in [
        "55 8B EC 83 EC ? 56 8B 75 08",
        "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 08 56",
        "55 8B EC 8B 0D ? ? ? ? 56 8B 75 08",
    ]:
        hits = find_all(eng.data, sig, limit=10)
        print(f"VGui sig {sig[:50]}: {[hex(h) for h in hits[:6]]}")

    # --- materialsystem CMatRenderContextBase vtable ---
    mat = pes["materialsystem.dll"]
    print("\n=== CMatRenderContextBase vtable (Push/Pop/Get RT) ===")
    for off, name in dump_rtti_names(mat, b"CMatRenderContextBase@@", limit=3):
        print(f"TD {name} @ +0x{off:X}")
        for col, vt in find_col_and_vtable(mat, off)[:2]:
            dump_vtable(mat, vt, 120, name)

    # Also CMatRenderContext if exists
    for off, name in dump_rtti_names(mat, b"CMatRenderContext@@", limit=5):
        print(f"TD {name} @ +0x{off:X}")
        for col, vt in find_col_and_vtable(mat, off)[:1]:
            dump_vtable(mat, vt, 120, name)

    # Search PushRT by looking for patterns that manipulate +0x4C stack
    print("\n=== PushRT heuristic: functions that INC [this+4C] ===")
    # FF 46 4C or FF 41 4C or 83 46 4C 01
    for sig, label in [
        ("FF 46 4C", "inc [esi+4C]"),
        ("FF 41 4C", "inc [ecx+4C]"),
        ("83 46 4C 01", "add [esi+4C],1"),
        ("FF 86 4C 00 00 00", "inc [esi+4C] long"),
    ]:
        hits = find_all(mat.data, sig, limit=20)
        print(f"  {label}: {[hex(h) for h in hits]}")
        for h in hits[:8]:
            prol = walk_back_to_prologue(mat.data, h, 0x100)
            if prol:
                print(f"    @+0x{h:X} prol+0x{prol:X}: {hexdump(mat.data, prol, 32)}")

    # Viewport: look for SetViewport style - 4 int params
    # GetViewport near GetRenderTarget (0x67C20) - next funcs
    print("\n=== Functions near GetRenderTarget 0x67C20 ===")
    # scan backwards/forwards for CC-padded prologues
    region = range(0x67C00, 0x69800)
    funcs = []
    for i in region:
        if mat.data[i] == 0xCC:
            continue
        if i > 0 and mat.data[i-1] == 0xCC and (mat.data[i] == 0x55 or mat.data[i] == 0x56 or mat.data[i] == 0x8B or mat.data[i] == 0x83):
            funcs.append(i)
    # dedupe close
    last = -100
    for f in funcs:
        if f - last < 8:
            continue
        last = f
        print(f"  fn +0x{f:X}: {hexdump(mat.data, f, 28)}")

    # --- server EyePosition: which of 3 full matches? Check CBaseEntity vtable ---
    srv = pes["server.dll"]
    print("\n=== CBaseEntity EyePosition via vtable ===")
    for off, name in dump_rtti_names(srv, b"CBaseEntity@@", limit=3):
        if name != ".?AVCBaseEntity@@":
            continue
        print(f"TD {name}")
        for col, vt in find_col_and_vtable(srv, off)[:2]:
            dump_vtable(srv, vt, 100, name)

    # Compare three EyePosition hits
    for off in [0x317260, 0x3DDBE0, 0x4B4840]:
        print(f"\nEyePos +0x{off:X}: {hexdump(srv.data, off, 48)}")
        for j in range(off + 8, off + 0x200):
            if srv.data[j] == 0xCC and srv.data[j+1] == 0xCC:
                print(f"  size 0x{j-off:X}")
                break

    # EyePosition string xrefs
    xref_string(srv, b"EyePosition")

    # game.cpp interface versions check on disk
    print("\n=== Extra iface strings ===")
    for mod in ["materialsystem.dll", "vguimatsurface.dll"]:
        blob = bytes(pes[mod].data)
        for pref in [b"VMaterialSystem", b"VGUI_Surface"]:
            i = 0
            while True:
                j = blob.find(pref, i)
                if j < 0:
                    break
                s = pes[mod].read_cstr(j)
                if s:
                    print(f"  {mod}: {s}")
                i = j + 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
