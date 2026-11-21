# 2D→3D conversion moved out of this plug-in

Up to and including **v2.23.0** this repository carried Leia's 2D→3D conversion (the NeurD
module) in two places:

- behind the **D3D11 display processor's `lift_*` slots** and the lift-only factory
  `create_dp_d3d11_lift` (Windows), which the runtime reached through its legacy `dp-lift`
  adapter;
- as the **conversion provider** `DisplayXR-LeiaConversion.{dll,so}` (`src/conv_leia`, runtime
  ADR-053), shipped inside this plug-in's Windows installer and Linux `.deb`.

Both are gone from here. Conversion lives only in the conversion provider, which now has its own
repository and its own package (`displayxr-leia-conversion`: `DisplayXRLeiaConversionSetup-*.exe`,
`displayxr-leia-conversion_*.deb`). The NeurD runtime itself was never part of this repository and
is not part of that one either: it is an external prerequisite the provider detects.

## What changed here

| | Before | Now |
|---|---|---|
| D3D11 DP lift slots (`lift_get_caps`, `lift_stream_create`, `lift_stream_destroy`, `lift_convert`, `lift_get_depth`) | filled when the NeurD headers were fetched at build time | always NULL |
| `xrt_plugin_iface::create_dp_d3d11_lift` | the lift-only DP | NULL |
| `src/drv_leia/leia_lift_*`, `src/conv_leia/` | here | in the conversion-provider repository |
| NeurD header fetch (`NEURD_SDK_REF`, `scripts/fetch-neurd-headers.*`) | Windows and Linux builds | none: nothing here builds against NeurD |
| Lift knobs: `DXR_LEIA_LIFT*`, `HKLM\SOFTWARE\DisplayXR\Leia\Lift`, `scripts/set-lift-knobs.bat` | read by the DP's module | nothing reads them. The provider's settings are `displayxr-cli conv set leia-conversion <Key> <value>` (same key names) |
| Windows installer | also installed the provider DLL and wrote `ConversionProviders\leia-conversion` | installs the display plug-in only; removes its own leftover (below) |
| Linux `.deb` | also shipped `DisplayXR-LeiaConversion.so` + its manifest | ships the display plug-in only; refuses to stage those two files |

**Plug-in ABI: unchanged (API 5).** The lift slots and the lift-only factory are appended,
`struct_size`-gated members, and NULL was already a supported state: a build without the NeurD
headers produced exactly this plug-in, and the Linux display processor never had lift slots. The
runtime's slot helper (`xrt_display_processor_d3d11_has_lift`) reads NULL as "absent", and its
`dp-lift` adapter then reports `ABSENT` and conversion falls through to the next provider. The
runtime spec freezes those slots as dead ABI until plug-in API 6; this plug-in simply stops
filling them earlier than it had to. No runtime pin moved for this change.

**Why both went together.** Removing only the provider would have let NeurD come back through the
display plug-in: with no provider declaring `Supersedes = dp-lift`, the runtime's `dp-lift` row is
visible again and loads NeurD **into the service** through the lift slots. CI asserts both on the
shipped artifacts (`build-windows.yml`: the DLL names NeurD nowhere and no provider is staged;
`build-linux.yml` / `package_deb_leia.sh`: no provider file in the `.deb`).

## What a box sees

- **With the conversion-provider package installed**: nothing changes. `leia-conversion` serves the
  same capabilities from the conversion host.
- **Without it**: no Leia conversion. The runtime uses its next conversion provider (Open
  Conversion on a default install). `displayxr-cli conv list` shows which.
- The browser and other apps reach conversion only through `XR_DXR_lift` and the runtime's
  providers, never through this plug-in, so no app changes.

## Installer handover (Windows)

Older releases of this installer wrote `HKLM\Software\DisplayXR\ConversionProviders\leia-conversion`
with `Binary = <this plug-in's dir>\DisplayXR-LeiaConversion.dll` and **this** uninstaller in
`UninstallString`, and their uninstaller deleted that row unconditionally. The separate package
takes the row over when it is installed (its own directory, its own uninstaller). From this
release on, this installer and its uninstaller:

- always remove the copy of `DisplayXR-LeiaConversion.dll` an older release left in the plug-in's
  directory;
- remove the row **only while its `Binary` still names that copy**, i.e. while it is still this
  installer's leftover. They read that one value and nothing else; a row that points anywhere else
  is the separate package's and is never rewritten or deleted;
- delete the row's values (`Version` first) rather than the key, so a `Settings` subkey the user
  created survives for the separate package;
- do it under the provider's update sequence (the `Global\DisplayXR-ConvMaintenance-leia-conversion`
  mutex and a Restart Manager session over that one file, which closes the conversion host and
  never the service, and restarts nothing).

Order does not matter between the two **new** installers. What is not symmetrical is an **old**
installer of this plug-in (v2.23.0 or earlier) run after the separate package: it rewrites the row
to its own copy, and its uninstaller deletes the row. Re-run the conversion-provider installer to
take it back. Hence the migration order.

## Linux `.deb` handover

`displayxr-leia-conversion` declares `Replaces` and `Breaks` against the `displayxr-leia-sr`
versions that shipped the two files, so dpkg moves them between the packages:

```
sudo apt install ./displayxr-leia-conversion_*.deb ./displayxr-leia-sr_*.deb
```

Upgrade both in one transaction on a box that has an older `displayxr-leia-sr`. Upgrading only
`displayxr-leia-sr` is also fine: the provider files leave with it, and the box has no Leia
conversion until the other package is installed. This package declares no relation to the other
one: it is in no Ubuntu archive (the install check verifies that every relation resolves), and
the display plug-in works without conversion.

## Migration order

1. Release the conversion-provider package (Windows `.exe` + Linux `.deb`).
2. Runtime side: a `versions.json` field and orchestrator rows for it; decide whether the bundle
   chains it.
3. Merge this change and release this plug-in. That release's version is the provider `.deb`'s
   `LEIA_SR_SPLIT_VERSION` (its `Replaces` / `Breaks` bound).
4. Bundle and orchestrator pin both.

Between 1 and 3, do not cut a plug-in release from the old line onto boxes that already have the
separate package (see *Installer handover*).
