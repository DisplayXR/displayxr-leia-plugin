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
process-wide deadline, started on first use (`create_device` /
`get_display_info`). **Never from `probe()`**: since the install-order work
(displayxr-runtime#1803) the probe is presence checks only and returns in
milliseconds — see [install-order-and-platform-state.md](install-order-and-platform-state.md):

| | |
|---|---|
| Default | **20 s** |
| Override | `DXR_LEIA_SR_READY_TIMEOUT_S=<seconds>` (`0` = never block; invalid → default) |
| Scope | the whole process — every startup caller shares it, so the total blocking time at service start can never exceed it (it used to be 2 + 2 + 5 + 5 s of *serial* blocking, each spinning fresh SR contexts) |
| After expiry | every geometry query returns `false` **immediately** (one file-mapping open, no SR context) until the panel is identified |

The SR-side verification spin that follows a successful wait is clamped to what
is left of the budget, with a 1 s floor (`leiasr_ready_clamp`).

The budget also applies to every in-process OpenXR app (`get_display_info` and
`create_device` run at each `xrCreateInstance`): an app started while the panel
sleeps waits up to the budget at instance create, then binds with fallback
geometry and is updated in place like the service.

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
or on plug-in `destroy`. It is restarted when the cached geometry is
**invalidated** — the EDID-matched panel was unplugged or replaced (P-c in
[install-order-and-platform-state.md](install-order-and-platform-state.md)) —
and then additionally waits for the platform state to be `READY` again, because
SR can keep a device identified after its panel is gone. Two other paths publish through the same function,
closing the gap when they get there first:

- the **D3D11 weaver's create worker** (`async_create_worker_body`, which
  retries forever and therefore also learns that the panel woke up) calls
  `leiasr_ready_note_weaver_ready()` after its READY CAS;
- the **runtime's own `get_display_info` re-calls** — it re-queries on every
  client compositor create and re-fills its cached info whenever the answer
  changes.

## After a replug: the D3D11 weaver's wait (#468, #469)

After a long unplug SR takes 10–30 s to identify the panel again (a short
re-seat: well under 1 s). Two things make that wait cheaper on the D3D11 arm,
the only arm whose weaver is created asynchronously (D3D12, GL and Vulkan
create synchronously and have no pending state):

- **The create retry wakes on identification (#468).** `async_create_worker_body`
  spins up to 5 s per attempt and then waited a fixed 5 s, so an
  identification that landed in the wait cost up to 5 s more of flat blit. The
  wait now ends within 100 ms of the shared-memory count going 0 → ≥ 1, or of
  a verified-geometry publish (the watcher's; it covers SR keeping the count
  across an unplug, where there is no edge). Both are compared against a mark
  taken just before each create attempt, so an identification that lands
  during the attempt wakes the wait at its first 100 ms tick. A count that was
  already ≥ 1 when the attempt started is no news, so a failed attempt on an
  identified panel still waits its period. The rule is pure C in `leia_sr_ready_rules.h`,
  host-tested by `tests/test_sr_ready_rules.c`.
- **A pending DP reports the panel's kept geometry (#469).** While the weaver
  is pending, `get_display_dimensions` / `get_display_pixel_info` used to
  return false, so the runtime had no screen and the app rendered a symmetric
  default FOV for the whole wait. Every publish now also KEEPS the geometry
  with the EDID identity of the panel it was verified on (manufacturer,
  product, desktop size), and invalidation (P-c) leaves the kept copy alone. A
  pending DP reports the verified geometry if there is one, else the kept one
  when the attached panel has the same identity, with the panel's current
  desktop rect as the pixel info. A per-screen DP reports it only while the
  panel is attached exactly at its binding's rect (so not during Windows'
  "panel alone at (0, 0)" re-seat transient). A panel never verified in this
  process gets nothing, as before. The pick is in the same header and test.

  The getters run on the render thread, every frame while the weaver is
  pending, so they never refresh the platform state themselves: that takes the
  platform state's lock exclusively, reads the registry, queries the SR Service
  status, and re-runs the EDID enumeration whenever the monitor set changed —
  exactly in the replug window. A **pending-panel refresher** thread does it
  every 100 ms instead and keeps the attached panel; the getters only read that
  copy under the readiness module's short mutex. The D3D11 create retry worker
  starts the refresher (so the snapshot is warm before the first frame asks),
  every getter call keeps it running, and it exits 2 s after the last ask. A
  snapshot older than 500 ms, or none yet, reads as "no panel attached", so a
  stalled refresher means nothing is reported, never a panel that may have
  gone. Worst case after an unplug: one refresh period plus that refresh's
  EDID enumeration (bounded at 500 ms by the snapshot age), as when the getter
  refreshed itself, but off the render thread.

  Reporting a size is what the runtime's segments manager reads as "this DP
  can carry the session" (displayxr-runtime ADR-047 A8, `dp_ready`), so a
  pending DP for a panel that is attached again now counts as ready during the
  flat-blit wait — which is what it is: the panel is there, and the window is
  shown flat on it until the weaver is up.

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
Leia D3D11 async weaver: create attempt 3 failed — retrying in 5 s, or as soon as SR identifies the panel
Leia D3D11 async weaver: SR identified the panel (shared-memory count 0 -> 1) 2.7 s into the retry wait — retrying now (attempt 4) (#468)
Leia D3D11 async weaver: READY 180 ms after SR identified the panel (#468)
Leia D3D11 DP: weaver not ready — reporting the panel's kept geometry (0.3442x0.1936 m, 3840x2160 px @ -3840,0; verified on mfr=0x7204 prod=0x0001 3840x2160) until SR identifies it (#469)
Leia D3D11 DP: weaver ready — SR's geometry (0.3442x0.1936 m) replaces the kept geometry reported for 28.6 s (#469)
```

(The two #469 lines are per DP; the create-retry lines repeat per attempt, as
before.)

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
