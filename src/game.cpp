#include "game.h"
#include "sdk.h"
#include "vr.h"
#include "hooks.h"
#include "offsets.h"
#include "d3d9_vr.h"
#include "bmvr_flags.h"

#include <unordered_map>
#include <initializer_list>
#include <cstdarg>
#include <cstdio>
#include <string>
using tCreateInterface = void*(__cdecl*)(const char* name, int* returnCode);

static HMODULE GetModuleWithRetry(const char* dllname, int maxTries = 1800, int delayMs = 100)
{
    for (int i = 0; i < maxTries; ++i)
    {
        if (HMODULE handle = GetModuleHandleA(dllname))
            return handle;
        if (i == 0 || (i + 1) % 50 == 0)
            Game::logMsg("Waiting for %s (%d/%d)", dllname, i + 1, maxTries);
        Sleep(delayMs);
    }
    Game::logMsg("Timed out waiting for %s", dllname);
    return nullptr;
}

static void* GetInterfaceSafe(const char* dllname, const char* interfacename)
{
    static std::unordered_map<std::string, void*> cache;
    const std::string key = std::string(dllname) + "::" + interfacename;
    if (auto it = cache.find(key); it != cache.end())
        return it->second;

    HMODULE mod = GetModuleWithRetry(dllname);
    if (!mod)
        return nullptr;

    auto CreateInterface = reinterpret_cast<tCreateInterface>(GetProcAddress(mod, "CreateInterface"));
    if (!CreateInterface)
    {
        Game::errorMsg(("CreateInterface missing in " + std::string(dllname)).c_str());
        return nullptr;
    }

    int rc = 0;
    void* iface = CreateInterface(interfacename, &rc);
    if (!iface)
        Game::logMsg("Interface not found: %s", interfacename);
    else
        cache[key] = iface;
    return iface;
}

static void* GetInterfaceAny(const char* dll, std::initializer_list<const char*> names)
{
    for (const char* n : names)
    {
        if (void* p = GetInterfaceSafe(dll, n))
            return p;
    }
    return nullptr;
}

Game::Game()
{
    m_BaseEngine = reinterpret_cast<uintptr_t>(GetModuleWithRetry("engine.dll"));
    m_BaseMaterialSystem = reinterpret_cast<uintptr_t>(GetModuleWithRetry("MaterialSystem.dll"));
    m_BaseClient = reinterpret_cast<uintptr_t>(GetModuleWithRetry("client.dll"));
    if (!m_BaseClient)
    {
        Game::errorMsg("Timed out waiting for client.dll.");
        return;
    }

    m_BaseServer = reinterpret_cast<uintptr_t>(GetModuleHandleA("server.dll"));
    m_BaseVgui2 = reinterpret_cast<uintptr_t>(GetModuleWithRetry("vgui2.dll"));

    m_ClientEntityList = static_cast<IClientEntityList*>(GetInterfaceAny("client.dll", { "VClientEntityList003" }));
    m_EngineTrace = static_cast<IEngineTrace*>(GetInterfaceAny("engine.dll", { "EngineTraceClient004", "EngineTraceClient003" }));
    m_EngineClient = static_cast<IEngineClient*>(GetInterfaceAny("engine.dll", { "VEngineClient015", "VEngineClient014", "VEngineClient013" }));
    m_MaterialSystem = static_cast<IMaterialSystem*>(GetInterfaceAny("MaterialSystem.dll", { "VMaterialSystem081", "VMaterialSystem080" }));
    m_ModelInfo = static_cast<IModelInfo*>(GetInterfaceAny("engine.dll", { "VModelInfoClient006", "VModelInfoClient004" }));
    m_ModelRender = static_cast<IModelRender*>(GetInterfaceAny("engine.dll", { "VEngineModel016" }));
    m_VguiInput = static_cast<IInput*>(GetInterfaceAny("vgui2.dll", { "VGUI_InputInternal001", "VGUI_Input005" }));
    // Do not bind ISurface until IsCursorVisible's vtable slot is verified on BM.
    // DXVK Present helpers call it when m_VguiSurface is non-null.
    m_VguiSurface = nullptr;

    Game::logMsg("Interfaces: engine=%p matsys=%p clientent=%p", m_EngineClient, m_MaterialSystem, m_ClientEntityList);

    m_Offsets = new Offsets();
    m_VR = new VR(this);
    m_Hooks = new Hooks(this);

    // Install D3D Present/SetRT hooks from this thread, not from inside Present.
    auto tryInstallD3DHooks = [this]() {
        if (!m_VR || m_VR->m_D3DHooksInstalled || !g_D3DVR9)
            return false;
        IDirect3DDevice9* device = nullptr;
        if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
            return false;
        m_VR->InstallDeviceHooks(device);
        device->Release();
        return m_VR->m_D3DHooksInstalled;
    };
    if (!tryInstallD3DHooks())
    {
        CreateThread(nullptr, 0, [](LPVOID ctx) -> DWORD {
            Game* game = static_cast<Game*>(ctx);
            for (int i = 0; i < 100; ++i)
            {
                Sleep(50);
                if (!g_D3DVR9 || !game->m_VR)
                    continue;
                IDirect3DDevice9* device = nullptr;
                if (FAILED(g_D3DVR9->GetD3DDevice(&device)) || !device)
                    continue;
                game->m_VR->InstallDeviceHooks(device);
                device->Release();
                if (game->m_VR->m_D3DHooksInstalled)
                    break;
            }
            return 0;
        }, this, 0, nullptr);
    }

    m_Initialized = true;
    bmvr::SetStage("game_ready");
        Game::logMsg("BMVR Game initialized (L4D2VR architecture, Black Mesa offsets). namedRT=%d stereoRV=%d hmdSwap=%d waitIdle=%d absView=%d menuVR=%d relLook=%d",
        bmvr::TryNamedRenderTargets() ? 1 : 0,
        bmvr::TryStereoRenderView() ? 1 : 0,
        bmvr::TryHmdSwapchain() ? 1 : 0,
        bmvr::TryWaitDeviceIdle() ? 1 : 0,
        bmvr::TryAbsoluteHmdView() ? 1 : 0,
        bmvr::TryMenuCompositor() ? 1 : 0,
        bmvr::TryRelativeHmdLook() ? 1 : 0);
}

void* Game::GetInterface(const char* dllname, const char* interfacename)
{
    return GetInterfaceSafe(dllname, interfacename);
}

C_BaseEntity* Game::GetClientEntity(int entityIndex)
{
    if (!m_ClientEntityList)
        return nullptr;
    return static_cast<C_BaseEntity*>(m_ClientEntityList->GetClientEntity(entityIndex));
}

void Game::ClientCmd(const char* szCmdString)
{
    if (m_EngineClient)
        m_EngineClient->ClientCmd(szCmdString);
}

void Game::ClientCmd_Unrestricted(const char* szCmdString)
{
    if (m_EngineClient)
        m_EngineClient->ClientCmd_Unrestricted(szCmdString);
}

int Game::GetMatQueueMode() const
{
    // IMaterialSystem081 GetThreadMode slot is not verified on BM. Calling a
    // guessed vtable entry from Present took the L4D2VR queued path or crashed.
    return 0;
}

void Game::logMsg(const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    bmvr::Log("%s", buf);
}

void Game::errorMsg(const char* msg)
{
    logMsg("[ERROR] %s", msg);
}
