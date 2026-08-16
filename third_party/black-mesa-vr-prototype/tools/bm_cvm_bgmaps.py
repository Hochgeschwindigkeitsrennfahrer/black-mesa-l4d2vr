#!/usr/bin/env python3
"""Follow-up: resolve BM thunks, CalcViewModelView owner, BackgroundMaps."""
from __future__ import annotations

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from bm_offset_scan import MODULES, hexdump, parse_pe  # noqa: E402
from bm_offset_deep import dump_rtti_names, find_col_and_vtable, read_u32  # noqa: E402


def resolve_e9(pe, rva: int):
    fo = pe.rva_to_off(rva)
    for i in range(0, 16):
        if pe.data[fo + i] == 0xE9:
            rel = struct.unpack_from("<i", pe.data, fo + i + 1)[0]
            return (rva + i + 5 + rel) & 0xFFFFFFFF
    return None


def resolve_first_e8(pe, rva: int, window: int = 40):
    fo = pe.rva_to_off(rva)
    for i in range(0, window):
        if pe.data[fo + i] == 0xE8:
            rel = struct.unpack_from("<i", pe.data, fo + i + 1)[0]
            return i, (rva + i + 5 + rel) & 0xFFFFFFFF
    return None, None


def main() -> int:
    client = parse_pe(MODULES["client.dll"])
    engine = parse_pe(MODULES["engine.dll"])
    print(f"client image_base=0x{client.image_base:X}")

    print("\n=== BM thunk / call targets ===")
    for rva, label in (
        (0x216EB0, "BM OverrideView E9"),
        (0x217250, "BM ShouldDrawCrosshair E9"),
        (0x216EC0, "BM ProcessInput E9"),
        (0x216B40, "BM KeyInput E9"),
        (0x217260, "BM ShouldDrawFog E9"),
        (0x2172C0, "BM Shutdown E9"),
        (0x217820, "BM VGui_Shutdown E9"),
    ):
        print(f"  {label} 0x{rva:X} -> 0x{resolve_e9(client, rva):X}")

    for rva, label in (
        (0x216130, "BM CreateMove E8"),
        (0x216B90, "BM LevelInit E8"),
        (0x216C60, "BM LevelShutdown E8"),
    ):
        off, tgt = resolve_first_e8(client, rva)
        print(f"  {label} 0x{rva:X} E8@{off} -> 0x{tgt:X}")

    print("\n=== CalcViewModelView owner ===")
    # ctor at 0xF2F0 writes vptr
    fo = client.rva_to_off(0xF2F0)
    print(f"  nearby ctor 0xF2F0: {hexdump(client.data, fo, 40)}")
    chunk = bytes(client.data[fo : fo + 0x60])
    idx = chunk.find(b"\xC7\x06")
    if idx < 0:
        idx = chunk.find(b"\xC7\x01")
    vt_va = struct.unpack_from("<I", chunk, idx + 2)[0]
    vt_fo = client.va_to_off(vt_va)
    print(f"  vptr store VA=0x{vt_va:X} file+0x{vt_fo:X} RVA=0x{client.off_to_rva(vt_fo):X}")

    cvm_fo = None
    cvm_va = client.image_base + 0xF090
    needle = struct.pack("<I", cvm_va)
    raw = bytes(client.data)
    start = 0
    while True:
        i = raw.find(needle, start)
        if i < 0:
            break
        print(f"  raw ref to CalcViewModelView @ file+0x{i:X}")
        cvm_fo = i
        start = i + 1

    if vt_fo is not None and cvm_fo is not None:
        slot = (cvm_fo - vt_fo) // 4
        print(f"  slot index vs ctor vtable: {slot}")
        col_va = read_u32(client, vt_fo - 4)
        col_fo = client.va_to_off(col_va)
        tname = "?"
        if col_fo is not None:
            td_va = read_u32(client, col_fo + 0x0C)
            td_fo = client.va_to_off(td_va)
            if td_fo is not None:
                tname = client.read_cstr(td_fo + 8) or "?"
            print(
                f"  COL VA=0x{col_va:X} type={tname} "
                f"sig={read_u32(client, col_fo)} offset={read_u32(client, col_fo+4)}"
            )
        print("  slots:")
        for s in range(max(0, slot - 4), slot + 6):
            va = read_u32(client, vt_fo + s * 4)
            f = client.va_to_off(va)
            r = client.off_to_rva(f) if f is not None else None
            mark = " <<CVM" if s == slot else ""
            hx = hexdump(client.data, f, 14) if f is not None else ""
            print(f"    [{s:02d}] RVA=0x{r:X}{mark}  {hx}")

    print("\n=== RTTI COL/VT pairs for ViewModel ===")
    for needle in (b"C_BaseViewModel@@", b"C_BlackMesaViewModel@@"):
        for off, name in dump_rtti_names(client, needle, limit=3):
            pairs = find_col_and_vtable(client, off)
            print(f"  {name}: {len(pairs)} pairs @ TD+0x{off:X}")
            for col, vt in pairs[:6]:
                hit = None
                for si in range(130):
                    va = read_u32(client, vt + si * 4)
                    f = client.va_to_off(va)
                    if f is not None and client.off_to_rva(f) == 0xF090:
                        hit = si
                        break
                print(
                    f"    COL+0x{col:X}/RVA=0x{client.off_to_rva(col):X} "
                    f"VT+0x{vt:X}/RVA=0x{client.off_to_rva(vt):X} CVM_slot={hit}"
                )

    # BlackMesaViewModel ctor vptr?
    print("\n=== scan constructors storing vtables near CVM ref ===")
    if cvm_fo is not None:
        # any mov dword ptr [reg], imm32 pointing near this table
        vt_candidate_vas = []
        for back in range(0, 40):
            cand = cvm_fo - back * 4
            vt_candidate_vas.append(client.image_base + client.off_to_rva(cand))
        for va in vt_candidate_vas:
            pat = b"\xC7\x06" + struct.pack("<I", va)
            j = raw.find(pat)
            if j >= 0:
                print(f"  C7 06 {va:08X} at file+0x{j:X} prol~RVA=0x{client.off_to_rva(j):X}")
            pat = b"\xC7\x01" + struct.pack("<I", va)
            j = raw.find(pat)
            if j >= 0:
                print(f"  C7 01 {va:08X} at file+0x{j:X}")

    print("\n=== BackgroundMaps engine function ===")
    # function at RVA 0xC0630
    fo = engine.rva_to_off(0xC0630)
    print(f"  fn RVA=0xC0630: {hexdump(engine.data, fo, 80)}")
    # string pushed early: 68 94 22 33 10
    str_va = 0x10332294
    str_fo = engine.va_to_off(str_va)
    print(f"  early push str VA=0x{str_va:X} -> {engine.read_cstr(str_fo)!r}")
    bg_va = 0x103322C4
    print(f"  BackgroundMaps VA -> {engine.read_cstr(engine.va_to_off(bg_va))!r}")
    # size / retn
    ch = bytes(engine.data[fo : fo + 0x400])
    for i in range(0x40, len(ch) - 2):
        if ch[i] == 0xC2 and ch[i + 2] == 0x00 and ch[i - 1] in (0x5D, 0xC9, 0x5E, 0x5F, 0x5B):
            print(f"  retn 0x{ch[i+1]:X} at +0x{i:X}")
            break
        if ch[i : i + 2] == b"\x5D\xC3":
            print(f"  ret at +0x{i+1:X}")
            break

    # Is this referenced from known load path?
    fn_va = engine.image_base + 0xC0630
    refs = []
    start = 0
    blob = bytes(engine.data)
    needle = struct.pack("<I", fn_va)
    while len(refs) < 8:
        i = blob.find(needle, start)
        if i < 0:
            break
        refs.append(i)
        start = i + 1
    print(f"  absolute refs to fn: {[hex(x) for x in refs]}")
    # also relative calls to it
    call_hits = []
    for i in range(len(blob) - 5):
        if blob[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", blob, i + 1)[0]
        # need caller RVA
        cr = engine.off_to_rva(i)
        if cr is None:
            continue
        tgt = (cr + 5 + rel) & 0xFFFFFFFF
        if tgt == 0xC0630:
            call_hits.append(cr)
            if len(call_hits) >= 10:
                break
    print(f"  E8 calls to 0xC0630: {[hex(x) for x in call_hits]}")

    print("\n=== Shared LevelInit string push (map name path) ===")
    fo = client.rva_to_off(0x110A80)
    print(hexdump(client.data, fo, 64))
    # 68 48 1A 44 10
    for i in range(0, 40):
        if client.data[fo + i] == 0x68:
            imm = struct.unpack_from("<I", client.data, fo + i + 1)[0]
            sof = client.va_to_off(imm)
            if sof is not None:
                print(f"  push str VA=0x{imm:X} -> {client.read_cstr(sof)!r}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
