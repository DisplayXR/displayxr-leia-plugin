# The untracked fallback, and how to tell which SDK shader drew a bad frame

When the SR SDK loses the viewer, the **SDK**, not this plug-in, decides what the panel shows. This page records that decision tree, the one place it has bitten us (#178), and the two diagnostic techniques that found the cause — both of which generalise to any vendor whose weaver is a closed per-frame call.

## Who owns the untracked frame

Nothing in `src/drv_leia/` participates. `leia_dp_*_process_atlas` hands the atlas to `weave()` and the SDK picks one of three behaviours, from the resolved config key `show_left_view_when_not_tracking`:

| Config value | SDK behaviour | Shader that draws |
|---|---|---|
| `off` | keep weaving from a default viewing position | the **weave** shader |
| `on` | show the left view, flat | the **blit** shader, pulse branch off |
| `on with shader` | show the left view, dimmed, with a rotating "searching" glow | the **blit** shader, **pulse branch on** |

The SDK's own tracking test is `getEyeSeparation() > 1.0f` — an inter-eye distance in millimetres, taken from **its** predicting eye tracker.

### Trap: that is not the tracker the DP reads

`leia_dp_*_get_predicted_eye_positions` returns the **weaver** tracker's pair. The SDK's untracked branch is decided by a **different object** with different state and a different no-data fallback. They disagree routinely:

- the weaver tracker's no-data fallback is a fixed constant — a pair collapsed onto `(0, 100, 600)` mm, which the runtime sees as a motionless `L == R` and reports as `is_tracking = false`;
- the eye tracker meanwhile cycles in and out of "has enough samples to predict", so the SDK flips between weaving and the pulse underneath us.

**So "the eye positions the runtime sees never changed" is not evidence that the SDK's tracking state never changed.** In #178 that inference cost real time: the runtime correctly reported an invariant collapsed pair for the whole session while the SDK was switching shaders every two seconds. If you need to know which branch the SDK is in, do not infer it from the DP's eye output — read it off the pixels (below).

## Reading a bad frame's alpha to identify the shader

The SDK's shaders differ in what they write to **alpha**, and that turns any captured frame into a fingerprint of the branch that drew it:

| Alpha in the captured frame | Branch that drew it |
|---|---|
| `255` (1.0), forced | the **weave** shader — it returns `float4(weaveResult, 1.0f)` unconditionally |
| the source atlas's alpha, unchanged | the **blit** shader, pulse **off** |
| the source atlas's alpha **× 0.8** — e.g. `204` for an opaque atlas | the **blit** shader, pulse **on** (the pulse dims by multiplying all four components) |
| unchanged from whatever the compositor cleared to | the weaver drew **nothing** |

This is how #178 was pinned without a debugger or a GPU capture: the black frames were uniformly `(0, 0, 0, 204)`. `204 = 0.8 × 255` said "pulse branch, and its texture fetch succeeded and returned an opaque texel" — which simultaneously identified the failing pass and ruled out the whole family of "the DP was handed a bad or unbindable input" theories that the black RGB otherwise suggests.

Capture a frame with the runtime's `DXR_WEAVE_PROBE=1` plus a `%TEMP%\dxr_woven_trigger` file (in-process), or `DXR_SPLIT_COVER_DIAG` (service path). Then read the actual channel values — do not eyeball the PNG. An image viewer composites a transparent frame over its own backdrop, so `(0,0,0,0)` and `(0,0,0,204)` and mid-grey all look identical on screen, and only the numbers separate them.

## Sanity-check the shader before blaming the device

The second technique: re-implement the suspect shader's arithmetic in a few lines of Python and ask what output it is *capable* of. For #178 that settled it immediately — a correctly executed pulse frame has a minimum channel value of ~0.037 and a bright rim glow, so it is mathematically incapable of being uniformly zero over a non-black atlas. That turns "the frame looks wrong" into "this pass did not execute as written", which is a different and much smaller search.

## #178: the fault this found

Symptom: with the runtime weaving on the scanout adapter (`DXR_WEAVE_ON_SCANOUT=1`) and no viewer, the panel showed ~22 black frames every ~2 s.

Cause, and it is **SDK-side, not in this plug-in**: the pulse branch is handed an animation clock of seconds-since-boot and feeds it straight into `sin`/`cos`/`fmod`. After a few days of uptime that argument is far outside the range some GPUs' trigonometric instructions define a result for; the pass produces NaN, and a UNORM render target stores NaN as 0. The frame goes black while its alpha keeps the pulse's 0.8 dim — which is exactly the fingerprint above. Tracked frames were unaffected because the weave shader never touches that clock.

The two-second cadence is not the bug — it is just how often the SDK's eye predictor rebuilds its filter history and briefly reports a zero inter-eye distance, dropping into the pulse branch. Every pulse frame was black; only ~18% of frames were pulse frames.

Fixed in the SDK: **[LeiaSR#189](https://github.com/LeiaInc/LeiaSR/issues/189)**. Tracked here as [#178](https://github.com/DisplayXR/displayxr-leia-plugin/issues/178), runtime side [DisplayXR/displayxr-runtime#1134](https://github.com/DisplayXR/displayxr-runtime/issues/1134).

**Mitigation without an SDK update**, if you hit this on a box you cannot upgrade: set `show_left_view_when_not_tracking = on` in the device's `sr_config.ini`. That selects the plain-blit branch, which never touches the animation clock. It costs the searching animation, so it is a workaround, not the fix.

## What this plug-in should *not* do about it

Detecting "untracked" here and drawing our own fallback instead of calling `weave()` was considered and rejected. Our tracking signal comes from the other tracker (see the trap above), so it cannot reliably shadow the SDK's branch; it would delete a product behaviour the SDK owns; and it puts vendor presentation policy inside the display processor, which is what ADR-019 exists to prevent. Untracked presentation is the SDK's job, and a fault in it is an SDK fix.
