// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The Leia conversion provider's D3D11 device + image transport (see
 *         leia_conv_d3d11.h).
 * @ingroup conv_leia
 */

#include "leia_conv_d3d11.h"

#include "util/u_logging.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <cstring>
#include <new>
#include <vector>

namespace {

//! An import unseen for this many calls is dropped (contract §2.4).
constexpr uint64_t kCacheMaxAge = 64;
//! Bound on any CPU wait for the GPU (keyed mutex, drain).
constexpr DWORD kWaitMs = 2000;

template <typename T>
void
safe_release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

const char *kBlitHlsl = R"(
cbuffer P : register(b0) { float U0; float U1; uint Mode; uint Pad; };
Texture2D<float4> Src : register(t0);
SamplerState Lin : register(s0);
SamplerState Pnt : register(s1);
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO vs_main(uint id : SV_VertexID)
{
	VO o;
	float2 t = float2((id << 1) & 2, id & 2);
	o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	o.uv = t;
	return o;
}
float4 ps_main(VO i) : SV_Target
{
	float2 uv = float2(lerp(U0, U1, i.uv.x), i.uv.y);
	if (Mode == 0) {
		return Src.Sample(Lin, uv);
	}
	float r = Src.Sample(Pnt, uv).r;
	if (Mode == 1) {
		r = 1.0 - r;
	}
	return float4(r, r, r, 1.0);
}
)";

struct cached_tex
{
	uint64_t key;
	ID3D11Texture2D *tex;
	IDXGIKeyedMutex *km; //!< non-NULL when the resource carries a keyed mutex
	uint64_t last_used;
};

struct cached_fence
{
	uint64_t key;
	ID3D11Fence *fence;
	uint64_t last_used;
};

struct scratch_rt
{
	uint32_t w, h;
	DXGI_FORMAT res_fmt; //!< resource format (TYPELESS for 8-bit colour)
	DXGI_FORMAT rtv_fmt;
	ID3D11Texture2D *tex;
	ID3D11RenderTargetView *rtv;
};

struct pending_release
{
	uint32_t kind; //!< XRT_CONV_SYNC_*
	ID3D11Fence *fence;
	IDXGIKeyedMutex *km;
	uint64_t value;
};

DXGI_FORMAT
dxgi_of(uint32_t f)
{
	switch (f) {
	case XRT_CONV_FMT_RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case XRT_CONV_FMT_RGBA8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	case XRT_CONV_FMT_BGRA8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case XRT_CONV_FMT_BGRA8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	case XRT_CONV_FMT_R8_UNORM: return DXGI_FORMAT_R8_UNORM;
	case XRT_CONV_FMT_R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
	case XRT_CONV_FMT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
	case XRT_CONV_FMT_RGBA16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case XRT_CONV_FMT_R16_UNORM: return DXGI_FORMAT_R16_UNORM;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

uint32_t
bytes_of(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R8_UNORM: return 1;
	case DXGI_FORMAT_R16_FLOAT:
	case DXGI_FORMAT_R16_UNORM: return 2;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
	default: return 4;
	}
}

//! 8-bit colour family -> TYPELESS (copies between the UNORM / SRGB views of one family are legal).
DXGI_FORMAT
family_of(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
	default: return f;
	}
}

//! Render-target view format for a resource format: sRGB bytes are written as they are (UNORM view).
DXGI_FORMAT
rtv_of(DXGI_FORMAT res)
{
	switch (res) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	default: return res;
	}
}

uint64_t
luid_u64(const LUID &l)
{
	return ((uint64_t)(uint32_t)l.HighPart << 32) | (uint64_t)l.LowPart;
}

} // namespace

struct leia_conv_d3d11
{
	uint64_t luid = 0;
	ID3D11Device *dev = nullptr;
	ID3D11Device1 *dev1 = nullptr;
	ID3D11Device5 *dev5 = nullptr; //!< fences (Windows 10 1703+); NULL = no FENCE_NT
	ID3D11DeviceContext *ctx = nullptr;
	ID3D11DeviceContext4 *ctx4 = nullptr;
	ID3D11Query *done = nullptr;
	ID3D11VertexShader *vs = nullptr;
	ID3D11PixelShader *ps = nullptr;
	ID3D11Buffer *cb = nullptr;
	ID3D11SamplerState *lin = nullptr;
	ID3D11SamplerState *pnt = nullptr;
	ID3D11RasterizerState *rs = nullptr;

	uint64_t tick = 0;
	std::vector<cached_tex> textures;
	std::vector<cached_fence> fences;
	std::vector<scratch_rt> rts;
	std::vector<pending_release> pending;
	bool drain_needed = false;
	bool lost = false;

	ID3D11Texture2D *in_tex = nullptr; //!< flat input copy
	D3D11_TEXTURE2D_DESC in_desc = {};
	ID3D11Texture2D *staging = nullptr; //!< CPU-target readback
	D3D11_TEXTURE2D_DESC staging_desc = {};
};

namespace {

void
check_lost(leia_conv_d3d11 *d)
{
	if (d->dev != nullptr && !d->lost && d->dev->GetDeviceRemovedReason() != S_OK) {
		d->lost = true;
		U_LOG_W("Leia conversion: the provider's D3D11 device was removed (0x%08lx) — reporting DEVICE_LOST",
		        (unsigned long)d->dev->GetDeviceRemovedReason());
	}
}

void
age_cache(leia_conv_d3d11 *d)
{
	d->tick++;
	for (size_t i = 0; i < d->textures.size();) {
		if (d->tick - d->textures[i].last_used > kCacheMaxAge) {
			safe_release(d->textures[i].km);
			safe_release(d->textures[i].tex);
			d->textures.erase(d->textures.begin() + (ptrdiff_t)i);
		} else {
			i++;
		}
	}
	for (size_t i = 0; i < d->fences.size();) {
		if (d->tick - d->fences[i].last_used > kCacheMaxAge) {
			safe_release(d->fences[i].fence);
			d->fences.erase(d->fences.begin() + (ptrdiff_t)i);
		} else {
			i++;
		}
	}
}

cached_tex *
open_texture(leia_conv_d3d11 *d, const struct xrt_conv_image *img)
{
	if (img->memory_kind != XRT_CONV_MEM_WIN32_NT_HANDLE || img->handle == 0) {
		return nullptr;
	}
	if (img->import_key != 0) {
		for (auto &c : d->textures) {
			if (c.key == img->import_key) {
				c.last_used = d->tick;
				return &c;
			}
		}
	}
	ID3D11Texture2D *tex = nullptr;
	HRESULT hr = d->dev1->OpenSharedResource1(reinterpret_cast<HANDLE>(img->handle), __uuidof(ID3D11Texture2D),
	                                          reinterpret_cast<void **>(&tex));
	if (FAILED(hr) || tex == nullptr) {
		U_LOG_W("Leia conversion: OpenSharedResource1 failed (0x%08lx) — image on adapter %016llx, provider on "
		        "%016llx",
		        (unsigned long)hr, (unsigned long long)img->adapter_luid, (unsigned long long)d->luid);
		return nullptr;
	}
	IDXGIKeyedMutex *km = nullptr;
	if (img->sync.kind == XRT_CONV_SYNC_KEYED_MUTEX) {
		tex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(&km));
	}
	// A keyless import is kept for this call only (age 0 ages out at once).
	d->textures.push_back({img->import_key, tex, km, img->import_key != 0 ? d->tick : 0});
	return &d->textures.back();
}

ID3D11Fence *
open_fence(leia_conv_d3d11 *d, const struct xrt_conv_sync *sync)
{
	if (sync->kind != XRT_CONV_SYNC_FENCE_NT || sync->handle == 0 || d->dev5 == nullptr || d->ctx4 == nullptr) {
		return nullptr;
	}
	if (sync->import_key != 0) {
		for (auto &c : d->fences) {
			if (c.key == sync->import_key) {
				c.last_used = d->tick;
				return c.fence;
			}
		}
	}
	ID3D11Fence *f = nullptr;
	if (FAILED(d->dev5->OpenSharedFence(reinterpret_cast<HANDLE>(sync->handle), __uuidof(ID3D11Fence),
	                                    reinterpret_cast<void **>(&f)))) {
		return nullptr;
	}
	d->fences.push_back({sync->import_key, f, sync->import_key != 0 ? d->tick : 0});
	return f;
}

//! Wait the image's acquire point (GPU wait for a fence, CPU for a keyed mutex).
bool
acquire(leia_conv_d3d11 *d, const struct xrt_conv_image *img, cached_tex *t)
{
	switch (img->sync.kind) {
	case XRT_CONV_SYNC_NONE: return true;
	case XRT_CONV_SYNC_FENCE_NT: {
		ID3D11Fence *f = open_fence(d, &img->sync);
		return f != nullptr && SUCCEEDED(d->ctx4->Wait(f, img->sync.acquire_value));
	}
	case XRT_CONV_SYNC_KEYED_MUTEX: {
		if (t == nullptr || t->km == nullptr) {
			return false;
		}
		const HRESULT hr = t->km->AcquireSync(img->sync.acquire_value, kWaitMs);
		if (hr != S_OK) {
			U_LOG_W("Leia conversion: keyed mutex AcquireSync(%llu) -> 0x%08lx",
			        (unsigned long long)img->sync.acquire_value, (unsigned long)hr);
			return false;
		}
		return true;
	}
	default: return false;
	}
}

//! Queue the image's release (signalled by finish, or at once for an input).
void
queue_release(leia_conv_d3d11 *d, const struct xrt_conv_image *img, cached_tex *t)
{
	pending_release r = {img->sync.kind, nullptr, nullptr, img->sync.release_value};
	if (img->sync.kind == XRT_CONV_SYNC_FENCE_NT) {
		r.fence = open_fence(d, &img->sync);
	} else if (img->sync.kind == XRT_CONV_SYNC_KEYED_MUTEX && t != nullptr) {
		r.km = t->km;
	} else {
		d->drain_needed = true;
		return;
	}
	d->pending.push_back(r);
}

bool
drain(leia_conv_d3d11 *d)
{
	d->ctx->End(d->done);
	d->ctx->Flush();
	const ULONGLONG t0 = GetTickCount64();
	HRESULT hr;
	while ((hr = d->ctx->GetData(d->done, nullptr, 0, 0)) == S_FALSE) {
		if (GetTickCount64() - t0 > kWaitMs) {
			U_LOG_W("Leia conversion: GPU drain timed out (%lu ms)", (unsigned long)kWaitMs);
			check_lost(d);
			return false;
		}
		Sleep(0);
	}
	if (hr != S_OK) {
		check_lost(d);
		return false;
	}
	return true;
}

bool
signal_pending(leia_conv_d3d11 *d)
{
	bool ok = true;
	for (const auto &r : d->pending) {
		if (r.kind == XRT_CONV_SYNC_FENCE_NT) {
			ok = ok && r.fence != nullptr && SUCCEEDED(d->ctx4->Signal(r.fence, r.value));
		} else if (r.kind == XRT_CONV_SYNC_KEYED_MUTEX && r.km != nullptr) {
			ok = ok && SUCCEEDED(r.km->ReleaseSync(r.value));
		}
	}
	d->pending.clear();
	return ok;
}

scratch_rt *
get_rt(leia_conv_d3d11 *d, uint32_t w, uint32_t h, DXGI_FORMAT target_fmt)
{
	const DXGI_FORMAT res = family_of(target_fmt);
	for (auto &r : d->rts) {
		if (r.w == w && r.h == h && r.res_fmt == res) {
			return &r;
		}
	}
	if (d->rts.size() >= 6) { // a handful of live (size, format) pairs; drop the oldest
		safe_release(d->rts.front().rtv);
		safe_release(d->rts.front().tex);
		d->rts.erase(d->rts.begin());
	}
	scratch_rt r = {w, h, res, rtv_of(res), nullptr, nullptr};
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = res;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET;
	D3D11_RENDER_TARGET_VIEW_DESC rd = {};
	rd.Format = r.rtv_fmt;
	rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	if (FAILED(d->dev->CreateTexture2D(&td, nullptr, &r.tex)) ||
	    FAILED(d->dev->CreateRenderTargetView(r.tex, &rd, &r.rtv))) {
		U_LOG_W("Leia conversion: render target %ux%u (DXGI %u) failed", w, h, (unsigned)res);
		safe_release(r.tex);
		check_lost(d);
		return nullptr;
	}
	d->rts.push_back(r);
	return &d->rts.back();
}

bool
draw(leia_conv_d3d11 *d, scratch_rt *rt, ID3D11Texture2D *src, float u0, float u1, uint32_t mode)
{
	ID3D11ShaderResourceView *srv = nullptr;
	if (FAILED(d->dev->CreateShaderResourceView(src, nullptr, &srv))) {
		U_LOG_W("Leia conversion: SRV over the module's output failed");
		return false;
	}
	D3D11_MAPPED_SUBRESOURCE m = {};
	if (FAILED(d->ctx->Map(d->cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
		srv->Release();
		return false;
	}
	float *pf = static_cast<float *>(m.pData);
	pf[0] = u0;
	pf[1] = u1;
	uint32_t *pu = static_cast<uint32_t *>(m.pData);
	pu[2] = mode;
	pu[3] = 0;
	d->ctx->Unmap(d->cb, 0);

	D3D11_VIEWPORT vp = {0.0f, 0.0f, (float)rt->w, (float)rt->h, 0.0f, 1.0f};
	ID3D11SamplerState *samp[2] = {d->lin, d->pnt};
	d->ctx->OMSetRenderTargets(1, &rt->rtv, nullptr);
	d->ctx->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
	d->ctx->OMSetDepthStencilState(nullptr, 0);
	d->ctx->RSSetState(d->rs);
	d->ctx->RSSetViewports(1, &vp);
	d->ctx->IASetInputLayout(nullptr);
	d->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	d->ctx->VSSetShader(d->vs, nullptr, 0);
	d->ctx->PSSetShader(d->ps, nullptr, 0);
	d->ctx->PSSetConstantBuffers(0, 1, &d->cb);
	d->ctx->PSSetShaderResources(0, 1, &srv);
	d->ctx->PSSetSamplers(0, 2, samp);
	d->ctx->Draw(3, 0);
	ID3D11ShaderResourceView *nsrv = nullptr;
	ID3D11RenderTargetView *nrtv = nullptr;
	d->ctx->PSSetShaderResources(0, 1, &nsrv);
	d->ctx->OMSetRenderTargets(1, &nrtv, nullptr);
	srv->Release();
	return true;
}

bool
ensure_in_tex(leia_conv_d3d11 *d, uint32_t w, uint32_t h, DXGI_FORMAT family)
{
	if (d->in_tex != nullptr && d->in_desc.Width == w && d->in_desc.Height == h && d->in_desc.Format == family) {
		return true;
	}
	safe_release(d->in_tex);
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = family;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(d->dev->CreateTexture2D(&td, nullptr, &d->in_tex))) {
		U_LOG_W("Leia conversion: input texture %ux%u failed", w, h);
		check_lost(d);
		return false;
	}
	d->in_desc = td;
	return true;
}

bool
ensure_staging(leia_conv_d3d11 *d, uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
	if (d->staging != nullptr && d->staging_desc.Width == w && d->staging_desc.Height == h &&
	    d->staging_desc.Format == fmt) {
		return true;
	}
	safe_release(d->staging);
	D3D11_TEXTURE2D_DESC sd = {};
	sd.Width = w;
	sd.Height = h;
	sd.MipLevels = 1;
	sd.ArraySize = 1;
	sd.Format = fmt;
	sd.SampleDesc.Count = 1;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(d->dev->CreateTexture2D(&sd, nullptr, &d->staging))) {
		check_lost(d);
		return false;
	}
	d->staging_desc = sd;
	return true;
}

IDXGIAdapter1 *
find_adapter(uint64_t luid)
{
	if (luid == 0) {
		return nullptr;
	}
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) {
		return nullptr;
	}
	IDXGIAdapter1 *found = nullptr;
	IDXGIAdapter1 *a = nullptr;
	for (UINT i = 0; found == nullptr && factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 desc = {};
		if (SUCCEEDED(a->GetDesc1(&desc)) && luid_u64(desc.AdapterLuid) == luid) {
			found = a;
		} else {
			a->Release();
		}
	}
	factory->Release();
	return found;
}

bool
compile(ID3D11Device *dev, const char *entry, const char *target, ID3DBlob **out)
{
	ID3DBlob *err = nullptr;
	HRESULT hr = D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "leia_conv_blit", nullptr, nullptr, entry, target, 0, 0, out,
	                        &err);
	if (FAILED(hr)) {
		U_LOG_E("Leia conversion: blit shader %s failed: %s", entry,
		        err != nullptr ? (const char *)err->GetBufferPointer() : "(no log)");
		safe_release(err);
		return false;
	}
	safe_release(err);
	(void)dev;
	return true;
}

} // namespace

extern "C" struct leia_conv_d3d11 *
leia_conv_d3d11_create(uint64_t preferred_luid, uint64_t avoid_luid)
{
	auto *d = new (std::nothrow) leia_conv_d3d11();
	if (d == nullptr) {
		return nullptr;
	}
	IDXGIAdapter1 *adapter = (preferred_luid != 0 && preferred_luid != avoid_luid) ? find_adapter(preferred_luid)
	                                                                                 : nullptr;
	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	HRESULT hr = D3D11CreateDevice(adapter, adapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
	                               nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION, &d->dev,
	                               nullptr, &d->ctx);
	safe_release(adapter);
	if (FAILED(hr) || d->dev == nullptr ||
	    FAILED(d->dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&d->dev1)))) {
		U_LOG_W("Leia conversion: no D3D11.1 device (0x%08lx)", (unsigned long)hr);
		leia_conv_d3d11_destroy(&d);
		return nullptr;
	}
	d->dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&d->dev5));
	d->ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&d->ctx4));
	ID3D11Multithread *mt = nullptr;
	if (SUCCEEDED(d->ctx->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void **>(&mt))) &&
	    mt != nullptr) {
		mt->SetMultithreadProtected(TRUE);
		mt->Release();
	}

	IDXGIDevice *dxgi = nullptr;
	if (SUCCEEDED(d->dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi)))) {
		IDXGIAdapter *ad = nullptr;
		if (SUCCEEDED(dxgi->GetAdapter(&ad)) && ad != nullptr) {
			DXGI_ADAPTER_DESC desc = {};
			if (SUCCEEDED(ad->GetDesc(&desc))) {
				d->luid = luid_u64(desc.AdapterLuid);
			}
			ad->Release();
		}
		dxgi->Release();
	}

	ID3DBlob *vsb = nullptr, *psb = nullptr;
	D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = 16;
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	D3D11_SAMPLER_DESC sd = {};
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	D3D11_SAMPLER_DESC sp = sd;
	sp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	D3D11_RASTERIZER_DESC rsd = {};
	rsd.FillMode = D3D11_FILL_SOLID;
	rsd.CullMode = D3D11_CULL_NONE;
	rsd.DepthClipEnable = TRUE;
	const bool ok = compile(d->dev, "vs_main", "vs_5_0", &vsb) && compile(d->dev, "ps_main", "ps_5_0", &psb) &&
	                SUCCEEDED(d->dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr,
	                                                     &d->vs)) &&
	                SUCCEEDED(d->dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr,
	                                                    &d->ps)) &&
	                SUCCEEDED(d->dev->CreateQuery(&qd, &d->done)) && SUCCEEDED(d->dev->CreateBuffer(&bd, nullptr, &d->cb)) &&
	                SUCCEEDED(d->dev->CreateSamplerState(&sd, &d->lin)) &&
	                SUCCEEDED(d->dev->CreateSamplerState(&sp, &d->pnt)) &&
	                SUCCEEDED(d->dev->CreateRasterizerState(&rsd, &d->rs));
	safe_release(vsb);
	safe_release(psb);
	if (!ok) {
		U_LOG_W("Leia conversion: blit pipeline setup failed");
		leia_conv_d3d11_destroy(&d);
		return nullptr;
	}
	U_LOG_W("Leia conversion: own D3D11 device on adapter %016llx (FL 0x%x)%s", (unsigned long long)d->luid,
	        (unsigned)d->dev->GetFeatureLevel(),
	        d->dev5 == nullptr ? " — no shared fences on this OS: FENCE_NT images are refused" : "");
	return d;
}

extern "C" void
leia_conv_d3d11_destroy(struct leia_conv_d3d11 **pd)
{
	if (pd == nullptr || *pd == nullptr) {
		return;
	}
	leia_conv_d3d11 *d = *pd;
	*pd = nullptr;
	leia_conv_d3d11_clear_cache(d);
	for (auto &r : d->rts) {
		safe_release(r.rtv);
		safe_release(r.tex);
	}
	d->rts.clear();
	safe_release(d->in_tex);
	safe_release(d->staging);
	safe_release(d->rs);
	safe_release(d->pnt);
	safe_release(d->lin);
	safe_release(d->cb);
	safe_release(d->ps);
	safe_release(d->vs);
	safe_release(d->done);
	safe_release(d->ctx4);
	safe_release(d->ctx);
	safe_release(d->dev5);
	safe_release(d->dev1);
	safe_release(d->dev);
	delete d;
}

extern "C" uint64_t
leia_conv_d3d11_luid(const struct leia_conv_d3d11 *d)
{
	return d != nullptr ? d->luid : 0;
}

extern "C" void *
leia_conv_d3d11_context(struct leia_conv_d3d11 *d)
{
	return d != nullptr ? d->ctx : nullptr;
}

extern "C" bool
leia_conv_d3d11_lost(struct leia_conv_d3d11 *d)
{
	if (d == nullptr) {
		return false;
	}
	check_lost(d);
	return d->lost;
}

extern "C" void
leia_conv_d3d11_clear_cache(struct leia_conv_d3d11 *d)
{
	if (d == nullptr) {
		return;
	}
	d->pending.clear();
	for (auto &c : d->textures) {
		safe_release(c.km);
		safe_release(c.tex);
	}
	d->textures.clear();
	for (auto &c : d->fences) {
		safe_release(c.fence);
	}
	d->fences.clear();
}

extern "C" bool
leia_conv_d3d11_import_input(
    struct leia_conv_d3d11 *d, const struct xrt_conv_image *img, void **out_tex, uint32_t *out_w, uint32_t *out_h)
{
	if (d == nullptr || img == nullptr || out_tex == nullptr || out_w == nullptr || out_h == nullptr || d->lost) {
		return false;
	}
	*out_tex = nullptr;
	age_cache(d);
	const uint32_t w = img->width, h = img->height;
	if (w == 0 || h == 0 || w > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || h > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
		return false;
	}

	if (img->memory_kind == XRT_CONV_MEM_CPU) {
		const DXGI_FORMAT f = dxgi_of(img->format);
		const DXGI_FORMAT fam = family_of(f);
		if (img->cpu_ptr == nullptr ||
		    (fam != DXGI_FORMAT_R8G8B8A8_TYPELESS && fam != DXGI_FORMAT_B8G8R8A8_TYPELESS)) {
			U_LOG_W("Leia conversion: CPU input format %u not RGBA8 / BGRA8", img->format);
			return false;
		}
		const uint32_t stride = img->row_stride != 0 ? img->row_stride : w * 4u;
		if (stride < w * 4u || (img->byte_size != 0 && (uint64_t)stride * (h - 1) + (uint64_t)w * 4u > img->byte_size)) {
			U_LOG_W("Leia conversion: CPU input %ux%u stride %u exceeds its %llu bytes", w, h, stride,
			        (unsigned long long)img->byte_size);
			return false;
		}
		if (!ensure_in_tex(d, w, h, fam)) {
			return false;
		}
		D3D11_BOX box = {0, 0, 0, w, h, 1};
		d->ctx->UpdateSubresource(d->in_tex, 0, &box, img->cpu_ptr, stride, 0);
	} else if (img->memory_kind == XRT_CONV_MEM_WIN32_NT_HANDLE) {
		cached_tex *t = open_texture(d, img);
		if (t == nullptr) {
			check_lost(d);
			return false;
		}
		D3D11_TEXTURE2D_DESC td = {};
		t->tex->GetDesc(&td);
		const DXGI_FORMAT fam = family_of(td.Format);
		if ((fam != DXGI_FORMAT_R8G8B8A8_TYPELESS && fam != DXGI_FORMAT_B8G8R8A8_TYPELESS) ||
		    td.SampleDesc.Count != 1 || td.Width < w || td.Height < h || img->array_layer >= td.ArraySize) {
			U_LOG_W("Leia conversion: unsupported input texture (DXGI %u, %ux%u x%u, layer %u, asked %ux%u)",
			        (unsigned)td.Format, td.Width, td.Height, td.ArraySize, img->array_layer, w, h);
			return false;
		}
		if (!ensure_in_tex(d, w, h, fam) || !acquire(d, img, t)) {
			return false;
		}
		D3D11_BOX box = {0, 0, 0, w, h, 1};
		d->ctx->CopySubresourceRegion(d->in_tex, 0, 0, 0, 0, t->tex,
		                              D3D11CalcSubresource(0, img->array_layer, td.MipLevels), &box);
		// Release the input as soon as the copy is queued: the producer may
		// reuse it while the conversion runs. (SYNC_NONE: nothing to signal.)
		if (img->sync.kind != XRT_CONV_SYNC_NONE) {
			queue_release(d, img, t);
			if (!signal_pending(d)) {
				U_LOG_W("Leia conversion: input release signal failed");
			}
		}
	} else {
		U_LOG_W("Leia conversion: input memory kind 0x%x not importable", img->memory_kind);
		return false;
	}
	*out_tex = d->in_tex;
	*out_w = w;
	*out_h = h;
	return true;
}

extern "C" bool
leia_conv_d3d11_write(struct leia_conv_d3d11 *d,
                      const struct xrt_conv_image *img,
                      uint32_t layer,
                      void *src,
                      float u0,
                      float u1,
                      uint32_t mode)
{
	if (d == nullptr || img == nullptr || src == nullptr || d->lost || img->width == 0 || img->height == 0) {
		return false;
	}
	age_cache(d);
	auto *src_tex = static_cast<ID3D11Texture2D *>(src);
	const uint32_t w = img->width, h = img->height;

	if (img->memory_kind == XRT_CONV_MEM_CPU) {
		const DXGI_FORMAT f = dxgi_of(img->format);
		if (f == DXGI_FORMAT_UNKNOWN || img->cpu_ptr == nullptr || layer != 0) {
			return false;
		}
		const uint32_t bpp = bytes_of(f);
		const uint32_t stride = img->row_stride != 0 ? img->row_stride : w * bpp;
		if (stride < w * bpp || (img->byte_size != 0 && (uint64_t)stride * (h - 1) + (uint64_t)w * bpp > img->byte_size)) {
			return false;
		}
		scratch_rt *rt = get_rt(d, w, h, f);
		if (rt == nullptr || !draw(d, rt, src_tex, u0, u1, mode) || !ensure_staging(d, w, h, rt->res_fmt)) {
			return false;
		}
		d->ctx->CopyResource(d->staging, rt->tex);
		D3D11_MAPPED_SUBRESOURCE map = {};
		if (FAILED(d->ctx->Map(d->staging, 0, D3D11_MAP_READ, 0, &map))) {
			check_lost(d);
			return false;
		}
		for (uint32_t y = 0; y < h; y++) {
			memcpy(static_cast<uint8_t *>(img->cpu_ptr) + (size_t)y * stride,
			       static_cast<const uint8_t *>(map.pData) + (size_t)y * map.RowPitch, (size_t)w * bpp);
		}
		d->ctx->Unmap(d->staging, 0);
		return true;
	}

	cached_tex *t = open_texture(d, img);
	if (t == nullptr) {
		check_lost(d);
		return false;
	}
	D3D11_TEXTURE2D_DESC td = {};
	t->tex->GetDesc(&td);
	if (layer >= td.ArraySize || td.SampleDesc.Count != 1 || td.Usage != D3D11_USAGE_DEFAULT) {
		U_LOG_W("Leia conversion: target layer %u of %u (samples %u, usage %u) not writable", layer, td.ArraySize,
		        td.SampleDesc.Count, (unsigned)td.Usage);
		return false;
	}
	const uint32_t cw = w < td.Width ? w : td.Width;
	const uint32_t ch = h < td.Height ? h : td.Height;
	scratch_rt *rt = get_rt(d, cw, ch, td.Format);
	if (rt == nullptr || !draw(d, rt, src_tex, u0, u1, mode)) {
		return false;
	}
	if (!acquire(d, img, t)) {
		return false;
	}
	D3D11_BOX box = {0, 0, 0, cw, ch, 1};
	d->ctx->CopySubresourceRegion(t->tex, D3D11CalcSubresource(0, layer, td.MipLevels), 0, 0, 0, rt->tex, 0, &box);
	queue_release(d, img, t);
	return true;
}

extern "C" bool
leia_conv_d3d11_finish(struct leia_conv_d3d11 *d)
{
	if (d == nullptr || d->ctx == nullptr) {
		return false;
	}
	bool ok = signal_pending(d);
	if (d->drain_needed) {
		d->drain_needed = false;
		ok = drain(d) && ok;
	} else {
		d->ctx->Flush();
	}
	// Keyless imports were for this call only.
	for (size_t i = 0; i < d->textures.size();) {
		if (d->textures[i].key == 0) {
			safe_release(d->textures[i].km);
			safe_release(d->textures[i].tex);
			d->textures.erase(d->textures.begin() + (ptrdiff_t)i);
		} else {
			i++;
		}
	}
	check_lost(d);
	return ok && !d->lost;
}
