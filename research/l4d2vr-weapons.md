# L4D2VR weapon / viewmodel system

Primary source: vendored `third_party/l4d2vr/L4D2VR`. Upstream: https://github.com/keyou91/l4d2vr.

Evidence tags: **Confirmed from source** / **Strong inference** / **Unknown**.

Hands (GLB / native IK) are documented in `research/l4d2vr-hands.md`. This file is the **weapon** side of the same hierarchy.

---

## 1. What L4D2VR does to the viewmodel

**Confirmed from source.** Combined approach, not a replacement gun mesh:

| Layer | Behavior |
|---|---|
| **Retain** | Original Source `CBaseViewModel` / `C_WeaponCSBase` `v_` studio models, sequences, attachments, particles. Weapon IDs and `GetViewModel` are unchanged. |
| **Move** | `CalcViewModelView` is fed controller (or mouse-mode) recommended origin/angles instead of HMD eye. Root follows the weapon hand. |
| **Offset** | Per-`WeaponID` and per-melee-name position/angle tables (`sdk/sdk.h` `viewmodelOffsets` / `meleeViewmodelOffsets`) plus live `m_ViewmodelPosAdjust` / `m_ViewmodelAngAdjust`. |
| **Stabilize** | Queued-render hard-lock via `SetAbsOrigin` + abs angles; optional bob kill by zeroing owner velocity around the original call. |
| **Bone-modify** | `DrawModelExecute`: AutoGrip rigid align, native arm IK/crop, magazine clip detach/freeze, first-person body mask. |
| **Hide arms only** | `HideArms` or melee-without-native-hands: `MATERIAL_VAR_NO_DRAW` on cached `/arms/` model. Weapon mesh still draws. |
| **Do not replace** | No independent weapon GLB. Detached **magazine** can be a native VM redraw with non-clip bones shoved away, plus an optional debug/standalone box from `VrHandSystem`. |

There is **no** second weapon entity. Two-handed mode rotates the **same** viewmodel/aim basis.

---

## 2. Complete transform hierarchy (equations)

Spaces:

- OpenVR tracking: meters (`m_RightControllerPose.TrackedDevicePos`).
- Source world: units, `m_VRScale = 43.2` by default (`vr.h` 1691).
- Source viewmodel layer: same origin, different FOV (`fovViewmodel`, `zNearViewmodel`).

### 2.1 Tracking → weapon-hand basis

**Confirmed from source.** `VR::UpdateTracking` (`vr/vr_tracking.inl` 1680–2422).

```text
controllerAbs = CameraAnchor - (0,0,64) + PivotYaw(hmdSmoothed + (ctrlLocal - hmdLocal), RotationOffset) * VRScale
(F,R,U) = AngleVectors(controllerAngSmoothed with yaw += RotationOffset)
```

Weapon-hand extra pitch (**default −45°**):

```text
F' = Rodrigues(F, R, WeaponAimPitchOffsetDeg)
U' = Rodrigues(U, R, WeaponAimPitchOffsetDeg)
```

`Rodrigues` is `VectorRotate` (`sdk/vector.h` 836–845).

If `IsVrHandsTwoHandedGripPoseActive()` and not mouse mode (`ResolvePavlovTwoHandedAimBasis`, 261–395):

```text
rearGrip  = rightControllerAbs
frontGrip = leftControllerAbs
            + F'* (offhandOffset.x * VRScale)     // skipped if mount-friendly
            + R'* (offhandOffset.y * VRScale)
            + U'* (offhandOffset.z * VRScale)
            // default offhandOffset = (0.12, 0, 0) m
twoHandF  = normalize(frontGrip - rearGrip)
clamp |front-rear| to max 0.85 m * VRScale
distanceWeight = 1 if pistol else clamp(|delta| / (0.12 m * VRScale), 0, 1)
F_aim = lerp(F', twoHandF, AimStrength * distanceWeight)   // strength default 1
```

Virtual stock (off unless `VrHandsVirtualStockEnabled`; skipped for pistols and melee):

```text
shoulder = HmdAbs + bodyFwd*(−0.28 m)*S + bodyRight*(0.16 m)*dominantSign*S + worldUp*(−0.12 m)*S
F_aim = lerp(F_aim, normalize(frontGrip - shoulder), VirtualStockStrength)  // default 0.65
U_ref = lerp(U', HmdUp, 0.15) if virtual stock else U'
```

Exponential smoothing (`ApplyPavlovTwoHandedAimSmoothing`, τ default **0.025 s**, clamp `[0, 0.25]`):

```text
α = 1 - exp(-dt / τ)
F = lerp(F_prev, F, α)
```

Special-infected auto-aim can replace `F',R',U'` before two-hand solve (**L4D2-specific**).

Mouse mode (optional): gun origin is HMD-anchored; aim is mouse pitch + body yaw or HMD ray. Controller pose is **not** the viewmodel origin.

### 2.2 Weapon-hand basis → viewmodel axes

**Confirmed from source.** After the above, unless mouse mode:

```text
ViewmodelForward/Right/Up = RightControllerForward/Right/Up
```

Then per-weapon + live adjust angles (`2412–2422`):

```text
F = Rodrigues(F, U, AngOffset.y)   // yaw
R = Rodrigues(R, U, AngOffset.y)
F = Rodrigues(F, R, AngOffset.x)   // pitch
U = Rodrigues(U, R, AngOffset.x)
R = Rodrigues(R, F, AngOffset.z)   // roll
U = Rodrigues(U, F, AngOffset.z)
```

`AngOffset = melee-or-gun table + m_ViewmodelAngAdjust`.

### 2.3 Viewmodel origin

**Confirmed from source.** `GetRecommendedViewmodelAbsPos` (`vr/vr_roomscale_prediction.inl` 1499–1521):

```text
P0 = GetRightControllerViewmodelAbsPos()     // normally m_RightControllerPosAbs
     // mouse mode: HmdAbs + HmdF*(anchor.x*S) + HmdR*(anchor.y*S) + HmdU*(anchor.z*S)
P  = P0 - ViewmodelForward * PosOffset.x
        - ViewmodelRight   * PosOffset.y
        - ViewmodelUp      * PosOffset.z
```

`PosOffset = table + m_ViewmodelPosAdjust`.

**Sign convention:** offsets are subtracted along the viewmodel axes, i.e. positive X pulls the model **backward** along gun forward. Tables are empirical “place the grip on the controller.”

`GetRecommendedViewmodelAbsAngle` (`1525–1538`):

```text
VectorAngles(ViewmodelForward, ViewmodelUp) → QAngle
```

Queued render reads a seqlocked copy (`m_RenderRecommendedViewmodelPos*`) via TLS when `t_UseRenderFrameSnapshot`.

### 2.4 Engine viewmodel entity

**Confirmed from source.** `Hooks::dCalcViewModelView` (`hooks/hooks_combat_network.inl` 84–227):

**Single-thread (`mat_queue_mode` 0/1):**

```text
original(ecx, owner, GetRecommendedViewmodelAbsPos(), GetRecommendedViewmodelAbsAngle())
```

If bob-kill or controller hard-lock (`VrHandsRightUseViewmodelPose || twoHandedGrip`): after original, `ent->GetAbsOrigin() = target`, `GetAbsAngles() = target`.

Bob-kill (`CallCalcViewModelViewOriginal`, 68–81): temporarily `owner->m_vecVelocity = 0` so Source move-bob is zero, then restore.

**Queued (`mat_queue_mode` 2):** same recommended pose as **input** to original (so bob/lag bones match the draw basis), then `CBaseEntity::SetAbsOrigin` if the offset exists, else write abs origin/angles.

Mouse mode + no stabilize + no hard-lock can still call original with **engine eye** on the queued path (`160–164`) — weapon stays head-locked in that configuration.

### 2.5 Studio bones after the root

**Confirmed from source.** Source runs sequences on the viewmodel. `dDrawModelExecute` then:

1. Copies `ModelRenderInfo_t` and may override `origin/angles` for this draw only. Queued stabilize applies `Δ = E_target · E_orig⁻¹` to every bone (`hooks_misc.inl` ~24140–24147) rather than rewriting shared entity state at draw time. Optional `m_SplitArmsToControllers` (default **false**) computes a second left-controller Δ and assigns bones by local-Y cluster when the arm span is > **4** units (`24150–24219`).
2. **AutoGrip** (`m_ViewmodelAutoGripAlignEnabled`, default true): discovers a grip/palm/attachment, computes a rigid delta so that bone sits on the controller (+ `m_ViewmodelAutoGripTargetOffsetMeters`). Blend-in over **0.18 s** (`hooks_misc.inl` 4181–4194). Official L4D2 studio fingerprints skip AutoGrip (stock grip + offset tables). `autoGripAligned=true` is stored on the VM pose snapshot so GLB `BuildViewmodelWorld` uses the snapshot palm directly instead of re-anchoring to the current entity root (`vr_hand_system.cpp` 1191–1197).
3. **Native arm IK / crop** on `/arms/` or `v_arms` / `v_hands` models only.
4. **Magazine interaction:** freeze VM pose; redraw clip bones at a hand-held world matrix; hide original clip by adding **+100000** to each clip-bone translation axis (teleport off-screen), not by skipping the draw.
5. `MaybeCaptureVrHandsVmPose` publishes bone worlds for gloves.

### 2.6 End-to-end (normal VR, no mouse, no two-hand)

```text
OpenVR right pose
  → controllerAbs (anchor, 64, VRScale, stick yaw)
  → F,R,U then −45° about R
  → F,R,U then per-weapon yaw/pitch/roll
  → origin = controllerAbs − F*ox − R*oy − U*oz
  → CBaseViewModel abs origin/angles
  → studio sequence local bones
  → optional AutoGrip Δ
  → GPU viewmodel projection (fovViewmodel)
```

Independent GLB (if enabled) is **not** in this chain unless `VrHandsRightUseViewmodelPose`, in which the glove `W` is parented to `Bip01_R_Hand` after AutoGrip.

---

## 3. Weapon model selection

**Confirmed from source.** L4D2VR does not call `SetModel` to swap `v_` paths. The active `C_WeaponCSBase` still picks the first-person model.

What *does* change per weapon:

- Offset tables (`sdk/sdk.h` 1638–1678).
- Aim-line / laser / scope eligibility.
- Magazine-interaction weapon class (shotgun shells vs detachable mag vs none).
- Two-hand pistol vs long-gun (`VrHandsLongWeapon`).
- Real-bullet-spread scale: two-hand **0.30**, single-hand continuous fire **1.30**, otherwise **1.0** (`kTwoHandedScale` / `kSingleHandScale`). Continuous-fire window **0.42 s** (`kContinuousWindowSeconds`, `vr_aiming.inl` 2572–2604). Hold windows 0.16 s two-hand vs 0.22 s single.

`C_BasePlayer::GetViewmodelOffset` (`sdk.h` 1836–1842) → active weapon `GetViewmodelOffset()`.

`GetViewmodelOffset` caches per `(weapon ptr, WeaponID)` and sets `g_Game->m_SwitchedWeapons` on change (`1684–1734`). Switch invalidates cached arms model (`hooks_misc.inl` 23703–23704).

### 3.1 Firearm table (Source units / degrees)

**Confirmed from source.** `{position {x,y,z}, angle {p,y,r}}`:

| WeaponID | pos | ang |
|---|---|---|
| NONE | 20, 3, 0 | 0,0,0 |
| PISTOL | 20.5, 5, −2 | −1, 0, 0 |
| UZI | 22.5, 5, −4 | −1.5, 0, 1 |
| PUMPSHOTGUN | 14.5, 3.5, −1.5 | −0.5, 0, 0 |
| AUTOSHOTGUN | 14.5, 3.5, −4 | −1.5, −2, 0 |
| M16A1 | 18, 5.5, −5.5 | −1.5, −2, 0 |
| HUNTING_RIFLE | 15, 4, −4 | −4.5, −5, 0 |
| MAC10 | 22.5, 4.5, −3.5 | −2, 0, 1 |
| SHOTGUN_CHROME | 14.5, 4, −2.5 | −1.5, −1, 0 |
| SCAR | 18, 5.5, −5.5 | −1.5, 0, −1 |
| SNIPER_MILITARY | 18.5, 5, −5 | 0, −1.5, 0 |
| SPAS | 16, 5, −4.5 | −1.5, −2, 0 |
| AK47 | 17.5, 5.5, −4.5 | −0.5, 0, 0 |
| MAGNUM | 22, 5, −2.5 | −0.5, 0, 0 |
| MP5 | 18.5, 4, −4.5 | −0.5, 0, 0 |
| SG552 | 20, 5.5, −4.5 | −0.5, 0, 0 |
| AWP | 21, 5.5, −5.5 | −0.5, 0, 0 |
| SCOUT | 19.5, 5, −3.5 | −0.5, 0, 0 |
| M60 | 19, 5.5, −7 | 0,0,0 |
| GRENADE_LAUNCHER | 14, 5, −2 | −1, 0, 0 |

Unknown IDs fall back to `NONE`.

### 3.2 Melee name table

Looked up via `GetMeleeWeaponInfoClient` → `CMeleeWeaponInfoStore::meleeWeaponName[256]` at `0x0CA4` (`sdk.h` 1065–1072, 1698–1724). **L4D2-specific** (map-based melee, `m_MapBasedMeleeID`).

| name | pos | ang |
|---|---|---|
| fireaxe | 12.5, −4, −21.5 | −12, −6.5, −44.5 |
| katana | 19, 6, −4 | −10.5, −18, −29 |
| electric_guitar (first entry) | 20.5, 4, −11 | −29, −11.5, −36.5 |
| baseball_bat | 18.5, 4.5, −5.5 | −58.5, −9, −25 |
| knife | 29, 7, −2.5 | −26, −19.5, −33.5 |
| golfclub | 10.5, 2, −19.5 | −8.5, −19, −34.5 |
| crowbar | 19.5, 6, −13.5 | −24.5, −6.5, −6 |
| cricket_bat | 19.5, 4, −5.5 | −63, −18, −33 |
| machete | 23.5, 6, −3.5 | −51, −11.5, −0.5 |
| tonfa | 20, 6.5, −0.5 | −54, −11.5, −23.5 |
| frying_pan | 22.5, 8.5, −7 | −12, −1.5, −41.5 |
| electric_guitar (duplicate key) | 22, 3.5, −14 | −2, 12, −16.5 |
| shovel | 17, −6.5, −11 | −17.5, −1.5, −70.5 |
| pitchfork | 12.5, 4, −9.5 | 40, 9, −3.5 |

**Confirmed from source:** `std::unordered_map` duplicate `"electric_guitar"` — the second initializer **overwrites** the first. Only `{22, 3.5, −14} / {−2, 12, −16.5}` is live.

Live calibration: holding a combo stores into `m_ViewmodelAdjustments[m_CurrentViewmodelKey]` (`vr_tracking.inl` 2358).

---

## 4. Animation sequences, events, attachments

**Confirmed from source.**

- Sequences keep running on `CBaseViewModel`. L4D2VR does not set `m_nSequence` for normal fire/reload (magazine interaction *freezes* bones while a clip is held).
- `GetPrimaryAttackActivity` is hooked but the detour is a pass-through (`hooks_misc.inl` 23016–23019); the VR melee path *calls the original* to satisfy `TestMeleeSwingCollision` preconditions (`hooks_combat_network.inl` 1740).
- Muzzle / smoke: comments at `vr.h` 867–899 prefer the **visible viewmodel** muzzle/empty bone for local tracers. Aim line can start at an anchored VM point and converge to the HMD/mouse ray (`vr.h` 1621–1688).
- Scope: separate RTT camera parented to the gun origin + `m_ScopeCameraOffset`.
- Recoil / punch: **no** L4D2VR rewrite of `m_vecPunchAngle`. Recoil is whatever Source baked into the viewmodel sequence and punch, computed around the **controller root** instead of the HMD. Empty-hands placeholder comments (`hooks_createmove.inl` 1792) note that stripping `IN_ATTACK` also suppresses muzzle flash, recoil, and reload anim.

**Strong inference:** viewmodel bob/lag that is a function of **eye** motion is either killed (`ViewmodelDisableMoveBob`) or retargeted by passing controller pose as the “eye” into `CalcViewModelView`. Punch that is applied to the **player view** may still rotate the HMD camera unless a separate path zeros it — **Unknown** whether L4D2VR zeros punch on the stereo `CViewSetup`.

---

## 5. Switching, equip, holster

**Confirmed from source.**

- Weapon switch is vanilla (`slotN`, next/prev item binds, inventory quick-switch zones around the **right** hand).
- Offset cache resets on `weapon ptr` or `WeaponID` change; `m_SwitchedWeapons` clears arms-material cache.
- Two-hand grip releases if `weaponId` changes (`vr_hand_vr_bridge.cpp` 5530–5544).
- Holster/unequip: empty-hands placeholder can spawn a dummy pistol so `IN_ATTACK2` shove still exists (`hooks_combat_network.inl` ~2096). That is inventory, not a custom holster animation.
- Auto fast-melee optionally `slot1` then `slot2` to cancel recovery (see melee doc) — a **gameplay** switch, not a viewmodel swap API.

---

## 6. Firing and aim vs the visible gun

**Confirmed from source.**

- Client `CUserCmd::viewangles` for VR-aware servers are **not** the gun pose; gun pose is packed into `mousedx/y`, `command_number` roll, `viewangles.z`, `upmove` (`WriteUsercmd`, `hooks_combat_network.inl` 2321–2458).
- For **melee**, encoded angles stay the **controller abs angles** (`BuildEncodedVRUsercmdControllerPose`, `hooks.cpp` 694–716 skips aim-line override when `m_IsMeleeWeaponActive`).
- For guns, encoded angles can be aim-line end − start (muzzle/converge), then optional `ApplyVrHandsRealBulletSpreadAimAngles`.
- `ForceNonVRServerMovement`: `CreateMove` writes `cmd->viewangles` from the right controller / non-VR aim solve so a vanilla server traces along the gun.

Tracers: optional visual-only from viewmodel muzzle (`vr.h` 867–870).

---

## 7. HUD / cursor vs the gun

**Confirmed from source.** See hands doc §11. Ammo HUD is a SteamVR overlay on the **gun-hand** controller, not a Source HUD element parented to `CBaseViewModel`. Laser cursor for menus uses controller **tip −Z**, independent of `v_` muzzle.

---

## 8. L4D2-specific vs reusable

**Reusable:**

- Uncouple `CalcViewModelView` to controller recommended pose.
- `origin = controllerAbs − F*ox − R*oy − U*oz` with a per-weapon table.
- −45° (or configurable) pitch from pointing-controller to gun-forward.
- Queued `SetAbsOrigin` hard-lock.
- AutoGrip as a rigid bone delta (needs BM attachment/bone names).
- Hide-arms material flag; keep weapon draw + sequences.

**L4D2-specific:**

- Entire `WeaponID` enum and melee **string** table (`fireaxe`, `katana`, …).
- `GetMeleeWeaponInfoClient` / `CMeleeWeaponInfoStore` layout `0x113C`.
- Terror viewmodel arm paths `models/weapons/arms/`.
- Magazine interaction / shotgun shell insert.
- Encoded VR `CUserCmd` packing.
- Special-infected forced aim.

**Unknown:** BM `v_` pivot vs L4D2 (HL2 crowbar/pistol are authored for ~54° viewmodel FOV; BMVR already notes this in `src/bmvr_flags.h`). Offsets cannot be copied 1:1 without retuning.

---

## 9. BMVR today (read-only note)

BMVR already ports the **sd805 / Portal 2 uncoupled viewmodel**:

- `src/hooks.cpp` `CalcViewModelView`.
- `src/vr.cpp` `GetRecommendedViewmodelAbsPos/Angle` with controller delta from HMD and `ResolveWeaponViewmodelPose` mapped from L4D2VR tables (`src/vr.cpp` ~1546 comment).
- No AutoGrip, no native arm IK, no GLB, no magazine detach, no two-handed Pavlov aim.

That is “retain + move + offset,” not L4D2VR’s later bone/hand stack.
