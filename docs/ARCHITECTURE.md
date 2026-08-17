# Architecture

## What L4D2VR actually is

L4D2VR is not a separate injector plus a graphics hook. It is **one Win32 `d3d9.dll`**:

1. Full DXVK D3D9→Vulkan translation (fork under `dxvk_new`).
2. `DllMain` starts a thread that constructs `Game` after Source modules exist.
3. `Game` uses `CreateInterface` + MinHook signature scans.
4. `VR` calls `VR_Init(VRApplication_Scene)` and `IVRCompositor::Submit` with `TextureType_Vulkan`.
5. DXVK `IDirect3DVR9` (`d3d9_vr.h`, UUID `7e272b32-a49c-46c7-b1a4-ef52936bec87`) exposes `GetVRDesc`, `TransferSurface`, `LockSubmissionQueue`, `GetD3DDevice`.
6. Eye textures are created with `VR::m_CreatingTextureID` set so `D3D9DeviceEx::CreateTexture` fills `m_D9*Surface` + `m_VK*` Vulkan descriptors.
7. `D3D9DeviceEx::Present` (inside DXVK) resolves/mirrors, presents the desktop swapchain, then calls `VR::Update()` (poses + submit).

That combined-DLL design is the default here.

## Source layout

| Path | Role |
| --- | --- |
| `src/` | All BMVR code (`dllmain`, `game`, `hooks`, `vr`, `sdk`) |
| `L4D2VR/` | Thin shims so DXVK can `#include "L4D2VR/game.h"` etc. Not a second copy of the project. |
| `third_party/l4d2vr` | Vendored L4D2VR + DXVK fork |
| `third_party/black-mesa-vr-prototype` | Old OpenXR prototype — evidence only |

## Black Mesa mapping

| L4D2VR | This build (retry first) | Fallback if *this* DLL dies mid-attempt |
| --- | --- | --- |
| Replace game `d3d9.dll` | Install to thirdparty DXVK folder **and** `bin\` **and** next to `bms.exe` | — |
| `VClient016` | `VClient018` | — |
| `VEngineClient013` | `VEngineClient015` | — |
| `VMaterialSystem080` | `VMaterialSystem081` | — |
| 4-arg `RenderView` twice | 3-arg `RenderView` twice into named eye RTs | Single pass-through + FullFrameFB capture crop |
| `CreateNamedRenderTargetTextureEx` | Same, without L4D2 `isGameRunning` poke | Private `CreateTexture` |
| Absolute HMD → `CViewSetup` | Same, with finite checks | Engine angles + IPD offset only |
| Swapchain forced to HMD size in `CreateDevice` | **Skipped after verify:** 3168×3100 blacked the desktop and stopped Submit after Reset | Desktop window size; private eye RTs + backbuffer capture |
| Exclusive fullscreen `Reset` | **Refused after verify:** `Windowed=false` + `ChangeDisplaySettings` 1920×1080@0 Reset-looped, 1 FPS, crash, leftover desktop res | Force `Windowed=TRUE` (`DXVK_FORCE_WINDOWED`) |
| `WaitDeviceIdle` | After `TransferSurface` | `TransferSurface(..., FALSE)` only |
| OpenVR | OpenVR | — |

Prototype OpenXR crashes are not treated as hard bans. Crash-sticky files `bmvr_in_*.flag` next to the loaded `d3d9.dll` disable only the attempt that killed the previous process.

## Load path (Ghidra `bms.exe`)

On modern Windows the launcher calls `SetDefaultDllDirectories(0xC00)` (`USER_DIRS | SYSTEM32`) and **does not** `LoadLibraryW` the full thirdparty path. `shaderapidx9` `LoadLibrary("d3d9.dll")` then searches AddDllDirectory folders. If the video menu is native D3D9, the dxvk folder is never added, so a thirdparty-only install never runs. Putting our DLL in `bin\` and next to `bms.exe` intercepts that `LoadLibrary`.

## Frame path (target = L4D2VR)

```
CreateDevice: desktop swapchain (HMD size skipped) + IDirect3DVR9
  → VR::InitOpenVR on the Game thread
  → private eye CreateTexture (named RTs skipped: bad GetBackBufferFormat)
  → single RenderView (stereo needs named ITexture*)
  → pre-Present StretchRect RT0 or GetBackBuffer → eye textures
  → VR::Update: WaitGetPoses, TransferSurface, IVRCompositor::Submit
```

If named RTs or double RenderView are disabled, capture is unbind/pre-Present `StretchRect` of FullFrameFB or the swapchain backbuffer (after Reset, RT0 is often null).

## What we did not copy from L4D2VR

Hands/ozz, Neko post, ReShade takeover, aim-line overlays, workshop, melee weapon tables, queued compositor worker, `ExitProcess` if `VR_Init` fails at device create, L4D2 `isGameRunning` MaterialSystem offset poke.

Uncoupled viewmodel and controller aiming follow **sd805/l4d2vr** and **Gistix/portal2vr** (`CalcViewModelView` + controller `cmd->viewangles`), not the vendored keyou91 hand/reload stack.
