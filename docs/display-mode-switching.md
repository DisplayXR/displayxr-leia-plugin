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
| macOS (srSDK, `drv_leia_macos`) | `srLensEnable` / `srLensDisable` on the DP's lens handle (`leia_dp_mac_request_display_mode`), under the **same ownership rules as Linux** (`leia_lens_owner_linux.h`): a 3D request before any 2D request sends nothing (the weaver raises the lens itself), and the first 2D request takes ownership. An EXTERNAL (screen-bound) weaver never votes the lens, so its 3D request is always sent. |

## Smooth 2D↔3D transitions are app-side, on every platform

Neither this plug-in nor the runtime eases a mode switch. The lens call happens on the frame
the runtime applies the mode, and from that frame the DP weaves (2×1 atlas) or flat-blits (1×1
atlas). No arm has a weaver fade, a lens-settle hold or a cross-fade: not Windows, Android,
Linux or macOS. The soft transition users see on Windows comes from the **app**. It ramps the
view rig's `ipdFactor` to 0 *before* requesting 2D, and requests 3D *before* ramping the
disparity back up, so the lens always switches on flat content. Apps do this with
`dxr::ModeSwitch` from
[displayxr-common](https://github.com/DisplayXR/displayxr-common) (`common/mode_switch.h`;
README *Smooth 2D↔3D transitions*). An app that requests modes directly snaps on every arm,
and changing this plug-in would not fix that. Diagnose a "harsh 2D/3D switch" in the app
first: displayxr-runtime's app linter flags it as INV-2.9.

On Windows the SR SDK's `SwitchableLensHint` is a **preference**, not a hard set: the platform
may aggregate hints from multiple applications or defer the switch, which is why the OpenXR API
is shaped as a *request* (returning `XR_SUCCESS` on acceptance, not on physical completion).

On Android, `leia_core_set_backlight` is direct: enabling the backlight engages the lightfield
optics, disabling it returns the panel to conventional 2D.

## Rendering modes; "LeiaSR multi-viewer" is a second 3D mode (Windows)

Apps always see two rendering modes: **mode 0 = 2D** (one view, 1×1, lens off) and **mode 1 =
3D** (two views, 2×1). That is the runtime's two-slot contract (runtime ADR-065). Behind it, a
display driver declares a *list* of entries, and the runtime maps the 3D slot to the entry the
screen's **3D mode** names (runtime ADR-066 C1).

The Leia plug-in declares:

| Entry | Name | Views | Weaver multi-viewer override | Declared |
|---|---|---|---|---|
| 0 | `2D` | 1 | (lens off) | always |
| 1 | `LeiaSR` | 2 | none (follows the LeiaSR system setting), or `OFF` with `MULTI_VIEWER=off` | always |
| 2 | `LeiaSR multi-viewer` | 2 | `SR_MULTI_VIEWER_MODE_ON` | Windows, when the LeiaSR runtime can weave for several viewers |

Entry 2 is the same atlas as entry 1 (views, scale, tiles, lens, tracking). Only the SR v2
weaver's option differs, so the runtime switches between the two **live**, under running apps,
with no swapchain change.

- **When entry 2 is declared.** At each Leia device creation, when all three hold: the plug-in
  was built against an SR SDK with `srWeaverSetMultiViewerMode` (`DXR_LEIA_HAS_SR_MULTI_VIEWER`),
  the installed LeiaSR runtime reports `multiViewerWeave`, and the plug-in was built against
  runtime headers with the declared-list contract (v2.70.7 or later). Otherwise the list is
  entries 0 and 1, exactly as before. Linux, macOS and Android declare entries 0 and 1.
- **Choosing it.** The DisplayXR Dashboard shows a "3D mode" picker on the screen's card (only
  when the list has more than one 3D entry), or:

  ```bat
  displayxr-cli dp mode list                                       REM each screen's choices
  displayxr-cli dp mode set leiasr_multi_viewer --screen <key>     REM applies now
  displayxr-cli dp mode set leiasr --screen <key>
  displayxr-cli dp mode reset --screen <key>                       REM back to the default
  ```

  The choice names are the runtime's slugs of the entry names: `leiasr`, `leiasr_multi_viewer`.
  The choice is stored by the runtime, per screen.
- **Which graphics APIs.** The SR **D3D11** and **Vulkan** weavers weave for several viewers.
  The SR **D3D12** and **OpenGL** weavers weave for one: they refuse the override
  (`SR_ERROR_FEATURE_NOT_SUPPORTED`), log one WARN per weaver, and keep weaving the primary
  viewer. So the mode takes effect for D3D11 and Vulkan apps (and for apps the runtime weaves
  through its D3D11 path, such as service clients); D3D12 and OpenGL apps on their own weaver
  stay single-viewer for now. The SR capability is runtime-wide, so the entry is offered
  whatever API an app uses.
- **What it does.** With one person in front the picture is the same as `LeiaSR`; the weaver's
  multi-viewer path engages with two or more tracked viewers. The app still renders one stereo
  pair. The `ON` override weaves every tracked viewer, up to four, and does not apply the
  viewer cap of the LeiaSR system setting; `LeiaSR` (no override) honours the system setting.
  The override is per weaver, so it does not change the system setting or other apps.
- **How it reaches the weavers.** For the head device's screen, the runtime names the entry
  through the device's `OUTPUT_MODE` property (`leia_hmd_set_property`, `leia_device.c`), kept
  process-wide. A display processor made for one screen (`create_dp_*_for_screen`) is told its
  own entry through its `set_output_entry` slot (D3D11, D3D12, GL, Vulkan) and uses that
  instead. Right before each weave the weaver applies the value of its entry
  (`leia_multi_viewer_wanted` → `leia_sr_v2_apply_multiviewer`), on change only. Going back to
  `LeiaSR` sets `SR_MULTI_VIEWER_MODE_FOLLOW_CONFIG`, which removes the override; no weaver is
  recreated. The 2D entry changes nothing (the lens is off). Weavers pinned to a simulated
  viewer (untracked secondary screens) are left alone, so the mode has no effect there.

### The `MULTI_VIEWER` setting (an alias)

The `leia-sr` display processor keeps its setting from before the mode existed:

```bat
displayxr-cli dp options leia-sr               REM lists MULTI_VIEWER, its value and where it came from
displayxr-cli dp set leia-sr MULTI_VIEWER on   REM per user; --machine for every user (admin)
displayxr-cli dp unset leia-sr MULTI_VIEWER    REM back to the default
```

| Value | Where entry 2 is declared | Where it is not (two entries) |
|---|---|---|
| `system` (default) | `LeiaSR` is the default 3D mode; it follows the LeiaSR system setting (SDK default `FOLLOW_CONFIG`) | the weaver follows the LeiaSR system setting |
| `on` | `LeiaSR multi-viewer` is the **default** 3D mode | `SR_MULTI_VIEWER_MODE_ON` on every weaver, when the SR runtime supports it; ignored otherwise |
| `off` | `LeiaSR` is the default 3D mode and forces `SR_MULTI_VIEWER_MODE_OFF` | `SR_MULTI_VIEWER_MODE_OFF`, when the SR runtime supports it; ignored otherwise |

- **A stored per-screen 3D mode wins over the setting.** The setting only names the default
  for a screen with no choice. With `on` and `LeiaSR` chosen for a screen, that screen follows
  the system setting.
- **How it is read.** Only through the runtime's `host->get_setting("leia-sr", "MULTI_VIEWER")`
  (ADR-054 decision 7), so the runtime's precedence applies: `DXR_DP_LEIA_SR_MULTI_VIEWER` in the
  **hosting** process's environment, then the per-user store, then the machine store. Words are
  case-insensitive; anything else logs one WARN and means `system`.
- **When.** At each Leia device creation (`leia_plugin_resolve_multi_viewer`, `leia_plugin.c`).
  Where entry 2 is declared, the runtime also polls the plug-in's default
  (`XRT_DEVICE_PROPERTY_CONFIGURED_3D_MODE`), and the setting is read again on each poll, so a
  change applies to running apps. Where it is not, a change takes effect at the next app start,
  or, for IPC apps, the next start of `displayxr-service` (it hosts the device, and reads its
  own environment, not the client's).
- **When it cannot apply.** If the plug-in was built against an SR SDK without
  `srWeaverSetMultiViewerMode`, or the installed LeiaSR runtime does not report
  `multiViewerWeave`, a non-default value is ignored with one WARN at device creation, and
  `displayxr-cli dp options` reports the setting as not supported here.
- **Log lines.** At device creation:
  `leia_plugin: multi-viewer weave: <value> (MULTI_VIEWER <value>[, default]); <n> rendering modes[ ('LeiaSR multi-viewer' offered)], default 3D mode '<name>'`.
  On a switch: `leia_plugin: 3D mode '<name>' (entry <n>; was '<name>')` for the head device,
  `Leia <backend> DP: 3D mode '<name>' for this screen (entry <n>)` for a per-screen display
  processor, and `Leia <backend>: multi-viewer weave ON|OFF|FOLLOW_CONFIG` when a weaver takes
  the value.

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

## Windows D3D11: the lens belongs to its SR instance (#541)

The D3D11 arm rebuilds its SR instance in place when the SR platform restarts (#158), on a
worker thread, while the async lens worker (#215) and the runtime's own threads may still be
switching or reading the lens. A v2 `SrLens` points into a hint that `srDestroyInstance` frees,
so a lens call that lands during or after the teardown is a use-after-free in the SR client
(LeiaInc/LeiaSR#598). LeiaSR#892 makes the SR side refuse such a call, but SR runtimes up to
1.39.x do not have it.

`leia_sr_lens_gate.h` makes the plug-in safe on its own:

- Every lens call (switch, read, and the weaver's 2D-start hold that rides with a switch) runs
  under one lock, and only while the gate is **live**.
- `destroy_sdk_objects` **retires** the gate first: it waits for a lens call in flight, refuses
  later ones, and destroys the lens handle before the weaver, the senses and the instance go.
- `create_weaver_attempt` **publishes** the gate once the lens on the new instance exists. A
  call that arrived while the gate was retired was a no-op; the publish re-requests the runtime's
  latest wish on the new lens (one WARN).
- A failing switch (`SR_ERROR_LENS_NOT_AVAILABLE`, or any failure from an SR runtime without
  #892) recreates the lens on the current instance, with the same display binding, and retries
  once. One WARN per failure episode.
- `get_hardware_3d_state` never waits behind a switch: while one holds the lock it answers
  "unknown", which the runtime already treats as no verdict.

Lock order, outermost first: the runtime's render mutex, then `lens_thread_mtx`, then the gate
(a leaf). The policy is host-tested without an SDK by `tests/test_sr_lens_gate.cpp` (Linux CI).
The D3D12, GL and Vulkan arms do not rebuild in place: a platform restart reports STALE, and the
display processor is destroyed (lens before instance, after joining its lens worker) and
recreated, so they need no gate.
