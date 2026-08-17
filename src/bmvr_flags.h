#pragma once
#include <Windows.h>
#include <cstdint>

// Crash-sticky retries of L4D2VR mechanisms the old OpenXR prototype banned.
// If Black Mesa dies mid-attempt, the in-progress flag remains on disk and
// that attempt is skipped next launch. Successful completion deletes the flag.
// Durable skips also live in bmvr_skip.txt next to bms.exe (probe loop + DLL).

namespace bmvr
{
    void SetDllModule(HMODULE module);
    HMODULE DllModule();
    void InitFromDisk();
    void StartWatchdog();

    void Log(const char* fmt, ...);
    void SetStage(const char* stage);

    bool TryHmdSwapchain();
    bool TryHmdFramebuffer();
    bool TryHmdNative();
    bool TryNamedRenderTargets();
    bool TryNamedStereoWrap();
    bool TryStereoRenderView();
    bool TryWaitDeviceIdle();
    bool TryAbsoluteHmdView();
    bool TryMenuCompositor();
    bool TryRelativeHmdLook();
    bool TryStereoCopy();
    bool TryStereoFov();

    void DisableNamedRenderTargets(const char* reason);
    void DisableStereoRenderView(const char* reason);
    void DisableStereoFov(const char* reason);
    void PersistSkip(const char* name, const char* reason);

    void BeginRisky(const wchar_t* name);
    void EndRisky(const wchar_t* name);

    // L4D2VR/Portal2: G-buffer = OpenVR recommended size. Window-fit is only
    // the fallback if recommended size crash-stickies. ApplyHmdAspectBackbuffer
    // rewrites the windowed D3D backbuffer to that size. GetScreenSize must
    // return the same values or HUD composite uses 16:9 CViewSetup on HMD RTs.
    void ComputeHmdFramebufferSize(uint32_t recW, uint32_t recH, uint32_t winW, uint32_t winH, float projAspect);
    bool HaveHmdFramebufferSize(uint32_t& width, uint32_t& height);
    bool ApplyHmdAspectBackbuffer(uint32_t& width, uint32_t& height);
    void InstallEarlyFramebufferHook();

    extern uint32_t g_RecommendedEyeWidth;
    extern uint32_t g_RecommendedEyeHeight;
    extern uint32_t g_FramebufferWidth;
    extern uint32_t g_FramebufferHeight;
    extern bool g_OpenVRInitedFromCreateDevice;
    extern float g_RenderScale;
    extern float g_TurnSpeed;
    extern bool g_SnapTurning;
    extern float g_SnapTurnAngle;
}
