#include "bmvr_flags.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace bmvr
{
    uint32_t g_RecommendedEyeWidth = 0;
    uint32_t g_RecommendedEyeHeight = 0;
    uint32_t g_FramebufferWidth = 0;
    uint32_t g_FramebufferHeight = 0;
    bool g_OpenVRInitedFromCreateDevice = false;
    float g_RenderScale = 1.f;
    float g_TurnSpeed = 0.25f;
    bool g_SnapTurning = false;
    float g_SnapTurnAngle = 45.f;
    float g_ViewmodelPosOffsetX = 16.f;
    float g_ViewmodelPosOffsetY = 3.f;
    float g_ViewmodelPosOffsetZ = -2.f;
    float g_ViewmodelAngOffsetX = 0.f;
    float g_ViewmodelAngOffsetY = 0.f;
    float g_ViewmodelAngOffsetZ = 0.f;
    float g_ControllerPitchTilt = -35.f;
    float g_IPDScale = 1.f;
    float g_HeightOffset = 0.f;
    bool g_AutoMatQueueMode = true;
    bool g_Haptics = true;
    bool g_HideCrosshair = true;
    bool g_MatchHmdHz = true;
    bool g_DisableViewBob = true;
    bool g_LeftHanded = false;
    bool g_RecenterResetsYaw = true;
    bool g_HideLocalPlayerModel = true;

    static HMODULE g_Module = nullptr;
    static std::mutex g_LogMutex;
    static bool g_TryHmdSwapchain = true;
    static bool g_TryHmdFramebuffer = true;
    static bool g_TryHmdNative = true;
    static bool g_TryNamedRT = true;
    static bool g_TryNamedStereoWrap = false;
    static bool g_TryStereoRV = true;
    static bool g_TryWaitIdle = true;
    static bool g_TryAbsView = true;
    static bool g_TryMenuCompositor = true;
    static bool g_TryRelativeHmdLook = true;
    static bool g_TryStereoCopy = false;
    static bool g_TryStereoFov = false;
    static bool g_TryMatQueue = true;
    static bool g_TrySteamVrEyeRt = false;
    static bool g_Inited = false;
    static bool g_WatchdogStarted = false;
    static char g_Stage[64] = "dll_attach";

    static std::wstring ModuleDir()
    {
        wchar_t path[MAX_PATH]{};
        HMODULE mod = g_Module;
        if (!mod)
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&ModuleDir), &mod);
        if (!mod)
            return L".";
        GetModuleFileNameW(mod, path, MAX_PATH);
        std::wstring full(path);
        const size_t slash = full.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return L".";
        return full.substr(0, slash);
    }

    static std::wstring ExeDir()
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring full(path);
        const size_t slash = full.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return L".";
        return full.substr(0, slash);
    }

    static std::wstring SkipPath()
    {
        return ExeDir() + L"\\bmvr_skip.txt";
    }

    static std::vector<std::wstring> FlagDirs()
    {
        std::vector<std::wstring> dirs;
        const std::wstring exe = ExeDir();
        const std::wstring mod = ModuleDir();
        dirs.push_back(exe);
        if (_wcsicmp(exe.c_str(), mod.c_str()) != 0)
            dirs.push_back(mod);
        return dirs;
    }

    static bool FileExists(const std::wstring& path)
    {
        const DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    static void WriteUtf8File(const std::wstring& path, const char* text, bool append)
    {
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), append ? L"ab" : L"wb");
        if (!f)
            return;
        fputs(text, f);
        if (text[0] && text[strlen(text) - 1] != '\n')
            fputc('\n', f);
        fflush(f);
        fclose(f);
    }

    static void WriteAll(const wchar_t* fileName, const char* text, bool append)
    {
        for (const auto& dir : FlagDirs())
            WriteUtf8File(dir + L"\\" + fileName, text, append);
    }

    static void ReadUserConfig(const std::wstring& path)
    {
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"r");
        if (!f)
            return;
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            char* n = line;
            while (*n == ' ' || *n == '\t')
                ++n;
            if (*n == '#' || *n == '\r' || *n == '\n' || !*n)
                continue;
            char* eq = strchr(n, '=');
            if (!eq)
                continue;
            *eq = 0;
            char* val = eq + 1;
            for (char* p = n + strlen(n); p > n && (p[-1] == ' ' || p[-1] == '\t'); --p)
                p[-1] = 0;
            while (*val == ' ' || *val == '\t')
                ++val;
            for (char* p = val; *p; ++p)
            {
                if (*p == '\r' || *p == '\n' || *p == '#')
                {
                    *p = 0;
                    break;
                }
            }
            if (std::strcmp(n, "RenderScale") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 0.5f && s <= 2.0f)
                    g_RenderScale = s;
            }
            else if (std::strcmp(n, "TurnSpeed") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 0.01f && s <= 5.f)
                    g_TurnSpeed = s;
            }
            else if (std::strcmp(n, "SnapTurning") == 0)
                g_SnapTurning = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "SnapTurnAngle") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 10.f && s <= 180.f)
                    g_SnapTurnAngle = s;
            }
            else if (std::strcmp(n, "ViewmodelPosOffsetX") == 0)
                g_ViewmodelPosOffsetX = static_cast<float>(atof(val));
            else if (std::strcmp(n, "ViewmodelPosOffsetY") == 0)
                g_ViewmodelPosOffsetY = static_cast<float>(atof(val));
            else if (std::strcmp(n, "ViewmodelPosOffsetZ") == 0)
                g_ViewmodelPosOffsetZ = static_cast<float>(atof(val));
            else if (std::strcmp(n, "ViewmodelAngOffsetX") == 0)
                g_ViewmodelAngOffsetX = static_cast<float>(atof(val));
            else if (std::strcmp(n, "ViewmodelAngOffsetY") == 0)
                g_ViewmodelAngOffsetY = static_cast<float>(atof(val));
            else if (std::strcmp(n, "ViewmodelAngOffsetZ") == 0)
                g_ViewmodelAngOffsetZ = static_cast<float>(atof(val));
            else if (std::strcmp(n, "ControllerPitchTilt") == 0)
                g_ControllerPitchTilt = static_cast<float>(atof(val));
            else if (std::strcmp(n, "IPDScale") == 0)
            {
                const float s = static_cast<float>(atof(val));
                if (s >= 0.5f && s <= 2.f)
                    g_IPDScale = s;
            }
            else if (std::strcmp(n, "HeightOffset") == 0)
                g_HeightOffset = static_cast<float>(atof(val));
            else if (std::strcmp(n, "AutoMatQueueMode") == 0)
                g_AutoMatQueueMode = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "Haptics") == 0)
                g_Haptics = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "HideCrosshair") == 0)
                g_HideCrosshair = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "MatchHmdHz") == 0)
                g_MatchHmdHz = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "DisableViewBob") == 0)
                g_DisableViewBob = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "LeftHanded") == 0)
                g_LeftHanded = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "RecenterResetsYaw") == 0)
                g_RecenterResetsYaw = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "HideLocalPlayerModel") == 0)
                g_HideLocalPlayerModel = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
            else if (std::strcmp(n, "ViewmodelDisableMoveBob") == 0)
                g_DisableViewBob = (std::strcmp(val, "true") == 0 || std::strcmp(val, "1") == 0);
        }
        fclose(f);
        Log("VR config %ls RenderScale=%.2f TurnSpeed=%.2f snap=%d vm=(%.1f,%.1f,%.1f) tilt=%.1f ipd=%.2f autoQueue=%d",
            path.c_str(), g_RenderScale, g_TurnSpeed, g_SnapTurning ? 1 : 0,
            g_ViewmodelPosOffsetX, g_ViewmodelPosOffsetY, g_ViewmodelPosOffsetZ,
            g_ControllerPitchTilt, g_IPDScale, g_AutoMatQueueMode ? 1 : 0);
    }

    static void ApplySkipName(const std::string& name, const char* via)
    {
        if (name == "hmd_swap")
            g_TryHmdSwapchain = false;
        else if (name == "hmd_fb")
            g_TryHmdFramebuffer = false;
        else if (name == "hmd_native")
            g_TryHmdNative = false;
        else if (name == "named_rt")
            g_TryNamedRT = false;
        else if (name == "stereo_rv")
            g_TryStereoRV = false;
        else if (name == "named_bind" || name == "named_l4d" || name == "named_eye" || name == "named_push")
            g_TryNamedStereoWrap = false;
        else if (name == "wait_idle")
            g_TryWaitIdle = false;
        else if (name == "abs_view")
            g_TryAbsView = false;
        else if (name == "menu_vr")
            g_TryMenuCompositor = false;
        else if (name == "rel_look")
            g_TryRelativeHmdLook = false;
        else if (name == "stereo_copy")
            g_TryStereoCopy = false;
        else if (name == "stereo_fov")
            g_TryStereoFov = false;
        else if (name == "mat_queue")
        {
            g_TryMatQueue = false;
            g_AutoMatQueueMode = false;
        }
        else if (name == "steamvr_rt")
            g_TrySteamVrEyeRt = false;
        else
            return;
        Log("Skip %s (%s)", name.c_str(), via ? via : "skip file");
    }

    static void ReadSkipFile(const std::wstring& path)
    {
        FILE* f = nullptr;
        _wfopen_s(&f, path.c_str(), L"r");
        if (!f)
            return;
        char line[128];
        while (fgets(line, sizeof(line), f))
        {
            char* n = line;
            while (*n == ' ' || *n == '\t')
                ++n;
            for (char* p = n; *p; ++p)
            {
                if (*p == '\r' || *p == '\n' || *p == '#')
                {
                    *p = 0;
                    break;
                }
            }
            if (!*n)
                continue;
            // named_rt was persisted because GetBackBufferFormat's vtable slot
            // is wrong on BM, not because CreateNamed crashed. Retry with the
            // verified RVAs. Crash-sticky bmvr_in_named_rt.flag still wins.
            // Named wrap onto HDR leftEye0 died in CSimpleWorldView after
            // three rewritten PushRT(NULL) even at 2560x1440. That skip must
            // not disable double RenderView / HMD-aspect G-buffer.
            if (std::strcmp(n, "hmd_fb") == 0)
            {
                // Matching swapchain still died after LevelInit. 2026-08-17
                // evening: GetScreenSize fix produced RenderView setup=1584,
                // then first stereo left RenderView during spawn crashed.
                // Crash-sticky is cleared without disabling the framebuffer.
                Log("Ignoring hmd_fb skip-file entry (1584 world RV works; stereo wrote height into m_eStereoEye at 0x1C)");
                continue;
            }
            if (std::strcmp(n, "named_rt") == 0)
            {
                Log("Ignoring named_rt skip-file entry (retry CreateNamedEx by RVA)");
                continue;
            }
            // rel_look sticky was left by a WaitGetPoses hang on the Present
            // thread (BeginRisky still set). That hang is not the look copy.
            if (std::strcmp(n, "rel_look") == 0)
            {
                Log("Ignoring rel_look skip-file entry (WaitGetPoses hang, not look)");
                continue;
            }
            // Named-RT stereo_rv / named_push deaths. Retry named_l4d.
            if (std::strcmp(n, "stereo_rv") == 0)
            {
                Log("Ignoring stereo_rv skip-file entry (retry L4D2 wrap + null rewrite)");
                continue;
            }
            // Outer L4D2 PushRT(eye) then BM PushRT(NULL,0,0,0,0) died.
            // named_l4d keeps the wrap and rewrites those null PushRTs.
            if (std::strcmp(n, "named_push") == 0)
            {
                Log("Ignoring named_push skip-file entry (retry L4D2 wrap + null rewrite)");
                continue;
            }
            // Rewrite-only (no wrap) was not the L4D2 path. RenderView's
            // prologue GetRT / SetRT(+0x18) restore needs the eye already
            // pushed or it puts the backbuffer back.
            if (std::strcmp(n, "named_bind") == 0)
            {
                Log("Ignoring named_bind skip-file entry (retry framebuffer-sized named RT)");
                continue;
            }
            if (std::strcmp(n, "named_l4d") == 0)
            {
                Log("Ignoring named_l4d skip-file entry (retry framebuffer-sized named RT)");
                continue;
            }
            if (std::strcmp(n, "named_eye") == 0)
            {
                Log("Ignoring named_eye skip-file entry (named PushRT wrap is off; HMD-aspect G-buffer retry)");
                continue;
            }
            ApplySkipName(n, "bmvr_skip.txt");
        }
        fclose(f);
    }

    void PersistSkip(const char* name, const char* reason)
    {
        if (!name || !name[0])
            return;
        ApplySkipName(name, reason);

        std::string existing;
        FILE* f = nullptr;
        _wfopen_s(&f, SkipPath().c_str(), L"r");
        if (f)
        {
            char buf[1024];
            while (fgets(buf, sizeof(buf), f))
                existing += buf;
            fclose(f);
        }
        if (existing.find(name) != std::string::npos)
            return;

        char line[256];
        snprintf(line, sizeof(line), "%s\n", name);
        WriteUtf8File(SkipPath(), line, true);
        WriteUtf8File(ModuleDir() + L"\\bmvr_skip.txt", line, true);
        if (reason)
            Log("Persisted skip '%s': %s", name, reason);
    }

    void SetDllModule(HMODULE module)
    {
        g_Module = module;
    }

    HMODULE DllModule()
    {
        return g_Module;
    }

    void Log(const char* fmt, ...)
    {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);

        std::lock_guard<std::mutex> lock(g_LogMutex);
        OutputDebugStringA("[BMVR] ");
        OutputDebugStringA(buf);
        OutputDebugStringA("\n");
        printf("[BMVR] %s\n", buf);

        const std::string utf8 = std::string(buf) + "\n";
        const std::wstring exeLog = ExeDir() + L"\\bmvr_log.txt";
        WriteUtf8File(exeLog, utf8.c_str(), true);
        const std::wstring modLog = ModuleDir() + L"\\bmvr_log.txt";
        if (_wcsicmp(exeLog.c_str(), modLog.c_str()) != 0)
            WriteUtf8File(modLog, utf8.c_str(), true);
    }

    static void WriteHeartbeat()
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "stage=%s pid=%lu tick=%lu\n",
            g_Stage, GetCurrentProcessId(), GetTickCount());
        WriteUtf8File(ExeDir() + L"\\bmvr_heartbeat.txt", buf, false);
    }

    void SetStage(const char* stage)
    {
        if (!stage)
            return;
        strncpy_s(g_Stage, stage, _TRUNCATE);
        WriteHeartbeat();
    }

    static DWORD WINAPI WatchdogThread(LPVOID)
    {
        Sleep(200);
        while (true)
        {
            WriteHeartbeat();
            Sleep(500);
        }
        return 0;
    }

    void StartWatchdog()
    {
        if (g_WatchdogStarted)
            return;
        g_WatchdogStarted = true;
        CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
    }

    static void ConsumeIfStuck(const wchar_t* name, bool& enabled, const char* key, const char* label)
    {
        bool stuck = false;
        for (const auto& dir : FlagDirs())
        {
            const std::wstring path = dir + L"\\bmvr_in_" + name + L".flag";
            if (!FileExists(path))
                continue;
            stuck = true;
            DeleteFileW(path.c_str());
        }
        if (!stuck)
            return;
        enabled = false;
        PersistSkip(key, "previous launch died while trying it");
        Log("Disabled %s: previous launch died while trying it", label);
    }

    void InitFromDisk()
    {
        if (g_Inited)
            return;
        g_Inited = true;
        StartWatchdog();
        ReadUserConfig(ExeDir() + L"\\VR\\config.txt");
        ReadSkipFile(SkipPath());
        ReadSkipFile(ModuleDir() + L"\\bmvr_skip.txt");
        ConsumeIfStuck(L"hmd_swap", g_TryHmdSwapchain, "hmd_swap", "HMD-sized swapchain");
        ConsumeIfStuck(L"hmd_native", g_TryHmdNative, "hmd_native", "L4D2VR recommended G-buffer size");
        // Last launch: 8 pass-through 1584 RenderViews (zNear=7) succeeded,
        // then stereo wrote 1440 into CViewSetup+0x1C (m_eStereoEye on BM).
        // RenderView indexes this+0x744 by that field. Do not disable hmd_fb.
        for (const auto& dir : FlagDirs())
            DeleteFileW((dir + L"\\bmvr_in_hmd_fb.flag").c_str());
        ConsumeIfStuck(L"named_rt", g_TryNamedRT, "named_rt", "MaterialSystem named eye RTs");
        ConsumeIfStuck(L"wait_idle", g_TryWaitIdle, "wait_idle", "WaitDeviceIdle");
        ConsumeIfStuck(L"abs_view", g_TryAbsView, "abs_view", "absolute HMD CViewSetup");
        ConsumeIfStuck(L"menu_vr", g_TryMenuCompositor, "menu_vr", "menu/background compositor Submit");
        ConsumeIfStuck(L"rel_look", g_TryRelativeHmdLook, "rel_look", "relative HMD look on RenderView copy");
        ConsumeIfStuck(L"stereo_copy", g_TryStereoCopy, "stereo_copy", "double RenderView blit stereo");
        ConsumeIfStuck(L"stereo_fov", g_TryStereoFov, "stereo_fov", "same-size HMD-FOV double RenderView");
        ConsumeIfStuck(L"mat_queue", g_TryMatQueue, "mat_queue", "L4D2VR AutoMatQueueMode / SetThreadMode 2");
        ConsumeIfStuck(L"steamvr_rt", g_TrySteamVrEyeRt, "steamvr_rt", "SteamVR recommended eye RT (offscreen)");
        // 2026-08-18: 3296x3216 private eyes + SetRT/depth redirect over the
        // 2560x1440 deferred G-buffer. Stereo pair logged redirected=1, blit
        // skipped, Present ~90fps, audio OK, Escape menu OK, world black on
        // desktop and HMD. Crash-sticky never fired (process stayed alive).
        PersistSkip("steamvr_rt", "3296 offscreen SetRT over 2560 G-buffer blacked world");
        // 16:9 blit stereo is not fused. Do not keep offering it.
        // Verified on this DLL 2026-08-16: CreateDevice + Reset forced 3168x3100,
        // desktop went black, one OpenVR Submit then rt0=null / waiting room.
        if (g_TryHmdSwapchain)
            PersistSkip("hmd_swap", "black desktop + SteamVR waiting room after Reset");
        // TransferSurface(..., TRUE) + WaitDeviceIdle during loading with
        // compositor DoNotHaveFocus coincided with the 1 FPS crash.
        if (g_TryWaitIdle)
            PersistSkip("wait_idle", "WaitDeviceIdle during loading crash");
        // Absolute HMD yaw/pitch on the live CViewSetup replaced the tram's
        // scripted camera (e.g. yaw 96.5 → HMD 14.7) and the headset went
        // black after the load logo. L4D2VR only writes HMD on stereo copies.
        if (g_TryAbsView)
            PersistSkip("abs_view", "absolute HMD on live CViewSetup blacked gameplay after load logo");
        if (g_TryStereoCopy)
            PersistSkip("stereo_copy", "16:9 blit stereo is not fused");
        PersistSkip("stereo_fov", "HMD FOV in 16:9 pixels is magnified and not fused");
        // Named HDR wrap + rewriting CSimpleWorldView PushRT(NULL) dies after
        // the third bind even when the named RT is 2560x1440. Do not retry it.
        g_TryNamedStereoWrap = false;
        SetStage("init");
    }

    bool TryHmdSwapchain() { return g_TryHmdSwapchain; }
    bool TryHmdFramebuffer() { return g_TryHmdFramebuffer; }
    bool TryHmdNative() { return g_TryHmdNative; }
    bool TryNamedRenderTargets() { return g_TryNamedRT; }
    bool TryNamedStereoWrap() { return g_TryNamedStereoWrap; }
    bool TryStereoRenderView() { return g_TryStereoRV; }
    bool TryWaitDeviceIdle() { return g_TryWaitIdle; }
    bool TryAbsoluteHmdView() { return g_TryAbsView; }
    bool TryMenuCompositor() { return g_TryMenuCompositor; }
    bool TryRelativeHmdLook() { return g_TryRelativeHmdLook; }
    bool TryStereoCopy() { return g_TryStereoCopy; }
    bool TryStereoFov() { return g_TryStereoFov; }
    bool TryMatQueue() { return g_TryMatQueue; }
    bool TrySteamVrEyeRt() { return g_TrySteamVrEyeRt; }

    void DisableNamedRenderTargets(const char* reason)
    {
        PersistSkip("named_rt", reason);
    }

    void DisableStereoRenderView(const char* reason)
    {
        PersistSkip("stereo_rv", reason);
    }

    void DisableStereoFov(const char* reason)
    {
        PersistSkip("stereo_fov", reason);
    }

    void BeginRisky(const wchar_t* name)
    {
        char ascii[64]{};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, ascii, sizeof(ascii) - 1, nullptr, nullptr);
        char stage[80];
        snprintf(stage, sizeof(stage), "in_%s", ascii);
        SetStage(stage);

        for (const auto& dir : FlagDirs())
        {
            const std::wstring path = dir + L"\\bmvr_in_" + name + L".flag";
            HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
                CloseHandle(h);
        }
    }

    void EndRisky(const wchar_t* name)
    {
        for (const auto& dir : FlagDirs())
            DeleteFileW((dir + L"\\bmvr_in_" + name + L".flag").c_str());
        SetStage("ok");
    }

    void FitHmdAspectInWindow(uint32_t winW, uint32_t winH, float aspect, uint32_t& eyeW, uint32_t& eyeH);

    void ComputeHmdFramebufferSize(uint32_t recW, uint32_t recH, uint32_t winW, uint32_t winH, float projAspect)
    {
        if (g_FramebufferWidth >= 640 && g_FramebufferHeight >= 360)
            return;
        if (recW < 640 || recH < 360)
            return;

        // L4D2VR/Portal2: m_RenderWidth/Height = GetRecommendedRenderTargetSize().
        // Named RTs at that size died on BM (PushRT NULL = backbuffer). Matching
        // G-buffer + swapchain is the equivalent. HWND stays the desktop window.
        // Exclusive 3k hmd_swap blacked the desktop; this path stays windowed.
        // Crash-sticky hmd_native falls back to fitting HMD aspect in the window.
        if (g_TryHmdNative)
        {
            uint32_t eyeW = (recW + 15u) & ~15u;
            uint32_t eyeH = (recH + 15u) & ~15u;
            if (eyeW >= 640 && eyeH >= 360)
            {
                static bool s_stampedNative;
                if (!s_stampedNative)
                {
                    s_stampedNative = true;
                    BeginRisky(L"hmd_native");
                    Log("HMD native G-buffer %ux%u (OpenVR recommended %ux%u, window %ux%u)",
                        eyeW, eyeH, recW, recH, winW, winH);
                }
                g_FramebufferWidth = eyeW;
                g_FramebufferHeight = eyeH;
                return;
            }
        }

        if (winW < 640)
            winW = recW;
        if (winH < 360)
            winH = recH;
        float aspect = projAspect;
        if (!(aspect > 0.5f && aspect < 3.f))
            aspect = static_cast<float>(recW) / static_cast<float>(recH);
        uint32_t eyeH = winH & ~1u;
        uint32_t eyeW = (static_cast<uint32_t>(static_cast<float>(eyeH) * aspect + 0.5f) + 1u) & ~1u;
        if (eyeW > winW)
        {
            eyeW = winW & ~1u;
            eyeH = (static_cast<uint32_t>(static_cast<float>(eyeW) / aspect + 0.5f) + 1u) & ~1u;
        }
        if (eyeW > recW || eyeH > recH)
        {
            const float sx = static_cast<float>(recW) / static_cast<float>(eyeW);
            const float sy = static_cast<float>(recH) / static_cast<float>(eyeH);
            const float s = sx < sy ? sx : sy;
            eyeW = (static_cast<uint32_t>(static_cast<float>(eyeW) * s) + 1u) & ~1u;
            eyeH = (static_cast<uint32_t>(static_cast<float>(eyeH) * s) + 1u) & ~1u;
        }
        if (eyeW < 640 || eyeH < 360)
            return;
        // Deferred MRT / HUD downsample want 16-pixel pitch. 1576 (1440*1.097
        // rounded even) is 1576%16=8. Menu 2D survived; first 3D/HUD did not.
        eyeW = (eyeW + 15u) & ~15u;
        eyeH = (eyeH + 15u) & ~15u;
        if (g_RenderScale > 1.001f || g_RenderScale < 0.999f)
        {
            uint32_t scaledW = (static_cast<uint32_t>(static_cast<float>(eyeW) * g_RenderScale + 0.5f) + 15u) & ~15u;
            uint32_t scaledH = (static_cast<uint32_t>(static_cast<float>(eyeH) * g_RenderScale + 0.5f) + 15u) & ~15u;
            const uint32_t recAlignW = (recW + 15u) & ~15u;
            const uint32_t recAlignH = (recH + 15u) & ~15u;
            if (scaledW > recAlignW)
                scaledW = recAlignW;
            if (scaledH > recAlignH)
                scaledH = recAlignH;
            if (scaledW >= 640 && scaledH >= 360)
            {
                Log("RenderScale %.2f G-buffer %ux%u -> %ux%u (OpenVR rec %ux%u)",
                    g_RenderScale, eyeW, eyeH, scaledW, scaledH, recW, recH);
                if (scaledW >= 2400 || scaledH >= 2400)
                    Log("RenderScale size is near the 2544 first-stereo crash. Lower RenderScale if this launch dies.");
                eyeW = scaledW;
                eyeH = scaledH;
            }
        }
        // RenderScale 1.5 of 1584x1440 is 2384x2160. That is taller than a
        // 1440p HWND. GetScreenSize 2160 on a 1440 swapchain smears the
        // bottom of the menu and spawn (2026-08-18) and stereo RenderView
        // at 2384 hung after pass-through 8/8.
        if (eyeW > winW || eyeH > winH)
        {
            const uint32_t beforeW = eyeW, beforeH = eyeH;
            FitHmdAspectInWindow(winW, winH, aspect, eyeW, eyeH);
            Log("G-buffer %ux%u exceeds window %ux%u, fitted HMD aspect %ux%u",
                beforeW, beforeH, winW, winH, eyeW, eyeH);
        }
        g_FramebufferWidth = eyeW;
        g_FramebufferHeight = eyeH;
    }

    void FitHmdAspectInWindow(uint32_t winW, uint32_t winH, float aspect, uint32_t& eyeW, uint32_t& eyeH)
    {
        if (winW < 640)
            winW = 1280;
        if (winH < 360)
            winH = 720;
        if (!(aspect > 0.5f && aspect < 3.f))
            aspect = 1.1f;
        uint32_t h = winH & ~1u;
        uint32_t w = (static_cast<uint32_t>(static_cast<float>(h) * aspect + 0.5f) + 1u) & ~1u;
        if (w > winW)
        {
            w = winW & ~1u;
            h = (static_cast<uint32_t>(static_cast<float>(w) / aspect + 0.5f) + 1u) & ~1u;
        }
        w = (w + 15u) & ~15u;
        h = (h + 15u) & ~15u;
        while (w > winW && w >= 16)
            w -= 16;
        while (h > winH && h >= 16)
            h -= 16;
        if (w < 640)
            w = winW & ~15u;
        if (h < 360)
            h = winH & ~15u;
        eyeW = w;
        eyeH = h;
    }

    bool QueryWindowClientSize(uint32_t& width, uint32_t& height)
    {
        HWND hwnd = FindWindowA("Valve001", nullptr);
        if (!hwnd)
            hwnd = FindWindowA(nullptr, "Black Mesa");
        if (!hwnd)
            return false;
        RECT rc{};
        if (!GetClientRect(hwnd, &rc))
            return false;
        const uint32_t w = static_cast<uint32_t>(rc.right - rc.left);
        const uint32_t h = static_cast<uint32_t>(rc.bottom - rc.top);
        if (w < 640 || h < 360)
            return false;
        width = w;
        height = h;
        return true;
    }

    bool HaveHmdFramebufferSize(uint32_t& width, uint32_t& height)
    {
        if (!g_TryHmdFramebuffer || g_FramebufferWidth < 640 || g_FramebufferHeight < 360)
            return false;
        // Size is latched at CreateDevice. Do not refit to the live HWND:
        // spawn Reset made GetClientRect 1920x1080 while eyes were 1584x1440,
        // EnsureStereoEyeSurfaces rebuilt 1200x1072 mid-frame and Present
        // died (2026-08-18).
        width = g_FramebufferWidth;
        height = g_FramebufferHeight;
        return true;
    }

    bool ApplyHmdAspectBackbuffer(uint32_t& width, uint32_t& height)
    {
        uint32_t w = 0, h = 0;
        if (!HaveHmdFramebufferSize(w, h))
            return false;
        if (width == w && height == h)
            return false;
        width = w;
        height = h;
        return true;
    }
}
