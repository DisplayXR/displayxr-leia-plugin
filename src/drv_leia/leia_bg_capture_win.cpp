// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Implementation of @ref leia_bg_capture_win.h
 * @ingroup drv_leia
 */

#include "leia_bg_capture_win.h"

#include "util/u_logging.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>
#include <wchar.h>

#include <windows.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>

#include <inspectable.h>
#include <roapi.h>
#include <windows.foundation.h>
#include <windows.graphics.h>
#include <windows.graphics.directx.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;

namespace WGC = ABI::Windows::Graphics::Capture;
namespace WGDX = ABI::Windows::Graphics::DirectX;
namespace WGD3D = ABI::Windows::Graphics::DirectX::Direct3D11;
namespace WG = ABI::Windows::Graphics;
namespace WF = ABI::Windows::Foundation;

// IDirect3DDxgiInterfaceAccess is a Win32 COM interface (not ABI), declared in
// the global Windows::Graphics::DirectX::Direct3D11 namespace by the SDK
// interop header.
using IDxgiAccess = Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

#ifdef LEIA_BG_CAPTURE_HAS_PREVIEW
//! Both preview dimensions must stay <= this (@ref xrt_dp_background_preview).
static const UINT LEIA_BG_PREVIEW_MAX_DIM = 512;
//! Minimum box-filter reduction, per the design brief (~4x ⟹ mip level 2).
static const UINT LEIA_BG_PREVIEW_MIN_SHIFT = 2;
//! Below this the window is not worth previewing (and the mip chain degenerates).
static const LONG LEIA_BG_PREVIEW_MIN_SRC = 8;

/*!
 * One half of the double-buffered readback. The producer writes slot N while
 * the reader Maps slot N-1, so the Map never waits on GPU work still in flight
 * — the whole reason there are two.
 */
struct leia_bg_preview_slot
{
	ComPtr<ID3D11Texture2D> staging; //!< STAGING + CPU_ACCESS_READ, preview-sized.
	UINT w = 0, h = 0;
	UINT64 gen = 0; //!< capture generation (signaled_value) these pixels came from.
	bool pending = false;
};
#endif // LEIA_BG_CAPTURE_HAS_PREVIEW

struct leia_bg_capture
{
	HWND hwnd;
	bool affinity_owned; //!< true ⟹ WE set WDA_EXCLUDEFROMCAPTURE; reset it on teardown (#551).
	HMONITOR monitor;
	UINT monitor_w;
	UINT monitor_h;
	RECT monitor_rect; // virtual-screen coords

	// Producer-side D3D11 (internal — separate from any DP).
	ComPtr<ID3D11Device> d3d11_device;
	ComPtr<ID3D11DeviceContext> d3d11_context;
	ComPtr<ID3D11DeviceContext4> d3d11_context4; //!< For Signal().

	// WGC plumbing.
	ComPtr<WGD3D::IDirect3DDevice> wg_device;
	ComPtr<WGC::IGraphicsCaptureItem> capture_item;
	ComPtr<WGC::IDirect3D11CaptureFramePool> frame_pool;
	ComPtr<WGC::IGraphicsCaptureSession> capture_session;

	// Shared staging texture (monitor-sized, BGRA8, SHARED_NTHANDLE).
	ComPtr<ID3D11Texture2D> staging_tex;
	HANDLE staging_shared_handle;

	uint64_t adapter_luid; //!< Packed LUID the producer device landed on (0 = unknown).

	// Cross-API GPU sync — D3D11 shared fence. Producer signals after each
	// CopyResource; consumers (D3D11 or D3D12) Wait before sampling.
	ComPtr<ID3D11Fence> shared_fence;
	HANDLE shared_fence_handle;
	std::atomic<UINT64> signaled_value;

	std::atomic<bool> has_frame;

#ifdef LEIA_BG_CAPTURE_HAS_PREVIEW
	// ---- rear depth budget background preview (#224) ----------------------
	// EVERYTHING below is touched from the compositor render thread ONLY:
	// produced at the tail of leia_bg_capture_poll() (itself called from
	// process_atlas → compose_run_pre_weave) and read by
	// leia_bg_capture_get_preview(), which the runtime calls on that same
	// thread right after process_atlas. Hence no atomics and no lock — the
	// two std::atomic members above exist because the WGC frame pool is
	// free-threaded, which none of this is.
	bool preview_enabled;      //!< false ⟹ LEIA_DP_DISABLE_BG_PREVIEW armed.
	bool preview_broken;       //!< one-shot: a D3D failure retired the producer.
	bool preview_logged_ready; //!< one-shot init log.
	bool poll_ok;              //!< verdict of the last leia_bg_capture_poll().

	UINT preview_src_w, preview_src_h; //!< window rect the mip chain is sized for.
	UINT preview_shift;                //!< mip level sampled == log2 of the reduction.
	ComPtr<ID3D11Texture2D> preview_mip_tex;
	ComPtr<ID3D11ShaderResourceView> preview_mip_srv;
	leia_bg_preview_slot preview_slot[2];
	uint32_t preview_write;      //!< slot index the NEXT produce writes into.
	UINT64 preview_last_src_gen; //!< signaled_value at the last produce (throttle gate).

	std::vector<uint8_t> preview_cpu; //!< latest read-back bytes, BGRA8 top-down, tightly packed.
	uint32_t preview_cpu_w, preview_cpu_h, preview_cpu_stride;
	UINT64 preview_cpu_gen;
#endif
};

// ---------- helpers --------------------------------------------------------

static bool
env_disable()
{
	char buf[8];
	DWORD n = GetEnvironmentVariableA("LEIA_DP_DISABLE_BG_CAPTURE", buf, sizeof(buf));
	return n > 0 && n < sizeof(buf) && (buf[0] == '1' || buf[0] == 't' || buf[0] == 'T');
}

static bool
os_supports_wda_exclude_from_capture()
{
	// WDA_EXCLUDEFROMCAPTURE requires Windows 10 2004 (build 19041).
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (ntdll == nullptr) {
		return false;
	}
	typedef LONG(WINAPI * RtlGetVersion_t)(OSVERSIONINFOEXW *);
	auto fn = (RtlGetVersion_t)GetProcAddress(ntdll, "RtlGetVersion");
	if (fn == nullptr) {
		return false;
	}
	OSVERSIONINFOEXW vi = {};
	vi.dwOSVersionInfoSize = sizeof(vi);
	if (fn(&vi) != 0) {
		return false;
	}
	return vi.dwBuildNumber >= 19041;
}

static uint64_t
pack_luid(LUID l)
{
	return ((uint64_t)(uint32_t)l.HighPart << 32) | (uint64_t)(uint32_t)l.LowPart;
}

static HRESULT
create_internal_d3d11(uint64_t adapter_luid, ID3D11Device **out_dev, ID3D11DeviceContext **out_ctx)
{
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	D3D_FEATURE_LEVEL fl;
	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

	// The producer device must live on the SAME adapter as the consumer
	// device that will open the shared staging texture: D3D11 shared
	// textures do not cross adapters, and on Intel UHD 30.0.100.x a
	// cross-adapter VK import crashes inside the ICD (#819). The default
	// (NULL-adapter) path follows the process GpuPreference, which says
	// nothing about the consumer, so a LUID is matched explicitly.
	if (adapter_luid != 0) {
		ComPtr<IDXGIFactory1> factory;
		HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
		if (SUCCEEDED(hr)) {
			for (UINT i = 0;; i++) {
				ComPtr<IDXGIAdapter1> adapter;
				if (factory->EnumAdapters1(i, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) {
					break;
				}
				DXGI_ADAPTER_DESC1 desc;
				if (FAILED(adapter->GetDesc1(&desc)) || pack_luid(desc.AdapterLuid) != adapter_luid) {
					continue;
				}
				return D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
				                         levels, 2, D3D11_SDK_VERSION, out_dev, &fl, out_ctx);
			}
		}
		// A requested adapter that cannot be found must NOT silently fall
		// back to the default one — that reintroduces the cross-adapter
		// import. Fail; the DP falls back to chroma-key.
		U_LOG_W("leia_bg_capture: no DXGI adapter with LUID 0x%016llx — falling back to chroma-key",
		        (unsigned long long)adapter_luid);
		return E_FAIL;
	}
	return D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
	                         D3D11_SDK_VERSION, out_dev, &fl, out_ctx);
}

static uint64_t
query_device_adapter_luid(ID3D11Device *dev)
{
	ComPtr<IDXGIDevice> dxgi_device;
	if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
		return 0;
	}
	ComPtr<IDXGIAdapter> adapter;
	if (FAILED(dxgi_device->GetAdapter(adapter.GetAddressOf()))) {
		return 0;
	}
	DXGI_ADAPTER_DESC desc;
	if (FAILED(adapter->GetDesc(&desc))) {
		return 0;
	}
	return pack_luid(desc.AdapterLuid);
}

static HRESULT
wrap_d3d11_as_winrt(ID3D11Device *d3d11, WGD3D::IDirect3DDevice **out)
{
	ComPtr<IDXGIDevice> dxgi_device;
	HRESULT hr = d3d11->QueryInterface(IID_PPV_ARGS(&dxgi_device));
	if (FAILED(hr)) {
		return hr;
	}
	ComPtr<IInspectable> inspectable;
	hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.Get(), &inspectable);
	if (FAILED(hr)) {
		return hr;
	}
	return inspectable->QueryInterface(__uuidof(WGD3D::IDirect3DDevice), reinterpret_cast<void **>(out));
}

static HRESULT
create_staging_texture(ID3D11Device *dev, UINT w, UINT h, ID3D11Texture2D **out_tex, HANDLE *out_handle)
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = w;
	desc.Height = h;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	// SHARED_NTHANDLE for cross-process / cross-API import; SHARED is implied.
	// Sync is via a separate shared fence (no keyed mutex — keyed mutex is
	// awkward to access from D3D12).
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
	HRESULT hr = dev->CreateTexture2D(&desc, nullptr, out_tex);
	if (FAILED(hr)) {
		return hr;
	}
	ComPtr<IDXGIResource1> resource1;
	hr = (*out_tex)->QueryInterface(IID_PPV_ARGS(&resource1));
	if (FAILED(hr)) {
		return hr;
	}
	return resource1->CreateSharedHandle(nullptr,
	                                     DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
	                                     nullptr,
	                                     out_handle);
}

static HRESULT
create_shared_fence(ID3D11Device *dev, ID3D11Fence **out_fence, HANDLE *out_handle)
{
	ComPtr<ID3D11Device5> dev5;
	HRESULT hr = dev->QueryInterface(IID_PPV_ARGS(&dev5));
	if (FAILED(hr)) {
		return hr;
	}
	hr = dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(out_fence));
	if (FAILED(hr)) {
		return hr;
	}
	return (*out_fence)->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, out_handle);
}

// ---------- background preview producer (#224) ------------------------------
#ifdef LEIA_BG_CAPTURE_HAS_PREVIEW

static bool
preview_env_disable()
{
	char buf[8];
	DWORD n = GetEnvironmentVariableA("LEIA_DP_DISABLE_BG_PREVIEW", buf, sizeof(buf));
	return n > 0 && n < sizeof(buf) && (buf[0] == '1' || buf[0] == 't' || buf[0] == 'T');
}

/*!
 * How far to reduce: at least 4x (the brief's box filter), more when a 4K-wide
 * window would still exceed the 512 px ceiling the runtime struct documents.
 */
static UINT
preview_shift_for(UINT w, UINT h)
{
	UINT shift = LEIA_BG_PREVIEW_MIN_SHIFT;
	while (shift < 12 && ((w >> shift) > LEIA_BG_PREVIEW_MAX_DIM || (h >> shift) > LEIA_BG_PREVIEW_MAX_DIM)) {
		shift++;
	}
	return shift;
}

/*!
 * (Re)build the mip chain + both staging slots for a window rect of @p src_w ×
 * @p src_h. Cheap no-op while the window keeps its size; a resize rebuilds, and
 * a resize is already the slow path.
 */
static bool
preview_ensure_targets(struct leia_bg_capture *c, UINT src_w, UINT src_h)
{
	const UINT shift = preview_shift_for(src_w, src_h);
	if (c->preview_mip_tex != nullptr && c->preview_src_w == src_w && c->preview_src_h == src_h &&
	    c->preview_shift == shift) {
		return true;
	}

	c->preview_mip_tex.Reset();
	c->preview_mip_srv.Reset();
	c->preview_slot[0] = leia_bg_preview_slot();
	c->preview_slot[1] = leia_bg_preview_slot();
	c->preview_write = 0;
	c->preview_src_w = 0;
	c->preview_src_h = 0;

	ID3D11Device *dev = c->d3d11_device.Get();
	if (dev == nullptr) {
		return false;
	}

	D3D11_TEXTURE2D_DESC md = {};
	md.Width = src_w;
	md.Height = src_h;
	md.MipLevels = shift + 1;
	md.ArraySize = 1;
	md.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	md.SampleDesc.Count = 1;
	md.Usage = D3D11_USAGE_DEFAULT;
	// GenerateMips needs both binds plus the misc flag.
	md.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	md.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
	if (FAILED(dev->CreateTexture2D(&md, nullptr, c->preview_mip_tex.GetAddressOf()))) {
		return false;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
	vd.Format = md.Format;
	vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	vd.Texture2D.MipLevels = md.MipLevels;
	if (FAILED(dev->CreateShaderResourceView(c->preview_mip_tex.Get(), &vd, c->preview_mip_srv.GetAddressOf()))) {
		return false;
	}

	const UINT pw = (src_w >> shift) != 0 ? (src_w >> shift) : 1u;
	const UINT ph = (src_h >> shift) != 0 ? (src_h >> shift) : 1u;

	D3D11_TEXTURE2D_DESC sd = {};
	sd.Width = pw;
	sd.Height = ph;
	sd.MipLevels = 1;
	sd.ArraySize = 1;
	sd.Format = md.Format;
	sd.SampleDesc.Count = 1;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	for (int i = 0; i < 2; i++) {
		if (FAILED(dev->CreateTexture2D(&sd, nullptr, c->preview_slot[i].staging.GetAddressOf()))) {
			return false;
		}
		c->preview_slot[i].w = pw;
		c->preview_slot[i].h = ph;
	}

	c->preview_src_w = src_w;
	c->preview_src_h = src_h;
	c->preview_shift = shift;

	if (!c->preview_logged_ready) {
		c->preview_logged_ready = true;
		U_LOG_W("leia_bg_capture: background preview ready — window %ux%u → %ux%u BGRA8 "
		        "(1/%u box filter via mip %u, double-buffered staging readback, produced "
		        "once per capture generation)",
		        src_w, src_h, pw, ph, 1u << shift, shift);
	}
	return true;
}

/*!
 * Produce one generation of preview. Called at the TAIL of
 * leia_bg_capture_poll(), i.e. after this generation's CopyResource of the WGC
 * frame into the monitor staging texture has been queued and after the window
 * rect is known — and only when @ref leia_bg_capture::signaled_value advanced,
 * so the cost is bounded by the capture throttle (<= 15 Hz), never per weave.
 *
 * Producing here rather than lazily in get_preview() is deliberate: the Map has
 * to be kept off the critical path either way, and doing it here means it maps
 * a slot whose copy was queued a FULL capture interval ago (>= ~66 ms) instead
 * of one queued microseconds earlier in the same call. Lazy production would
 * also have to re-derive the window rect and would run on the runtime's slot
 * call, which happens at weave rate, not capture rate.
 */
static void
preview_produce(struct leia_bg_capture *c, LONG rx, LONG ry, LONG rw, LONG rh, UINT64 gen)
{
	if (!c->preview_enabled || c->preview_broken) {
		return;
	}

	// Clamp the window rect to the captured monitor texture.
	if (rx < 0) {
		rw += rx;
		rx = 0;
	}
	if (ry < 0) {
		rh += ry;
		ry = 0;
	}
	if (rx + rw > (LONG)c->monitor_w) {
		rw = (LONG)c->monitor_w - rx;
	}
	if (ry + rh > (LONG)c->monitor_h) {
		rh = (LONG)c->monitor_h - ry;
	}
	if (rw < LEIA_BG_PREVIEW_MIN_SRC || rh < LEIA_BG_PREVIEW_MIN_SRC) {
		return;
	}

	if (!preview_ensure_targets(c, (UINT)rw, (UINT)rh)) {
		c->preview_broken = true;
		U_LOG_W("leia_bg_capture: background preview targets could not be created — preview "
		        "producer DISABLED; get_background_preview will report no source and the "
		        "runtime keeps its clip-at-the-display-plane fallback");
		return;
	}

	ID3D11DeviceContext *ctx = c->d3d11_context.Get();

	// 1. window sub-rect of the just-captured monitor frame → mip 0.
	D3D11_BOX box = {(UINT)rx, (UINT)ry, 0u, (UINT)(rx + rw), (UINT)(ry + rh), 1u};
	ctx->CopySubresourceRegion(c->preview_mip_tex.Get(), 0, 0, 0, 0, c->staging_tex.Get(), 0, &box);

	// 2. box-filter down. GenerateMips averages 2×2 per level, so mip `shift`
	//    IS the 1/2^shift box filter the brief asks for — no shader source, no
	//    d3dcompiler dependency in this module, no extra pipeline state.
	ctx->GenerateMips(c->preview_mip_srv.Get());

	// 3. the reduced level → this generation's staging slot.
	leia_bg_preview_slot &wslot = c->preview_slot[c->preview_write];
	ctx->CopySubresourceRegion(wslot.staging.Get(), 0, 0, 0, 0, c->preview_mip_tex.Get(), c->preview_shift,
	                           nullptr);
	wslot.gen = gen;
	wslot.pending = true;
	// The copy must reach the GPU now so it is retired by the time the NEXT
	// generation maps this slot.
	ctx->Flush();

	// 4. Read back the OTHER slot — generation N-1, queued a full capture
	//    interval ago. DO_NOT_WAIT so even a driver that has not retired it
	//    costs nothing: the slot stays pending and is picked up next time.
	leia_bg_preview_slot &rslot = c->preview_slot[c->preview_write ^ 1u];
	c->preview_write ^= 1u;
	if (!rslot.pending) {
		return;
	}
	D3D11_MAPPED_SUBRESOURCE m = {};
	HRESULT hr = ctx->Map(rslot.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
	if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
		return; // keep it pending; next generation will get it
	}
	if (FAILED(hr)) {
		rslot.pending = false;
		return;
	}
	const uint32_t stride = rslot.w * 4u;
	c->preview_cpu.resize((size_t)stride * rslot.h);
	const uint8_t *src = (const uint8_t *)m.pData;
	for (UINT y = 0; y < rslot.h; y++) {
		memcpy(c->preview_cpu.data() + (size_t)y * stride, src + (size_t)y * m.RowPitch, stride);
	}
	ctx->Unmap(rslot.staging.Get(), 0);
	rslot.pending = false;

	c->preview_cpu_w = rslot.w;
	c->preview_cpu_h = rslot.h;
	c->preview_cpu_stride = stride;
	c->preview_cpu_gen = rslot.gen;
}

//! True when the runtime's struct_size covers @p field in full.
#define LEIA_BGP_FITS(out, field)                                                                                      \
	((size_t)offsetof(struct xrt_dp_background_preview, field) + sizeof((out)->field) <= (size_t)(out)->struct_size)

extern "C" bool
leia_bg_capture_get_preview(struct leia_bg_capture *c, struct xrt_dp_background_preview *out)
{
	if (c == nullptr || out == nullptr) {
		return false;
	}
	if (!c->preview_enabled || c->preview_broken) {
		return false;
	}
	// Capture off / no frame yet / cross-monitor drag — the last poll said so.
	if (!c->poll_ok) {
		return false;
	}
	if (c->preview_cpu.empty() || c->preview_cpu_w == 0 || c->preview_cpu_h == 0) {
		return false;
	}
	// A runtime whose struct is too small to describe a buffer cannot be
	// handed one; failing closed is always safe.
	if (!LEIA_BGP_FITS(out, width) || !LEIA_BGP_FITS(out, height) || !LEIA_BGP_FITS(out, stride_bytes) ||
	    !LEIA_BGP_FITS(out, bgra)) {
		return false;
	}

	if (LEIA_BGP_FITS(out, generation)) {
		out->generation = (uint32_t)c->preview_cpu_gen;
	}
	out->width = c->preview_cpu_w;
	out->height = c->preview_cpu_h;
	out->stride_bytes = c->preview_cpu_stride;
	out->bgra = c->preview_cpu.data();

	// The preview covers the window's CLIENT rect, which is the desktop region
	// under the canvas for every app that fills its window — i.e. 0,0,1,1 per
	// the spec. (A canvas sub-rect inside the window, the #131 compose remap,
	// is not modelled here: the sub-rect is DP state, not capture state. A
	// follow-up can plumb it through and narrow these UVs.)
	if (LEIA_BGP_FITS(out, canvas_v1)) {
		out->canvas_u0 = 0.0f;
		out->canvas_v0 = 0.0f;
		out->canvas_u1 = 1.0f;
		out->canvas_v1 = 1.0f;
	}

	// STALE = the source knows the preview no longer reflects the screen; an
	// unchanged desktop is NOT stale — the capture only delivers on change.
	//
	// WGC hands us a frame only when the desktop CHANGES, so `generation` sitting
	// still for minutes means "the background is exactly what you last saw", not
	// "the capture died". Age is therefore NOT evidence of staleness, and reading
	// it as such would re-clip a model over a perfectly quiet desktop once a
	// second.
	//
	// The cases where the module POSITIVELY knows the cached bytes stopped
	// describing the screen — the capture session closed or was recreated, the
	// window crossed monitors, client-present mode — are all already covered by
	// the `poll_ok` gate above, which returns false outright. So there is nothing
	// left for this bit to report today and it is always 0. If a future path ever
	// serves a cached preview it knows to be wrong (rather than refusing to serve
	// one), set it here.
	if (LEIA_BGP_FITS(out, flags)) {
		out->flags = 0u;
	}
	return true;
}

#endif // LEIA_BG_CAPTURE_HAS_PREVIEW

// ---------- public API -----------------------------------------------------

extern "C" struct leia_bg_capture *
leia_bg_capture_create(HWND hwnd, uint64_t adapter_luid)
{
	if (env_disable()) {
		U_LOG_W("leia_bg_capture: disabled via LEIA_DP_DISABLE_BG_CAPTURE — falling back to chroma-key");
		return nullptr;
	}
	if (hwnd == nullptr) {
		U_LOG_W("leia_bg_capture: NULL hwnd — falling back to chroma-key");
		return nullptr;
	}
	if (!os_supports_wda_exclude_from_capture()) {
		U_LOG_W("leia_bg_capture: OS < Win10 2004 lacks WDA_EXCLUDEFROMCAPTURE — falling back to chroma-key");
		return nullptr;
	}
	// WDA_EXCLUDEFROMCAPTURE excludes this window from our WGC monitor capture
	// so we composite over the desktop BEHIND it, not our own weave. The call
	// requires owning the window. For an out-of-process DP — the service-side
	// compositor weaving an IPC client's cross-process HWND — it returns
	// ERROR_ACCESS_DENIED; there the runtime sets the affinity from the
	// window-owning process, so we detect the already-set state (via
	// GetWindowDisplayAffinity, which works from any process) and proceed
	// instead of falling back to chroma-key. (#551)
	bool affinity_owned = false;
	if (SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
		affinity_owned = true;
	} else {
		DWORD err = GetLastError();
		DWORD aff = WDA_NONE;
		bool already_excluded = GetWindowDisplayAffinity(hwnd, &aff) && (aff == WDA_EXCLUDEFROMCAPTURE);
		if (err == ERROR_ACCESS_DENIED && already_excluded) {
			U_LOG_W("leia_bg_capture: cross-process window — capture-exclusion affinity "
			        "pre-set by the runtime; proceeding with WGC compose-under");
		} else {
			U_LOG_W("leia_bg_capture: SetWindowDisplayAffinity failed (err=%lu, aff=0x%lx) — "
			        "falling back to chroma-key",
			        err, aff);
			return nullptr;
		}
	}
	// Only undo the affinity on an error path / teardown if WE set it — the
	// runtime owns it on the cross-process path. (Capture by value: both are
	// fixed hereafter; [&] would self-capture on MSVC.)
	auto reset_affinity = [affinity_owned, hwnd]() {
		if (affinity_owned) {
			SetWindowDisplayAffinity(hwnd, WDA_NONE);
		}
	};

	HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi = {};
	mi.cbSize = sizeof(mi);
	if (monitor == nullptr || !GetMonitorInfoW(monitor, &mi)) {
		U_LOG_W("leia_bg_capture: monitor info lookup failed");
		reset_affinity();
		return nullptr;
	}
	UINT monitor_w = mi.rcMonitor.right - mi.rcMonitor.left;
	UINT monitor_h = mi.rcMonitor.bottom - mi.rcMonitor.top;

	HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
	if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
		U_LOG_W("leia_bg_capture: RoInitialize failed: 0x%08x", (unsigned)hr);
		reset_affinity();
		return nullptr;
	}

	ComPtr<ID3D11Device> d3d11_device;
	ComPtr<ID3D11DeviceContext> d3d11_context;
	hr = create_internal_d3d11(adapter_luid, d3d11_device.GetAddressOf(), d3d11_context.GetAddressOf());
	if (FAILED(hr)) {
		U_LOG_W("leia_bg_capture: D3D11CreateDevice failed: 0x%08x", (unsigned)hr);
		reset_affinity();
		return nullptr;
	}

	ComPtr<WGD3D::IDirect3DDevice> wg_device;
	hr = wrap_d3d11_as_winrt(d3d11_device.Get(), wg_device.GetAddressOf());
	if (FAILED(hr)) {
		U_LOG_W("leia_bg_capture: WinRT D3D11 device wrap failed: 0x%08x", (unsigned)hr);
		reset_affinity();
		return nullptr;
	}

	ComPtr<IGraphicsCaptureItemInterop> interop;
	{
		HStringReference cls(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem);
		ComPtr<IActivationFactory> factory;
		hr = RoGetActivationFactory(cls.Get(), IID_PPV_ARGS(&factory));
		if (FAILED(hr)) {
			U_LOG_W("leia_bg_capture: WGC item activation factory unavailable: 0x%08x — falling back to chroma-key",
			        (unsigned)hr);
			reset_affinity();
			return nullptr;
		}
		hr = factory.As(&interop);
		if (FAILED(hr)) {
			U_LOG_W("leia_bg_capture: IGraphicsCaptureItemInterop QI failed: 0x%08x", (unsigned)hr);
			reset_affinity();
			return nullptr;
		}
	}

	ComPtr<WGC::IGraphicsCaptureItem> capture_item;
	hr = interop->CreateForMonitor(monitor, IID_PPV_ARGS(capture_item.GetAddressOf()));
	if (FAILED(hr)) {
		U_LOG_W("leia_bg_capture: CreateForMonitor failed: 0x%08x", (unsigned)hr);
		reset_affinity();
		return nullptr;
	}

	WG::SizeInt32 item_size = {};
	capture_item->get_Size(&item_size);

	ComPtr<WGC::IDirect3D11CaptureFramePoolStatics2> pool_statics;
	{
		HStringReference cls(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool);
		hr = RoGetActivationFactory(cls.Get(), IID_PPV_ARGS(&pool_statics));
		if (FAILED(hr)) {
			U_LOG_W("leia_bg_capture: FramePool statics unavailable: 0x%08x", (unsigned)hr);
			reset_affinity();
			return nullptr;
		}
	}

	ComPtr<WGC::IDirect3D11CaptureFramePool> frame_pool;
	hr = pool_statics->CreateFreeThreaded(wg_device.Get(),
	                                      WGDX::DirectXPixelFormat_B8G8R8A8UIntNormalized,
	                                      2,
	                                      item_size,
	                                      frame_pool.GetAddressOf());
	if (FAILED(hr)) {
		U_LOG_W("leia_bg_capture: FramePool::CreateFreeThreaded failed: 0x%08x", (unsigned)hr);
		reset_affinity();
		return nullptr;
	}

	ComPtr<WGC::IGraphicsCaptureSession> capture_session;
	hr = frame_pool->CreateCaptureSession(capture_item.Get(), capture_session.GetAddressOf());
	if (FAILED(hr)) {
		U_LOG_W("leia_bg_capture: CreateCaptureSession failed: 0x%08x", (unsigned)hr);
		reset_affinity();
		return nullptr;
	}

	// Don't capture the cursor — we draw our own UI; the desktop cursor under us
	// would be doubled.
	ComPtr<WGC::IGraphicsCaptureSession2> session2;
	if (SUCCEEDED(capture_session.As(&session2))) {
		session2->put_IsCursorCaptureEnabled(false);
	}
	// Suppress the yellow "this screen is being captured" border DWM draws
	// around the captured region. Requires Windows 11 22H2+; on older Windows
	// the QI fails or the put silently no-ops — fine, we just keep the border.
	ComPtr<WGC::IGraphicsCaptureSession3> session3;
	if (SUCCEEDED(capture_session.As(&session3))) {
		session3->put_IsBorderRequired(false);
	}
	// Capture delivery throttle (Suki's measurement, avatar iGPU study
	// 2026-08-10): WGC delivers a frame per DESKTOP CHANGE — monitor capture
	// is served by DWM, so every delivery is dwm-column GPU work, and a
	// moving cursor alone drives it (~13 system GPU points). Capping delivery
	// is FREE when the desktop is quiet (the still-cost is flat across every
	// cap) and only bites when something changes; the trade is band
	// freshness, bounded by the cap. The compose consumer only reads the
	// latest frame, so dropped intermediates cost nothing. 66 ms measured as
	// the knee (~9 points saved under motion). LEIA_DP_CAPTURE_MIN_INTERVAL_MS:
	// unset = 66, 0 = uncapped, N = cap at N ms. Requires
	// IGraphicsCaptureSession5 (Win11 24H2); QI failure = uncapped, as today.
	{
		long interval_ms = 66;
		const char *e = getenv("LEIA_DP_CAPTURE_MIN_INTERVAL_MS");
		if (e != nullptr && e[0] != '\0') {
			interval_ms = atol(e);
			if (interval_ms < 0 || interval_ms > 1000) {
				interval_ms = 66;
			}
		}
		if (interval_ms > 0) {
			ComPtr<WGC::IGraphicsCaptureSession5> session5;
			if (SUCCEEDED(capture_session.As(&session5))) {
				ABI::Windows::Foundation::TimeSpan ts;
				ts.Duration = (INT64)interval_ms * 10000; // 100 ns units
				if (SUCCEEDED(session5->put_MinUpdateInterval(ts))) {
					U_LOG_W("leia_bg_capture: delivery capped at %ld ms "
					        "(LEIA_DP_CAPTURE_MIN_INTERVAL_MS)",
					        interval_ms);
				}
			}
		}
	}

	ComPtr<ID3D11Texture2D> staging_tex;
	HANDLE staging_handle = nullptr;
	hr = create_staging_texture(d3d11_device.Get(), monitor_w, monitor_h,
	                            staging_tex.GetAddressOf(), &staging_handle);
	if (FAILED(hr)) {
		U_LOG_W("leia_bg_capture: staging tex create failed: 0x%08x", (unsigned)hr);
		reset_affinity();
		return nullptr;
	}

	ComPtr<ID3D11Fence> shared_fence;
	HANDLE shared_fence_handle = nullptr;
	hr = create_shared_fence(d3d11_device.Get(), shared_fence.GetAddressOf(), &shared_fence_handle);
	if (FAILED(hr)) {
		U_LOG_W("leia_bg_capture: shared fence create failed: 0x%08x", (unsigned)hr);
		CloseHandle(staging_handle);
		reset_affinity();
		return nullptr;
	}

	ComPtr<ID3D11DeviceContext4> ctx4;
	hr = d3d11_context.As(&ctx4);
	if (FAILED(hr)) {
		U_LOG_W("leia_bg_capture: ID3D11DeviceContext4 QI failed: 0x%08x", (unsigned)hr);
		CloseHandle(shared_fence_handle);
		CloseHandle(staging_handle);
		reset_affinity();
		return nullptr;
	}

	hr = capture_session->StartCapture();
	if (FAILED(hr)) {
		U_LOG_W("leia_bg_capture: StartCapture failed: 0x%08x", (unsigned)hr);
		CloseHandle(shared_fence_handle);
		CloseHandle(staging_handle);
		reset_affinity();
		return nullptr;
	}

	auto *c = new leia_bg_capture();
	c->hwnd = hwnd;
	c->affinity_owned = affinity_owned;
	c->monitor = monitor;
	c->monitor_w = monitor_w;
	c->monitor_h = monitor_h;
	c->monitor_rect = mi.rcMonitor;
	c->d3d11_device = d3d11_device;
	c->d3d11_context = d3d11_context;
	c->d3d11_context4 = ctx4;
	c->wg_device = wg_device;
	c->capture_item = capture_item;
	c->frame_pool = frame_pool;
	c->capture_session = capture_session;
	c->staging_tex = staging_tex;
	c->staging_shared_handle = staging_handle;
	c->adapter_luid = query_device_adapter_luid(d3d11_device.Get());
	c->shared_fence = shared_fence;
	c->shared_fence_handle = shared_fence_handle;
	c->signaled_value = 0;
	c->has_frame = false;

#ifdef LEIA_BG_CAPTURE_HAS_PREVIEW
	c->preview_enabled = !preview_env_disable();
	if (!c->preview_enabled) {
		U_LOG_W("leia_bg_capture: LEIA_DP_DISABLE_BG_PREVIEW=1 ARMED — background preview "
		        "producer off; get_background_preview reports no source and the rear depth "
		        "budget stays at clip-at-the-display-plane");
	}
#endif

	U_LOG_W("leia_bg_capture: ready (monitor=%ux%u, hwnd=0x%p, adapter_luid=0x%016llx)", monitor_w, monitor_h,
	        hwnd, (unsigned long long)c->adapter_luid);

	// #119 — cross-adapter guard: WGC monitor capture from a device on a
	// DIFFERENT adapter than the monitor's scanout owner delivers black or no
	// frames (Optimus boxes: session on the dGPU, panel on the iGPU). That
	// presents downstream as an opaque squared Local2D / dark silhouette
	// fringes and can intermittently work, which makes it a trap — so find
	// the adapter that OWNS the captured monitor's output and WARN loudly on
	// a mismatch. Detection only; the session must be placed on the scanout
	// adapter (DXR_VK_FORCE_GPU / DXR_D3D_FORCE_GPU).
	//
	// Resolution order matters, and getting it wrong inverts the verdict.
	// The DXGI output walk alone is NOT authoritative on a hybrid box: a
	// render-only discrete adapter also enumerates the panel's output, so the
	// walk can name the dGPU as the scanout owner when the iGPU actually
	// drives the panel. Under the #918 weave-on-scanout split the capture
	// device is deliberately placed ON the scanout adapter — so the weak walk
	// reported a CROSS-ADAPTER mismatch precisely when placement was CORRECT,
	// and told the user to move the session to where it already was.
	//
	// Ask the OS display-config database first, exactly as the runtime's own
	// getScanoutAdapter() does (aux/d3d/d3d_scanout_helpers.cpp):
	// DISPLAYCONFIG_PATH_SOURCE_INFO::adapterId is the same LUID space as
	// DXGI_ADAPTER_DESC::AdapterLuid. Keep the output walk as the fallback for
	// when no active display path matches.
	{
		uint64_t scanout_luid = 0;
		const wchar_t *scanout_name = nullptr;
		DXGI_ADAPTER_DESC1 scanout_desc = {};
		bool resolved = false;

		// Primary: the display-config database, keyed by GDI device name.
		MONITORINFOEXW mi_sc = {};
		mi_sc.cbSize = sizeof(mi_sc);
		LUID path_luid = {};
		bool have_path_luid = false;
		if (GetMonitorInfoW(monitor, &mi_sc)) {
			UINT32 path_count = 0, mode_count = 0;
			if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) ==
			    ERROR_SUCCESS) {
				std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
				std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
				if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
				                       modes.data(), nullptr) == ERROR_SUCCESS) {
					paths.resize(path_count);
					for (const DISPLAYCONFIG_PATH_INFO &p : paths) {
						DISPLAYCONFIG_SOURCE_DEVICE_NAME sdn = {};
						sdn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
						sdn.header.size = sizeof(sdn);
						sdn.header.adapterId = p.sourceInfo.adapterId;
						sdn.header.id = p.sourceInfo.id;
						if (DisplayConfigGetDeviceInfo(&sdn.header) != ERROR_SUCCESS) {
							continue;
						}
						if (wcscmp(sdn.viewGdiDeviceName, mi_sc.szDevice) == 0) {
							path_luid = p.sourceInfo.adapterId;
							have_path_luid = true;
							break;
						}
					}
				}
			}
		}

		ComPtr<IDXGIFactory1> fac;
		if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) {
			if (have_path_luid) {
				for (UINT a = 0; !resolved; ++a) {
					ComPtr<IDXGIAdapter1> ad;
					if (fac->EnumAdapters1(a, ad.GetAddressOf()) != S_OK) break;
					DXGI_ADAPTER_DESC1 adsc = {};
					if (FAILED(ad->GetDesc1(&adsc))) continue;
					if (adsc.AdapterLuid.HighPart == path_luid.HighPart &&
					    adsc.AdapterLuid.LowPart == path_luid.LowPart) {
						scanout_desc = adsc;
						scanout_luid = ((uint64_t)(uint32_t)adsc.AdapterLuid.HighPart << 32) |
						               (uint32_t)adsc.AdapterLuid.LowPart;
						scanout_name = scanout_desc.Description;
						resolved = true;
					}
				}
			}

			// Fallback: the output walk. Weaker (see above), but it is the
			// only answer available when no active display path matches.
			if (!resolved) {
				for (UINT a = 0; !resolved; ++a) {
					ComPtr<IDXGIAdapter1> ad;
					if (fac->EnumAdapters1(a, ad.GetAddressOf()) != S_OK) break;
					for (UINT o = 0;; ++o) {
						ComPtr<IDXGIOutput> out;
						if (ad->EnumOutputs(o, out.GetAddressOf()) != S_OK) break;
						DXGI_OUTPUT_DESC od = {};
						if (SUCCEEDED(out->GetDesc(&od)) && od.Monitor == monitor) {
							DXGI_ADAPTER_DESC1 adsc = {};
							ad->GetDesc1(&adsc);
							scanout_desc = adsc;
							scanout_luid =
							    ((uint64_t)(uint32_t)adsc.AdapterLuid.HighPart << 32) |
							    (uint32_t)adsc.AdapterLuid.LowPart;
							scanout_name = scanout_desc.Description;
							resolved = true;
							break;
						}
					}
				}
			}
		}

		if (resolved && scanout_luid != c->adapter_luid) {
			U_LOG_W("leia_bg_capture: CROSS-ADAPTER CAPTURE — session/capture "
			        "adapter 0x%016llx but the monitor is scanned out by "
			        "0x%016llx (%ls). WGC will deliver black/no frames; "
			        "transparent content will break. Place the session on the "
			        "scanout adapter (DXR_VK_FORCE_GPU / DXR_D3D_FORCE_GPU).",
			        (unsigned long long)c->adapter_luid, (unsigned long long)scanout_luid,
			        scanout_name ? scanout_name : L"<unknown>");
		} else if (resolved) {
			U_LOG_I("leia_bg_capture: capture adapter matches the panel's scanout adapter "
			        "0x%016llx (%ls).",
			        (unsigned long long)scanout_luid, scanout_name ? scanout_name : L"<unknown>");
		}
	}
	return c;
}

extern "C" uint64_t
leia_bg_capture_get_adapter_luid(struct leia_bg_capture *c)
{
	return c != nullptr ? c->adapter_luid : 0;
}

extern "C" long
leia_bg_capture_open_d3d11(struct leia_bg_capture *c,
                           ID3D11Device *dev,
                           ID3D11Texture2D **out_tex,
                           ID3D11ShaderResourceView **out_srv)
{
	if (c == nullptr || dev == nullptr || out_tex == nullptr || out_srv == nullptr) {
		return E_INVALIDARG;
	}
	ComPtr<ID3D11Device1> dev1;
	HRESULT hr = dev->QueryInterface(IID_PPV_ARGS(&dev1));
	if (FAILED(hr)) {
		return hr;
	}
	hr = dev1->OpenSharedResource1(c->staging_shared_handle, IID_PPV_ARGS(out_tex));
	if (FAILED(hr)) {
		return hr;
	}
	D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MipLevels = 1;
	return dev->CreateShaderResourceView(*out_tex, &sd, out_srv);
}

extern "C" long
leia_bg_capture_open_d3d12(struct leia_bg_capture *c, ID3D12Device *dev, ID3D12Resource **out_res)
{
	if (c == nullptr || dev == nullptr || out_res == nullptr) {
		return E_INVALIDARG;
	}
	return dev->OpenSharedHandle(c->staging_shared_handle, IID_PPV_ARGS(out_res));
}

extern "C" long
leia_bg_capture_open_fence_d3d11(struct leia_bg_capture *c, ID3D11Device *dev, ID3D11Fence **out_fence)
{
	if (c == nullptr || dev == nullptr || out_fence == nullptr) {
		return E_INVALIDARG;
	}
	ComPtr<ID3D11Device5> dev5;
	HRESULT hr = dev->QueryInterface(IID_PPV_ARGS(&dev5));
	if (FAILED(hr)) {
		return hr;
	}
	return dev5->OpenSharedFence(c->shared_fence_handle, IID_PPV_ARGS(out_fence));
}

extern "C" long
leia_bg_capture_open_fence_d3d12(struct leia_bg_capture *c, ID3D12Device *dev, ID3D12Fence **out_fence)
{
	if (c == nullptr || dev == nullptr || out_fence == nullptr) {
		return E_INVALIDARG;
	}
	return dev->OpenSharedHandle(c->shared_fence_handle, IID_PPV_ARGS(out_fence));
}

extern "C" HANDLE
leia_bg_capture_get_shared_handle(struct leia_bg_capture *c)
{
	return c != nullptr ? c->staging_shared_handle : nullptr;
}

extern "C" void
leia_bg_capture_get_size(struct leia_bg_capture *c, uint32_t *out_w, uint32_t *out_h)
{
	if (c == nullptr) {
		if (out_w) *out_w = 0;
		if (out_h) *out_h = 0;
		return;
	}
	if (out_w) *out_w = c->monitor_w;
	if (out_h) *out_h = c->monitor_h;
}

extern "C" bool
leia_bg_capture_poll(struct leia_bg_capture *c,
                     float out_bg_uv_origin[2],
                     float out_bg_uv_extent[2],
                     uint64_t *out_fence_wait_value)
{
	if (c == nullptr) {
		return false;
	}

	// One-shot delivery diagnostics (#116 debugging): distinguish "WGC never
	// delivers a frame" from "frames arrive but their content is wrong".
	static std::atomic<uint32_t> s_polls{0};
	static std::atomic<bool> s_logged_first{false};
	static std::atomic<bool> s_logged_none{false};
	const uint32_t poll_n = s_polls.fetch_add(1, std::memory_order_relaxed) + 1;

	// Drain framepool, keep newest.
	ComPtr<WGC::IDirect3D11CaptureFrame> latest_frame;
	for (;;) {
		ComPtr<WGC::IDirect3D11CaptureFrame> f;
		HRESULT hr = c->frame_pool->TryGetNextFrame(f.GetAddressOf());
		if (FAILED(hr) || f == nullptr) {
			break;
		}
		latest_frame = std::move(f);
	}
	if (latest_frame != nullptr) {
		bool expected = false;
		if (s_logged_first.compare_exchange_strong(expected, true)) {
			U_LOG_W("leia_bg_capture: first WGC frame drained (poll %u)", poll_n);
		}
	} else if (poll_n == 600 && !c->has_frame.load(std::memory_order_acquire)) {
		bool expected = false;
		if (s_logged_none.compare_exchange_strong(expected, true)) {
			U_LOG_W("leia_bg_capture: NO WGC frames after %u polls — capture session delivering nothing",
			        poll_n);
		}
	}
	if (latest_frame != nullptr) {
		ComPtr<WGD3D::IDirect3DSurface> wg_surface;
		if (SUCCEEDED(latest_frame->get_Surface(wg_surface.GetAddressOf()))) {
			ComPtr<IDxgiAccess> access;
			if (SUCCEEDED(wg_surface.As(&access))) {
				ComPtr<ID3D11Texture2D> wgc_tex;
				if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(&wgc_tex)))) {
					c->d3d11_context->CopyResource(c->staging_tex.Get(), wgc_tex.Get());
					// Signal after the copy queues — consumers wait on this value
					// before sampling the staging tex.
					UINT64 next = c->signaled_value.fetch_add(1, std::memory_order_acq_rel) + 1;
					c->d3d11_context4->Signal(c->shared_fence.Get(), next);
					// Flush the device context so the signal makes it to the GPU
					// queue before the consumer (on a different device) waits.
					c->d3d11_context->Flush();
					c->has_frame.store(true, std::memory_order_release);

					// One-shot content probe (#116 debugging): mean byte of the
					// frame's center 16x16 — 0 means WGC is delivering BLACK
					// frames (the cross-adapter failure mode), >0 means real
					// desktop content is arriving.
					static std::atomic<bool> s_probed{false};
					bool pexp = false;
					if (s_probed.compare_exchange_strong(pexp, true)) {
						D3D11_TEXTURE2D_DESC td = {};
						wgc_tex->GetDesc(&td);
						D3D11_TEXTURE2D_DESC sd = td;
						sd.Width = 16;
						sd.Height = 16;
						sd.MipLevels = 1;
						sd.ArraySize = 1;
						sd.SampleDesc.Count = 1;
						sd.SampleDesc.Quality = 0;
						sd.Usage = D3D11_USAGE_STAGING;
						sd.BindFlags = 0;
						sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
						sd.MiscFlags = 0;
						ComPtr<ID3D11Texture2D> probe;
						ComPtr<ID3D11Device> dev;
						c->d3d11_context->GetDevice(dev.GetAddressOf());
						if (dev != nullptr &&
						    SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, probe.GetAddressOf()))) {
							D3D11_BOX box = {td.Width / 2, td.Height / 2, 0,
							                 td.Width / 2 + 16, td.Height / 2 + 16, 1};
							c->d3d11_context->CopySubresourceRegion(probe.Get(), 0, 0, 0, 0,
							                                        wgc_tex.Get(), 0, &box);
							D3D11_MAPPED_SUBRESOURCE m = {};
							if (SUCCEEDED(c->d3d11_context->Map(probe.Get(), 0, D3D11_MAP_READ,
							                                    0, &m))) {
								uint64_t sum = 0;
								const uint8_t *rows = (const uint8_t *)m.pData;
								for (uint32_t y = 0; y < 16; y++) {
									for (uint32_t x = 0; x < 16 * 4; x++) {
										sum += rows[y * m.RowPitch + x];
									}
								}
								c->d3d11_context->Unmap(probe.Get(), 0);
								U_LOG_W("leia_bg_capture: first-frame center 16x16 mean byte = "
								        "%llu (0 = BLACK frames)",
								        (unsigned long long)(sum / (16 * 16 * 4)));
							}
						}
					}
				}
			}
		}
		ComPtr<WF::IClosable> closable;
		if (SUCCEEDED(latest_frame.As(&closable))) {
			closable->Close();
		}
	}

	if (out_fence_wait_value != nullptr) {
		*out_fence_wait_value = c->signaled_value.load(std::memory_order_acquire);
	}

	// Window-on-monitor rect → normalized UVs.
	HMONITOR cur = MonitorFromWindow(c->hwnd, MONITOR_DEFAULTTONEAREST);
	if (cur != c->monitor) {
		// Cross-monitor move: capture session is still bound to the old monitor.
		// Recreating the session mid-stream is non-trivial; for now skip compose
		// (caller can fall through to opaque-only weave).
		// TODO follow-up: tear-down + re-create on monitor change.
		out_bg_uv_origin[0] = 0;
		out_bg_uv_origin[1] = 0;
		out_bg_uv_extent[0] = 0;
		out_bg_uv_extent[1] = 0;
#ifdef LEIA_BG_CAPTURE_HAS_PREVIEW
		c->poll_ok = false;
#endif
		return false;
	}
	// Compositor's swap chain renders into the window's CLIENT area only —
	// the title bar and borders are non-client and drawn by DWM. Mapping the
	// bg sample to GetWindowRect would shift / scale by the title-bar height.
	// Use GetClientRect + ClientToScreen so the UV rect covers exactly the
	// pixels the atlas's tile content maps onto. Pixel-perfect in both
	// titled-window and borderless cases.
	RECT cr;
	if (!GetClientRect(c->hwnd, &cr)) {
#ifdef LEIA_BG_CAPTURE_HAS_PREVIEW
		c->poll_ok = false;
#endif
		return false;
	}
	POINT tl = {cr.left, cr.top};
	POINT br = {cr.right, cr.bottom};
	if (!ClientToScreen(c->hwnd, &tl) || !ClientToScreen(c->hwnd, &br)) {
#ifdef LEIA_BG_CAPTURE_HAS_PREVIEW
		c->poll_ok = false;
#endif
		return false;
	}
	const float inv_w = 1.0f / (float)c->monitor_w;
	const float inv_h = 1.0f / (float)c->monitor_h;
	out_bg_uv_origin[0] = (float)(tl.x - c->monitor_rect.left) * inv_w;
	out_bg_uv_origin[1] = (float)(tl.y - c->monitor_rect.top) * inv_h;
	out_bg_uv_extent[0] = (float)(br.x - tl.x) * inv_w;
	out_bg_uv_extent[1] = (float)(br.y - tl.y) * inv_h;

	const bool ok = c->has_frame.load(std::memory_order_acquire);

#ifdef LEIA_BG_CAPTURE_HAS_PREVIEW
	// #224 rear depth budget: produce one preview per CAPTURE generation, on
	// the same throttle the WGC delivery already runs at (<= 15 Hz), never per
	// weave. When the desktop is quiet signaled_value does not move and this
	// costs a compare.
	c->poll_ok = ok;
	if (ok) {
		const UINT64 gen = c->signaled_value.load(std::memory_order_acquire);
		if (gen != c->preview_last_src_gen) {
			c->preview_last_src_gen = gen;
			preview_produce(c, tl.x - c->monitor_rect.left, tl.y - c->monitor_rect.top,
			                br.x - tl.x, br.y - tl.y, gen);
		}
	}
#endif

	return ok;
}

extern "C" void
leia_bg_capture_destroy(struct leia_bg_capture *c)
{
	if (c == nullptr) {
		return;
	}
	{
		ComPtr<WF::IClosable> closable;
		if (c->frame_pool != nullptr && SUCCEEDED(c->frame_pool.As(&closable))) {
			closable->Close();
		}
	}
	{
		ComPtr<WF::IClosable> closable;
		if (c->capture_session != nullptr && SUCCEEDED(c->capture_session.As(&closable))) {
			closable->Close();
		}
	}
	if (c->staging_shared_handle != nullptr) {
		CloseHandle(c->staging_shared_handle);
	}
	if (c->shared_fence_handle != nullptr) {
		CloseHandle(c->shared_fence_handle);
	}
	// Only undo the affinity if WE set it — on the cross-process path the
	// runtime owns it (and will clear it) (#551).
	if (c->hwnd != nullptr && c->affinity_owned) {
		SetWindowDisplayAffinity(c->hwnd, WDA_NONE);
	}
	delete c;
}
