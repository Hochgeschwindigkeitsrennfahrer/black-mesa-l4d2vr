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

## Pass 2026-08-28 weapon menu / MP5 / crowbar / haptics / hivehand / scale

**Compiled** (Win32 `d3d9.dll` 2699264 bytes + x64 OpenXR helper). **Installed** to DXVK folder, `bin\`, next to `bms.exe`, SteamVR manifest, and `VR\openxr_helper64`. **Not launched. Not HMD-verified.** Do not claim headset presentation.

Architecture unchanged: fused stereo, uncoupled VM, controller pose, `CUserCmd::weaponselect` (never `invnext`), existing freeze/`PulseAimHaptic`/DME scale.

| Item | Status | Notes |
|---|---|---|
| Weapon selection wheel | compiled + installed | Hold right stick click ≥180 ms. World-space HEV-amber icons in front of HMD. Right-controller ray hover glows. Release on icon → `m_PendingWeaponSelect` → CreateMove `weaponselect`. Release elsewhere cancels. Analog turn/jump/crouch/NextItem suppressed only while held. Short tap still recenters. SteamVR: `/actions/main/in/WeaponMenu` on right stick/trackpad click. OpenXR: `InventoryQuickSwitch` on the same right-stick click as `ResetPosition`. If SteamVR cached old bindings, restore BMVR defaults. |
| MP5 recoil | compiled + installed | Zero `m_vecPunchAngle` / vel and `m_recoilPunchAngles` / `m_recoilPositionOffset` / `m_recoilStartTime` on the owner while MP5 is active. Freeze MP5 fire-sequence cycle/rate (not reload/draw) so the remaining studio kick does not move the uncoupled VM. Other weapons unchanged. |
| Crowbar anims | compiled + installed | Crowbar/wrench only: freeze cycle/rate except draw/holster/deploy/pickup/admire. Idle/move/attack/swing stay pinned so the controller is the swing. Null sequence labels do not freeze (hdr may not be latched). |
| Fire haptics | compiled + installed | `UpdateWeaponFireHaptics`: pulse aim hand on `m_nMuzzleFlashParity` change or clip decrease. Not trigger-press, not a timer. Crowbar skipped here. |
| Crowbar-hit haptics | compiled + installed | Fan-trace hit still `PulseAimHaptic(3999)`. Swing-start pulse is not used. |
| Hivehand pose | compiled + installed | Root cause: identity `w_hgun` never matched `hive`/`hornet`, table was (0,0,0); `v_hgun` MainBody rest `(-11.03,-23.34,-15.69)` left the mesh ~0.6 m off the hand. Now match `hgun`/`hive`/`hornet`. Offset is rest compensation (`ox=restX`, `oy=-restY`, `oz=restZ`) from the AbsPos convention `p0 -= fwd*ox+right*oy+up*oz`, or live DME bake with X sign flipped. Not a random XYZ. Needs HMD check. |
| Grenade / revolver scale | compiled + installed | DME only: grenade/frag `×1.25`, 357/python/revolver `×1.15`. Global `g_ViewmodelScale` and crowbar `1.0` unchanged. |

Log tags: `Weapon menu opened`, `Weapon menu select entity=`, `Weapon menu cancelled`, `Viewmodel bake bone=MainBody`, `Viewmodel pose … ox,oy,oz=`.

**HMD checklist (not done):** hold right stick → readable wheel in headset; hover glow; release equips; analog turn still works when not held; MP5 no kick; crowbar visually still except draw; fire rumble on shot; crowbar rumble only on hit; hivehand on the hand; grenade larger, revolver slightly larger, crowbar unchanged.

## Pass 2026-08-28b hex menu / MP5 idle / crowbar idle / hivehand X

**HMD from previous install (user):** weapon **select works**; icons were a stacked row (hard to pick). **MP5 recoil still on.** Crowbar draw-only was good but **attack still posed** the mesh. **Haptics work.** Hivehand alignment better, still **too far forward**.

**This pass compiled + installed** (`d3d9.dll` 2712576). **Not HMD-verified.**

| Item | Change |
|---|---|
| Weapon menu | HL2VR-style **pointy-top hex honeycomb** with empty hub. Fixed category slots (pistols up, MP5/tau NE, shotgun/xbow east, crowbar down, RPG SW, gluon west, hive/nades NW). Native BM HUD icons from `bms_textures_dir.vpk` `materials/vgui/hud/weapon_*.vtf` (BGRA8888). Hover thickens the amber hex glow. Log: `Weapon menu HUD icons loaded=` |
| MP5 / crowbar | Root cause: freeze ran **after** `CalcViewModelView`, so SetupBones already used fire/attack **frame 0** (the kick/swing pose). Now force **idle sequence** (`idle1` / `IdleFidget`) **before** original CalcViewModelView. Crowbar equip/draw still play. MP5 reload/draw still play. Punch netvars still zeroed. Log: `Viewmodel force idle seq=` |
| Hivehand | Hull X `-3.6 .. +51.4`. `ox=restX=-11` put MainBody on the hand but the mesh still ~35 hu forward. Keep Y/Z rest (`oy=23.34`, `oz=-15.69`). **ox=24** (same in-hand pull as other VMs). |

Do not claim HMD success on 28b.

## Pass 2026-08-28c hex packing / HUD icons / hand-follow / MP5 bullets / numpad

**Compiled + installed this pass** (`d3d9.dll` 2727936). **Not HMD-verified.** Previous HMD: hexes packed on desktop, **gapped in the headset**; HUD icons were mostly orange squares (only pixel crowbar/glock); menu stayed world-locked while walking; MP5 **bullets** still climbed; numpad offsets were off.

| Item | Change |
|---|---|
| Hex packing | Draw radius is now `neighborPx / √3` from the same projection as the cell centers. The old 22–110 px clamp shrank HMD hexes while centers stayed FOV-projected. Thin gutter `0.97`. Same amber/dark fill, white hover outline (no hover scale). |
| Layout | 11 packed cells + **empty center = empty hands**. Inner ring of 6, outer 5 (12/10/2/8/4 o'clock, no 6 o'clock). Extra weapons overflow outside the flower. |
| Follow hand | Menu origin is recomputed every frame from the right controller + HMD basis (`hand + fwd*11 + up*5`). Walking moves it. |
| HUD icons | VTF decode now handles BGRA/RGBA/DXT1/DXT5. Upload is DXVK-safe `SYSTEMMEM` + `DEFAULT` + `UpdateTexture` (MANAGED lock was the likely miss). Alpha-crop so 256×128 HUD sprites fill the hex. Log: `Weapon menu HUD icons loaded=`. |
| Empty hands | Center hex on release sets `m_EmptyHands`: skip viewmodel DME, strip `IN_ATTACK`/`IN_ATTACK2`. Picking a weapon clears it. Not `ClientCmd`. |
| MP5 bullets | Punch/recoil netvars zeroed **and** controller `cmd->viewangles` applied **before** original CreateMove (FireBullets runs inside original). Post-hook alone was too late for predicted shots. |
| Numpad offsets | Per-weapon extras on top of the built-in table. Numpad 4/6 Y, 8/2 Z, 7/9 X; Ctrl = angles; 5 save to `VR/viewmodel_offsets.txt`; 0 reset this weapon; Shift = fine. Log: `Viewmodel numpad`. |

Do not claim HMD success on 28c.

## Pass 2026-08-28d baked poses / MP5 fire anim / menu lock / gloves / crowbar

**Compiled + installed this pass** (`d3d9.dll` 2728960). **Not HMD-verified.** Previous HMD: saved numpad poses; MP5 bullets still climbed; MP5 fire anim frozen; empty hands hid the right glove; weapon menu followed the hand; gloves sat behind the controllers; crowbar hair-trigger.

| Item | Change |
|---|---|
| Weapon poses | 2026-08-28 numpad extras baked into `ResolveWeaponViewmodelPose`. Grenade/satchel/tripmine/snark and gauss/gluon are separate rows. Install overwrites `VR/viewmodel_offsets.txt` so extras do not double. Numpad still adds on top. |
| MP5 fire anim | Removed idle-force on MP5 fire sequences. Crowbar idle-force is unchanged. **Never freeze gun fire anims.** Punch netvars still zeroed; also `sv_suppress_viewpunch 1` + `sv_viewpunch_spring_constant 0`. |
| Empty hands | Right HEV glove draws when `m_EmptyHands` (no grip-curl, no gun palm offset). Right mesh is always warmed up. |
| Weapon menu | Latches origin/basis at open (still spawned near the hand). After that it translates with player body and rotates only with stick yaw (`m_RotationOffsetY`). Hand can move freely to ray-select. |
| Glove forward | `VrHandsPoseOffsetMeters` Z `-0.10` (~60% of a ~16 cm scaled hand along aim; controller basis Z is -forward). Default + config + installer updated together. |
| Crowbar | Swing on `>2.4 m/s`, off `<0.9 m/s`, 400 ms cooldown. No longer resets NewSwing on every dip below 1.1. |

Do not claim HMD success on 28d.

## Pass 2026-08-28e OpenXR head-turn jitter (compiled + zipped; not HMD-verified)

**Compiled** (`d3d9.dll` 2729472) and packed `dist/Black-Mesa-VR-drop-in.zip`. **Not HMD-verified.**

Default path is `VRRuntimeBackend=openxr` + helper. SteamVR **Motion Smoothing** is an OpenVR compositor feature, so toggling it does nothing here. The helper was locating views at `predictedDisplayTime` and attaching those poses to the **previous** game eye image. That makes ATW/reprojection assume the late image matches the current head pose → jitter on turn.

Fix: snapshot the OpenXR HMD pose used for the stereo `RenderView`, publish it with the eye textures, and set `OpenXRHelperUseGameRenderPoseForProjection=true` so `xrEndFrame` uses that pose. Helper binary already supported the flag; d3d9 now publishes the pose. **Not HMD-verified.**



