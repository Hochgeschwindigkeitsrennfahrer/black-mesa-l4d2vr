#!/usr/bin/env python3
"""Black Mesa VR offset / signature / RTTI / CreateInterface scanner."""
from __future__ import annotations

import argparse
import mmap
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, List, Optional, Sequence, Tuple

GAME = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Black Mesa")
MODULES = {
    "client.dll": GAME / "bms" / "bin" / "client.dll",
    "engine.dll": GAME / "bin" / "engine.dll",
    "materialsystem.dll": GAME / "bin" / "materialsystem.dll",
    "server.dll": GAME / "bms" / "bin" / "server.dll",
    "vguimatsurface.dll": GAME / "bin" / "vguimatsurface.dll",
}


@dataclass
class Section:
    name: str
    virt_size: int
    virt_addr: int
    raw_size: int
    raw_ptr: int


@dataclass
class PE:
    path: Path
    data: memoryview
    image_base: int
    sections: List[Section]

    def rva_to_off(self, rva: int) -> Optional[int]:
        for s in self.sections:
            if s.virt_addr <= rva < s.virt_addr + max(s.virt_size, s.raw_size):
                return s.raw_ptr + (rva - s.virt_addr)
        return None

    def off_to_rva(self, off: int) -> Optional[int]:
        for s in self.sections:
            if s.raw_ptr <= off < s.raw_ptr + s.raw_size:
                return s.virt_addr + (off - s.raw_ptr)
        return None

    def va_to_off(self, va: int) -> Optional[int]:
        return self.rva_to_off(va - self.image_base)

    def read_cstr(self, off: int, maxlen: int = 256) -> Optional[str]:
        if off < 0 or off >= len(self.data):
            return None
        end = min(len(self.data), off + maxlen)
        try:
            chunk = bytes(self.data[off:end])
            z = chunk.find(b"\x00")
            if z < 0:
                return None
            return chunk[:z].decode("ascii", errors="strict")
        except Exception:
            return None


def parse_pe(path: Path) -> PE:
    raw = path.read_bytes()
    mv = memoryview(raw)
    if raw[:2] != b"MZ":
        raise ValueError(f"not PE: {path}")
    e_lfanew = struct.unpack_from("<I", raw, 0x3C)[0]
    if raw[e_lfanew : e_lfanew + 4] != b"PE\0\0":
        raise ValueError(f"bad PE sig: {path}")
    coff = e_lfanew + 4
    num_sections = struct.unpack_from("<H", raw, coff + 2)[0]
    size_opt = struct.unpack_from("<H", raw, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", raw, opt)[0]
    if magic != 0x10B:  # PE32
        raise ValueError(f"expected PE32, got {magic:#x}: {path}")
    image_base = struct.unpack_from("<I", raw, opt + 28)[0]
    sec_off = opt + size_opt
    sections: List[Section] = []
    for i in range(num_sections):
        o = sec_off + i * 40
        name = bytes(raw[o : o + 8]).split(b"\0", 1)[0].decode("ascii", "replace")
        vsize, vaddr, rsize, rptr = struct.unpack_from("<IIII", raw, o + 8)
        sections.append(Section(name, vsize, vaddr, rsize, rptr))
    return PE(path, mv, image_base, sections)


def parse_sig(sig: str) -> Tuple[bytes, bytes]:
    parts = sig.split()
    pat = bytearray()
    mask = bytearray()
    for p in parts:
        if p in ("?", "??"):
            pat.append(0)
            mask.append(0)
        else:
            pat.append(int(p, 16))
            mask.append(0xFF)
    return bytes(pat), bytes(mask)


def find_all(data: memoryview, sig: str, limit: int = 50) -> List[int]:
    pat, mask = parse_sig(sig)
    n = len(pat)
    hits: List[int] = []
    # naive scan; DLLs are a few MB
    d = data
    for i in range(0, len(d) - n + 1):
        ok = True
        for j in range(n):
            if mask[j] and d[i + j] != pat[j]:
                ok = False
                break
        if ok:
            hits.append(i)
            if len(hits) >= limit:
                break
    return hits


def find_bytes(data: memoryview, needle: bytes, limit: int = 100) -> List[int]:
    hits: List[int] = []
    start = 0
    b = bytes(data) if not isinstance(data, (bytes, bytearray)) else data
    # memoryview find works for bytes
    blob = bytes(data)
    while True:
        i = blob.find(needle, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
        if len(hits) >= limit:
            break
    return hits


def hexdump(data: memoryview, off: int, n: int = 64) -> str:
    chunk = bytes(data[off : off + n])
    return " ".join(f"{b:02X}" for b in chunk)


def is_func_prologue(data: memoryview, off: int) -> bool:
    if off < 0 or off + 3 >= len(data):
        return False
    b0, b1, b2 = data[off], data[off + 1], data[off + 2]
    # push ebp; mov ebp, esp
    if b0 == 0x55 and b1 == 0x8B and b2 == 0xEC:
        return True
    # push esi / push edi / mov esi,ecx style
    if b0 in (0x56, 0x57, 0x53) and b1 == 0x8B:
        return True
    # sub esp / with stack cookie variants start with 55 8B EC
    return False


def walk_back_to_prologue(data: memoryview, off: int, max_back: int = 0x80) -> Optional[int]:
    for i in range(off, max(0, off - max_back) - 1, -1):
        if is_func_prologue(data, i):
            # prefer CC/C3 padding before
            if i == 0 or data[i - 1] in (0xCC, 0xC3, 0x90, 0x00):
                return i
            # also accept if previous is retn imm
            if i >= 3 and data[i - 3] == 0xC2:
                return i
    return None


def find_string_refs(pe: PE, s: bytes) -> List[Tuple[int, int]]:
    """Return list of (string_file_off, code_xref_file_off) for push imm32 / mov refs."""
    stroffs = find_bytes(pe.data, s + b"\x00")
    results: List[Tuple[int, int]] = []
    for so in stroffs:
        rva = pe.off_to_rva(so)
        if rva is None:
            continue
        va = pe.image_base + rva
        # search for absolute VA as little-endian dword in code
        needle = struct.pack("<I", va)
        for xo in find_bytes(pe.data, needle, limit=40):
            results.append((so, xo))
    return results


def dump_rtti_names(pe: PE, substr: bytes, limit: int = 40) -> List[Tuple[int, str]]:
    """Find .?AV / .?AU type descriptors containing substr."""
    hits = []
    blob = bytes(pe.data)
    start = 0
    while len(hits) < limit:
        i = blob.find(b".?AV", start)
        if i < 0:
            i = blob.find(b".?AU", start)
            if i < 0:
                break
        s = pe.read_cstr(i, 200)
        start = i + 4
        if s and substr.decode("ascii", "ignore") in s:
            hits.append((i, s))
    return hits


def extract_iface_versions(pe: PE, prefixes: Sequence[str]) -> List[str]:
    blob = bytes(pe.data)
    found = set()
    for pref in prefixes:
        pb = pref.encode("ascii")
        start = 0
        while True:
            i = blob.find(pb, start)
            if i < 0:
                break
            s = pe.read_cstr(i, 64)
            if s and s.startswith(pref) and s[len(pref) :].isdigit():
                found.add(s)
            start = i + 1
    return sorted(found)


def analyze_candidate(pe: PE, name: str, off: int, note: str = "") -> None:
    rva = pe.off_to_rva(off)
    print(f"  [{name}] file+0x{off:X} RVA=0x{(rva or 0):X}  {note}")
    print(f"    bytes: {hexdump(pe.data, off, 48)}")
    # nearby padding check
    before = bytes(pe.data[max(0, off - 8) : off])
    print(f"    before: {' '.join(f'{b:02X}' for b in before)}")


# ---- signatures from L4D2 / Portal2 / BMSVR ----
SIGS = [
    # client
    ("RenderView_L4D2", "client.dll", "55 8B EC 81 EC ? ? ? ? 53 56 57 8B D9"),
    ("RenderView_Portal2", "client.dll", "55 8B EC 83 EC 2C 53 56 8B F1 6A 00 8D 8E ? ? ? ? E8 ? ? ? ?"),
    ("RenderView_BMS_cand", "client.dll", "55 8B EC 83 EC ? 53 56 57 8B F9"),
    ("RenderView_wide", "client.dll", "55 8B EC 81 EC ? ? ? ? 53 56 57 8B F9"),
    ("RenderView_wide2", "client.dll", "55 8B EC 83 EC ? 53 56 8B F1"),
    ("CalcViewModelView_L4D2", "client.dll", "55 8B EC 83 EC 48 A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 8B 10"),
    ("CalcViewModelView_Portal2", "client.dll", "55 8B EC 83 EC 34 53 8B D9 80 BB"),
    ("CalcViewModelView_BMS", "client.dll", "55 8B EC 83 EC ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10"),
    ("CalcViewModelView_generic", "client.dll", "55 8B EC 83 EC ? 53 8B D9 8B 0D"),
    ("CreateMove_Portal2", "client.dll", "55 8B EC A1 ? ? ? ? 83 EC 0C 83 78 30 00 56 8B 75 0C 57 8B F9 74 43"),
    ("CreateMove_generic", "client.dll", "55 8B EC A1 ? ? ? ? 83 EC ? 83 78 30 00 56"),
    ("g_pClientMode_L4D2", "client.dll", "89 04 B5 ? ? ? ? E8"),
    ("g_pClientMode_Portal2", "client.dll", "8B 0D ? ? ? ? 8B"),
    ("g_pClientMode_BMS", "client.dll", "8B 0D ? ? ? ? 8B 01 FF 50"),
    ("AdjustEngineViewport_L4D2", "client.dll", "55 8B EC 8B 0D ? ? ? ? 85 C9 74 17"),
    ("AdjustEngineViewport_BMS", "client.dll", "55 8B EC 8B 0D ? ? ? ? 85 C9 74"),
    ("AdjustEngineViewport_short", "client.dll", "55 8B EC 8B 0D ? ? ? ? 85 C9 74 ? 8B 01"),
    # engine
    ("DrawModelExecute_L4D2", "engine.dll", "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 56 8B 75 08 57 8B"),
    ("VGui_Paint_L4D2", "engine.dll", "55 8B EC E8 ? ? ? ? 8B 10 8B C8 8B 52 38"),
    ("VGui_Paint_alt", "engine.dll", "55 8B EC 83 E4 F8 83 EC 08 E8"),
    # materialsystem
    ("GetRenderTarget_L4D2", "materialsystem.dll", "83 79 4C 00"),
    ("Viewport_L4D2", "materialsystem.dll", "55 8B EC 83 EC 28 8B C1"),
    ("Viewport_BMS", "materialsystem.dll", "55 8B EC 83 EC ? 8B C1"),
    ("GetViewport_L4D2", "materialsystem.dll", "55 8B EC 8B 41 4C 8B 49 40 8D 04 C0 83 7C 81 ? ?"),
    ("PushRT_L4D2", "materialsystem.dll", "55 8B EC 83 EC 24 8B 45 08 8B 55 10 89"),
    ("PopRT_L4D2", "materialsystem.dll", "56 8B F1 83 7E 4C 00"),
    # server
    ("EyePosition_L4D2", "server.dll", "55 8B EC 56 8B F1 8B 86 ? ? ? ? C1 E8 0B A8 01 74 05 E8 ? ? ? ? 8B 45 08 F3"),
]


KNOWN_OFFSETS = {
    "client.dll": {
        "RenderView": 0x76E10,
        "CalcViewModelView": 0xE490,
        "AdjustEngineViewport": 0x76E80,
        "g_pClientMode": 0x16A156,
    },
    "engine.dll": {
        "DrawModelExecute": 0xF5E20,
        "VGui_Paint": 0x115CE0,
    },
    "materialsystem.dll": {
        "GetRenderTarget": 0x67C20,
        "PopRT": 0x69650,
        "Viewport": 0x1BCD0,
        "GetViewport": 0x2D240,
        "PushRT": 0x2D5F0,
    },
    "server.dll": {
        "EyePosition": 0x317260,
    },
}


def main() -> int:
    print("=== Loading PEs ===")
    pes: dict[str, PE] = {}
    for name, path in MODULES.items():
        if not path.exists():
            print(f"MISSING {name}: {path}")
            continue
        pe = parse_pe(path)
        pes[name] = pe
        print(f"{name}: {path} size={len(pe.data)} base=0x{pe.image_base:X} sections={[s.name for s in pe.sections]}")

    print("\n=== CreateInterface version strings ===")
    for mod, prefixes in [
        ("materialsystem.dll", ["VMaterialSystem", "VMaterialSystemInternal", "MaterialSystemHardwareConfig"]),
        ("vguimatsurface.dll", ["VGUI_Surface", "VGUI_Input", "VGUI_Panel"]),
        ("engine.dll", ["VEngineClient", "VEngineServer", "VEngineRenderView", "VEngineModel", "VEngineCvar", "VEngineVGui"]),
        ("client.dll", ["VClient", "VClientEntityList", "GameClientExports", "ClientTools"]),
        ("server.dll", ["ServerGameDLL", "ServerGameEnts", "ServerGameClients", "VServer"]),
    ]:
        if mod not in pes:
            continue
        vers = extract_iface_versions(pes[mod], prefixes)
        print(f"{mod}:")
        for v in vers:
            print(f"  {v}")

    print("\n=== RTTI type names of interest ===")
    for mod, needles in [
        ("client.dll", [b"CViewRender", b"ClientMode", b"CBaseViewModel", b"CViewSetup", b"CHLClient"]),
        ("engine.dll", [b"CModelRender", b"CVGui", b"CEngineVGui"]),
        ("materialsystem.dll", [b"CMatRenderContext", b"CMaterialSystem"]),
        ("server.dll", [b"CBaseEntity", b"CBasePlayer"]),
    ]:
        if mod not in pes:
            continue
        print(f"-- {mod}")
        for needle in needles:
            hits = dump_rtti_names(pes[mod], needle, limit=15)
            for off, s in hits:
                print(f"  +0x{off:X}  {s}")

    print("\n=== String presence / xrefs ===")
    interesting = {
        "client.dll": [
            b"CViewRender",
            b"RenderView",
            b"ViewRender",
            b"CalcViewModelView",
            b"ClientMode",
            b"AdjustEngineViewport",
            b"CViewSetup",
            b"g_pClientMode",
            b"viewmodel",
        ],
        "engine.dll": [
            b"DrawModelExecute",
            b"VGui_Paint",
            b"CModelRender",
        ],
        "materialsystem.dll": [
            b"PushRenderTargetAndViewport",
            b"PopRenderTargetAndViewport",
            b"GetRenderTarget",
        ],
        "server.dll": [
            b"EyePosition",
        ],
    }
    for mod, strs in interesting.items():
        if mod not in pes:
            continue
        pe = pes[mod]
        print(f"-- {mod}")
        for s in strs:
            offs = find_bytes(pe.data, s + b"\x00", limit=5)
            if not offs:
                # also try without requiring exact cstr end (embedded)
                offs = find_bytes(pe.data, s, limit=5)
            if not offs:
                print(f"  STR miss: {s!r}")
                continue
            print(f"  STR {s!r} @ " + ", ".join(f"+0x{o:X}" for o in offs[:5]))
            # absolute VA xrefs
            for so in offs[:2]:
                rva = pe.off_to_rva(so)
                if rva is None:
                    continue
                va = pe.image_base + rva
                xrefs = find_bytes(pe.data, struct.pack("<I", va), limit=8)
                if xrefs:
                    print(f"    VA=0x{va:X} xrefs: " + ", ".join(f"+0x{x:X}" for x in xrefs[:8]))
                    for xo in xrefs[:3]:
                        prol = walk_back_to_prologue(pe.data, xo, 0x200)
                        if prol is not None:
                            print(f"      xref+0x{xo:X} -> prologue candidate +0x{prol:X}: {hexdump(pe.data, prol, 32)}")

    print("\n=== Known candidate sites ===")
    for mod, items in KNOWN_OFFSETS.items():
        if mod not in pes:
            continue
        pe = pes[mod]
        print(f"-- {mod}")
        for name, off in items.items():
            if off + 16 > len(pe.data):
                print(f"  {name} +0x{off:X} OUT OF RANGE")
                continue
            analyze_candidate(pe, name, off)

    print("\n=== Signature scan (all hits, capped) ===")
    for name, mod, sig in SIGS:
        if mod not in pes:
            print(f"{name:40} {mod} MISSING")
            continue
        hits = find_all(pes[mod].data, sig, limit=20)
        if not hits:
            print(f"{name:40} {mod} NOT FOUND")
            continue
        hitstr = ", ".join(f"+0x{h:X}" for h in hits[:10])
        more = f" (+{len(hits)-10} more)" if len(hits) > 10 else ""
        print(f"{name:40} {mod} hits={len(hits)}: {hitstr}{more}")

    # Extra: look near CViewRender RTTI for vtable / RenderView
    if "client.dll" in pes:
        pe = pes["client.dll"]
        print("\n=== CViewRender deeper ===")
        for off, s in dump_rtti_names(pe, b"CViewRender", limit=10):
            print(f"typeinfo {s} @ +0x{off:X}")
            # MSVC: Complete Object Locator often points here; search refs to type descriptor VA
            rva = pe.off_to_rva(off)
            if rva is None:
                continue
            va = pe.image_base + rva
            for xo in find_bytes(pe.data, struct.pack("<I", va), limit=15):
                print(f"  ref +0x{xo:X}: {hexdump(pe.data, max(0, xo - 16), 48)}")

        # Search for string "CViewRender::RenderView" style debug
        for needle in [
            b"CViewRender::RenderView",
            b"CViewRender::Render",
            b"ViewRender::RenderView",
            b"RenderingView",
            b"renderview",
            b"RenderView",
        ]:
            for so in find_bytes(pe.data, needle, limit=5):
                print(f"needle {needle!r} @ +0x{so:X} context={bytes(pe.data[so:so+40])!r}")

    # CreateMove / ClientModeShared patterns
    if "client.dll" in pes:
        pe = pes["client.dll"]
        print("\n=== ClientMode / CreateMove related ===")
        for off, s in dump_rtti_names(pe, b"ClientMode", limit=20):
            print(f"  {s} @ +0x{off:X}")
        # Portal2 CreateMove style
        for sig_name, sig in [
            ("CM_P2", "55 8B EC A1 ? ? ? ? 83 EC 0C 83 78 30 00 56 8B 75 0C 57 8B F9"),
            ("CM_alt1", "55 8B EC 83 EC ? 56 57 8B F9 8B 0D ? ? ? ? 8B 01"),
            ("CM_alt2", "55 8B EC 56 8B 75 0C 57 8B F9 85 F6"),
            ("AEV_full", "55 8B EC 8B 0D ? ? ? ? 85 C9 74 ? 8B 01 8B 50 ? FF D2"),
        ]:
            hits = find_all(pe.data, sig, limit=15)
            print(f"  {sig_name}: {len(hits)} hits -> " + ", ".join(f"+0x{h:X}" for h in hits[:8]))

    # materialsystem: validate GetRenderTarget uniqueness / nearby funcs
    if "materialsystem.dll" in pes:
        pe = pes["materialsystem.dll"]
        print("\n=== materialsystem RT helpers context ===")
        for name, off in KNOWN_OFFSETS["materialsystem.dll"].items():
            print(f"{name} +0x{off:X}: {hexdump(pe.data, off, 40)}")
        # Find all PopRT-like
        hits = find_all(pe.data, "56 8B F1 83 7E 4C 00", limit=30)
        print(f"PopRT pattern hits ({len(hits)}): " + ", ".join(f"+0x{h:X}" for h in hits))
        hits = find_all(pe.data, "83 79 4C 00", limit=30)
        print(f"GetRT pattern hits ({len(hits)}): " + ", ".join(f"+0x{h:X}" for h in hits))
        # PushRT variants
        for sig in [
            "55 8B EC 83 EC 24 8B 45 08 8B 55 10 89",
            "55 8B EC 83 EC ? 8B 45 08 8B 55 10",
            "55 8B EC 83 EC ? 53 8B 5D 08 56 8B F1",
        ]:
            hits = find_all(pe.data, sig, limit=20)
            print(f"PushRT '{sig[:40]}...': {len(hits)} -> " + ", ".join(f"+0x{h:X}" for h in hits[:8]))

    # engine DrawModelExecute / VGui_Paint validation
    if "engine.dll" in pes:
        pe = pes["engine.dll"]
        print("\n=== engine candidates context ===")
        for name, off in KNOWN_OFFSETS["engine.dll"].items():
            analyze_candidate(pe, name, off)
        for off, s in dump_rtti_names(pe, b"CModelRender", limit=5):
            print(f"RTTI {s} @ +0x{off:X}")
        for needle in [b"DrawModelExecute", b"VGui_Paint", b"CEngineVGui::Paint"]:
            for so in find_bytes(pe.data, needle, limit=5):
                print(f"  {needle!r} @ +0x{so:X}")

    # server EyePosition
    if "server.dll" in pes:
        pe = pes["server.dll"]
        print("\n=== server EyePosition ===")
        hits = find_all(pe.data, SIGS[-1][2], limit=20)
        print(f"EyePosition sig hits={len(hits)}: " + ", ".join(f"+0x{h:X}" for h in hits[:15]))
        off = 0x317260
        analyze_candidate(pe, "EyePosition_cand", off)
        # check how many almost-identical prologues
        hits2 = find_all(pe.data, "55 8B EC 56 8B F1 8B 86 ? ? ? ? C1 E8 0B A8 01", limit=30)
        print(f"EyePosition short hits={len(hits2)}: " + ", ".join(f"+0x{h:X}" for h in hits2[:20]))

    # Distance check: AdjustEngineViewport vs RenderView
    if "client.dll" in pes:
        pe = pes["client.dll"]
        print("\n=== RenderView vs AdjustEngineViewport proximity ===")
        rv, aev = 0x76E10, 0x76E80
        print(f"delta = 0x{aev-rv:X}")
        print(f"RenderView region:\n  {hexdump(pe.data, rv, 0x80)}")
        print(f"AEV region:\n  {hexdump(pe.data, aev, 0x40)}")
        # Is 0x76E80 even a separate function? Check for CC between them
        mid = bytes(pe.data[rv:aev])
        print(f"bytes between RV and AEV ({len(mid)}): first ret/int3? " +
              f"C3@{[i for i,b in enumerate(mid) if b==0xC3][:5]} CC@{[i for i,b in enumerate(mid) if b==0xCC][:5]}")

    print("\nDone.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
