#include "hooks.h"
#include "game.h"
#include "sdk.h"
#include "vr.h"
#include "offsets.h"
#include "bmvr_flags.h"
#include "sigscanner.h"
#include "in_buttons.h"
#include "trace.h"
#include "texture.h"
#include <Windows.h>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace
{
    void** SehMaterialSystemVTable(IMaterialSystem* mat)
    {
        void** vt = nullptr;
        if (!mat)
            return nullptr;
        __try
        {
            vt = *reinterpret_cast<void***>(mat);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            vt = nullptr;
        }
        return vt;
    }

    bool EngineInGame()
    {
        return Hooks::m_Game && Hooks::m_Game->m_EngineClient
            && Hooks::m_Game->m_EngineClient->IsInGame();
    }

    // Source viewrender.h: RENDERVIEW_DRAWVIEWMODEL=1, RENDERVIEW_DRAWHUD=2.
    constexpr int kRenderViewDrawHud = 0x2;

    bool TextureNameIsHudRt(const char* name)
    {
        if (!name || !name[0])
            return false;
        return std::strstr(name, "_rt_gui") != nullptr
            || std::strstr(name, "_rt_Hud") != nullptr
            || std::strstr(name, "_rt_HUD") != nullptr;
    }
}

Hooks::Hooks(Game* game)
{
    const MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED)
        Game::errorMsg("Failed to init MinHook");

    m_Game = game;
    m_VR = m_Game->m_VR;
    initSourceHooks();

    auto enableIfReady = [](auto& hk, const char* name) {
        if (hk.pTarget && hk.fOriginal)
        {
            if (hk.enableHook() == 0)
                Game::logMsg("Hook enabled: %s", name);
            else
                Game::logMsg("Hook enable failed: %s", name);
        }
        else
            Game::logMsg("Hook skipped: %s", name);
    };

    enableIfReady(hkGetRenderTarget, "GetRenderTarget");
    enableIfReady(hkPushRenderTargetAndViewport, "PushRT");
    enableIfReady(hkPopRenderTargetAndViewport, "PopRT");
    enableIfReady(hkViewport, "Viewport");
    enableIfReady(hkGetViewport, "GetViewport");
    enableIfReady(hkGetBackBufferDimensions, "GetBackBufferDimensions");
    enableIfReady(hkGetScreenSize, "GetScreenSize");
    enableIfReady(hkCreateNamedRTEx, "CreateNamedRTEx");
    enableIfReady(hkAdjustEngineViewport, "AdjustEngineViewport");
    enableIfReady(hkDrawModelExecute, "DrawModelExecute");
    enableIfReady(hkVgui_Paint, "VGui_Paint");
    enableIfReady(hkRenderView, "RenderView");
    enableIfReady(hkCreateMove, "CreateMove");
    enableIfReady(hkLevelInit, "LevelInit");
    enableIfReady(hkLevelShutdown, "LevelShutdown");
    enableIfReady(hkCalcViewModelView, "CalcViewModelView");
    enableIfReady(hkGetViewModelFOV, "GetViewModelFOV");
    enableIfReady(hkEndFrame, "EndFrame");
    enableIfReady(hkTraceRay, "TraceRay");
}

Hooks::~Hooks()
{
    MH_Uninitialize();
}

int Hooks::initSourceHooks()
{
    auto& o = *m_Game->m_Offsets;

    if (o.GetRenderTarget.valid)
        hkGetRenderTarget.createHook((LPVOID)o.GetRenderTarget.address, &dGetRenderTarget);
    if (o.RenderView.valid)
        hkRenderView.createHook((LPVOID)o.RenderView.address, &dRenderView);
    // Portal 2 VR: CreateMove writes viewangles. Camera stays HMD on the
    // RenderView copies. Only mutate cmds on gameplay maps — a previous
    // trampoline on background01 coincided with a 17s crash after the first
    // menu Submit.
    if (o.CreateMove.valid)
        hkCreateMove.createHook((LPVOID)o.CreateMove.address, &dCreateMove);
    if (o.CalcViewModelView.valid)
        hkCalcViewModelView.createHook((LPVOID)o.CalcViewModelView.address, &dCalcViewModelView);
    if (o.GetViewModelFOV.valid)
        hkGetViewModelFOV.createHook((LPVOID)o.GetViewModelFOV.address, &dGetViewModelFOV);
    if (o.AdjustEngineViewport.valid)
        hkAdjustEngineViewport.createHook((LPVOID)o.AdjustEngineViewport.address, &dAdjustEngineViewport);
    if (o.Viewport.valid)
        hkViewport.createHook((LPVOID)o.Viewport.address, &dViewport);
    if (o.GetViewport.valid)
        hkGetViewport.createHook((LPVOID)o.GetViewport.address, &dGetViewport);
    if (o.PushRenderTargetAndViewport.valid)
        hkPushRenderTargetAndViewport.createHook((LPVOID)o.PushRenderTargetAndViewport.address, &dPushRenderTargetAndViewport);
    if (o.PopRenderTargetAndViewport.valid)
        hkPopRenderTargetAndViewport.createHook((LPVOID)o.PopRenderTargetAndViewport.address, &dPopRenderTargetAndViewport);
    if (o.GetBackBufferDimensions.valid && !hkGetBackBufferDimensions.pTarget)
        hkGetBackBufferDimensions.createHook((LPVOID)o.GetBackBufferDimensions.address, &dGetBackBufferDimensions);
    if (o.GetScreenSize.valid && !hkGetScreenSize.pTarget)
        hkGetScreenSize.createHook((LPVOID)o.GetScreenSize.address, &dGetScreenSize);
    if (o.CreateNamedRTEx.valid)
        hkCreateNamedRTEx.createHook((LPVOID)o.CreateNamedRTEx.address, &dCreateNamedRTEx);
    if (o.VGui_Paint.valid && bmvr::TryVguiPaint())
        hkVgui_Paint.createHook((LPVOID)o.VGui_Paint.address, &dVGui_Paint);
    else if (o.VGui_Paint.valid)
        Game::logMsg("VGui_Paint createHook skipped (sticky vgui_paint)");
    // Real CModelRender::DrawModelExecute is engine.dll 0x113E80 (vtable +0x4C),
    // thiscall 3-arg, ret 0xC. 0xF6A20 is a displacement loader — do not hook it.
    if (o.DrawModelExecute.valid && bmvr::TryDrawModelExecute())
    {
        // Hold the sticky only around createHook. The old window lasted until
        // the first gameplay DME, so a later crash (ff_hmdfit) false-banned
        // yFix / scale / HideViewmodelArms.
        bmvr::BeginRisky(L"dme");
        if (hkDrawModelExecute.createHook((LPVOID)o.DrawModelExecute.address, &dDrawModelExecute) != 0)
        {
            bmvr::EndRisky(L"dme");
            Game::logMsg("DrawModelExecute createHook failed");
        }
        else
        {
            bmvr::EndRisky(L"dme");
            Game::logMsg("DrawModelExecute createHook CModelRender+0x4C rva=0x%X", o.DrawModelExecute.offset);
        }
    }
    else if (o.DrawModelExecute.valid)
        Game::logMsg("DrawModelExecute createHook skipped (sticky dme)");
    // LevelInit stays unhooked: MinHook on it crashed inside the original on
    // background01 (docs/RUNTIME.md). Map names come from GetLevelNameShort.
    if (m_Game->MaterialVTableMatchesDump() && m_Game->m_MaterialSystem)
    {
        void** vt = SehMaterialSystemVTable(m_Game->m_MaterialSystem);
        if (vt && vt[37])
            hkEndFrame.createHook(vt[37], &dEndFrame);
        else
            Game::logMsg("EndFrame hook skipped (no vtbl[37])");
    }
    else
        Game::logMsg("EndFrame hook skipped (IMaterialSystem dump slot 37 unverified on BM)");

    if (bmvr::TryMeleeTrace() && m_Game->m_EngineTrace)
    {
        void** vt = nullptr;
        __try { vt = *reinterpret_cast<void***>(m_Game->m_EngineTrace); }
        __except (EXCEPTION_EXECUTE_HANDLER) { vt = nullptr; }
        if (vt && vt[5])
            hkTraceRay.createHook(vt[5], &dTraceRay);
        else
            Game::logMsg("TraceRay hook skipped (no EngineTrace vtbl[5])");
    }

    EnsureClientFlashlightHook();
    EnsureWeaponShootOriginHooks();

    return 1;
}

namespace
{
    thread_local IMatRenderContext* g_MatCtx = nullptr;
    thread_local ITexture* g_StereoRedirect = nullptr;
    thread_local int g_VguiOverlayReentry = 0;
    thread_local int g_RenderViewNest = 0;
    constexpr int kRtStackMax = 32;
    struct RtStackEntry
    {
        char name[64];
        int w;
        int h;
        bool aux;
    };
    thread_local RtStackEntry g_RtStack[kRtStackMax]{};
    thread_local int g_RtStackDepth = 0;
    thread_local int g_AuxRtDepth = 0;
    bool g_CostActive = false;
    int g_ShadowPush = 0;
    int g_GbDepthPush = 0;
    int g_GbLightPush = 0;
    int g_LsMaskPush = 0;

    bool NestedRenderView()
    {
        return g_RenderViewNest > 1;
    }

    bool TextureNameIsAuxSceneRt(const char* name)
    {
        if (!name || !name[0] || name[0] == '?')
            return false;
        if (std::strstr(name, "backbuffer"))
            return false;
        if (std::strstr(name, "_rt_FullFrameFB") || std::strstr(name, "_rt_ResolvedFullFrame"))
            return false;
        // Gameplay G-buffers are the stereo scene. `_rt_gbXog` contains "xog"
        // and was treated as aux, so every later PushRT/Viewport/GetScreenSize
        // stayed 2560x1440 on 2544x2480 RTs (warp + bottom garbage, 2026-08-26).
        if (std::strstr(name, "_rt_gb")
            && !std::strstr(name, "shadow") && !std::strstr(name, "Shadow")
            && !std::strstr(name, "flashlight") && !std::strstr(name, "Flashlight")
            && !std::strstr(name, "csm") && !std::strstr(name, "CSM"))
            return false;
        if (std::strstr(name, "Water") || std::strstr(name, "water"))
            return true;
        if (std::strstr(name, "Reflect") || std::strstr(name, "Refract"))
            return true;
        if (std::strstr(name, "PowerOfTwo") || std::strstr(name, "_rt_Camera"))
            return true;
        if (std::strstr(name, "SmallFB") || std::strstr(name, "Small2FB") || std::strstr(name, "Small8FB")
            || std::strstr(name, "Small16FB") || std::strstr(name, "Small32FB"))
            return true;
        if (std::strstr(name, "shadow") || std::strstr(name, "Shadow") || std::strstr(name, "CSM")
            || std::strstr(name, "csm") || std::strstr(name, "flashlight") || std::strstr(name, "Flashlight"))
            return true;
        if (std::strstr(name, "Dof") || std::strstr(name, "xbow")
            || std::strstr(name, "gbShadow") || std::strstr(name, "_rt_ls"))
            return true;
        return false;
    }

    bool AuxSceneRtBound()
    {
        return g_AuxRtDepth > 0;
    }

    void NotePushRt(const char* name, int w, int h)
    {
        if (g_RtStackDepth >= kRtStackMax)
            return;
        RtStackEntry& e = g_RtStack[g_RtStackDepth++];
        e.w = w;
        e.h = h;
        e.name[0] = 0;
        if (name && name[0] && name[0] != '?')
            strncpy_s(e.name, name, _TRUNCATE);
        else
            strncpy_s(e.name, "backbuffer", _TRUNCATE);
        const bool smallVp = (w > 0 && h > 0 && (w < 640 || h < 360));
        e.aux = smallVp || TextureNameIsAuxSceneRt(e.name);
        if (e.aux)
            ++g_AuxRtDepth;
        if (g_CostActive && name)
        {
            if (std::strstr(name, "shadow") || std::strstr(name, "Shadow")
                || std::strstr(name, "CSM") || std::strstr(name, "csm"))
                ++g_ShadowPush;
            if (std::strstr(name, "gbdepth") || std::strstr(name, "gbDepth") || std::strstr(name, "GBDepth"))
                ++g_GbDepthPush;
            if (std::strstr(name, "gblight") || std::strstr(name, "gbLight") || std::strstr(name, "GBLight"))
                ++g_GbLightPush;
            if (std::strstr(name, "lsmask") || std::strstr(name, "lsMask") || std::strstr(name, "LSMask"))
                ++g_LsMaskPush;
        }
    }

    void NotePopRt()
    {
        if (g_RtStackDepth <= 0)
            return;
        const RtStackEntry e = g_RtStack[--g_RtStackDepth];
        if (e.aux && g_AuxRtDepth > 0)
            --g_AuxRtDepth;
    }

    struct RenderViewNestScope
    {
        RenderViewNestScope() { ++g_RenderViewNest; }
        ~RenderViewNestScope() { --g_RenderViewNest; }
    };

    void NoteMatContext(void* ecx)
    {
        if (ecx)
            g_MatCtx = reinterpret_cast<IMatRenderContext*>(ecx);
    }

    const char* SafeTextureName(ITexture* texture)
    {
        if (!texture)
            return "null";
        const char* name = "?";
        __try
        {
            name = texture->GetName();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            name = "?";
        }
        return name ? name : "?";
    }

    const char* SafeModelName(IModelInfo* info, void* model)
    {
        if (!info || !model)
            return nullptr;
        const char* name = nullptr;
        __try
        {
            name = info->GetModelName(model);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            name = nullptr;
        }
        return name;
    }

    int SafeLocalPlayerIndex()
    {
        if (!Hooks::m_Game || !Hooks::m_Game->m_EngineClient)
            return 0;
        int local = 0;
        __try
        {
            local = Hooks::m_Game->m_EngineClient->GetLocalPlayer();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            local = 0;
        }
        return local;
    }

    bool ModelNameIsViewmodel(const char* name)
    {
        if (!name || !name[0])
            return false;
        return std::strstr(name, "/v_") != nullptr
            || std::strstr(name, "\\v_") != nullptr
            || std::strstr(name, "/V_") != nullptr
            || std::strstr(name, "\\V_") != nullptr;
    }

    bool CachedModelIsViewmodel(void* pModel)
    {
        struct Entry { void* model; bool isVm; bool filled; };
        static Entry cache[256]{};
        if (!pModel)
            return false;
        const unsigned i = (static_cast<unsigned>(reinterpret_cast<uintptr_t>(pModel) >> 4)) & 255u;
        if (cache[i].filled && cache[i].model == pModel)
            return cache[i].isVm;
        const char* name = nullptr;
        if (Hooks::m_Game && Hooks::m_Game->m_ModelInfo)
            name = SafeModelName(Hooks::m_Game->m_ModelInfo, pModel);
        const bool vm = ModelNameIsViewmodel(name);
        cache[i] = { pModel, vm, true };
        return vm;
    }

    int g_CachedLocalPlayer = 0;
    bool g_HaveCachedLocalPlayer = false;

    int CachedLocalPlayerIndex()
    {
        if (g_HaveCachedLocalPlayer)
            return g_CachedLocalPlayer;
        g_CachedLocalPlayer = SafeLocalPlayerIndex();
        g_HaveCachedLocalPlayer = true;
        return g_CachedLocalPlayer;
    }

    long long QpcNow()
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return t.QuadPart;
    }

    double QpcMs(long long ticks)
    {
        static double s_ms = 0.0;
        if (s_ms == 0.0)
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            s_ms = 1000.0 / static_cast<double>(f.QuadPart);
        }
        return static_cast<double>(ticks) * s_ms;
    }

    struct StereoCost
    {
        long long leftTicks = 0;
        long long rightTicks = 0;
        long long blitTicks = 0;
        long long handsTicks = 0;
        long long dmeOrigTicks = 0;
        long long dmeHookTicks = 0;
        int dme = 0;
        int dmeL = 0;
        int dmeR = 0;
        int dmeEnt = 0;
        int dmeVm = 0;
        int dmeShadow = 0;
        int dmeBones32 = 0;
        int uniqEnt = 0;
        uint32_t seenEnt[128]{};
        void* lastCharModel = nullptr;
        int eye = 0;
        bool active = false;
    };
    StereoCost g_Cost{};

    void CostMarkEnt(int idx)
    {
        if (idx <= 1 || idx >= 4096)
            return;
        const unsigned w = static_cast<unsigned>(idx) >> 5;
        const unsigned b = static_cast<unsigned>(idx) & 31u;
        const uint32_t bit = 1u << b;
        if (g_Cost.seenEnt[w] & bit)
            return;
        g_Cost.seenEnt[w] |= bit;
        ++g_Cost.uniqEnt;
    }

    void StereoCostBegin()
    {
        g_Cost = StereoCost{};
        g_Cost.active = true;
        g_CostActive = true;
        g_ShadowPush = 0;
        g_GbDepthPush = 0;
        g_GbLightPush = 0;
        g_LsMaskPush = 0;
        g_HaveCachedLocalPlayer = false;
    }

    void StereoCostLog()
    {
        if (!g_Cost.active)
            return;
        g_Cost.active = false;
        g_CostActive = false;
        const double leftMs = QpcMs(g_Cost.leftTicks);
        const double rightMs = QpcMs(g_Cost.rightTicks);
        const double pairMs = leftMs + rightMs;
        const double blitMs = QpcMs(g_Cost.blitTicks);
        const double handsMs = QpcMs(g_Cost.handsTicks);
        const double origMs = QpcMs(g_Cost.dmeOrigTicks);
        const double hookMs = QpcMs(g_Cost.dmeHookTicks);
        static DWORD s_lastLogMs;
        const DWORD now = GetTickCount();
        const bool slow = pairMs >= 20.0;
        if (!slow && (now - s_lastLogMs) < 1000)
            return;
        if (slow && (now - s_lastLogMs) < 200)
            return;
        s_lastLogMs = now;
        const char* charName = "?";
        if (g_Cost.lastCharModel && Hooks::m_Game && Hooks::m_Game->m_ModelInfo)
        {
            const char* n = SafeModelName(Hooks::m_Game->m_ModelInfo, g_Cost.lastCharModel);
            if (n && n[0])
                charName = n;
        }
        Game::logMsg(
            "Stereo cost pair=%.1fms L=%.1f R=%.1f blit=%.1f hands=%.1f dme=%d (L=%d R=%d) orig=%.1fms hook=%.1fms ents=%d uniq=%d bones32+=%d shadowDme=%d shadowPush=%d vm=%d gbDepth=%d gbLight=%d lsMask=%d char=%s",
            pairMs, leftMs, rightMs, blitMs, handsMs,
            g_Cost.dme, g_Cost.dmeL, g_Cost.dmeR, origMs, hookMs,
            g_Cost.dmeEnt, g_Cost.uniqEnt, g_Cost.dmeBones32,
            g_Cost.dmeShadow, g_ShadowPush, g_Cost.dmeVm,
            g_GbDepthPush, g_GbLightPush, g_LsMaskPush, charName);
    }

    // Source DrawModelState_t (x86). CModelRender::DrawModelExecute
    // (engine 0x113E80) reads studiohdr flags at state[0]+0x98 and
    // drawFlags at state+0x14 — first pointer is studiohdr_t*, not CStudioHdr.
    struct DrawModelStateLite
    {
        unsigned char* studioHdr;
        void* studioHWData;
        void* renderable;
        const void* modelToWorld;
        int decals;
        int drawFlags;
        int lod;
    };

    constexpr int kStudioIdst = 0x54534449; // 'IDST'
    constexpr int kStudioHdrLength = 76;
    constexpr int kMaxStudioBones = 256;
    constexpr int kStudioHdrNumBones = 156;
    constexpr int kStudioHdrBoneIndex = 160;
    constexpr int kStudioHdrNumBodyparts = 232;
    constexpr int kStudioHdrBodypartIndex = 236;
    constexpr int kStudioBoneSize = 216;
    constexpr int kStudioBodypartSize = 16;
    constexpr int kStudioModelSize = 148;
    constexpr int kStudioModelNumMeshes = 72;

    struct StudioHdrPatch
    {
        int* p = nullptr;
        int original = 0;
    };
    constexpr int kMaxStudioHdrPatches = 96;
    StudioHdrPatch g_ArmHdrPatches[kMaxStudioHdrPatches]{};
    int g_ArmHdrPatchCount = 0;
    constexpr int kVmDrawSlots = 4;
    thread_local int g_VmDrawSlot = 0;
    thread_local float g_ScaledViewmodelBones[kVmDrawSlots][kMaxStudioBones][3][4];
    thread_local float g_ScaledViewmodelModelToWorld[kVmDrawSlots][3][4];
    thread_local unsigned char g_ScaledViewmodelInfo[kVmDrawSlots][sizeof(ModelRenderInfo_t)];
    // DT_LocalPlayerExclusive RecvTable (Ghidra FUN_100b7240): m_vecVelocity[0]
    // at +0xF8, not the L4D2 C_BasePlayer +0x100 (that is only .z here).
    constexpr int kLocalPlayerVecVelocity = 0xF8;
    // DT_BaseAnimating / DT_ServerAnimationData (Ghidra FUN_10090ac0 / FUN_10090f90).
    constexpr int kViewmodelSequence = 0x960;
    constexpr int kViewmodelCycle = 0x968;
    constexpr int kViewmodelPlaybackRate = 0x6E8;
    constexpr int kStudioHdrNumLocalSeq = 188;
    constexpr int kStudioHdrLocalSeqIndex = 192;
    constexpr int kStudioSeqDescSize = 212;
    constexpr int kStudioSeqLabelIndex = 4;

    unsigned char* g_LastViewmodelStudioHdr = nullptr;
    int g_LastViewmodelIdleSeq = -1;
    const char* StudioSequenceLabel(unsigned char* hdr, int seq);

    bool SequenceLabelLooksLikeWeaponAction(const char* label)
    {
        if (!label || !label[0])
            return false;
        char buf[96]{};
        for (int i = 0; i < 95 && label[i]; ++i)
            buf[i] = static_cast<char>(tolower(static_cast<unsigned char>(label[i])));
        auto has = [&](const char* s) { return std::strstr(buf, s) != nullptr; };
        if (has("reload") || has("fire") || has("shoot") || has("attack")
            || has("draw") || has("holster") || has("deploy") || has("pump")
            || has("bolt") || has("eject") || has("inspect") || has("admire")
            || has("pickup") || has("hit") || has("miss") || has("primary")
            || has("secondary") || has("ironsight") || has("_is"))
            return true;
        return false;
    }

    // Only explicit movement/idle names. Unknown/null labels must NOT suppress —
    // that froze fire/reload/draw (equip delayed several seconds).
    bool SequenceLabelLooksLikeLocomotionOnly(const char* label)
    {
        if (!label || !label[0])
            return false;
        char buf[96]{};
        for (int i = 0; i < 95 && label[i]; ++i)
            buf[i] = static_cast<char>(tolower(static_cast<unsigned char>(label[i])));
        auto has = [&](const char* s) { return std::strstr(buf, s) != nullptr; };
        // Keep draw/fire/reload even if the name also contains "idle" (rare).
        if (SequenceLabelLooksLikeWeaponAction(label))
            return false;
        if (has("sprint") || has("swim") || has("walk") || has("run") || has("bob"))
            return true;
        // Plain idle / fidget only — not idletosprint transitions that may share
        // bones with equip; freeze cycle, do not rewrite sequence index.
        if (has("fidget"))
            return true;
        if (has("idle") && !has("to") && !has("from"))
            return true;
        return false;
    }

    bool SequenceLabelLooksLikeEquipOnly(const char* label)
    {
        if (!label || !label[0])
            return false;
        char buf[96]{};
        for (int i = 0; i < 95 && label[i]; ++i)
            buf[i] = static_cast<char>(tolower(static_cast<unsigned char>(label[i])));
        auto has = [&](const char* s) { return std::strstr(buf, s) != nullptr; };
        return has("draw") || has("holster") || has("deploy") || has("pickup") || has("admire");
    }

    int FindIdleSequence(unsigned char* hdr)
    {
        if (!hdr)
            return -1;
        int numSeq = 0;
        __try
        {
            numSeq = *reinterpret_cast<int*>(hdr + kStudioHdrNumLocalSeq);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
        if (numSeq <= 0 || numSeq > 64)
            return -1;
        int best = -1;
        int bestScore = -1;
        for (int i = 0; i < numSeq; ++i)
        {
            const char* label = StudioSequenceLabel(hdr, i);
            if (!label || !label[0])
                continue;
            char buf[96]{};
            for (int n = 0; n < 95 && label[n]; ++n)
                buf[n] = static_cast<char>(tolower(static_cast<unsigned char>(label[n])));
            auto has = [&](const char* s) { return std::strstr(buf, s) != nullptr; };
            if (has("draw") || has("holster") || has("deploy") || has("reload")
                || has("fire") || has("shoot") || has("attack") || has("hit")
                || has("miss") || has("sprint") || has("low") || has("to") || has("from"))
                continue;
            int score = 0;
            if (std::strcmp(buf, "idle1") == 0)
                score = 4;
            else if (std::strcmp(buf, "idle") == 0)
                score = 3;
            else if (has("idle") && !has("fidget"))
                score = 2;
            else if (has("idle"))
                score = 1;
            if (score > bestScore)
            {
                bestScore = score;
                best = i;
            }
        }
        return best;
    }

    void ForceViewmodelIdlePose(void* viewmodel, unsigned char* hdr)
    {
        if (!viewmodel)
            return;
        const int idle = FindIdleSequence(hdr);
        __try
        {
            if (idle >= 0)
                *reinterpret_cast<int*>(static_cast<char*>(viewmodel) + kViewmodelSequence) = idle;
            *reinterpret_cast<float*>(static_cast<char*>(viewmodel) + kViewmodelCycle) = 0.f;
            *reinterpret_cast<float*>(static_cast<char*>(viewmodel) + kViewmodelPlaybackRate) = 0.f;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        static int s_idleLog;
        if (s_idleLog < 8)
        {
            const char* label = idle >= 0 ? StudioSequenceLabel(hdr, idle) : "?";
            Game::logMsg("Viewmodel force idle seq=%d '%s'", idle, label ? label : "?");
            ++s_idleLog;
        }
    }

    bool ViewmodelNameHas(const char* model, const char* token)
    {
        return model && token && std::strstr(model, token) != nullptr;
    }

    bool IsCrowbarViewmodel(const char* model)
    {
        return ViewmodelNameHas(model, "crowbar") || ViewmodelNameHas(model, "Crowbar")
            || ViewmodelNameHas(model, "wrench") || ViewmodelNameHas(model, "Wrench");
    }

    bool IsMp5Viewmodel(const char* model)
    {
        return ViewmodelNameHas(model, "mp5") || ViewmodelNameHas(model, "MP5")
            || ViewmodelNameHas(model, "smg") || ViewmodelNameHas(model, "mp5k");
    }

    void ZeroPlayerViewRecoil(void* player)
    {
        if (!player)
            return;
        // DT_LocalPlayerExclusive m_Local +0x103C. DT_Local m_vecPunchAngle +0x74,
        // m_vecPunchAngleVel +0xB0 (Ghidra FUN_100b7240 / FUN_100b6d20).
        // DT_BlackMesaPlayer m_recoilPunchAngles +0x1934, m_recoilPositionOffset +0x1940
        // (FUN_1025cb60).
        __try
        {
            float* punch = reinterpret_cast<float*>(
                static_cast<char*>(player) + 0x103C + 0x74);
            float* punchVel = reinterpret_cast<float*>(
                static_cast<char*>(player) + 0x103C + 0xB0);
            float* recoil = reinterpret_cast<float*>(
                static_cast<char*>(player) + 0x1934);
            float* recoilPos = reinterpret_cast<float*>(
                static_cast<char*>(player) + 0x1940);
            float* recoilStart = reinterpret_cast<float*>(
                static_cast<char*>(player) + 0x194C);
            punch[0] = punch[1] = punch[2] = 0.f;
            punchVel[0] = punchVel[1] = punchVel[2] = 0.f;
            recoil[0] = recoil[1] = recoil[2] = 0.f;
            recoilPos[0] = recoilPos[1] = recoilPos[2] = 0.f;
            *recoilStart = 0.f;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    const char* StudioSequenceLabel(unsigned char* hdr, int seq)
    {
        if (!hdr || seq < 0)
            return nullptr;
        int numSeq = 0;
        int seqIndex = 0;
        __try
        {
            numSeq = *reinterpret_cast<int*>(hdr + kStudioHdrNumLocalSeq);
            seqIndex = *reinterpret_cast<int*>(hdr + kStudioHdrLocalSeqIndex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
        if (numSeq <= 0 || seq >= numSeq || seqIndex <= 0)
            return nullptr;
        unsigned char* desc = hdr + seqIndex + seq * kStudioSeqDescSize;
        int labelOff = 0;
        __try
        {
            labelOff = *reinterpret_cast<int*>(desc + kStudioSeqLabelIndex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
        if (labelOff <= 0)
            return nullptr;
        return reinterpret_cast<const char*>(desc + labelOff);
    }

    void SuppressViewmodelMovementAnims(void* viewmodel)
    {
        if (!viewmodel || !Hooks::m_VR || !Hooks::m_VR->IsGameplayEligible())
            return;
        unsigned char* hdr = g_LastViewmodelStudioHdr;
        int seq = 0;
        __try
        {
            seq = *reinterpret_cast<int*>(static_cast<char*>(viewmodel) + kViewmodelSequence);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        const char* label = StudioSequenceLabel(hdr, seq);
        const bool melee = Hooks::m_VR->IsPerformingMelee();
        const bool isAction = SequenceLabelLooksLikeWeaponAction(label);
        const bool isLoco = SequenceLabelLooksLikeLocomotionOnly(label);
        const char* model = nullptr;
        if (Hooks::m_Game)
            model = Hooks::m_Game->GetEntityModelName(static_cast<C_BaseEntity*>(viewmodel));
        if (!model || !model[0])
            model = Hooks::m_VR->m_LastViewmodelModel.c_str();
        const bool crowbar = IsCrowbarViewmodel(model);

        auto restoreRate = [&]() {
            __try
            {
                float* rate = reinterpret_cast<float*>(static_cast<char*>(viewmodel) + kViewmodelPlaybackRate);
                if (*rate <= 0.01f)
                    *rate = 1.f;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        };
        auto freeze = [&]() {
            __try
            {
                *reinterpret_cast<float*>(static_cast<char*>(viewmodel) + kViewmodelCycle) = 0.f;
                *reinterpret_cast<float*>(static_cast<char*>(viewmodel) + kViewmodelPlaybackRate) = 0.f;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        };

        // Crowbar: keep only equip/draw. Attack/idle/sprint would otherwise
        // snap to the first frame of HITCENTER. Force the idle sequence so
        // the controller is the swing (implementation-plan §9).
        // Never freeze MP5 (or any other gun) fire/reload/draw here.
        if (crowbar)
        {
            if (SequenceLabelLooksLikeEquipOnly(label))
                restoreRate();
            else
                ForceViewmodelIdlePose(viewmodel, hdr);
            return;
        }

        // Crowbar VR swing: pin cycle once without thrashing every frame
        // (that made HITCENTER look jerky). Prefer idle hold when possible.
        if (melee)
        {
            if (isAction && label
                && (std::strstr(label, "hit") || std::strstr(label, "HIT")
                    || std::strstr(label, "miss") || std::strstr(label, "MISS")
                    || std::strstr(label, "attack") || std::strstr(label, "Attack")))
            {
                ForceViewmodelIdlePose(viewmodel, hdr);
            }
            return;
        }

        // Never rewrite m_nSequence. Freeze cycle/rate only for explicit
        // sprint/swim/walk/run/bob/idle/fidget. Never freeze draw/holster/
        // reload/fire/attack. If idle zeroed playbackRate, restore 1 on action.
        if (isLoco && !isAction)
            freeze();
        else
            restoreRate();
    }

    unsigned char* AsStudioHdr(void* p)
    {
        if (!p)
            return nullptr;
        int id = 0;
        int ver = 0;
        __try
        {
            id = *reinterpret_cast<int*>(p);
            ver = *(reinterpret_cast<int*>(p) + 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
        if (id != kStudioIdst || ver < 44 || ver > 49)
            return nullptr;
        return static_cast<unsigned char*>(p);
    }

    unsigned char* ResolveStudioHdr(void* state)
    {
        if (!state)
            return nullptr;
        DrawModelStateLite* st = nullptr;
        unsigned char* hdr = nullptr;
        void* inner = nullptr;
        __try
        {
            st = reinterpret_cast<DrawModelStateLite*>(state);
            hdr = AsStudioHdr(st->studioHdr);
            if (hdr)
                return hdr;
            inner = *reinterpret_cast<void**>(st->studioHdr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
        return AsStudioHdr(inner);
    }

    void ScaleMatrix3x4AroundPivot(float m[3][4], const float pivot[3], float s)
    {
        for (int r = 0; r < 3; ++r)
        {
            m[r][0] *= s;
            m[r][1] *= s;
            m[r][2] *= s;
            m[r][3] = pivot[r] + (m[r][3] - pivot[r]) * s;
        }
    }

    // Source VM layer builds yScale = xScale * aspect from the *window*
    // (l4d2vr-hands.md). Eye RTs are ~1.1 while the HWND is 16:9, so the gun
    // stretches along view-up: barrel when the controller is upright, grip
    // when it is on its side. Undo that in world space around the HMD camera.
    void UnstretchMatrix3x4ViewY(float m[3][4], const float cam[3],
        const float right[3], const float up[3], const float fwd[3], float yFix)
    {
        const float rel[3] = {
            m[0][3] - cam[0],
            m[1][3] - cam[1],
            m[2][3] - cam[2]
        };
        const float vx = rel[0] * right[0] + rel[1] * right[1] + rel[2] * right[2];
        const float vy = (rel[0] * up[0] + rel[1] * up[1] + rel[2] * up[2]) * yFix;
        const float vz = rel[0] * fwd[0] + rel[1] * fwd[1] + rel[2] * fwd[2];
        m[0][3] = cam[0] + right[0] * vx + up[0] * vy + fwd[0] * vz;
        m[1][3] = cam[1] + right[1] * vx + up[1] * vy + fwd[1] * vz;
        m[2][3] = cam[2] + right[2] * vx + up[2] * vy + fwd[2] * vz;
        const float k = yFix - 1.f;
        for (int c = 0; c < 3; ++c)
        {
            const float au = m[0][c] * up[0] + m[1][c] * up[1] + m[2][c] * up[2];
            m[0][c] += up[0] * au * k;
            m[1][c] += up[1] * au * k;
            m[2][c] += up[2] * au * k;
        }
    }

    void UnstretchPointViewY(float p[3], const float cam[3],
        const float right[3], const float up[3], const float fwd[3], float yFix)
    {
        const float rel[3] = { p[0] - cam[0], p[1] - cam[1], p[2] - cam[2] };
        const float vx = rel[0] * right[0] + rel[1] * right[1] + rel[2] * right[2];
        const float vy = (rel[0] * up[0] + rel[1] * up[1] + rel[2] * up[2]) * yFix;
        const float vz = rel[0] * fwd[0] + rel[1] * fwd[1] + rel[2] * fwd[2];
        p[0] = cam[0] + right[0] * vx + up[0] * vy + fwd[0] * vz;
        p[1] = cam[1] + right[1] * vx + up[1] * vy + fwd[1] * vz;
        p[2] = cam[2] + right[2] * vx + up[2] * vy + fwd[2] * vz;
    }

    void BuildMatrix3x4FromOrgAngles(float out[3][4], const Vector& origin, const QAngle& ang)
    {
        Vector f, r, u;
        QAngle::AngleVectors(ang, &f, &r, &u);
        out[0][0] = f.x; out[0][1] = r.x; out[0][2] = u.x; out[0][3] = origin.x;
        out[1][0] = f.y; out[1][1] = r.y; out[1][2] = u.y; out[1][3] = origin.y;
        out[2][0] = f.z; out[2][1] = r.z; out[2][2] = u.z; out[2][3] = origin.z;
    }

    void InvertMatrix3x4TR(const float in[3][4], float out[3][4])
    {
        out[0][0] = in[0][0]; out[0][1] = in[1][0]; out[0][2] = in[2][0];
        out[1][0] = in[0][1]; out[1][1] = in[1][1]; out[1][2] = in[2][1];
        out[2][0] = in[0][2]; out[2][1] = in[1][2]; out[2][2] = in[2][2];
        const float tx = -in[0][3];
        const float ty = -in[1][3];
        const float tz = -in[2][3];
        out[0][3] = tx * out[0][0] + ty * out[0][1] + tz * out[0][2];
        out[1][3] = tx * out[1][0] + ty * out[1][1] + tz * out[1][2];
        out[2][3] = tx * out[2][0] + ty * out[2][1] + tz * out[2][2];
    }

    void MulMatrix3x4(const float a[3][4], const float b[3][4], float out[3][4])
    {
        float tmp[3][4];
        for (int r = 0; r < 3; ++r)
        {
            tmp[r][0] = a[r][0] * b[0][0] + a[r][1] * b[1][0] + a[r][2] * b[2][0];
            tmp[r][1] = a[r][0] * b[0][1] + a[r][1] * b[1][1] + a[r][2] * b[2][1];
            tmp[r][2] = a[r][0] * b[0][2] + a[r][1] * b[1][2] + a[r][2] * b[2][2];
            tmp[r][3] = a[r][0] * b[0][3] + a[r][1] * b[1][3] + a[r][2] * b[2][3] + a[r][3];
        }
        std::memcpy(out, tmp, sizeof(tmp));
    }

    void ApplyMatrix3x4Delta(float m[3][4], const float delta[3][4])
    {
        float tmp[3][4];
        MulMatrix3x4(delta, m, tmp);
        std::memcpy(m, tmp, sizeof(tmp));
    }

    int StudioHdrNumBones(unsigned char* hdr)
    {
        if (!hdr)
            return 0;
        int n = 0;
        __try { n = *reinterpret_cast<int*>(hdr + kStudioHdrNumBones); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        if (n < 1 || n > kMaxStudioBones)
            return 0;
        return n;
    }

    bool SehCopyAndFixViewmodelMatrices(float* dst, const void* src, int count,
        const float pivot[3], float scale,
        const float cam[3], const float right[3], const float up[3], const float fwd[3],
        float yFix, const float* rigidDelta)
    {
        if (!dst || !src || count < 1)
            return false;
        __try
        {
            std::memcpy(dst, src, static_cast<size_t>(count) * 12 * sizeof(float));
            for (int i = 0; i < count; ++i)
            {
                float* m = dst + i * 12;
                auto matrix = reinterpret_cast<float(*)[4]>(m);
                if (rigidDelta)
                    ApplyMatrix3x4Delta(matrix, reinterpret_cast<const float(*)[4]>(rigidDelta));
                if (yFix > 0.2f && yFix < 2.f && fabsf(yFix - 1.f) > 0.03f)
                    UnstretchMatrix3x4ViewY(matrix, cam, right, up, fwd, yFix);
                if (scale > 0.2f && scale < 1.5f && fabsf(scale - 1.f) > 0.001f)
                    ScaleMatrix3x4AroundPivot(matrix, pivot, scale);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool SehWriteModelScale(void* renderable, float scale)
    {
        if (!renderable)
            return false;
        __try
        {
            *reinterpret_cast<float*>(reinterpret_cast<char*>(renderable) + 0x7C0) = scale;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    const char* StudioRelString(const unsigned char* base, int rel)
    {
        if (!base || rel <= 0 || rel > 0x100000)
            return "";
        const char* s = reinterpret_cast<const char*>(base + rel);
        __try
        {
            if (!s[0])
                return "";
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return "";
        }
        return s;
    }

    bool PokeInt(int* p, int value)
    {
        if (!p)
            return false;
        DWORD oldProt = 0;
        if (!VirtualProtect(p, sizeof(int), PAGE_READWRITE, &oldProt))
            return false;
        bool ok = false;
        __try
        {
            *p = value;
            ok = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ok = false;
        }
        DWORD tmp = 0;
        VirtualProtect(p, sizeof(int), oldProt, &tmp);
        return ok;
    }

    void RememberHdrPatch(int* p, int original)
    {
        if (!p || g_ArmHdrPatchCount >= kMaxStudioHdrPatches)
            return;
        for (int i = 0; i < g_ArmHdrPatchCount; ++i)
        {
            if (g_ArmHdrPatches[i].p == p)
                return;
        }
        g_ArmHdrPatches[g_ArmHdrPatchCount].p = p;
        g_ArmHdrPatches[g_ArmHdrPatchCount].original = original;
        ++g_ArmHdrPatchCount;
    }

    int WeaponRootScore(const char* n)
    {
        if (!n || !n[0])
            return -1;
        if (_stricmp(n, "R_Arm") == 0 || _stricmp(n, "L_Arm") == 0)
            return -1;
        if (std::strstr(n, "SCI_Hand") || std::strstr(n, "gman_arms"))
            return -1;
        char buf[64]{};
        strncpy_s(buf, n, _TRUNCATE);
        for (char* c = buf; *c; ++c)
            *c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
        if (std::strstr(buf, "projectile") || std::strstr(buf, "bullet")
            || std::strstr(buf, "clip") || std::strstr(buf, "trigger")
            || std::strstr(buf, "shell") || std::strstr(buf, "chamber")
            || std::strstr(buf, "loader") || std::strstr(buf, "eject")
            || std::strstr(buf, "muzzle") || std::strstr(buf, "bolt")
            || std::strstr(buf, "pump") || std::strstr(buf, "screen"))
            return 0;
        if (std::strstr(buf, "crowbar") || std::strstr(buf, "bone_gun")
            || std::strcmp(buf, "357") == 0 || std::strstr(buf, "mp5")
            || std::strstr(buf, "v_rpg") || std::strstr(buf, "tau")
            || std::strstr(buf, "egon") || std::strstr(buf, "gauss")
            || std::strstr(buf, "glock") || std::strstr(buf, "shotgun")
            || std::strstr(buf, "wrench") || std::strstr(buf, "hgun")
            || std::strstr(buf, "hive") || std::strcmp(buf, "mainbody") == 0)
            return 10;
        return 1;
    }

    void NoteWeaponRootFromHdr(unsigned char* hdr, const char* modelName)
    {
        if (!hdr || !Hooks::m_VR)
            return;
        int length = 0;
        int numbones = 0;
        int boneindex = 0;
        __try
        {
            length = *reinterpret_cast<int*>(hdr + kStudioHdrLength);
            numbones = *reinterpret_cast<int*>(hdr + kStudioHdrNumBones);
            boneindex = *reinterpret_cast<int*>(hdr + kStudioHdrBoneIndex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        if (length < 256 || length > 4 * 1024 * 1024)
            return;
        if (numbones <= 0 || numbones > 256)
            return;
        if (boneindex < 0 || boneindex + numbones * kStudioBoneSize > length)
            return;

        int bestScore = -1;
        int bestIndex = -1;
        float rest[3]{};
        char bestName[64]{};
        for (int i = 0; i < numbones; ++i)
        {
            unsigned char* bone = hdr + boneindex + i * kStudioBoneSize;
            int parent = -2;
            int szname = 0;
            float pos[3]{};
            __try
            {
                szname = *reinterpret_cast<int*>(bone);
                parent = *reinterpret_cast<int*>(bone + 4);
                pos[0] = *reinterpret_cast<float*>(bone + 32);
                pos[1] = *reinterpret_cast<float*>(bone + 36);
                pos[2] = *reinterpret_cast<float*>(bone + 40);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                continue;
            }
            if (parent != -1)
                continue;
            const char* n = StudioRelString(bone, szname);
            const int score = WeaponRootScore(n);
            if (score > bestScore)
            {
                bestScore = score;
                bestIndex = i;
                rest[0] = pos[0];
                rest[1] = pos[1];
                rest[2] = pos[2];
                strncpy_s(bestName, n, _TRUNCATE);
            }
        }
        if (bestScore < 1 || bestIndex < 0)
            return;
        Hooks::m_VR->NoteViewmodelWeaponBake(modelName, bestName, rest[0], rest[1], rest[2]);
    }

    struct HideArmsResult
    {
        int bodyparts = 0;
        int armsPart = -1;
        int armsModels = 0;
        int selected = -1;
        int meshesZeroed = 0;
        int alreadyZero = 0;
        char armsName[32]{};
        char gunName[64]{};
        int hwLods = -1;
        int hwMats = -1;
    };

    HideArmsResult InspectAndHideArmBodypart(void* state, int body, bool hideMeshes)
    {
        HideArmsResult r{};
        unsigned char* hdr = ResolveStudioHdr(state);
        if (!hdr)
            return r;

        if (state)
        {
            __try
            {
                auto* st = reinterpret_cast<DrawModelStateLite*>(state);
                int* hw = reinterpret_cast<int*>(st->studioHWData);
                if (hw)
                {
                    r.hwLods = hw[1];
                    if (r.hwLods > 0 && r.hwLods <= 8 && hw[2])
                    {
                        unsigned char* lod0 = reinterpret_cast<unsigned char*>(
                            static_cast<uintptr_t>(hw[2]));
                        r.hwMats = *reinterpret_cast<int*>(lod0 + 8);
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        int length = 0;
        int numbody = 0;
        int bodyindex = 0;
        __try
        {
            length = *reinterpret_cast<int*>(hdr + kStudioHdrLength);
            numbody = *reinterpret_cast<int*>(hdr + kStudioHdrNumBodyparts);
            bodyindex = *reinterpret_cast<int*>(hdr + kStudioHdrBodypartIndex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return r;
        }
        if (length < 256 || length > 4 * 1024 * 1024)
            return r;
        if (numbody <= 0 || numbody > 16)
            return r;
        if (bodyindex < 0 || bodyindex + numbody * kStudioBodypartSize > length)
            return r;
        r.bodyparts = numbody;

        for (int bi = 0; bi < numbody; ++bi)
        {
            unsigned char* bp = hdr + bodyindex + bi * kStudioBodypartSize;
            int szname = 0;
            int nummodels = 0;
            int base = 1;
            int modelindex = 0;
            __try
            {
                szname = *reinterpret_cast<int*>(bp);
                nummodels = *reinterpret_cast<int*>(bp + 4);
                base = *reinterpret_cast<int*>(bp + 8);
                modelindex = *reinterpret_cast<int*>(bp + 12);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                continue;
            }
            const char* bpName = StudioRelString(bp, szname);
            if (bi == 0)
                strncpy_s(r.gunName, bpName, _TRUNCATE);
            if (_stricmp(bpName, "arms") != 0)
                continue;
            r.armsPart = bi;
            r.armsModels = nummodels;
            strncpy_s(r.armsName, bpName, _TRUNCATE);
            if (nummodels <= 0 || nummodels > 16 || base <= 0)
                continue;
            r.selected = (body / base) % nummodels;
            if (modelindex < 0 || bp + modelindex + nummodels * kStudioModelSize > hdr + length)
                continue;
            for (int mi = 0; mi < nummodels; ++mi)
            {
                unsigned char* model = bp + modelindex + mi * kStudioModelSize;
                int* pMeshes = reinterpret_cast<int*>(model + kStudioModelNumMeshes);
                int nMesh = 0;
                __try { nMesh = *pMeshes; }
                __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
                if (nMesh < 0 || nMesh > 64)
                    continue;
                if (nMesh == 0)
                {
                    ++r.alreadyZero;
                    continue;
                }
                if (!hideMeshes)
                    continue;
                RememberHdrPatch(pMeshes, nMesh);
                if (PokeInt(pMeshes, 0))
                    ++r.meshesZeroed;
            }
        }
        return r;
    }

    void ApplyViewmodelStudioWork(void* state, const char* modelName, int body, int skin,
        bool hideArms, void* pCustomBoneToWorld)
    {
        unsigned char* hdr = ResolveStudioHdr(state);
        if (!hdr)
        {
            static int s_noHdr;
            if (s_noHdr < 4)
            {
                Game::logMsg("HideViewmodelArms no studiohdr %s", modelName ? modelName : "?");
                ++s_noHdr;
            }
        }
        NoteWeaponRootFromHdr(hdr, modelName);
        if (hdr)
            g_LastViewmodelStudioHdr = hdr;
        static char s_lastVm[260]{};
        if (modelName && modelName[0] && _stricmp(s_lastVm, modelName) != 0)
        {
            strncpy_s(s_lastVm, modelName, _TRUNCATE);
            g_LastViewmodelIdleSeq = -1;
        }
        const HideArmsResult hide = InspectAndHideArmBodypart(state, body, hideArms);
        static int s_armLog;
        if (s_armLog < 8 && (hideArms || hide.armsPart >= 0))
        {
            Game::logMsg(
                "HideViewmodelArms %s body=%d skin=%d parts=%d arms='%s' idx=%d/%d "
                "zeroed=%d already=%d hwLod=%d hwMat=%d hide=%d bones=%d",
                modelName ? modelName : "?", body, skin, hide.bodyparts,
                hide.armsName[0] ? hide.armsName : "?",
                hide.selected, hide.armsModels, hide.meshesZeroed, hide.alreadyZero,
                hide.hwLods, hide.hwMats, hideArms ? 1 : 0,
                pCustomBoneToWorld ? 1 : 0);
            ++s_armLog;
        }
    }

    void RestoreArmBodypartMeshes()
    {
        int n = 0;
        for (int i = 0; i < g_ArmHdrPatchCount; ++i)
        {
            if (g_ArmHdrPatches[i].p)
            {
                PokeInt(g_ArmHdrPatches[i].p, g_ArmHdrPatches[i].original);
                ++n;
            }
            g_ArmHdrPatches[i] = {};
        }
        if (g_ArmHdrPatchCount)
            Game::logMsg("HideViewmodelArms restored %d studiohdr nummeshes", n);
        g_ArmHdrPatchCount = 0;
    }

    struct SourceRenderQueueBuildScope
    {
        VR* vr = nullptr;
        bool active = false;
        SourceRenderQueueBuildScope(VR* owner, bool enabled)
            : vr(owner), active(enabled && owner != nullptr)
        {
            if (active)
            {
                std::lock_guard<std::recursive_mutex> consumerGate(vr->m_SourceRenderConsumerGate);
                vr->m_SourceRenderQueueBuildCount.fetch_add(1, std::memory_order_acq_rel);
            }
        }
        ~SourceRenderQueueBuildScope()
        {
            if (active)
            {
                std::lock_guard<std::recursive_mutex> consumerGate(vr->m_SourceRenderConsumerGate);
                vr->m_SourceRenderQueueBuildCount.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
        SourceRenderQueueBuildScope(const SourceRenderQueueBuildScope&) = delete;
        SourceRenderQueueBuildScope& operator=(const SourceRenderQueueBuildScope&) = delete;
    };

    // Only the HDR scene color buffer. Redirecting 8192 CSM/flashlight
    // targets into the eye RT (and keeping their 8K depth) died on the first
    // named-RT stereo frame (2026-08-17, `_rt_gbshadowmaprt`).
    bool IsStereoSceneColorTarget(ITexture* texture)
    {
        const char* name = SafeTextureName(texture);
        if (!name || !name[0] || name[0] == '?')
            return false;
        if (std::strstr(name, "shadow") || std::strstr(name, "Shadow"))
            return false;
        if (std::strstr(name, "flashlight") || std::strstr(name, "Flashlight"))
            return false;
        if (std::strstr(name, "csm") || std::strstr(name, "CSM"))
            return false;
        if (std::strstr(name, "water") || std::strstr(name, "Water"))
            return false;
        if (std::strstr(name, "depth") || std::strstr(name, "Depth"))
            return false;
        // HUD PushRT at RenderView 1020fa90 uses FindTexture "_rt_gui"
        // (IMaterialSystem +0x150) then context PushRT +0x23C. Leave it.
        if (std::strstr(name, "_rt_gui"))
            return false;
        if (std::strstr(name, "bmvrHUD") || std::strstr(name, "vrHUD"))
            return false;
        return std::strstr(name, "_rt_FullFrameFB") != nullptr;
    }

    // Isolated for MSVC: __try cannot live in a function with C++ unwind objects.
    IMatRenderContext* GetMatRenderContextFromMatsys()
    {
        if (!Hooks::m_Game || !Hooks::m_Game->m_MaterialSystem)
            return nullptr;
        void* mat = Hooks::m_Game->m_MaterialSystem;
        IMatRenderContext* ctx = nullptr;
        __try
        {
            void** vt = *reinterpret_cast<void***>(mat);
            if (vt)
            {
                using Fn = IMatRenderContext*(__thiscall*)(void*);
                auto fn = reinterpret_cast<Fn>(vt[Offsets::kIMaterialSystem_GetRenderContext / 4]);
                if (fn)
                    ctx = fn(mat);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ctx = nullptr;
        }
        return ctx;
    }

    void ContextRelease(IMatRenderContext* ctx)
    {
        if (!ctx)
            return;
        __try
        {
            void** vt = *reinterpret_cast<void***>(ctx);
            if (!vt)
                return;
            using Fn = int(__thiscall*)(void*);
            auto fn = reinterpret_cast<Fn>(vt[Offsets::kIMatRenderContext_Release / 4]);
            if (fn)
                fn(ctx);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void CallSetAbsOriginAngles(void* ent, const float* origin, const float* angles)
    {
        if (!ent || !origin || !angles)
            return;
        HMODULE client = GetModuleHandleA("client.dll");
        if (!client)
            return;
        using SetVecFn = void(__thiscall*)(void*, const float*);
        auto setOrigin = reinterpret_cast<SetVecFn>(
            reinterpret_cast<uintptr_t>(client) + Offsets::kCBaseEntity_SetAbsOrigin);
        auto setAngles = reinterpret_cast<SetVecFn>(
            reinterpret_cast<uintptr_t>(client) + Offsets::kCBaseEntity_SetAbsAngles);
        __try
        {
            setOrigin(ent, origin);
            setAngles(ent, angles);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    void SnapLocalViewmodelForFire()
    {
        if (!Hooks::m_VR || !Hooks::m_Game || !Hooks::m_VR->m_IsVREnabled)
            return;
        if (!Hooks::m_VR->m_ControllerPoseValid || !Hooks::m_VR->IsGameplayEligible())
            return;
        C_BaseEntity* vm = Hooks::m_Game->GetViewModelEntity();
        if (!vm)
            return;
        Vector body = Hooks::m_VR->m_HasStereoBodyOrigin ? Hooks::m_VR->m_StereoBodyOrigin : Hooks::m_VR->m_SetupOrigin;
        if (body.LengthSqr() <= 1.f)
            body = Hooks::m_VR->m_SetupOrigin;
        const Vector targetOrigin = Hooks::m_VR->GetRecommendedViewmodelAbsPos(body);
        const QAngle targetAng = Hooks::m_VR->GetRecommendedViewmodelAbsAngle();
        const float origin3[3] = { targetOrigin.x, targetOrigin.y, targetOrigin.z };
        const float angles3[3] = { targetAng.x, targetAng.y, targetAng.z };
        CallSetAbsOriginAngles(vm, origin3, angles3);
    }

    void CallCalcViewModelViewOriginal(void* ecx, void* owner, const Vector& eyePosition, const QAngle& eyeAngles)
    {
        if (!Hooks::hkCalcViewModelView.fOriginal)
            return;
        Vector savedVel{};
        bool zeroVel = bmvr::g_DisableViewBob && owner;
        if (zeroVel)
        {
            __try
            {
                savedVel = *reinterpret_cast<Vector*>(reinterpret_cast<char*>(owner) + kLocalPlayerVecVelocity);
                *reinterpret_cast<Vector*>(reinterpret_cast<char*>(owner) + kLocalPlayerVecVelocity) = Vector(0.f, 0.f, 0.f);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                zeroVel = false;
            }
        }
        Hooks::hkCalcViewModelView.fOriginal(ecx, owner, eyePosition, eyeAngles);
        if (zeroVel)
        {
            __try
            {
                *reinterpret_cast<Vector*>(reinterpret_cast<char*>(owner) + kLocalPlayerVecVelocity) = savedVel;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    void ContextSetRenderTarget(IMatRenderContext* ctx, ITexture* texture)
    {
        if (!ctx)
            return;
        __try
        {
            void** vt = *reinterpret_cast<void***>(ctx);
            if (!vt)
                return;
            using Fn = void(__thiscall*)(void*, ITexture*);
            auto fn = reinterpret_cast<Fn>(vt[Offsets::kIMatRenderContext_SetRenderTarget / 4]);
            if (fn)
                fn(ctx, texture);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // L4D2VR CRefPtr: GetRenderContext already AddRefs; we own that one ref.
    struct MatCtxScope
    {
        IMatRenderContext* ctx = nullptr;
        bool owned = false;

        MatCtxScope()
        {
            ctx = GetMatRenderContextFromMatsys();
            if (ctx)
                owned = true;
            else
                ctx = g_MatCtx;
        }

        ~MatCtxScope()
        {
            if (owned && ctx)
                ContextRelease(ctx);
        }

        MatCtxScope(const MatCtxScope&) = delete;
        MatCtxScope& operator=(const MatCtxScope&) = delete;
    };

    // L4D2VR EyeRenderTargetScope: original PushRT(eye)/PopRT around RenderView.
    // Fallback is BM-verified SetRT at context +0x18 (RenderView 1020f5e4).
    struct EyeRtPush
    {
        IMatRenderContext* ctx = nullptr;
        ITexture* oldRT = nullptr;
        int oldX = 0;
        int oldY = 0;
        int oldW = 0;
        int oldH = 0;
        bool hasViewport = false;
        bool pushed = false;

        EyeRtPush(IMatRenderContext* renderContext, ITexture* target, int width, int height)
            : ctx(renderContext)
        {
            if (!ctx || !target)
                return;
            if (Hooks::hkPushRenderTargetAndViewport.fOriginal
                && Hooks::hkPopRenderTargetAndViewport.fOriginal)
            {
                Hooks::hkPushRenderTargetAndViewport.fOriginal(
                    ctx, target, nullptr, 0, 0, width, height);
                pushed = true;
                return;
            }
            if (Hooks::hkGetRenderTarget.fOriginal)
                oldRT = Hooks::hkGetRenderTarget.fOriginal(ctx);
            if (Hooks::hkGetViewport.fOriginal && Hooks::hkViewport.fOriginal)
            {
                Hooks::hkGetViewport.fOriginal(ctx, oldX, oldY, oldW, oldH);
                hasViewport = true;
            }
            ContextSetRenderTarget(ctx, target);
            if (Hooks::hkViewport.fOriginal)
                Hooks::hkViewport.fOriginal(ctx, 0, 0, width, height);
        }

        ~EyeRtPush()
        {
            if (!ctx)
                return;
            if (pushed)
            {
                if (Hooks::hkPopRenderTargetAndViewport.fOriginal)
                    Hooks::hkPopRenderTargetAndViewport.fOriginal(ctx);
                return;
            }
            ContextSetRenderTarget(ctx, oldRT);
            if (hasViewport && Hooks::hkViewport.fOriginal)
                Hooks::hkViewport.fOriginal(ctx, oldX, oldY, oldW, oldH);
        }

        EyeRtPush(const EyeRtPush&) = delete;
        EyeRtPush& operator=(const EyeRtPush&) = delete;
    };

    void NormalizeViewSetupForVREye(CViewSetup& view, const VR* vr)
    {
        // L4D2VR hooks_render.inl: eye copies are HMD size + HMD FOV + HMD
        // aspect. 16:9 engine FOV + center-crop (2026-08-18) zoomed the world
        // (objects huge), broke distance fusion (IPD still ~2.5in), and sat
        // the camera above NPC eye level. Do not write height into 0x1C
        // (that is m_eStereoEye on BM). Do not force zNear=6.
        const int eyeWidth = static_cast<int>(vr->m_RenderWidth);
        const int eyeHeight = static_cast<int>(vr->m_RenderHeight);
        if (eyeWidth < 640 || eyeHeight < 360)
            return;
        view.x = 0;
        view.y = 0;
        view.m_nUnscaledX = 0;
        view.m_nUnscaledY = 0;
        // fl_gbmatch: keep engine 2560 width/height so deferred flashlight
        // apply matches the G-buffer. Still write HMD fov/aspect (below).
        if (!bmvr::UseGbMatchViewLock())
        {
            view.width = eyeWidth;
            view.height = eyeHeight;
            view.m_nUnscaledWidth = eyeWidth;
            view.m_nUnscaledHeight = eyeHeight;
        }
        // Viewmodel pass uses viewport aspect, not m_flAspectRatio (L4D2VR
        // vr_hand_math / l4d2vr-hands.md). Match both to the eye RT so the gun
        // is not 16:9-projected into a ~1.1 HMD buffer (tall grip, short slide).
        const float rtAspect = static_cast<float>(eyeWidth) / static_cast<float>(eyeHeight);
        view.m_flAspectRatio = rtAspect;
        view.fov = vr->m_Fov;
        view.fovViewmodel = vr->m_Fov;
        // L4D2VR: native Source viewmodels use a separate projection and a
        // compressed 0..0.1 depth range so they draw over the world. In VR the
        // gun is a world-space mesh; keep Z (and therefore size vs world
        // geometry) on the same projection as the eye scene.
        if (view.zNear > 0.01f)
            view.zNearViewmodel = view.zNear;
        if (view.zFar > view.zNear)
            view.zFarViewmodel = view.zFar;
    }

    bool IsOffscreenWorldRtName(const char* name)
    {
        if (!name || !name[0] || name[0] == '?')
            return false;
        if (std::strstr(name, "shadow") || std::strstr(name, "Shadow")
            || std::strstr(name, "flashlight") || std::strstr(name, "Flashlight")
            || std::strstr(name, "csm") || std::strstr(name, "CSM")
            || std::strstr(name, "Hud") || std::strstr(name, "gui")
            || std::strstr(name, "Dof") || std::strstr(name, "Water")
            || std::strstr(name, "Camera") || std::strstr(name, "_rt_ls"))
            return false;
        return std::strstr(name, "_rt_FullFrame") != nullptr
            || std::strstr(name, "_rt_ResolvedFullFrame") != nullptr
            || std::strstr(name, "_rt_gb") != nullptr;
    }

    bool OffscreenStereoSizeLie(int& width, int& height)
    {
        if (!bmvr::OffscreenWorldMatchesEyes() || !Hooks::m_VR)
            return false;
        // Skip only HUD / true aux. Stereo callOriginal stays nest 1, but
        // inner views must still see HMD size.
        if (AuxSceneRtBound() || Hooks::m_VR->HudPaintActive())
            return false;
        if (!Hooks::m_VR->StereoEyeBlitActive() && Hooks::m_VR->m_StereoEye == 0)
            return false;
        const int eyeW = static_cast<int>(Hooks::m_VR->m_RenderWidth);
        const int eyeH = static_cast<int>(Hooks::m_VR->m_RenderHeight);
        if (eyeW < 640 || eyeH < 360)
            return false;
        width = eyeW;
        height = eyeH;
        return true;
    }

    bool LooksLikeAuxSceneView(const CViewSetup& setup)
    {
        if (!Hooks::m_VR)
            return false;
        const Vector body = Hooks::m_VR->m_HasStereoBodyOrigin
            ? Hooks::m_VR->m_StereoBodyOrigin
            : Hooks::m_VR->m_SetupOrigin;
        const float dx = setup.origin.x - body.x;
        const float dy = setup.origin.y - body.y;
        const float dz = setup.origin.z - body.z;
        if ((dx * dx + dy * dy + dz * dz) > 25.f)
            return true;
        const Vector va = Hooks::m_VR->GetViewAngle();
        if (fabsf(setup.angles.x + va.x) < 12.f && fabsf(va.x) > 1.f)
            return true;
        return false;
    }

    void ClampStereoViewport(int& x, int& y, int& width, int& height)
    {
        if (!Hooks::m_VR)
            return;
        if (AuxSceneRtBound())
            return;
        if (NestedRenderView() && !Hooks::m_VR->StereoEyeBlitActive()
            && Hooks::m_VR->m_StereoEye == 0)
            return;
        if (Hooks::m_VR->HudPaintActive())
        {
            int hx = 0, hy = 0, hw = 0, hh = 0;
            int fbW = static_cast<int>(Hooks::m_VR->m_RenderWidth);
            int fbH = static_cast<int>(Hooks::m_VR->m_RenderHeight);
            uint32_t hmdW = 0, hmdH = 0;
            if (bmvr::HaveHmdFramebufferSize(hmdW, hmdH))
            {
                fbW = static_cast<int>(hmdW);
                fbH = static_cast<int>(hmdH);
            }
            if (fbW < 640)
                fbW = (width > 0) ? width : 1280;
            if (fbH < 360)
                fbH = (height > 0) ? height : 720;
            if (Hooks::m_VR->ComputeHudInset(fbW, fbH, hx, hy, hw, hh))
            {
                x = hx;
                y = hy;
                width = hw;
                height = hh;
            }
            return;
        }
        // Water / PowerOfTwo / cubemap DrawSetup never calls RenderView, so
        // nest stays 1. Do not expand a 512 reflection viewport to the HMD
        // eye — that stamps a view-locked world into the reflection RT.
        if (width > 0 && height > 0 && (width < 640 || height < 360))
            return;
        if (!g_StereoRedirect && !Hooks::m_VR->StereoEyeBlitActive()
            && Hooks::m_VR->m_StereoEye == 0)
            return;
        const int eyeW = static_cast<int>(Hooks::m_VR->m_RenderWidth);
        const int eyeH = static_cast<int>(Hooks::m_VR->m_RenderHeight);
        if (eyeW < 640 || eyeH < 360)
            return;
        if (width > 0 && height > 0 && width + 32 < eyeW && height + 32 < eyeH)
            return;
        // Flashlight apply PushRT/Viewport is 2560 (G-buffer actual). Forcing
        // 1584 here is why the beam never landed in fused eyes. Keep engine
        // size; squash-blit after RenderView.
        if (bmvr::UseGbMatchViewLock())
            return;
        x = 0;
        y = 0;
        width = eyeW;
        height = eyeH;
    }

    void BindStereoPushToEye(ITexture*& pTexture, ITexture*& pDepthTexture, int& nViewX, int& nViewY, int& nViewW, int& nViewH, bool& redirected)
    {
        redirected = false;
        if (!g_StereoRedirect || !Hooks::m_VR)
            return;
        const int eyeW = static_cast<int>(Hooks::m_VR->m_RenderWidth);
        const int eyeH = static_cast<int>(Hooks::m_VR->m_RenderHeight);
        if (eyeW < 640 || eyeH < 360)
            return;
        if (pTexture == g_StereoRedirect)
            return;
        // Shadows / flashlight / water stay on their own RTs.
        if (pTexture && !IsStereoSceneColorTarget(pTexture) && nViewW >= 640 && nViewH >= 360)
            return;
        // NULL = backbuffer. ViewDrawScene / CSimpleWorldView PushRT(NULL,0x0)
        // three times. Binding the LDR swapchain on top of the HDR eye killed
        // named_push. Rewrite while g_StereoRedirect is set.
        pTexture = g_StereoRedirect;
        pDepthTexture = nullptr;
        nViewX = 0;
        nViewY = 0;
        nViewW = eyeW;
        nViewH = eyeH;
        redirected = true;
    }

    // L4D2VR PaintToHudOnce: extra VGui_Paint into vrHUD. Engine dest stays
    // untouched so desktop VGUI survives. BM never PushRT `_rt_gui` during
    // stereo gameplay (log: only gbuffer/null), so the PopRT blit never ran
    // and the overlay stayed hidden — headset only saw in-eye HUD at the
    // 16:9 edges (2026-08-18).
    void PaintVguiToOverlay(void* vgui, int mode)
    {
        (void)mode;
        VR* vr = Hooks::m_VR;
        if (!vr || !Hooks::hkVgui_Paint.fOriginal)
            return;
        if (!vr->HudOverlayReady() || !vr->m_HUDTexture)
            return;
        // Extra paint is engine VGUI only. Forcing UIPANELS|INGAMEPANELS|CURSOR
        // every gameplay frame (log: mode=0x7 pause=0) copies GameUI onto a
        // cleared-transparent SteamVR quad. That is the transparent pause menu
        // in the HMD. HEV HUD is client DRAWHUD, not this path.
        if (!vr->PauseUiActive())
            return;
        if (vr->HudPaintedThisFrame())
            return;

        ITexture* hud = vr->m_HUDTexture;
        const int tw = hud->GetActualWidth();
        const int th = hud->GetActualHeight();
        if (tw < 640 || th < 360)
            return;

        int x = 0, y = 0, w = tw, h = th;
        vr->ComputeHudInset(tw, th, x, y, w, h);

        MatCtxScope scope;
        if (!scope.ctx)
            return;

        vr->ClearHudSurface(false);
        vr->SetHudPaintActive(true);
        {
            EyeRtPush push(scope.ctx, hud, tw, th);
            if (Hooks::hkViewport.fOriginal)
                Hooks::hkViewport.fOriginal(scope.ctx, x, y, w, h);
            const int paintMode = PAINT_UIPANELS | PAINT_CURSOR;
            ++g_VguiOverlayReentry;
            Hooks::hkVgui_Paint.fOriginal(vgui, paintMode);
            --g_VguiOverlayReentry;
            static int s_ov;
            if (s_ov < 8)
            {
                Game::logMsg("VGui extra-paint overlay inset=%d,%d %dx%d of %dx%d mode=0x%X pause=%d",
                    x, y, w, h, tw, th, paintMode, vr->PauseUiActive() ? 1 : 0);
                ++s_ov;
            }
        }
        vr->SetHudPaintActive(false);
        vr->NoteHudPainted();
    }
}

ITexture* __fastcall Hooks::dGetRenderTarget(void* ecx, void* edx)
{
    (void)edx;
    NoteMatContext(ecx);
    return hkGetRenderTarget.fOriginal(ecx);
}

void __fastcall Hooks::dRenderView(void* ecx, void* edx, CViewSetup& setup, int nClearFlags, int whatToDraw)
{
    (void)edx;
    RenderViewNestScope nest;
    if (!hkRenderView.fOriginal)
        return;

    if (g_RenderViewNest > 1)
    {
        hkRenderView.fOriginal(ecx, setup, nClearFlags, whatToDraw);
        return;
    }

    auto callOriginal = [&](CViewSetup& view, int clearFlags, int drawFlags) {
        hkRenderView.fOriginal(ecx, view, clearFlags, drawFlags);
    };

    if (m_VR)
        m_VR->FlushPendingGameUi();

    EnsureServerFlashlightHook();
    if (m_VR)
    {
        m_VR->ApplyRenderTargetFramebufferOverride();
        m_VR->LogFullFrameSizeIfReady();
    }

    const bool queued = m_Game && m_Game->GetMatQueueMode() != 0;
    SourceRenderQueueBuildScope sourceRenderQueueBuildScope(m_VR, queued);

    const bool mainView = setup.width >= 640 && setup.height >= 360;
    const bool inGame = m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();

    if (m_VR && mainView)
        m_VR->m_SetupOrigin = setup.origin;

    if (m_VR && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible() && inGame)
        m_VR->WaitPosesForStereoFrame();

    if (m_VR && m_Game && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible()
        && inGame && mainView && !m_VR->m_StereoEyesDrawnThisFrame)
        m_VR->TickMatQueueFromRenderView();

    // L4D2VR/Portal2: HMD angles + origin on a COPY. Writing the same values
    // onto the live CViewSetup is abs_view (tram camera blacked).
    // Do not SetViewAngles from RenderView — L4D2VR only does that in
    // mat_queue_mode 0 and restores. Permanent overwrite + CreateMove HMD
    // angles fights interpolation (rubberband). CreateMove already writes
    // cmd->viewangles.
    auto applyL4d2VrHead = [&](CViewSetup& view, bool stereo, bool leftEye) {
        if (!m_VR || !m_VR->m_HmdPoseValid)
            return;
        view.angles = m_VR->GetViewAngle();
        if (stereo)
            view.origin = leftEye ? m_VR->GetViewOriginLeft(setup.origin)
                                  : m_VR->GetViewOriginRight(setup.origin);
        else
            view.origin = m_VR->GetViewOrigin(setup.origin);
    };

    auto passThrough = [&]() {
        if (m_VR && !m_VR->m_StereoEyesDrawnThisFrame)
        {
            m_VR->m_DirectEyeSubmit = false;
            m_VR->m_StereoRenderViewActive = false;
        }
        if (m_VR && m_VR->IsGameplayEligible() && inGame
            && mainView && m_VR->m_HmdPoseValid)
        {
            CViewSetup vrView = setup;
            applyL4d2VrHead(vrView, false, true);

            static int s_relLog;
            if (s_relLog < 4)
            {
                Game::logMsg("L4D2VR look copy engine=(%.1f,%.1f) hmd=(%.1f,%.1f) out=(%.1f,%.1f)",
                    setup.angles.x, setup.angles.y,
                    m_VR->m_HmdAngAbs.x, m_VR->m_HmdAngAbs.y,
                    vrView.angles.x, vrView.angles.y);
                ++s_relLog;
            }

            static int s_relFrames;
            if (s_relFrames == 0)
                bmvr::BeginRisky(L"rel_look");
            callOriginal(vrView, nClearFlags, whatToDraw);
            ++s_relFrames;
            if (s_relFrames == 8)
                bmvr::EndRisky(L"rel_look");
            return;
        }
        callOriginal(setup, nClearFlags, whatToDraw);
    };

    if (!m_VR || !m_VR->m_IsVREnabled)
    {
        callOriginal(setup, nClearFlags, whatToDraw);
        return;
    }
    if (mainView && m_VR->IsGameplayEligible())
    {
        static int s_setupLog;
        if (s_setupLog < 8)
        {
            Game::logMsg("RenderView setup=%dx%d fov=%.1f aspect=%.3f origin=(%.1f,%.1f,%.1f) ang=(%.1f,%.1f) whatToDraw=0x%X submits=%d inGame=%d pass=%u",
                setup.width, setup.height, setup.fov, setup.m_flAspectRatio,
                setup.origin.x, setup.origin.y, setup.origin.z,
                setup.angles.x, setup.angles.y, whatToDraw, m_VR->m_SubmitCount, inGame ? 1 : 0,
                m_VR->m_PassThroughMainViews);
            ++s_setupLog;
        }
    }
    else if (m_VR->IsGameplayEligible())
    {
        static int s_smallLog;
        if (s_smallLog < 8)
        {
            Game::logMsg("RenderView small setup=%dx%d inGame=%d", setup.width, setup.height, inGame ? 1 : 0);
            ++s_smallLog;
        }
    }

    const bool originOk = std::isfinite(setup.origin.x) && std::isfinite(setup.origin.y)
        && std::isfinite(setup.origin.z)
        && fabsf(setup.origin.x) < 100000.f && fabsf(setup.origin.y) < 100000.f;
    const bool fovOk = std::isfinite(setup.fov) && setup.fov > 10.f && setup.fov < 170.f;

    // 2026-08-17: GetScreenSize made the first gameplay view 1584x1440, then
    // this hook immediately ran two RenderViews with HMD FOV/zNear/IPD during
    // spawn. Died inside the left callOriginal. Menu 1584 and that first
    // setup= line both succeeded. Wait for pass-through world frames first.
    constexpr uint32_t kStereoAfterPassThrough = VR::kPassThroughViewsBeforeQueued;
    if (bmvr::TryStereoRenderView()
        && m_VR->IsGameplayEligible()
        && inGame
        && mainView
        && originOk
        && fovOk
        && m_VR->StereoEyesReady()
        && m_VR->m_PassThroughMainViews < kStereoAfterPassThrough)
    {
        ++m_VR->m_PassThroughMainViews;
        if (m_VR->m_PassThroughMainViews == 1)
            bmvr::BeginRisky(L"hmd_fb");
        static int s_ptLog;
        if (s_ptLog < 10)
        {
            Game::logMsg("HMD-fb pass-through %u/%u inGame=%d setup=%dx%d fov=%.1f zNear=%.1f",
                m_VR->m_PassThroughMainViews, kStereoAfterPassThrough, inGame ? 1 : 0,
                setup.width, setup.height, setup.fov, setup.zNear);
            ++s_ptLog;
        }
        passThrough();
        return;
    }

    // Named PushRT wrap onto HDR leftEye0 dies in CSimpleWorldView after three
    // rewritten PushRT(NULL) — even when the named RT is 2560x1440, matching
    // the G-buffer (2026-08-17). L4D2's real fix is game render size = HMD
    // size. On BM that is GetBackBufferDimensions / FullFrameFB at HMD aspect
    // with the 16:9 window left alone, then two RenderViews + blit (the path
    // that survived as stereo_copy) instead of a second HDR RT.
    if (bmvr::TryStereoRenderView()
        && m_VR->IsGameplayEligible()
        && inGame
        && mainView
        && originOk
        && fovOk
        && m_VR->StereoEyesReady())
    {
        if (m_VR->m_StereoEyesDrawnThisFrame)
        {
            // 47777b5: skip only same-size duplicate player views after the
            // stereo pair. d24bc07 also skipped leftover 16:9 mains
            // (windowed169) — that dropped the deferred flashlight apply that
            // still painted the desktop G-buffer. Nested water/cubemap already
            // returned at nest>1. Reflections at 1024² must still run.
            const int eyeW = static_cast<int>(m_VR->m_RenderWidth);
            const int eyeH = static_cast<int>(m_VR->m_RenderHeight);
            const bool sameAsStereo = eyeW >= 640 && std::abs(setup.width - eyeW) < 32
                && std::abs(setup.height - eyeH) < 32;
            const bool windowed169 = setup.m_flAspectRatio > 1.45f && setup.width >= 1600;
            // Leftover policy: gbmatch + gb_leftskip skips the 16:9 desktop
            // main (2 scene renders). gbmatch without the skip renders it.
            // No-gbmatch uses DesktopLeftoverRender (default off).
            const bool gbSkipLeftover = bmvr::TryFlashlightGbMatch() && bmvr::TryGbLeftSkip();
            const bool skipLeftover = bmvr::TryFlashlightGbMatch()
                ? gbSkipLeftover
                : !bmvr::g_DesktopLeftoverRender;
            if ((sameAsStereo || windowed169)
                && !LooksLikeAuxSceneView(setup)
                && skipLeftover)
            {
                static int s_gbLeftSkipArmed;
                if (gbSkipLeftover && !s_gbLeftSkipArmed)
                {
                    s_gbLeftSkipArmed = 1;
                    bmvr::BeginRisky(L"gb_leftskip");
                }
                static int s_skipDup;
                if (s_skipDup < 8)
                {
                    Game::logMsg("Skip leftover main RenderView after stereo %dx%d",
                        setup.width, setup.height);
                    ++s_skipDup;
                }
                return;
            }
            static int s_auxLog;
            if (s_auxLog < 8)
            {
                Game::logMsg("Allow aux RenderView after stereo %dx%d origin=(%.1f,%.1f,%.1f) pitch=%.1f",
                    setup.width, setup.height, setup.origin.x, setup.origin.y, setup.origin.z,
                    setup.angles.x);
                ++s_auxLog;
            }
            callOriginal(setup, nClearFlags, whatToDraw);
            return;
        }
        m_VR->m_StereoEyesDrawnThisFrame = true;
        m_VR->BeginStereoFramePose();
        static int s_enterLog;
        if (s_enterLog < 4)
        {
            Game::logMsg("Stereo HMD-fb enter setup=%dx%d fov=%.1f zNear=%.1f",
                setup.width, setup.height, setup.fov, setup.zNear);
            ++s_enterLog;
        }
        CViewSetup leftEyeView = setup;
        CViewSetup rightEyeView = setup;
        NormalizeViewSetupForVREye(leftEyeView, m_VR);
        NormalizeViewSetupForVREye(rightEyeView, m_VR);
        applyL4d2VrHead(leftEyeView, true, true);
        applyL4d2VrHead(rightEyeView, true, false);
        const float ipd = m_VR->m_Ipd * m_VR->m_IpdScale * m_VR->m_VRScale;
        const int eyeW = static_cast<int>(m_VR->m_RenderWidth);
        const int eyeH = static_cast<int>(m_VR->m_RenderHeight);

        static int s_hmdFbOnce;
        if (s_hmdFbOnce == 0)
        {
            s_hmdFbOnce = 1;
            bmvr::BeginRisky(L"hmd_fb");
            if (bmvr::TryOffscreenHmd())
                bmvr::BeginRisky(L"hmd_offscreen");
            if (bmvr::TrySteamVrEyeRt())
                bmvr::BeginRisky(L"steamvr_rt");
            if (bmvr::UseGbMatchViewLock())
                bmvr::BeginRisky(L"fl_gbmatch");
            Game::logMsg("Stereo HMD-fb begin setup=%dx%d unscaled=%dx%d stereoEye=%d eye=%dx%d fov=%.1f->%.1f aspect=%.3f->%.3f zNear=%.1f ipd=%.2f L=(%.1f,%.1f,%.1f) R=(%.1f,%.1f,%.1f) steamvr_rt=%d offscreen=%d worldMatch=%d gbmatch=%d",
                setup.width, setup.height, setup.m_nUnscaledWidth, setup.m_nUnscaledHeight,
                setup.m_eStereoEye, eyeW, eyeH,
                setup.fov, leftEyeView.fov, setup.m_flAspectRatio, leftEyeView.m_flAspectRatio,
                leftEyeView.zNear, ipd,
                leftEyeView.origin.x, leftEyeView.origin.y, leftEyeView.origin.z,
                rightEyeView.origin.x, rightEyeView.origin.y, rightEyeView.origin.z,
                bmvr::TrySteamVrEyeRt() ? 1 : 0,
                bmvr::TryOffscreenHmd() ? 1 : 0,
                bmvr::OffscreenWorldMatchesEyes() ? 1 : 0,
                bmvr::UseGbMatchViewLock() ? 1 : 0);
        }

        m_VR->m_DirectEyeSubmit = true;
        m_VR->m_StereoRenderViewActive = false;
        m_VR->m_DesktopMirrorEnabled = false;

        static int s_eyeRvLog;
        if (s_eyeRvLog < 4)
        {
            Game::logMsg("Stereo HMD-fb left RenderView %dx%d",
                leftEyeView.width, leftEyeView.height);
            ++s_eyeRvLog;
        }
        m_VR->m_StereoBodyOrigin = setup.origin;
        m_VR->m_HasStereoBodyOrigin = true;
        StereoCostBegin();
        g_Cost.eye = 1;
        m_VR->m_StereoEye = 1;
        m_VR->BeginStereoEyeBlit(m_VR->m_D9LeftEyeSurface);
        {
            const int eyeDraw = whatToDraw & ~kRenderViewDrawHud;
            const long long t0 = QpcNow();
            callOriginal(leftEyeView, nClearFlags, eyeDraw);
            g_Cost.leftTicks += QpcNow() - t0;
        }
        const bool leftUnbind = m_VR->EndStereoEyeBlit();
        {
            (void)leftUnbind;
            // Native: LDR backbuffer bind was redirected onto the eye (G-buffer
            // and FullFrame match). Do not StretchRect the 2560 window over it.
            // Unbind of A2R10 FullFrame still whites LDR eyes (ff_hmdfit).
            const bool keepNative = bmvr::OffscreenWorldMatchesEyes()
                && m_VR->StereoRedirectedToEye();
            if (!keepNative)
            {
                const long long t0 = QpcNow();
                const bool leftBb = m_VR->BlitHmdViewFromBackbuffer(
                    m_VR->m_D9LeftEyeSurface, bmvr::g_StereoBlitGpuFlush);
                if (!leftBb)
                    m_VR->BlitCurrentGameColorTo(
                        m_VR->m_D9LeftEyeSurface, true);
                g_Cost.blitTicks += QpcNow() - t0;
            }
            if (bmvr::g_VrHandsGlovesEnabled || bmvr::g_VrHandsDebugBoxes || bmvr::g_HandHud
                || m_VR->WeaponMenuOpen())
            {
                const long long t0 = QpcNow();
                m_VR->DrawIndependentHandMarkers(m_VR->ColorTargetForStereoEye(1), 1);
                g_Cost.handsTicks += QpcNow() - t0;
            }
        }
        if (s_eyeRvLog < 8)
        {
            Game::logMsg("Stereo HMD-fb right RenderView %dx%d",
                rightEyeView.width, rightEyeView.height);
            ++s_eyeRvLog;
        }
        g_Cost.eye = 2;
        g_Cost.eye = 2;
        m_VR->m_StereoEye = 2;
        m_VR->BeginStereoEyeBlit(m_VR->m_D9RightEyeSurface);
        {
            const int eyeDraw = whatToDraw & ~kRenderViewDrawHud;
            const long long t0 = QpcNow();
            callOriginal(rightEyeView, nClearFlags, eyeDraw);
            g_Cost.rightTicks += QpcNow() - t0;
        }
        const bool rightUnbind = m_VR->EndStereoEyeBlit();
        {
            (void)rightUnbind;
            const bool keepNative = bmvr::OffscreenWorldMatchesEyes()
                && m_VR->StereoRedirectedToEye();
            if (!keepNative)
            {
                const long long t0 = QpcNow();
                const bool rightBb = m_VR->BlitHmdViewFromBackbuffer(
                    m_VR->m_D9RightEyeSurface, false);
                if (!rightBb)
                    m_VR->BlitCurrentGameColorTo(m_VR->m_D9RightEyeSurface, false);
                g_Cost.blitTicks += QpcNow() - t0;
            }
            if (bmvr::g_VrHandsGlovesEnabled || bmvr::g_VrHandsDebugBoxes || bmvr::g_HandHud
                || m_VR->WeaponMenuOpen())
            {
                const long long t0 = QpcNow();
                m_VR->DrawIndependentHandMarkers(m_VR->ColorTargetForStereoEye(2), 2);
                g_Cost.handsTicks += QpcNow() - t0;
            }
        }
        m_VR->m_StereoEye = 0;
        if (bmvr::OffscreenWorldMatchesEyes())
            m_VR->MirrorStereoToDesktopWindow();
        else if (m_VR->m_IsVREnabled)
            m_VR->ClearUnusedDesktopBackbuffer();
        // gbmatch already draws flashlight in the eye passes. A third window
        // DRAWHUD pass was a 15fps regression. Keep it only for the fused
        // fallback (eyes stripped HUD).
        const bool runDrawHudPass = !bmvr::TryFlashlightGbMatch() && bmvr::TryDrawHud();
        if (runDrawHudPass)
        {
            static int s_drawHudFrames;
            if (s_drawHudFrames == 0)
                bmvr::BeginRisky(L"drawhud");
            // Eyes stripped DRAWHUD. One window-sized pass fills _rt_gui /
            // _rt_Hud (client RenderView only — engine VGui_Paint cannot).
            // Keep engine width/height; do not NormalizeViewSetupForVREye.
            CViewSetup hudView = setup;
            applyL4d2VrHead(hudView, false, true);
            callOriginal(hudView, nClearFlags, whatToDraw | kRenderViewDrawHud);
            ++s_drawHudFrames;
            if (s_drawHudFrames == 120)
                bmvr::EndRisky(L"drawhud");
        }
        if (bmvr::g_VrHandsGlovesEnabled || bmvr::g_VrHandsDebugBoxes)
            m_VR->DrawIndependentHandsOnDesktop();
        m_VR->m_HasStereoBodyOrigin = false;
        m_VR->EndStereoFramePose();
        StereoCostLog();
        // Do not stretch eyes onto the backbuffer. That overwrote the
        // engine's 1584 tram strip with a black A2R10 copy (2026-08-18).

        m_VR->m_RenderedNewFrame.store(true, std::memory_order_release);

        static int s_hmdFbDone;
        if (s_hmdFbDone < 4)
        {
            Game::logMsg("Stereo HMD-fb pair done %dx%d redirected=%d worldMatch=%d",
                eyeW, eyeH, m_VR->StereoRedirectedToEye() ? 1 : 0,
                bmvr::OffscreenWorldMatchesEyes() ? 1 : 0);
            if (bmvr::OffscreenWorldMatchesEyes())
                Game::logMsg("offscreen native: FullFrame/G-buffer match eyes %dx%d", eyeW, eyeH);
            ++s_hmdFbDone;
        }

        static int s_hmdFbFrames;
        ++s_hmdFbFrames;
        if (s_hmdFbFrames >= 120)
        {
            // >= not == : live SS / later BeginRisky must not leave flags
            // after the first successful 120 frames (installer false-ban).
            bmvr::EndRisky(L"hmd_fb");
            if (bmvr::TryOffscreenHmd())
                bmvr::EndRisky(L"hmd_offscreen");
            if (bmvr::TrySteamVrEyeRt())
                bmvr::EndRisky(L"steamvr_rt");
            if (bmvr::TryFullFrameStereo())
                bmvr::EndRisky(L"ff_stereo");
            if (bmvr::TryHmdFitFullFrame())
                bmvr::EndRisky(L"ff_hmdfit");
            if (bmvr::TryEyeFitWorldRts())
                bmvr::EndRisky(L"ff_gbfit");
            if (bmvr::TryFlashlightGbMatch())
                bmvr::EndRisky(L"fl_gbmatch");
            if (bmvr::TryGbLeftSkip())
                bmvr::EndRisky(L"gb_leftskip");
        }
        return;
    }

    passThrough();
}

bool __fastcall Hooks::dCreateMove(void* ecx, void* edx, float flInputSampleTime, CUserCmd* cmd)
{
    (void)edx;
    if (!hkCreateMove.fOriginal)
        return false;
    if (!cmd)
        return hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);

    auto applyControllerAim = [&]() {
        if (!m_VR || !m_VR->m_IsVREnabled || !cmd->command_number)
            return;
        if (!(m_VR->IsGameplayEligible() && EngineInGame() && m_VR->m_HmdPoseValid))
            return;
        // Falling back to the headset angle sends shots wherever the player is
        // looking, which is normally well above where the gun is pointed, and
        // it sticks for as long as the controller pose stays flagged invalid.
        // Hold the last good controller aim across dropouts instead, and only
        // use the headset before any controller aim has been seen.
        static bool s_haveLastAim = false;
        static QAngle s_lastAim{};
        if (m_VR->m_ControllerPoseValid)
        {
            const QAngle aim = m_VR->GetAimAngles();
            s_lastAim = aim;
            s_haveLastAim = true;
            cmd->viewangles.Init(aim.x, aim.y, 0.f);
        }
        else if (s_haveLastAim)
            cmd->viewangles.Init(s_lastAim.x, s_lastAim.y, 0.f);
        else
        {
            const Vector hmdVa = m_VR->GetViewAngle();
            cmd->viewangles.Init(hmdVa.x, hmdVa.y, 0.f);
        }
    };

    void* localPlayer = nullptr;
    bool mp5 = false;
    if (m_Game && m_Game->m_EngineClient && m_Game->m_ClientEntityList)
    {
        const int local = m_Game->m_EngineClient->GetLocalPlayer();
        if (local > 0)
            localPlayer = m_Game->m_ClientEntityList->GetClientEntity(local);
        mp5 = IsMp5Viewmodel(m_Game->GetActiveWeaponModelName());
        if (!mp5 && m_VR)
            mp5 = IsMp5Viewmodel(m_VR->m_LastViewmodelModel.c_str());
    }
    if (m_VR && m_VR->EmptyHands())
        cmd->buttons &= ~(IN_ATTACK | IN_ATTACK2);
    if (mp5)
        ZeroPlayerViewRecoil(localPlayer);
    // FireBullets / ItemPostFrame run inside original player CreateMove.
    // Aim, VR buttons, and weaponselect must be applied before that.
    applyControllerAim();

    auto applyVrUserCmd = [&]() {
        if (!m_VR || !m_VR->m_IsVREnabled)
            return;
        m_VR->FlushPendingWeaponSounds();
        m_VR->FlushPendingGameUi();
        EnsureServerFlashlightHook();
        EnsureWeaponShootOriginHooks();
        SnapLocalViewmodelForFire();

        if (!m_VR->m_ProcessInputEnabled)
            return;

        const uint32_t impulse = m_VR->m_PendingImpulse.load(std::memory_order_acquire);
        if (impulse)
        {
            cmd->impulse = static_cast<byte>(impulse);
            m_VR->m_PendingImpulse.store(0, std::memory_order_release);
            Game::logMsg("CreateMove applied impulse %u cmd=%d buttons=0x%x",
                impulse, cmd->command_number, cmd->buttons);
        }

        const float maxSpeed = 450.f;
        const float analogF = m_VR->m_WalkForward.load(std::memory_order_acquire) * maxSpeed;
        const float analogS = m_VR->m_WalkSide.load(std::memory_order_acquire) * maxSpeed;
        if (m_VR->m_ControllerPoseValid && m_VR->m_HmdPoseValid)
        {
            const Vector hmdVa = m_VR->GetViewAngle();
            const QAngle aim = m_VR->GetRightControllerAbsAngle();
            Vector hmdF, hmdR, hmdU, cF, cR, cU;
            QAngle::AngleVectors(QAngle(0.f, hmdVa.y, 0.f), &hmdF, &hmdR, &hmdU);
            QAngle::AngleVectors(QAngle(0.f, aim.y, 0.f), &cF, &cR, &cU);
            const Vector wish = hmdF * analogF + hmdR * analogS;
            cmd->forwardmove += DotProduct2D(wish, cF);
            cmd->sidemove += DotProduct2D(wish, cR);
        }
        else
        {
            cmd->forwardmove += analogF;
            cmd->sidemove += analogS;
        }
        cmd->buttons |= static_cast<int>(m_VR->HeldButtons());
        const int inv = m_VR->m_PendingInvDelta.exchange(0, std::memory_order_acq_rel);
        if (inv != 0 && m_Game)
        {
            const int weap = m_Game->CycleWeaponSelect(inv > 0 ? 1 : -1);
            if (weap > 0)
            {
                cmd->weaponselect = weap;
                Game::logMsg("Weapon cycle via CUserCmd weaponselect=%d dir=%d", weap, inv);
            }
            else
                Game::logMsg("Weapon cycle skipped (no other weapon) dir=%d", inv);
        }
        const int menuWeap = m_VR->m_PendingWeaponSelect.exchange(0, std::memory_order_acq_rel);
        if (menuWeap > 0)
        {
            cmd->weaponselect = menuWeap;
            Game::logMsg("Weapon menu via CUserCmd weaponselect=%d", menuWeap);
        }
    };

    if (m_VR && m_VR->m_IsVREnabled && cmd->command_number)
        applyVrUserCmd();

    bool result = hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);

    if (m_VR && cmd->command_number && m_VR->IsGameplayEligible())
        m_VR->m_SeenGameplay = true;

    if (!m_VR || !m_VR->m_IsVREnabled || !cmd->command_number)
        return result;

    // Camera stays HMD on RenderView copies. Shooting uses controller
    // viewangles. Do not hook EyePosition (that would move the camera).
    applyControllerAim();
    if (mp5)
        ZeroPlayerViewRecoil(localPlayer);
    if (m_VR && m_VR->EmptyHands())
        cmd->buttons &= ~(IN_ATTACK | IN_ATTACK2);
    else
        cmd->buttons |= static_cast<int>(m_VR->HeldButtons());

    EnsureServerFlashlightHook();
    EnsureWeaponShootOriginHooks();

    m_VR->AfterCreateMoveFireHaptics();
    return result;
}

void __fastcall Hooks::dLevelInit(void* ecx, void* edx, const char* newmap)
{
    (void)edx;
    EnsureServerFlashlightHook();
    EnsureClientFlashlightHook();
    EnsureWeaponShootOriginHooks();
    if (m_VR)
    {
        m_VR->OnLevelInit(newmap);
        m_VR->ApplyRenderTargetFramebufferOverride();
    }
    if (hkLevelInit.fOriginal)
        hkLevelInit.fOriginal(ecx, newmap);
}

void __fastcall Hooks::dLevelShutdown(void* ecx, void* edx)
{
    (void)edx;
    if (m_VR)
        m_VR->OnLevelShutdown();
    if (hkLevelShutdown.fOriginal)
        hkLevelShutdown.fOriginal(ecx);
}

void __fastcall Hooks::dCalcViewModelView(void* ecx, void* edx, void* owner, const Vector& eyePosition, const QAngle& eyeAngles)
{
    (void)edx;
    if (!hkCalcViewModelView.fOriginal)
        return;
    if (m_VR && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible() && EngineInGame() && m_VR->m_HmdPoseValid)
    {
        // Uncoupled viewmodel: same world pose both eyes (sd805 / Portal 2).
        // Do not IPD the gun — that drew two weapons (2026-08-17).
        if (m_VR->m_ControllerPoseValid)
        {
            if (m_Game)
            {
                const char* weaponModel = m_Game->GetActiveWeaponModelName();
                if (weaponModel)
                    m_VR->NoteViewmodelModel(weaponModel);
            }
            // L4D2VR: feed the aim-controller pose as CalcViewModelView eye so
            // lag/bob and $origin are in the same frame as the hard-lock.
            const Vector targetOrigin = m_VR->GetRecommendedViewmodelAbsPos(eyePosition);
            const QAngle targetAng = m_VR->GetRecommendedViewmodelAbsAngle();
            // Crowbar idle-force runs in SuppressViewmodelMovementAnims.
            // MP5 fire sequences must keep playing (never idle-force those).
            if (m_Game)
            {
                const char* weaponModel = m_Game->GetActiveWeaponModelName();
                if (IsMp5Viewmodel(weaponModel)
                    || (m_VR && IsMp5Viewmodel(m_VR->m_LastViewmodelModel.c_str())))
                    ZeroPlayerViewRecoil(owner);
            }
            SuppressViewmodelMovementAnims(ecx);
            CallCalcViewModelViewOriginal(ecx, owner, targetOrigin, targetAng);
            const float origin3[3] = { targetOrigin.x, targetOrigin.y, targetOrigin.z };
            const float angles3[3] = { targetAng.x, targetAng.y, targetAng.z };
            CallSetAbsOriginAngles(ecx, origin3, angles3);
            if (m_Game)
            {
                const char* weaponModel = m_Game->GetActiveWeaponModelName();
                if (IsMp5Viewmodel(weaponModel)
                    || (m_VR && IsMp5Viewmodel(m_VR->m_LastViewmodelModel.c_str())))
                    ZeroPlayerViewRecoil(owner);
            }
            SuppressViewmodelMovementAnims(ecx);
            return;
        }
        Vector origin = m_VR->GetViewOrigin(
            m_VR->m_HasStereoBodyOrigin ? m_VR->m_StereoBodyOrigin : eyePosition);
        const Vector va = m_VR->GetViewAngle();
        QAngle ang(va.x, va.y, va.z);
        hkCalcViewModelView.fOriginal(ecx, owner, origin, ang);
        return;
    }
    hkCalcViewModelView.fOriginal(ecx, owner, eyePosition, eyeAngles);
}

float __fastcall Hooks::dGetViewModelFOV(void* ecx, void* edx)
{
    (void)edx;
    if (m_VR && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible() && EngineInGame() && m_VR->m_Fov > 10.f)
        return m_VR->m_Fov;
    if (hkGetViewModelFOV.fOriginal)
        return hkGetViewModelFOV.fOriginal(ecx);
    return 54.f;
}

void __fastcall Hooks::dAdjustEngineViewport(void* ecx, void* edx, int& x, int& y, int& width, int& height)
{
    if (hkAdjustEngineViewport.fOriginal)
        hkAdjustEngineViewport.fOriginal(ecx, x, y, width, height);
    ClampStereoViewport(x, y, width, height);
}

void __fastcall Hooks::dViewport(void* ecx, void* edx, int x, int y, int width, int height)
{
    NoteMatContext(ecx);
    ClampStereoViewport(x, y, width, height);
    if (hkViewport.fOriginal)
        hkViewport.fOriginal(ecx, x, y, width, height);
}

void __fastcall Hooks::dGetViewport(void* ecx, void* edx, int& x, int& y, int& width, int& height)
{
    NoteMatContext(ecx);
    if (hkGetViewport.fOriginal)
        hkGetViewport.fOriginal(ecx, x, y, width, height);
    ClampStereoViewport(x, y, width, height);
}

void __fastcall Hooks::dDrawModelExecute(void* ecx, void* edx, void* state, const ModelRenderInfo_t& info, void* pCustomBoneToWorld)
{
    (void)edx;
    const long long dmeEnter = g_Cost.active ? QpcNow() : 0;
    char modelNameBuf[260]{};
    bool hideArms = false;
    bool skipLocal = false;
    bool isViewmodel = false;
    const char* modelName = nullptr;
    int body = 0;
    int skin = 0;
    int entityIndex = 0;
    void* pModel = nullptr;
    __try
    {
        pModel = info.pModel;
        entityIndex = info.entity_index;
        body = info.body;
        skin = info.skin;
        if (bmvr::g_HideLocalPlayerModel
            && m_VR
            && m_VR->IsGameplayEligible())
        {
            const int local = CachedLocalPlayerIndex();
            if (local > 0 && entityIndex == local)
                skipLocal = true;
        }
        // World props: skip GetModelName. Open maps call DME thousands of times
        // per eye; the name string was the hot cost next to the draw itself.
        isViewmodel = CachedModelIsViewmodel(pModel);
        if (isViewmodel && m_Game && m_Game->m_ModelInfo)
        {
            modelName = SafeModelName(m_Game->m_ModelInfo, pModel);
            if (modelName && modelName[0])
            {
                strncpy_s(modelNameBuf, modelName, _TRUNCATE);
                modelName = modelNameBuf;
            }
        }
        static int s_dmeEnter;
        if (s_dmeEnter < 6)
        {
            Game::logMsg("DME %s viewmodel=%d eligible=%d body=%d skin=%d ent=%d bones=%d",
                modelName ? modelName : "?", isViewmodel ? 1 : 0,
                (m_VR && m_VR->IsGameplayEligible()) ? 1 : 0,
                body, skin, entityIndex, pCustomBoneToWorld ? 1 : 0);
            ++s_dmeEnter;
        }
        hideArms = bmvr::g_HideViewmodelArms
            && isViewmodel
            && m_VR
            && m_VR->m_IsVREnabled
            && m_VR->IsGameplayEligible()
            && EngineInGame();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        hideArms = false;
        skipLocal = false;
        isViewmodel = false;
        Game::logMsg("DME info SEH - passthrough original");
    }

    if (g_Cost.active)
    {
        ++g_Cost.dme;
        if (g_Cost.eye == 1)
            ++g_Cost.dmeL;
        else if (g_Cost.eye == 2)
            ++g_Cost.dmeR;
        if (AuxSceneRtBound())
            ++g_Cost.dmeShadow;
        if (isViewmodel)
            ++g_Cost.dmeVm;
        if (entityIndex > 1)
        {
            ++g_Cost.dmeEnt;
            CostMarkEnt(entityIndex);
        }
        if (state)
        {
            const auto* lite = reinterpret_cast<const DrawModelStateLite*>(state);
            const unsigned char* hdr = lite->studioHdr;
            if (hdr)
            {
                const int nb = *reinterpret_cast<const int*>(hdr + kStudioHdrNumBones);
                if (nb >= 32 && nb <= kMaxStudioBones)
                {
                    ++g_Cost.dmeBones32;
                    if (pModel)
                        g_Cost.lastCharModel = pModel;
                }
            }
        }
        g_Cost.dmeHookTicks += QpcNow() - dmeEnter;
    }

    if (skipLocal)
    {
        static int s_hideLog;
        if (s_hideLog < 4)
        {
            Game::logMsg("HideLocalPlayerModel skip %s", modelName ? modelName : "?");
            ++s_hideLog;
        }
        return;
    }

    if (isViewmodel && m_VR && m_VR->EmptyHands() && m_VR->IsGameplayEligible() && EngineInGame())
        return;

    if (isViewmodel && m_VR && m_VR->IsGameplayEligible() && EngineInGame())
        ApplyViewmodelStudioWork(state, modelName, body, skin, hideArms, pCustomBoneToWorld);

    void* bonesToDraw = pCustomBoneToWorld;
    const ModelRenderInfo_t* infoToDraw = &info;
    if (isViewmodel && m_VR && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible()
        && EngineInGame() && m_VR->m_ControllerPoseValid)
    {
        const float scale = [&]() -> float {
            float s = bmvr::g_ViewmodelScale;
            if (modelName && (std::strstr(modelName, "crowbar") || std::strstr(modelName, "wrench")
                || std::strstr(modelName, "Crowbar") || std::strstr(modelName, "Wrench")))
                s = 1.f;
            if (modelName && (std::strstr(modelName, "shotgun") || std::strstr(modelName, "spas")
                || std::strstr(modelName, "pump")))
                s = 0.64f;
            if (modelName && (std::strstr(modelName, "grenade") || std::strstr(modelName, "Grenade")
                || std::strstr(modelName, "frag") || std::strstr(modelName, "Frag")))
                s *= 1.25f;
            if (modelName && (std::strstr(modelName, "357") || std::strstr(modelName, "python")
                || std::strstr(modelName, "revolver") || std::strstr(modelName, "Revolver")))
                s *= 1.15f;
            if (s < 0.2f)
                s = 0.2f;
            if (s > 1.5f)
                s = 1.5f;
            return s;
        }();
        float yFix = 1.f;
        bool eyePass = m_VR->m_StereoEye != 0 || m_VR->StereoEyeBlitActive();
        if (!eyePass && g_MatCtx && hkGetViewport.fOriginal)
        {
            int vx = 0, vy = 0, vw = 0, vh = 0;
            hkGetViewport.fOriginal(g_MatCtx, vx, vy, vw, vh);
            if (vw >= 640 && vh >= 360 && m_VR->m_RenderWidth >= 640 && m_VR->m_RenderHeight >= 360)
            {
                const float vpAspect = static_cast<float>(vw) / static_cast<float>(vh);
                const float eyeAspect = static_cast<float>(m_VR->m_RenderWidth)
                    / static_cast<float>(m_VR->m_RenderHeight);
                if (fabsf(vpAspect - eyeAspect) < 0.12f)
                    eyePass = true;
            }
        }
        if (eyePass && m_VR->m_RenderWidth >= 640 && m_VR->m_RenderHeight >= 360)
        {
            uint32_t winW = 0, winH = 0;
            bmvr::QueryWindowClientSize(winW, winH);
            if (winW < 640 || winH < 360)
            {
                winW = 16;
                winH = 9;
            }
            const float winAspect = static_cast<float>(winW) / static_cast<float>(winH);
            const float eyeAspect = static_cast<float>(m_VR->m_RenderWidth)
                / static_cast<float>(m_VR->m_RenderHeight);
            if (winAspect > 0.5f && eyeAspect > 0.5f)
                yFix = eyeAspect / winAspect;
        }
        const bool needScale = scale > 0.2f && scale < 1.5f && fabsf(scale - 1.f) > 0.001f;
        const bool needYFix = yFix > 0.2f && yFix < 2.f && fabsf(yFix - 1.f) > 0.03f;
        {
            const int slot = g_VmDrawSlot++ & (kVmDrawSlots - 1);
            Vector pivot = info.origin;
            const Vector body = m_VR->m_HasStereoBodyOrigin ? m_VR->m_StereoBodyOrigin : m_VR->m_SetupOrigin;
            if (body.LengthSqr() > 1.f)
                pivot = m_VR->GetRightControllerAbsPos(body);
            const float pivot3[3] = { pivot.x, pivot.y, pivot.z };

            const Vector targetOrigin = m_VR->GetRecommendedViewmodelAbsPos(
                body.LengthSqr() > 1.f ? body : info.origin);
            const QAngle targetAng = m_VR->GetRecommendedViewmodelAbsAngle();
            float rigidDelta[3][4]{};
            const float* rigidPtr = nullptr;
            if (info.origin.LengthSqr() > 1.f)
            {
                float origEntity[3][4]{};
                float origInv[3][4]{};
                float targetEntity[3][4]{};
                BuildMatrix3x4FromOrgAngles(origEntity, info.origin, info.angles);
                InvertMatrix3x4TR(origEntity, origInv);
                BuildMatrix3x4FromOrgAngles(targetEntity, targetOrigin, targetAng);
                MulMatrix3x4(targetEntity, origInv, rigidDelta);
                rigidPtr = &rigidDelta[0][0];
            }

            // Same world mesh both eyes. Per-eye unstretch (IPD) drew two guns.
            const Vector cam = m_VR->GetViewOrigin(body.LengthSqr() > 1.f ? body : info.origin);
            Vector fwd, right, up;
            m_VR->GetViewBasis(&fwd, &right, &up);
            const float cam3[3] = { cam.x, cam.y, cam.z };
            const float right3[3] = { right.x, right.y, right.z };
            const float up3[3] = { up.x, up.y, up.z };
            const float fwd3[3] = { fwd.x, fwd.y, fwd.z };

            unsigned char* hdr = ResolveStudioHdr(state);
            const int nBones = StudioHdrNumBones(hdr);
            bool usedBones = false;
            if (pCustomBoneToWorld && nBones > 0
                && SehCopyAndFixViewmodelMatrices(&g_ScaledViewmodelBones[slot][0][0][0], pCustomBoneToWorld,
                    nBones, pivot3, needScale ? scale : 1.f, cam3, right3, up3, fwd3,
                    needYFix ? yFix : 1.f, rigidPtr))
            {
                bonesToDraw = g_ScaledViewmodelBones[slot];
                usedBones = true;
            }
            else if (needScale)
                SehWriteModelScale(info.pRenderable, scale);

            std::memcpy(g_ScaledViewmodelInfo[slot], &info, sizeof(ModelRenderInfo_t));
            auto* scaledInfo = reinterpret_cast<ModelRenderInfo_t*>(g_ScaledViewmodelInfo[slot]);
            scaledInfo->origin = targetOrigin;
            scaledInfo->angles = targetAng;
            float origin3[3] = { targetOrigin.x, targetOrigin.y, targetOrigin.z };
            if (needYFix)
                UnstretchPointViewY(origin3, cam3, right3, up3, fwd3, yFix);
            if (needScale)
            {
                origin3[0] = pivot.x + (origin3[0] - pivot.x) * scale;
                origin3[1] = pivot.y + (origin3[1] - pivot.y) * scale;
                origin3[2] = pivot.z + (origin3[2] - pivot.z) * scale;
            }
            scaledInfo->origin.x = origin3[0];
            scaledInfo->origin.y = origin3[1];
            scaledInfo->origin.z = origin3[2];
            if (info.pModelToWorld
                && SehCopyAndFixViewmodelMatrices(&g_ScaledViewmodelModelToWorld[slot][0][0], info.pModelToWorld,
                    1, pivot3, needScale ? scale : 1.f, cam3, right3, up3, fwd3,
                    needYFix ? yFix : 1.f, rigidPtr))
            {
                scaledInfo->pModelToWorld = reinterpret_cast<const matrix3x4_t*>(g_ScaledViewmodelModelToWorld[slot]);
            }
            infoToDraw = scaledInfo;
            static int s_scaleLog;
            if (s_scaleLog < 8)
            {
                Game::logMsg("Viewmodel DME scale=%.2f yFix=%.3f eyePass=%d rigid=%d bones=%d usedBones=%d origin=(%.1f,%.1f,%.1f)->(%.1f,%.1f,%.1f)",
                    scale, yFix, eyePass ? 1 : 0, rigidPtr ? 1 : 0, nBones, usedBones ? 1 : 0,
                    info.origin.x, info.origin.y, info.origin.z,
                    scaledInfo->origin.x, scaledInfo->origin.y, scaledInfo->origin.z);
                ++s_scaleLog;
            }
        }
    }

    if (hkDrawModelExecute.fOriginal)
    {
        __try
        {
            const long long t0 = g_Cost.active ? QpcNow() : 0;
            hkDrawModelExecute.fOriginal(ecx, state, *infoToDraw, bonesToDraw);
            if (g_Cost.active)
                g_Cost.dmeOrigTicks += QpcNow() - t0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Game::logMsg("DME original SEH %s", modelName ? modelName : "?");
        }
    }

    // Studio draw is queued after this returns (NO_DRAW restore-after-original
    // left HEV arms visible 2026-08-19). Keep arms nummeshes=0 until map end.
}

void Hooks::RestoreViewmodelArmHides()
{
    RestoreArmBodypartMeshes();
}

void __fastcall Hooks::dEndFrame(void* ecx, void* edx)
{
    (void)edx;
    if (hkEndFrame.fOriginal)
        hkEndFrame.fOriginal(ecx);
    if (!m_VR)
        return;
    m_VR->m_PresentSpikeEndFrameThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (m_Game && m_Game->GetMatQueueMode() != 0)
    {
        const uint32_t queued = m_VR->m_SourceRenderQueueMarkerQueuedId.load(std::memory_order_acquire);
        m_VR->m_SourceRenderQueueMarkerCompletedId.store(queued, std::memory_order_release);
    }
}

void __fastcall Hooks::dPushRenderTargetAndViewport(void* ecx, void* edx, ITexture* pTexture, ITexture* pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH)
{
    NoteMatContext(ecx);
    const char* pushName = SafeTextureName(pTexture);
    NotePushRt(pushName, nViewW, nViewH);
    if (m_VR && m_VR->IsGameplayEligible())
    {
        static int s_pushNames;
        if (s_pushNames < 24)
        {
            Game::logMsg("PushRT name=%s %dx%d aux=%d",
                pushName, nViewW, nViewH, AuxSceneRtBound() ? 1 : 0);
            ++s_pushNames;
        }
        if (pushName && (std::strstr(pushName, "flashlight") || std::strstr(pushName, "Flashlight")
                || std::strstr(pushName, "FlashLight"))
            && (m_VR->StereoEyeBlitActive() || m_VR->m_StereoEye != 0))
        {
            static int s_flPush;
            if (s_flPush < 12)
            {
                Game::logMsg("Flashlight PushRT inside eye RV %s %dx%d stereoEye=%d",
                    pushName, nViewW, nViewH, m_VR->m_StereoEye);
                ++s_flPush;
            }
        }
    }
    if (m_VR && TextureNameIsHudRt(pushName))
        m_VR->NoteEngineHudRtPush(pushName, nViewW, nViewH);
    // Native offscreen: ViewDrawScene still PushRT's HMD-sized G-buffers at
    // HWND 2560x1440. Do not trust GetActualWidth here — it can report the
    // videomode (1440) after LITERAL grew the GPU texture to 2480.
    if (m_VR && bmvr::OffscreenWorldMatchesEyes()
        && (m_VR->StereoEyeBlitActive() || m_VR->m_StereoEye != 0)
        && !AuxSceneRtBound() && !m_VR->HudPaintActive())
    {
        const int eyeW = static_cast<int>(m_VR->m_RenderWidth);
        const int eyeH = static_cast<int>(m_VR->m_RenderHeight);
        const bool namedWorld = IsOffscreenWorldRtName(pushName);
        const bool backbuffer = (pTexture == nullptr)
            || (pushName && (std::strcmp(pushName, "null") == 0));
        if (eyeW >= 640 && eyeH >= 360 && (namedWorld || backbuffer)
            && (nViewW != eyeW || nViewH != eyeH || nViewW <= 0 || nViewH <= 0))
        {
            static int s_offVp;
            if (s_offVp < 12)
            {
                Game::logMsg("PushRT %s viewport %dx%d -> eye %dx%d (offscreen native)",
                    pushName, nViewW, nViewH, eyeW, eyeH);
                ++s_offVp;
            }
            nViewX = 0;
            nViewY = 0;
            nViewW = eyeW;
            nViewH = eyeH;
        }
    }
    if (g_StereoRedirect)
    {
        bool redir = false;
        const char* before = SafeTextureName(pTexture);
        const int beforeW = nViewW;
        const int beforeH = nViewH;
        BindStereoPushToEye(pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH, redir);
        static int s_pushLog;
        if (s_pushLog < 20)
        {
            Game::logMsg("Stereo PushRT %s %dx%d redirect=%d -> %s %dx%d",
                before, beforeW, beforeH, redir ? 1 : 0,
                SafeTextureName(pTexture), nViewW, nViewH);
            ++s_pushLog;
        }
    }
    if (hkPushRenderTargetAndViewport.fOriginal)
        hkPushRenderTargetAndViewport.fOriginal(ecx, pTexture, pDepthTexture, nViewX, nViewY, nViewW, nViewH);
}

void __fastcall Hooks::dPopRenderTargetAndViewport(void* ecx, void* edx)
{
    if (m_VR && m_VR->EngineHudRtPushed())
        m_VR->BlitEngineHudRtToOverlay();
    if (hkPopRenderTargetAndViewport.fOriginal)
        hkPopRenderTargetAndViewport.fOriginal(ecx);
    NotePopRt();
}

void __fastcall Hooks::dVGui_Paint(void* ecx, void* edx, int mode)
{
    (void)edx;
    if (g_VguiOverlayReentry)
    {
        if (hkVgui_Paint.fOriginal)
            hkVgui_Paint.fOriginal(ecx, mode);
        return;
    }

    const bool inGame = EngineInGame();
    const bool eligible = m_VR && m_VR->IsGameplayEligible();

    // Never steal the engine destination. Last steal emptied _rt_gui.
    // Menu/background stay original only. In-game: original (desktop) plus
    // one extra paint into bmvrHUD (headset overlay).
    static int s_paintLog;
    static int s_ingamePaintLog;
    if (s_paintLog < 8)
    {
        Game::logMsg("VGui_Paint original dest inGame=%d eligible=%d mode=0x%X overlay=%d",
            inGame ? 1 : 0, eligible ? 1 : 0, mode,
            (m_VR && m_VR->HudOverlayReady()) ? 1 : 0);
        ++s_paintLog;
    }
    else if (eligible && s_ingamePaintLog < 4)
    {
        Game::logMsg("VGui_Paint in-game original mode=0x%X overlay=%d",
            mode, (m_VR && m_VR->HudOverlayReady()) ? 1 : 0);
        ++s_ingamePaintLog;
    }
    if (hkVgui_Paint.fOriginal)
    {
        if (m_VR)
            m_VR->SetVguiPaintActive(true);
        hkVgui_Paint.fOriginal(ecx, mode);
        if (m_VR)
            m_VR->SetVguiPaintActive(false);
    }
    if (inGame && eligible && m_VR && m_VR->HudOverlayReady() && m_VR->PauseUiActive())
        PaintVguiToOverlay(ecx, mode);
}

void __fastcall Hooks::dTraceRay(void* ecx, void* edx, const Ray_t& ray, unsigned int fMask, CTraceFilter* filter, CGameTrace* pTrace)
{
    (void)edx;
    // 47777b5 / L4D2VR melee: do not rewrite TraceRay. Hit geometry is the
    // client Rodrigues fan from viewmodel abs origin in UpdateCrowbarMelee.
    // Invented pitch clamps + origin rewrite were the ceiling-hit path.
    if (hkTraceRay.fOriginal)
        hkTraceRay.fOriginal(ecx, ray, fMask, filter, pTrace);
}

void Hooks::EnsureServerFlashlightHook()
{
    if (hkImpulseCommands.pTarget)
        return;
    HMODULE server = GetModuleHandleA("server.dll");
    if (!server)
        return;
    auto* p = reinterpret_cast<unsigned char*>(server) + Offsets::kCBasePlayer_ImpulseCommands;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
    {
        static int s_nq;
        if (s_nq < 2)
        {
            Game::logMsg("ImpulseCommands RVA 0x%X not executable", Offsets::kCBasePlayer_ImpulseCommands);
            ++s_nq;
        }
        return;
    }
    bool hasImpulseField = false;
    __try
    {
        for (int i = 0; i < 120; ++i)
        {
            if (p[i] == 0x44 && p[i + 1] == 0x0E && p[i + 2] == 0x00 && p[i + 3] == 0x00)
            {
                hasImpulseField = true;
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        hasImpulseField = false;
    }
    if (!hasImpulseField)
    {
        Game::logMsg("ImpulseCommands skipped (no m_nImpulse +0xE44 near 0x%X)",
            Offsets::kCBasePlayer_ImpulseCommands);
        hkImpulseCommands.pTarget = p;
        return;
    }
    if (hkImpulseCommands.createHook(p, &dImpulseCommands) != 0
        || hkImpulseCommands.enableHook() != 0)
    {
        Game::logMsg("ImpulseCommands hook failed rva=0x%X", Offsets::kCBasePlayer_ImpulseCommands);
        return;
    }
    Game::logMsg("Hook enabled: ImpulseCommands rva=0x%X (passthrough; CreateMove impulse 100)",
        Offsets::kCBasePlayer_ImpulseCommands);
}

void __fastcall Hooks::dImpulseCommands(void* ecx, void* edx)
{
    (void)edx;
    // Working flashlight (47777b5) was CreateMove cmd->impulse=100 only.
    // Intercepting impulse 100 here cleared it and called EF_DIMLIGHT
    // virtuals that suit/gamerules may no-op — vanilla never saw the impulse.
    // Pass through; do not add flashlight cvars.
    if (hkImpulseCommands.fOriginal)
        hkImpulseCommands.fOriginal(ecx);
}

void Hooks::EnsureClientFlashlightHook()
{
    if (hkUpdateFlashlightState.pTarget)
        return;
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client)
        return;
    auto* p = reinterpret_cast<unsigned char*>(client) + Offsets::kUpdateFlashlightState;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
    {
        Game::logMsg("UpdateFlashlightState RVA 0x%X not executable", Offsets::kUpdateFlashlightState);
        return;
    }
    if (hkUpdateFlashlightState.createHook(p, &dUpdateFlashlightState) != 0
        || hkUpdateFlashlightState.enableHook() != 0)
    {
        Game::logMsg("UpdateFlashlightState hook failed rva=0x%X", Offsets::kUpdateFlashlightState);
        return;
    }
    Game::logMsg("Hook enabled: UpdateFlashlightState rva=0x%X (HMD origin/forward for VR eyes)",
        Offsets::kUpdateFlashlightState);
}

void __fastcall Hooks::dUpdateFlashlightState(void* ecx, void* edx, void* flashlightState)
{
    (void)edx;
    // Desktop beam works; HMD missed it because FlashlightState_t is filled
    // from player EyePosition/EyeAngles (body) while stereo cameras use the
    // HMD (research/resolution-hud-flashlight.md §8). Retarget to HMD view.
    if (flashlightState && m_VR && m_VR->m_IsVREnabled && m_VR->IsGameplayEligible()
        && m_VR->m_HmdPoseValid && EngineInGame())
    {
        __try
        {
            auto* f = reinterpret_cast<float*>(flashlightState);
            const Vector body = m_VR->m_HasStereoBodyOrigin ? m_VR->m_StereoBodyOrigin : m_VR->m_SetupOrigin;
            const Vector origin = m_VR->GetViewOrigin(body.LengthSqr() > 1.f ? body : m_VR->m_SetupOrigin);
            Vector fwd, right, up;
            m_VR->GetViewBasis(&fwd, &right, &up);
            if (VectorNormalize(fwd) > 0.001f && origin.LengthSqr() > 1.f)
            {
                f[0] = origin.x;
                f[1] = origin.y;
                f[2] = origin.z;
                f[3] = fwd.x;
                f[4] = fwd.y;
                f[5] = fwd.z;
                static int s_flLog;
                if (s_flLog < 6)
                {
                    Game::logMsg("FlashlightState -> HMD origin=(%.1f,%.1f,%.1f) fwd=(%.2f,%.2f,%.2f)",
                        origin.x, origin.y, origin.z, fwd.x, fwd.y, fwd.z);
                    ++s_flLog;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    if (hkUpdateFlashlightState.fOriginal)
        hkUpdateFlashlightState.fOriginal(ecx, flashlightState);
}

namespace
{
    bool CodeBytesMatch(const unsigned char* p, const char* hex)
    {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        int i = 0;
        while (hex[0] && hex[1])
        {
            if (hex[0] == ' ')
            {
                ++hex;
                continue;
            }
            const int hi = nibble(hex[0]);
            const int lo = nibble(hex[1]);
            if (hi < 0 || lo < 0)
                return false;
            if (p[i] != static_cast<unsigned char>((hi << 4) | lo))
                return false;
            ++i;
            hex += 2;
        }
        return i > 0;
    }

    int ServerEntityIndex(void* ent)
    {
        if (!ent)
            return 0;
        int idx = 0;
        __try
        {
            unsigned char* net = reinterpret_cast<unsigned char*>(ent) + 8;
            void** vt = *reinterpret_cast<void***>(net);
            using Fn = int(__thiscall*)(void*);
            idx = reinterpret_cast<Fn>(vt[0x24 / 4])(net);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            idx = 0;
        }
        return idx;
    }

    bool ShouldRewriteShootOrigin(void* player, const Vector* current)
    {
        if (!Hooks::m_VR || !Hooks::m_VR->m_IsVREnabled || !Hooks::m_VR->m_ControllerPoseValid)
            return false;
        if (!Hooks::m_VR->IsGameplayEligible() || !EngineInGame())
            return false;
        if (!Hooks::m_Game)
            return false;
        C_BaseEntity* local = Hooks::m_Game->GetLocalPlayerEntity();
        if (local && player == local)
            return true;
        if (Hooks::m_Game->m_EngineClient)
        {
            const int want = Hooks::m_Game->m_EngineClient->GetLocalPlayer();
            const int have = ServerEntityIndex(player);
            if (want > 0 && have == want)
                return true;
        }
        if (current)
        {
            Vector body = Hooks::m_VR->m_HasStereoBodyOrigin ? Hooks::m_VR->m_StereoBodyOrigin : Hooks::m_VR->m_SetupOrigin;
            if (body.LengthSqr() > 1.f)
            {
                const Vector d = *current - body;
                if (d.LengthSqr() < 96.f * 96.f)
                    return true;
            }
        }
        return false;
    }

    Vector* RewriteShootOrigin(void* player, Vector* out, tWeaponShootPosition original)
    {
        if (original && out)
            original(player, out);
        else if (out)
        {
            out->x = 0.f;
            out->y = 0.f;
            out->z = 0.f;
        }
        if (!out)
            return out;
        if (!ShouldRewriteShootOrigin(player, out))
            return out;
        Vector muzzle{};
        if (!Hooks::m_VR->TryGetVrShootOrigin(muzzle))
            return out;
        static int s_log;
        if (s_log < 8)
        {
            Game::logMsg("Weapon_ShootPosition VR muzzle (%.1f,%.1f,%.1f) was (%.1f,%.1f,%.1f)",
                muzzle.x, muzzle.y, muzzle.z, out->x, out->y, out->z);
            ++s_log;
        }
        *out = muzzle;
        return out;
    }
}

void Hooks::EnsureWeaponShootOriginHooks()
{
    HMODULE client = GetModuleHandleA("client.dll");
    if (client && !hkClientWeaponShootPosition.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(client) + Offsets::kCBasePlayer_Weapon_ShootPosition;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "558BEC8B11FF7508FF9268020000"))
        {
            if (hkClientWeaponShootPosition.createHook(p, &dClientWeaponShootPosition) == 0
                && hkClientWeaponShootPosition.enableHook() == 0)
                Game::logMsg("Hook enabled: client Weapon_ShootPosition rva=0x%X", Offsets::kCBasePlayer_Weapon_ShootPosition);
            else
                Game::logMsg("client Weapon_ShootPosition hook failed");
        }
        else if (!hkClientWeaponShootPosition.pTarget)
        {
            Game::logMsg("client Weapon_ShootPosition skipped (bytes/rva 0x%X)", Offsets::kCBasePlayer_Weapon_ShootPosition);
            hkClientWeaponShootPosition.pTarget = p;
        }
    }
    if (client && !hkGetAttachmentVec.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(client) + Offsets::kCBaseAnimating_GetAttachmentVec;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "558BEC56578B7D088BF183FF01"))
        {
            if (hkGetAttachmentVec.createHook(p, &dGetAttachmentVec) == 0
                && hkGetAttachmentVec.enableHook() == 0)
                Game::logMsg("Hook enabled: GetAttachment vec rva=0x%X", Offsets::kCBaseAnimating_GetAttachmentVec);
            else
                Game::logMsg("GetAttachment vec hook failed");
        }
        else
        {
            Game::logMsg("GetAttachment vec skipped (bytes/rva 0x%X)", Offsets::kCBaseAnimating_GetAttachmentVec);
            hkGetAttachmentVec.pTarget = p;
        }
    }
    if (client && !hkGetAttachmentMatrix.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(client) + Offsets::kCBaseAnimating_GetAttachmentMatrix;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "558BEC568B7508578BF983FE01"))
        {
            if (hkGetAttachmentMatrix.createHook(p, &dGetAttachmentMatrix) == 0
                && hkGetAttachmentMatrix.enableHook() == 0)
                Game::logMsg("Hook enabled: GetAttachment matrix rva=0x%X", Offsets::kCBaseAnimating_GetAttachmentMatrix);
            else
                Game::logMsg("GetAttachment matrix hook failed");
        }
        else
        {
            Game::logMsg("GetAttachment matrix skipped (bytes/rva 0x%X)", Offsets::kCBaseAnimating_GetAttachmentMatrix);
            hkGetAttachmentMatrix.pTarget = p;
        }
    }

    HMODULE server = GetModuleHandleA("server.dll");
    if (server && !hkServerWeaponShootPosition.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(server) + Offsets::kCBasePlayer_Weapon_ShootPosition_Server;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "558BEC8B11FF7508FF9230020000"))
        {
            if (hkServerWeaponShootPosition.createHook(p, &dServerWeaponShootPosition) == 0
                && hkServerWeaponShootPosition.enableHook() == 0)
                Game::logMsg("Hook enabled: server Weapon_ShootPosition rva=0x%X", Offsets::kCBasePlayer_Weapon_ShootPosition_Server);
            else
                Game::logMsg("server Weapon_ShootPosition hook failed");
        }
        else
        {
            Game::logMsg("server Weapon_ShootPosition skipped (bytes/rva 0x%X)", Offsets::kCBasePlayer_Weapon_ShootPosition_Server);
            hkServerWeaponShootPosition.pTarget = p;
        }
    }
    if (server && !hkGetShootAngles.pTarget)
    {
        auto* p = reinterpret_cast<unsigned char*>(server) + Offsets::kBlackMesaPlayer_GetShootAngles_Server;
        MEMORY_BASIC_INFORMATION mbi{};
        // prologue, mov esi/ecx, call vtable+0x234 (EyeAngles), fetch the out arg
        if (VirtualQuery(p, &mbi, sizeof(mbi)) && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            && CodeBytesMatch(p, "558BEC568BF18B06FF90340200008B5508"))
        {
            if (hkGetShootAngles.createHook(p, &dGetShootAngles) == 0
                && hkGetShootAngles.enableHook() == 0)
                Game::logMsg("Hook enabled: server GetShootAngles rva=0x%X", Offsets::kBlackMesaPlayer_GetShootAngles_Server);
            else
                Game::logMsg("server GetShootAngles hook failed");
        }
        else
        {
            Game::logMsg("server GetShootAngles skipped (bytes/rva 0x%X)", Offsets::kBlackMesaPlayer_GetShootAngles_Server);
            hkGetShootAngles.pTarget = p;
        }
    }
}

void __fastcall Hooks::dGetShootAngles(void* ecx, void* edx, QAngle* out)
{
    (void)edx;
    if (!hkGetShootAngles.fOriginal)
        return;
    hkGetShootAngles.fOriginal(ecx, out);
    if (!ecx || !out || !bmvr::g_DisableRecoilAim)
        return;
    // The original returns EyeAngles + m_vecPunchAngle + m_recoilPunchAngles.
    // Subtract back exactly the recoil term it added, so bullets fly along the
    // aim instead of climbing as recoil accumulates over sustained fire. The
    // client keeps its own recoil state, so the gun still kicks on screen.
    __try
    {
        const float* recoil = reinterpret_cast<const float*>(
            static_cast<char*>(ecx) + Offsets::kBlackMesaPlayer_RecoilPunchAngles_Server);
        out->x -= recoil[0];
        out->y -= recoil[1];
        out->z -= recoil[2];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

Vector* __fastcall Hooks::dClientWeaponShootPosition(void* ecx, void* edx, Vector* out)
{
    (void)edx;
    return RewriteShootOrigin(ecx, out, hkClientWeaponShootPosition.fOriginal);
}

Vector* __fastcall Hooks::dServerWeaponShootPosition(void* ecx, void* edx, Vector* out)
{
    (void)edx;
    return RewriteShootOrigin(ecx, out, hkServerWeaponShootPosition.fOriginal);
}

int __fastcall Hooks::dGetAttachmentVec(void* ecx, void* edx, int number, Vector* origin, QAngle* angles)
{
    (void)edx;
    int result = 0;
    if (hkGetAttachmentVec.fOriginal)
        result = hkGetAttachmentVec.fOriginal(ecx, number, origin, angles);
    if (result && origin && m_VR)
        m_VR->ScaleViewmodelRenderableAttachment(ecx, *origin);
    return result;
}

int __fastcall Hooks::dGetAttachmentMatrix(void* ecx, void* edx, int number, float* matrix)
{
    (void)edx;
    int result = 0;
    if (hkGetAttachmentMatrix.fOriginal)
        result = hkGetAttachmentMatrix.fOriginal(ecx, number, matrix);
    if (result && matrix && m_VR)
    {
        Vector origin(matrix[3], matrix[7], matrix[11]);
        if (m_VR->ScaleViewmodelRenderableAttachment(ecx, origin))
        {
            matrix[3] = origin.x;
            matrix[7] = origin.y;
            matrix[11] = origin.z;
        }
    }
    return result;
}

void __fastcall Hooks::dGetBackBufferDimensions(void* ecx, void* edx, int& width, int& height)
{
    (void)edx;
    if (hkGetBackBufferDimensions.fOriginal)
        hkGetBackBufferDimensions.fOriginal(ecx, width, height);
    if (m_VR && m_VR->HudPaintActive())
    {
        int x = 0, y = 0, w = 0, h = 0;
        int fbW = 0;
        int fbH = 0;
        if (m_VR->m_HUDTexture)
        {
            fbW = m_VR->m_HUDTexture->GetActualWidth();
            fbH = m_VR->m_HUDTexture->GetActualHeight();
        }
        if (fbW < 640)
            fbW = (width > 0) ? width : 1280;
        if (fbH < 360)
            fbH = (height > 0) ? height : 720;
        if (m_VR->ComputeHudInset(fbW, fbH, x, y, w, h))
        {
            width = w;
            height = h;
            return;
        }
    }
    if (OffscreenStereoSizeLie(width, height))
        return;
    // Same-buffer stereo (ff_hmdfit only; ff_gbfit is persist-skipped).
    if (bmvr::TryHmdFitFullFrame()
        && m_VR && !NestedRenderView() && !AuxSceneRtBound()
        && (m_VR->StereoEyeBlitActive() || m_VR->m_StereoEye != 0)
        && m_VR->m_RenderWidth >= 640 && m_VR->m_RenderHeight >= 360)
    {
        width = static_cast<int>(m_VR->m_RenderWidth);
        height = static_cast<int>(m_VR->m_RenderHeight);
        return;
    }
    uint32_t fbW = 0, fbH = 0;
    if (bmvr::TryHmdFitFullFrame() && bmvr::HaveHmdFramebufferSize(fbW, fbH)
        && !NestedRenderView() && !AuxSceneRtBound())
    {
        static int s_bbLog;
        if (s_bbLog < 8 && (width != static_cast<int>(fbW) || height != static_cast<int>(fbH)))
        {
            Game::logMsg("GetBackBufferDimensions %dx%d -> %ux%u (HMD-fit)",
                width, height, fbW, fbH);
            ++s_bbLog;
        }
        width = static_cast<int>(fbW);
        height = static_cast<int>(fbH);
        return;
    }
    if (!bmvr::HaveHmdFramebufferSize(fbW, fbH))
        return;
    static int s_bbKeep;
    if (s_bbKeep < 8)
    {
        Game::logMsg("GetBackBufferDimensions %dx%d (HMD-fb %ux%u unused; keep window size)",
            width, height, fbW, fbH);
        ++s_bbKeep;
    }
}

void __fastcall Hooks::dGetScreenSize(void* ecx, void* edx, int& width, int& height)
{
    (void)edx;
    if (hkGetScreenSize.fOriginal)
        hkGetScreenSize.fOriginal(ecx, width, height);
    if (m_VR && m_VR->HudPaintActive())
    {
        int x = 0, y = 0, w = 0, h = 0;
        int fbW = 0;
        int fbH = 0;
        if (m_VR->m_HUDTexture)
        {
            fbW = m_VR->m_HUDTexture->GetActualWidth();
            fbH = m_VR->m_HUDTexture->GetActualHeight();
        }
        if (fbW < 640)
            fbW = (width > 0) ? width : 1280;
        if (fbH < 360)
            fbH = (height > 0) ? height : 720;
        if (m_VR->ComputeHudInset(fbW, fbH, x, y, w, h))
        {
            width = w;
            height = h;
            return;
        }
    }
    if (OffscreenStereoSizeLie(width, height))
        return;
    if (bmvr::TryHmdFitFullFrame()
        && m_VR && !NestedRenderView() && !AuxSceneRtBound()
        && (m_VR->StereoEyeBlitActive() || m_VR->m_StereoEye != 0)
        && m_VR->m_RenderWidth >= 640 && m_VR->m_RenderHeight >= 360)
    {
        width = static_cast<int>(m_VR->m_RenderWidth);
        height = static_cast<int>(m_VR->m_RenderHeight);
        return;
    }
    uint32_t fbW = 0, fbH = 0;
    if (bmvr::TryHmdFitFullFrame() && bmvr::HaveHmdFramebufferSize(fbW, fbH)
        && !NestedRenderView() && !AuxSceneRtBound())
    {
        static int s_ssFit;
        if (s_ssFit < 8 && (width != static_cast<int>(fbW) || height != static_cast<int>(fbH)))
        {
            Game::logMsg("GetScreenSize %dx%d -> %ux%u (HMD-fit)",
                width, height, fbW, fbH);
            ++s_ssFit;
        }
        width = static_cast<int>(fbW);
        height = static_cast<int>(fbH);
        return;
    }
    if (!bmvr::HaveHmdFramebufferSize(fbW, fbH))
        return;
    // Permanent 1584x1440 videomode inside a 2560x1440 HWND pillarboxed the
    // desktop and offset VGUI mouse (2026-08-18) when ff_hmdfit is off.
    static int s_ssLog;
    if (s_ssLog < 8 && (width != static_cast<int>(fbW) || height != static_cast<int>(fbH)))
    {
        Game::logMsg("GetScreenSize %dx%d (HMD-fb %ux%u unused; keep window size)",
            width, height, fbW, fbH);
        ++s_ssLog;
    }
}

ITexture* __fastcall Hooks::dCreateNamedRTEx(void* ecx, void* edx, const char* name, int w, int h, int sizeMode, int format, int depth, unsigned textureFlags, unsigned renderTargetFlags)
{
    (void)edx;
    if (m_VR)
        m_VR->ApplyRenderTargetFramebufferOverride(ecx);

    uint32_t fbW = 0, fbH = 0;
    const bool haveFb = bmvr::TryHmdFramebuffer() && bmvr::HaveHmdFramebufferSize(fbW, fbH);
    const bool fullMode = sizeMode == RT_SIZE_FULL_FRAME_BUFFER
        || sizeMode == RT_SIZE_FULL_FRAME_BUFFER_ROUNDED_UP;
    const bool namedFb = name && (std::strstr(name, "_rt_FullFrameFB")
        || std::strstr(name, "_rt_ResolvedFullFrame"));
    const bool namedGb = name && std::strstr(name, "_rt_gb")
        && !std::strstr(name, "shadow") && !std::strstr(name, "Shadow")
        && !std::strstr(name, "flashlight") && !std::strstr(name, "Flashlight")
        && !std::strstr(name, "csm") && !std::strstr(name, "CSM");
    const bool skipGrow = name && (std::strstr(name, "_rt_Hud") || std::strstr(name, "_rt_gui")
        || std::strstr(name, "Dof") || std::strstr(name, "dof")
        || std::strstr(name, "Water") || std::strstr(name, "Reflect")
        || std::strstr(name, "PowerOfTwo") || std::strstr(name, "_rt_Camera"));

    uint32_t growW = 0, growH = 0;
    // FullFrame/G-buffer LITERAL grow is persist-skipped (hmd_world): BM
    // PushRT stays HWND-sized, so a taller GPU texture only fills the top.
    const bool looksLikeSceneBuffer = fullMode || w <= 0 || h <= 0
        || (w >= 1280 && h >= 720 && w != h);
    uint32_t offW = 0, offH = 0;
    const bool wantGrowFb = namedFb && !skipGrow && sizeMode != RT_SIZE_PICMIP
        && bmvr::TryOffscreenWorldGrow()
        && bmvr::ComputeOffscreenEyeSize(offW, offH);
    uint32_t gbW = 0, gbH = 0;
    const bool wantGrowGb = namedGb && looksLikeSceneBuffer && !skipGrow
        && sizeMode != RT_SIZE_PICMIP
        && bmvr::ComputeGrownWorldFramebuffer(gbW, gbH);
    if (wantGrowFb)
    {
        growW = offW;
        growH = offH;
    }
    if (wantGrowGb)
    {
        growW = gbW;
        growH = gbH;
    }
    const bool wantGrow = wantGrowFb || wantGrowGb;
    if (wantGrow)
    {
        const int oldW = w;
        const int oldH = h;
        const int oldMode = sizeMode;
        w = static_cast<int>(growW);
        h = static_cast<int>(growH);
        sizeMode = RT_SIZE_LITERAL;
        static int s_growLog;
        if (s_growLog < 12)
        {
            Game::logMsg("CreateNamedRT %s grow %dx%d mode=%d -> %dx%d LITERAL (offscreen rec=%ux%u ff=%d gb=%d)",
                name ? name : "?", oldW, oldH, oldMode, w, h,
                bmvr::g_RecommendedEyeWidth, bmvr::g_RecommendedEyeHeight,
                wantGrowFb ? 1 : 0, wantGrowGb ? 1 : 0);
            ++s_growLog;
        }
        // Do not BeginRisky here. Stereo enter already marks hmd_offscreen;
        // a later map-change grow left the flag set after EndRisky(120).
    }
    else if (bmvr::TryHmdFitFullFrame()
        && haveFb && !skipGrow && (namedFb || namedGb)
        && name
        && sizeMode != RT_SIZE_PICMIP
        && !(w >= 256 && h >= 256))
    {
        const int oldW = w;
        const int oldH = h;
        const int oldMode = sizeMode;
        w = static_cast<int>(fbW);
        h = static_cast<int>(fbH);
        sizeMode = RT_SIZE_LITERAL;
        static int s_fitLog;
        if (s_fitLog < 12)
        {
            Game::logMsg("CreateNamedRT %s %dx%d mode=%d -> LITERAL %ux%u (%s, rec=%ux%u unused)",
                name, oldW, oldH, oldMode, fbW, fbH,
                namedGb ? "eye-fit G-buffer" : "HMD-fit FullFrame",
                bmvr::g_RecommendedEyeWidth, bmvr::g_RecommendedEyeHeight);
            ++s_fitLog;
        }
        static bool s_ffFitRisky;
        if (!s_ffFitRisky)
        {
            s_ffFitRisky = true;
            if (bmvr::TryHmdFitFullFrame())
                bmvr::BeginRisky(L"ff_hmdfit");
            if (bmvr::TryEyeFitWorldRts())
                bmvr::BeginRisky(L"ff_gbfit");
        }
    }
    else if (haveFb && (fullMode || namedFb) && name
        && !std::strstr(name, "shadow") && !std::strstr(name, "Shadow")
        && !std::strstr(name, "csm") && !std::strstr(name, "CSM")
        && !std::strstr(name, "flashlight"))
    {
        static int s_log;
        if (s_log < 8)
        {
            Game::logMsg("CreateNamedRT %s %dx%d mode=%d (keep engine size, HMD-fb %ux%u is eyes only)",
                name, w, h, sizeMode, fbW, fbH);
            ++s_log;
        }
    }
    if (!hkCreateNamedRTEx.fOriginal)
        return nullptr;
    ITexture* tex = hkCreateNamedRTEx.fOriginal(ecx, name, w, h, sizeMode, format, depth, textureFlags, renderTargetFlags);
    if (tex && name && (fullMode || namedFb || namedGb || wantGrow))
    {
        int aw = 0, ah = 0;
        __try
        {
            aw = tex->GetActualWidth();
            ah = tex->GetActualHeight();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            aw = 0;
            ah = 0;
        }
        static int s_actualLog;
        if (s_actualLog < 16)
        {
            Game::logMsg("CreateNamedRT %s requested %dx%d mode=%d actual %dx%d",
                name, w, h, sizeMode, aw, ah);
            ++s_actualLog;
        }
        if (aw > 0 && name && std::strstr(name, "_rt_FullFrameFB") && !std::strstr(name, "FB1") && !std::strstr(name, "FB2"))
        {
            bmvr::g_FullFrameActualWidth = static_cast<uint32_t>(aw);
            bmvr::g_FullFrameActualHeight = static_cast<uint32_t>(ah);
            if (bmvr::TryHmdFitFullFrame() && haveFb
                && std::abs(aw - static_cast<int>(fbW)) < 32
                && std::abs(ah - static_cast<int>(fbH)) < 32)
                bmvr::EndRisky(L"ff_hmdfit");
        }
        else if (aw > 0 && namedFb && bmvr::g_FullFrameActualWidth < 640)
        {
            bmvr::g_FullFrameActualWidth = static_cast<uint32_t>(aw);
            bmvr::g_FullFrameActualHeight = static_cast<uint32_t>(ah);
        }
        if (aw >= 640 && ah >= 360 && namedGb)
        {
            if (static_cast<uint32_t>(aw) >= bmvr::g_GbActualWidth
                && static_cast<uint32_t>(ah) >= bmvr::g_GbActualHeight)
            {
                bmvr::g_GbActualWidth = static_cast<uint32_t>(aw);
                bmvr::g_GbActualHeight = static_cast<uint32_t>(ah);
            }
        }
    }
    return tex;
}

void bmvr::InstallEarlyFramebufferHook()
{
    static bool attempted = false;
    if (attempted)
        return;
    attempted = true;
    if (!TryHmdFramebuffer())
        return;

    const MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("MinHook init failed for framebuffer hook (%d)", static_cast<int>(mh));
        return;
    }

    HMODULE mat = GetModuleHandleA("materialsystem.dll");
    if (!mat)
        mat = GetModuleHandleA("MaterialSystem.dll");
    if (!mat)
    {
        Log("Framebuffer hook: materialsystem.dll not loaded yet");
        return;
    }

    int off = SigScanner::VerifyOffset("materialsystem.dll", 0x52d20,
        "55 8B EC 8B 0D ? ? ? ? 8B 01 8B 80 58 04 00 00 5D FF E0");
    if (off < 0)
        off = SigScanner::VerifyOffset("MaterialSystem.dll", 0x52d20,
            "55 8B EC 8B 0D ? ? ? ? 8B 01 8B 80 58 04 00 00 5D FF E0");
    if (off == -1)
    {
        Log("GetBackBufferDimensions signature not found");
        return;
    }
    if (off == 0)
        off = 0x52d20;
    void* target = reinterpret_cast<uint8_t*>(mat) + off;
    if (Hooks::hkGetBackBufferDimensions.createHook(target, &Hooks::dGetBackBufferDimensions) != 0
        || Hooks::hkGetBackBufferDimensions.enableHook() != 0)
    {
        Log("GetBackBufferDimensions hook failed rva=0x%X", off);
        return;
    }
    Log("Hook enabled: GetBackBufferDimensions rva=0x%X fb=%ux%u",
        off, g_FramebufferWidth, g_FramebufferHeight);

    HMODULE eng = GetModuleHandleA("engine.dll");
    if (!eng)
    {
        Log("GetScreenSize: engine.dll not loaded yet");
        return;
    }
    int screenOff = SigScanner::VerifyOffset("engine.dll", 0xA6BD0,
        "55 8B EC 8B 0D ? ? ? ? 56 8B 01 FF 90 9C 01 00 00");
    if (screenOff == -1)
    {
        Log("GetScreenSize signature not found");
        return;
    }
    if (screenOff == 0)
        screenOff = 0xA6BD0;
    void* screenTarget = reinterpret_cast<uint8_t*>(eng) + screenOff;
    if (Hooks::hkGetScreenSize.pTarget)
        return;
    if (Hooks::hkGetScreenSize.createHook(screenTarget, &Hooks::dGetScreenSize) != 0
        || Hooks::hkGetScreenSize.enableHook() != 0)
    {
        Log("GetScreenSize hook failed rva=0x%X", screenOff);
        return;
    }
    Log("Hook enabled: GetScreenSize rva=0x%X", screenOff);
}
