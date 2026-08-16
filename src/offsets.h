#pragma once
#include "sigscanner.h"
#include "game.h"

// Signatures verified offline against the installed Steam Black Mesa modules
// (tools/scan_offsets.py). All MATCH on this machine except OverrideView
// (pattern too short; not hooked). Prototype build stamp 19042901 is still
// valid for steam.inf PatchVersion=100002.

struct Offset
{
    std::string moduleName;
    int offset;
    int address;
    std::string signature;
    int sigOffset;
    bool optional;
    bool valid;

    Offset(std::string moduleName, int currentOffset, std::string signature, int sigOffset = 0, bool optional = false)
        : moduleName(std::move(moduleName))
        , offset(currentOffset)
        , address(0)
        , signature(std::move(signature))
        , sigOffset(sigOffset)
        , optional(optional)
        , valid(false)
    {
        if (!GetModuleHandleA(this->moduleName.c_str()))
            return;

        int newOffset = SigScanner::VerifyOffset(this->moduleName, currentOffset, this->signature, this->sigOffset);
        if (newOffset > 0)
            this->offset = newOffset;
        if (newOffset == -1)
        {
            if (!optional)
                Game::logMsg("Signature not found in %s: %s", this->moduleName.c_str(), this->signature.c_str());
            return;
        }
        this->address = (int)((uintptr_t)GetModuleHandleA(this->moduleName.c_str()) + this->offset);
        this->valid = this->address != 0;
    }
};

class Offsets
{
public:
    // CBlackMesaViewRender IViewRender vtable slot 6. 3-arg thiscall
    // (CViewSetup&, nClearFlags, whatToDraw), ret 0xC.
    // 0x207730 matches a nearby helper whose callers push floats — not RenderView.
    Offset RenderView{ "client.dll", 0x20EE40,
        "55 8B EC 81 EC DC 02 00 00 A1 ? ? ? ? 33 C5 89 45 FC 53 8B 5D 08 56" };

    Offset g_pClientMode{ "client.dll", 0x16AD56,
        "56 57 8B F9 8B 0D ? ? ? ? 8B 01 FF 50 24", 6 };

    Offset CreateMove{ "client.dll", 0x110310,
        "55 8B EC E8 ? ? ? ? 8B C8 85 C9 75 06 B0 01 5D C2 08 00" };

    Offset CalcViewModelView{ "client.dll", 0x29D930,
        "55 8B EC 83 EC 24 53 56 8B 75 08 57 8B F9 85 F6" };

    Offset AdjustEngineViewport{ "client.dll", 0x1102C0,
        "C2 10 00 CC CC CC CC CC CC CC CC CC CC CC CC CC B0 01 C2 08 00" };

    Offset LevelInit{ "client.dll", 0x110A80,
        "55 8B EC 83 EC 20 56 8B F1 6A 01 68 ? ? ? ?" };

    Offset LevelShutdown{ "client.dll", 0x110B30,
        "55 8B EC 83 EC 20 56 8B F1 B9 ? ? ? ? E8" };

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

    // IMaterialSystem vtable is shifted vs L4D2 (GetBackBufferFormat returned a
    // pointer). Call these by RVA. BeginRT no-ops after startup unless
    // isGameRunning at kCMaterialSystem_isGameRunning is cleared (BM, not L4D2's +0x2AB8).
    static constexpr int kCMaterialSystem_isGameRunning = 0x2AA4;
    Offset BeginRTAlloc{ "materialsystem.dll", 0x48450,
        "56 8B F1 80 BE A4 2A 00 00 00 74 10" };
    Offset EndRTAlloc{ "materialsystem.dll", 0x4AD80,
        "80 B9 A4 2A 00 00 00 75 62" };
    // Real CreateNamedRenderTargetTextureEx (BeginRT+3). Distinct from Ex2 at
    // 0x49600 which ends the shared prologue with `mov eax,[ecx]`.
    Offset CreateNamedRTEx{ "materialsystem.dll", 0x49660,
        "55 8B EC 83 B9 A0 2A 00 00 00 75 14 68 ? ? ? ? FF 15 ? ? ? ? 83 C4 04 33 C0 5D C2 20 00 8B 0D" };

    Offset ProcessUsercmds{ "server.dll", 0x5320F0,
        "55 8B EC B8 ? ? ? ? E8 ? ? ? ? A1 ? ? ? ? 33 C5 89 45 FC 8B 45 0C 8B 55 08", 0, true };

    // L4D2 leftover referenced by copied sdk.h melee helpers. Unused on Black Mesa.
    struct { int address = 0; } GetMeleeWeaponInfoClient;
};
