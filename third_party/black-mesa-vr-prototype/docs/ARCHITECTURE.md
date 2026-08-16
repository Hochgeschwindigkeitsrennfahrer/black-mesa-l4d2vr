# Black Mesa VR — Architecture

## Goal

Add 6DoF OpenXR VR to **retail Black Mesa** without engine source access.

Crowbar Collective has stated they will not release Black Mesa source. That rules out a true HL2VR-style SDK rebuild. The workable path is the same class of mod as **L4D2VR / Portal2VR**: inject into the running Source process, hook rendering/input, and submit stereo frames to a VR runtime.

## Reverse engineering notes

### Half-Life 2: VR Mod (`Half-Life 2 VR`)

Observed on disk at `steamapps\common\Half-Life 2 VR`:

| Piece | Role |
| --- | --- |
| `hl2.exe` / `hl2vr.exe` | Launchers; real game is Source SDK 2013–based |
| `hlvr\` game dir | Mod content (`gameinfo.txt`, `client.dll`, `server.dll`) |
| `bin\openvr_api.dll` | OpenVR API |
| `bin\d3d9.dll` + `dxvk_d3d9.dll` | Custom DXVK fork (`DXVK: HL2VR` in log) with **OpenVR + OpenXR** extension providers |
| `hlvr\bin\client.dll` / `server.dll` | Full VR game code (hand interaction, weapons, comfort, UI) |
| `hlvr\cfg\steamvr\actions.json` | Rich SteamVR Input action sets (`move`, `ground`, `weapon`, `interact`, …) |

HL2VR is a **first-class Source build**: VR is inside `client.dll`/`server.dll`, not an external injector. Source is closed; public open-sourcing is planned but not available. OpenXR appears in DXVK’s compositor path; gameplay still leans on OpenVR/SteamVR Input.

Relevant convars / systems (string dump from `client.dll`): teleport locomotion, vignette comfort, two-hand weapons, `hlvr_*` GameUI, `VRWeapon`, snap/smooth turn, seated mode, vehicle camera modes.

### L4D2VR / Portal2VR (open source)

These are the templates for Black Mesa:

1. Build a Win32 `d3d9.dll` that embeds a **VR-capable DXVK** (Vulkan images from D3D9 surfaces).
2. On load, start a worker that waits for Source modules (`client.dll`, `engine.dll`, `MaterialSystem.dll`, …).
3. Resolve Source interfaces via `CreateInterface`.
4. Signature-scan + MinHook critical functions (`RenderView`, `CreateMove`, `CalcViewModelView`, viewport, VGUI paint, …).
5. Each frame: render left/right eye into engine RTs → extract `VkImage` via `IDirect3DVR9` → submit to compositor → drive `CUserCmd` from tracked controllers.

Portal2VR is a fork of L4D2VR; same skeleton.

### Black Mesa (this machine)

- AppID `362890`, **32-bit** Source branch with optional DXVK.
- Current Steam state: **partial download / staging** — core `engine.dll` / `client.dll` / `bms.exe` not present yet. Saves + NVIDIA RTX Remix `bin\` overlay are present.
- RTX Remix also replaces `d3d9.dll` → **mutually exclusive** with VR DXVK until Remix and VR are unified (out of scope).

## Chosen design for BMSVR

```
Black Mesa (bms.exe / hl2.exe -game bms)
        │
        ▼
  d3d9.dll  = DXVK VR fork (Vulkan + IDirect3DVR9)
        │
        ▼
  BMSVR hooks (same DLL or companion init)
        │
        ├── OpenXR runtime (preferred: SteamVR OpenXR, Meta, WMR, …)
        │     xrCreateSession + XR_KHR_vulkan_enable2
        │     stereo swapchain from VkImages
        │     action-based input
        └── Source hooks
              RenderView stereo, viewmodel to hand, CreateMove locomotion
```

### Why OpenXR

- Khronos standard; works with SteamVR’s OpenXR runtime and native runtimes.
- HL2VR’s DXVK already advertises an OpenXR provider — confirms Vulkan→OpenXR submission is viable for Source + DXVK.
- Avoids hard dependency on legacy OpenVR for new work (OpenVR remains a fallback option via SteamVR if needed).

### What we intentionally do *not* copy from HL2VR (yet)

HL2VR’s physical grab, manual reload, ladder climbing, and gravity-gun hand physics require deep `client`/`server` changes. Without BM source those need either:

- invasive binary patching / reimplementation of entities, or
- a longer reverse-engineering campaign once retail binaries are installed.

Phase 1 targets **viewer VR + motion-aimed weapons + locomotion** (L4D2VR feature set). Phase 2 can chase HL2VR-style interaction.

## Module map

| Component | Path | Notes |
| --- | --- | --- |
| Inject DLL | `BMSVR/` → builds `d3d9.dll` when linked with DXVK | Or `bmsvr.dll` loaded by a thin d3d9 proxy |
| OpenXR backend | `openxr_backend.*` | Session, views, swapchains, actions |
| Source glue | `game.*`, `hooks.*`, `offsets.*` | Signatures must be rescanned per BM build |
| DXVK VR | submodule `thirdparty/dxvk` | `fholger/dxvk_l4d2vr` or IronWolf VR-DX9 branch |
| Config | `assets/bmsvr.cfg` | Scale, snap turn, HUD distance |
| Offset tool | `tools/scan_offsets.ps1` | Runs after BM install completes |

## Coordinate spaces

Source uses Z-up, units ≈ inches. OpenXR uses Y-up meters. Conversion (same idea as L4D2VR):

- meters → Source units with `vr_scale` (default ~39.37–43.2)
- Y-up → Z-up rotation
- HMD yaw offset for snap/smooth turn

## Blocking dependency

Finish installing Black Mesa from Steam so `bin\engine.dll`, `bms\bin\client.dll`, `bms\bin\server.dll` exist. Then run `tools/scan_offsets.ps1` and fill `offsets.h`.
