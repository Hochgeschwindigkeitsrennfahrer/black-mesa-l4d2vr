#include "hooks.h"
#include "game.h"
#include "sdk.h"
#include "vr.h"
#include "offsets.h"
#include "bmvr_flags.h"
#include "sigscanner.h"
#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>

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
    // CreateMove trampoline on background01 coincided with a 17s crash after
    // the first menu Submit. Look is applied in RenderView (CViewSetup).
    // LevelInit MinHook crashed inside the original on background01.
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

    return 1;
}

namespace
{
    thread_local IMatRenderContext* g_MatCtx = nullptr;
    thread_local ITexture* g_StereoRedirect = nullptr;

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
        const int eyeWidth = static_cast<int>(vr->m_RenderWidth);
        const int eyeHeight = static_cast<int>(vr->m_RenderHeight);
        view.x = 0;
        view.y = 0;
        view.m_nUnscaledX = 0;
        view.m_nUnscaledY = 0;
        view.width = eyeWidth;
        view.height = eyeHeight;
        view.m_nUnscaledWidth = eyeWidth;
        view.m_nUnscaledHeight = eyeHeight;
        // m_eStereoEye stays MONO (0). It lives at 0x1C on BM; stuffing the
        // eye height there made RenderView index this+0x744+1440 and crash.
        const float pixelAspect = (eyeHeight > 0) ? (static_cast<float>(eyeWidth) / static_cast<float>(eyeHeight)) : vr->m_Aspect;
        view.m_flAspectRatio = pixelAspect;
        view.fov = vr->HorizontalFovForAspect(pixelAspect);
        // Keep engine viewmodel FOV / zNear. Pass-through used zNear=7;
        // L4D2's zNear=6 is not required now that the G-buffer is HMD-sized.
    }

    void ClampStereoViewport(int& x, int& y, int& width, int& height)
    {
        if (!Hooks::m_VR)
            return;
        const int eyeW = static_cast<int>(Hooks::m_VR->m_RenderWidth);
        const int eyeH = static_cast<int>(Hooks::m_VR->m_RenderHeight);
        if (eyeW < 640 || eyeH < 360)
            return;
        if (g_StereoRedirect)
        {
            x = 0;
            y = 0;
            if (width > eyeW)
                width = eyeW;
            if (height > eyeH)
                height = eyeH;
            if (width < 1)
                width = eyeW;
            if (height < 1)
                height = eyeH;
            return;
        }
        // HMD-aspect G-buffer: engine may still pass the 16:9 window as the
        // main view. Do not clamp 8K CSM / flashlight targets.
        if (bmvr::TryHmdFramebuffer() && Hooks::m_VR->IsGameplayEligible()
            && height >= eyeH - 16 && height <= eyeH + 16 && width > eyeW
            && width <= eyeW * 2)
        {
            x = 0;
            y = 0;
            width = eyeW;
            height = eyeH;
        }
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
    if (!hkRenderView.fOriginal)
        return;

    static thread_local int s_depth = 0;
    auto callOriginal = [&](CViewSetup& view, int clearFlags, int drawFlags) {
        ++s_depth;
        hkRenderView.fOriginal(ecx, view, clearFlags, drawFlags);
        --s_depth;
    };

    if (s_depth > 0)
    {
        hkRenderView.fOriginal(ecx, setup, nClearFlags, whatToDraw);
        return;
    }

    const bool mainView = setup.width >= 640 && setup.height >= 360;

    auto wrap180 = [](float a) {
        while (a > 180.f) a -= 360.f;
        while (a < -180.f) a += 360.f;
        return a;
    };

    auto applyRelativeLook = [&](CViewSetup& view) {
        if (!m_VR || !bmvr::TryRelativeHmdLook() || !m_VR->m_HmdPoseValid || !m_VR->m_HmdOriginLatched)
            return;
        view.angles.y += wrap180(m_VR->m_HmdAngAbs.y - m_VR->m_HmdAngAbsZero.y);
        float pitch = view.angles.x + wrap180(m_VR->m_HmdAngAbs.x - m_VR->m_HmdAngAbsZero.x);
        if (pitch > 89.f) pitch = 89.f;
        if (pitch < -89.f) pitch = -89.f;
        view.angles.x = pitch;
    };

    auto passThrough = [&]() {
        if (m_VR)
        {
            m_VR->m_DirectEyeSubmit = false;
            m_VR->m_StereoRenderViewActive = false;
        }
        // Relative yaw/pitch on a copy. Absolute HMD on the live setup
        // replaced the tram camera and blacked the headset (abs_view).
        // Desktop will move: capture submits this same RenderView.
        if (m_VR && bmvr::TryRelativeHmdLook() && m_VR->IsGameplayEligible()
            && mainView && m_VR->m_HmdPoseValid && m_VR->m_HmdOriginLatched)
        {
            CViewSetup vrView = setup;
            applyRelativeLook(vrView);

            static int s_relLog;
            if (s_relLog < 4)
            {
                Game::logMsg("Relative look copy engine=(%.1f,%.1f) hmd=(%.1f,%.1f) out=(%.1f,%.1f)",
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
            if (s_relFrames == 120)
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
    const bool inGame = m_Game && m_Game->m_EngineClient && m_Game->m_EngineClient->IsInGame();
    if (mainView && m_VR->IsGameplayEligible())
    {
        static int s_setupLog;
        if (s_setupLog < 8)
        {
            Game::logMsg("RenderView setup=%dx%d fov=%.1f aspect=%.3f origin=(%.1f,%.1f,%.1f) ang=(%.1f,%.1f) submits=%d inGame=%d pass=%u",
                setup.width, setup.height, setup.fov, setup.m_flAspectRatio,
                setup.origin.x, setup.origin.y, setup.origin.z,
                setup.angles.x, setup.angles.y, m_VR->m_SubmitCount, inGame ? 1 : 0,
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
    constexpr uint32_t kStereoAfterPassThrough = 8;
    if (bmvr::TryStereoRenderView()
        && m_VR->IsGameplayEligible()
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
        && mainView
        && originOk
        && fovOk
        && m_VR->StereoEyesReady())
    {
        CViewSetup leftEyeView = setup;
        CViewSetup rightEyeView = setup;
        NormalizeViewSetupForVREye(leftEyeView, m_VR);
        NormalizeViewSetupForVREye(rightEyeView, m_VR);
        applyRelativeLook(leftEyeView);
        applyRelativeLook(rightEyeView);

        QAngle lookAng(leftEyeView.angles.x, leftEyeView.angles.y, leftEyeView.angles.z);
        Vector right{};
        QAngle::AngleVectors(lookAng, nullptr, &right, nullptr);
        leftEyeView.origin = m_VR->GetViewOriginLeft(setup.origin, right);
        rightEyeView.origin = m_VR->GetViewOriginRight(setup.origin, right);
        const float ipd = m_VR->m_Ipd * m_VR->m_IpdScale * m_VR->m_VRScale;
        const int eyeW = static_cast<int>(m_VR->m_RenderWidth);
        const int eyeH = static_cast<int>(m_VR->m_RenderHeight);

        static int s_hmdFbOnce;
        if (s_hmdFbOnce == 0)
        {
            s_hmdFbOnce = 1;
            bmvr::BeginRisky(L"hmd_fb");
            Game::logMsg("Stereo HMD-fb begin setup=%dx%d unscaled=%dx%d stereoEye=%d eye=%dx%d fov=%.1f->%.1f aspect=%.3f->%.3f zNear=%.1f ipd=%.2f",
                setup.width, setup.height, setup.m_nUnscaledWidth, setup.m_nUnscaledHeight,
                setup.m_eStereoEye, eyeW, eyeH,
                setup.fov, leftEyeView.fov, setup.m_flAspectRatio, leftEyeView.m_flAspectRatio,
                leftEyeView.zNear, ipd);
        }

        m_VR->m_DirectEyeSubmit = true;
        m_VR->m_StereoRenderViewActive = false;
        m_VR->m_DesktopMirrorEnabled = false;

        static int s_eyeRvLog;
        if (s_eyeRvLog < 4)
        {
            Game::logMsg("Stereo HMD-fb left RenderView %dx%d", eyeW, eyeH);
            ++s_eyeRvLog;
        }
        callOriginal(leftEyeView, nClearFlags, whatToDraw);
        m_VR->BlitCurrentGameColorTo(m_VR->m_D9LeftEyeSurface);
        if (s_eyeRvLog < 8)
        {
            Game::logMsg("Stereo HMD-fb right RenderView %dx%d", eyeW, eyeH);
            ++s_eyeRvLog;
        }
        callOriginal(rightEyeView, nClearFlags, whatToDraw);
        m_VR->BlitCurrentGameColorTo(m_VR->m_D9RightEyeSurface);

        m_VR->m_RenderedNewFrame.store(true, std::memory_order_release);

        static int s_hmdFbFrames;
        ++s_hmdFbFrames;
        if (s_hmdFbFrames == 120)
            bmvr::EndRisky(L"hmd_fb");
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

    bool result = hkCreateMove.fOriginal(ecx, flInputSampleTime, cmd);

    if (m_VR && cmd->command_number && m_VR->IsGameplayEligible())
        m_VR->m_SeenGameplay = true;

    if (!m_VR || !m_VR->m_IsVREnabled || !cmd->command_number)
        return result;

    if (m_VR->m_SafeLookActive && m_VR->m_HmdPoseValid && m_VR->m_LookApplyEnabled)
    {
        auto wrap180 = [](float a) {
            while (a > 180.f) a -= 360.f;
            while (a < -180.f) a += 360.f;
            return a;
        };
        auto clampAbs = [](float v, float lim) {
            if (v > lim) return lim;
            if (v < -lim) return -lim;
            return v;
        };

        float dyaw = wrap180(m_VR->m_HmdAngAbs.y - m_VR->m_PrevAppliedHmdYaw);
        if (fabsf(dyaw) > 0.01f)
            cmd->viewangles.y += dyaw;
        m_VR->m_PrevAppliedHmdYaw = m_VR->m_HmdAngAbs.y;

        if (m_VR->m_SoftPitchLook)
        {
            float dpitch = wrap180(m_VR->m_HmdAngAbs.x - m_VR->m_PrevAppliedHmdPitch);
            dpitch = clampAbs(dpitch, 2.0f);
            if (fabsf(dpitch) > 0.01f)
            {
                cmd->viewangles.x += dpitch;
                if (cmd->viewangles.x > 89.f) cmd->viewangles.x = 89.f;
                if (cmd->viewangles.x < -89.f) cmd->viewangles.x = -89.f;
            }
            m_VR->m_PrevAppliedHmdPitch = m_VR->m_HmdAngAbs.x;
        }
    }

    if (!m_VR->m_ProcessInputEnabled)
        return result;

    const float maxSpeed = 450.f;
    cmd->forwardmove += m_Game->m_AnalogForward * maxSpeed;
    cmd->sidemove += m_Game->m_AnalogSide * maxSpeed;
    return result;
}

void __fastcall Hooks::dLevelInit(void* ecx, void* edx, const char* newmap)
{
    (void)edx;
    if (m_VR)
        m_VR->OnLevelInit(newmap);
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
    if (hkCalcViewModelView.fOriginal)
        hkCalcViewModelView.fOriginal(ecx, owner, eyePosition, eyeAngles);
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
    if (hkDrawModelExecute.fOriginal)
        hkDrawModelExecute.fOriginal(ecx, state, info, pCustomBoneToWorld);
}

void __fastcall Hooks::dPushRenderTargetAndViewport(void* ecx, void* edx, ITexture* pTexture, ITexture* pDepthTexture, int nViewX, int nViewY, int nViewW, int nViewH)
{
    NoteMatContext(ecx);
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
    if (hkPopRenderTargetAndViewport.fOriginal)
        hkPopRenderTargetAndViewport.fOriginal(ecx);
}

void __fastcall Hooks::dVGui_Paint(void* ecx, void* edx, int mode)
{
    if (hkVgui_Paint.fOriginal)
        hkVgui_Paint.fOriginal(ecx, mode);
}

void __fastcall Hooks::dGetBackBufferDimensions(void* ecx, void* edx, int& width, int& height)
{
    (void)edx;
    if (hkGetBackBufferDimensions.fOriginal)
        hkGetBackBufferDimensions.fOriginal(ecx, width, height);
    uint32_t fbW = 0, fbH = 0;
    if (!bmvr::HaveHmdFramebufferSize(fbW, fbH))
        return;
    static int s_log;
    if (s_log < 8)
    {
        Game::logMsg("GetBackBufferDimensions %dx%d -> %ux%u", width, height, fbW, fbH);
        ++s_log;
    }
    width = static_cast<int>(fbW);
    height = static_cast<int>(fbH);
}

void __fastcall Hooks::dGetScreenSize(void* ecx, void* edx, int& width, int& height)
{
    (void)edx;
    if (hkGetScreenSize.fOriginal)
        hkGetScreenSize.fOriginal(ecx, width, height);
    uint32_t fbW = 0, fbH = 0;
    if (!bmvr::HaveHmdFramebufferSize(fbW, fbH))
        return;
    static int s_log;
    if (s_log < 12 && (width != static_cast<int>(fbW) || height != static_cast<int>(fbH)))
    {
        Game::logMsg("GetScreenSize %dx%d -> %ux%u", width, height, fbW, fbH);
        ++s_log;
    }
    width = static_cast<int>(fbW);
    height = static_cast<int>(fbH);
}

ITexture* __fastcall Hooks::dCreateNamedRTEx(void* ecx, void* edx, const char* name, int w, int h, int sizeMode, int format, int depth, unsigned textureFlags, unsigned renderTargetFlags)
{
    (void)edx;
    uint32_t fbW = 0, fbH = 0;
    const bool haveFb = bmvr::TryHmdFramebuffer() && bmvr::HaveHmdFramebufferSize(fbW, fbH);
    const bool fullMode = sizeMode == RT_SIZE_FULL_FRAME_BUFFER
        || sizeMode == RT_SIZE_FULL_FRAME_BUFFER_ROUNDED_UP;
    const bool namedFb = name && (std::strstr(name, "_rt_FullFrameFB")
        || std::strstr(name, "_rt_ResolvedFullFrame"));
    if (haveFb && (fullMode || namedFb) && name
        && !std::strstr(name, "shadow") && !std::strstr(name, "Shadow")
        && !std::strstr(name, "csm") && !std::strstr(name, "CSM")
        && !std::strstr(name, "flashlight"))
    {
        static int s_log;
        if (s_log < 16)
        {
            Game::logMsg("CreateNamedRT %s %dx%d mode=%d -> LITERAL %ux%u",
                name, w, h, sizeMode, fbW, fbH);
            ++s_log;
        }
        w = static_cast<int>(fbW);
        h = static_cast<int>(fbH);
        sizeMode = RT_SIZE_LITERAL;
    }
    if (!hkCreateNamedRTEx.fOriginal)
        return nullptr;
    return hkCreateNamedRTEx.fOriginal(ecx, name, w, h, sizeMode, format, depth, textureFlags, renderTargetFlags);
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
