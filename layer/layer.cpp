// Archan VR Capture - OpenXR API layer (picture only).
//
// Sits between the game and any OpenXR runtime (VDXR, Pimax, SteamVR...).
// PimaxHome is skipped (never sent to OBS). With Pimax quad views, view 0 is the wide left view.
// On every xrEndFrame it takes the first projection layer, view 0 (= left eye),
// and copies that eye's imageRect from the swapchain image the game just
// released into a D3D11 shared texture (keyed mutex). The OBS plugin reads it.
// Only copies while OBS is actually rendering the source (reader heartbeat).
// Graphics API: D3D11 only. Other APIs are passed through untouched and logged.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <d3d11.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <dxgi.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/qvd_shared.h"

#define LAYER_NAME "XR_APILAYER_ARCHAN_vr_capture"

namespace {

// ---------------------------------------------------------------- logging
std::mutex g_logMutex;
std::wstring g_logPath;

std::wstring ExeName()
{
	wchar_t p[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, p, MAX_PATH);
	const wchar_t *s = wcsrchr(p, L'\\');
	return s ? s + 1 : p;
}

void InitLogPath()
{
	if (!g_logPath.empty())
		return;
	PWSTR base = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
		std::wstring dir = std::wstring(base) + L"\\ArchanVRCapture";
		CreateDirectoryW(dir.c_str(), nullptr);
		dir += L"\\logs";
		CreateDirectoryW(dir.c_str(), nullptr);
		g_logPath = dir + L"\\layer.log";
		CoTaskMemFree(base);
	}
}

void Log(const char *fmt, ...)
{
	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	std::lock_guard<std::mutex> lock(g_logMutex);
	InitLogPath();
	if (g_logPath.empty())
		return;
	FILE *f = _wfopen(g_logPath.c_str(), L"a");
	if (!f)
		return;
	SYSTEMTIME t;
	GetLocalTime(&t);
	fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d.%03d [pid %lu %ls] %s\n", t.wYear, t.wMonth, t.wDay, t.wHour,
		t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId(), ExeName().c_str(), msg);
	fclose(f);
}

// ---------------------------------------------------------------- next layer functions
PFN_xrGetInstanceProcAddr next_GetInstanceProcAddr = nullptr;
PFN_xrDestroyInstance next_DestroyInstance = nullptr;
PFN_xrGetInstanceProperties next_GetInstanceProperties = nullptr;
PFN_xrCreateSession next_CreateSession = nullptr;
PFN_xrDestroySession next_DestroySession = nullptr;
PFN_xrCreateSwapchain next_CreateSwapchain = nullptr;
PFN_xrDestroySwapchain next_DestroySwapchain = nullptr;
PFN_xrEnumerateSwapchainImages next_EnumerateSwapchainImages = nullptr;
PFN_xrAcquireSwapchainImage next_AcquireSwapchainImage = nullptr;
PFN_xrReleaseSwapchainImage next_ReleaseSwapchainImage = nullptr;
PFN_xrEndFrame next_EndFrame = nullptr;

// ---------------------------------------------------------------- state
struct SwapchainInfo {
	int64_t format = 0;
	uint32_t width = 0, height = 0, arraySize = 0, sampleCount = 0;
	XrSwapchainUsageFlags usage = 0;
	// D3D11 session: the runtime's textures (not owned).
	// D3D12 session: D3D11On12 wrappers of the runtime's ID3D12Resources (owned, see ReleaseImages).
	std::vector<ID3D11Texture2D *> images;
	bool ownsImages = false;
	std::deque<uint32_t> acquired;
	int lastReleased = -1;
};

std::mutex g_mutex;
XrInstance g_instance = XR_NULL_HANDLE;
std::string g_appName;

// Apps that are never sent to OBS (user decision 2026-11-05: skip PimaxHome).
// PimaxHome: exe 'PimaxHome-Win64-Shipping.exe', OpenXR app name 'PimaxHome' (layer.log).
bool IsSkippedApp()
{
	const std::wstring exe = ExeName();
	return _wcsnicmp(exe.c_str(), L"PimaxHome", 9) == 0 || _stricmp(g_appName.c_str(), "PimaxHome") == 0;
}
std::string g_runtimeName;
XrSession g_session = XR_NULL_HANDLE;
ID3D11Device *g_device = nullptr; // AddRef'd. D3D12 session: our D3D11On12 device on the game's queue
ID3D11DeviceContext *g_context = nullptr;
ID3D11On12Device *g_on12 = nullptr; // only in a D3D12 session
std::unordered_map<XrSwapchain, SwapchainInfo> g_swapchains;

void ReleaseImages(SwapchainInfo &s)
{
	if (s.ownsImages)
		for (ID3D11Texture2D *t : s.images)
			if (t)
				t->Release();
	s.images.clear();
	s.ownsImages = false;
}

void ClearSwapchains()
{
	for (auto &kv : g_swapchains)
		ReleaseImages(kv.second);
	g_swapchains.clear();
}

void ReleaseDevice()
{
	if (g_context) {
		if (g_on12) { // our own D3D11On12 context: finish our work. Never touch a D3D11 game's context state.
			g_context->ClearState();
			g_context->Flush();
		}
		g_context->Release();
		g_context = nullptr;
	}
	if (g_on12) {
		g_on12->Release();
		g_on12 = nullptr;
	}
	if (g_device) {
		g_device->Release();
		g_device = nullptr;
	}
}

// shared memory + shared texture
HANDLE g_shmHandle = nullptr;
QvdShared *g_shm = nullptr;
ID3D11Texture2D *g_sharedTex = nullptr;
IDXGIKeyedMutex *g_sharedMutex = nullptr;
uint32_t g_sharedW = 0, g_sharedH = 0, g_sharedFmt = 0, g_sharedSwapFmt = 0;
uint32_t g_sharedHandleVal = 0;
uint32_t g_sharedFlags = 0;
bool g_loggedNotOwner = false;
bool g_loggedFirstCopy = false;
bool g_loggedMsaa = false;
bool g_loggedFormat = false;
bool g_loggedNoProjection = false;
bool g_readerWasActive = false;
uint64_t g_readerActiveSinceMs = 0; // when this process last saw OBS start asking for the picture
uint64_t g_copiedFrames = 0;

void ShmWriteBegin()
{
	g_shm->seq = g_shm->seq + 1; // odd
	MemoryBarrier();
}
void ShmWriteEnd()
{
	MemoryBarrier();
	g_shm->seq = g_shm->seq + 1; // even
}

bool OwnsChannel()
{
	return g_shm && g_shm->writerPid == GetCurrentProcessId();
}

// True if the descriptor names another process that is still running.
bool OtherWriterAlive()
{
	const uint32_t pid = g_shm ? g_shm->writerPid : 0;
	if (pid == 0 || pid == GetCurrentProcessId())
		return false;
	HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
	if (!p)
		return GetLastError() == ERROR_ACCESS_DENIED; // exists but protected
	const bool alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
	CloseHandle(p);
	return alive;
}

bool OpenShm()
{
	if (g_shm)
		return true;
	SECURITY_DESCRIPTOR sd;
	InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
	SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE); // OBS may run at another elevation
	SECURITY_ATTRIBUTES sa = {sizeof(sa), &sd, FALSE};
	g_shmHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(QvdShared), qvd_channel_name());
	if (!g_shmHandle) {
		Log("CreateFileMapping failed, error %lu", GetLastError());
		return false;
	}
	const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
	g_shm = static_cast<QvdShared *>(MapViewOfFile(g_shmHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(QvdShared)));
	if (!g_shm) {
		Log("MapViewOfFile failed, error %lu", GetLastError());
		CloseHandle(g_shmHandle);
		g_shmHandle = nullptr;
		return false;
	}
	if (existed && g_shm->magic == QVD_MAGIC && OtherWriterAlive()) {
		// Another program with this layer (still running) owns the channel: do not touch it.
		Log("shared memory opened - owned by running pid %u ('%s'), not taking over", g_shm->writerPid,
		    g_shm->appName);
		return true;
	}
	ShmWriteBegin();
	g_shm->magic = QVD_MAGIC;
	g_shm->version = QVD_VERSION;
	g_shm->sharedHandle = 0;
	g_shm->width = g_shm->height = 0;
	g_shm->writerPid = GetCurrentProcessId();
	strncpy_s(g_shm->appName, g_appName.c_str(), _TRUNCATE);
	strncpy_s(g_shm->runtimeName, g_runtimeName.c_str(), _TRUNCATE);
	ShmWriteEnd();
	Log("shared memory opened (%s)", existed ? "already existed - OBS was waiting" : "new");
	return true;
}

// Writes this process's picture into the descriptor (only called when this process may own it).
void Publish()
{
	ShmWriteBegin();
	g_shm->magic = QVD_MAGIC;
	g_shm->version = QVD_VERSION;
	g_shm->sharedHandle = g_sharedHandleVal;
	g_shm->width = g_sharedW;
	g_shm->height = g_sharedH;
	g_shm->dxgiFormat = g_sharedFmt;
	g_shm->swapchainFormat = g_sharedSwapFmt;
	g_shm->flags = g_sharedFlags;
	g_shm->writerPid = GetCurrentProcessId();
	strncpy_s(g_shm->appName, g_appName.c_str(), _TRUNCATE);
	strncpy_s(g_shm->runtimeName, g_runtimeName.c_str(), _TRUNCATE);
	ShmWriteEnd();
}

void ReleaseSharedTexture()
{
	if (OwnsChannel()) {
		ShmWriteBegin();
		g_shm->sharedHandle = 0;
		g_shm->width = g_shm->height = 0;
		ShmWriteEnd();
	}
	if (g_sharedMutex) {
		g_sharedMutex->Release();
		g_sharedMutex = nullptr;
	}
	if (g_sharedTex) {
		g_sharedTex->Release();
		g_sharedTex = nullptr;
	}
	g_sharedW = g_sharedH = g_sharedFmt = g_sharedSwapFmt = 0;
	g_sharedHandleVal = g_sharedFlags = 0;
}

void CloseShm()
{
	ReleaseSharedTexture();
	if (g_shm) {
		if (OwnsChannel()) {
			ShmWriteBegin();
			g_shm->writerPid = 0;
			ShmWriteEnd();
		}
		UnmapViewOfFile(g_shm);
		g_shm = nullptr;
	}
	if (g_shmHandle) {
		CloseHandle(g_shmHandle);
		g_shmHandle = nullptr;
	}
}

// Typeless family of the texture format, so CopySubresourceRegion is legal and
// OBS can open it (OBS accepts the *_TYPELESS 8-bit formats, R10G10B10A2_UNORM, RGBA16F).
DXGI_FORMAT SharedFormatFor(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		return DXGI_FORMAT_R8G8B8A8_TYPELESS;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return DXGI_FORMAT_B8G8R8A8_TYPELESS;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		return DXGI_FORMAT_B8G8R8X8_TYPELESS;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default:
		return DXGI_FORMAT_UNKNOWN;
	}
}

bool IsSrgb(int64_t f)
{
	return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
	       f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

bool EnsureSharedTexture(uint32_t w, uint32_t h, DXGI_FORMAT fmt, int64_t swapFmt)
{
	if (g_sharedTex && g_sharedW == w && g_sharedH == h && g_sharedFmt == (uint32_t)fmt &&
	    g_sharedSwapFmt == (uint32_t)swapFmt)
		return true;
	ReleaseSharedTexture();

	D3D11_TEXTURE2D_DESC d = {};
	d.Width = w;
	d.Height = h;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	HRESULT hr = g_device->CreateTexture2D(&d, nullptr, &g_sharedTex);
	if (FAILED(hr)) {
		Log("CreateTexture2D(shared %ux%u fmt %d) failed hr=0x%08lX", w, h, (int)fmt, hr);
		return false;
	}
	hr = g_sharedTex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&g_sharedMutex);
	IDXGIResource *res = nullptr;
	HANDLE handle = nullptr;
	if (SUCCEEDED(hr))
		hr = g_sharedTex->QueryInterface(__uuidof(IDXGIResource), (void **)&res);
	if (SUCCEEDED(hr))
		hr = res->GetSharedHandle(&handle);
	if (res)
		res->Release();
	if (FAILED(hr) || !handle) {
		Log("shared handle failed hr=0x%08lX", hr);
		ReleaseSharedTexture();
		return false;
	}
	g_sharedW = w;
	g_sharedH = h;
	g_sharedFmt = (uint32_t)fmt;
	g_sharedSwapFmt = (uint32_t)swapFmt;

	uint32_t flags = 0;
	if (IsSrgb(swapFmt))
		flags |= QVD_FLAG_SRGB_FORMAT;
	if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT)
		flags |= QVD_FLAG_LINEAR_FLOAT;

	g_sharedHandleVal = (uint32_t)(uintptr_t)handle;
	g_sharedFlags = flags;
	Publish();
	Log("shared texture %ux%u shared-fmt %d swapchain-fmt %lld flags %u handle 0x%08X", w, h, (int)fmt,
	    (long long)swapFmt, flags, (uint32_t)(uintptr_t)handle);
	return true;
}

void CaptureLeftEye(const XrFrameEndInfo *info)
{
	if (!g_device || !g_shm)
		return;

	const uint64_t now = GetTickCount64();
	const bool readerActive = now - g_shm->readerTickMs < QVD_READER_TIMEOUT_MS;
	if (readerActive != g_readerWasActive) {
		Log(readerActive ? "OBS is showing the picture - copying left eye" : "OBS not showing the picture - copy paused");
		g_readerWasActive = readerActive;
		if (readerActive)
			g_readerActiveSinceMs = now;
	}
	if (!readerActive)
		return;

	if (!OwnsChannel()) {
		const bool ownerAlive = OtherWriterAlive();
		// A running owner keeps the channel only while it delivers frames. If OBS has been asking
		// for the picture for QVD_READER_TIMEOUT_MS and the owner copied nothing in that time
		// (e.g. a launcher like PimaxHome that has a session but submits no picture), take over.
		const bool ownerDelivering = ownerAlive && (now - g_readerActiveSinceMs < QVD_READER_TIMEOUT_MS ||
		                                            now - g_shm->writerTickMs < QVD_READER_TIMEOUT_MS);
		if (ownerDelivering) {
			if (!g_loggedNotOwner) {
				Log("picture channel is used by running pid %u ('%s') - this app is not sent to OBS",
				    g_shm->writerPid, g_shm->appName);
				g_loggedNotOwner = true;
			}
			return;
		}
		if (ownerAlive)
			Log("taking over the picture channel (running owner pid %u '%s' sent no frame for %llu ms)",
			    g_shm->writerPid, g_shm->appName, (unsigned long long)QVD_READER_TIMEOUT_MS);
		else
			Log("taking over the picture channel (previous owner pid %u has ended)", g_shm->writerPid);
		g_loggedNotOwner = false;
		if (g_sharedTex)
			Publish();
		else {
			ShmWriteBegin();
			g_shm->sharedHandle = 0;
			g_shm->width = g_shm->height = 0;
			g_shm->writerPid = GetCurrentProcessId();
			ShmWriteEnd();
		}
	}

	const XrCompositionLayerProjection *proj = nullptr;
	for (uint32_t i = 0; i < info->layerCount; i++) {
		const XrCompositionLayerBaseHeader *l = info->layers[i];
		if (l && l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
			proj = reinterpret_cast<const XrCompositionLayerProjection *>(l);
			break;
		}
	}
	if (!proj || proj->viewCount < 1) {
		if (!g_loggedNoProjection) {
			Log("frame without projection layer (layerCount %u) - nothing copied", info->layerCount);
			g_loggedNoProjection = true;
		}
		return;
	}

	const XrSwapchainSubImage &sub = proj->views[0].subImage; // view 0 = left eye
	auto it = g_swapchains.find(sub.swapchain);
	if (it == g_swapchains.end() || it->second.lastReleased < 0 ||
	    (size_t)it->second.lastReleased >= it->second.images.size())
		return;
	SwapchainInfo &sc = it->second;
	ID3D11Texture2D *src = sc.images[sc.lastReleased];
	if (!src)
		return;

	D3D11_TEXTURE2D_DESC sd;
	src->GetDesc(&sd);
	if (sd.SampleDesc.Count > 1) {
		if (!g_loggedMsaa) {
			Log("left eye swapchain is MSAA (%u samples) - not supported, nothing copied", sd.SampleDesc.Count);
			g_loggedMsaa = true;
		}
		return;
	}
	const DXGI_FORMAT sharedFmt = SharedFormatFor(sd.Format);
	if (sharedFmt == DXGI_FORMAT_UNKNOWN) {
		if (!g_loggedFormat) {
			Log("left eye texture format %d not supported - nothing copied", (int)sd.Format);
			g_loggedFormat = true;
		}
		return;
	}

	// Clamp the rect to the texture.
	int32_t x = sub.imageRect.offset.x, y = sub.imageRect.offset.y;
	int32_t w = sub.imageRect.extent.width, h = sub.imageRect.extent.height;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x + w > (int32_t)sd.Width) w = (int32_t)sd.Width - x;
	if (y + h > (int32_t)sd.Height) h = (int32_t)sd.Height - y;
	if (w <= 0 || h <= 0 || sub.imageArrayIndex >= sd.ArraySize)
		return;

	if (!EnsureSharedTexture((uint32_t)w, (uint32_t)h, sharedFmt, sc.format))
		return;

	// Never block the game: if OBS holds the texture right now, skip this frame.
	if (g_sharedMutex->AcquireSync(0, 0) != S_OK)
		return;
	const UINT subres = D3D11CalcSubresource(0, sub.imageArrayIndex, sd.MipLevels);
	D3D11_BOX box = {(UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1};
	ID3D11Resource *wrapped = src;
	if (g_on12) // D3D12: the image is released in RENDER_TARGET on the game's queue (spec, khr_d3d12_enable)
		g_on12->AcquireWrappedResources(&wrapped, 1);
	g_context->CopySubresourceRegion(g_sharedTex, 0, 0, 0, 0, src, subres, &box);
	if (g_on12)
		g_on12->ReleaseWrappedResources(&wrapped, 1);
	g_sharedMutex->ReleaseSync(0);
	g_context->Flush();

	g_shm->writerTickMs = now;
	g_shm->frameCount = g_shm->frameCount + 1;
	g_copiedFrames++;
	if (!g_loggedFirstCopy) {
		const XrFovf &fov = proj->views[0].fov;
		Log("first left eye copy: rect %d,%d %dx%d array %u image %d of %zu, tex %ux%u fmt %d, views %u, "
		    "fov L %.2f R %.2f U %.2f D %.2f deg",
		    x, y, w, h, sub.imageArrayIndex, sc.lastReleased, sc.images.size(), sd.Width, sd.Height,
		    (int)sd.Format, proj->viewCount, fov.angleLeft * 57.29578f, fov.angleRight * 57.29578f,
		    fov.angleUp * 57.29578f, fov.angleDown * 57.29578f);
		g_loggedFirstCopy = true;
	}
}

// ---------------------------------------------------------------- hooks
XrResult XRAPI_CALL hook_xrDestroyInstance(XrInstance instance)
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		CloseShm();
		g_instance = XR_NULL_HANDLE;
	}
	Log("xrDestroyInstance");
	return next_DestroyInstance(instance);
}

XrResult XRAPI_CALL hook_xrCreateSession(XrInstance instance, const XrSessionCreateInfo *createInfo, XrSession *session)
{
	XrResult r = next_CreateSession(instance, createInfo, session);
	if (XR_FAILED(r))
		return r;

	const XrGraphicsBindingD3D11KHR *d3d11 = nullptr;
	const XrGraphicsBindingD3D12KHR *d3d12 = nullptr;
	XrStructureType otherType = XR_TYPE_UNKNOWN;
	for (auto *e = reinterpret_cast<const XrBaseInStructure *>(createInfo->next); e; e = e->next) {
		if (e->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
			d3d11 = reinterpret_cast<const XrGraphicsBindingD3D11KHR *>(e);
			break;
		}
		if (e->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
			d3d12 = reinterpret_cast<const XrGraphicsBindingD3D12KHR *>(e);
			break;
		}
		if (otherType == XR_TYPE_UNKNOWN)
			otherType = e->type;
	}

	std::lock_guard<std::mutex> lock(g_mutex);
	if (IsSkippedApp()) {
		Log("xrCreateSession: '%s' (%ls) is skipped - never sent to OBS", g_appName.c_str(), ExeName().c_str());
		return r;
	}
	if (!(d3d11 && d3d11->device) && !(d3d12 && d3d12->device && d3d12->queue)) {
		Log("xrCreateSession: graphics binding type %d is not D3D11/D3D12 - picture capture off for this session",
		    (int)otherType);
		return r;
	}
	CloseShm();
	ClearSwapchains();
	ReleaseDevice();
	if (d3d11) {
		g_device = d3d11->device;
		g_device->AddRef();
		g_device->GetImmediateContext(&g_context);
	} else {
		// D3D12: a D3D11On12 device on the game's own queue. Copies are submitted on that queue,
		// where the spec makes released images available (khr_d3d12_enable "Swapchain Image Resource State").
		IUnknown *queues[] = {d3d12->queue};
		const HRESULT hr = D3D11On12CreateDevice(d3d12->device, 0, nullptr, 0, queues, 1, 0, &g_device, &g_context,
							 nullptr);
		if (SUCCEEDED(hr))
			g_device->QueryInterface(__uuidof(ID3D11On12Device), (void **)&g_on12);
		if (FAILED(hr) || !g_on12) {
			Log("xrCreateSession: D3D11On12CreateDevice failed hr=0x%08lX - picture capture off for this session", hr);
			ReleaseDevice();
			return r;
		}
	}
	g_session = *session;
	g_loggedFirstCopy = g_loggedMsaa = g_loggedFormat = g_loggedNoProjection = false;
	g_readerWasActive = false;
	g_copiedFrames = 0;
	OpenShm();
	Log("xrCreateSession: %s session ready (app '%s', runtime '%s')", g_on12 ? "D3D12" : "D3D11", g_appName.c_str(),
	    g_runtimeName.c_str());
	return r;
}

XrResult XRAPI_CALL hook_xrDestroySession(XrSession session)
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (session == g_session) {
			Log("xrDestroySession: %llu frames copied", (unsigned long long)g_copiedFrames);
			CloseShm();
			ClearSwapchains();
			ReleaseDevice();
			g_session = XR_NULL_HANDLE;
		}
	}
	return next_DestroySession(session);
}

XrResult XRAPI_CALL hook_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo *ci, XrSwapchain *swapchain)
{
	XrResult r = next_CreateSwapchain(session, ci, swapchain);
	if (XR_SUCCEEDED(r)) {
		std::lock_guard<std::mutex> lock(g_mutex);
		if (session == g_session) {
			SwapchainInfo &s = g_swapchains[*swapchain];
			s.format = ci->format;
			s.width = ci->width;
			s.height = ci->height;
			s.arraySize = ci->arraySize;
			s.sampleCount = ci->sampleCount;
			s.usage = ci->usageFlags;
		}
	}
	return r;
}

XrResult XRAPI_CALL hook_xrDestroySwapchain(XrSwapchain swapchain)
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_swapchains.find(swapchain);
		if (it != g_swapchains.end()) {
			if (it->second.ownsImages && g_context)
				g_context->Flush(); // our copies from these images are submitted before they go away
			ReleaseImages(it->second);
			g_swapchains.erase(it);
		}
	}
	return next_DestroySwapchain(swapchain);
}

XrResult XRAPI_CALL hook_xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t capacity, uint32_t *count,
						     XrSwapchainImageBaseHeader *images)
{
	XrResult r = next_EnumerateSwapchainImages(swapchain, capacity, count, images);
	if (XR_SUCCEEDED(r) && images && capacity > 0 && images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR && !g_on12) {
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_swapchains.find(swapchain);
		if (it != g_swapchains.end()) {
			auto *d = reinterpret_cast<XrSwapchainImageD3D11KHR *>(images);
			ReleaseImages(it->second);
			it->second.images.assign(*count, nullptr);
			for (uint32_t i = 0; i < *count; i++)
				it->second.images[i] = d[i].texture;
		}
	} else if (XR_SUCCEEDED(r) && images && capacity > 0 && images->type == XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) {
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_swapchains.find(swapchain);
		// Only color swapchains: the projection views we copy are color, and the spec state
		// (RENDER_TARGET at release) is defined for color rendering targets.
		if (it != g_swapchains.end() && g_on12 && (it->second.usage & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)) {
			auto *d = reinterpret_cast<XrSwapchainImageD3D12KHR *>(images);
			ReleaseImages(it->second);
			it->second.images.assign(*count, nullptr);
			it->second.ownsImages = true;
			D3D11_RESOURCE_FLAGS flags = {}; // copy source only: no bind flags
			uint32_t wrappedCount = 0;
			for (uint32_t i = 0; i < *count; i++) {
				ID3D11Texture2D *t = nullptr;
				const HRESULT hr = g_on12->CreateWrappedResource(
					d[i].texture, &flags, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
					__uuidof(ID3D11Texture2D), (void **)&t);
				if (SUCCEEDED(hr)) {
					it->second.images[i] = t;
					wrappedCount++;
				} else {
					Log("CreateWrappedResource(image %u) failed hr=0x%08lX", i, hr);
				}
			}
			Log("D3D12 swapchain %ux%u fmt %lld: %u of %u images wrapped for copying", it->second.width,
			    it->second.height, (long long)it->second.format, wrappedCount, *count);
		}
	}
	return r;
}

XrResult XRAPI_CALL hook_xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo *ai,
						  uint32_t *index)
{
	XrResult r = next_AcquireSwapchainImage(swapchain, ai, index);
	if (XR_SUCCEEDED(r)) {
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_swapchains.find(swapchain);
		if (it != g_swapchains.end())
			it->second.acquired.push_back(*index);
	}
	return r;
}

XrResult XRAPI_CALL hook_xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *ri)
{
	XrResult r = next_ReleaseSwapchainImage(swapchain, ri);
	if (XR_SUCCEEDED(r)) {
		std::lock_guard<std::mutex> lock(g_mutex);
		auto it = g_swapchains.find(swapchain);
		if (it != g_swapchains.end() && !it->second.acquired.empty()) {
			// images are released in the order they were acquired
			it->second.lastReleased = (int)it->second.acquired.front();
			it->second.acquired.pop_front();
		}
	}
	return r;
}

XrResult XRAPI_CALL hook_xrEndFrame(XrSession session, const XrFrameEndInfo *info)
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (session == g_session && info)
			CaptureLeftEye(info);
	}
	return next_EndFrame(session, info);
}

XrResult XRAPI_CALL hook_xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *function)
{
	XrResult r = next_GetInstanceProcAddr(instance, name, function);
	if (XR_FAILED(r) || !name)
		return r;
#define HOOK(fn)                                                         \
	if (strcmp(name, #fn) == 0) {                                    \
		*function = reinterpret_cast<PFN_xrVoidFunction>(hook_##fn); \
		return r;                                                \
	}
	HOOK(xrGetInstanceProcAddr)
	HOOK(xrDestroyInstance)
	HOOK(xrCreateSession)
	HOOK(xrDestroySession)
	HOOK(xrCreateSwapchain)
	HOOK(xrDestroySwapchain)
	HOOK(xrEnumerateSwapchainImages)
	HOOK(xrAcquireSwapchainImage)
	HOOK(xrReleaseSwapchainImage)
	HOOK(xrEndFrame)
#undef HOOK
	return r;
}

XrResult XRAPI_CALL layer_xrCreateApiLayerInstance(const XrInstanceCreateInfo *info, const XrApiLayerCreateInfo *layerInfo,
						    XrInstance *instance)
{
	if (!layerInfo || !layerInfo->nextInfo || strcmp(layerInfo->nextInfo->layerName, LAYER_NAME) != 0) {
		Log("xrCreateApiLayerInstance: unexpected layer chain");
		return XR_ERROR_INITIALIZATION_FAILED;
	}
	next_GetInstanceProcAddr = layerInfo->nextInfo->nextGetInstanceProcAddr;

	XrApiLayerCreateInfo chain = *layerInfo;
	chain.nextInfo = layerInfo->nextInfo->next;
	XrResult r = layerInfo->nextInfo->nextCreateApiLayerInstance(info, &chain, instance);
	if (XR_FAILED(r)) {
		Log("next xrCreateApiLayerInstance failed %d", (int)r);
		return r;
	}

#define GET(fn) next_GetInstanceProcAddr(*instance, "xr" #fn, reinterpret_cast<PFN_xrVoidFunction *>(&next_##fn))
	GET(DestroyInstance);
	GET(GetInstanceProperties);
	GET(CreateSession);
	GET(DestroySession);
	GET(CreateSwapchain);
	GET(DestroySwapchain);
	GET(EnumerateSwapchainImages);
	GET(AcquireSwapchainImage);
	GET(ReleaseSwapchainImage);
	GET(EndFrame);
#undef GET

	std::lock_guard<std::mutex> lock(g_mutex);
	g_instance = *instance;
	g_appName = info && info->applicationInfo.applicationName[0] ? info->applicationInfo.applicationName : "";
	g_runtimeName.clear();
	if (next_GetInstanceProperties) {
		XrInstanceProperties p = {XR_TYPE_INSTANCE_PROPERTIES};
		if (XR_SUCCEEDED(next_GetInstanceProperties(*instance, &p)))
			g_runtimeName = p.runtimeName;
	}
	Log("instance created: app '%s', runtime '%s'", g_appName.c_str(), g_runtimeName.c_str());
	return r;
}

} // namespace

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
	const XrNegotiateLoaderInfo *loaderInfo, const char *layerName, XrNegotiateApiLayerRequest *req)
{
	if (!loaderInfo || !req || loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
	    req->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
	    loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
	    loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION) {
		Log("negotiate: loader interface not compatible");
		return XR_ERROR_INITIALIZATION_FAILED;
	}
	if (layerName && strcmp(layerName, LAYER_NAME) != 0) {
		Log("negotiate: called for '%s'", layerName);
		return XR_ERROR_INITIALIZATION_FAILED;
	}
	req->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
	req->layerApiVersion = XR_API_VERSION_1_0; // only OpenXR 1.0 functions are used
	req->getInstanceProcAddr = hook_xrGetInstanceProcAddr;
	req->createApiLayerInstance = layer_xrCreateApiLayerInstance;
	Log("layer loaded");
	return XR_SUCCESS;
}
