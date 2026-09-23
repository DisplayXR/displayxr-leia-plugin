# Leia Transparent Backgrounds — Compose-Under-Bg + Chroma-Key Fallback

How the Leia display processor composites a transparent app window over the Windows desktop, what each graphics API path does, and how an app opts in.

This supersedes [`chroma-key-transparent-overlay.md`](chroma-key-overlay.md) as the primary path. Chroma-key is still the documented fallback when WGC desktop capture is unavailable.

## Per-API status

| API | Primary path | Notes |
|---|---|---|
| D3D11 | compose-under-bg | Falls back to chroma-key on WGC failure. |
| D3D12 | compose-under-bg | Queue-level shared-fence `Wait` for sync. |
| Vulkan | compose-under-bg | D3D11 NT-handle → VK external image import. No GPU semaphore wait yet (relies on temporal separation; see [Limits](#limits)). |
| OpenGL | chroma-key only | Compose-under-bg blocked on the deferred DComp + `WGL_NV_DX_interop2` bridge. |
| Metal (macOS) | alpha-native | SR weaver preserves alpha end-to-end (commit `e94d07292`); no compose or chroma trick needed. |

## Why compose-under-bg

The SR weaver flattens alpha during interlacing. Two ways to expose the desktop through transparent app regions:

1. **Chroma-key (legacy).** Replace `α=0` atlas pixels with a magenta sentinel pre-weave, then post-weave detect that color and rewrite `α=0` for DWM. Survives the weaver as RGB. **Hard mask on AA edges** — the lerp toward key + exact-RGB strip can't represent `0<α<1`. Also vulnerable to disocclusion fringe at silhouette boundaries (see [`chroma-key-transparent-overlay.md`](chroma-key-overlay.md) §Limits).
2. **Compose-under-bg (preferred).** Capture the desktop region behind the window via Windows Graphics Capture and composite it UNDER each per-view atlas tile before the weaver runs. Output is genuinely opaque RGB with the desktop already integrated; the weaver consumes it normally. AA edges and semi-transparent pixels work correctly.

## Architecture

```
              ┌──────────────────────────────────────────┐
              │ leia_bg_capture (Win-only helper)         │
              │  - Internal D3D11 device                  │
              │  - WGC: monitor capture → staging tex     │
              │  - SHARED_NTHANDLE BGRA8 (monitor size)   │
              │  - D3D11_FENCE_FLAG_SHARED for sync       │
              └────────────┬─────────────────────────────┘
                           │ NT handles (texture + fence)
       ┌───────────────────┼───────────────────┐
       ▼                   ▼                   ▼
  D3D11 Leia DP      D3D12 Leia DP      Vulkan Leia DP
  OpenSharedResource1 OpenSharedHandle   VK_KHR_external_memory_win32
  ID3D11Fence Wait    ID3D12CommandQueue ::Wait   (no Wait yet — caveat)
       │                   │                   │
       └─── compose pass: lerp(bg, atlas.rgb, atlas.a), a=1 ───┐
                                                               ▼
                                                      SR weaver (opaque RGB in)
                                                               │
                                                               ▼
                                                  DComp swap chain → DWM
```

The per-DP `compose_*` pipelines reuse the existing chroma-key intermediate target (`ck_fill_*`) as the render target — same `R8G8B8A8_UNORM` format, no extra allocation. Distinct pipelines/descriptor sets/samplers (compose uses linear filtering; ck uses point).

## How "per eye" works out

The desktop sits at `z=0` (display plane), so the same captured background region is sampled into both tile 0 and tile 1. Per-eye-ness comes from the atlas content (which already has parallax per view), not the background. After the weaver interleaves, each eye sees `desktop + its own view's overlay` with correct parallax on the overlay and the desktop pinned at the display plane.

Shader logic (HLSL/GLSL identical in spirit):

```glsl
float4 a = atlas.Sample(samp, uv);
float2 tile_local = frac(uv * float2(tile_count));   // wrap into per-tile 0..1
float2 bg_uv = bg_uv_origin + tile_local * bg_uv_extent;
float3 b = bg.SampleLevel(samp, bg_uv, 0).rgb;
return float4(mix(b, a.rgb, a.a), 1.0);
```

`bg_uv_origin` / `bg_uv_extent` map the **client area** of the window onto the captured monitor texture (NOT the outer window rect — that would shift by the title-bar height).

## Self-capture defense

`leia_bg_capture_create` calls `SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)` so WGC does not recursively capture our own woven output back into the background. Requires Windows 10 build 19041+ (2004); on older Windows the bg-capture module fails to create and the DP falls back to chroma-key.

## Linux: a window-excluded mutter capture (GNOME)

Linux has no `WDA_EXCLUDEFROMCAPTURE`, and the first Linux capture (an xdg-desktop-portal ScreenCast of the whole monitor) recorded our own window: the previous **woven** frame was composed under the new one and woven again, putting both views into both eyes (confirmed on a DS1 panel; `DXR_LEIA_BG_DEBUG=1` showed a recursive tunnel).

The fix has two halves:

- **The exclusion** lives in the compositor, in the DisplayXR GNOME Shell extension `window-geometry@displayxr.org` (**version 2+**, shipped from `displayxr-runtime` `contrib/gnome-shell/`). Its `org.displayxr.CaptureExclusion1.Exclude(0)` leaves every window of the calling process out of mutter's *off-screen* stage paints, while the window keeps drawing on screen. Runtime spec: `docs/specs/runtime/wayland-window-geometry.md` §6.
- **The capture** (`src/drv_leia_linux/leia_bg_capture_linux.c`, `leia_mutter_capture_linux.c`) uses `org.gnome.Mutter.ScreenCast` directly instead of the portal. It excludes our windows first, reads the panel's **logical** rectangle from `org.gnome.Mutter.DisplayConfig`, and calls `RecordArea` over the panel's **full** rectangle with the cursor hidden. `RecordArea` renders off-screen, which is what the exclusion keys on; `RecordMonitor` can blit the on-screen view and would contain the window. The PipeWire node is consumed over the default socket, with no portal and no consent dialog. The existing PipeWire consumer and Vulkan staging are unchanged.

**Units.** `RecordArea` takes logical coordinates and streams device pixels: the panel's logical rect × its scale. A 1920×1080 logical rect at 200 % gives 3840×2160, and 2304×1296 at 5/3 gives 3840×2160. The window rect the DP receives is device pixels relative to the panel (present origin + target extent). It is normalised by the panel's device size (mutter's current mode, with the stream size as fallback) to address the stream. This supersedes the portal-era logical/device mix-up that #254 fixed.

**Trust.** The capture is used only while the exclusion is live:

| condition | behaviour |
|---|---|
| Extension absent (or the screen was locked at start) | No capture is started. **One WARN** says that installing the extension (then logging out and back in) is what enables correct transparency. Silhouette intersection. |
| Extension is version 1 (geometry only) | Same, with "update the extension". |
| Extension disappears mid-session (lock screen / disabled) | Capture distrusted at once. Silhouette intersection. |
| Extension comes back | The DP re-registers. It then also skips as many frames as the PipeWire pool holds, because a frame recorded before the exclusion was back can still be in flight. It trusts the capture again after that. |
| mutter closes the session (e.g. *Stop Screen Sharing*) | No capture for the rest of the session. Silhouette intersection. |
| Panel moved or rescaled | The recorded area is stale. The capture is re-created (at most once a second). |

**Alpha-gate rule, per frame.** When this frame's compose sampled a trusted capture, the gate punches only where **every** view is transparent (the Windows rule). The fringe keeps the captured desktop composed under it, with no halo and no shrink. On any other frame it punches where **any** view is transparent: that is silhouette intersection ([chroma-key-overlay.md §Limits](chroma-key-overlay.md#limits--disocclusion-fringe-near-the-silhouette)), which needs no background, at the cost of edges that shrink by the disparity.

**Rear depth budget.** `get_background_preview` is implemented (Windows contract). The pw thread box-filters the whole captured panel by 4× at ≤ 15 Hz. The render thread crops the window's region, reduces it further to ≤ 512 px, and publishes the covered canvas rect, rounded outward or clamped when the window hangs off the panel. It reports "no source" whenever the capture is not trusted, so the runtime clips only when the background behind the window actually has horizontal disparity.

**Threading, and the rule the first DS1 run taught us.** The render thread (`poll`, `get_background_preview`) must never wait on the capture. The PipeWire thread does the per-frame CPU work (copy into staging, the 4× box filter) with no lock held, reading the PipeWire buffer itself, which is cached memory. It hands the result over through a three-buffer pointer swap, and the render thread only *try*-locks that swap. The first panel run read the Vulkan staging buffer instead. That memory is write-combined: a CPU read of a 4K frame from it takes ~2.5–3.6 s, against ~60 ms cached. The read happened under the lock `get_preview` takes, so the app ran at 0.4 fps. Staging now also prefers a `HOST_CACHED` memory type (a 4K write costs 4.4 ms vs 11.7 ms). The D-Bus calls the render-thread pump needs (re-registering after an unlock, re-reading the layout) are asynchronous. If `poll` or `get_background_preview` ever takes more than 4 ms, one `RENDER-THREAD STALL` WARN per session names it.

**Why SHM and not dma-buf.** The stream is negotiated as SHM (MemFd/MemPtr) because our format offer deliberately carries no DRM modifier. That is the #109 workaround for mutter's Xorg path, which fixates dma-buf and never delivers. The spike offered a mandatory LINEAR modifier and got dma-buf. Zero-copy needs three things before it can be the default: a Wayland-only modifier offer; holding each dma-buf until our GPU has finished sampling it (today the dma-buf branch requeues the buffer to mutter immediately); and a GPU-side preview readback, because CPU reads of a mapped dma-buf hit the same uncached-memory trap. Until then, each delivered 4K frame costs the PipeWire thread about 4.4 ms of copy plus 4.8 ms of preview filter, at the 66 ms cap (≈15 fps under motion, nothing on a quiet desktop).

**Lazy capture: capable is not active.** Transparency capability is declared once, at session creation. An app that *can* go transparent (a model viewer with a Ctrl+T toggle) declares it from its first frame, and before this change the DP ran the capture, the compose-under and the alpha-gate on every opaque frame too. With a runtime that carries `set_transparency_active` (`XRT_DP_VK_HAS_TRANSPARENCY_ACTIVE`), the runtime probes the atlas for alpha < 1 every app frame and reports transitions: ACTIVE on the first transparent frame, IDLE after 60 consecutive opaque frames, and IDLE *before* transparency is enabled, so an opaque-start session never starts a capture. While IDLE the DP skips compose-under and the alpha-gate (the content has nothing to punch) and stops the capture; the pipelines are kept for the next toggle. On ACTIVE the alpha-gate is on at once in silhouette intersection, and the capture starts; its first trusted frame switches the gate to the every-view rule. Nothing captured before the idle stretch is held over. Starting and stopping a capture is D-Bus plus PipeWire work, so both run on a per-DP worker thread (`leia_bg_capture_worker_linux.c`) in posting order — a stop always finishes before the next start, so two ScreenCast sessions never overlap — and a stopped capture is handed to the worker only after three more `process_atlas` calls, when the GPU can no longer be sampling it. The panel-moved restart goes through the same worker. Built against an older runtime (the Linux pin) the slot compiles out and the capture runs for the whole session as before. The log lines to look for: `leia_lnx_dp: content transparency ACTIVE/IDLE` and `leia_bg_capture_worker: desktop capture start … / stopped (N ms, off the frame thread)`.

**UX note.** Any mutter ScreenCast session makes GNOME show its "screen is being shared" indicator while a transparent app runs — with lazy capture, only while its content is actually transparent. Its *Stop* button ends our session too, which the DP handles as above.

| env var | effect |
|---|---|
| *(unset)* | Window-excluded capture when the extension (v2+) is present, else silhouette intersection. |
| `LEIA_DP_DISABLE_BG_CAPTURE=1` | Never capture; silhouette intersection. Same name as the Windows switch, for A/B testing. |
| `LEIA_DP_CAPTURE_MIN_INTERVAL_MS=N` | Capture delivery cap, as on Windows: unset = 66 ms (≈15 fps), `0` = uncapped (mutter records on every damaging frame, up to the refresh rate). Offered to mutter as the stream's `maxFramerate`, which mutter enforces by skipping records. Each record is a full off-screen re-render of the panel, so this cap also bounds mutter's GPU cost. Measured in the nested shell: 14.7 fps capped vs 36.7 fps uncapped for the same animating window. |
| `DXR_LEIA_BG_DEBUG=1` | The window shows **only** the captured background, and the alpha gate is skipped. On the panel this is the check that our own window is absent from the capture. |
| `DXR_LEIA_PANEL_CONNECTOR=<name>` | Force which mutter monitor is the panel (dev/testing), e.g. `HDMI-1`. |

The interim `DXR_LEIA_BG_CAPTURE=1` opt-in is gone. It existed only because the portal capture could not exclude our window.

## Cross-API sync

The producer is the internal D3D11 device inside `leia_bg_capture_win`. Consumers are the DP's own device (D3D11/D3D12/VK). After each `CopyResource` from the WGC frame into the shared staging texture, the producer signals an `ID3D11Fence` (created with `D3D11_FENCE_FLAG_SHARED`) and flushes its context. Consumers wait before sampling:

- **D3D11:** `ID3D11DeviceContext4::Wait(fence, value)` in the DP's command stream.
- **D3D12:** `ID3D12CommandQueue::Wait(fence, value)` at the queue level before the cmd list executes.
- **VK:** Not implemented — the VK DP relies on the temporal gap between WGC's ~60Hz producer and the consumer's ~120Hz render rate (the producer's copy is sub-ms; the chance of mid-copy sample is negligible). A proper `VK_KHR_external_semaphore_win32` import via `VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT` is a follow-up.

## App-side recipe

To get transparency on D3D11 / D3D12 / Vulkan apps with a Leia 3D display:

1. **Window style.** Standard top-level window with `WS_EX_NOREDIRECTIONBITMAP`, null background brush. (DComp owns the redirection bitmap; a non-null brush would paint over the composition swap chain.)
2. **Clear to `RGBA(0,0,0,0)`** in the app's render target. The compose pass turns the cleared regions into the captured desktop.
3. **Opt-in via `XR_DXR_win32_window_binding`:**
   ```c
   XrWin32WindowBindingCreateInfoDXR bind = {
       .type = XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_DXR,
       .windowHandle = hwnd,
       .transparentBackgroundEnabled = XR_TRUE,
       .chromaKeyColor = 0,   // 0 = DP picks default (used only on fallback)
   };
   ```
4. **No `WS_EX_LAYERED`, no `SetLayeredWindowAttributes`** — those don't compose with the runtime's flip-model swap chain.

The test apps `cube_handle_d3d11_win`, `cube_handle_d3d12_win`, `cube_handle_vk_win` opt in via the `DISPLAYXR_TRANSPARENT_BG=1` env var.

## Fallback behavior

Compose-under-bg fails to initialize → DP transparently falls back to chroma-key. Triggers:

- `LEIA_DP_DISABLE_BG_CAPTURE=1` env var (for testing the fallback path).
- Windows version < 10 2004 (no `WDA_EXCLUDEFROMCAPTURE`).
- WGC `RoGetActivationFactory` failure (graphics class not registered).
- `SetWindowDisplayAffinity` failure (rare; some virtualization stacks).

The session log will show one of:
```
Leia D3D11 DP: transparency = compose-under-bg (WGC)
Leia D3D11 DP: transparency = chroma-key (key=0x00ff00ff — DP default)
```

## Limits

- **DRM-protected content** under the window appears black in WGC capture (standard WGC limitation — same as any screen recorder).
- **Window crossing monitors mid-session** — the capture session is bound to the monitor at create. `leia_bg_capture_poll` detects the monitor change and skips the compose pass for that frame so the DP doesn't sample the wrong desktop. Recreating the WGC session mid-stream is a follow-up.
- **Vulkan GPU sync** — see [Cross-API sync](#cross-api-sync). Visible only in pathological timing.
- **OpenGL apps** still use chroma-key. They inherit the AA-edge and disocclusion-fringe limitations documented in [`chroma-key-transparent-overlay.md`](chroma-key-overlay.md).

## Key source files

| File | Role |
|---|---|
| `src/drv_leia/leia_bg_capture_win.{h,cpp}` | WGC capture, shared NT-handle staging tex, shared fence. Win-only. |
| `src/drv_leia/leia_display_processor_d3d11.cpp` | D3D11 compose pipeline + fallback. |
| `src/drv_leia/leia_display_processor_d3d12.cpp` | D3D12 compose pipeline (PSO, root sig, descriptor heap) + fallback. |
| `src/drv_leia/leia_display_processor.cpp` | Vulkan compose pipeline (render pass reuse, external image import) + fallback. |
| `src/drv_leia/shaders/compose_under_bg.frag` | GLSL fragment shader (Vulkan). HLSL inlined in the D3D11/D3D12 .cpp files. |

## References

- `XR_DXR_win32_window_binding` spec_version 5 — [`src/external/openxr_includes/openxr/XR_DXR_win32_window_binding.h`](https://github.com/DisplayXR/displayxr-runtime/blob/main/src/external/openxr_includes/openxr/XR_DXR_win32_window_binding.h) (runtime repo; auto-synced to [displayxr-extensions](https://github.com/DisplayXR/displayxr-extensions))
- [`chroma-key-transparent-overlay.md`](chroma-key-overlay.md) — the legacy fallback path, kept as reference for the GL DP and as historical context.
- [Windows Graphics Capture (WGC)](https://learn.microsoft.com/en-us/windows/uwp/audio-video-camera/screen-capture)
- [`VK_KHR_external_memory_win32`](https://registry.khronos.org/vulkan/specs/latest/man/html/VK_KHR_external_memory_win32.html)
