# Install order and platform state

Part of the order-independent installation epic,
[displayxr-runtime#1803](https://github.com/DisplayXR/displayxr-runtime/issues/1803).
The goal: the LeiaSR platform, the DisplayXR runtime and this plug-in can be
installed (and uninstalled) in any order, with or without the 3D display
attached, and the plug-in always says *why* it is not weaving.

## The plug-in loads without the LeiaSR platform

`DisplayXR-LeiaSR.dll` no longer has the SR client DLLs in its import table.
`SimulatedRealityCore`, `SimulatedRealityDisplays`, `SimulatedRealityDirectX`
and `SimulatedRealityOpenGL` are linked with `/DELAYLOAD`
(`src/drv_leia/CMakeLists.txt`), so the plug-in DLL loads on a machine where
LeiaSR is not installed. It then reports `PLATFORM_ABSENT` and declines, and
the runtime falls back to the simulation display. Before this change it failed
to load with error 126 and the fallback happened without any explanation.

**Where the SR DLLs come from.** A delay-load notification hook
(`src/drv_leia/leia_sr_delayload_win.c`) resolves each SR DLL by full path in
`<SR install dir>\bin`. The install dir is the default value of
`HKLM\SOFTWARE\Dimenco\Simulated Reality`, for example
`C:\Program Files\LeiaSR\Platform`. The hook loads with
`LOAD_WITH_ALTERED_SEARCH_PATH`, so the SR DLLs' own dependencies (such as
`opencv_world343.dll`) resolve from the same directory. If the hook misses, the
normal search (`PATH`) still applies. Because the plug-in reads the registry
instead of `PATH`, a DisplayXR service that was already running when LeiaSR was
installed still finds the SR DLLs without a restart. The registry read is
cached for the process once it succeeds. A miss is read again on the next
attempt, so an install that lands later is still found.

**When it is safe to call SR.** If a delay-loaded DLL or export is missing, the
first call into it raises an SEH exception at the call site. To prevent that,
`leia_sr_client_bind()` binds every SR import up front (SEH-guarded). Every
path that can reach the SR SDK checks this bind before making any SR call:
`create_device`, the geometry resolver behind `get_display_info`, and all four
DP factories. After one successful bind, no later SR call can fail on symbol
resolution.

**Testing aid:** `DXR_LEIA_SR_DIR=<dir>` replaces the registry value for DLL
resolution. `<dir>` is the folder that holds the client DLLs, i.e. the `bin`
folder. Point it at a directory that does not exist, and remove
`LeiaSR\Platform\bin` from `PATH`, to simulate an uninstalled platform on a
machine that has one.

## `probe()` never blocks

`probe()`, `probe_displays()` and the platform-state slot only run presence
checks, and each check returns in milliseconds:

| Check | Cost |
|---|---|
| SR registry key | one `RegOpenKeyEx` |
| SR client DLLs bindable | first call loads the SR DLLs (tens of ms, cold); sticky afterwards |
| SR Service running | one `OpenFileMapping("Global\sharedDeviceSerialMemory")` |
| Panel attached | EDID table match (SetupDi enumeration; re-run only when the monitor topology changes, for frame-adjacent callers) |
| Stale EDID table | count byte of the same mapping (SR has identified a device) |

The old 2 s SR-context spin on an EDID-table miss, and with it the 20 s
readiness budget, is gone from the probe path. Readiness waiting now happens
only in device creation and `get_display_info` ([sr-readiness.md](sr-readiness.md)).
D3D11 weaver creation was already asynchronous. D3D12, GL and VK weaver
creation never used the budget.

Measured on the ConceptD 7 dev box: the first probe takes 16 ms with the platform
absent and 94 ms with it present (this includes loading the SR DLLs from
Program Files). Repeat probes take 0.5 to 4 ms. The probe logs its first duration
once: `leia_plugin: probe took N ms (platform state S)`.

## Platform states

These are the runtime's generic `xrt_plugin_platform_state` values (ADR-045),
reported through `xrt_plugin_iface::get_platform_state` when the runtime
headers have it (`XRT_PLUGIN_HAS_PLATFORM_STATE`). Against older runtime
headers, the same state machine still drives probe and claim decisions but is
not reported.

| State | Meaning | Hint shown to the user |
|---|---|---|
| `PLATFORM_ABSENT` | SR registry key missing | `Install the LeiaSR Runtime` |
| `PLATFORM_ABSENT` | key present, SR client DLLs cannot be loaded | `LeiaSR Runtime files not found - reinstall the LeiaSR Runtime` |
| `INCOMPATIBLE` | SR DLLs load but lack an export this build imports | `The installed LeiaSR Runtime is not compatible with this plug-in - update the LeiaSR Runtime` |
| `PLATFORM_NOT_RUNNING` | key present, and the SCM does not report the `SR Service` service as `SERVICE_RUNNING`, or its `Global\sharedDeviceSerialMemory` section is absent | `The SR Service is not running` |
| `NO_DISPLAY` | SR running, no Leia panel attached: no EDID match, and either a table-known panel was seen earlier in this process (an unplug) or SR has identified no device | `No Leia 3D display detected` |
| `READY` | SR running and a panel is attached (EDID match, or SR has identified a panel the frozen EDID table does not know) | (empty) |

The SCM state is the authority for "running" (#294). The shared-memory section
alone is not enough: it survives a stopped SR Service for as long as another SR
client (the SR dashboard tray app, SRSession, ...) holds a handle to it. The
section stays a secondary check, meaning the service is far enough up to talk
to. The SCM query is cached for 1 s and works unelevated.

`probe()` succeeds only in `READY`. `probe_displays()` returns no claims in any
other state. In `READY` it claims EDID matches as `VERIFIED`. So with SR Service
stopped, the runtime starts on the fallback DP, and its slow re-probe timer
adopts this plug-in once the service is back. The plug-in never sets
`XRT_PLUGIN_PLATFORM_FLAG_FALLBACK`.

## Display hot-plug

- **Unplug while bound:** the state becomes `NO_DISPLAY`. The cached SR
  geometry is invalidated, so the probe cache and `get_display_info` stop
  reporting the old panel. All four Windows DPs pass view 0 through unwoven
  and skip the SR weaver: D3D11 and D3D12 with the flat-blit pipeline of the
  weaver-not-ready window, GL and VK with their single-view blit. Under
  ADR-045's no-live-swap rule, the runtime keeps the DP bound.
- **Re-plug:** the state returns to `READY`. The geometry watcher, which the
  invalidation restarted, re-derives the geometry once SR identifies the panel,
  then updates the live head device in place. Weaving resumes.
- **No more primary-monitor claim after an unplug.** The stale-table fallback
  (claim the primary monitor `VERIFIED` because SR reports a device) only
  applies if no table-known panel has been seen in this process. Before this
  change, an unplug led to weaving on an ordinary monitor.
- The DPs detect the change with a 1 Hz check of the monitor-topology
  signature (`EnumDisplayMonitors`) and run the EDID enumeration again only when
  that signature changes. The runtime's re-probes run a full EDID probe on
  every call.

## Log lines

All of these are one-shot `WARN`s, logged when the value changes:

```
Leia SR: client DLL directory resolved to 'C:\Program Files\LeiaSR\Platform\bin\' (HKLM\SOFTWARE\Dimenco\Simulated Reality)
Leia SR: client DLLs bound (4 DLLs, delay-loaded)
Leia SR: client DLLs NOT usable — SimulatedRealityCore.dll could not be loaded (code 0xc06d007e); ...
Leia SR platform state: READY (was UNKNOWN; key=1 service=1 scm_running=1 edid_match=1 dlls_bound=1)
Leia SR platform state: PLATFORM_NOT_RUNNING - The SR Service is not running (was READY; key=1 service=0 scm_running=0 ...)
Leia SR platform state: PLATFORM_ABSENT - Install the LeiaSR Runtime (was UNKNOWN; key=0 ...)
leia_plugin: probe took 0.5 ms (platform state READY)
Leia display geometry invalidated (the Leia panel is no longer attached (EDID match lost)) — ...
Leia D3D11 DP: Leia panel not attached (NO_DISPLAY) — passing pixels through unwoven
Leia D3D11 DP: Leia panel attached again — weaving resumes
Leia GL DP: Leia panel not attached (NO_DISPLAY) — passing view 0 through unwoven
Leia VK DP: Leia panel attached again — weaving resumes
Leia D3D11 DP: SR platform client DLLs not usable — not creating the display processor
```

Exception code `0xc06d007e` means a module was not found and `0xc06d007f`
means an export was not found. Both are raised by the delay-load helper.
