# Current progress (2026-08-21)

Read this file at the start of every session. Do not rediscover items below.

## Working (user-verified; do not rewrite)

- Fused 3D stereo, uncoupled 6DoF, save-load (nested `LockSubmissionQueue` deadlock fixed).
- Left-menu pause activation (slot 108 engine thread).
- Desktop HUD visible (do not steal `_rt_gui` destination).
- Crosshair off via `bmvr.cfg`.
- **FP arms hidden in HMD** — bodypart `arms` `nummeshes=0` sticky patch. User confirmed gone.
- Independent left/right controller tracking in-game: **cyan left**, **magenta right**, each follows its own controller (debug boxes; superseded by GLB gloves below).
- **Weapon proportions in HMD** — view-Y unstretch; upright vs flat controller matches desktop Glock shape (user-verified 2026-08-19).
- **Walk jump/ghost of the gun** — user-verified 2026-08-19. Velocity bob-kill at RecvTable `+0xF8`; DME rigid bone snap + 4-slot ring; `cl_viewmodel_lag` / `r_jiggle_bones` off with `DisableViewBob`.
- HEV gloves work in HMD. Do **not** enable `VrHandsRightUseViewmodelPose`.

## Why 47777b5 looked identical on desktop and HMD

`47777b5` (~30–40 FPS) coupled the engine G-buffer and the HMD eyes: they were the same pixels. Multicore is not what made later builds worse on this machine: it is already off (`AutoMatQueueMode=false`, skip `mat_queue`), and L4D2VR’s own default is also queue-off. Do **not** re-enable `SetThreadMode(2)`.

The first break is the immediate child **`d24bc07`** (no commits in between). It stopped sizing FullFrame to the HMD, always crop-blit 16:9 → eyes, and skipped leftover 16:9 `RenderView`s. Later `bad197a` flashlight ImpulseCommands is **not** the original break.

| Topic | `47777b5` (known-good) | `d24bc07` (first break) | This pass (restore same-buffer) |
|---|---|---|---|
| FullFrame | LITERAL HMD-fit (~1584×1440 in 2560×1440 HWND) | Engine 16:9 window size | LITERAL HMD-fit again (`ff_hmdfit`; **not** `ff_stereo` grow) |
| GetScreenSize / BB dims | Report HMD-fit | Keep window size except during eye blit | HMD-fit for non-nested, non-aux (HUD inset kept) |
| Leftover `RenderView` | Skip **same-size** duplicates only | Also skip leftover 16:9 mains | Same-size only (`windowed169` skip reverted) |
| Eye blit | 1:1 when sizes match | Always top-left crop of 16:9 BB | 1:1 unbind when FullFrame == eyes; crop is fallback |
| Multicore | `GetMatQueueMode` stub 0 | `AutoMatQueueMode` + `SetThreadMode(2)` | Queue **off** (already skipped; do not re-enable) |
| Flashlight | Same stereo pair feeds desktop + HMD | Deferred apply stayed on leftover 16:9 | HMD `FlashlightState` retarget kept; apply should run in eye RV |

Named `leftEye0` / `steamvr_rt` / `hmd_native` / FullFrame **grow** (`ff_stereo`) stay crash-sticky. Do **not** retry those.

## Flashlight vs fused stereo (2026-08-22; do not retry)

Fused stereo (1584×1440 HMD FOV + IPD + `GetProjectionRaw` UVs, G-buffers stay 2560) is the known-good HMD path. Desktop leftover 16:9 after stereo still applies flashlight on the monitor. Eyes blit **before** that leftover, so the HMD beam is missing.

Tried and rejected:

| Attempt | Result | Retry |
|---|---|---|
| Overlay hide / BB-size lie during stereo | Flashlight still missing | no |
| Stereo at 2560 16:9 + center crop + Submit 0..1 | Flashlight works; **stretched, stereo broken** | **no** |
| `ff_hmdfit` LITERAL FullFrame 1584, GB still 2560 | White HMD (A2R10 unbind) | **no** |
| `ff_gbfit` LITERAL FullFrame **and** `_rt_gb*` 1584 | Alloc 1584; process died on `background04` before stereo; user miss | **no** |

Keep fused 1584 eyes + 2560 world RTs. Leftover 16:9 after stereo stays (desktop beam). HMD flashlight still needs a different approach — not RT resize, not 16:9 stereo.

## Pass 2026-08-22 `fl_gbmatch` (compiled; not HMD-verified)

Stereo `CViewSetup` stays **2560×1440** (G-buffer size) with **HMD fov/aspect/IPD**. Viewport is not clamped to 1584. Squash-blit the full 2560 BB into 1584 eyes. Leftover 16:9 still runs for the desktop. Do **not** resize RTs. If the headset stretches or fusion breaks, persist-skip `fl_gbmatch` — that is the 16:9 stereo failure mode. Log: `gbmatch=1`, `left RenderView 2560x1440`, `HMD BB blit … squash=1`, `Flashlight PushRT inside eye RV`.

## Pass 2026-08-21 same-buffer restore (compiled + installed; **not HMD-verified**)

- Restore `47777b5` same-buffer: CreateNamedRT `_rt_FullFrameFB*` only (not `_rt_gbDepth2`/`_rt_gbNormal2` 1024 PICMIP). Sticky `ff_hmdfit`. Never grow. **2026-08-21 HMD white textures:** log showed unbind of A2R10 FullFrame (`fmt=35`) while desktop was 2560 composite; HMD-fit had also LITERAL-resized G-buffer *2 downsamples 1024→1584 (deferred albedo broken, flashlight still applied). Do not HMD-fit PICMIP/explicit-size `_rt_gb*`.
- Leftover skip: only same-as-stereo duplicates. Unbind StretchRect is 1:1 when sizes match. `m_DesktopMirrorEnabled` stays false (A2R10 black stretch).
- Flashlight: keep `dUpdateFlashlightState` HMD origin/forward. Log `Flashlight PushRT inside eye RV` if `_rt_gbShadowMapFlashLight` happens during an eye `RenderView`.
- Hands: right glove **off** (`VrHandsRightEnabled=false`). Left stays on. Grip `Rz=-180`. Scales unchanged.
- Anims: never rewrite `m_nSequence`. Freeze cycle/rate only sprint/swim/walk/run/bob/idle/fidget. Restore `playbackRate=1` on draw/holster/reload/fire/attack.
- Melee: `|vel|>1.1` new-swing edge; 10 Rodrigues +50° about controller right; hull ±16; range 56; origin = **viewmodel abs origin**. `dTraceRay` rewrite **removed**. CreateMove viewangles stay controller. `IN_ATTACK` 120 ms. `playbackRate=0` only on hit/miss/attack labels while melee.

Log tags to confirm: `CreateNamedRT … LITERAL` HMD-fit (not 2144 grow), no `Skip leftover … 2560x1440`, `FlashlightState -> HMD`, `Crowbar swing` without TraceRay rewrite.

**Do not claim HMD success.** User should confirm: desktop≈HMD, flashlight in HMD, FPS vs 47777b5, no warp.
