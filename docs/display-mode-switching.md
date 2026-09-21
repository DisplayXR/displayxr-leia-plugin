# Leia SR — Display Mode Switching (2D/3D)

How the Leia plug-in implements the neutral `xrRequestDisplayRenderingModeDXR` /
`xrRequestDisplayModeDXR` contract from
[`XR_DXR_display_info`](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/specs/extensions/XR_DXR_display_info.md). The extension spec stays
vendor-neutral: the runtime translates a mode request into the display processor's `set_property`
call, and the vendor SDK implements it as either a preference-based request (aggregated across
applications) or direct hardware control. This page documents the concrete Leia mechanism.

## Per-platform translation

| Platform | Runtime Implementation |
|---|---|
| Windows (SR SDK) | `SwitchableLensHint::enable()` / `SwitchableLensHint::disable()` — preference-based, aggregated across applications. |
| Android (CNSDK) | `leia_core_set_backlight(core, true)` / `leia_core_set_backlight(core, false)` — direct backlight control. |
| Linux (srSDK) | `srLensEnable` / `srLensDisable` on the process-wide SR context's lens handle — preference-based, and **owned** after the first call (see below). |

On Windows the SR SDK's `SwitchableLensHint` is a **preference**, not a hard set: the platform
may aggregate hints from multiple applications or defer the switch, which is why the OpenXR API
is shaped as a *request* (returning `XR_SUCCESS` on acceptance, not on physical completion).

On Android, `leia_core_set_backlight` is direct: enabling the backlight engages the lightfield
optics, disabling it returns the panel to conventional 2D.

## Hardware vs processing (runtime ADR-028, runtime#542)

`request_display_mode` is **hardware-only**: it drives the lens hint / backlight and nothing
else. The DP's atlas processing — weave vs flat-blit, and the mono eye-position centering —
follows the **per-frame atlas grid** the runtime hands to `process_atlas`
(`tile_columns × tile_rows > 1` ⇒ weave, `1×1` ⇒ flat blit), tracked as `ldp->view_count`.

The two channels are deliberately independent: the repurposed `xrRequestDisplayModeDXR`
(spec v15) overrides the hardware state for the current mode without changing it, so a
hardware-2D override over an active 3D mode keeps the weave running with the lens off — the
panel shows the woven atlas flat, and an app fading its parallax to zero converges back to a
sharp image (the MANUAL tracking-loss transition). Implemented in all four API variants
(PR #45); contract: runtime `docs/reference/xrt_plugin_iface.md` + ADR-028.

## Linux: who owns the lens preference

Applies to the srSDK backend (`src/drv_leia_linux/leia_sr_linux_sdk.c`) from LeiaSR #266 on.
The rules are code in `src/drv_leia_linux/leia_lens_owner_linux.h`, unit-tested by
`src/drv_leia_linux/tests/test_lens_owner_linux.c`.

### The SDK rule

SRService keeps **one lens preference per client connection**, i.e. per SR context, and the last
writer wins: `srCreateLens` and the weaver share the same `SwitchableLensHint`.

- **Before the application's first lens call the weaver owns it.** It votes the lens on at the
  first woven frame and withdraws the vote only after 500 ms continuously off the panel.
- **The first `srLensEnable` or `srLensDisable` on a context makes the preference the
  application's for the rest of that context's life** (`applicationOwnsPreference`). From then
  on the Linux weaver never writes it again: not on panel exit or re-entry, not after tracking
  loss, not from a weaver recreated on the same context. The weaver still picks woven 3D or
  plain 2D *output* every frame; only the lens preference has changed hands.
- **A new context starts over** with the weaver in charge and the flag cleared.

### What the plug-in does with it

The plug-in keeps one process-wide SR context, so "the application" is this process: the app, or
DisplayXR acting on its behalf.

| Situation | Lens call | Why |
|---|---|---|
| SR context brought up | none | A startup `srLensEnable` would take ownership at once and lose the weaver's automatic "lens off when the window leaves the panel", which is the Windows-parity behaviour for an untoggled session. |
| 3D requested, nothing has ever asked for 2D | none | The runtime asks for 3D at every `xrBeginSession`. The weaver lights the lens at its first woven frame anyway, so sending it would only give ownership away. |
| 2D requested | `srLensDisable` | Takes ownership. Deliberately: nothing else could turn the lens off. |
| 3D requested after any 2D | `srLensEnable` | We own the lens now; the weaver will not turn it back on. |
| New SR context (first creation, retry after a failed bring-up, or replacement of a context invalidated by an SRService restart) | the last call sent, if any | So "the application took control" survives the restart. With nothing sent yet, nothing is re-applied and the new context's weaver owns the lens. |

### The contract every caller must keep

**After its first explicit call, DisplayXR owns the lens for the rest of the context, and nobody
else will ever change it.** So every path that asks for 2D must pair with a path that asks for
the previous state back when its reason clears, and a runtime-side degrade must restore the
**app's own last choice**, not force 3D (an app that chose 2D must stay 2D). The runtime's audit
of those paths is in `docs/specs/vendor/lens-preference-ownership.md` in displayxr-runtime.

### Context loss

`SR_EVENT_TYPE_CONTEXT_INVALID` means the connection to SRService is gone (for example, the
service restarted); the SDK does not reconnect the instance. The plug-in latches it and replaces
the context on the next `sr_ctx_ensure` call once **no weaver is alive** on the dead one, which in
practice is the next DP creation. The dead context is abandoned, not destroyed, because
`srDestroyInstance` joins SDK threads without bound when the service died under it. Callbacks from
it are fenced off by a context generation tag. A session that is running when the context dies
keeps the dead context (no eye tracking, no lens control) until it ends; re-creating a live
weaver on a new context is not implemented.

### Known consequences

- An app that toggles to 2D and back owns the lens from then on, so the weaver's off-panel
  release no longer applies to it for the rest of the process. On Wayland the runtime's own
  refuse-rather-than-resample gate asks for 2D when the surface leaves the panel and restores
  the app's choice when it returns, which covers that case. On X11 nothing does.
- `xrEndSession` asks for 2D, so a process that ends a session and begins another one owns the
  lens in the second session.
