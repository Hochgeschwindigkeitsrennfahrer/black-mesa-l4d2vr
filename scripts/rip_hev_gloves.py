#!/usr/bin/env python3
"""Split HL2VR ValveBiped player.mdl into independent L/R GLBs.

HL2VR uses one dual-arm HEV mesh and IK each arm to a controller. L4D2VR
independent hands are two skinned GLBs on controller matrices. This rips
wrist+finger subtrees, puts the wrist at the origin in meters, and names
joints like SteamVR vr_glove so BMVR's summary-curl palette still works.
"""
from __future__ import annotations

import json
import math
import os
import struct
import sys
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
MDL = os.path.join(ROOT, "assets", "hand models", "content", "models", "player.mdl")
VVD = os.path.join(ROOT, "assets", "hand models", "content", "models", "player.vvd")
VTX = os.path.join(ROOT, "assets", "hand models", "content", "models", "player.dx90.vtx")
VTF = os.path.join(
    ROOT, "assets", "hand models", "content", "materials", "models", "player", "v_hand.vtf"
)
OUT_DIR = os.path.join(ROOT, "VR", "hands")
SOURCE_UNITS_PER_METER = 39.3700787


def read_c_string(data: bytes, off: int) -> str:
    end = data.find(b"\x00", off)
    if end < 0:
        end = len(data)
    return data[off:end].decode("ascii", "replace")


def mat4_identity():
    return [
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, 0.0, 1.0,
    ]


def mat4_mul(a, b):
    out = [0.0] * 16
    for r in range(4):
        for c in range(4):
            out[c * 4 + r] = (
                a[0 * 4 + r] * b[c * 4 + 0]
                + a[1 * 4 + r] * b[c * 4 + 1]
                + a[2 * 4 + r] * b[c * 4 + 2]
                + a[3 * 4 + r] * b[c * 4 + 3]
            )
    return out


def mat4_from_pos_quat(px, py, pz, qx, qy, qz, qw):
    n = math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw) or 1.0
    qx, qy, qz, qw = qx / n, qy / n, qz / n, qw / n
    xx, yy, zz = qx * qx, qy * qy, qz * qz
    xy, xz, yz = qx * qy, qx * qz, qy * qz
    wx, wy, wz = qw * qx, qw * qy, qw * qz
    # Column-major, R * v for column vectors (matches mat4_mul / glTF).
    return [
        1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy), 0.0,
        2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx), 0.0,
        2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy), 0.0,
        px, py, pz, 1.0,
    ]


def mat4_invert(m):
    # Affine inverse: R^T and -R^T t
    r00, r10, r20 = m[0], m[1], m[2]
    r01, r11, r21 = m[4], m[5], m[6]
    r02, r12, r22 = m[8], m[9], m[10]
    tx, ty, tz = m[12], m[13], m[14]
    out = [
        r00, r01, r02, 0.0,
        r10, r11, r12, 0.0,
        r20, r21, r22, 0.0,
        -(r00 * tx + r10 * ty + r20 * tz),
        -(r01 * tx + r11 * ty + r21 * tz),
        -(r02 * tx + r12 * ty + r22 * tz),
        1.0,
    ]
    return out


def mat4_mul_vec3(m, x, y, z, w=1.0):
    return (
        m[0] * x + m[4] * y + m[8] * z + m[12] * w,
        m[1] * x + m[5] * y + m[9] * z + m[13] * w,
        m[2] * x + m[6] * y + m[10] * z + m[14] * w,
    )


def vec_len(v):
    return math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])


def vec_norm(v):
    l = vec_len(v)
    if l < 1e-8:
        return (0.0, 0.0, 1.0)
    return (v[0] / l, v[1] / l, v[2] / l)


def vec_cross(a, b):
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def vec_dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def look_basis(z_axis, y_hint):
    z = vec_norm(z_axis)
    y = y_hint
    if vec_len(y) < 1e-4:
        y = (0.0, 0.0, 1.0)
    x = vec_cross(y, z)
    if vec_len(x) < 1e-4:
        x = vec_cross((0.0, 1.0, 0.0), z)
    x = vec_norm(x)
    y = vec_cross(z, x)
    return [
        x[0], x[1], x[2], 0.0,
        y[0], y[1], y[2], 0.0,
        z[0], z[1], z[2], 0.0,
        0.0, 0.0, 0.0, 1.0,
    ]


def load_bones(mdl: bytes):
    numbones, boneindex = struct.unpack_from("<ii", mdl, 156)
    bones = []
    for i in range(numbones):
        off = boneindex + i * 216
        nameidx, parent = struct.unpack_from("<ii", mdl, off)
        px, py, pz = struct.unpack_from("<fff", mdl, off + 32)
        qx, qy, qz, qw = struct.unpack_from("<ffff", mdl, off + 44)
        bones.append(
            {
                "name": read_c_string(mdl, off + nameidx),
                "parent": parent,
                "pos": (px, py, pz),
                "quat": (qx, qy, qz, qw),
            }
        )
    worlds = []
    for i, b in enumerate(bones):
        local = mat4_from_pos_quat(*b["pos"], *b["quat"])
        if b["parent"] >= 0:
            worlds.append(mat4_mul(worlds[b["parent"]], local))
        else:
            worlds.append(local)
    return bones, worlds


def load_vertices(vvd: bytes):
    vertex_start = struct.unpack_from("<i", vvd, 56)[0]
    nverts = struct.unpack_from("<i", vvd, 16)[0]
    verts = []
    for i in range(nverts):
        off = vertex_start + i * 48
        w0, w1, w2 = struct.unpack_from("<fff", vvd, off)
        b0, b1, b2, nb = struct.unpack_from("<bbbb", vvd, off + 12)
        px, py, pz = struct.unpack_from("<fff", vvd, off + 16)
        nx, ny, nz = struct.unpack_from("<fff", vvd, off + 28)
        u, v = struct.unpack_from("<ff", vvd, off + 40)
        verts.append(
            {
                "pos": (px, py, pz),
                "nrm": (nx, ny, nz),
                "uv": (u, v),
                "bones": (b0, b1, b2),
                "weights": (w0, w1, w2),
                "nb": nb,
            }
        )
    return verts


def load_indices(vtx: bytes, mesh_vertex_offsets):
    """VTX v7 packed MeshHeader (9) + StripGroupHeader (25). Offsets are relative to each struct."""
    num_bp, bp_off = struct.unpack_from("<ii", vtx, 28)
    bp = bp_off
    _num_models, model_off = struct.unpack_from("<ii", vtx, bp)
    mh = bp + model_off
    _num_lods, lod_off = struct.unpack_from("<ii", vtx, mh)
    lh = mh + lod_off
    num_meshes, mesh_off, _switch = struct.unpack_from("<iif", vtx, lh)
    mesh_base = lh + mesh_off
    tris = []
    mesh_stride = 9
    for mi in range(num_meshes):
        moff = mesh_base + mi * mesh_stride
        num_sg, sg_rel = struct.unpack_from("<ii", vtx, moff)
        sg_base = moff + sg_rel
        voff = mesh_vertex_offsets[mi] if mi < len(mesh_vertex_offsets) else 0
        sg_stride = 25
        for si in range(num_sg):
            sgo = sg_base + si * sg_stride
            nverts, vert_rel, nidx, idx_rel = struct.unpack_from("<iiii", vtx, sgo)
            orig = []
            verts_off = sgo + vert_rel
            for vi in range(nverts):
                orig_id = struct.unpack_from("<H", vtx, verts_off + vi * 9 + 4)[0]
                orig.append(voff + orig_id)
            idxs_off = sgo + idx_rel
            for i in range(0, nidx - 2, 3):
                a, b, c = struct.unpack_from("<HHH", vtx, idxs_off + 2 * i)
                if a >= nverts or b >= nverts or c >= nverts:
                    continue
                tris.append((orig[a], orig[b], orig[c]))
    return tris


def decode_dxt5(data: bytes, w: int, h: int) -> bytes:
    out = bytearray(w * h * 4)
    bx_n = (w + 3) // 4
    by_n = (h + 3) // 4
    o = 0

    def unpack565(c):
        r = ((c >> 11) & 31) * 255 // 31
        g = ((c >> 5) & 63) * 255 // 63
        b = (c & 31) * 255 // 31
        return r, g, b

    for by in range(by_n):
        for bx in range(bx_n):
            a0, a1 = data[o], data[o + 1]
            abits = int.from_bytes(data[o + 2 : o + 8], "little")
            c0 = int.from_bytes(data[o + 8 : o + 10], "little")
            c1 = int.from_bytes(data[o + 10 : o + 12], "little")
            bits = int.from_bytes(data[o + 12 : o + 16], "little")
            o += 16
            r0, g0, b0 = unpack565(c0)
            r1, g1, b1 = unpack565(c1)
            if c0 > c1:
                colors = [
                    (r0, g0, b0),
                    (r1, g1, b1),
                    ((2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3),
                    ((r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3),
                ]
            else:
                colors = [
                    (r0, g0, b0),
                    (r1, g1, b1),
                    ((r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2),
                    (0, 0, 0),
                ]
            alphas = [a0, a1]
            if a0 > a1:
                for i in range(6):
                    alphas.append(((6 - i) * a0 + (i + 1) * a1) // 7)
            else:
                for i in range(4):
                    alphas.append(((4 - i) * a0 + (i + 1) * a1) // 5)
                alphas.extend([0, 255])
            for py in range(4):
                for px in range(4):
                    x = bx * 4 + px
                    y = by * 4 + py
                    if x >= w or y >= h:
                        continue
                    ci = (bits >> (2 * (py * 4 + px))) & 3
                    ai = (abits >> (3 * (py * 4 + px))) & 7
                    r, g, b = colors[ci]
                    a = alphas[ai]
                    i = (y * w + x) * 4
                    out[i : i + 4] = bytes((r, g, b, 255 if a < 8 else a))
    return bytes(out)


def downsample_rgba(src: bytes, sw: int, sh: int, dw: int, dh: int) -> bytes:
    out = bytearray(dw * dh * 4)
    for y in range(dh):
        sy0 = y * sh // dh
        sy1 = min(sh, (y + 1) * sh // dh)
        for x in range(dw):
            sx0 = x * sw // dw
            sx1 = min(sw, (x + 1) * sw // dw)
            r = g = b = a = n = 0
            for sy in range(sy0, max(sy0 + 1, sy1)):
                row = sy * sw * 4
                for sx in range(sx0, max(sx0 + 1, sx1)):
                    i = row + sx * 4
                    r += src[i]
                    g += src[i + 1]
                    b += src[i + 2]
                    a += src[i + 3]
                    n += 1
            n = max(n, 1)
            o = (y * dw + x) * 4
            out[o : o + 4] = bytes((r // n, g // n, b // n, 255))
    return bytes(out)


def dxt5_mip_size(w: int, h: int) -> int:
    return ((w + 3) // 4) * ((h + 3) // 4) * 16


def vtf_highres_offset(data: bytes) -> int:
    header_size = struct.unpack_from("<I", data, 12)[0]
    version_minor = struct.unpack_from("<I", data, 8)[0]
    if version_minor >= 3 and header_size >= 96:
        num_res = struct.unpack_from("<I", data, 68)[0]
        if 0 < num_res < 32:
            for i in range(num_res):
                off = 80 + i * 8
                if off + 8 > len(data):
                    break
                # ResourceEntryInfo: 3-byte tag + flags, then offset.
                # 0x30 = high-res IMAGE.
                if data[off] == 0x30:
                    return struct.unpack_from("<I", data, off + 4)[0]
    low_w, low_h = data[61], data[62]
    thumb = 0
    if low_w and low_h:
        thumb = ((low_w + 3) // 4) * ((low_h + 3) // 4) * 8
    return header_size + thumb


def vtf_largest_mip_offset(data: bytes, highres_off: int, w: int, h: int, mip_count: int) -> int:
    """VTF high-res data is smallest mip first, largest last."""
    skip = 0
    mw, mh = max(1, w // 2), max(1, h // 2)
    for _ in range(max(0, mip_count - 1)):
        skip += dxt5_mip_size(max(1, mw), max(1, mh))
        mw = max(1, mw // 2)
        mh = max(1, mh // 2)
    return highres_off + skip


def load_vtf_png_bytes(path: str, max_w=1024, max_h=512) -> bytes:
    data = open(path, "rb").read()
    w, h = struct.unpack_from("<HH", data, 16)
    fmt = struct.unpack_from("<i", data, 52)[0]
    mip_count = data[56]
    if fmt != 15:
        raise RuntimeError(f"unsupported VTF format {fmt}")
    highres = vtf_highres_offset(data)
    payload = data[vtf_largest_mip_offset(data, highres, w, h, mip_count) :]
    print(f"decoding DXT5 {w}x{h} mips={mip_count} (this takes a few seconds)")
    rgba = decode_dxt5(payload, w, h)
    dw, dh = max_w, max_h
    rgba = downsample_rgba(rgba, w, h, dw, dh)
    return encode_png(rgba, dw, dh)


def encode_png(rgba: bytes, w: int, h: int) -> bytes:
    def chunk(tag: bytes, payload: bytes):
        crc = zlib.crc32(tag)
        crc = zlib.crc32(payload, crc) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", crc)

    raw = b""
    stride = w * 4
    for y in range(h):
        raw += b"\x00" + rgba[y * stride : (y + 1) * stride]
    compressed = zlib.compress(raw, 9)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", compressed) + chunk(b"IEND", b"")


FINGER_SRC = [
    ("thumb", "Finger0"),
    ("index", "Finger1"),
    ("middle", "Finger2"),
    ("ring", "Finger3"),
    ("pinky", "Finger4"),
]


def joint_plan(side: str):
    """side is L or R. SteamVR suffix is l/r."""
    suf = "l" if side == "L" else "r"
    names = [f"wrist_{suf}"]
    parents = [-1]
    src = [f"ValveBiped.Bip01_{side}_Hand"]
    for steam, vb in FINGER_SRC:
        for seg, tail in enumerate(("", "1", "2")):
            names.append(f"finger_{steam}_{seg}_{suf}")
            if seg == 0:
                parents.append(0)
                src.append(f"ValveBiped.Bip01_{side}_{vb}")
            else:
                parents.append(len(names) - 2)
                src.append(f"ValveBiped.Bip01_{side}_{vb}{tail}")
    return names, parents, src


def dominant_bone(v):
    best_i = 0
    best_w = -1.0
    for i in range(min(3, v["nb"])):
        if v["weights"][i] > best_w:
            best_w = v["weights"][i]
            best_i = v["bones"][i]
    return best_i


def pad4(n: int) -> int:
    return (n + 3) & ~3


def write_glb(
    path: str,
    vertices,
    indices,
    joint_names,
    joint_parents,
    bind_mats,
    png: bytes,
    material_name="v_hand",
    generator="rip_hev_gloves.py",
):
    # column-major float32 buffers
    def f32(seq):
        return b"".join(struct.pack("<f", float(x)) for x in seq)

    pos = []
    nrm = []
    uv = []
    jnt = []
    wts = []
    for v in vertices:
        pos.extend(v["pos"])
        nrm.extend(v["nrm"])
        uv.extend(v["uv"])
        jnt.extend(v["joints"] + (0,) * (4 - len(v["joints"])))
        wts.extend(v["weights"] + (0.0,) * (4 - len(v["weights"])))

    pos_b = f32(pos)
    nrm_b = f32(nrm)
    uv_b = f32(uv)
    jnt_b = b"".join(struct.pack("<HHHH", *tuple(jnt[i : i + 4])) for i in range(0, len(jnt), 4))
    wts_b = f32(wts)
    idx_b = b"".join(struct.pack("<H", i) for i in indices)
    ibm_b = f32([c for m in bind_mats for c in mat4_invert(m)])

    blob = b""
    views = []

    def add_view(data: bytes, target=None):
        nonlocal blob
        while len(blob) % 4:
            blob += b"\x00"
        off = len(blob)
        blob += data
        while len(blob) % 4:
            blob += b"\x00"
        view = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        views.append(view)
        return len(views) - 1

    img_view = add_view(png)
    pos_view = add_view(pos_b, 34962)
    nrm_view = add_view(nrm_b, 34962)
    uv_view = add_view(uv_b, 34962)
    jnt_view = add_view(jnt_b, 34962)
    wts_view = add_view(wts_b, 34962)
    idx_view = add_view(idx_b, 34963)
    ibm_view = add_view(ibm_b)

    nverts = len(vertices)
    accessors = [
        {
            "bufferView": pos_view,
            "componentType": 5126,
            "count": nverts,
            "type": "VEC3",
            "min": [min(pos[i::3]) for i in range(3)],
            "max": [max(pos[i::3]) for i in range(3)],
        },
        {"bufferView": nrm_view, "componentType": 5126, "count": nverts, "type": "VEC3"},
        {"bufferView": uv_view, "componentType": 5126, "count": nverts, "type": "VEC2"},
        {"bufferView": jnt_view, "componentType": 5123, "count": nverts, "type": "VEC4"},
        {"bufferView": wts_view, "componentType": 5126, "count": nverts, "type": "VEC4"},
        {"bufferView": idx_view, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
        {
            "bufferView": ibm_view,
            "componentType": 5126,
            "count": len(joint_names),
            "type": "MAT4",
        },
    ]

    nodes = []
    for i, name in enumerate(joint_names):
        p = joint_parents[i]
        if p >= 0:
            local = mat4_mul(mat4_invert(bind_mats[p]), bind_mats[i])
        else:
            local = bind_mats[i]
        nodes.append({"name": name, "matrix": local})
        if p >= 0:
            nodes[p].setdefault("children", []).append(i)

    mesh_node = len(nodes)
    nodes.append({"name": "mesh", "mesh": 0, "skin": 0})

    gltf = {
        "asset": {"version": "2.0", "generator": generator},
        "buffers": [{"byteLength": len(blob)}],
        "bufferViews": views,
        "accessors": accessors,
        "images": [{"bufferView": img_view, "mimeType": "image/png"}],
        "samplers": [{"magFilter": 9729, "minFilter": 9729, "wrapS": 10497, "wrapT": 10497}],
        "textures": [{"sampler": 0, "source": 0}],
        "materials": [
            {
                "name": material_name,
                "pbrMetallicRoughness": {
                    "baseColorTexture": {"index": 0},
                    "metallicFactor": 0.0,
                    "roughnessFactor": 1.0,
                },
            }
        ],
        "meshes": [
            {
                "primitives": [
                    {
                        "attributes": {
                            "POSITION": 0,
                            "NORMAL": 1,
                            "TEXCOORD_0": 2,
                            "JOINTS_0": 3,
                            "WEIGHTS_0": 4,
                        },
                        "indices": 5,
                        "material": 0,
                    }
                ]
            }
        ],
        "nodes": nodes,
        "skins": [
            {
                "joints": list(range(len(joint_names))),
                "inverseBindMatrices": 6,
            }
        ],
        "scenes": [{"nodes": [0, mesh_node]}],
        "scene": 0,
    }
    json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    while len(json_bytes) % 4:
        json_bytes += b" "
    while len(blob) % 4:
        blob += b"\x00"
    gltf["buffers"][0]["byteLength"] = len(blob)
    json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    while len(json_bytes) % 4:
        json_bytes += b" "

    glb = b"glTF" + struct.pack("<I", 2)
    total = 12 + 8 + len(json_bytes) + 8 + len(blob)
    glb += struct.pack("<I", total)
    glb += struct.pack("<I", len(json_bytes)) + b"JSON" + json_bytes
    glb += struct.pack("<I", len(blob)) + b"BIN\x00" + blob
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(glb)
    print(f"wrote {path} verts={len(vertices)} tris={len(indices)//3} bytes={len(glb)}")


def extract_hand(
    side: str,
    bones,
    worlds,
    verts,
    tris,
    png: bytes,
    out_stem="hev_glove",
    material_name="v_hand",
    generator="rip_hev_gloves.py",
):
    names, parents, src_names = joint_plan(side)
    name_to_idx = {b["name"]: i for i, b in enumerate(bones)}
    src_ids = []
    for n in src_names:
        if n not in name_to_idx:
            raise RuntimeError(f"missing bone {n}")
        src_ids.append(name_to_idx[n])
    src_set = set(src_ids)
    wrist_src = src_ids[0]
    forearm_name = f"ValveBiped.Bip01_{side}_Forearm"
    forearm_id = name_to_idx.get(forearm_name, -1)
    allowed = set(src_ids)
    if forearm_id >= 0:
        allowed.add(forearm_id)

    # Model space: fingers +Z (SteamVR glove, then config yaw 180 → aim),
    # +Y = back-of-hand / Source up, origin at the FRONT (fingertips) so the
    # mesh front sits on the controller tracking origin (old debug squares).
    wrist_m = worlds[wrist_src]
    suf = "l" if side == "L" else "r"
    mid_src = src_ids[names.index(f"finger_middle_2_{suf}")]
    thumb_src = src_ids[names.index(f"finger_thumb_2_{suf}")]
    tip = (worlds[mid_src][12], worlds[mid_src][13], worlds[mid_src][14])
    thumb_p = (worlds[thumb_src][12], worlds[thumb_src][13], worlds[thumb_src][14])
    wrist_p = (wrist_m[12], wrist_m[13], wrist_m[14])
    finger = (tip[0] - wrist_p[0], tip[1] - wrist_p[1], tip[2] - wrist_p[2])
    thumb = (thumb_p[0] - wrist_p[0], thumb_p[1] - wrist_p[1], thumb_p[2] - wrist_p[2])
    palm = vec_cross(finger, thumb)
    if vec_len(palm) < 1e-4:
        palm = (0.0, 0.0, 1.0)
    if vec_dot(palm, (0.0, 0.0, 1.0)) < 0.0:
        palm = (-palm[0], -palm[1], -palm[2])
    # align_at_wrist maps model → Source (fingers +Z, back-of-hand +Y, wrist origin).
    # invert(wrist)*invert(align) is the wrong product and leaves T-pose ±Y
    # (left at ceiling, right at the ground after controller yaw 180).
    align = look_basis(finger, palm)
    align[12], align[13], align[14] = wrist_p
    to_model = mat4_invert(align)

    bind = []
    for sid in src_ids:
        bind.append(mat4_mul(to_model, worlds[sid]))
        # convert translation to meters
        bind[-1][12] /= SOURCE_UNITS_PER_METER
        bind[-1][13] /= SOURCE_UNITS_PER_METER
        bind[-1][14] /= SOURCE_UNITS_PER_METER

    src_to_joint = {src_ids[i]: i for i in range(len(src_ids))}
    src_to_joint[forearm_id] = 0

    used = set()
    for a, b, c in tris:
        if dominant_bone(verts[a]) in allowed or dominant_bone(verts[b]) in allowed or dominant_bone(verts[c]) in allowed:
            used.add(a)
            used.add(b)
            used.add(c)

    remap = {}
    out_verts = []
    for vi in sorted(used):
        v = verts[vi]
        px, py, pz = v["pos"]
        nx, ny, nz = v["nrm"]
        mx, my, mz = mat4_mul_vec3(to_model, px, py, pz, 1.0)
        nnx, nny, nnz = mat4_mul_vec3(to_model, nx, ny, nz, 0.0)
        joints = []
        weights = []
        for k in range(min(3, v["nb"])):
            bid = v["bones"][k]
            j = src_to_joint.get(bid, 0 if bid in allowed else -1)
            if j < 0:
                continue
            joints.append(j)
            weights.append(v["weights"][k])
        if not joints:
            joints = [0]
            weights = [1.0]
        s = sum(weights) or 1.0
        weights = [w / s for w in weights]
        while len(joints) < 4:
            joints.append(0)
            weights.append(0.0)
        remap[vi] = len(out_verts)
        out_verts.append(
            {
                "pos": (mx / SOURCE_UNITS_PER_METER, my / SOURCE_UNITS_PER_METER, mz / SOURCE_UNITS_PER_METER),
                "nrm": vec_norm((nnx, nny, nnz)),
                "uv": v["uv"],
                "joints": tuple(joints[:4]),
                "weights": tuple(weights[:4]),
            }
        )

    out_idx = []
    for a, b, c in tris:
        if a not in remap or b not in remap or c not in remap:
            continue
        ia, ib, ic = remap[a], remap[b], remap[c]
        if ia == ib or ib == ic or ia == ic:
            continue
        out_idx.extend((ia, ib, ic))

    # Put the visual front (fingertips) at the origin so it sits on the
    # controller tracking point / old debug squares. Wrist goes to -Z.
    if out_verts:
        xs = [v["pos"][0] for v in out_verts]
        ys = [v["pos"][1] for v in out_verts]
        zs = [v["pos"][2] for v in out_verts]
        max_z = max(zs)
        for v in out_verts:
            p = v["pos"]
            v["pos"] = (p[0], p[1], p[2] - max_z)
        for m in bind:
            m[14] -= max_z
        print(
            f"{side} aabb x=[{min(xs):.3f},{max(xs):.3f}] y=[{min(ys):.3f},{max(ys):.3f}] "
            f"z=[{min(zs) - max_z:.3f},{0.0:.3f}] wrist=({bind[0][12]:.3f},{bind[0][13]:.3f},{bind[0][14]:.3f})"
        )

    out = os.path.join(OUT_DIR, f"{out_stem}_{'left' if side == 'L' else 'right'}_model.glb")
    write_glb(
        out,
        out_verts,
        out_idx,
        names,
        parents,
        bind,
        png,
        material_name=material_name,
        generator=generator,
    )
    return out


def run_rip(
    mdl_path,
    vvd_path,
    vtx_path,
    vtf_path,
    mesh_vertex_offsets,
    out_stem,
    material_name,
    generator,
    png_w,
    png_h,
):
    for p in (mdl_path, vvd_path, vtx_path, vtf_path):
        if not os.path.isfile(p):
            print("missing", p)
            return 1
    mdl = open(mdl_path, "rb").read()
    vvd = open(vvd_path, "rb").read()
    vtx = open(vtx_path, "rb").read()
    bones, worlds = load_bones(mdl)
    verts = load_vertices(vvd)
    tris = load_indices(vtx, mesh_vertex_offsets)
    print(f"bones={len(bones)} verts={len(verts)} tris={len(tris)}")
    if len(tris) < 100:
        print("VTX parse produced too few triangles; aborting")
        return 1
    png = load_vtf_png_bytes(vtf_path, png_w, png_h)
    extract_hand("L", bones, worlds, verts, tris, png, out_stem, material_name, generator)
    extract_hand("R", bones, worlds, verts, tris, png, out_stem, material_name, generator)
    return 0


def main():
    return run_rip(
        MDL,
        VVD,
        VTX,
        VTF,
        [0, 3006],
        "hev_glove",
        "v_hand",
        "rip_hev_gloves.py",
        1024,
        512,
    )


if __name__ == "__main__":
    sys.exit(main())
