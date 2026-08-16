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
    bool TryNamedRenderTargets();
    bool TryStereoRenderView();
    bool TryWaitDeviceIdle();
    bool TryAbsoluteHmdView();
    bool TryMenuCompositor();
    bool TryRelativeHmdLook();

    void DisableNamedRenderTargets(const char* reason);
    void DisableStereoRenderView(const char* reason);
    void PersistSkip(const char* name, const char* reason);

    void BeginRisky(const wchar_t* name);
    void EndRisky(const wchar_t* name);

    extern uint32_t g_RecommendedEyeWidth;
    extern uint32_t g_RecommendedEyeHeight;
    extern bool g_OpenVRInitedFromCreateDevice;
}
