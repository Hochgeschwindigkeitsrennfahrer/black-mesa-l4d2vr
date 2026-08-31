"""Check the bytes a module actually has at a given RVA.

Used to confirm a reverse-engineered offset and its byte signature before
hooking it, so a wrong RVA is caught at the desk instead of in the headset.

    python tools/check_rva.py <module.dll> <rva-hex> [expected-hex-bytes]

Example:
    python tools/check_rva.py server.dll 0x47EC90 558BEC568BF18B06FF9034020000
"""

import struct
import sys


def sections(data):
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        raise ValueError("not a PE image")
    num_sections = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    table = pe_off + 24 + opt_size
    out = []
    for i in range(num_sections):
        entry = table + i * 40
        name = data[entry:entry + 8].rstrip(b"\0").decode("latin-1")
        virt_size, virt_addr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, entry + 8)
        out.append((name, virt_addr, max(virt_size, raw_size), raw_ptr))
    return out


def rva_to_offset(data, rva):
    for name, virt_addr, size, raw_ptr in sections(data):
        if virt_addr <= rva < virt_addr + size:
            return raw_ptr + (rva - virt_addr), name
    raise ValueError(f"RVA 0x{rva:X} is not inside any section")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    with open(sys.argv[1], "rb") as handle:
        data = handle.read()
    rva = int(sys.argv[2], 16)
    offset, section = rva_to_offset(data, rva)
    actual = data[offset:offset + 48]
    print(f"section {section}  file offset 0x{offset:X}")
    print("bytes    ", actual.hex().upper())

    if len(sys.argv) > 3:
        expected = bytes.fromhex(sys.argv[3])
        got = data[offset:offset + len(expected)]
        print("expected ", expected.hex().upper())
        print("MATCH" if got == expected else "MISMATCH")
        return 0 if got == expected else 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
