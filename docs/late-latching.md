# Late latching (SR)

What the SR weaver's late latching is, which backends actually implement it, what the API hides,
and how to verify it — including why the obvious verification is a trap.

Runtime-side latency levers (late weave, repaint, deferred present, queue tiering) are documented
in the runtime repo: `docs/reference/motion-to-photon-levers.md`. This file is the vendor half.

## Status: verified on D3D11, unverified on VK

**It works.** Measured quantitatively 2026-08-12 by LeiaSR on D3D11 through the v2 C99 API, with
an automated harness that weaves offscreen, reads pixels back and computes red/green centroid
separation — no eyeballs in the loop:

```
GPU held ~37 fps, frames-in-flight bounded ~6 by an event-query ring,
viewer tracked (eyeSep 63.7-64.1 mm, telemetry from inside the weaver) and moving

latch ON    mean 9.04 px   max 10.15 px   46/49 samples separated
            useWeaveShader=1, fif~6, never self-disabled
```

The OFF control read 0.00 px but its tracking state was not verified at the time, so it is not
being claimed — though structurally the latch-off path writes `DXYInitial = DXY` at record, so 0 is
forced. Tool: `C:\Libs\srprobe\latch_test.exe --load N --latch 0|1 --seconds S`; it self-validates
and prints `RUN INVALID` rather than a fake null when the viewer is not tracked.

**Vulkan remains unverified** — see the backend table and the author's statement below.

## What it does

Re-samples the eye position at submit time and patches the vertex buffer of frames **already
queued but not yet executed**. It therefore only has anything to patch when frames are genuinely
in flight — if the compositor waits on its submit fence before returning, nothing is queued and
the latch is a no-op regardless of how well it works.

## Per backend — from SR source, not inferred from testing

Testing cannot distinguish these, because an automatic backend and a silently-dead one both look
like "enable succeeded":

| Backend | Mechanism |
|---|---|
| **D3D11** | **Automatic.** Submits implicitly while recording, so the weaver places its own `D3D11_QUERY_EVENT` inside `weave()`. Enable only. |
| **GL** | **Automatic**, same shape via `glFenceSync`. |
| **VK** | **Needs a hook.** The app/compositor owns `vkQueueSubmit`, so the weaver cannot mark the frame in flight by itself → `srWeaverWeaveSubmittedVulkan` (dispatch slot 80). |
| **D3D12** | **Stub.** `{ /*Not implemented*/ }` upstream; `enable` returns `SR_SUCCESS` and does nothing. Do not call it. |

The split is the **submission model**, not an inconsistency: implicit submission lets the weaver
place its own marker, explicit submission cannot. D3D12 would need the same hook Vulkan got.

**On Vulkan specifically:** the weaver's author stated (2026-06-25, before the submit-hook entry
point existed) that *"late latching is just DX11 and OpenGL, no Vulkan."* The VK path is new and
unproven upstream — the most economical explanation for a VK null.

## What the API hides

`srWeaverIsLateLatchingEnabled` reports the **effective** flag rather than an echo of the enable —
the stubs return a hardcoded `false`, and a live backend clears the flag itself on failure. That
makes it worth asserting after enabling, and **again at the end of a run**: the device-lost path
(`VK_ERROR_DEVICE_LOST`) hard-disables and never re-enables, so a late measurement can be a null
by definition.

**But it is not a complete check.** Where the weaver declines for a documented reason it declines
**silently** and this still returns `true` — the API denies its own documented limitation. Read it
as "not obviously off", never as proof the latch runs.

### The two contract constraints, from the shipped public header

`LeiaSR-SDK-1.35.0.2011/include/sr/weaver/dx11weaver.h:118`:

> "Enables late latching. Note that late latching requires applications to call `weave()` once per
> frame, and does not work with deferred contexts."

**Deferred contexts — not a concern for us.** Both the D3D11 service compositor and the in-process
D3D11 path use the **immediate** context.

**Once per frame — worth watching.** The runtime's #868 repaint deliberately re-weaves outside the
app's frame loop, at display rate. DX11 self-disables at `dx11weaver.cpp:1378` with *"Exceeded
maximum frames in flight. Disabling late latching."*, and the comment there reads "probably the
user is doing multiple weaves per frame".

**We have never tripped it.** Zero occurrences of that string, or of `Invalid frame count for late
latching`, across all 2,665 logs on the reference box — and vendor lines are confirmed to reach the
DisplayXR log stream, so that is a real negative rather than a capture gap. Recorded because a
silent self-disable would look exactly like a working feature: **if repaint's cadence changes,
grep for both strings again.**

### THE lifecycle trap: a weaver created AFTER `srInitialize` has a dead eye pipeline

The weaver's internal predicting trackers are **senses**, and senses created after `srInitialize`
never start. The failure is total and completely silent:

```
getEyeSeparation()      0.0 forever (nominal, identical eyes)
isTracking              false forever
useWeaveShader          false -> the app BLITS with the "searching for user"
                        pulse animation and never weaves at all
late latching           gated off permanently
every API call          SR_SUCCESS
isLateLatchingEnabled   TRUE
```

Correct order — `sr_instance.h` documents it, nothing enforces it:

```
srCreateInstance -> srCreateWeaver* / srCreateEyeTracker + callbacks -> srInitialize
```

**`isLateLatchingEnabled` does not catch this.** It stays `true` while the latch is structurally
dead, which is precisely why "enable and assert the effective flag" is not a sufficient check.

**We are safe by construction, and it is worth knowing where that safety lives:**
`leia_sr_v2_create_instance` deliberately does **not** initialise (`leia_sr_v2_common.h`), and each
arm calls `leia_sr_v2_initialize` only after its weaver exists — see the ordering comments in
`leia_sr_d3d11.cpp` and `leia_sr_d3d12.cpp`, which mirror the identical v1 constraint. Because the
rule is enforced in the shared helper rather than by per-arm discipline, a new arm inherits it by
default. Keep it that way.

### Capability-probing trap

Weaver trampolines null-check the **handle before the dispatch slot**, so calling with a `NULL`
weaver returns `SR_ERROR_HANDLE_INVALID` whether the slot exists or not. "Call it and read the
error" is only a capability test **with a valid handle** — this produced a false "12 of 13 slots
present" against a runtime that had none of them.

## Verifying it — and why the obvious test lies

The debug overlay draws two eye-position dots: **red = record-time**, **green = late-latched**.
They sum, so **yellow = coincident**.

Three things must all be true or the test says nothing:

1. **Small dots.** `dotRadius = weavingPattern - 400 + 1` (`shader_weave_frag.glsl:221`). The
   widely-quoted `pattern = 450` therefore draws **51-pixel-radius** discs, which swallow a
   few-pixel offset entirely and read as one yellow blob. **Use `pattern = 405`** (radius 6) or
   `402` (radius 3).
2. **A moving viewer.** In `updateGeometry` (`vkweaver.cpp:1775`), `if (!async) vertex.DXYInitial =
   vertex.DXY` — green is drawn at `DXY`, red at `DXYInitial`, so the separation is exactly
   **`P(t1) − P(t0)`**, the *change* in predicted eye position between record and latch. **A
   stationary viewer cannot produce separation however well the latch works.** Move briskly,
   laterally, continuously.
3. **Real GPU load.** The effect scales with how far the CPU runs ahead of the GPU. At 60 fps with
   a light scene the dots coincide and a working latch is indistinguishable from a dead one.

`pattern` resolves through `player.ini` then `weaver.ini`, **weaver.ini winning**. `weaver.ini` has
no section header and is re-read live, so edits apply without a restart. Restore `pattern = 0`
afterwards.

### The bracket that makes a result trustworthy

Run on **D3D11 first** — it latches automatically, has no submit hook to get wrong, and no
fence-wait constraint holding frames out of flight:

```
pattern = 405, GPU loaded, viewer moving throughout

  D3D11, late latching ENABLED    -> expect SEPARATION   (positive control)
  D3D11, late latching DISABLED   -> expect YELLOW       (negative control)
```

A genuine off is `lateLatching=0` under the device section of **`player.ini`**
(`GetLateLatchingForceOff()`), which beats any API call. Only read VK results after that brackets.

### Do not build a load rig — one exists

A purpose-built demo was written in 2024 to prove late latching to customers: a modified
`example_directx11_weaving.exe` rendering a **torus knot whose polygon count is the GPU-load knob**
(`-SLICES`, `-STACKS` default 10000, `-RADIUS`, `-COLOR`), with the dots hacked into the weaver; a
later build adds an ImGui mesh-detail slider and a perf readout. Slack `#sw-sdk-late-latching-demo`
(`C07QRRU2HKM`).

Two things to know before hunting for it: the torus builds live on **personal OneDrive and are not
SharePoint-indexed**, so ask the LeiaSR side for a current link; and the 2024-10-29 build was
reported broken in March 2025 and was never integrated into SR, so a failure to launch is a known
state, not a new regression.

## Known-invalid prior result

The 2026-08-12 DisplayXR matrix (six configurations across VK and D3D11, all reading yellow) is
**invalid** and must not be cited: it used `pattern = 450` (51-pixel discs) and did not treat
viewer movement as the thing that creates the signal. Separately, the SR dev drop was re-cut
mid-session, so the last four runs straddled a module swap.

## Our wiring

`leiasr_enable_late_latching` **refuses** to enable on VK without the submit hook, so the dangerous
combination — enabled but never marked in flight — is unreachable. D3D12 is deliberately not
enabled, with the reason in the code. Standing decision (David, 2026-08-12): **keep the wiring; it
reports healthy and costs nothing.** The bar it must keep clearing is: no crash, no regression.

That decision was taken when the feature was entirely unproven. It now has a quantitative positive
result on D3D11 (above), so the D3D11 and GL arms are enabling something real. **Vulkan is still
unproven** — the author's "just DX11 and OpenGL" predates the submit hook, and our own VK runs
measured nothing (with an invalid instrument, so they establish neither direction).

Note also that the runtime's repaint may make late latching redundant *by construction* — it
re-weaves at display rate with a fresh eye pose, so the staleness late latching exists to remove is
already being removed every refresh.
