#include <Windows.h>
#include <Shellapi.h>
#include "game.h"

#pragma comment(lib, "Shell32.lib")

DWORD WINAPI InitBMSVR(HMODULE)
{
#ifdef _DEBUG
    AllocConsole();
    FILE *fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);
#endif

    Game::logMsg("BMSVR loading...");
    g_Game = new Game();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)InitBMSVR, hModule, 0, nullptr);
    }
    return TRUE;
}

// When built without embedding DXVK, export a tiny bootstrap so an ASI loader / proxy can LoadLibrary us.
extern "C" __declspec(dllexport) void BMSVR_Init()
{
    // DllMain already starts init.
}
