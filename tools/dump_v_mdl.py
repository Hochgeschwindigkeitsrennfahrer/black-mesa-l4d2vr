"""Extract and dump Source v48 viewmodel MDL bodyparts/bones from a VPK."""
from __future__ import annotations

import struct
import sys
from pathlib import Path


def read_cstring(data: bytes, off: int) -> str:
    if off < 0 or off >= len(data):
        return ""
    end = data.find(b"\x00", off)
    if end < 0:
        end = len(data)
    return data[off:end].decode("ascii", errors="replace")


def parse_vpk_tree(dir_path: Path) -> dict[str, tuple[int, int, int, bytes]]:
    raw = dir_path.read_bytes()
    sig, ver, tree_size = struct.unpack_from("<III", raw, 0)
    if sig != 0x55AA1234:
        raise SystemExit(f"bad vpk signature {sig:#x}")
    header_size = 12
    if ver == 2:
        header_size = 28
    tree = raw[header_size : header_size + tree_size]
    files: dict[str, tuple[int, int, int, bytes]] = {}
    i = 0

    def read_z() -> str:
        nonlocal i
        z = tree.find(b"\x00", i)
        s = tree[i:z].decode("ascii", errors="replace")
        i = z + 1
        return s

    while True:
        ext = read_z()
        if not ext:
            break
        while True:
            path = read_z()
            if not path:
                break
            while True:
                name = read_z()
                if not name:
                    break
                crc, preload, archive, offset, length, term = struct.unpack_from(
                    "<IHHIIH", tree, i
                )
                i += 18
                preload_bytes = tree[i : i + preload]
                i += preload
                rel = f"{path}/{name}.{ext}".replace("\\", "/").lower()
                files[rel] = (archive, offset, length, preload_bytes)
    return files


def extract_file(vpk_dir: Path, rel: str) -> bytes:
    files = parse_vpk_tree(vpk_dir)
    key = rel.replace("\\", "/").lower()
    if key not in files:
        matches = [k for k in files if k.endswith(key) or key in k]
        raise SystemExit(f"missing {rel}; close={matches[:12]}")
    archive, offset, length, preload = files[key]
    if archive == 0x7FFF:
        return preload[:length]
    stem = vpk_dir.name.replace("_dir.vpk", "")
    data_path = vpk_dir.with_name(f"{stem}_{archive:03d}.vpk")
    with data_path.open("rb") as f:
        f.seek(offset)
        rest = f.read(length - len(preload) if length > len(preload) else 0)
    return preload + rest


def dump_mdl(data: bytes, label: str) -> None:
    if data[:4] != b"IDST":
        raise SystemExit(f"{label} not IDST")
    ver, checksum, name = struct.unpack_from("<II64s", data, 4)
    name = name.split(b"\x00", 1)[0].decode("ascii", errors="replace")
    length = struct.unpack_from("<I", data, 76)[0]
    hull_min = struct.unpack_from("<3f", data, 104)
    hull_max = struct.unpack_from("<3f", data, 116)
    flags, numbones, boneindex = struct.unpack_from("<iii", data, 152)
    numbodyparts, bodypartindex = struct.unpack_from("<ii", data, 232)
    numtextures, textureindex = struct.unpack_from("<ii", data, 204)
    print(f"\n=== {label} ===")
    print(f"version={ver} name={name} length={length} flags={flags:#x}")
    print(f"hull min={hull_min} max={hull_max}")
    dx = hull_max[0] - hull_min[0]
    dy = hull_max[1] - hull_min[1]
    dz = hull_max[2] - hull_min[2]
    print(f"hull size=({dx:.2f},{dy:.2f},{dz:.2f}) longest={max(dx, dy, dz):.2f}")
    print(f"numbones={numbones} boneindex={boneindex}")
    print(f"numbodyparts={numbodyparts} bodypartindex={bodypartindex}")
    print(f"numtextures={numtextures}")

    # mstudiobone_t v48 size: 216 bytes on Source 2007/2013
    bone_size = 216
    print("-- bones --")
    for i in range(numbones):
        off = boneindex + i * bone_size
        szname, parent = struct.unpack_from("<ii", data, off)
        pos = struct.unpack_from("<3f", data, off + 8 + 24)  # after bonecontroller[6]
        # layout: sznameindex(4) parent(4) bonecontroller[6](24) pos(12)
        pos = struct.unpack_from("<3f", data, off + 32)
        bname = read_cstring(data, off + szname)
        print(f"  [{i:02d}] parent={parent:3d} pos=({pos[0]:8.3f},{pos[1]:8.3f},{pos[2]:8.3f}) {bname}")

    print("-- bodyparts --")
    for bi in range(numbodyparts):
        boff = bodypartindex + bi * 16
        szname, nummodels, base, modelindex = struct.unpack_from("<iiii", data, boff)
        bpname = read_cstring(data, boff + szname)
        print(f"  bodypart {bi} '{bpname}' nummodels={nummodels} base={base} modelindex={modelindex}")
        # mstudiomodel_t v48: 148 bytes
        model_size = 148
        for mi in range(nummodels):
            moff = boff + modelindex + mi * model_size
            mname = data[moff : moff + 64].split(b"\x00", 1)[0].decode("ascii", errors="replace")
            typ, radius, nummeshes, meshindex, numverts = struct.unpack_from(
                "<ifiii", data, moff + 64
            )
            print(
                f"    model {mi} '{mname}' type={typ} radius={radius:.2f} "
                f"nummeshes={nummeshes} numverts={numverts}"
            )
            mesh_size = 116  # mstudiomesh_t v48 approx; material is first int
            for gi in range(min(nummeshes, 8)):
                goff = moff + meshindex + gi * mesh_size
                material = struct.unpack_from("<i", data, goff)[0]
                print(f"      mesh {gi} material_index={material}")

    print("-- textures --")
    # mstudiotexture_t v48: sznameindex, flags, used, unused1, material, clientmaterial, unused[10]
    tex_size = 64
    for ti in range(numtextures):
        toff = textureindex + ti * tex_size
        szname = struct.unpack_from("<i", data, toff)[0]
        tname = read_cstring(data, toff + szname)
        print(f"  [{ti}] {tname}")


def main() -> None:
    game = Path(r"C:\Program Files (x86)\Steam\steamapps\common\Black Mesa\bms")
    vpk = game / "bms_models_dir.vpk"
    names = sys.argv[1:] or [
        "models/weapons/v_crowbar.mdl",
        "models/weapons/v_glock.mdl",
        "models/weapons/v_mp5.mdl",
        "models/weapons/v_shotgun.mdl",
    ]
    for n in names:
        data = extract_file(vpk, n)
        dump_mdl(data, n)


if __name__ == "__main__":
    main()
