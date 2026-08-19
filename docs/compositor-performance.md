# Compositor / present stalls (2026-08-19)

## Symptom

Intermittent or **constant** severe HMD stutter (~10–15 fps perceived) while the **desktop FPS counter stays high**. Removing the headset clears stalls in some sessions.

## Log evidence

From `bmvr_log.txt` during glock / combat sessions:

- `present tick ~11–14fps` coincides with repeated `Pose waiter WaitGetPoses err=0 dt=110ms`
- Crowbar swings at ~115 fps did not always trigger the same pattern
- No DXVK shader-compile line correlated with the stall window

## Root cause (2026-08-19 regression)

**ApplicationPerformsPostPresentHandoff** requires `PostPresentHandoff()` every frame. A prior mitigation gated handoff on `poseAge ≤ 35 ms` and unchanged compositor frame index. The pose waiter often delivers poses older than 35 ms, so handoff was **skipped almost every frame** → compositor starvation → constant HMD stutter with fine desktop FPS.

## Fix (current default)

| Setting | Default | Effect |
|---|---|---|
| `CompositorPostPresentHandoff=false` | **false** | `VRCompositorTimingMode_Explicit_RuntimePerformsPostPresentHandoff` — runtime handles handoff |
| App `PostPresentHandoff()` | Skipped | `TryCompositorPostPresentHandoff` is a no-op unless config forces app mode |

Set `CompositorPostPresentHandoff=true` only to A/B test L4D2VR-style app handoff (unconditional every frame, no poseAge gating).

## Instrumentation retained

- `Present stall interval=…` when Present gap ≥ 50 ms (rate-limited)
- Slow handoff log when app mode enabled and `dt ≥ 50 ms`

## What this is not

- Not fixed by hiding weapons or disabling VR input
- Not proven as DXVK pipeline compile (no log correlation yet)
- Not solved by skipping `SendWeaponAnim`

## Next verification

1. HMD smoothness after runtime timing mode (default)
2. If stalls return, grep log for `WaitGetPoses dt=110` vs `Submit()` blocking
3. App handoff A/B: `CompositorPostPresentHandoff=true` in config next to `d3d9.dll`
