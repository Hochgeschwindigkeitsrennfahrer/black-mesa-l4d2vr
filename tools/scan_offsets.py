#!/usr/bin/env python3
"""Offline signature scan of Black Mesa modules. Does not launch the game."""
from __future__ import annotations

import argparse
import mmap
import os
import struct
from pathlib import Path


BM_ROOT = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Black Mesa")

MODULES = {
    "client.dll": BM_ROOT / "bms" / "bin" / "client.dll",
    "server.dll": BM_ROOT / "bms" / "bin" / "server.dll",
    "engine.dll": BM_ROOT / "bin" / "engine.dll",
    "materialsystem.dll": BM_ROOT / "bin" / "materialsystem.dll",
}

# Prototype (build 19042901) + L4D2VR-style patterns to re-verify on the installed build.
SIGNATURES = [
    ("RenderView", "client.dll", 0x207730, "55 8B EC 83 EC 08 A1 ? ? ? ? 53 8B D9 89 45 F8"),
    ("g_pClientMode", "client.dll", 0x16AD56, "56 57 8B F9 8B 0D ? ? ? ? 8B 01 FF 50 24", 6),
    ("CreateMove", "client.dll", 0x110310, "55 8B EC E8 ? ? ? ? 8B C8 85 C9 75 06 B0 01 5D C2 08 00"),
    ("CalcViewModelView", "client.dll", 0x29D930, "55 8B EC 83 EC 24 53 56 8B 75 08 57 8B F9 85 F6"),
    ("AdjustEngineViewport", "client.dll", 0x1102C0, "C2 10 00 CC CC CC CC CC CC CC CC CC CC CC CC CC B0 01 C2 08 00"),
    ("LevelInit", "client.dll", 0x110A80, "55 8B EC 83 EC 20 56 8B F1 6A 01 68 ? ? ? ?"),
    ("LevelShutdown", "client.dll", 0x110B30, "55 8B EC 83 EC 20 56 8B F1 B9 ? ? ? ? E8"),
    ("OverrideView", "client.dll", 0x110BE0, "55 8B EC 8B 45 08"),
    ("DrawModelExecute", "engine.dll", 0xF6A20, "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 56 8B 75 08 57 8B"),
    ("VGui_Paint", "engine.dll", 0x238C50, "55 8B EC 83 EC 18 53 8B D9 8B 0D ? ? ? ? FF 15"),
    ("GetRenderTarget", "materialsystem.dll", 0x68820, "83 79 4C 00 7E 0E 8B 41 4C 8D 14 C0"),
    ("GetViewport", "materialsystem.dll", 0x68A70, "55 8B EC 8B 41 4C 56 8D 14 C0 8B 41 40 83 7C 90 F8 00"),
    ("Viewport", "materialsystem.dll", 0x69F30, "55 8B EC 56 FF 75 14 8B F1 FF 75 10 FF 75 0C FF 75 08 E8"),
    ("PushRenderTargetAndViewport", "materialsystem.dll", 0x6A3D0, "55 8B EC 83 EC 24 8B 45 08 89 45 DC 8B 45 0C 89 45 EC"),
    ("PopRenderTargetAndViewport", "materialsystem.dll", 0x6A250, "56 8B F1 83 7E 4C 00 74 15 8B 06 6A 00 FF 50 10 FF 4E 4C"),
    ("ProcessUsercmds", "server.dll", 0x5320F0, "55 8B EC B8 ? ? ? ? E8 ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 0C 8B 55 08"),
]


def parse_pattern(sig: str) -> list[int | None]:
    out: list[int | None] = []
    for tok in sig.split():
        if tok in ("?", "??"):
            out.append(None)
        else:
            out.append(int(tok, 16))
    return out


def find_pattern(data: bytes, pattern: list[int | None]) -> list[int]:
    plen = len(pattern)
    hits = []
    n = len(data) - plen
    first = pattern[0]
    start = 0
    while start <= n:
        if first is not None:
            idx = data.find(bytes([first]), start)
            if idx < 0 or idx > n:
                break
            start = idx
        ok = True
        for j, b in enumerate(pattern):
            if b is not None and data[start + j] != b:
                ok = False
                break
        if ok:
            hits.append(start)
            if len(hits) >= 8:
                break
        start += 1
    return hits


def pe_image(path: Path) -> tuple[bytes, int]:
    raw = path.read_bytes()
    if raw[:2] != b"MZ":
        raise RuntimeError(f"not PE: {path}")
    e_lfanew = struct.unpack_from("<I", raw, 0x3C)[0]
    optional = e_lfanew + 24
    magic = struct.unpack_from("<H", raw, optional)[0]
    if magic != 0x10B:
        raise RuntimeError(f"not PE32: {path}")
    size_of_image = struct.unpack_from("<I", raw, optional + 56)[0]
    num_sections = struct.unpack_from("<H", raw, e_lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", raw, e_lfanew + 20)[0]
    section_off = e_lfanew + 24 + opt_size
    image = bytearray(size_of_image)
    for i in range(num_sections):
        off = section_off + i * 40
        va = struct.unpack_from("<I", raw, off + 12)[0]
        raw_size = struct.unpack_from("<I", raw, off + 16)[0]
        raw_ptr = struct.unpack_from("<I", raw, off + 20)[0]
        virt_size = struct.unpack_from("<I", raw, off + 8)[0]
        copy = min(raw_size, virt_size, max(0, size_of_image - va))
        image[va : va + copy] = raw[raw_ptr : raw_ptr + copy]
    return bytes(image), size_of_image


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=str(Path(__file__).resolve().parents[1] / "docs" / "OFFSETS.md"))
    args = parser.parse_args()

    images: dict[str, bytes] = {}
    for name, path in MODULES.items():
        if not path.exists():
            print(f"MISSING {name}: {path}")
            continue
        images[name], size = pe_image(path)
        print(f"loaded {name} image={size} file={path.stat().st_size}")

    lines = [
        "# Black Mesa offsets (scanned)",
        "",
        f"Install: `{BM_ROOT}`",
        "",
        "| Name | Module | Expected RVA | Status | Found RVAs |",
        "| --- | --- | --- | --- | --- |",
    ]
    for name, module, expected, sig, *rest in SIGNATURES:
        sig_off = rest[0] if rest else 0
        data = images.get(module)
        if data is None:
            lines.append(f"| {name} | {module} | `0x{expected:X}` | MODULE MISSING | |")
            continue
        pattern = parse_pattern(sig)
        hits = [h + sig_off for h in find_pattern(data, pattern)]
        if expected in hits or (expected - sig_off) in [h - sig_off for h in hits]:
            status = "MATCH"
        elif hits:
            status = "MOVED"
        else:
            status = "NOT FOUND"
        found = ", ".join(f"`0x{h:X}`" for h in hits) if hits else ""
        print(f"{status:10} {name:28} expected=0x{expected:X} found={found}")
        lines.append(f"| {name} | {module} | `0x{expected:X}` | {status} | {found} |")

    Path(args.out).write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
