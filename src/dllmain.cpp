#include <Windows.h>
#include "game.h"
#include "bmvr_flags.h"

extern "C" void __cdecl L4D2VR_ShutdownSystemMouseInputSuppression();
extern "C" void __cdecl L4D2VR_ShutdownReShadeVRBridge();

static BOOL CALLBACK RestoreMonitorMode(HMONITOR monitor, HDC, LPRECT, LPARAM)
{
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info)))
        return TRUE;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_REGISTRY_SETTINGS, &mode))
        return TRUE;
    ChangeDisplaySettingsExW(info.szDevice, &mode, nullptr, 0, nullptr);
    return TRUE;
}

static void RestoreDesktopDisplayModes()
{
    EnumDisplayMonitors(nullptr, nullptr, RestoreMonitorMode, 0);
}

static DWORD WINAPI InitBMVR(LPVOID)
{
#ifdef _DEBUG
    AllocConsole();
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
#endif
    bmvr::InitFromDisk();
    bmvr::Log("BMVR d3d9.dll loaded (L4D2VR architecture, Black Mesa offsets)");
    g_Game = new Game();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        // DXVK NormalizePresentParameters honors this. Exclusive fullscreen
        // after VR_Init caused a ChangeDisplaySettings Reset loop, 1 FPS,
        // crash, and a leftover lower desktop resolution (2026-08-16).
        SetEnvironmentVariableA("DXVK_FORCE_WINDOWED", "1");
        bmvr::SetDllModule(hModule);
        bmvr::StartWatchdog();
        bmvr::SetStage("dll_attach");
        CreateThread(nullptr, 0, InitBMVR, hModule, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        RestoreDesktopDisplayModes();
        L4D2VR_ShutdownReShadeVRBridge();
        L4D2VR_ShutdownSystemMouseInputSuppression();
        break;
    default:
        break;
    }
    return TRUE;
}
