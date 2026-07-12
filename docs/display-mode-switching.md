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
