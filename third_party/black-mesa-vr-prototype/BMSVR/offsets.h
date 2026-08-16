#pragma once
#include "sigscanner.h"
#include "game.h"

// Black Mesa build 19042901 — RVAs verified offline against on-disk DLLs
// (tools/verify_offsets.py). SigScanner offsets are RVAs into the loaded module.
//
// CalcViewModelView: gameplay uses C_BlackMesaViewModel override 0x29D930 (retn 0x0C).
//   Base Shared impl 0x7CF60 (same sig) is NOT called by BM slot [230].
// g_pClientMode: signature match starts at 0x16AD50; stored offset is +6 (imm32).
// ClientMode vtables / LevelInit gate candidates: see constexpr section + docs/OFFSETS.md.

struct Offset
{
    std::string moduleName;
    int offset;
    int address;
    std::string signature;
    int sigOffset;

    Offset(std::string moduleName, int currentOffset, std::string signature, int sigOffset = 0)
        : moduleName(std::move(moduleName)), offset(currentOffset), signature(std::move(signature)), sigOffset(sigOffset)
    {
        if (!GetModuleHandleA(this->moduleName.c_str()))
        {
            address = 0;
            return;
        }

        int newOffset = SigScanner::VerifyOffset(this->moduleName, currentOffset, this->signature, this->sigOffset);
        if (newOffset > 0)
            this->offset = newOffset;
        if (newOffset == -1)
        {
            Game::logMsg("Signature not found in %s: %s", this->moduleName.c_str(), this->signature.c_str());
            address = 0;
            return;
        }
        address = (int)((uintptr_t)GetModuleHandleA(this->moduleName.c_str()) + this->offset);
    }

    bool valid() const { return address != 0; }
};

class Offsets
{
public:
    // CViewRender::RenderView — 3-arg (setup, clearFlags, whatToDraw), NOT Portal/L4D 4-arg
    Offset RenderView{ "client.dll", 0x207730,
        "55 8B EC 83 EC 08 A1 ? ? ? ? 53 8B D9 89 45 F8" };

    // Imm32 at +6 → g_pClientMode storage (pattern base RVA 0x16AD50)
    Offset g_pClientMode{ "client.dll", 0x16AD56,
        "56 57 8B F9 8B 0D ? ? ? ? 8B 01 FF 50 24", 6 };

    // ClientModeShared::CreateMove
    Offset CreateMove{ "client.dll", 0x110310,
        "55 8B EC E8 ? ? ? ? 8B C8 85 C9 75 06 B0 01 5D C2 08 00" };

    // C_BlackMesaViewModel::CalcViewModelView — retn 0x0C (owner*, const Vector& eyePos, const QAngle& eyeAng)
    // eyePos/eyeAng are INPUTS; BM SetLocalOrigin/Angles from them. Pose nudge = local copies before original.
    // Gameplay hits BM override; Shared/base 0x7CF60 is unused by BM vt slot [230].
    Offset CalcViewModelView{ "client.dll", 0x29D930,
        "55 8B EC 83 EC 24 53 56 8B 75 08 57 8B F9 85 F6" };

    // Empty stub on ClientMode vtable — safe to hook for VR viewport forcing
    Offset AdjustEngineViewport{ "client.dll", 0x1102C0,
        "C2 10 00 CC CC CC CC CC CC CC CC CC CC CC CC CC B0 01 C2 08 00" };

    Offset DrawModelExecute{ "engine.dll", 0xF6A20,
        "55 8B EC 81 EC ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 10 56 8B 75 08 57 8B" };

    Offset VGui_Paint{ "engine.dll", 0x238C50,
        "55 8B EC 83 EC 18 53 8B D9 8B 0D ? ? ? ? FF 15" };

    Offset GetRenderTarget{ "materialsystem.dll", 0x68820,
        "83 79 4C 00 7E 0E 8B 41 4C 8D 14 C0" };
    Offset GetViewport{ "materialsystem.dll", 0x68A70,
        "55 8B EC 8B 41 4C 56 8D 14 C0 8B 41 40 83 7C 90 F8 00" };
    Offset Viewport{ "materialsystem.dll", 0x69F30,
        "55 8B EC 56 FF 75 14 8B F1 FF 75 10 FF 75 0C FF 75 08 E8" };
    Offset PushRenderTargetAndViewport{ "materialsystem.dll", 0x6A3D0,
        "55 8B EC 83 EC 24 8B 45 08 89 45 DC 8B 45 0C 89 45 EC" };
    Offset PopRenderTargetAndViewport{ "materialsystem.dll", 0x6A250,
        "56 8B F1 83 7E 4C 00 74 15 8B 06 6A 00 FF 50 10 FF 4E 4C" };

    Offset EyePosition{ "server.dll", 0x317E60,
        "55 8B EC 56 8B F1 8B 86 04 01 00 00 C1 E8 0B A8 01 74 05 E8" };

    // Verified unique on BM server.dll (was stale 0xEF710)
    Offset ProcessUsercmds{ "server.dll", 0x5320F0,
        "55 8B EC B8 ? ? ? ? E8 ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 0C 8B 55 08" };

    // -------------------------------------------------------------------------
    // Offline RE candidates (NOT hooked). RVAs only — see docs/OFFSETS.md.
    // Confidence: HIGH = COL/vtable + unique prologue; MED = thunk/wrapper.
    // -------------------------------------------------------------------------

    // ClientModeShared vtable RVA 0x4415A8 (COL-backed .?AVClientModeShared@@)
    // ClientModeBlackMesaNormal vtable RVA 0x469D94 (COL-backed)
    // IClientMode slot layout matches Source: CreateMove=21, LevelInit=22,
    // LevelShutdown=23, AdjustEngineViewport=26, GetViewModelFOV=32.

    // HIGH — map / VR gate: fires with const char* newmap; BM calls Shared then local setup.
    // Prefer Shared — BM Normal E8s here. Reject background* in VR::OnLevelInit.
    Offset LevelInit{ "client.dll", 0x110A80,
        "55 8B EC 83 EC 20 56 8B F1 6A 01 68 ? ? ? ?" };
    Offset LevelShutdown{ "client.dll", 0x110B30,
        "55 8B EC 83 EC 20 56 8B F1 B9 ? ? ? ? E8" };

    static constexpr int ClientModeShared_LevelInit = 0x110A80;
    static constexpr int ClientModeShared_LevelShutdown = 0x110B30;
    static constexpr int ClientModeBM_LevelInit = 0x216B90;       // E8 -> Shared LevelInit
    static constexpr int ClientModeBM_LevelShutdown = 0x216C60;   // E8 -> Shared LevelShutdown

    // HIGH — real OverrideView(CViewSetup*); retn 0x4. BM slot is jmp-thunk to this.
    static constexpr int ClientModeShared_OverrideView = 0x110BE0;
    static constexpr int ClientModeBM_OverrideView = 0x216EB0;    // MED: 55 8B EC 5D E9 -> Shared

    // HIGH — BM gameplay CreateMove wrapper (E8 -> Shared 0x110310). Current MinHook on
    // Shared still works because BM always calls through.
    static constexpr int ClientModeBM_CreateMove = 0x216130;

    // HIGH — GetViewModelFOV (reads viewmodel_fov cvar path)
    static constexpr int ClientModeShared_GetViewModelFOV = 0x110490;
    static constexpr int ClientModeBM_GetViewModelFOV = 0x216510;

    // HIGH — BM GetMapName returns this+0x224 (wchar buffer on ClientMode object)
    static constexpr int ClientModeBM_GetMapName = 0x2164B0; // lea eax,[ecx+0x224]; ret

    // CalcViewModelView: MinHook BM 0x29D930 (above). Base Shared 0x7CF60 / vt 0x41CBAC slot [230]
    // is overridden by C_BlackMesaViewModel vt 0x4A4A98 slot [230] — hook BM for live calls.
    static constexpr int CalcViewModelView_Shared = 0x7CF60;
    static constexpr int CalcViewModelView_BM = 0x29D930;
};
