#include "vr.h"
#include "game.h"
#include "in_buttons.h"
#include "bmvr_flags.h"
#include "openxr_helper_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <d3d9.h>

namespace
{
    constexpr int kMaxMenuSlots = 16;
    constexpr DWORD kMenuHoldMs = 180;
    constexpr float kMenuHandForwardHu = 11.f;
    constexpr float kMenuHandUpHu = 5.f;

    void YawAroundZ(Vector& v, float yawDeg)
    {
        const float rad = yawDeg * (3.14159265f / 180.f);
        const float s = sinf(rad);
        const float c = cosf(rad);
        const float nx = v.x * c - v.y * s;
        const float ny = v.x * s + v.y * c;
        v.x = nx;
        v.y = ny;
    }

    Vector MenuPlayerBody(const VR* vr)
    {
        Vector body = vr->m_HasStereoBodyOrigin ? vr->m_StereoBodyOrigin : vr->m_SetupOrigin;
        if (body.LengthSqr() <= 1.f)
            body = vr->m_SetupOrigin;
        return body;
    }

    void BillboardFacingEye(const Vector& origin, const Vector& eye, const Vector& hintUp,
        const Vector& hintRight, Vector& fwd, Vector& right, Vector& up)
    {
        fwd = eye - origin;
        const float len = fwd.Length();
        if (len < 0.5f)
            fwd = CrossProduct(hintRight, hintUp);
        else
            fwd *= (1.f / len);
        right = CrossProduct(hintUp, fwd);
        if (right.LengthSqr() < 1e-4f)
            right = hintRight;
        VectorNormalize(right);
        up = CrossProduct(fwd, right);
        VectorNormalize(up);
    }
    constexpr float kHexRadiusPxAt1440 = 76.f;
    constexpr float kHexPackScale = 1.20f;
    constexpr float kHexWorldHu = 3.55f;
    constexpr float kSqrt3 = 1.73205078f;

    int ReadMuzzleFlashParity(void* vm)
    {
        if (!vm)
            return -1;
        int parity = -1;
        __try
        {
            parity = *reinterpret_cast<const unsigned char*>(
                reinterpret_cast<uintptr_t>(vm) + 0x9F8);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            parity = -1;
        }
        return parity;
    }

    enum WeaponKind
    {
        KindUnknown = 0,
        KindCrowbar,
        KindGlock,
        KindRevolver,
        KindMp5,
        KindShotgun,
        KindCrossbow,
        KindRpg,
        KindGauss,
        KindGluon,
        KindHivehand,
        KindGrenade,
        KindSatchel,
        KindTripmine,
        KindSnark,
        KindCount
    };

    struct MenuVert
    {
        float x, y, z, rhw;
        D3DCOLOR color;
    };

    struct MenuVertTex
    {
        float x, y, z, rhw;
        D3DCOLOR color;
        float u, v;
    };

    void LowerCopy(char* dst, size_t dstSize, const char* src)
    {
        if (!dst || dstSize == 0)
            return;
        size_t n = 0;
        if (src)
        {
            for (; n + 1 < dstSize && src[n]; ++n)
                dst[n] = static_cast<char>(tolower(static_cast<unsigned char>(src[n])));
        }
        dst[n] = 0;
    }

    bool NameHas(const char* lower, const char* token)
    {
        return lower && token && std::strstr(lower, token) != nullptr;
    }

    WeaponKind KindFromNames(const char* model, const char* net)
    {
        char m[128]{};
        char n[80]{};
        LowerCopy(m, sizeof(m), model);
        LowerCopy(n, sizeof(n), net);
        auto has = [&](const char* t) { return NameHas(m, t) || NameHas(n, t); };
        if (has("crowbar") || has("wrench"))
            return KindCrowbar;
        if (has("glock") || has("pistol") || has("9mm") || has("beretta"))
            return KindGlock;
        if (has("357") || has("python") || has("revolver"))
            return KindRevolver;
        if (has("mp5") || has("smg") || has("mp5k"))
            return KindMp5;
        if (has("shotgun") || has("spas") || has("pump"))
            return KindShotgun;
        if (has("crossbow"))
            return KindCrossbow;
        if (has("rpg") || has("rocket"))
            return KindRpg;
        if (has("gauss") || has("tau"))
            return KindGauss;
        if (has("egon") || has("gluon"))
            return KindGluon;
        if (has("hgun") || has("hive") || has("hornet"))
            return KindHivehand;
        if (has("grenade") || has("frag"))
            return KindGrenade;
        if (has("satchel"))
            return KindSatchel;
        if (has("tripmine") || has("trip"))
            return KindTripmine;
        if (has("squeak") || has("snark"))
            return KindSnark;
        return KindUnknown;
    }

    const char* LabelForKind(WeaponKind kind)
    {
        switch (kind)
        {
        case KindCrowbar: return "CROWBAR";
        case KindGlock: return "GLOCK";
        case KindRevolver: return "357";
        case KindMp5: return "MP5";
        case KindShotgun: return "SHOTGUN";
        case KindCrossbow: return "XBOW";
        case KindRpg: return "RPG";
        case KindGauss: return "TAU";
        case KindGluon: return "GLUON";
        case KindHivehand: return "HIVE";
        case KindGrenade: return "FRAG";
        case KindSatchel: return "SATCHEL";
        case KindTripmine: return "TRIP";
        case KindSnark: return "SNARK";
        default: return "WEAPON";
        }
    }

    const char* DrawSoundForKind(WeaponKind kind)
    {
        // scripts/game_sounds from bms_misc_000.vpk. Missing names fall back
        // to 2D common/wpn_select.wav in FlushPendingWeaponSounds.
        switch (kind)
        {
        case KindGlock: return "weapon_glock.Draw";
        case KindRevolver: return "weapon_357.draw";
        case KindMp5: return "weapon_mp5.Draw";
        case KindShotgun: return "weapon_shotgun.draw";
        case KindCrossbow: return "weapon_crossbow.Draw";
        case KindRpg: return "weapon_rpg.Draw";
        case KindGauss: return "weapon_tau.Draw";
        case KindGrenade: return "weapon_frag.Draw";
        case KindSnark: return "weapon_snark.draw";
        default: return nullptr;
        }
    }

    const char* HudVtfNameForKind(WeaponKind kind)
    {
        switch (kind)
        {
        case KindCrowbar: return "weapon_crowbar.vtf";
        case KindGlock: return "weapon_glock.vtf";
        case KindRevolver: return "weapon_357.vtf";
        case KindMp5: return "weapon_mp5.vtf";
        case KindShotgun: return "weapon_shotgun.vtf";
        case KindCrossbow: return "weapon_crossbow.vtf";
        case KindRpg: return "weapon_rpg.vtf";
        case KindGauss: return "weapon_tau.vtf";
        case KindGluon: return "weapon_gluon.vtf";
        case KindHivehand: return "weapon_hivehand.vtf";
        case KindGrenade: return "weapon_frag.vtf";
        case KindSatchel: return "weapon_satchel.vtf";
        case KindTripmine: return "weapon_tripmine.vtf";
        case KindSnark: return "weapon_snark.vtf";
        default: return nullptr;
        }
    }

    // HLA/HL2VR flower: empty center, inner ring of 6, outer 5 (no 6 o'clock).
    constexpr int kLayoutCells[][2] = {
        {1, 0}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}, {0, 1},
        {-1, 2}, {1, 1}, {-2, 1}, {2, -1}, {0, -2}
    };

    bool IsLayoutCell(int q, int r)
    {
        for (const auto& c : kLayoutCells)
        {
            if (c[0] == q && c[1] == r)
                return true;
        }
        return false;
    }

    void HexSlotForKind(WeaponKind kind, int& q, int& r)
    {
        switch (kind)
        {
        case KindGlock: q = 0; r = -1; break;
        case KindRevolver: q = 0; r = -2; break;
        case KindMp5: q = 1; r = -1; break;
        case KindGauss: q = 1; r = 1; break;
        case KindShotgun: q = 1; r = 0; break;
        case KindCrossbow: q = 2; r = -1; break;
        case KindCrowbar: q = 0; r = 1; break;
        case KindRpg: q = -1; r = 1; break;
        case KindGluon: q = -1; r = 0; break;
        case KindHivehand: q = -2; r = 1; break;
        case KindGrenade: q = -1; r = 2; break;
        default: q = 99; r = 99; break;
        }
    }

    void HexToOffset(int q, int r, float radius, float& x, float& y)
    {
        x = radius * (kSqrt3 * (static_cast<float>(q) + static_cast<float>(r) * 0.5f));
        y = radius * (1.5f * static_cast<float>(r));
    }

    float HexDrawRadiusPx(UINT h)
    {
        const float hh = (h > 8) ? static_cast<float>(h) : 1440.f;
        return kHexRadiusPxAt1440 * (hh / 1440.f);
    }

    void HexScreenCenter(float ocx, float ocy, int q, int r, float packR, float& cx, float& cy)
    {
        float ox = 0.f, oy = 0.f;
        HexToOffset(q, r, packR, ox, oy);
        cx = ocx + ox;
        cy = ocy - oy;
    }

    bool ProjectWorldToScreen(const Vector& world, const Vector& eye, const Vector& fwd,
        const Vector& right, const Vector& up, float tanHalf, float aspect, UINT w, UINT h,
        float& sx, float& sy)
    {
        const Vector delta = world - eye;
        const float z = delta.Dot(fwd);
        if (z < 3.f)
            return false;
        const float x = delta.Dot(right);
        const float y = delta.Dot(up);
        const float ndcX = (x / z) / tanHalf;
        const float ndcY = ((y / z) * aspect) / tanHalf;
        sx = (ndcX * 0.5f + 0.5f) * static_cast<float>(w);
        sy = (-ndcY * 0.5f + 0.5f) * static_cast<float>(h);
        return true;
    }

    bool PointInHex(float x, float y, float radius)
    {
        const float px = fabsf(x);
        const float py = fabsf(y);
        if (py > radius)
            return false;
        if (px > radius * (kSqrt3 * 0.5f))
            return false;
        return (px / kSqrt3 + py) <= radius + 0.001f;
    }

    void MenuQuad(IDirect3DDevice9* device, float x, float y, float w, float h, D3DCOLOR color)
    {
        MenuVert v[4] = {
            { x, y, 0.f, 1.f, color },
            { x + w, y, 0.f, 1.f, color },
            { x, y + h, 0.f, 1.f, color },
            { x + w, y + h, 0.f, 1.f, color }
        };
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(MenuVert));
    }

    void MenuLine(IDirect3DDevice9* device, float x0, float y0, float x1, float y1, float t, D3DCOLOR color)
    {
        float dx = x1 - x0;
        float dy = y1 - y0;
        const float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.5f)
            return;
        dx /= len;
        dy /= len;
        const float px = -dy * t * 0.5f;
        const float py = dx * t * 0.5f;
        MenuVert v[4] = {
            { x0 + px, y0 + py, 0.f, 1.f, color },
            { x1 + px, y1 + py, 0.f, 1.f, color },
            { x0 - px, y0 - py, 0.f, 1.f, color },
            { x1 - px, y1 - py, 0.f, 1.f, color }
        };
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(MenuVert));
    }

    void DrawHexFill(IDirect3DDevice9* device, float cx, float cy, float r, D3DCOLOR color)
    {
        MenuVert v[8]{};
        v[0] = { cx, cy, 0.f, 1.f, color };
        for (int i = 0; i < 7; ++i)
        {
            const float a = (static_cast<float>(i) * 60.f - 90.f) * 3.14159265f / 180.f;
            v[i + 1] = { cx + cosf(a) * r, cy + sinf(a) * r, 0.f, 1.f, color };
        }
        device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 6, v, sizeof(MenuVert));
    }

    // Continuous hex ring (triangle strip), not six separate quads that gap at vertices.
    void DrawHexRing(IDirect3DDevice9* device, float cx, float cy, float rOuter, float rInner, D3DCOLOR color)
    {
        if (!(rOuter > rInner + 0.5f))
            return;
        MenuVert v[14]{};
        for (int i = 0; i <= 6; ++i)
        {
            const float a = (static_cast<float>(i) * 60.f - 90.f) * 3.14159265f / 180.f;
            const float c = cosf(a);
            const float s = sinf(a);
            v[i * 2] = { cx + c * rOuter, cy + s * rOuter, 0.f, 1.f, color };
            v[i * 2 + 1] = { cx + c * rInner, cy + s * rInner, 0.f, 1.f, color };
        }
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 12, v, sizeof(MenuVert));
    }

    bool ReadFileBytes(const wchar_t* path, std::vector<unsigned char>& out)
    {
        HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 8 * 1024 * 1024)
        {
            CloseHandle(h);
            return false;
        }
        out.resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        const BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
        CloseHandle(h);
        return ok && got == out.size();
    }

    bool ExtractVpkFile(const wchar_t* dirVpk, const char* rel, std::vector<unsigned char>& out)
    {
        std::vector<unsigned char> raw;
        if (!ReadFileBytes(dirVpk, raw) || raw.size() < 32)
            return false;
        unsigned sig = 0, ver = 0, treeSize = 0;
        memcpy(&sig, raw.data(), 4);
        memcpy(&ver, raw.data() + 4, 4);
        memcpy(&treeSize, raw.data() + 8, 4);
        if (sig != 0x55AA1234)
            return false;
        size_t header = (ver == 2) ? 28u : 12u;
        if (header + treeSize > raw.size())
            return false;
        const unsigned char* tree = raw.data() + header;
        size_t i = 0;
        auto readZ = [&]() -> std::string {
            std::string s;
            while (i < treeSize && tree[i])
            {
                s.push_back(static_cast<char>(tree[i]));
                ++i;
            }
            if (i < treeSize)
                ++i;
            return s;
        };
        std::string want = rel;
        for (char& c : want)
        {
            if (c == '\\')
                c = '/';
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }
        unsigned archive = 0x7FFF, offset = 0, length = 0, preload = 0;
        bool found = false;
        std::vector<unsigned char> preloadBytes;
        while (i < treeSize)
        {
            const std::string ext = readZ();
            if (ext.empty())
                break;
            while (i < treeSize)
            {
                const std::string path = readZ();
                if (path.empty())
                    break;
                while (i < treeSize)
                {
                    const std::string name = readZ();
                    if (name.empty())
                        break;
                    if (i + 18 > treeSize)
                        return false;
                    unsigned crc = 0;
                    unsigned short pre = 0, arch = 0, term = 0;
                    unsigned off = 0, len = 0;
                    memcpy(&crc, tree + i, 4);
                    memcpy(&pre, tree + i + 4, 2);
                    memcpy(&arch, tree + i + 6, 2);
                    memcpy(&off, tree + i + 8, 4);
                    memcpy(&len, tree + i + 12, 4);
                    memcpy(&term, tree + i + 16, 2);
                    i += 18;
                    const unsigned char* prePtr = tree + i;
                    i += pre;
                    std::string key = path + "/" + name + "." + ext;
                    for (char& c : key)
                    {
                        if (c == '\\')
                            c = '/';
                        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                    }
                    if (!found && key == want)
                    {
                        found = true;
                        archive = arch;
                        offset = off;
                        length = len;
                        preload = pre;
                        preloadBytes.assign(prePtr, prePtr + pre);
                    }
                    (void)crc;
                    (void)term;
                }
            }
        }
        if (!found || length == 0)
            return false;
        if (archive == 0x7FFF)
        {
            out.assign(preloadBytes.begin(), preloadBytes.begin() + (std::min)(preloadBytes.size(), static_cast<size_t>(length)));
            return !out.empty();
        }
        std::wstring dataPath(dirVpk);
        const size_t under = dataPath.rfind(L"_dir.vpk");
        if (under == std::wstring::npos)
            return false;
        wchar_t num[16]{};
        swprintf_s(num, L"_%03u.vpk", archive);
        dataPath.replace(under, wcslen(L"_dir.vpk"), num);
        HANDLE h = CreateFileW(dataPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER pos{};
        pos.QuadPart = offset;
        if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN))
        {
            CloseHandle(h);
            return false;
        }
        out.resize(length);
        if (preload > 0 && preload <= length)
            memcpy(out.data(), preloadBytes.data(), preload);
        DWORD got = 0;
        const DWORD need = length - preload;
        const BOOL ok = need == 0 || ReadFile(h, out.data() + preload, need, &got, nullptr);
        CloseHandle(h);
        return ok && (need == 0 || got == need);
    }

    void Unpack565(unsigned short c, unsigned char* rgb)
    {
        rgb[0] = static_cast<unsigned char>(((c >> 11) & 31) * 255 / 31);
        rgb[1] = static_cast<unsigned char>(((c >> 5) & 63) * 255 / 63);
        rgb[2] = static_cast<unsigned char>((c & 31) * 255 / 31);
    }

    void DecodeDxtColors(const unsigned char* block, unsigned char px[16][4], bool dxt1)
    {
        const unsigned short c0 = static_cast<unsigned short>(block[0] | (block[1] << 8));
        const unsigned short c1 = static_cast<unsigned short>(block[2] | (block[3] << 8));
        unsigned char rgb[4][3]{};
        Unpack565(c0, rgb[0]);
        Unpack565(c1, rgb[1]);
        if (c0 > c1 || !dxt1)
        {
            for (int i = 0; i < 3; ++i)
            {
                rgb[2][i] = static_cast<unsigned char>((2 * rgb[0][i] + rgb[1][i]) / 3);
                rgb[3][i] = static_cast<unsigned char>((rgb[0][i] + 2 * rgb[1][i]) / 3);
            }
        }
        else
        {
            for (int i = 0; i < 3; ++i)
                rgb[2][i] = static_cast<unsigned char>((rgb[0][i] + rgb[1][i]) / 2);
            rgb[3][0] = rgb[3][1] = rgb[3][2] = 0;
        }
        const unsigned bits = block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24);
        for (int i = 0; i < 16; ++i)
        {
            const int idx = static_cast<int>((bits >> (2 * i)) & 3u);
            px[i][0] = rgb[idx][2];
            px[i][1] = rgb[idx][1];
            px[i][2] = rgb[idx][0];
            px[i][3] = (dxt1 && c0 <= c1 && idx == 3) ? 0 : 255;
        }
    }

    void DecodeDxt5Alpha(const unsigned char* block, unsigned char a[16])
    {
        const unsigned a0 = block[0];
        const unsigned a1 = block[1];
        unsigned tbl[8] = { a0, a1 };
        if (a0 > a1)
        {
            for (int i = 1; i <= 6; ++i)
                tbl[i + 1] = (a0 * (7 - i) + a1 * i) / 7;
        }
        else
        {
            for (int i = 1; i <= 4; ++i)
                tbl[i + 1] = (a0 * (5 - i) + a1 * i) / 5;
            tbl[6] = 0;
            tbl[7] = 255;
        }
        unsigned long long bits = 0;
        memcpy(&bits, block + 2, 6);
        for (int i = 0; i < 16; ++i)
            a[i] = static_cast<unsigned char>(tbl[(bits >> (3 * i)) & 7u]);
    }

    size_t MipByteSize(unsigned format, int w, int h)
    {
        w = (std::max)(1, w);
        h = (std::max)(1, h);
        if (format == 13)
            return static_cast<size_t>(((w + 3) / 4) * ((h + 3) / 4) * 8);
        if (format == 14 || format == 15)
            return static_cast<size_t>(((w + 3) / 4) * ((h + 3) / 4) * 16);
        if (format == 3)
            return static_cast<size_t>(w) * static_cast<size_t>(h) * 3u;
        return static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    }

    bool DecodeVtfToBgra(const std::vector<unsigned char>& vtf, int& w, int& h,
        std::vector<unsigned char>& bgra, unsigned* formatOut)
    {
        if (vtf.size() < 80 || memcmp(vtf.data(), "VTF\0", 4) != 0)
            return false;
        unsigned headerSize = 0, format = 0;
        unsigned short vw = 0, vh = 0, frames = 1;
        memcpy(&headerSize, vtf.data() + 12, 4);
        memcpy(&vw, vtf.data() + 16, 2);
        memcpy(&vh, vtf.data() + 18, 2);
        memcpy(&frames, vtf.data() + 24, 2);
        memcpy(&format, vtf.data() + 52, 4);
        const unsigned char mipCount = vtf.size() > 56 ? vtf[56] : 1;
        if (formatOut)
            *formatOut = format;
        w = vw;
        h = vh;
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024)
            return false;
        if (format != 0 && format != 3 && format != 11 && format != 12 && format != 13 && format != 15)
            return false;
        unsigned imageOff = headerSize;
        if (headerSize >= 72 && vtf.size() >= headerSize)
        {
            unsigned numRes = 0;
            memcpy(&numRes, vtf.data() + 68, 4);
            if (numRes > 0 && numRes < 16)
            {
                for (unsigned r = 0; r < numRes; ++r)
                {
                    const size_t e = 72 + r * 8;
                    if (e + 8 > vtf.size())
                        break;
                    if (vtf[e] == 0x30)
                    {
                        memcpy(&imageOff, vtf.data() + e + 4, 4);
                        break;
                    }
                }
            }
        }
        size_t skip = 0;
        for (int m = mipCount - 1; m >= 1; --m)
        {
            const int mw = (std::max)(1, w >> m);
            const int mh = (std::max)(1, h >> m);
            skip += MipByteSize(format, mw, mh) * (std::max)(1, static_cast<int>(frames));
        }
        const size_t bytes = MipByteSize(format, w, h);
        if (imageOff + skip + bytes > vtf.size())
            return false;
        const unsigned char* src = vtf.data() + imageOff + skip;
        bgra.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
        if (format == 12)
        {
            memcpy(bgra.data(), src, bytes);
            return true;
        }
        if (format == 0)
        {
            for (int i = 0; i < w * h; ++i)
            {
                bgra[i * 4 + 0] = src[i * 4 + 2];
                bgra[i * 4 + 1] = src[i * 4 + 1];
                bgra[i * 4 + 2] = src[i * 4 + 0];
                bgra[i * 4 + 3] = src[i * 4 + 3];
            }
            return true;
        }
        if (format == 11)
        {
            for (int i = 0; i < w * h; ++i)
            {
                bgra[i * 4 + 0] = src[i * 4 + 3];
                bgra[i * 4 + 1] = src[i * 4 + 2];
                bgra[i * 4 + 2] = src[i * 4 + 1];
                bgra[i * 4 + 3] = src[i * 4 + 0];
            }
            return true;
        }
        if (format == 3)
        {
            for (int i = 0; i < w * h; ++i)
            {
                bgra[i * 4 + 0] = src[i * 3 + 0];
                bgra[i * 4 + 1] = src[i * 3 + 1];
                bgra[i * 4 + 2] = src[i * 3 + 2];
                bgra[i * 4 + 3] = 255;
            }
            return true;
        }
        const bool dxt1 = (format == 13);
        const int bw = (w + 3) / 4;
        const int bh = (h + 3) / 4;
        const int blockBytes = dxt1 ? 8 : 16;
        for (int by = 0; by < bh; ++by)
        {
            for (int bx = 0; bx < bw; ++bx)
            {
                const unsigned char* block = src + (static_cast<size_t>(by) * bw + bx) * blockBytes;
                unsigned char px[16][4]{};
                unsigned char alpha[16];
                if (dxt1)
                {
                    DecodeDxtColors(block, px, true);
                }
                else
                {
                    DecodeDxt5Alpha(block, alpha);
                    DecodeDxtColors(block + 8, px, false);
                    for (int i = 0; i < 16; ++i)
                        px[i][3] = alpha[i];
                }
                for (int py = 0; py < 4; ++py)
                {
                    const int y = by * 4 + py;
                    if (y >= h)
                        break;
                    for (int px_ = 0; px_ < 4; ++px_)
                    {
                        const int x = bx * 4 + px_;
                        if (x >= w)
                            break;
                        memcpy(bgra.data() + (static_cast<size_t>(y) * w + x) * 4, px[py * 4 + px_], 4);
                    }
                }
            }
        }
        return true;
    }

    void CropBgraToAlpha(int& w, int& h, std::vector<unsigned char>& bgra)
    {
        if (w <= 0 || h <= 0 || bgra.size() < static_cast<size_t>(w) * h * 4u)
            return;
        int minx = w, miny = h, maxx = -1, maxy = -1;
        for (int y = 0; y < h; ++y)
        {
            const unsigned char* row = bgra.data() + static_cast<size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x)
            {
                if (row[x * 4 + 3] <= 24)
                    continue;
                if (x < minx) minx = x;
                if (y < miny) miny = y;
                if (x > maxx) maxx = x;
                if (y > maxy) maxy = y;
            }
        }
        if (maxx < minx || maxy < miny)
            return;
        minx = (std::max)(0, minx - 2);
        miny = (std::max)(0, miny - 2);
        maxx = (std::min)(w - 1, maxx + 2);
        maxy = (std::min)(h - 1, maxy + 2);
        const int nw = maxx - minx + 1;
        const int nh = maxy - miny + 1;
        if (nw >= w && nh >= h)
            return;
        std::vector<unsigned char> cropped(static_cast<size_t>(nw) * nh * 4u);
        for (int y = 0; y < nh; ++y)
        {
            memcpy(cropped.data() + static_cast<size_t>(y) * nw * 4,
                bgra.data() + (static_cast<size_t>(miny + y) * w + minx) * 4,
                static_cast<size_t>(nw) * 4u);
        }
        bgra.swap(cropped);
        w = nw;
        h = nh;
    }

    bool UploadBgraTexture(IDirect3DDevice9* device, int w, int h,
        const std::vector<unsigned char>& bgra, IDirect3DTexture9** out)
    {
        *out = nullptr;
        if (!device || w <= 0 || h <= 0)
            return false;
        IDirect3DTexture9* sys = nullptr;
        IDirect3DTexture9* gpu = nullptr;
        if (FAILED(device->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) || !sys)
            return false;
        D3DLOCKED_RECT lr{};
        if (FAILED(sys->LockRect(0, &lr, nullptr, 0)))
        {
            sys->Release();
            return false;
        }
        auto* dst = static_cast<unsigned char*>(lr.pBits);
        for (int y = 0; y < h; ++y)
            memcpy(dst + y * lr.Pitch, bgra.data() + static_cast<size_t>(y) * w * 4, static_cast<size_t>(w) * 4u);
        sys->UnlockRect(0);
        if (FAILED(device->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &gpu, nullptr)) || !gpu)
        {
            sys->Release();
            return false;
        }
        const HRESULT hr = device->UpdateTexture(sys, gpu);
        sys->Release();
        if (FAILED(hr))
        {
            gpu->Release();
            return false;
        }
        *out = gpu;
        return true;
    }

    IDirect3DTexture9* g_WeaponIconTex[KindCount]{};
    IDirect3DDevice9* g_WeaponIconDevice = nullptr;
    bool g_WeaponIconTried = false;

    void ReleaseWeaponIcons()
    {
        for (int i = 0; i < KindCount; ++i)
        {
            if (g_WeaponIconTex[i])
            {
                g_WeaponIconTex[i]->Release();
                g_WeaponIconTex[i] = nullptr;
            }
        }
        g_WeaponIconDevice = nullptr;
        g_WeaponIconTried = false;
    }

    std::vector<std::wstring> HudVpkPaths()
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            dir.resize(slash);
        const wchar_t* names[] = {
            L"\\bms\\bms_textures_dir.vpk",
            L"\\bms\\bms_misc_dir.vpk",
            L"\\bms\\bms_materials_dir.vpk",
            L"\\bms_textures_dir.vpk",
            L"\\bms_misc_dir.vpk"
        };
        std::vector<std::wstring> out;
        for (const wchar_t* n : names)
        {
            std::wstring p = dir + n;
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                out.push_back(std::move(p));
        }
        return out;
    }

    bool ExtractHudVtf(const std::vector<std::wstring>& vpks, WeaponKind kind,
        std::vector<unsigned char>& vtf)
    {
        const char* name = HudVtfNameForKind(kind);
        if (!name)
            return false;
        char rels[4][96]{};
        sprintf_s(rels[0], "materials/vgui/hud/%s", name);
        sprintf_s(rels[1], "materials/vgui/hud/weapons/%s", name);
        sprintf_s(rels[2], "materials/hud/%s", name);
        const char* bare = name;
        if (strncmp(name, "weapon_", 7) == 0)
            bare = name + 7;
        sprintf_s(rels[3], "materials/vgui/hud/%s", bare);
        for (const auto& vpk : vpks)
        {
            for (const auto& rel : rels)
            {
                if (rel[0] && ExtractVpkFile(vpk.c_str(), rel, vtf))
                    return true;
            }
        }
        return false;
    }

    void EnsureWeaponIcons(IDirect3DDevice9* device)
    {
        if (!device)
            return;
        if (g_WeaponIconDevice && g_WeaponIconDevice != device)
            ReleaseWeaponIcons();
        if (g_WeaponIconTried)
            return;
        g_WeaponIconTried = true;
        g_WeaponIconDevice = device;
        const auto vpks = HudVpkPaths();
        if (vpks.empty())
        {
            Game::logMsg("Weapon menu: no bms_*_dir.vpk next to bms.exe");
            return;
        }
        int loaded = 0;
        for (int k = 1; k < KindCount; ++k)
        {
            std::vector<unsigned char> vtf;
            if (!ExtractHudVtf(vpks, static_cast<WeaponKind>(k), vtf))
            {
                Game::logMsg("Weapon menu HUD vtf missing kind=%d %s",
                    k, HudVtfNameForKind(static_cast<WeaponKind>(k)));
                continue;
            }
            int w = 0, h = 0;
            unsigned format = 0;
            std::vector<unsigned char> bgra;
            if (!DecodeVtfToBgra(vtf, w, h, bgra, &format))
            {
                Game::logMsg("Weapon menu HUD vtf decode fail kind=%d fmt=%u size=%u",
                    k, format, static_cast<unsigned>(vtf.size()));
                continue;
            }
            CropBgraToAlpha(w, h, bgra);
            IDirect3DTexture9* tex = nullptr;
            if (!UploadBgraTexture(device, w, h, bgra, &tex) || !tex)
            {
                Game::logMsg("Weapon menu HUD upload fail kind=%d %dx%d", k, w, h);
                continue;
            }
            g_WeaponIconTex[k] = tex;
            ++loaded;
        }
        Game::logMsg("Weapon menu HUD icons loaded=%d vpks=%d", loaded, static_cast<int>(vpks.size()));
    }

    void DrawKindIcon(IDirect3DDevice9* device, float x, float y, float s, WeaponKind kind, D3DCOLOR color)
    {
        switch (kind)
        {
        case KindCrowbar:
            MenuQuad(device, x - s * 1.6f, y - s * 0.18f, s * 3.2f, s * 0.36f, color);
            MenuQuad(device, x + s * 1.1f, y - s * 0.55f, s * 0.7f, s * 0.9f, color);
            break;
        case KindGlock:
            MenuQuad(device, x - s * 1.1f, y - s * 0.22f, s * 2.4f, s * 0.44f, color);
            MenuQuad(device, x - s * 0.55f, y - s * 0.22f, s * 0.4f, s * 1.15f, color);
            break;
        case KindRevolver:
            MenuQuad(device, x - s * 1.2f, y - s * 0.28f, s * 2.6f, s * 0.55f, color);
            MenuQuad(device, x + s * 0.7f, y - s * 0.55f, s * 0.7f, s * 0.7f, color);
            break;
        case KindMp5:
            MenuQuad(device, x - s * 1.6f, y - s * 0.22f, s * 3.1f, s * 0.44f, color);
            MenuQuad(device, x - s * 0.2f, y - s * 0.22f, s * 0.4f, s * 1.05f, color);
            MenuQuad(device, x - s * 1.5f, y + s * 0.15f, s * 0.9f, s * 0.28f, color);
            break;
        case KindShotgun:
            MenuQuad(device, x - s * 1.8f, y - s * 0.18f, s * 3.6f, s * 0.36f, color);
            MenuQuad(device, x - s * 0.4f, y - s * 0.18f, s * 0.45f, s * 0.95f, color);
            break;
        case KindCrossbow:
            MenuQuad(device, x - s * 1.5f, y - s * 0.12f, s * 3.0f, s * 0.24f, color);
            MenuQuad(device, x - s * 0.15f, y - s * 0.85f, s * 0.3f, s * 1.7f, color);
            break;
        case KindRpg:
            MenuQuad(device, x - s * 1.8f, y - s * 0.28f, s * 3.6f, s * 0.55f, color);
            break;
        case KindGauss:
        case KindGluon:
            MenuQuad(device, x - s * 1.3f, y - s * 0.22f, s * 2.6f, s * 0.44f, color);
            MenuQuad(device, x + s * 0.6f, y - s * 0.7f, s * 0.5f, s * 1.4f, color);
            break;
        case KindHivehand:
            MenuQuad(device, x - s * 0.9f, y - s * 0.7f, s * 1.8f, s * 1.4f, color);
            MenuQuad(device, x - s * 0.25f, y - s * 1.1f, s * 0.5f, s * 0.5f, color);
            break;
        case KindGrenade:
            MenuQuad(device, x - s * 0.55f, y - s * 0.55f, s * 1.1f, s * 1.1f, color);
            MenuQuad(device, x - s * 0.18f, y - s * 0.95f, s * 0.36f, s * 0.45f, color);
            break;
        case KindSatchel:
        case KindTripmine:
            MenuQuad(device, x - s * 0.9f, y - s * 0.45f, s * 1.8f, s * 0.9f, color);
            break;
        case KindSnark:
            MenuQuad(device, x - s * 0.7f, y - s * 0.45f, s * 1.4f, s * 0.9f, color);
            MenuQuad(device, x - s * 0.2f, y - s * 0.85f, s * 0.4f, s * 0.45f, color);
            break;
        default:
            MenuQuad(device, x - s * 0.7f, y - s * 0.7f, s * 1.4f, s * 1.4f, color);
            break;
        }
    }

    bool RayHitHex(const Vector& origin, const Vector& dir, const Vector& center,
        const Vector& right, const Vector& up, float radius, Vector* hitOut)
    {
        const Vector n = CrossProduct(right, up);
        const float denom = dir.Dot(n);
        if (fabsf(denom) < 0.0001f)
            return false;
        const float t = (center - origin).Dot(n) / denom;
        if (t < 0.04f || t > 80.f)
            return false;
        const Vector hit = origin + dir * t;
        const Vector d = hit - center;
        if (!PointInHex(d.Dot(right), d.Dot(up), radius * 1.08f))
            return false;
        if (hitOut)
            *hitOut = hit;
        return true;
    }
}

bool VR::WeaponMenuStickHeld() const
{
    if (PressedDigitalAction(m_ActionWeaponMenu))
        return true;
    if (PressedDigitalAction(m_ActionInventoryQuickSwitch))
        return true;
    return PressedDigitalAction(m_ActionResetPosition);
}

void VR::UpdateWeaponMenu(bool stickClickHeld, float deltaMs)
{
    (void)deltaMs;
    const DWORD now = GetTickCount();
    if (!m_GameplayEligible || !m_Game || PauseUiActive())
    {
        m_WeaponMenuOpen = false;
        m_WeaponMenuClickHeld = false;
        m_WeaponMenuOpenedThisHold = false;
        m_WeaponMenuLatched = false;
        m_WeaponMenuHover = -1;
        return;
    }

    if (stickClickHeld && !m_WeaponMenuClickHeld)
    {
        m_WeaponMenuClickStartMs = now;
        m_WeaponMenuOpenedThisHold = false;
        m_WeaponMenuHover = -1;
    }

    if (stickClickHeld)
    {
        m_WeaponMenuClickHeld = true;
        if (!m_WeaponMenuOpen && (now - m_WeaponMenuClickStartMs) >= kMenuHoldMs)
        {
            m_WeaponMenuOpen = true;
            m_WeaponMenuOpenedThisHold = true;
            m_WeaponMenuLatched = false;
            PulseHandHaptic(vr::TrackedControllerRole_RightHand, 900, 0.35f);
            Game::logMsg("Weapon menu opened (right stick hold)");
        }
    }

    if (m_WeaponMenuOpen)
    {
        if (m_HmdPoseValid)
        {
            const Vector body = MenuPlayerBody(this);
            if (!m_WeaponMenuLatched)
            {
                Vector fwd, right, up;
                GetViewBasis(&fwd, &right, &up);
                Vector hand{};
                {
                    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
                    hand = ControllerTrackingToWorld(body, m_PhysicalRightPosAbs);
                }
                m_WeaponMenuLatchBody = body;
                m_WeaponMenuLatchDelta = hand + fwd * kMenuHandForwardHu + up * kMenuHandUpHu - body;
                m_WeaponMenuLatchFwd = fwd;
                m_WeaponMenuLatchRight = right;
                m_WeaponMenuLatchUp = up;
                m_WeaponMenuLatchYaw = m_RotationOffsetY.load(std::memory_order_acquire);
                m_WeaponMenuLatched = true;
            }

            const float yawDelta = m_RotationOffsetY.load(std::memory_order_acquire)
                - m_WeaponMenuLatchYaw;
            Vector delta = m_WeaponMenuLatchDelta;
            YawAroundZ(delta, yawDelta);
            m_WeaponMenuOrigin = body + delta;
            Vector hmdF, hmdR, hmdU;
            GetViewBasis(&hmdF, &hmdR, &hmdU);
            BillboardFacingEye(m_WeaponMenuOrigin, GetViewOrigin(body), hmdU, hmdR,
                m_WeaponMenuFwd, m_WeaponMenuRight, m_WeaponMenuUp);
        }

        Game::InventoryWeapon inv[kMaxMenuSlots]{};
        const int n = m_Game->CollectInventoryWeapons(inv, kMaxMenuSlots);
        C_BaseEntity* active = m_Game->GetActiveWeaponEntity();

        for (int i = 0; i < kMaxMenuSlots; ++i)
            m_WeaponMenuSlots[i] = {};
        bool taken[11][11]{};
        auto occupy = [&](int q, int r) -> bool {
            const int i = q + 5;
            const int j = r + 5;
            if (i < 0 || i >= 11 || j < 0 || j >= 11)
                return false;
            if (taken[i][j])
                return false;
            taken[i][j] = true;
            return true;
        };
        occupy(0, 0);
        m_WeaponMenuSlots[0].emptyHand = true;
        m_WeaponMenuSlots[0].axialQ = 0;
        m_WeaponMenuSlots[0].axialR = 0;
        m_WeaponMenuSlots[0].center = m_WeaponMenuOrigin;
        m_WeaponMenuSlots[0].equipped = m_EmptyHands;
        strncpy_s(m_WeaponMenuSlots[0].label, "HAND", _TRUNCATE);

        int placed = 1;
        auto placeAt = [&](int q, int r, const Game::InventoryWeapon& wpn, WeaponKind kind) {
            if (placed >= kMaxMenuSlots)
                return;
            float ox = 0.f, oy = 0.f;
            HexToOffset(q, r, kHexWorldHu, ox, oy);
            m_WeaponMenuSlots[placed].entityIndex = wpn.entityIndex;
            m_WeaponMenuSlots[placed].kind = kind;
            m_WeaponMenuSlots[placed].axialQ = q;
            m_WeaponMenuSlots[placed].axialR = r;
            m_WeaponMenuSlots[placed].center = m_WeaponMenuOrigin
                + m_WeaponMenuRight * ox + m_WeaponMenuUp * oy;
            C_BaseEntity* ent = m_Game->GetClientEntity(wpn.entityIndex);
            m_WeaponMenuSlots[placed].equipped = !m_EmptyHands && (ent == active);
            m_WeaponMenuSlots[placed].dry = (kind != KindCrowbar)
                && m_Game->WeaponHasNoAmmo(ent);
            strncpy_s(m_WeaponMenuSlots[placed].label, LabelForKind(kind), _TRUNCATE);
            ++placed;
        };

        for (int i = 0; i < n; ++i)
        {
            const WeaponKind kind = KindFromNames(inv[i].modelName, inv[i].networkName);
            int q = 0, r = 0;
            HexSlotForKind(kind, q, r);
            if (!IsLayoutCell(q, r) || !occupy(q, r))
            {
                bool found = false;
                for (const auto& c : kLayoutCells)
                {
                    if (occupy(c[0], c[1]))
                    {
                        q = c[0];
                        r = c[1];
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    static const int kOverflow[][2] = {
                        {2, 0}, {-2, 0}, {0, 2}, {1, -2}, {-1, -1},
                        {2, -2}, {-2, 2}, {2, 1}, {-2, -1}
                    };
                    for (const auto& c : kOverflow)
                    {
                        if (occupy(c[0], c[1]))
                        {
                            q = c[0];
                            r = c[1];
                            found = true;
                            break;
                        }
                    }
                }
                if (!found)
                    continue;
            }
            placeAt(q, r, inv[i], kind);
        }
        m_WeaponMenuCount = placed;

        Vector rayOrig{};
        Vector rayDir{};
        {
            std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
            Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
            if (body.LengthSqr() <= 1.f)
                body = m_SetupOrigin;
            rayOrig = ControllerTrackingToWorld(body, m_PhysicalRightPosAbs);
            QAngle::AngleVectors(m_PhysicalRightAngAbs, &rayDir, nullptr, nullptr);
        }
        const int prevHover = m_WeaponMenuHover;
        m_WeaponMenuHover = -1;
        const UINT hw = m_RenderWidth > 64 ? m_RenderWidth : 1584u;
        const UINT hh = m_RenderHeight > 64 ? m_RenderHeight : 1440u;
        const float aspect = static_cast<float>(hw) / static_cast<float>(hh);
        const float projFov = HorizontalFovForAspect(aspect);
        const float tanHalf = tanf(projFov * 0.5f * 3.14159265f / 180.f);
        const Vector eye = GetViewOrigin(MenuPlayerBody(this));
        Vector vf, vr, vu;
        GetViewBasis(&vf, &vr, &vu);
        float ocx = 0.f, ocy = 0.f;
        const bool haveOrigin = (tanHalf > 0.01f)
            && ProjectWorldToScreen(m_WeaponMenuOrigin, eye, vf, vr, vu, tanHalf, aspect, hw, hh, ocx, ocy);
        const float drawR = HexDrawRadiusPx(hh);
        const float packR = drawR * kHexPackScale;
        Vector planeHit{};
        bool haveHit = false;
        {
            const Vector n = m_WeaponMenuFwd;
            const float denom = rayDir.Dot(n);
            if (fabsf(denom) > 0.0001f)
            {
                const float t = (m_WeaponMenuOrigin - rayOrig).Dot(n) / denom;
                if (t > 0.04f && t < 80.f)
                {
                    planeHit = rayOrig + rayDir * t;
                    haveHit = true;
                }
            }
        }
        float hx = 0.f, hy = 0.f;
        if (haveHit && haveOrigin)
            haveHit = ProjectWorldToScreen(planeHit, eye, vf, vr, vu, tanHalf, aspect, hw, hh, hx, hy);
        else
            haveHit = false;
        if (haveHit)
        {
            float bestD = 1.0e9f;
            for (int i = 0; i < m_WeaponMenuCount; ++i)
            {
                float cx = 0.f, cy = 0.f;
                HexScreenCenter(ocx, ocy, m_WeaponMenuSlots[i].axialQ,
                    m_WeaponMenuSlots[i].axialR, packR, cx, cy);
                if (!PointInHex(hx - cx, hy - cy, drawR * 1.08f))
                    continue;
                const float d = (hx - cx) * (hx - cx) + (hy - cy) * (hy - cy);
                if (d < bestD)
                {
                    bestD = d;
                    m_WeaponMenuHover = i;
                }
            }
        }
        if (m_WeaponMenuHover >= 0 && m_WeaponMenuHover != prevHover)
            QueueWeaponMenuSound(kWeaponSoundHover);
    }

    if (!stickClickHeld && m_WeaponMenuClickHeld)
    {
        if (m_WeaponMenuOpenedThisHold)
        {
            if (m_WeaponMenuHover >= 0 && m_WeaponMenuHover < m_WeaponMenuCount)
            {
                const WeaponMenuSlot& slot = m_WeaponMenuSlots[m_WeaponMenuHover];
                if (slot.emptyHand)
                {
                    m_EmptyHands = true;
                    QueueWeaponMenuSound(kWeaponSoundSelect, 0, 0);
                    PulseHandHaptic(vr::TrackedControllerRole_RightHand, 1400, 0.55f);
                    Game::logMsg("Weapon menu select empty hands");
                }
                else if (slot.entityIndex > 0)
                {
                    m_EmptyHands = false;
                    m_PendingWeaponSelect.store(slot.entityIndex, std::memory_order_release);
                    QueueWeaponMenuSound(kWeaponSoundSelect, slot.kind, slot.entityIndex);
                    PulseHandHaptic(vr::TrackedControllerRole_RightHand, 1400, 0.55f);
                    Game::logMsg("Weapon menu select entity=%d %s",
                        slot.entityIndex, slot.label);
                }
            }
            else
                Game::logMsg("Weapon menu cancelled (no hover)");
        }
        else if ((now - m_WeaponMenuClickStartMs) < kMenuHoldMs)
        {
            m_HmdOriginLatched = false;
            if (bmvr::g_RecenterResetsYaw)
                m_RotationOffsetY.store(0.f, std::memory_order_release);
            Game::logMsg("Right-stick tap: recenter");
        }
        m_WeaponMenuOpen = false;
        m_WeaponMenuClickHeld = false;
        m_WeaponMenuOpenedThisHold = false;
        m_WeaponMenuLatched = false;
        m_WeaponMenuHover = -1;
    }
}

void VR::DrawWeaponMenu(IDirect3DDevice9* device, UINT w, UINT h,
    const Vector& eyeOrig, const Vector& fwd, const Vector& right, const Vector& up)
{
    if (!m_WeaponMenuOpen || !device || m_WeaponMenuCount <= 0)
        return;
    EnsureWeaponIcons(device);
    const float aspect = (h > 0) ? (static_cast<float>(w) / static_cast<float>(h)) : m_Aspect;
    const float projFov = HorizontalFovForAspect(aspect);
    const float tanHalf = tanf(projFov * 0.5f * 3.14159265f / 180.f);
    if (!(tanHalf > 0.01f))
        return;
    auto project = [&](const Vector& world, float& sx, float& sy) -> bool {
        const Vector delta = world - eyeOrig;
        const float z = delta.Dot(fwd);
        if (z < 3.f)
            return false;
        const float x = delta.Dot(right);
        const float y = delta.Dot(up);
        const float ndcX = (x / z) / tanHalf;
        const float ndcY = ((y / z) * aspect) / tanHalf;
        sx = (ndcX * 0.5f + 0.5f) * static_cast<float>(w);
        sy = (-ndcY * 0.5f + 0.5f) * static_cast<float>(h);
        return true;
    };

    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

    Vector rayOrig{};
    Vector rayDir{};
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        Vector body = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : m_SetupOrigin;
        if (body.LengthSqr() <= 1.f)
            body = m_SetupOrigin;
        rayOrig = ControllerTrackingToWorld(body, m_PhysicalRightPosAbs);
        QAngle::AngleVectors(m_PhysicalRightAngAbs, &rayDir, nullptr, nullptr);
    }
    float lx0 = 0.f, ly0 = 0.f, lx1 = 0.f, ly1 = 0.f;
    const bool haveLaser0 = project(rayOrig, lx0, ly0);
    float ocx = 0.f, ocy = 0.f;
    const bool haveOrigin = project(m_WeaponMenuOrigin, ocx, ocy);
    const float drawR = HexDrawRadiusPx(h);
    const float packR = drawR * kHexPackScale;
    bool haveLaser1 = false;
    if (m_WeaponMenuHover >= 0 && m_WeaponMenuHover < m_WeaponMenuCount && haveOrigin)
    {
        const WeaponMenuSlot& hs = m_WeaponMenuSlots[m_WeaponMenuHover];
        HexScreenCenter(ocx, ocy, hs.axialQ, hs.axialR, packR, lx1, ly1);
        haveLaser1 = true;
    }
    else
    {
        Vector laserEnd = rayOrig + rayDir * 40.f;
        haveLaser1 = project(laserEnd, lx1, ly1);
    }
    if (haveLaser0 && haveLaser1)
        MenuLine(device, lx0, ly0, lx1, ly1, 3.2f, D3DCOLOR_XRGB(255, 220, 80));

    if (!haveOrigin)
    {
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        return;
    }

    for (int i = 0; i < m_WeaponMenuCount; ++i)
    {
        const WeaponMenuSlot& slot = m_WeaponMenuSlots[i];
        const bool hover = (i == m_WeaponMenuHover);
        float cx = 0.f, cy = 0.f;
        HexScreenCenter(ocx, ocy, slot.axialQ, slot.axialR, packR, cx, cy);
        // ~8% of hex width (pointy-top width = R√3). Ring stays inside R so
        // packing ×1.20 leaves a visible gap.
        const float radius = drawR;
        const float edge = radius * 0.08f;
        const bool dry = slot.dry && !slot.emptyHand;
        const D3DCOLOR fill = dry
            ? (hover ? D3DCOLOR_ARGB(220, 72, 16, 12) : D3DCOLOR_ARGB(190, 42, 10, 8))
            : (hover ? D3DCOLOR_ARGB(210, 48, 36, 10) : D3DCOLOR_ARGB(170, 14, 12, 8));
        const D3DCOLOR frame = dry
            ? (hover ? D3DCOLOR_XRGB(255, 120, 90) : D3DCOLOR_XRGB(255, 56, 40))
            : (hover ? D3DCOLOR_XRGB(255, 240, 140)
                : (slot.equipped ? D3DCOLOR_XRGB(255, 176, 0) : D3DCOLOR_ARGB(220, 200, 140, 30)));
        DrawHexFill(device, cx, cy, radius - edge, fill);
        DrawHexRing(device, cx, cy, radius, radius - edge, frame);
        if (hover)
        {
            const D3DCOLOR glow = dry
                ? D3DCOLOR_ARGB(170, 255, 90, 70)
                : D3DCOLOR_ARGB(160, 255, 255, 200);
            DrawHexRing(device, cx, cy, radius + 1.5f, radius - edge * 0.35f, glow);
        }

        if (slot.emptyHand)
            continue;

        const int kind = slot.kind;
        IDirect3DTexture9* icon = (kind > 0 && kind < KindCount) ? g_WeaponIconTex[kind] : nullptr;
        if (icon)
        {
            const float iw = radius * 1.05f;
            const float ih = radius * 0.66f;
            const float x0 = cx - iw * 0.5f;
            const float y0 = cy - ih * 0.5f;
            const D3DCOLOR tint = dry
                ? (hover ? D3DCOLOR_XRGB(255, 170, 150) : D3DCOLOR_XRGB(255, 72, 56))
                : (hover ? D3DCOLOR_XRGB(255, 255, 255) : D3DCOLOR_XRGB(255, 230, 180));
            device->SetTexture(0, icon);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
            MenuVertTex tv[4] = {
                { x0, y0, 0.f, 1.f, tint, 0.f, 0.f },
                { x0 + iw, y0, 0.f, 1.f, tint, 1.f, 0.f },
                { x0, y0 + ih, 0.f, 1.f, tint, 0.f, 1.f },
                { x0 + iw, y0 + ih, 0.f, 1.f, tint, 1.f, 1.f }
            };
            device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tv, sizeof(MenuVertTex));
            device->SetTexture(0, nullptr);
            device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        }
        else
        {
            DrawKindIcon(device, cx, cy, radius * 0.28f,
                static_cast<WeaponKind>(slot.kind),
                dry
                    ? (hover ? D3DCOLOR_XRGB(255, 160, 140) : D3DCOLOR_XRGB(255, 72, 56))
                    : (hover ? D3DCOLOR_XRGB(255, 255, 200) : D3DCOLOR_XRGB(255, 176, 0)));
        }
    }

    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

bool VR::UpdateWeaponFireHaptics()
{
    if (!m_Game || !m_GameplayEligible || PauseUiActive())
        return false;
    C_BaseEntity* vm = m_Game->GetViewModelEntity();
    C_BaseEntity* weapon = m_Game->GetActiveWeaponEntity();
    if (weapon != m_LastFireWeapon)
    {
        m_LastFireWeapon = weapon;
        m_LastMuzzleFlashParity = -1;
        m_LastFireClip = -1;
    }

    bool fired = false;
    if (vm)
    {
        const int parity = ReadMuzzleFlashParity(vm);
        if (parity >= 0)
        {
            if (m_LastMuzzleFlashParity >= 0 && parity != m_LastMuzzleFlashParity)
                fired = true;
            m_LastMuzzleFlashParity = parity;
        }
    }

    if (weapon)
    {
        const int clip = m_Game->ReadWeaponClip(weapon);
        if (clip >= 0)
        {
            if (m_LastFireClip >= 0 && clip < m_LastFireClip)
                fired = true;
            m_LastFireClip = clip;
        }
    }

    if (!fired)
        return false;

    bool melee = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        melee = m_LastViewmodelModel.find("crowbar") != std::string::npos
            || m_LastViewmodelModel.find("wrench") != std::string::npos;
    }
    if (melee)
        return false;
    PulseHandHaptic(vr::TrackedControllerRole_RightHand, 2500, 0.85f);
    return true;
}

void VR::AfterCreateMoveFireHaptics()
{
    const uint32_t held = HeldButtons();
    const bool attackEdge = (held & IN_ATTACK) && !(m_PrevHeldButtons & IN_ATTACK);
    m_PrevHeldButtons = held;
    // Do not consume clip/parity here — ProcessInput on the Present thread
    // is the path that used to rumble each shot. Only latch energy-weapon
    // trigger edges that never decrement clip.
    if (!attackEdge || EmptyHands())
        return;
    bool melee = false;
    {
        std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
        melee = m_LastViewmodelModel.find("crowbar") != std::string::npos
            || m_LastViewmodelModel.find("wrench") != std::string::npos;
    }
    if (!melee)
        m_PendingFireHaptic.store(1, std::memory_order_release);
}

void VR::PulseHandHaptic(vr::ETrackedControllerRole hand, unsigned short durationUs, float amplitude)
{
    if (!bmvr::g_Haptics)
        return;
    const uint32_t openXrHand = (hand == vr::TrackedControllerRole_LeftHand)
        ? L4D2VR_OPENXR_HAND_LEFT : L4D2VR_OPENXR_HAND_RIGHT;
    if (m_OpenXrHelperBridgeActive)
    {
        // OpenVR TriggerHapticPulse units are microseconds of a click (1-3999).
        // OpenXR duration is real vibration time; 2ms is below SteamVR's floor.
        float seconds = durationUs / 1000000.0f;
        if (durationUs <= 3999)
            seconds = std::clamp(durationUs / 3999.0f * 0.07f, 0.045f, 0.09f);
        L4D2VR_PublishOpenXrHapticRequest(openXrHand, seconds, 160.0f, amplitude);
        static int s_hapticLog;
        if (s_hapticLog < 8)
        {
            Game::logMsg("OpenXR haptic hand=%u dur=%.3fs amp=%.2f", openXrHand, seconds, amplitude);
            ++s_hapticLog;
        }
        return;
    }
    if (!m_System)
        return;
    if (durationUs < 1)
        durationUs = 1;
    if (durationUs > 3999)
        durationUs = 3999;
    const vr::TrackedDeviceIndex_t idx = m_System->GetTrackedDeviceIndexForControllerRole(hand);
    if (idx == vr::k_unTrackedDeviceIndexInvalid)
        return;
    m_System->TriggerHapticPulse(idx, 0, durationUs);
}

const char* VR::WeaponMenuDrawSoundName(int kind) const
{
    return DrawSoundForKind(static_cast<WeaponKind>(kind));
}
