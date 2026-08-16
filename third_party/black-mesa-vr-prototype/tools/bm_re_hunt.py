#!/usr/bin/env python3
"""Deep offline RE: ClientMode / LevelInit / OverrideView / engine level helpers."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from bm_offset_scan import (  # noqa: E402
    MODULES,
    find_all,
    find_bytes,
    hexdump,
    parse_pe,
    walk_back_to_prologue,
)
from bm_offset_deep import dump_rtti_names, dump_vtable, find_col_and_vtable, read_u32  # noqa: E402


def xref_str(pe, needle: bytes, max_xrefs: int = 8):
    blob = bytes(pe.data)
    idx = blob.find(needle + b"\x00")
    if idx < 0:
        idx = blob.find(needle)
    if idx < 0:
        print(f"  MISS {needle!r}")
        return
    rva = pe.off_to_rva(idx)
    va = pe.image_base + rva
    print(f"  STR {needle!r} file+0x{idx:X} RVA=0x{rva:X} VA=0x{va:X}")
    xrefs = find_bytes(pe.data, struct.pack("<I", va), limit=max_xrefs)
    print(f"    xrefs ({len(xrefs)}): " + ", ".join(f"+0x{x:X}" for x in xrefs[:max_xrefs]))
    for xo in xrefs[:max_xrefs]:
        prol = walk_back_to_prologue(pe.data, xo, 0x500)
        print(f"    xref+0x{xo:X} ctx={hexdump(pe.data, max(0, xo - 8), 28)}")
        if prol is not None:
            pr = pe.off_to_rva(prol)
            print(f"      prologue RVA=0x{pr:X} bytes={hexdump(pe.data, prol, 36)}")
            # find retn nearby
            ch = bytes(pe.data[prol : prol + 0x600])
            for i in range(0x20, len(ch) - 2):
                if ch[i] == 0xC2 and ch[i + 2] == 0x00 and ch[i - 1] in (0x5D, 0xC9, 0x5E, 0x5F, 0x5B):
                    print(f"      likely retn 0x{ch[i+1]:X} at +0x{i:X}")
                    break


def export_names(pe, substr: bytes):
    # PE export directory
    raw = bytes(pe.data)
    e_lfanew = struct.unpack_from("<I", raw, 0x3C)[0]
    opt = e_lfanew + 4 + 20
    magic = struct.unpack_from("<H", raw, opt)[0]
    if magic != 0x10B:  # PE32
        return []
    # DataDirectory[0] export
    export_rva = struct.unpack_from("<I", raw, opt + 96)[0]
    export_size = struct.unpack_from("<I", raw, opt + 100)[0]
    if not export_rva:
        return []
    eo = pe.rva_to_off(export_rva)
    if eo is None:
        return []
    # IMAGE_EXPORT_DIRECTORY
    name_rva = struct.unpack_from("<I", raw, eo + 12)[0]
    n_names = struct.unpack_from("<I", raw, eo + 24)[0]
    names_rva = struct.unpack_from("<I", raw, eo + 32)[0]
    ords_rva = struct.unpack_from("<I", raw, eo + 36)[0]
    funcs_rva = struct.unpack_from("<I", raw, eo + 28)[0]
    names_off = pe.rva_to_off(names_rva)
    ords_off = pe.rva_to_off(ords_rva)
    funcs_off = pe.rva_to_off(funcs_rva)
    out = []
    for i in range(n_names):
        nr = struct.unpack_from("<I", raw, names_off + i * 4)[0]
        no = pe.rva_to_off(nr)
        if no is None:
            continue
        name = pe.read_cstr(no) or ""
        if substr.decode().lower() in name.lower():
            ord_ = struct.unpack_from("<H", raw, ords_off + i * 2)[0]
            fr = struct.unpack_from("<I", raw, funcs_off + ord_ * 4)[0]
            out.append((name, fr))
    return out


def main() -> int:
    pes = {n: parse_pe(p) for n, p in MODULES.items() if p.exists()}
    client = pes["client.dll"]
    engine = pes["engine.dll"]
    server = pes["server.dll"]

    print("=== RTTI ClientMode* ===")
    for needle in (b"ClientModeBlackMesa", b"ClientModeShared", b"IClientMode"):
        for off, name in dump_rtti_names(client, needle, limit=8):
            print(f"  {name} @ +0x{off:X}")
            for col, vt in find_col_and_vtable(client, off)[:1]:
                dump_vtable(client, vt, 48, label=name)

    print("\n=== RTTI CViewRender ===")
    for off, name in dump_rtti_names(client, b"CViewRender@@", limit=2):
        print(f"  {name} @ +0x{off:X}")
        for col, vt in find_col_and_vtable(client, off)[:1]:
            dump_vtable(client, vt, 40, label="CViewRender")

    print("\n=== String xrefs (client) ===")
    for s in (
        b"LevelInitPreEntity",
        b"LevelInitPostEntity",
        b"OverrideView",
        b"GetViewModelFOV",
        b"ClientModeBlackMesaNormal",
        b"CHudCrosshair",
        b"DrawCrosshair",
    ):
        xref_str(client, s)

    print("\n=== String xrefs (engine) ===")
    for s in (
        b"IsLevelMainMenuBackground",
        b"GetLevelNameShort",
        b"GetLevelName",
        b"IsDrawingLoadingImage",
        b"LevelInitPreEntity",
        b"map load failed",
        b"Background map",
    ):
        xref_str(engine, s)

    print("\n=== Engine CreateInterface / exports containing Level/Map ===")
    # CreateInterface strings
    for s in (b"VEngineClient015", b"VEngineClient014", b"EngineTraceClient004"):
        idx = bytes(engine.data).find(s)
        print(f"  iface {s!r}: {'RVA 0x%X' % engine.off_to_rva(idx) if idx>=0 else 'MISS'}")

    print("\n=== Search engine for IsInGame / IsConnected stubs via string ===")
    for s in (b"IsInGame", b"IsConnected", b"GetLocalPlayer", b"GetScreenSize"):
        xref_str(engine, s, max_xrefs=4)

    print("\n=== CalcViewModelView unique epilogue check ===")
    # confirm primary at 0xF090 has retn 14 as first real epilogue
    fo = client.rva_to_off(0xF090)
    ch = bytes(client.data[fo : fo + 0x200])
    for i in range(0x40, len(ch) - 2):
        if ch[i : i + 3] == b"\xC2\x14\x00" and ch[i - 1] == 0x5D:
            print(f"  OK first real retn 0x14 at +0x{i:X}")
            print(f"  sig candidate: {hexdump(client.data, fo, 28)}")
            break

    print("\n=== materialsystem CreateNamedRenderTarget ===")
    mat = pes["materialsystem.dll"]
    for s in (
        b"CreateNamedRenderTargetTextureEx",
        b"CreateNamedRenderTargetTexture",
        b"BeginRenderTargetAllocation",
        b"EndRenderTargetAllocation",
    ):
        xref_str(mat, s, max_xrefs=4)

    print("\n=== server FireBullets / ProcessUsercmds ===")
    for s in (b"FireBullets", b"ProcessUsercmds", b"EyePosition"):
        xref_str(server, s, max_xrefs=3)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
