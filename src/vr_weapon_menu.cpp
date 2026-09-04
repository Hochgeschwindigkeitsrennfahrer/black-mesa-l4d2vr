#include "vr.h"
#include "game.h"
#include "in_buttons.h"
#include "bmvr_flags.h"
#include "vr_hud_icons.h"
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
    // Sit on the grip, not 11 HU down the barrel (that looked like a face/aim
    // spawn when the controller was pitched).
    constexpr float kMenuHandForwardHu = 4.f;
    constexpr float kMenuHandUpHu = 0.f;

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
        // Engine pawn origin, not the HMD-adjusted stereo copy. Parenting the
        // wheel to m_StereoBodyOrigin made it translate with every head move.
        Vector body = vr->m_SetupOrigin;
        if (body.LengthSqr() <= 1.f && vr->m_HasStereoBodyOrigin)
            body = vr->m_StereoBodyOrigin;
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
    // World-HU hex size. Packing > 1 leaves a gutter. Draw and hover both use
    // this plane (not screen-pixel honeycomb around a projected origin).
    constexpr float kHexPackScale = 1.20f;
    constexpr float kHexWorldHu = 2.55f;
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

    // Plane offset of each layout cell, in hex radii, with +y along menu up:
    //
    //   inner ring   {0,-1} lower-left   {1,-1} lower-right  {1,0} right
    //                {0,1}  upper-right  {-1,1} upper-left   {-1,0} left
    //   outer ring   {0,-2} lower-far-left   {2,-1} lower-far-right
    //                {1,1}  upper-far-right  {-1,2} top
    //                {-2,1} upper-far-left
    //
    // Crowbar and pistol take the two bottom inner cells: those are the ones
    // the hand falls onto when the wheel opens centred, and they are the two
    // weapons reached most often. MP5 and shotgun sit adjacent on the right so
    // swapping between the primaries is one short move. Heavies go outer ring.
    void HexSlotForKind(WeaponKind kind, int& q, int& r)
    {
        switch (kind)
        {
        case KindCrowbar: q = 0; r = -1; break;
        case KindGlock: q = 1; r = -1; break;
        case KindMp5: q = 1; r = 0; break;
        case KindShotgun: q = 0; r = 1; break;
        case KindRevolver: q = -1; r = 1; break;
        case KindCrossbow: q = -1; r = 0; break;
        case KindGauss: q = 1; r = 1; break;
        case KindRpg: q = -1; r = 2; break;
        case KindGluon: q = -2; r = 1; break;
        case KindGrenade: q = 2; r = -1; break;
        case KindHivehand: q = 0; r = -2; break;
        case KindSatchel: q = 2; r = 0; break;
        case KindTripmine: q = -2; r = 0; break;
        case KindSnark: q = 0; r = 2; break;
        default: q = 99; r = 99; break;
        }
    }

    void HexToOffset(int q, int r, float radius, float& x, float& y)
    {
        x = radius * (kSqrt3 * (static_cast<float>(q) + static_cast<float>(r) * 0.5f));
        y = radius * (1.5f * static_cast<float>(r));
    }

    Vector HexPlanePoint(const Vector& center, const Vector& planeRight, const Vector& planeUp,
        float radiusHu, float angleDeg)
    {
        const float a = angleDeg * (3.14159265f / 180.f);
        // Match the old screen hex (angle -90 = visually up): screen Y is down,
        // so -sin in the plane-up axis.
        return center + planeRight * (cosf(a) * radiusHu) + planeUp * (-sinf(a) * radiusHu);
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

    void DrawHexFillPts(IDirect3DDevice9* device, float cx, float cy,
        const float* xs, const float* ys, D3DCOLOR color)
    {
        MenuVert v[8]{};
        v[0] = { cx, cy, 0.f, 1.f, color };
        for (int i = 0; i < 7; ++i)
            v[i + 1] = { xs[i], ys[i], 0.f, 1.f, color };
        device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 6, v, sizeof(MenuVert));
    }

    void DrawHexRingPts(IDirect3DDevice9* device, const float* xo, const float* yo,
        const float* xi, const float* yi, D3DCOLOR color)
    {
        MenuVert v[14]{};
        for (int i = 0; i <= 6; ++i)
        {
            v[i * 2] = { xo[i], yo[i], 0.f, 1.f, color };
            v[i * 2 + 1] = { xi[i], yi[i], 0.f, 1.f, color };
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
    bool g_WeaponIconBlueShift = false;

    // HEV HUD silhouettes are orange. Blue Shift's Calhoun HUD is blue
    // (same RGB as the pause-menu cursor). R↔B swap maps (255,176,0) to
    // (0,176,255), next to Calhoun (64,168,255). Skip if the atlas is
    // already blue-dominant so a native bshift VTF is left alone.
    void RemapHevAmberToCalhoun(std::vector<unsigned char>& bgra)
    {
        unsigned long long sumR = 0, sumB = 0, n = 0;
        for (size_t i = 0; i + 3 < bgra.size(); i += 4)
        {
            if (bgra[i + 3] < 16)
                continue;
            sumB += bgra[i];
            sumR += bgra[i + 2];
            ++n;
        }
        if (n == 0 || sumB >= sumR)
            return;
        for (size_t i = 0; i + 3 < bgra.size(); i += 4)
            std::swap(bgra[i], bgra[i + 2]);
    }

    struct WheelPalette
    {
        D3DCOLOR fillHover;
        D3DCOLOR fillIdle;
        D3DCOLOR fillDryHover;
        D3DCOLOR fillDryIdle;
        D3DCOLOR frameHover;
        D3DCOLOR frameEquipped;
        D3DCOLOR frameIdle;
        D3DCOLOR frameDryHover;
        D3DCOLOR frameDryIdle;
        D3DCOLOR glow;
        D3DCOLOR glowDry;
        D3DCOLOR tintHover;
        D3DCOLOR tintIdle;
        D3DCOLOR tintDryHover;
        D3DCOLOR tintDryIdle;
        D3DCOLOR glyphHover;
        D3DCOLOR glyphIdle;
        D3DCOLOR glyphDryHover;
        D3DCOLOR glyphDryIdle;
    };

    WheelPalette MakeWheelPalette()
    {
        WheelPalette p{};
        // Empty-clip warning stays red in both campaigns.
        p.fillDryHover = D3DCOLOR_ARGB(220, 72, 16, 12);
        p.fillDryIdle = D3DCOLOR_ARGB(190, 42, 10, 8);
        p.frameDryHover = D3DCOLOR_XRGB(255, 120, 90);
        p.frameDryIdle = D3DCOLOR_XRGB(255, 56, 40);
        p.glowDry = D3DCOLOR_ARGB(170, 255, 90, 70);
        p.tintDryHover = D3DCOLOR_XRGB(255, 170, 150);
        p.tintDryIdle = D3DCOLOR_XRGB(255, 72, 56);
        p.glyphDryHover = D3DCOLOR_XRGB(255, 160, 140);
        p.glyphDryIdle = D3DCOLOR_XRGB(255, 72, 56);
        if (bmvr::IsBlueShift())
        {
            p.fillHover = D3DCOLOR_ARGB(210, 10, 28, 52);
            p.fillIdle = D3DCOLOR_ARGB(170, 6, 14, 28);
            p.frameHover = D3DCOLOR_XRGB(160, 220, 255);
            p.frameEquipped = D3DCOLOR_XRGB(64, 168, 255);
            p.frameIdle = D3DCOLOR_ARGB(220, 40, 110, 200);
            p.glow = D3DCOLOR_ARGB(160, 180, 230, 255);
            p.tintHover = D3DCOLOR_XRGB(255, 255, 255);
            p.tintIdle = D3DCOLOR_XRGB(180, 220, 255);
            // Icons are already Calhoun-blue; a red multiply would go purple.
            p.tintDryHover = D3DCOLOR_XRGB(210, 180, 190);
            p.tintDryIdle = D3DCOLOR_XRGB(140, 110, 130);
            p.glyphHover = D3DCOLOR_XRGB(200, 230, 255);
            p.glyphIdle = D3DCOLOR_XRGB(64, 168, 255);
        }
        else
        {
            p.fillHover = D3DCOLOR_ARGB(210, 48, 36, 10);
            p.fillIdle = D3DCOLOR_ARGB(170, 14, 12, 8);
            p.frameHover = D3DCOLOR_XRGB(255, 240, 140);
            p.frameEquipped = D3DCOLOR_XRGB(255, 176, 0);
            p.frameIdle = D3DCOLOR_ARGB(220, 200, 140, 30);
            p.glow = D3DCOLOR_ARGB(160, 255, 255, 200);
            p.tintHover = D3DCOLOR_XRGB(255, 255, 255);
            p.tintIdle = D3DCOLOR_XRGB(255, 230, 180);
            p.glyphHover = D3DCOLOR_XRGB(255, 255, 200);
            p.glyphIdle = D3DCOLOR_XRGB(255, 176, 0);
        }
        return p;
    }

    void ReleaseHudIconCache();

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
        g_WeaponIconBlueShift = false;
        ReleaseHudIconCache();
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

    bool ExtractHudVtfByName(const std::vector<std::wstring>& vpks, const char* name,
        std::vector<unsigned char>& vtf)
    {
        if (!name || !*name)
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

    bool ExtractHudVtf(const std::vector<std::wstring>& vpks, WeaponKind kind,
        std::vector<unsigned char>& vtf)
    {
        return ExtractHudVtfByName(vpks, HudVtfNameForKind(kind), vtf);
    }

    void EnsureWeaponIcons(IDirect3DDevice9* device)
    {
        if (!device)
            return;
        const bool wantBlue = bmvr::IsBlueShift();
        if (g_WeaponIconDevice && g_WeaponIconDevice != device)
            ReleaseWeaponIcons();
        if (g_WeaponIconTried && g_WeaponIconBlueShift != wantBlue)
            ReleaseWeaponIcons();
        if (g_WeaponIconTried)
            return;
        g_WeaponIconTried = true;
        g_WeaponIconDevice = device;
        g_WeaponIconBlueShift = wantBlue;
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
            if (wantBlue)
                RemapHevAmberToCalhoun(bgra);
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

    // Name-keyed cache for HUD icons the wrist HUD asks for by filename
    // (health cross, HEV figure, ammo types). Separate from the weapon-kind
    // array above because those are indexed by WeaponKind.
    struct HudIconEntry
    {
        char name[64]{};
        IDirect3DTexture9* tex = nullptr;
    };
    constexpr int kHudIconCacheMax = 24;
    HudIconEntry g_HudIconCache[kHudIconCacheMax]{};
    int g_HudIconCount = 0;
    IDirect3DDevice9* g_HudIconDevice = nullptr;
    bool g_HudIconBlueShift = false;

    void ReleaseHudIconCache()
    {
        for (int i = 0; i < g_HudIconCount; ++i)
        {
            if (g_HudIconCache[i].tex)
                g_HudIconCache[i].tex->Release();
            g_HudIconCache[i] = HudIconEntry{};
        }
        g_HudIconCount = 0;
        g_HudIconDevice = nullptr;
        g_HudIconBlueShift = false;
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

IDirect3DTexture9* bmvr::AcquireHudIcon(IDirect3DDevice9* device, const char* vtfName)
{
    if (!device || !vtfName || !*vtfName)
        return nullptr;
    const bool wantBlue = IsBlueShift();
    if (g_HudIconDevice && g_HudIconDevice != device)
        ReleaseHudIconCache();
    if (g_HudIconCount > 0 && g_HudIconBlueShift != wantBlue)
        ReleaseHudIconCache();
    g_HudIconDevice = device;
    g_HudIconBlueShift = wantBlue;
    for (int i = 0; i < g_HudIconCount; ++i)
    {
        if (_stricmp(g_HudIconCache[i].name, vtfName) == 0)
            return g_HudIconCache[i].tex;
    }
    if (g_HudIconCount >= kHudIconCacheMax)
        return nullptr;

    // Claim the slot before loading so a missing asset is only attempted once.
    HudIconEntry& entry = g_HudIconCache[g_HudIconCount++];
    strncpy_s(entry.name, vtfName, _TRUNCATE);

    const auto vpks = HudVpkPaths();
    std::vector<unsigned char> vtf;
    if (vpks.empty() || !ExtractHudVtfByName(vpks, vtfName, vtf))
    {
        Game::logMsg("Wrist HUD icon missing %s", vtfName);
        return nullptr;
    }
    int iw = 0, ih = 0;
    unsigned format = 0;
    std::vector<unsigned char> bgra;
    if (!DecodeVtfToBgra(vtf, iw, ih, bgra, &format))
    {
        Game::logMsg("Wrist HUD icon decode fail %s fmt=%u", vtfName, format);
        return nullptr;
    }
    CropBgraToAlpha(iw, ih, bgra);
    if (wantBlue)
        RemapHevAmberToCalhoun(bgra);
    IDirect3DTexture9* tex = nullptr;
    if (!UploadBgraTexture(device, iw, ih, bgra, &tex) || !tex)
    {
        Game::logMsg("Wrist HUD icon upload fail %s %dx%d", vtfName, iw, ih);
        return nullptr;
    }
    entry.tex = tex;
    Game::logMsg("Wrist HUD icon loaded %s %dx%d", vtfName, iw, ih);
    return tex;
}

const char* bmvr::PrimaryAmmoIconVtf(const char* model, const char* net)
{
    switch (KindFromNames(model, net))
    {
    case KindRevolver: return "ammo_357.vtf";
    case KindShotgun: return "ammo_buckshot.vtf";
    case KindCrossbow: return "ammo_bolt.vtf";
    case KindGauss:
    case KindGluon: return "ammo_energy.vtf";
    case KindRpg: return "ammo_grenade_rpg.vtf";
    // Black Mesa ships no hornet .vtf, only the .vmt. The lookup fails once,
    // logs, and the counter then draws without an icon.
    case KindHivehand: return "ammo_grenade_hornet.vtf";
    case KindGrenade: return "ammo_grenade_frag.vtf";
    case KindSatchel: return "ammo_grenade_satchel.vtf";
    case KindTripmine: return "ammo_grenade_tripmine.vtf";
    case KindSnark: return "ammo_snark.vtf";
    default: return "ammo_9mm.vtf";
    }
}

const char* bmvr::SecondaryAmmoIconVtf(const char* model, const char* net)
{
    switch (KindFromNames(model, net))
    {
    case KindMp5: return "ammo_grenade_mp5.vtf";
    case KindShotgun: return "ammo_buckshot.vtf";
    default: return "ammo_grenade_frag.vtf";
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
        // Do not snap to the empty-hand cell. Hover is resolved after slots
        // are built from the currently equipped weapon.
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
                Vector aim{};
                Vector ctrlUp{};
                {
                    std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
                    // Same tracking body as the visible gloves so the origin
                    // matches the grip, not a leftover 16:9 setup.origin.
                    Vector trackingBody = m_HasStereoBodyOrigin ? m_StereoBodyOrigin : body;
                    if (trackingBody.LengthSqr() <= 1.f)
                        trackingBody = body;
                    hand = ControllerTrackingToWorld(trackingBody, m_PhysicalRightPosAbs);
                    QAngle::AngleVectors(m_PhysicalRightAngAbs, &aim, nullptr, &ctrlUp);
                }
                if (VectorNormalize(aim) <= 0.01f)
                    aim = fwd;
                if (VectorNormalize(ctrlUp) <= 0.01f)
                    ctrlUp = up;
                // Small offset in front of / above the grip so the hover ray
                // has a positive t and the hexes are not inside the glove.
                m_WeaponMenuLatchBody = body;
                m_WeaponMenuLatchWorld = hand + aim * kMenuHandForwardHu + ctrlUp * kMenuHandUpHu;
                m_WeaponMenuLatchDelta = m_WeaponMenuLatchWorld - body;
                m_WeaponMenuLatchFwd = fwd;
                m_WeaponMenuLatchRight = right;
                m_WeaponMenuLatchUp = up;
                m_WeaponMenuLatchYaw = m_RotationOffsetY.load(std::memory_order_acquire);
                m_WeaponMenuLatched = true;

                // Billboard basis is latched with the rest of the pose. Solving
                // it per frame against the live eye spun the whole wheel under
                // the cursor as the head moved, which fought the hand.
                BillboardFacingEye(m_WeaponMenuLatchWorld, GetViewOrigin(body), up, right,
                    m_WeaponMenuFwd, m_WeaponMenuRight, m_WeaponMenuUp);
                m_WeaponMenuLatchBillboardFwd = m_WeaponMenuFwd;
                m_WeaponMenuLatchBillboardRight = m_WeaponMenuRight;
                m_WeaponMenuLatchBillboardUp = m_WeaponMenuUp;
            }

            // Wheel origin follows the latched body-relative delta, not the
            // live hand pose each frame. That avoids jitter from player
            // movement (body position changes, tracking is latched).
            const float yawDelta = m_RotationOffsetY.load(std::memory_order_acquire)
                - m_WeaponMenuLatchYaw;
            Vector delta = m_WeaponMenuLatchDelta;
            YawAroundZ(delta, yawDelta);
            m_WeaponMenuOrigin = body + delta;
            m_WeaponMenuFwd = m_WeaponMenuLatchBillboardFwd;
            m_WeaponMenuRight = m_WeaponMenuLatchBillboardRight;
            m_WeaponMenuUp = m_WeaponMenuLatchBillboardUp;
            YawAroundZ(m_WeaponMenuFwd, yawDelta);
            YawAroundZ(m_WeaponMenuRight, yawDelta);
            YawAroundZ(m_WeaponMenuUp, yawDelta);
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
            HexToOffset(q, r, kHexWorldHu * kHexPackScale, ox, oy);
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

        static const int kOverflow[][2] = {
            {2, 0}, {-2, 0}, {0, 2}, {1, -2}, {-1, -1},
            {2, -2}, {-2, 2}, {2, 1}, {-2, -1}
        };
        auto placeOverflow = [&](int& q, int& r) -> bool {
            for (const auto& c : kLayoutCells)
            {
                if (occupy(c[0], c[1]))
                {
                    q = c[0];
                    r = c[1];
                    return true;
                }
            }
            for (const auto& c : kOverflow)
            {
                if (occupy(c[0], c[1]))
                {
                    q = c[0];
                    r = c[1];
                    return true;
                }
            }
            return false;
        };

        // Reserved cells first so late-game extras cannot steal crowbar/pistol.
        bool seated[kMaxMenuSlots]{};
        for (int i = 0; i < n; ++i)
        {
            const WeaponKind kind = KindFromNames(inv[i].modelName, inv[i].networkName);
            int q = 0, r = 0;
            HexSlotForKind(kind, q, r);
            if (q >= 90)
                continue;
            if (!occupy(q, r))
                continue;
            placeAt(q, r, inv[i], kind);
            seated[i] = true;
        }
        for (int i = 0; i < n; ++i)
        {
            if (seated[i])
                continue;
            const WeaponKind kind = KindFromNames(inv[i].modelName, inv[i].networkName);
            int q = 0, r = 0;
            if (!placeOverflow(q, r))
                continue;
            placeAt(q, r, inv[i], kind);
        }
        m_WeaponMenuCount = placed;

        Vector rayOrig{};
        Vector rayDir{};
        {
            std::lock_guard<std::recursive_mutex> lock(m_ControllerMutex);
            Vector body = MenuPlayerBody(this);
            rayOrig = ControllerTrackingToWorld(body, m_PhysicalRightPosAbs);
            QAngle::AngleVectors(m_PhysicalRightAngAbs, &rayDir, nullptr, nullptr);
        }
        const int prevHover = m_WeaponMenuHover;
        const Vector planeN = m_WeaponMenuFwd;
        const float planeDenom = rayDir.Dot(planeN);
        int hover = prevHover;
        if (hover < 0 || hover >= m_WeaponMenuCount)
        {
            hover = 0;
            if (!m_EmptyHands)
            {
                for (int i = 1; i < m_WeaponMenuCount; ++i)
                {
                    if (m_WeaponMenuSlots[i].equipped)
                    {
                        hover = i;
                        break;
                    }
                }
            }
        }
        const float packR = kHexWorldHu * kHexPackScale;
        if (fabsf(planeDenom) > 0.12f)
        {
            const float hitT = (m_WeaponMenuOrigin - rayOrig).Dot(planeN) / planeDenom;
            if (hitT > 0.02f && hitT < 80.f)
            {
                const Vector planeHit = rayOrig + rayDir * hitT;
                const Vector rel = planeHit - m_WeaponMenuOrigin;
                const float hx = rel.Dot(m_WeaponMenuRight);
                const float hy = rel.Dot(m_WeaponMenuUp);
                float bestD = 1.0e9f;
                int best = -1;
                for (int i = 0; i < m_WeaponMenuCount; ++i)
                {
                    float ox = 0.f, oy = 0.f;
                    HexToOffset(m_WeaponMenuSlots[i].axialQ, m_WeaponMenuSlots[i].axialR,
                        packR, ox, oy);
                    const float lx = hx - ox;
                    const float ly = hy - oy;
                    if (!PointInHex(lx, ly, packR * 1.12f))
                        continue;
                    const float dist = lx * lx + ly * ly;
                    if (dist < bestD)
                    {
                        bestD = dist;
                        best = i;
                    }
                }
                if (best >= 0)
                    hover = best;
            }
        }
        m_WeaponMenuHover = hover;
        if (prevHover >= 0 && m_WeaponMenuHover != prevHover)
            QueueWeaponMenuSound(kWeaponSoundHover);
    }

    if (!stickClickHeld && m_WeaponMenuClickHeld)
    {
        if (m_WeaponMenuOpenedThisHold)
        {
            // Keep the last hovered cell. Missing the honeycomb no longer
            // snaps to the centre empty-hand slot.
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

    // World-locked honeycomb: each hex lives in the latched plane. Projecting
    // vertices (not a screen-pixel ring around one origin) keeps the wheel
    // still when the head moves. Snap-turn still yaws the latched basis.
    auto projectRing = [&](const Vector& center, float rHu, float* xs, float* ys) -> bool {
        for (int k = 0; k <= 6; ++k)
        {
            const float ang = static_cast<float>(k) * 60.f - 90.f;
            if (!project(HexPlanePoint(center, m_WeaponMenuRight, m_WeaponMenuUp, rHu, ang),
                xs[k], ys[k]))
                return false;
        }
        return true;
    };

    const float radiusHu = kHexWorldHu;
    const float edgeHu = radiusHu * 0.08f;
    const WheelPalette pal = MakeWheelPalette();

    for (int i = 0; i < m_WeaponMenuCount; ++i)
    {
        const WeaponMenuSlot& slot = m_WeaponMenuSlots[i];
        const bool hover = (i == m_WeaponMenuHover);
        float cx = 0.f, cy = 0.f;
        if (!project(slot.center, cx, cy))
            continue;
        float xo[7]{}, yo[7]{}, xi[7]{}, yi[7]{};
        if (!projectRing(slot.center, radiusHu, xo, yo)
            || !projectRing(slot.center, radiusHu - edgeHu, xi, yi))
            continue;
        const bool dry = slot.dry && !slot.emptyHand;
        const D3DCOLOR fill = dry
            ? (hover ? pal.fillDryHover : pal.fillDryIdle)
            : (hover ? pal.fillHover : pal.fillIdle);
        const D3DCOLOR frame = dry
            ? (hover ? pal.frameDryHover : pal.frameDryIdle)
            : (hover ? pal.frameHover
                : (slot.equipped ? pal.frameEquipped : pal.frameIdle));
        DrawHexFillPts(device, cx, cy, xi, yi, fill);
        DrawHexRingPts(device, xo, yo, xi, yi, frame);
        if (hover)
        {
            float xg[7]{}, yg[7]{}, xgi[7]{}, ygi[7]{};
            if (projectRing(slot.center, radiusHu * 1.04f, xg, yg)
                && projectRing(slot.center, radiusHu - edgeHu * 0.35f, xgi, ygi))
            {
                DrawHexRingPts(device, xg, yg, xgi, ygi, dry ? pal.glowDry : pal.glow);
            }
        }

        if (slot.emptyHand)
            continue;

        const int kind = slot.kind;
        IDirect3DTexture9* icon = (kind > 0 && kind < KindCount) ? g_WeaponIconTex[kind] : nullptr;
        if (icon)
        {
            const float iw = radiusHu * 1.05f;
            const float ih = radiusHu * 0.66f;
            const Vector tl = slot.center - m_WeaponMenuRight * (iw * 0.5f) + m_WeaponMenuUp * (ih * 0.5f);
            const Vector tr = slot.center + m_WeaponMenuRight * (iw * 0.5f) + m_WeaponMenuUp * (ih * 0.5f);
            const Vector bl = slot.center - m_WeaponMenuRight * (iw * 0.5f) - m_WeaponMenuUp * (ih * 0.5f);
            const Vector br = slot.center + m_WeaponMenuRight * (iw * 0.5f) - m_WeaponMenuUp * (ih * 0.5f);
            float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f, x2 = 0.f, y2 = 0.f, x3 = 0.f, y3 = 0.f;
            if (!project(tl, x0, y0) || !project(tr, x1, y1)
                || !project(bl, x2, y2) || !project(br, x3, y3))
                continue;
            const D3DCOLOR tint = dry
                ? (hover ? pal.tintDryHover : pal.tintDryIdle)
                : (hover ? pal.tintHover : pal.tintIdle);
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
                { x1, y1, 0.f, 1.f, tint, 1.f, 0.f },
                { x2, y2, 0.f, 1.f, tint, 0.f, 1.f },
                { x3, y3, 0.f, 1.f, tint, 1.f, 1.f }
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
            const float glyph = sqrtf((xo[0] - cx) * (xo[0] - cx) + (yo[0] - cy) * (yo[0] - cy)) * 0.28f;
            DrawKindIcon(device, cx, cy, glyph,
                static_cast<WeaponKind>(slot.kind),
                dry
                    ? (hover ? pal.glyphDryHover : pal.glyphDryIdle)
                    : (hover ? pal.glyphHover : pal.glyphIdle));
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
