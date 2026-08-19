# L4D2VR motion melee

Primary source: vendored `third_party/l4d2vr/L4D2VR`. Upstream: https://github.com/keyou91/l4d2vr.

Evidence tags: **Confirmed from source** / **Strong inference** / **Unknown**.

This is the **actual** L4D2VR melee stack, not a generic “swing fast → damage” design. Several similarly named systems do **not** deal melee damage.

---

## 1. Which system is which

**Confirmed from source.** Four separate mechanisms:

| Name in code | What it actually does | Deals melee damage? |
|---|---|---|
| **VR-aware encoded usercmd + server 10-trace fan** | If right-controller **tracking** speed `> 1.1` while `IN_ATTACK` is packed, server interpolates controller yaw/pitch across the command and calls vanilla `TestMeleeSwingCollision` 10 times. | **Yes** (vanilla collision/damage) |
| **Non-VR server gesture → `IN_ATTACK`** | `ForceNonVRServerMovement` only. Horizontal controller speed vs a threshold (default **999 = off**) synthesizes attack + aim lock. Vanilla server melee then runs. | **Yes**, but only if threshold is lowered and this mode is on |
| **`UpdateMotionGestures`** | Left-hand outward speed → **`+attack2` shove**; right-hand downward speed → **`+reload`**; HMD up → **jump**. | **No** (shove/reload/jump) |
| **Effective-range melee fan + AutoFastMelee** | Client targeting assist / hold-to-pulse / slot cancel. Uses eye traces, not the VR swing hull. | Indirectly, by pressing vanilla attack |

`dDoMeleeSwingServer` / `dStartMeleeSwingServer` / `dGetPrimaryAttackActivity` detours are **pass-throughs** (`hooks_misc.inl` 22968–23019). They do not implement motion melee.

---

## 2. VR-aware server motion melee (the real algorithm)

This is the path when `m_EncodeVRUsercmd && !m_ForceNonVRServerMovement` (defaults: encode **true**, force-non-VR **false** unless the user toggles it).

### 2.1 Client: pose packing and “swing in motion” flag

**Confirmed from source.** `Hooks::dWriteUsercmd` (`hooks/hooks_combat_network.inl` 2321–2458).

1. `BuildEncodedVRUsercmdControllerPose` (`hooks.cpp` 678–718):
   - Position: `GetRightControllerViewmodelAbsPos()` (usually `m_RightControllerPosAbs`), or aim-line start if an aim line exists.
   - Angles: `GetRightControllerAbsAngle()`.
   - **If melee is active, do not replace angles with aim-line direction** (`695–716`). Melee uses the **controller orientation**, not the bullet converge ray.
2. `tick_count *= -1` → “this cmd has VR payload.”
3. Pack:
   - `mousedx = (int)(controllerAngles.x * 10)`  // pitch, 0.1°
   - `mousedy = (int)(controllerAngles.y * 10)`  // yaw
   - `command_number += ((int)angles.z + 180) / 2 * 10_000_000`  // roll, 2° steps
   - `viewangles.z = controllerPos.x`, `upmove = controllerPos.y`
   - `viewangles.x` encodes original pitch together with `controllerPos.z`
4. **Swing bit:**

```cpp
if (VectorLength(m_VR->m_RightControllerPose.TrackedDeviceVel) > 1.1f)
    to->command_number *= -1; // Signal to server that melee swing in motion
```

**Confirmed from source** (`2426–2428`).

Constants:

- Threshold **`1.1`** — OpenVR `TrackedDeviceVel` magnitude. Comment in `vr.h` for the *other* melee path says “m/s-ish in tracking space.” **Strong inference:** same space here (OpenVR tracking meters/second), **not** Source units.
- No hysteresis, no acceleration, no pose-history buffer on this flag. It is **instantaneous speed** on this usercmd.
- Requires `IN_ATTACK` to be in `to->buttons` for the negative-`command_number` signal to matter together with melee; the signal is written whenever speed > 1.1 even for guns, but the server only *traces* if `WeaponID == 19` (MELEE) and `isMeleeing`.

**Coordinate space of velocity:** `GetPoseData` remaps OpenVR without `m_VRScale` (`vr_lifecycle_update.inl` 4810–4824):

```text
pos = (-mat.m[2][3], -mat.m[0][3], mat.m[1][3])
vel = (-vVelocity.z, -vVelocity.x, vVelocity.y)
```

So `|TrackedDeviceVel| > 1.1` is **tracking meters/second**, not Source units. Stick-yaw pivot and −45° gun pitch are **not** applied to this speed test. Motion gestures also use this **raw** `TrackedDevicePos`, not `m_ControllerSmoothing`.

After write, the local cmd is restored so prediction CRC stays valid (`2446–2458`).

### 2.2 Server: decode

**Confirmed from source.** `dReadUsercmd` (`1892–1940`):

```text
if tick_count < 0:  isUsingVR = true; tick_count = -tick_count
if command_number < 0: isMeleeing = true; command_number = -command_number
else: isMeleeing = false
controllerAngle.x = mousedx / 10
controllerAngle.y = mousedy / 10
controllerPos.x = viewangles.z
controllerPos.y = upmove
controllerAngle.z = (rollEncoding * 2) - 180
```

Stored on `Game::m_PlayersVRInfo[playerIndex]` (`game.h` 119–129): `controllerAngle`, `prevControllerAngle`, `isMeleeing`, `isNewSwing`.

### 2.3 Server: 10-trace Rodrigues fan

**Confirmed from source.** After `ProcessUsercmds` original returns (`hooks_combat_network.inl` 1683–1774). Runs **every processed usercmd** while `isUsingVR && isMeleeing`.

Preconditions:

- Valid player index.
- Active weapon `GetWeaponID() == 19` (`MELEE` in `sdk.h` 1558).
- `isNewSwing` true → `entitiesHitThisSwing = 0`, then `isNewSwing = false`.
- If the player is **not** in the meleeing branch, `isNewSwing` is set back to **true** (`1766–1768`). So a gap in `isMeleeing` (speed dropped ≤ 1.1 or attack released so the negative command_number stopped) **re-arms** the per-swing hit list.

Geometry (controller angles from **this** cmd vs **previous** cmd):

```text
AngleVectors(prevControllerAngle) → initialForward, initialRight, initialUp
AngleVectors(controllerAngle)     → finalForward,   finalRight,   finalUp

initialDir = normalize( Rodrigues(initialForward, initialRight, +50°) )
finalDir   = normalize( Rodrigues(finalForward,   finalRight,   +50°) )

swingDot   = clamp(dot(initialDir, finalDir), -1, 1)
pivot      = normalize(initialDir × finalDir)
if |pivot| ≈ 0:
    if swingDot > -0.999: abort   // nearly parallel, no plane
    else pivot = initialUp        // 180° flip fallback
swingAngle = acos(swingDot) * 180/π
if swingAngle not finite or ≤ 0.01°: abort

Call original GetPrimaryAttackActivity(weapon, meleeInfo)   // required by L4D2 before traces
m_PerformingMelee = true
dir = initialDir
for i in 0 .. 9:                             // numTraces = 10
    dir = Rodrigues(dir, pivot, swingAngle / 10)
    TestMeleeSwingCollision(weapon, dir)     // original server function
m_PerformingMelee = false
if any collisionResult != 0 or entitiesHitThisSwing increased:
    NotifyMeleeHitConfirmed() for local player   // haptics only
prevControllerAngle = controllerAngle
```

**Constants (hardcoded, not cvars):**

| Symbol | Value | Role |
|---|---|---|
| blade pitch | **50°** about controller right | Tilts “forward” toward the melee blade / swing plane |
| traces | **10** | Discrete samples along the shortest rotation from prev to current |
| min angle | **0.01°** | Ignore jitter |
| antiparallel epsilon | **dot ≤ −0.999** | Allow 180° with `initialUp` as pivot |
| velocity gate (client) | **1.1** | Must be swinging this cmd |

**Trace origin / shape / mask:** L4D2VR passes **only a direction** into `CTerrorMeleeWeapon::TestMeleeSwingCollision(Vector const& vec)`. Origin, hull, length, and damage are **vanilla L4D2**. L4D2VR does not rewrite the start point to the hand or GLB.

**Unknown:** exact vanilla hull vs ray, length, and damage table inside `server.dll` `TestMeleeSwingServer` (`offsets.h` 0x3E79E0). Would need Ghidra on `server.dll`; not in this plugin.

**Repeated-hit prevention:** vanilla `Server_WeaponCSBase::entitiesHitThisSwing` at **0x17F0** (`sdk/sdk_server.h` 420). Reset once per new swing when `isNewSwing` fires. The 10 traces in one cmd share that counter, so the same entity is not awarded 10 times **if** vanilla uses that field that way. **Strong inference:** that is why L4D2VR zeroes it at swing start. Cross-cmd: as long as `isMeleeing` stays true (speed stays > 1.1 and attack packed), `isNewSwing` stays false, so the counter is **not** reset — a long slash can keep hitting **new** entities but not re-hit the same ones. **Unknown:** vanilla expiry of that list at swing end / next `StartMeleeSwing`.

**Damage:** L4D2VR does not apply `TakeDamage`. `TestMeleeSwingCollision` original does. Haptics: `NotifyMeleeHitConfirmed` (`vr_aiming.inl` 4302–4320) debounce **0.05 s** any hit, **0.20 s** same `entityTag`; profile `{0.035 s, 95 Hz, 0.72 amp}`. Also game events via `HandleMeleeHitHapticsGameEvent`.

**Frequency:** once per **server usercmd** (~client tick, typically 30–100 Hz depending on host), **10 traces each**, only while the 1.1 m/s bit is set. Not once per rendered VR frame.

**Relationship to hand/weapon meshes:** none. Direction is controller orientation + 50° about local right, interpolated from previous packed angles. GLB gloves and viewmodel bone positions are **not** the hit geometry.

`m_PerformingMelee` is set around the fan; `hooks_misc.inl` 23206 checks it (other code can know a VR fan is in progress).

---

## 3. Non-VR server path (`ForceNonVRServerMovement`)

**Confirmed from source.** Comment at `hooks_createmove.inl` 11 and gate `treatServerAsNonVR = m_VR->m_ForceNonVRServerMovement` (251). Encoded VR cmds are **not** sent (`canEncode` false). Vanilla `TestMeleeSwingCollision` uses **viewangles**.

Inside `if (treatServerAsNonVR)` and `m_IsMeleeWeaponActive` (`584–739`):

### 3.1 Velocity

```text
relVel = RightControllerPose.TrackedDeviceVel - HmdPose.TrackedDeviceVel
dt = flInputSampleTime if > 0.0001 else 0.011111   // ~90 Hz fallback
if have previous sample:
    derived = ((ctrlPos - prevCtrl) - (hmdPos - prevHmd)) / dt
    if |relVel| < 0.01 and |derived| > 0.01:
        relVel = derived
swingVel = relVel; swingVel.z = 0     // ignore vertical
v = |swingVel|
angV = |TrackedDeviceAngVel|          // deg/s, tracking space
```

**No low-pass filter** other than “prefer hardware vel, else finite difference.” HMD velocity is subtracted so walking/turning the head is less likely to count as a swing.

### 3.2 Thresholds, hysteresis, timing (cvars; defaults from `vr.h` 2726–2735)

| Cvar | Default | Meaning |
|---|---|---|
| `NonVRMeleeSwingThreshold` | **999.0** | Linear speed to trigger. **999 disables** the gesture. |
| `NonVRMeleeHysteresis` | **0.60** | Re-arm when `v < threshold * 0.60` (and angVel similarly) |
| `NonVRMeleeAngVelThreshold` | **0.0** | Wrist-flick; **0 = disabled** |
| `NonVRMeleeSwingCooldown` | **0.30 s** (min applied 0.05) | One gesture → one swing |
| `NonVRMeleeAttackDelay` | **0.04 s** | Wind-up before `IN_ATTACK` |
| `NonVRMeleeHoldTime` | **0.06 s** | Hold `IN_ATTACK`; ticks = clamp(ceil(hold/dt), 1, **8**) |
| `NonVRMeleeAimLockTime` | **0.12 s** | Freeze `cmd->viewangles` after fire |
| `NonVRMeleeSwingDirBlend` | **0.0** | 0..1 blend locked aim toward `VectorAngles(swingVel)` |

Trigger:

```text
above = (thr > 0 && v > thr) || (angThr > 0 && angV > angThr)
below = (thr <= 0 || v < thr*hyst) && (angThr <= 0 || angV < angThr*hyst)
if below: armed = true
if above && armed && !pending && now >= cooldown:
    armed = false
    start cooldown
    if delayT > 0: pending fire
    else: IN_ATTACK now + lock angles
```

Leaving melee clears pending/lock so it cannot ghost-swing.

**Default 999 means this algorithm is off** in the sample config unless the user lowers the threshold. The VR-aware 1.1 / 10-trace path is the one that is on for local/listenserver with encode enabled.

---

## 4. Motion gestures — not melee

**Confirmed from source.** Entire function `VR::UpdateMotionGestures` (`vr_tracking.inl` 2580–2674), called at the end of `UpdateTracking` (render/update rate, not usercmd rate).

Pose history: **one previous sample** of `TrackedDevicePos` for left, right, HMD. First call only stores. `deltaSeconds` from `steady_clock`. If `dt ≤ 0`, refresh prev and return.

```text
leftDelta  = leftPos  - prevLeft
rightDelta = rightPos - prevRight
hmdDelta   = hmdPos   - prevHmd

rightDownSpeed = (-rightDelta.z) / dt
hmdVerticalSpeed = hmdDelta.z / dt

leftForwardHoriz = normalize((LeftControllerForward.x, LeftControllerForward.y, 0))
leftOutwardSpeed = max(0, dot(leftDelta, leftForwardHoriz)) / dt
                 or |leftDelta.xy| / dt if |leftForwardHoriz| ≤ 0.01
```

Uses **already −45°-pitched** `m_LeftControllerForward` (off-hand calibration), in **Source world** for the axis, but positions are **tracking-local** `TrackedDevicePos`. Mixing those spaces is what the code does.

| Gesture | Condition | Action | Defaults |
|---|---|---|---|
| “Swing” | `leftOutwardSpeed >= MotionGestureSwingThreshold` | hold **`+attack2`** (`m_SecondaryAttackGestureHoldUntil`) | thresh **2.0**, hold **0.2 s**, cooldown **0.8 s** |
| “Down swing” | `rightDownSpeed >= MotionGestureDownSwingThreshold` | hold **`+reload`** | thresh **2.0** |
| Jump | on ground (`m_hGroundEntity != -1`) and `hmdVerticalSpeed >= JumpThreshold` | `+jump` | thresh **2.0** |

Disabled category → that threshold forced to **999** (`vr_viewmodel_config.inl` 3000–3007).

`MotionGesturePushThreshold` (**1.5**) is **loaded and can be forced to 999**, but **`UpdateMotionGestures` never reads it**. **Confirmed from source:** dead cvar / leftover.

Consumption (`vr_process_input.inl` 597–601, 750, 1619–1669): `ClientCmd_Unrestricted("+attack2")` / `+reload` / `+jump` while `now < HoldUntil`. Inventory quick-switch **cancels** the reload gesture.

**Do not copy this as melee.** A left-hand outward flick is L4D2 **shove**.

---

## 5. Client collision hook (haptics only)

**Confirmed from source.**

```cpp
dTestMeleeSwingCollisionClient: original + NotifyLocalMeleeCollisionHaptics(false, ...)
dTestMeleeSwingCollisionServer: original + haptics if entitiesHitThisSwing increased
```

(`hooks_misc.inl` 22951–22966, `hooks.cpp` `NotifyLocalMeleeCollisionHaptics`.) No direction rewrite on the client hook. Client prediction still uses whatever direction **vanilla client** `TestMeleeSwingCollision` was called with (usually viewangles-based), which can disagree with the server fan. **Strong inference:** predicted client hit effects may not match the VR fan; server is authoritative for damage.

---

## 6. Auto fast-melee and effective-range fan

### 6.1 AutoFastMelee

**Confirmed from source.** `hooks_createmove.inl` 1269–1373. Default **false**.

While holding attack on `WeaponID::MELEE`:

```text
+attack immediately
wait 1 tick; ClientCmd "slot1"
wait 1 tick; ClientCmd "slot2"
wait 15 ticks
repeat
intent grace 24 ticks
```

Purpose: cancel L4D2 melee recovery by switching away and back. Optional `AutoFastMeleeUseWeaponSwitch` (default true). Config keys `AutoFastMeleePushWaitTicks` / `AutoFastMeleePostWaitTicks` exist in the overlay sample but are **not loaded** — the waits above are hardcoded.

This is **not** motion detection.

### 6.2 Effective-range melee fan (aim assist)

**Confirmed from source.** `VR::TryFindEffectiveAttackRangeMeleeFanTarget` (`vr_aiming.inl` 3007–3102).

Defaults (`vr.h` 1034–1036):

- `m_EffectiveAttackRangeMeleeDistance = 70` Source units
- `m_EffectiveAttackRangeMeleeFanAngle = 90°`
- `m_EffectiveAttackRangeMeleeAutoFastMeleeIntervalSeconds = 0.60`

Algorithm:

```text
forward = normalize((end - start).xy)     // planar
minDot = cos(fanAngle/2)
start = localPlayer->EyePosition()        // NOT the hand
for each entity:
    skip team 2 (survivors) and team 0
    targetPos = origin + (0,0,36)
    planarDistance = |(target - start).xy|
    if distance > maxDistance or dot(forward, dir) < minDot: skip
    Ray from EyePosition to targetPos, STANDARD_TRACE_MASK
    pick closest unobstructed IsEffectiveAttackRangeTarget
```

Used to tint/lock the aim line and to pulse `IN_ATTACK` for effective-range auto-fire. **Origin is the eye.** Not the motion-melee hit shape.

Third-person melee: `CreateMove` can steer `viewangles` toward `m_AimConvergePoint` (`hooks_createmove.inl` 566–581) so vanilla traces follow the reticle.

---

## 7. Viewmodel / animation during melee

**Confirmed from source.**

- Melee uses the same uncoupled `CalcViewModelView` as guns, with **melee-name offsets** (weapons doc).
- `hideArms` includes `m_IsMeleeWeaponActive && !NativeViewmodelHandsOnly` so the huge FP swing arms disappear unless native-hands IK is the chosen hand system.
- Sequences still play (`ACT` from `GetPrimaryAttackActivity`). VR fan does **not** drive the studio swing pose; the pose is still the canned melee animation around the controller root.
- Two-hand virtual stock is **disabled** while melee is active (`vr_tracking.inl` 340–341, 381–382).

---

## 8. Copy-paste constant sheet

VR-aware (encode) path — **use this** for “L4D2VR motion melee”:

```text
TRACKING_VEL_GATE          = 1.1          // |OpenVR TrackedDeviceVel|
BLADE_PITCH_DEG            = 50           // about controller right
NUM_TRACES                 = 10
MIN_SWING_ANGLE_DEG        = 0.01
ANTIPARALLEL_DOT           = -0.999
WEAPON_ID_MELEE            = 19
entitiesHitThisSwing       @ server weapon + 0x17F0
```

Non-VR server path (off by default):

```text
NonVRMeleeSwingThreshold   = 999.0        // disabled
NonVRMeleeHysteresis       = 0.60
NonVRMeleeSwingCooldown    = 0.30 s
NonVRMeleeHoldTime         = 0.06 s
NonVRMeleeAttackDelay      = 0.04 s
NonVRMeleeAimLockTime      = 0.12 s
NonVRMeleeAngVelThreshold  = 0.0          // off
NonVRMeleeSwingDirBlend    = 0.0
holdTicks                  = clamp(ceil(holdTime/dt), 1, 8)
dt_fallback                = 0.011111 s
vertical ignored           = yes (swingVel.z = 0)
HMD vel subtracted         = yes
```

Gestures (not melee damage):

```text
MotionGestureSwingThreshold      = 2.0    // left outward → shove
MotionGestureDownSwingThreshold  = 2.0    // right down → reload
MotionGestureJumpThreshold       = 2.0
MotionGestureCooldown            = 0.8 s
MotionGestureHoldDuration        = 0.2 s
MotionGesturePushThreshold       = 1.5    // UNUSED in UpdateMotionGestures
```

---

## 9. L4D2-specific vs reusable

**Reusable ideas:**

- Gate extra traces on **controller tracking speed**, not viewangles speed.
- Interpolate **previous vs current controller orientation**, not a single ray along the gun.
- Sample N directions with Rodrigues about `prev × curr`.
- Keep **vanilla** melee damage/hull; only replace the **direction**.
- Reset a per-swing hit set when a new slash starts.
- Do not use “left hand flick” as melee if that bind is shove in the source game.

**L4D2-specific:**

- `WeaponID == 19` melee + `GetMeleeWeaponInfo` + map melee names.
- Encoded `CUserCmd` packing (negative tick/command_number). Black Mesa will not decode this unless BMVR implements both ends.
- `TestMeleeSwingCollision(Vector const&)` signature and `entitiesHitThisSwing`.
- Shove is `IN_ATTACK2`; HL2/BM crowbar is typically `IN_ATTACK` only.
- AutoFastMelee `slot1`/`slot2` cancel.
- Effective-range survivor-team skip (`team == 2`).

**Unknown / must re-verify on BM:** crowbar `TraceRay` vs L4D2 melee hull; whether BM has an equivalent of `entitiesHitThisSwing`; whether a listen server can run a ProcessUsercmds-style fan without the encoded usercmd protocol.

---

## 10. BMVR today (read-only note)

`docs/L4D2VR-MAP.md`: melee is listed as L4D2-only, not ported as the encoded fan.

`src/vr.cpp` has a **crowbar melee fan** log path (`Crowbar melee fan hit`) and `src/bmvr_flags.cpp` crash-sticky `melee_trace` (“crowbar TraceRay origin rewrite”). That is a **client TraceRay origin** experiment, not a copy of the 1.1 / 50° / 10-trace server algorithm above. Do not assume BMVR already has L4D2VR motion melee.
