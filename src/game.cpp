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
#include <cstring>
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
    m_Cvar = GetInterfaceAny("vstdlib.dll", { "VEngineCvar007", "VEngineCvar004", "VEngineCvar002" });
    if (!m_Cvar)
        m_Cvar = GetInterfaceAny("engine.dll", { "VEngineCvar007", "VEngineCvar004" });

    Game::logMsg("Interfaces: engine=%p matsys=%p clientent=%p icvar=%p",
        m_EngineClient, m_MaterialSystem, m_ClientEntityList, m_Cvar);

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
        Game::logMsg("BMVR Game initialized (L4D2VR architecture, Black Mesa offsets). namedRT=%d stereoRV=%d stereoCopy=%d stereoFov=%d hmdSwap=%d hmdFb=%d hmdNative=%d steamvrRT=%d waitIdle=%d absView=%d menuVR=%d relLook=%d",
        bmvr::TryNamedRenderTargets() ? 1 : 0,
        bmvr::TryStereoRenderView() ? 1 : 0,
        bmvr::TryStereoCopy() ? 1 : 0,
        bmvr::TryStereoFov() ? 1 : 0,
        bmvr::TryHmdSwapchain() ? 1 : 0,
        bmvr::TryHmdFramebuffer() ? 1 : 0,
        bmvr::TryHmdNative() ? 1 : 0,
        bmvr::TrySteamVrEyeRt() ? 1 : 0,
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

int Game::CycleWeaponSelect(int direction)
{
    // invnext/invprev via ClientCmd_Unrestricted crash BM from CreateMove
    // (2026-08-18, same bucket as gameui_activate). Drive CUserCmd::weaponselect
    // from DT_BaseCombatCharacter::m_hMyWeapons instead.
    if (!m_EngineClient || !m_ClientEntityList || direction == 0)
        return 0;
    const int local = m_EngineClient->GetLocalPlayer();
    if (local <= 0)
        return 0;
    void* player = m_ClientEntityList->GetClientEntity(local);
    if (!player)
        return 0;

    // client.dll FUN_100a3de0 RecvTable: m_hMyWeapons 0xEE4 count 0x30,
    // m_hActiveWeapon 0xFA4. EHANDLE RecvProxy FUN_101d0220 uses 13-bit entry
    // (mask 0x1FFF) with 12-bit networked index.
    constexpr int kMyWeapons = 0xEE4;
    constexpr int kActiveWeapon = 0xFA4;
    constexpr int kMaxWeapons = 48;
    constexpr uint32_t kInvalid = 0xFFFFFFFFu;
    constexpr uint32_t kEntryMask = 0x1FFFu;

    const auto* handles = reinterpret_cast<const uint32_t*>(
        reinterpret_cast<uintptr_t>(player) + kMyWeapons);
    const uint32_t active = *reinterpret_cast<const uint32_t*>(
        reinterpret_cast<uintptr_t>(player) + kActiveWeapon);

    int indices[kMaxWeapons];
    int n = 0;
    int current = -1;
    for (int i = 0; i < kMaxWeapons; ++i)
    {
        const uint32_t h = handles[i];
        if (h == 0 || h == kInvalid)
            continue;
        void* weapon = m_ClientEntityList->GetClientEntityFromHandle(static_cast<int>(h));
        if (!weapon)
            continue;
        int idx = static_cast<int>(h & kEntryMask);
        if (m_ClientEntityList->GetClientEntity(idx) != weapon)
        {
            idx = 0;
            const int hi = m_ClientEntityList->GetHighestEntityIndex();
            for (int e = 1; e <= hi; ++e)
            {
                if (m_ClientEntityList->GetClientEntity(e) == weapon)
                {
                    idx = e;
                    break;
                }
            }
        }
        if (idx <= 0)
            continue;
        if (h == active)
            current = n;
        indices[n++] = idx;
    }
    if (n <= 1)
        return 0;
    int next = 0;
    if (current >= 0)
        next = (current + (direction > 0 ? 1 : n - 1)) % n;
    else
        next = direction > 0 ? 0 : n - 1;
    return indices[next];
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
    // Cache only. DXVK Present calls this with the device lock held
    // (x32dbg PID 32704, 2026-08-18: GetThreadMode nested stdshader memcpy
    // until Not Responding). Probe from RenderView, not from D3D.
    return m_CachedMatQueueMode.load(std::memory_order_acquire);
}

void Game::ProbeMatQueueModeFromRenderView()
{
    if (!bmvr::TryMatQueue() || !m_MaterialSystem
        || !m_VR || !m_VR->IsGameplayEligible()
        || !m_EngineClient || !m_EngineClient->IsInGame()
        || !m_VR->PassThroughWarmupDone())
    {
        m_CachedMatQueueMode.store(0, std::memory_order_release);
        return;
    }
    int mode = 0;
    __try
    {
        void** vtbl = *reinterpret_cast<void***>(m_MaterialSystem);
        if (!vtbl || !vtbl[11])
        {
            m_CachedMatQueueMode.store(0, std::memory_order_release);
            return;
        }
        using tGetThreadMode = int(__thiscall*)(IMaterialSystem*);
        mode = reinterpret_cast<tGetThreadMode>(vtbl[11])(m_MaterialSystem);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        m_CachedMatQueueMode.store(0, std::memory_order_release);
        return;
    }
    if (mode < 0 || mode > 2)
        mode = 0;
    m_CachedMatQueueMode.store(mode, std::memory_order_release);
}

bool Game::MaterialVTableMatchesDump() const
{
    if (!m_MaterialSystem || !m_Offsets || !m_Offsets->GetBackBufferDimensions.valid)
        return false;
    __try
    {
        void** vtbl = *reinterpret_cast<void***>(m_MaterialSystem);
        if (!vtbl)
            return false;
        return vtbl[30] == reinterpret_cast<void*>(m_Offsets->GetBackBufferDimensions.address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool Game::SetMatQueueMode(int mode) const
{
    if (!m_MaterialSystem || mode < 0 || mode > 2)
        return false;
    if (!m_VR || !m_VR->IsGameplayEligible())
        return false;
    if (!m_EngineClient || !m_EngineClient->IsInGame())
        return false;
    if (!m_VR->PassThroughWarmupDone())
        return false;
    __try
    {
        void** vtbl = *reinterpret_cast<void***>(m_MaterialSystem);
        if (!vtbl || !vtbl[10])
            return false;
        using tSetThreadMode = void(__thiscall*)(IMaterialSystem*, int, int);
        reinterpret_cast<tSetThreadMode>(vtbl[10])(m_MaterialSystem, mode, -1);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    SetConVarInt("mat_queue_mode", mode);
    return true;
}

namespace
{
    void* FindConVarProbe(void* icvar, const char* name)
    {
        (void)icvar;
        (void)name;
        // 2026-08-18: calling ICvar vtbl[8..22] as FindVar then ConVar
        // vtbl[7..14] as SetValue ran just before load-to-menu stuck in
        // nested stdshader_dx9 / DXVK SetRenderTarget. Do not probe slots
        // until the BM ICvar FindVar index is confirmed in Ghidra.
        return nullptr;
    }

    bool ConVarSetInt(void* cvar, int value)
    {
        if (!cvar)
            return false;
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", value);
        void** vt = nullptr;
        __try { vt = *reinterpret_cast<void***>(cvar); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (!vt)
            return false;
        for (int slot = 7; slot <= 14; ++slot)
        {
            __try
            {
                using tSetStr = void(__thiscall*)(void*, const char*);
                if (!vt[slot])
                    continue;
                reinterpret_cast<tSetStr>(vt[slot])(cvar, buf);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        return false;
    }

    bool ConVarSetFloat(void* cvar, float value)
    {
        if (!cvar)
            return false;
        char buf[32];
        snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(value));
        void** vt = nullptr;
        __try { vt = *reinterpret_cast<void***>(cvar); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (!vt)
            return false;
        for (int slot = 7; slot <= 14; ++slot)
        {
            __try
            {
                using tSetStr = void(__thiscall*)(void*, const char*);
                if (!vt[slot])
                    continue;
                reinterpret_cast<tSetStr>(vt[slot])(cvar, buf);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
        return false;
    }
}

bool Game::SetConVarInt(const char* name, int value) const
{
    void* cvar = FindConVarProbe(m_Cvar, name);
    if (!cvar)
        return false;
    return ConVarSetInt(cvar, value);
}

bool Game::SetConVarFloat(const char* name, float value) const
{
    void* cvar = FindConVarProbe(m_Cvar, name);
    if (!cvar)
        return false;
    return ConVarSetFloat(cvar, value);
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
