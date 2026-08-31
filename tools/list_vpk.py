"""List entries in a Source VPK directory file.

Used to confirm which HUD icon assets Black Mesa actually ships before the mod
tries to load them by name.

    python tools/list_vpk.py <bms_textures_dir.vpk> [substring]
"""

import struct
import sys


def read_cstr(data, pos):
    end = data.index(b"\x00", pos)
    return data[pos:end].decode("latin-1"), end + 1


def iter_entries(path):
    with open(path, "rb") as handle:
        data = handle.read()

    signature, version, tree_size = struct.unpack_from("<III", data, 0)
    if signature != 0x55AA1234:
        raise ValueError(f"{path}: not a VPK (signature 0x{signature:08X})")
    pos = 28 if version == 2 else 12
    tree_end = pos + tree_size

    while pos < tree_end:
        ext, pos = read_cstr(data, pos)
        if not ext:
            break
        while True:
            folder, pos = read_cstr(data, pos)
            if not folder:
                break
            while True:
                name, pos = read_cstr(data, pos)
                if not name:
                    break
                _crc, preload, _archive, _offset, length = struct.unpack_from("<IHHII", data, pos)
                pos += 18
                pos += preload
                folder_part = "" if folder == " " else folder + "/"
                yield f"{folder_part}{name}.{ext}", length + preload


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    vpk = sys.argv[1]
    needle = sys.argv[2].lower() if len(sys.argv) > 2 else ""
    count = 0
    for name, size in iter_entries(vpk):
        if needle and needle not in name.lower():
            continue
        print(f"{size:>9}  {name}")
        count += 1
    print(f"-- {count} entries", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
