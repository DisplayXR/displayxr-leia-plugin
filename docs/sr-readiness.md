# SR panel readiness and late geometry (Windows)

How the plug-in copes with the SR platform not having identified the panel yet
when `displayxr-service` (or any OpenXR app) starts — and how the geometry
reaches the runtime and the live head device once it does. Code:
`src/drv_leia/leia_sr_ready.{h,cpp}`; consumers in `leia_device.c`,
`leia_plugin.c`, `leia_sr_probe.cpp`, `leia_sr_d3d11.cpp`.

## The boot race

The SR platform has **no readiness API**. `SRService` starts at boot, but a
panel is only *identified* once the FPC serial link (the eye-tracker's COM
port) has answered:

- with the panel **on**, 4–7 s after `SRSession` starts (measured on several
  boots of an 8K box);
- with the panel **asleep**, never — until the monitor wakes. An unattended
  Windows-Update reboot from idle sleep puts the panel back to sleep ~3 s after
  boot, so on those boots identification came a day later, when someone
  touched the machine.

Until identification, every SR client sees a **"default display"** with an
empty product code (SR Session log: `Can not retrieve product code` →
`Default DevKit settings have been used instead of screen.ini file` →
`Active display changed to default display`):

- on the v1 C++ API, `getPrimaryActiveSRDisplay()` is not `isValid()` / has no
  location;
- on the v2 C99 API, `rt_DisplayGet*` return `SR_SUCCESS` with 15.6"-4K-ish
  **placeholder** values (the header says `SR_ERROR_DISPLAY_NOT_FOUND`; the
  implementation does not);
- the SDK **caches the `IDisplay*` per handle**, so a display handle created
  before identification stays "default" for the life of that handle. Only a
  fresh handle sees the real panel.

`displayxr-service` auto-starts at logon, straight into that window. Before
the readiness module, the plug-in queried once (2 s), fell back to hardcoded
15.6" 4K geometry, **latched** it — view scale (`g_view_scale_valid = true` on
the fallback path), probe cache (`g_probe_done` set before trying), the head
device's views/FOV — and never asked again. `get_display_info` returned
`false` and the runtime cached *nothing* for the life of the service. Result:
wrong view poses / no proper 3D for every IPC client (browser, shell) until
someone restarted the service by hand. Tracked as
[#266](https://github.com/DisplayXR/displayxr-leia-plugin/issues/266) (plug-in
side) and a matching runtime issue.

## The two readiness signals that work

| Signal | Cost | Used for |
|---|---|---|
| **Count byte ≥ 1 in `Global\sharedDeviceSerialMemory`** (SR's shared-memory block: 1 count byte, then 32-byte device serials; the same mapping `leia_edid_probe.c` opens for `service_running`) | one file-mapping open, no SR context | `leiasr_display_identified()` — gates *every* static SR query; polled |
| **A fresh display handle** that is `isValid()` with a non-empty `getLocation()` | one temporary SR context | verification inside the geometry query (`leiasr_query_recommended_view_dimensions`, `leia_sr_v2_query_display` already spin on exactly this) |

Signals that do **not** work, for the record: `Global\sharedDeviceSerialEvent*`
is defined by the SDK but never signalled; `DEVICE_READY` system events are
non-persistent and skipped at boot when the device matches the registry's
last-seen list.

**Rule:** no SR context is created while signal 1 is false, and nothing read
while unidentified is ever cached.

## The startup budget — ONE, shared

`leiasr_ready_wait()` block-polls signal 1 at 100 ms within a single
process-wide deadline, started on first use (probe / `create_device` /
`get_display_info`):

| | |
|---|---|
| Default | **20 s** |
| Override | `DXR_LEIA_SR_READY_TIMEOUT_S=<seconds>` (`0` = never block; invalid → default) |
| Scope | the whole process — every startup caller shares it, so the total blocking time at service start can never exceed it (it used to be 2 + 2 + 5 + 5 s of *serial* blocking, each spinning fresh SR contexts) |
| After expiry | every geometry query returns `false` **immediately** (one file-mapping open, no SR context) until the panel is identified |

The SR-side verification spin that follows a successful wait is clamped to what
is left of the budget, with a 1 s floor (`leiasr_ready_clamp`).

The budget also applies to every in-process OpenXR app (the plug-in's `probe`
runs at each `xrCreateInstance`): an app started while the panel sleeps waits
up to the budget at instance create, then binds with fallback geometry and is
updated in place like the service.

## Late re-derivation

If the budget expires unidentified, **one** detached watcher thread polls
signal 1 at 1 Hz. When it flips, the watcher resolves the geometry with a fresh
handle and **publishes** it:

1. the process-wide cache (`leiasr_geometry_get` — what `get_display_info` and
   the probe cache read);
2. the ONE per-view-scale derivation (`leia_view_scale_set_from_dims`; the
   fallback is no longer latched, so the late value lands);
3. the **live `leia_hmd`** in place, under the resolver's mutex:
   `hmd->screens`, `views[]`/viewports and FOV (through the same
   `u_device_setup_split_side_by_side` path creation uses), physical size,
   nominal viewer distance / static pose / eye offsets, and the LeiaSR mode's
   `view_scale_x/y`. The runtime never rebuilds the head device, so this is
   how the geometry reaches apps.

The watcher exits once the geometry is resolved (by itself or by anyone else),
or on plug-in `destroy`. Two other paths publish through the same function,
closing the gap when they get there first:

- the **D3D11 weaver's create worker** (`async_create_worker_body`, which
  retries forever and therefore also learns that the panel woke up) calls
  `leiasr_ready_note_weaver_ready()` after its READY CAS;
- the **runtime's own `get_display_info` re-calls** — it re-queries on every
  client compositor create and re-fills its cached info whenever the answer
  changes.

## Contract with the runtime (plugin_api 5 — no new ABI fields)

1. `get_display_info` is cheap and non-blocking once the startup budget is
   spent: `false` while the panel is unidentified (never the placeholder
   values — a `false` is retried, a wrong `true` is cached for the life of the
   service), `true` with the real geometry once it is.
2. The plug-in keeps its own head device correct in place when geometry
   arrives late.

## Log lines (all one-off `WARN`, never per poll)

```
Leia SR has not identified the panel yet (count=0 in Global\sharedDeviceSerialMemory — panel asleep, or SR still enumerating) — waiting up to 20.0 s (DXR_LEIA_SR_READY_TIMEOUT_S)
Leia SR identified the panel 5.3 s after first use — resuming geometry queries
Leia SR readiness budget (20.0 s) expired with the panel unidentified — geometry queries now return immediately; a 1 Hz watcher will re-derive the geometry once SR identifies the panel (...)
Leia display geometry resolved: 7680x4320 px, 0.6981x0.3927 m, nominal Z=0.60 m, view 3840x2160, 60.0 Hz (create_device)
Leia display geometry re-derived after late SR identification: 7680x4320 px, ... (late SR identification, watcher; 63012.4 s after first use) — live head device updated in place
Created Leia 3D display: 3840x2160 px, 0.3440x0.1940 m, ... (geometry from hardcoded defaults — SR has not identified the panel yet; the device will be updated in place once it does)
```

## Reproducing / testing

The window only exists while `SRService` is up with **no identified panel**.
Two ways to get there on a real box:

- **Panel asleep (the field case).** Power the monitor off (or put the box to
  sleep and wake it with the monitor off), then start `displayxr-service.exe`.
  Expect the "not identified yet" line, the budget expiring after 20 s, the
  device created from hardcoded defaults. Power the monitor on: within ~1 s of
  SR identifying it, expect the "re-derived after late SR identification"
  line; the next client connect (`displayxr-cli info`, the browser, a
  `cube_*` app under `XRT_FORCE_MODE=ipc`) shows the real geometry with no
  service restart.
- **SR service restart.** `net stop "SR Service"` (elevated), start
  `displayxr-service.exe`, then `net start "SR Service"`. Note that with the
  service *stopped* the shared-memory mapping does not exist, so the plug-in's
  `probe()` declines and the runtime binds sim-display instead — that is the
  documented "no SR stack" behaviour, not this bug. Use this variant to
  exercise the identified→unidentified→identified edge of an *already bound*
  plug-in (stop/start SR while the service runs, then wake the panel), or
  start the DisplayXR service in the few seconds between `net start` and
  identification (with the panel on that window is 4–7 s; set
  `DXR_LEIA_SR_READY_TIMEOUT_S=1` to make the budget expire inside it).
- **Shorten the wait** for iteration: `DXR_LEIA_SR_READY_TIMEOUT_S=2`.

Headless check on any box: the count byte can be read from PowerShell with
`OpenFileMapping`/`MapViewOfFile` on `Global\sharedDeviceSerialMemory`; `1`
followed by a space-padded serial (e.g. `DM022344CZ0001`) means identified.
