# Black Mesa first-person weapons — structure for VR hands

**Date:** 2026-08-18  
**Scope:** research only. No implementation, build, inject, or live attach.  
**Install inspected:** `C:\Program Files (x86)\Steam\steamapps\common\Black Mesa`  
**Extracts:** `%TEMP%\bm-weapon-research` (outside this repo)  
**Ghidra:** Steam `bms\bin\client.dll`, image base `0x10000000`  
**Not used as a primary source:** Hochgeschwindigkeitsrennfahrer/black-mesa-vr  

Evidence tags used below: **Confirmed from source** / **Confirmed from runtime** / **Confirmed from asset inspection** / **Confirmed from Ghidra** / **Strong inference** / **Unknown**

---

## Short answer

**Yes — Black Mesa weapon *geometry* and *weapon-bone animations* can survive if traditional first-person arms are replaced by independent VR hands.** The `v_*` viewmodels are one MDL per weapon, but arms and gun are already split as **two bodyparts** with **separate root bone trees**. Sequences still drive both trees at once.

The closest viable architecture is **hybrid**:

1. Keep the existing `C_BlackMesaViewModel` + `v_<weapon>.mdl` sequences (fire, reload, pump, draw, sprint).
2. Hide the `arms` bodypart (asset-side blank bodygroup, or client-side skip of `$viewmodelhands` materials).
3. Draw independent SteamVR/OpenVR hands at the controllers. Do **not** reuse `models/weapons/v_hands.mdl` as an animation source — different skeleton (`Bip001` vs `R_Arm`).
4. Do **not** expect viewmodel finger/reload-hand poses to retarget onto VR hands without a new animation layer.

BMVR today does **not** split arms from weapons. It parents the whole combined viewmodel to the motion controller.

---

## A. Current BMVR (read-only)

Snapshot of `src/` at the time of this write-up. Another agent may still be editing these files.

### What BMVR does **not** do

- No custom hand meshes, no ozz/L4D2VR hand system, no dual-wield models. **Confirmed from source** (`docs/ARCHITECTURE.md`, `README.md`: sd805/Portal 2 uncoupled viewmodel, not keyou91 hands).
- No bodygroup writes, no `$viewmodelhands` material skip, no `SetWeaponModel` replacement. **Confirmed from source**
- `DrawModelExecute` is **compiled but not installed** (load hang coincidence 2026-08-18). `HideLocalPlayerModel` therefore never runs. **Confirmed from source** (`src/hooks.cpp` `initSourceHooks`)

### Viewmodel pose (uncoupled gun)

`Hooks::dCalcViewModelView` (`src/hooks.cpp`) replaces the engine eye input with the aim-hand controller pose when VR gameplay is eligible:

1. `Game::GetActiveWeaponModelName()` → `VR::NoteViewmodelModel` (substring tables).
2. `VR::GetRecommendedViewmodelAbsPos/Angle` from right controller (left if `LeftHanded`).
3. Call original `C_BlackMesaViewModel::CalcViewModelView`.
4. Hard-lock with `C_BaseEntity::SetAbsOrigin` / `SetAbsAngles` (`client.dll` RVA `0xAF720` / `0xAF600`, Ghidra `FUN_100af720` / `FUN_100af600`) so bob/lag cannot pull the gun back to the camera.
5. Optional `m_flModelScale` write at viewmodel `+0x7C0` when `ViewmodelScale` is not 1.

Same world pose both eyes (IPD on the gun drew two weapons). If the controller pose is invalid, the gun is head-locked to `GetViewOrigin` + HMD angles. **Confirmed from source**

Space conversion: `GetViewOrigin(body) + (controller − current HMD)`, 80 hu reach clamp, yaw from recenter. **Confirmed from source** (`VR::GetRecommendedViewmodelAbsPos`)

Per-weapon grip offsets (L4D2VR tables mapped to BM name substrings) live in `VR::ResolveWeaponViewmodelPose`. Config `ViewmodelPosOffset*` / `ViewmodelAngOffset*` are **added on top**. Crowbar/wrench: pos `(19.5, 6, -13.5)`, ang `(-24.5, -6.5, -6)`. Controller pitch tilt default `-35°` (`ControllerPitchTilt`). **Confirmed from source**

`GetViewModelFOV` is hooked (`offsets.h` `client.dll` `0x216510`) and returns HMD `m_Fov` in gameplay instead of the weapon’s `fov_viewmodel` (~50–75). **Confirmed from source**

### Weapon identity

`Game::GetActiveWeaponEntity` reads `DT_BaseCombatCharacter::m_hActiveWeapon` at player `+0xFA4`.  
`Game::GetActiveWeaponModelName` then reads **the weapon entity’s** `DT_BaseEntity::m_nModelIndex` at `+0x94`.

That index is the **world / playermodel** (`w_crowbar.mdl`), not the viewmodel (`v_crowbar.mdl`). Crowbar detection uses `find("crowbar")`, so `w_crowbar` still matches. `DrawModelExecute` would see `/v_` paths via `ModelNameIsViewmodel`, but that hook is not installed. **Confirmed from source** + **Confirmed from Ghidra** (RecvTables)

Weapon cycle: `CUserCmd::weaponselect` from `m_hMyWeapons` at `+0xEE4` (48 handles). Not `invnext`. **Confirmed from source**

### Hands / third-person body

`HideLocalPlayerModel` (config default true) would skip `DrawModelExecute` for the local player’s world model only, and **not** viewmodels. Inactive because DME is unhooked. **Confirmed from source**

No VR hand entity, no attachment to `R_Wrist`, no empty-hands model swap. **Confirmed from source**

### Scale / pivot

`ViewmodelScale` default is **1.0** in `src/bmvr_flags.cpp` and `VR/config.txt`. Docs that still say `0.5` are stale relative to this snapshot. Scale is `C_BaseAnimating::m_flModelScale` at `+0x7C0` (immediately before `m_flPoseParameter[0]` at `+0x7C4`). That scale is around the **viewmodel entity origin** (camera-space origin of the `v_` MDL), not the grip bone. **Confirmed from source** + **Confirmed from Ghidra** (`DT_BaseViewModel`)

### Crowbar melee

`VR::UpdateCrowbarMelee` (ProcessInput path): if last model name contains `crowbar` or `wrench` and OpenVR controller speed `> 1.1` m/s, fan **10** hull traces (`±16` cube, range **70** hu) along a 50° blade-tilted arc from the viewmodel origin. On hit: haptic + `IN_ATTACK` for 120 ms.

`Hooks::dTraceRay` rewrites the ray start to that origin while `m_PerformingMelee` (crash-sticky `melee_trace`). BM has no L4D2 `TestMeleeSwingCollision`. Engine crowbar script range is **56** hu (see §B). **Confirmed from source** + **Confirmed from asset inspection**

---

## B. Black Mesa assets

Tools: `bin\vpk.exe` list, Python VPK+MDL v48 parser (no Crowbar CLI; `bin\hlmv.exe` present but not run). QC/SMD **not** in VPKs.

### Scripts are DMX, not `weapon_*.txt`

Path: `bms_misc` → `scripts/gameplay/weapons/weapon_*.dmx` (keyvalues2). Client loads `scripts/gameplay/*/*.dmx` (`FUN_10259090`). **Confirmed from asset inspection** + **Confirmed from Ghidra**

| Classname | viewmodel | playermodel (world) | notes |
|---|---|---|---|
| `weapon_crowbar` | `models/weapons/v_crowbar.mdl` | `w_crowbar.mdl` / `_mp` | `anim_prefix` `crowbar`; melee range 56 / 56 MP; dmg 10 / 20 MP |
| `weapon_glock` | `v_glock.mdl` | `w_glock.mdl` | clip 17; 9mm |
| `weapon_mp5` | `v_mp5.mdl` | `w_mp5.mdl` | |
| `weapon_shotgun` | `v_shotgun.mdl` | `w_shotgun.mdl` | **`fov_viewmodel` 50** |
| `weapon_357` | `v_357.mdl` | `w_357.mdl` | |
| `weapon_crossbow` | `v_crossbow.mdl` | `w_crossbow.mdl` | |
| `weapon_tau` | `v_gauss.mdl` | `w_gauss.mdl` | |
| `weapon_gluon` | `v_egon.mdl` | pickup + `playermodel_carried` `w_egon.mdl` | |
| `weapon_rpg` | `v_rpg.mdl` | `w_rpg.mdl` | |
| `weapon_hivehand` | `v_hgun.mdl` | `w_hgun.mdl` | **`fov_viewmodel` 65** |
| `weapon_frag` | `v_grenade.mdl` | `w_grenade.mdl` | |
| `weapon_satchel` | `v_satchel.mdl` | `w_satchel.mdl` | radio: `v_satchel_radio.mdl` (hardcoded string in client) |
| `weapon_tripmine` | `v_tripmine.mdl` | `w_tripmine.mdl` | |
| `weapon_snark` | `v_squeak.mdl` | nest / `w_snark` | |
| `weapon_headcrab` | `v_headcrabbed.mdl` | `models/xenians/headcrab.mdl` | |
| `weapon_assassin_glock` | `v_glock.mdl` | `w_glock.mdl` | NPC |

Also in models VPK, no dedicated DMX in the extracted set: `v_pipewrench`, `v_stunstick`, `v_syringe`, `v_suitcase`, `v_switchblade`, `v_zombiemelee`, `v_hands`.

FileWeaponInfo keys (Ghidra table at `PTR_s_viewmodel_1055eb78`): `viewmodel`, `playermodel`, `playermodel_carried`, `anim_prefix`, ammo, muzzleflash 1P/3P, `brass_ejection_model`, `tracer`, `bucket*`, `fov_viewmodel` (default string **`"75"`** at `104568d4`), `fov_scope`, zoom FOVs, `sounds`, `primary_attack`, `secondary_attack`, `npc_attack`. **Confirmed from Ghidra**

### `v_*` vs `w_*`

- **Viewmodels (`v_*`):** large (crowbar MDL 73 KB + VVD 2.8 MB), first-person camera-space, arms+weapon, many sequences.
- **Worldmodels (`w_*`):** tiny (crowbar MDL 1840 B, 347 verts, one bone `ValveBiped.Bip01_R_Hand`, sequence `idle` only). No FP animation. **Confirmed from asset inspection**

There is **no** compiled weapon-only first-person MDL in the VPK. **Confirmed from asset inspection**

### One MDL, two bodyparts (the important split)

Every combat `v_*` inspected is MDL **v48**, **`includemodels=0`**.

Typical layout (357, glock, mp5, shotgun, crowbar, gauss, egon, rpg, hgun, …):

| Bodypart | Name | Role |
|---|---|---|
| 0 | `studio` or `body` | Weapon mesh only (one model) |
| 1 | `arms` | **7** (sometimes 6) mutually exclusive arm meshes |

Crowbar example (`v_crowbar.mdl`):

- Bodypart `studio`: `v_crowbar_crowbar_reference.smd`, 845 verts, material `crowbar_d` (`materials/models/weapons/v_crowbar/crowbar_d.vmt`) — **no** `$viewmodelhands`.
- Bodypart `arms` (7 models, all `*_r.smd` = **right arm only**):

| Index | SMD | Materials |
|---|---|---|
| 0 | `v_crowbar_hev_standardrig_r.smd` | `v_hand`, `v_hand_m` |
| 1 | `…_marine_standardrig_r.smd` | `marine_d` |
| 2 | `…_sci_standardrig_r.smd` | `bare_hands_d`, `gman_hands_d` |
| 3 | `…_grd_standardrig_r.smd` | `bare_hands_d`, `sec_guard_d` |
| 4 | `…_zomb_standardrig_r.smd` | `zombie_hands_diffuse` |
| 5 | `…_zomb_guard_standardrig_r.smd` | `zombie_guard_hands_d` |
| 6 | `…_gman_standardrig_r.smd` | `bare_hands_d`, `gman_hands_d` |

**There is no blank / hidden arms choice in the compiled MDL.** **Confirmed from asset inspection**

Two-handed guns (glock, shotgun, 357, …) use the same 7-way `arms` bodygroup with **both** arm meshes (`hev_standardrig.smd` without `_r`). Extra bodyparts on some weapons (not arms): crossbow `dart_live`, grenade `grenade`, rpg `rocket`, satchel `satchel`, snark `snark`, tripmine `tripmine` — visibility toggles for the held object. **Confirmed from asset inspection**

`pipewrench` / `stunstick`: 6 arm variants (no gman slot). **Confirmed from asset inspection**

### Playermodel manifest drives `arms` by **name**

`scripts/playermodel_manifest.txt` documents `vm_bodygroup_overrides` per viewmodel slot `0`/`1`:

| Player | `"arms"` index | Mesh |
|---|---|---|
| `mp_scientist_hev` (default) | 0 | HEV |
| `mp_marine*` | 1 | marine |
| `mp_scientist*` | 2 | scientist |
| `mp_guard*` | 3 | guard |
| `mp_zombie_sci` | 4 | zombie |
| `mp_zombie_guard` | 5 | zombie guard (comment: planned 6) |
| `mp_gman` | 6 | G-Man |

Comments in-file: *“Viewmodel index. (we can have multiple viewmodels in source)”* and planned extra SMDs. **Confirmed from asset inspection**

This is first-party proof the engine looks up bodygroup **`"arms"`** independently of the weapon bodypart. **Confirmed from asset inspection**  
String `vm_bodygroup_overrides` was **not** found as a literal in `client.dll`; another KV path almost certainly reads nested `"arms"` (playermodel loader `FUN_102447b0` only copies `label` for UI). **Unknown** exact setter function; **Strong inference** it sets `C_BaseViewModel::m_nBody` (`+0x6C4`).

### Bones: shared *convention*, not one parented skeleton

Rest pose of `R_Arm` / `L_Arm` is identical across weapons:  
`R_Arm (-7.919, 13.112, -10.355)`, `L_Arm (7.919, 13.112, -10.355)`. Finger chain names match (`R_Wrist`, `R_Index1`…). That is a **shared rig convention**, not `$includemodel` (count is 0). **Confirmed from asset inspection**

Weapon bones are **separate roots** (`parent = -1`), not parented to the arm:

- Crowbar: `crowbar_new` root + `R_Arm` root. **No left arm. No attachments.**
- Glock: `bone_gun` / `bone_clip` / `bone_trigger` roots; `bone.muzzle` and `ValveBiped.eject` parented to `bone_gun`; attachments `muzzle`, `ejectbrass`.
- Shotgun: `bone_gun`, `bone_pump`, `bone_shell` roots; attachments `muzzle`, `ejectbrass`.
- 357: gun `357` root + chamber/loader roots; `ValveBiped.muzzle` on the gun; attachment `muzzle`.

Arm bones are **not** attached to the weapon; the weapon is **not** attached to `R_Wrist`. Sequences still sample every bone. **Confirmed from asset inspection**

### Sequences (crowbar and family)

`v_crowbar.mdl` (19 sequences). No swim, no inspect-by-that-name (`draw_admire` = `ACT_VM_PICKUP`). Events = 0 on all crowbar seqs.

| Sequence | Activity |
|---|---|
| `idle1`, `idle2` | `ACT_VM_IDLE` |
| `draw`, `draw2` | `ACT_VM_DRAW` |
| `draw_admire` | `ACT_VM_PICKUP` |
| `holster` | `ACT_VM_HOLSTER` |
| `attack{1,2,3}_miss` | `ACT_VM_MISSCENTER` |
| `attack{1,2,3,4}_hit` | `ACT_VM_HITCENTER` |
| `idletolow` / `lowtoidle` / `lowidle` | lowered set |
| `idletosprint` / `sprinttoidle` / `sprintidle1` | sprint set |

Other weapons add `ACT_VM_PRIMARYATTACK`, `ACT_VM_RELOAD` / `_LONG`, `ACT_VM_FIDGET`, ironsight `ACT_VM_IDLE_IS` / `_TO_IS`, shotgun pump/`AE_CLIENT_EJECT_CUSTOM`, `AE_MUZZLEFLASH`, `AE_CL_PLAYSOUND`, `AE_CLIENT_EJECT_BRASS`. **Confirmed from asset inspection**

Player *world* crowbar activities exist in client strings (`ACT_BMMP_IDLE_CROWBAR`, `ACT_BMMP_SWIM_CROWBAR`, jump/run/gesture). Those are third-person player anims, not the `v_` model. **Confirmed from Ghidra**

### `v_hands.mdl` is not the weapon includemodel

- Skeleton: `Bip001 R/L Clavicle` (3ds Max Biped), **not** `R_Arm`.
- One bodypart `arms`, one sequence `admire1` / `ACT_VM_IDLE`.
- Materials: `v_hand`, `hev_suit`, `v_hand_sheet`.
- **No** `v_hands` path string in `client.dll`. Not referenced from extracted weapon DMX or `character_manifest.txt`.

**Confirmed from asset inspection** + **Confirmed from Ghidra** (string absent). **Strong inference:** leftover / reference / unused HEV idle, not the runtime arms source.

### Materials / `$viewmodelhands`

HEV glove VMT `materials/models/weapons/v_hand/v_hand.vmt`:

- Comment: `THIS IS KEVINS HEV HAND TEXTURE`
- `$viewmodelhands 1`
- `ModelDetailFx` proxy (blood/slime overlay from `scripts/detailmodelfx.txt`)

Crowbar metal VMT has `ModelDetailFx` but **not** `$viewmodelhands`. **Confirmed from asset inspection**

`$viewmodelhands` in client is an **IMaterialVar** on the detail-fx proxy (`FUN_1024c4c0`), together with `cl_mdldetailfx_enable_hands` / `_universal_hands`. It marks which meshes get hand gore FX — it is **not** a geometry hide switch. **Confirmed from Ghidra** + **Confirmed from asset inspection**

### Crowbar gameplay numbers (DMX)

`primary_attack.melee`: `range` 56, `cycle_hit` 0.25 / MP 0.2, `cycle_miss` 0.5 / MP 0.4, `force` 13, `damage` 10 / MP 20. Sounds `weapon_crowbar.Melee_Hit` / `Melee_Miss`. **Confirmed from asset inspection**

### Animation events (engine)

`FUN_101354a0` registers SDK-like events plus BM extras: `AE_CL_ENABLE_BODYGROUP` (0x22), `AE_CL_DISABLE_BODYGROUP` (0x23), `AE_CL_BODYGROUP_SET_VALUE` (0x24), `AE_WPN_HIDE` / `UNHIDE`, `AE_CLIENT_EJECT_CUSTOM`, clip events, `AE_CL_CROSSBOW_BOLT_ADVANCE`. Crowbar sequences currently fire **none** of these. **Confirmed from Ghidra** + **Confirmed from asset inspection**

### QC / SMD

Not shipped. SMD names survive inside the MDL (`v_crowbar_crowbar_reference.smd`, `v_crowbar_hev_standardrig_r.smd`, …). Recompile path is decompile (Crowbar GUI) → edit QC `$bodygroup arms` → `studiomdl`. **Confirmed from asset inspection**

---

## C. Ghidra / SDK 2013 correlation

Module: `bms\bin\client.dll`. Symbols stripped; names from RTTI, RecvTables, and strings.

### Classes

| Name | Evidence | Notes |
|---|---|---|
| `C_BaseViewModel` | RTTI `.?AVC_BaseViewModel@@` @ `105749f4` | SDK 2013 equivalent |
| `C_BlackMesaViewModel` | RTTI @ `1057ca44`; ClientClass `"C_BlackMesaViewModel"`; network name **`blackmesa_viewmodel`**; `DT_BlackMesaViewModel` | Drop-in for SDK `predicted_viewmodel` (that string **absent**) |
| `C_Weapon_Crowbar` / `CWeapon_Crowbar` | strings + `DT_Weapon_Crowbar` | RecvTable is **baseclass only** — no extra netvars. Melee lives in DMX FileWeaponInfo |
| `DT_BaseCombatWeapon` | `FUN_10070db0` | `m_iViewModelIndex` `+0xA30`, `m_iWorldModelIndex` `+0xA34`, `m_hOwner` `+0xA18` |

**Confirmed from Ghidra**

### `DT_BaseViewModel` (`FUN_1007c780`) vs SDK 2013

Same field *names* as Source SDK 2013 `C_BaseViewModel` RecvTable; offsets are BM-specific (extra pose/modulation):

| Field | Offset | SDK role |
|---|---|---|
| `m_nModelIndex` | `0x94` | model |
| `m_fEffects` | `0x80` | EF_NODRAW etc. |
| `m_nSkin` | `0x6C0` | |
| `m_nBody` | `0x6C4` | **bodygroups** |
| `m_flPlaybackRate` | `0x6E8` | |
| `m_flPoseParameter[0]` × 24 | `0x7C4` | |
| `m_nSequence` | `0x960` | |
| `m_nNewSequenceParity` | `0x934` | |
| `m_nResetEventsParity` | `0x938` | |
| `m_nMuzzleFlashParity` | `0x9F8` | |
| `m_nViewModelIndex` | `0xA28` | slot 0/1 |
| `m_hOwner` | `0xA2C` | |
| `m_hWeapon` | `0xA48` | owning weapon |
| `m_nAnimationParity` | `0xA38` | |
| `m_ModulationColors[0]` × 3 | `0x9A1` | BM extra |

`m_flModelScale` is not in this RecvTable; BMVR’s `+0x7C0` sits immediately before pose params, matching typical `C_BaseAnimating` layout. **Confirmed from Ghidra** / **Strong inference** for the scale field name.

`DT_BlackMesaViewModel` (`FUN_1029d680`): **only `baseclass`**. Extra behavior is client-side. **Confirmed from Ghidra**

`DT_BasePlayer`: `m_hViewModel[0]` at `+0x13F0`, array **count 2** (`FUN_100b6a00`). Matches SDK `MAX_VIEWMODELS` (2). Manifest also keys slots `"0"` and `"1"`. SP crowbar uses slot 0. **Confirmed from Ghidra** + **Confirmed from asset inspection**

`DT_LocalWeaponData`: `m_nViewModelIndex` `+0xA1C`, `m_bFlipViewModel` `+0xA94`. **Confirmed from Ghidra**

### `CalcViewModelView` — `FUN_1029d930` (RVA `0x29D930`)

Thiscall `(this, owner, eyePosition, eyeAngles)`. BMVR hooks this address.

Flow vs SDK 2013 `CBaseViewModel::CalcViewModelView`:

1. If owner null or `owner+vfunc(0x23C)` false → treat owner as null (SDK: `IsAlive`-style).
2. If `*(g_pSourceVR + 0x38)` true (`UseVR`): add player vectors at `owner+0x42C` and `+0x64D` (engine VR offsets / `vr_viewmodel_offset_forward` family). **Does not run** for injected `d3d9.dll` (`UseVR()` false). **Confirmed from Ghidra** + **Confirmed from source** (docs: `ISourceVirtualReality` unused)
3. Else: `FUN_1007d180(this)` = resolve `m_hWeapon` at `this+0xA48`. If weapon `vfunc 0x254` and not `vfunc 0x644`: `FUN_1029d850` (**viewmodel lag**, `cl_viewmodel_lag`) then weapon `vfunc 0x4F8` (bob / weapon-specific view tweak).
4. `this+vfunc 0x39C` / `0x394` (SDK-like origin/angle helpers).
5. `FUN_100af720` = `SetAbsOrigin`; `FUN_100af600` = `SetAbsAngles`.

**Confirmed from Ghidra.** Matches SDK 2013 structure with a BM VR branch and Black Mesa lag helper at `this+0xA60`.

Nearby: `FUN_1029d680` RecvTable, `FUN_1029d6f0` ClientClass — this is **`C_BlackMesaViewModel::CalcViewModelView`**. **Strong inference** (no symbol); BMVR comments already name it that.

### `GetViewModelFOV` — `FUN_10216510` (RVA `0x216510`, IClientMode slot 32)

1. Read cvar `viewmodel_fov_override`; if `> DAT_1040cf2c` (0) return it.
2. Else local player → `FUN_1025e150` (active weapon if `vfunc 0x254`) → weapon `vfunc 0x570` (per-weapon FOV from `fov_viewmodel`).
3. Else float at `10428c28` = **75.0**.

Hook fallback `54.f` in BMVR is HL2 folklore; BM’s own default is **75**. Shotgun script overrides to 50, hivehand to 65. **Confirmed from Ghidra** + **Confirmed from asset inspection**

Also present: `cl_viewmodelfov`, `r_drawviewmodel`, `cl_viewmodel_lag`, `vr_viewmodel_translate_with_head`, `vr_viewmodel_offset_forward(_large)`. Engine-VR cvars. **Confirmed from Ghidra**

### SetWeaponModel / viewmodel creation

No `SetWeaponModel` string (inlined, as in SDK). SDK pattern still holds:

- Weapon script `viewmodel` → `m_iViewModelIndex`
- Player `CreateViewModel` spawns classname `blackmesa_viewmodel` (not `predicted_viewmodel`)
- `CBaseCombatWeapon::SetViewModel` → viewmodel `SetModel` + `m_hWeapon`

**Strong inference** from RecvTables + classname + FileWeaponInfo; spawn call not fully walked. `FUN_1007d180` is the viewmodel→weapon handle walk, SDK `GetOwningWeapon`. **Confirmed from Ghidra**

### Crowbar class

`DT_Weapon_Crowbar` (`FUN_1028f710`): baseclass only. ClientClass xref near `1005cc6f`. No extra predicted melee netvar. Range/damage from DMX. **Confirmed from Ghidra**

---

## D. Critical questions

### Arms and weapon — separate models or same MDL?

**Same MDL, two bodyparts.** `v_crowbar.mdl` contains both `studio` (weapon) and `arms`. Not two entities. **Confirmed from asset inspection**

World `w_crowbar.mdl` is a third, unrelated, animation-less mesh. **Confirmed from asset inspection**

### Shared skeleton?

**Shared bone *names and rest pose* across weapons; not `$includemodel`; not one parented tree.** Each `v_*` embeds its own copy of the arm hierarchy plus weapon roots. `v_hands.mdl` does **not** share that skeleton. **Confirmed from asset inspection**

### Independent weapon bones?

**Yes.** Crowbar `crowbar_new` is a root. Glock `bone_gun` is a root. Shotgun `bone_gun` / `bone_pump` are roots. **Confirmed from asset inspection**

### Weapon attached to arm bones?

**No.** No weapon bone has `R_Wrist` / `R_Arm` as parent. No attachment named `weapon` on the arm. Muzzle attachments hang off **gun** bones. **Confirmed from asset inspection**

### Do animation sequences affect both?

**Yes.** One `m_nSequence` on the viewmodel entity drives the whole bone array (arms + gun). Hiding arm *meshes* does not stop arm *bones* from animating (harmless if no mesh). Gun moving parts keep animating. **Confirmed from asset inspection** + **Strong inference** (standard studiomdl)

### Do weapon-only FP models exist?

**No** compiled `v_*` without an `arms` bodypart (except `v_hands`, which has *only* arms and the wrong skeleton). `w_*` are third-person. **Confirmed from asset inspection**

### Can bodygroups hide arms?

**Swap yes; hide not with the stock MDL.** Engine already selects `arms` 0–6 via `vm_bodygroup_overrides`. No blank model is compiled, so `m_nBody` cannot currently mean “no arms.” Adding `$bodygroup arms { blank }` (or an extra empty SMD) is the asset-side fix; `AE_CL_DISABLE_BODYGROUP` could also hide a named group if a sequence fired it (crowbar does not). **Confirmed from asset inspection** + **Confirmed from Ghidra** (events exist)  

`m_nBody` is **networked** on `DT_BaseViewModel`. A client-only SetBodygroup can fight updates; SP listen server is the same process. **Confirmed from Ghidra**

### Can animations be reused on another representation?

- **Same MDL, hide arms:** weapon sequences keep working on weapon bones. **Strong inference** with high confidence (independent roots + bodypart split).
- **`v_hands.mdl` or SteamVR hands:** **no** without retargeting (`Bip001` ≠ `R_Arm`). **Confirmed from asset inspection**
- **Export sequences to a new weapon-only MDL:** possible if bone names/order for the *weapon* roots are preserved; arm channels can be dropped. Requires Crowbar decompile + `studiomdl`. QC not shipped. **Strong inference**

### Custom compile to keep animation and drop arms?

**Yes, that is the clean asset path.** Keep sequences and weapon SMDs; add `blank` to `$bodygroup arms` (or omit arm `$model` and accept unused bones). Do not need to split to two MDLs. **Strong inference** (studiomdl bodygroups already used this way in-game)

### Runtime vs asset-side vs hybrid?

| Approach | Hide arms? | Keep gun anims? | Notes |
|---|---|---|---|
| **Runtime `m_nBody`** | Not with stock MDL | Yes | No blank index |
| **Runtime skip `$viewmodelhands` / `v_hand*` materials** in `DrawModelExecute` | Yes (visual) | Yes | DME currently unhooked; local only; HEV/marine/sci all use those materials |
| **Asset blank bodygroup** | Yes | Yes | Matches existing `arms` system; MP-safe if server sets it |
| **Replace `SetWeaponModel` with a custom `v_*`** | Yes | If custom MDL keeps sequences | More work, same outcome |
| **Use `w_*` in first person** | N/A | **No** | No FP sequences |
| **Bone-merge `v_hands` onto `v_crowbar`** | N/A | N/A | Skeleton mismatch |

**Closest viable architecture: hybrid** — keep `C_BlackMesaViewModel` + stock sequences; hide `arms` (blank bodygroup and/or material skip); independent VR hands at controllers; do not drive those hands from `v_` finger bones.

Caveats for that hybrid:

- **Reload / two-hand poses:** mag, slide, pump, chamber still move; the left FP hand that was on the gun disappears. VR left hand will not auto-follow the old `L_Arm` animation.
- **Melee:** crowbar swing sequences rotate `crowbar_new` *and* BMVR already parents the whole VM to the controller. Stacking both can double-swing. **Strong inference:** freeze to `ACT_VM_IDLE` while motion-meleeing, or don’t play `ACT_VM_HITCENTER` on VR swings.
- **Scale/FOV:** `v_` meshes are authored for a ~50–75° viewmodel projection sitting at the camera, not a 1:1 world gun. Uncoupled VR makes them huge unless grip offset + optional scale (around origin, not grip) are tuned. **Confirmed from source** + **Confirmed from asset inspection** (`eyeposition` 0,0,0; non-zero root bone offsets; `view_bb` all zeros)
- **`$origin`:** not in MDL keyvalues. Camera-space placement is baked into root bone rest poses (e.g. crowbar `crowbar_new` at `(-13.3, -4.75, 14.1)`). **Confirmed from asset inspection**

---

## Scale / pivot / FOV (engine)

- Viewmodels are a **separate render FOV** (`IClientMode::GetViewModelFOV`), default **75**, per-weapon `fov_viewmodel`, override cvar `viewmodel_fov_override`. BMVR replaces this with HMD FOV in gameplay. **Confirmed from Ghidra** + **Confirmed from source**
- Attachment origins on guns are ~identity in bone space (`muzzle` at gun bone origin). Grip is **not** an attachment; BMVR uses hardcoded L4D2VR offsets. **Confirmed from asset inspection** + **Confirmed from source**
- `cl_viewmodel_lag` still runs inside original `CalcViewModelView`; BMVR then SetAbsOrigin/Angles. **Confirmed from Ghidra** + **Confirmed from source**

---

## Files / addresses (quick index)

| Item | Location |
|---|---|
| BMVR pose hook | `src/hooks.cpp` `dCalcViewModelView` |
| BMVR offsets / melee | `src/vr.cpp` `ResolveWeaponViewmodelPose`, `UpdateCrowbarMelee` |
| Weapon name | `src/game.cpp` `GetActiveWeaponModelName` |
| Hook RVAs | `src/offsets.h` `CalcViewModelView 0x29D930`, `GetViewModelFOV 0x216510` |
| CalcViewModelView | `client.dll` `FUN_1029d930` |
| SetAbsOrigin/Angles | `FUN_100af720` / `FUN_100af600` |
| GetViewModelFOV | `FUN_10216510` |
| DT_BaseViewModel | `FUN_1007c780` |
| Weapon DMX loader | `FUN_10259090` |
| Animevent table | `FUN_101354a0` |
| Steam models | `bms\bms_models_dir.vpk` `models/weapons/v_*.mdl` |
| Steam scripts | `bms\bms_misc_dir.vpk` `scripts/gameplay/weapons/*.dmx` |

---

## What this research did not do

- No HLMV GUI, no Crowbar decompile to QC, no live `m_nBody` poke, no server.dll `CWeapon_Crowbar::PrimaryAttack` walk.
- No attach to a running `bms.exe`.
- Exact client function that applies `vm_bodygroup_overrides` → `m_nBody`: **Unknown** (data contract is clear from the manifest + MDL).
