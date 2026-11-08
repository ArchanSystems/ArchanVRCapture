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
#include <winternl.h>
#include <pdh.h>
#include <pdhmsg.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../common/qvd_shared.h"
#include "../common/log_cleanup.h"
#include "../common/overlay_key.h"

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
	QvdLogLock fileLock; // other games / the cleanup
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
PFN_xrBeginFrame next_BeginFrame = nullptr;
PFN_xrCreateReferenceSpace next_CreateReferenceSpace = nullptr;
PFN_xrDestroySpace next_DestroySpace = nullptr;

// Reference spaces the app created: type + pose, to know world "up" in the projection layer's space.
struct SpaceInfo {
	XrReferenceSpaceType type;
	XrPosef pose;
};
std::unordered_map<XrSpace, SpaceInfo> g_spaces; // guarded by g_mutex

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
ID3D12Device *g_d12Device = nullptr;      // D3D12 session: the game's device and queue (AddRef'd)
ID3D12CommandQueue *g_d12Queue = nullptr;
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
	if (g_d12Queue) {
		g_d12Queue->Release();
		g_d12Queue = nullptr;
	}
	if (g_d12Device) {
		g_d12Device->Release();
		g_d12Device = nullptr;
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
// quad views (4 views, foveated rendering): the sharp inner left view (view 2), second shared texture
ID3D11Texture2D *g_innerTex = nullptr;
IDXGIKeyedMutex *g_innerMutex = nullptr;
uint32_t g_innerW = 0, g_innerH = 0, g_innerFmt = 0, g_innerHandleVal = 0;
bool g_loggedInner = false;

// Hand-over to OBS at the next xrBeginFrame (D3D11 games; measured 2026-11-07). In DCS (D3D11) AcquireSync at
// xrEndFrame took 2.7 ms per frame and the frame interval was that much longer (15.7 ms copying, 13.0 ms
// not copying, same session): any sync on the game's context right after it queued its frame waits until the
// driver has taken that whole frame. A plain copy does not wait. So xrEndFrame only copies the eye into a
// spare texture; the next xrBeginFrame on the same thread (after xrWaitFrame, the driver has caught up) takes
// the shared texture, copies spare -> shared, writes the pose, gives it back and flushes. If xrBeginFrame
// comes on another thread or not between two frames, this session hands over at xrEndFrame as before.
// D3D12 games (D3D11On12 on our own device) keep the hand-over at xrEndFrame: measured 21-24 us there.
struct PoseSnap {
	int64_t displayTimeNs;
	float orient[4], pos[3], fovTan[4], up[3];
	uint32_t flags;
	float innerFovTan[4], innerOrient[4];
	uint32_t eye; // 0 left, 1 right
};
uint32_t g_eye = 0; // the eye being copied (v5: OBS chooses it)
struct PendingFrame {
	bool valid = false, inner = false;
	DWORD thread = 0;
	double endUs = 0, lockUs = 0; // this frame's time at xrEndFrame (for the cost log)
	PoseSnap pose = {};
};
PendingFrame g_pending;
ID3D11Texture2D *g_spareTex = nullptr, *g_spareInner = nullptr; // same size/format as the shared ones, not shared
bool g_handAtBegin = false;  // this session hands over at xrBeginFrame
bool g_loggedHandover = false;
std::atomic<bool> g_testHandAtEnd{false}; // test switch: hand over at xrEndFrame (the old way), for comparisons
const DWORD kHandoverWaitMs = 1;          // longest wait for OBS to give the picture back (it holds it ~0.1 ms)
bool g_loggedNotOwner = false;
bool g_loggedFirstCopy = false;
bool g_loggedPose = false;
bool g_loggedMsaa = false;
bool g_loggedFormat = false;
bool g_loggedNoProjection = false;
bool g_readerWasActive = false;
uint64_t g_readerActiveSinceMs = 0; // when this process last saw OBS start asking for the picture
uint64_t g_copiedFrames = 0;

// Cost measurement (2026-11-05, user: the capture must stay away from the game's render thread):
// time this layer spends inside the game's xrEndFrame, on the game's thread, logged every 10 s.
struct CostStats {
	double freq = 0;           // QPC ticks per second
	int64_t windowStart = 0;   // QPC
	int64_t lastEndFrame = 0;  // QPC at the previous xrEndFrame (game frame interval)
	std::vector<double> hookUs; // this layer's time per frame (copy frames only), microseconds
	double flushUs = 0, copyUs = 0, intervalMs = 0, belowMs = 0;
	double lockUs = 0, acquireUs = 0; // waiting for this layer's lock / for the shared picture (AcquireSync)
	double spareUs = 0, beginUs = 0;  // D3D11: copy into the spare at xrEndFrame / the whole hand-over at xrBeginFrame
	int64_t lockedAt = 0;             // QPC when the lock was taken this frame
	uint32_t frames = 0, copies = 0, spares = 0;
	uint32_t busy = 0; // frames not given to OBS because it held the picture
};
CostStats g_cost;
int64_t Qpc()
{
	LARGE_INTEGER v;
	QueryPerformanceCounter(&v);
	return v.QuadPart;
}

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
	g_shm->innerHandle = g_innerHandleVal;
	g_shm->innerWidth = g_innerW;
	g_shm->innerHeight = g_innerH;
	g_shm->innerDxgiFormat = g_innerFmt;
	g_shm->writerPid = GetCurrentProcessId();
	strncpy_s(g_shm->appName, g_appName.c_str(), _TRUNCATE);
	strncpy_s(g_shm->runtimeName, g_runtimeName.c_str(), _TRUNCATE);
	ShmWriteEnd();
}

void ReleaseInnerTexture(bool publish)
{
	g_pending.valid = false; // its spare goes with it
	if (g_spareInner) {
		g_spareInner->Release();
		g_spareInner = nullptr;
	}
	if (g_innerMutex) {
		g_innerMutex->Release();
		g_innerMutex = nullptr;
	}
	if (g_innerTex) {
		g_innerTex->Release();
		g_innerTex = nullptr;
	}
	g_innerW = g_innerH = g_innerFmt = g_innerHandleVal = 0;
	if (publish && OwnsChannel() && g_sharedTex)
		Publish();
}

void ReleaseSharedTexture()
{
	ReleaseInnerTexture(false);
	if (OwnsChannel()) {
		ShmWriteBegin();
		g_shm->sharedHandle = 0;
		g_shm->width = g_shm->height = 0;
		g_shm->innerHandle = 0;
		g_shm->innerWidth = g_shm->innerHeight = 0;
		ShmWriteEnd();
	}
	g_pending.valid = false;
	if (g_spareTex) {
		g_spareTex->Release();
		g_spareTex = nullptr;
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
			g_shm->overlayVisible = 0;
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

// The inner view's shared texture (quad views). Same kind as the main one; published with it.
bool EnsureInnerTexture(uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
	if (g_innerTex && g_innerW == w && g_innerH == h && g_innerFmt == (uint32_t)fmt)
		return true;
	ReleaseInnerTexture(false);
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
	HRESULT hr = g_device->CreateTexture2D(&d, nullptr, &g_innerTex);
	if (FAILED(hr)) {
		Log("CreateTexture2D(inner %ux%u fmt %d) failed hr=0x%08lX", w, h, (int)fmt, hr);
		return false;
	}
	hr = g_innerTex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&g_innerMutex);
	IDXGIResource *res = nullptr;
	HANDLE handle = nullptr;
	if (SUCCEEDED(hr))
		hr = g_innerTex->QueryInterface(__uuidof(IDXGIResource), (void **)&res);
	if (SUCCEEDED(hr))
		hr = res->GetSharedHandle(&handle);
	if (res)
		res->Release();
	if (FAILED(hr) || !handle) {
		Log("inner shared handle failed hr=0x%08lX", hr);
		ReleaseInnerTexture(false);
		return false;
	}
	g_innerW = w;
	g_innerH = h;
	g_innerFmt = (uint32_t)fmt;
	g_innerHandleVal = (uint32_t)(uintptr_t)handle;
	Publish();
	Log("inner view shared texture %ux%u fmt %d handle 0x%08X (quad views)", w, h, (int)fmt, g_innerHandleVal);
	return true;
}

// v = rotate vector by the inverse of unit quaternion q
void RotateByInverse(const XrQuaternionf &q, const float in[3], float out[3])
{
	// conj(q) * v * q, expanded (t = 2 * cross(-q.xyz, v); v' = v + w*t + cross(-q.xyz, t))
	const float qx = -q.x, qy = -q.y, qz = -q.z, qw = q.w;
	const float tx = 2.0f * (qy * in[2] - qz * in[1]);
	const float ty = 2.0f * (qz * in[0] - qx * in[2]);
	const float tz = 2.0f * (qx * in[1] - qy * in[0]);
	out[0] = in[0] + qw * tx + (qy * tz - qz * ty);
	out[1] = in[1] + qw * ty + (qz * tx - qx * tz);
	out[2] = in[2] + qw * tz + (qx * ty - qy * tx);
}

// The pose of the copied picture, taken at xrEndFrame. fullRect: the whole imageRect was copied (the FOV
// describes exactly the copied picture). inner: the inner view (quad views) was copied too.
void MakePose(const XrFrameEndInfo *info, const XrCompositionLayerProjection *proj, uint32_t eye, bool fullRect, bool inner,
	      PoseSnap &p)
{
	const XrCompositionLayerProjectionView &v = proj->views[eye];
	p.eye = eye;
	p.displayTimeNs = info->displayTime;
	p.orient[0] = v.pose.orientation.x;
	p.orient[1] = v.pose.orientation.y;
	p.orient[2] = v.pose.orientation.z;
	p.orient[3] = v.pose.orientation.w;
	p.pos[0] = v.pose.position.x;
	p.pos[1] = v.pose.position.y;
	p.pos[2] = v.pose.position.z;
	p.fovTan[0] = tanf(v.fov.angleLeft);
	p.fovTan[1] = tanf(v.fov.angleRight);
	p.fovTan[2] = tanf(v.fov.angleUp);
	p.fovTan[3] = tanf(v.fov.angleDown);
	p.up[0] = p.up[1] = p.up[2] = 0.0f;
	uint32_t flags = fullRect ? QVD_POSE_VALID : 0u;
	auto it = g_spaces.find(proj->space);
	if (it != g_spaces.end() &&
	    (it->second.type == XR_REFERENCE_SPACE_TYPE_LOCAL || it->second.type == XR_REFERENCE_SPACE_TYPE_STAGE ||
	     it->second.type == (XrReferenceSpaceType)1000426000 /* LOCAL_FLOOR */)) {
		// these spaces have +Y against gravity; the app's space may be rotated by its pose
		const float worldUp[3] = {0.0f, 1.0f, 0.0f};
		RotateByInverse(it->second.pose.orientation, worldUp, p.up);
		flags |= QVD_POSE_UP;
	}
	if (inner) {
		const XrCompositionLayerProjectionView &v2 = proj->views[2 + eye];
		p.innerFovTan[0] = tanf(v2.fov.angleLeft);
		p.innerFovTan[1] = tanf(v2.fov.angleRight);
		p.innerFovTan[2] = tanf(v2.fov.angleUp);
		p.innerFovTan[3] = tanf(v2.fov.angleDown);
		p.innerOrient[0] = v2.pose.orientation.x;
		p.innerOrient[1] = v2.pose.orientation.y;
		p.innerOrient[2] = v2.pose.orientation.z;
		p.innerOrient[3] = v2.pose.orientation.w;
		flags |= QVD_POSE_INNER;
	}
	p.flags = flags;
	if (!g_loggedPose) {
		Log("pose for stabilization: space type %d, %s, up %s (%.3f %.3f %.3f)",
		    it != g_spaces.end() ? (int)it->second.type : -1, fullRect ? "full rect" : "CLAMPED rect - pose not used",
		    (flags & QVD_POSE_UP) ? "known" : "unknown", p.up[0], p.up[1], p.up[2]);
		g_loggedPose = true;
	}
}

// Called while the shared texture's keyed mutex is held: the pose belongs to the copied picture.
void WritePose(const PoseSnap &p)
{
	g_shm->displayTimeNs = p.displayTimeNs;
	for (int i = 0; i < 4; i++) {
		g_shm->poseOrient[i] = p.orient[i];
		g_shm->fovTan[i] = p.fovTan[i];
	}
	for (int i = 0; i < 3; i++)
		g_shm->posePos[i] = p.pos[i];
	if (p.flags & QVD_POSE_UP)
		for (int i = 0; i < 3; i++)
			g_shm->up[i] = p.up[i];
	if (p.flags & QVD_POSE_INNER)
		for (int i = 0; i < 4; i++) {
			g_shm->innerFovTan[i] = p.innerFovTan[i];
			g_shm->innerPoseOrient[i] = p.innerOrient[i];
		}
	g_shm->poseFlags = p.flags;
	g_shm->picEye = p.eye;
}

// A texture like `like` (size, format), not shared: the spare for the hand-over at xrBeginFrame.
bool EnsureSpare(ID3D11Texture2D *&spare, ID3D11Texture2D *like)
{
	D3D11_TEXTURE2D_DESC d;
	like->GetDesc(&d);
	if (spare) {
		D3D11_TEXTURE2D_DESC s;
		spare->GetDesc(&s);
		if (s.Width == d.Width && s.Height == d.Height && s.Format == d.Format)
			return true;
		spare->Release();
		spare = nullptr;
	}
	d.BindFlags = 0;
	d.MiscFlags = 0;
	const HRESULT hr = g_device->CreateTexture2D(&d, nullptr, &spare);
	if (FAILED(hr)) {
		Log("CreateTexture2D(spare %ux%u fmt %d) failed hr=0x%08lX", d.Width, d.Height, (int)d.Format, hr);
		spare = nullptr;
		return false;
	}
	return true;
}

// The frame was given to OBS (shared texture written, pose written, mutex released).
void FrameDelivered(uint64_t now)
{
	g_shm->writerTickMs = now;
	g_shm->frameCount = g_shm->frameCount + 1;
	g_copiedFrames++;
	g_cost.copies++;
}

void CaptureLeftEye(const XrFrameEndInfo *info)
{
	if (!g_device || !g_shm)
		return;

	const uint64_t now = GetTickCount64();
	const bool readerActive = now - g_shm->readerTickMs < QVD_READER_TIMEOUT_MS;
	if (readerActive != g_readerWasActive) {
		Log(readerActive ? "OBS is showing the picture - copying" : "OBS not showing the picture - copy paused");
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

	// the eye OBS asks for (v5, user decision 2026-11-07): view 0 = left, 1 = right; quad views: inner 2 / 3
	uint32_t eye = g_shm->wantEye == 1 ? 1u : 0u;
	if (eye >= proj->viewCount)
		eye = 0;
	if (eye != g_eye) {
		Log("OBS asks for the %s eye - copying view %u%s", eye ? "right" : "left", eye,
		    proj->viewCount >= 4 ? (eye ? " (quad views: inner view 3)" : " (quad views: inner view 2)") : "");
		g_eye = eye;
		g_loggedInner = false;
		g_loggedFirstCopy = false;
	}
	const XrSwapchainSubImage &sub = proj->views[eye].subImage;
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
			Log("eye swapchain is MSAA (%u samples) - not supported, nothing copied", sd.SampleDesc.Count);
			g_loggedMsaa = true;
		}
		return;
	}
	const DXGI_FORMAT sharedFmt = SharedFormatFor(sd.Format);
	if (sharedFmt == DXGI_FORMAT_UNKNOWN) {
		if (!g_loggedFormat) {
			Log("eye texture format %d not supported - nothing copied", (int)sd.Format);
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

	// Quad views (4 views: 0/1 wide, 2/3 the sharp inner views of foveated rendering): the inner left
	// view (2) is copied too, so OBS can put it into the wide view like the headset shows it.
	ID3D11Texture2D *src2 = nullptr;
	UINT subres2 = 0;
	D3D11_BOX box2 = {};
	bool inner = false;
	if (proj->viewCount >= 4) {
		const XrSwapchainSubImage &sub2 = proj->views[2 + eye].subImage;
		auto it2 = g_swapchains.find(sub2.swapchain);
		if (it2 != g_swapchains.end() && it2->second.lastReleased >= 0 &&
		    (size_t)it2->second.lastReleased < it2->second.images.size()) {
			src2 = it2->second.images[it2->second.lastReleased];
			D3D11_TEXTURE2D_DESC sd2;
			if (src2) {
				src2->GetDesc(&sd2);
				const DXGI_FORMAT fmt2 = SharedFormatFor(sd2.Format);
				int32_t x2 = sub2.imageRect.offset.x, y2 = sub2.imageRect.offset.y;
				int32_t w2 = sub2.imageRect.extent.width, h2 = sub2.imageRect.extent.height;
				if (x2 < 0) x2 = 0;
				if (y2 < 0) y2 = 0;
				if (x2 + w2 > (int32_t)sd2.Width) w2 = (int32_t)sd2.Width - x2;
				if (y2 + h2 > (int32_t)sd2.Height) h2 = (int32_t)sd2.Height - y2;
				// same format family as the main view (one picture in OBS), whole rect, no MSAA
				if (sd2.SampleDesc.Count == 1 && fmt2 == sharedFmt && w2 > 0 && h2 > 0 &&
				    sub2.imageArrayIndex < sd2.ArraySize && w2 == sub2.imageRect.extent.width &&
				    h2 == sub2.imageRect.extent.height && EnsureInnerTexture((uint32_t)w2, (uint32_t)h2, fmt2)) {
					subres2 = D3D11CalcSubresource(0, sub2.imageArrayIndex, sd2.MipLevels);
					box2 = {(UINT)x2, (UINT)y2, 0, (UINT)(x2 + w2), (UINT)(y2 + h2), 1};
					inner = true;
					if (!g_loggedInner) {
						const XrFovf &f2 = proj->views[2 + eye].fov;
						Log("quad views: inner view %u rect %d,%d %dx%d array %u, tex %ux%u fmt %d, fov L %.2f R %.2f "
						    "U %.2f D %.2f deg",
						    2 + eye, x2, y2, w2, h2, sub2.imageArrayIndex, sd2.Width, sd2.Height, (int)sd2.Format,
						    f2.angleLeft * 57.29578f, f2.angleRight * 57.29578f, f2.angleUp * 57.29578f,
						    f2.angleDown * 57.29578f);
						g_loggedInner = true;
					}
				} else if (!g_loggedInner) {
					Log("quad views: inner left view not copied (format %d vs %d, rect %dx%d of %ux%u, samples %u)",
					    (int)sd2.Format, (int)sd.Format, sub2.imageRect.extent.width, sub2.imageRect.extent.height,
					    sd2.Width, sd2.Height, sd2.SampleDesc.Count);
					g_loggedInner = true;
				}
			}
		}
	} else if (g_innerTex) {
		ReleaseInnerTexture(true); // no longer quad views
		Log("quad views ended - only the main view is copied");
		g_loggedInner = false;
	}

	const UINT subres = D3D11CalcSubresource(0, sub.imageArrayIndex, sd.MipLevels);
	D3D11_BOX box = {(UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1};
	const bool fullRect = w == sub.imageRect.extent.width && h == sub.imageRect.extent.height;
	auto logFirstCopy = [&](const char *how) {
		if (g_loggedFirstCopy)
			return;
		const XrFovf &fov = proj->views[eye].fov;
		Log("first %s eye copy (%s): rect %d,%d %dx%d array %u image %d of %zu, tex %ux%u fmt %d, views %u, "
		    "fov L %.2f R %.2f U %.2f D %.2f deg",
		    eye ? "right" : "left", how, x, y, w, h, sub.imageArrayIndex, sc.lastReleased, sc.images.size(), sd.Width, sd.Height,
		    (int)sd.Format, proj->viewCount, fov.angleLeft * 57.29578f, fov.angleRight * 57.29578f,
		    fov.angleUp * 57.29578f, fov.angleDown * 57.29578f);
		g_loggedFirstCopy = true;
	};

	// D3D11: only copy into the spare here, no sync and no flush; the next xrBeginFrame hands it over
	if (g_handAtBegin && !g_testHandAtEnd) {
		if (g_pending.valid) { // the last frame was not handed over: no xrBeginFrame on this thread in between
			Log("no xrBeginFrame on this thread (%lu) between two frames - this session gives the picture to OBS at "
			    "xrEndFrame from now on",
			    GetCurrentThreadId());
			g_handAtBegin = false;
			g_pending.valid = false;
		} else if (EnsureSpare(g_spareTex, g_sharedTex) && (!inner || EnsureSpare(g_spareInner, g_innerTex))) {
			const int64_t s0 = Qpc();
			g_context->CopySubresourceRegion(g_spareTex, 0, 0, 0, 0, src, subres, &box);
			if (inner)
				g_context->CopySubresourceRegion(g_spareInner, 0, 0, 0, 0, src2, subres2, &box2);
			g_cost.spareUs += (double)(Qpc() - s0) * 1e6 / g_cost.freq;
			g_cost.spares++;
			MakePose(info, proj, eye, fullRect, inner, g_pending.pose);
			g_pending.inner = inner;
			g_pending.thread = GetCurrentThreadId();
			g_pending.endUs = g_pending.lockUs = 0; // set by hook_xrEndFrame
			g_pending.valid = true;
			logFirstCopy("into the spare at xrEndFrame, given to OBS at the next xrBeginFrame");
			return;
		} else {
			Log("no spare texture - this session gives the picture to OBS at xrEndFrame");
			g_handAtBegin = false;
		}
	}

	// At xrEndFrame (D3D12, or D3D11 without the hand-over at xrBeginFrame). Never block the game: if OBS
	// holds a texture right now, skip this frame. Order: main, then inner (OBS takes them in the same order).
	g_pending.valid = false; // an older frame in the spare must not follow this one
	const int64_t a0 = Qpc();
	HRESULT acq = g_sharedMutex->AcquireSync(0, 0);
	if (acq == S_OK && inner && g_innerMutex->AcquireSync(0, 0) != S_OK) {
		g_sharedMutex->ReleaseSync(0);
		acq = E_FAIL;
	}
	const int64_t c0 = Qpc();
	g_cost.acquireUs += (double)(c0 - a0) * 1e6 / g_cost.freq;
	if (acq != S_OK) {
		g_cost.busy++;
		return;
	}
	ID3D11Resource *wrapped[2] = {src, src2};
	const UINT nWrapped = (inner && src2 != src) ? 2u : 1u;
	if (g_on12) // D3D12: the image is released in RENDER_TARGET on the game's queue (spec, khr_d3d12_enable)
		g_on12->AcquireWrappedResources(wrapped, nWrapped);
	g_context->CopySubresourceRegion(g_sharedTex, 0, 0, 0, 0, src, subres, &box);
	if (inner)
		g_context->CopySubresourceRegion(g_innerTex, 0, 0, 0, 0, src2, subres2, &box2);
	if (g_on12)
		g_on12->ReleaseWrappedResources(wrapped, nWrapped);
	PoseSnap pose;
	MakePose(info, proj, eye, fullRect, inner, pose);
	WritePose(pose);
	if (inner)
		g_innerMutex->ReleaseSync(0);
	g_sharedMutex->ReleaseSync(0);
	const int64_t c1 = Qpc();
	g_context->Flush();
	const int64_t c2 = Qpc();
	g_cost.copyUs += (double)(c1 - c0) * 1e6 / g_cost.freq;
	g_cost.flushUs += (double)(c2 - c1) * 1e6 / g_cost.freq;
	FrameDelivered(now);
	logFirstCopy("given to OBS at xrEndFrame");
}

// D3D11: the next xrBeginFrame on the thread that ended the frame. The game has not queued its new frame
// yet, so the sync here does not wait for the driver (see g_pending). If OBS holds the picture right now
// (it does for ~0.1 ms per read), wait for it at most kHandoverWaitMs, else skip this frame.
// Returns true when a frame was given to OBS.
bool HandOver(int64_t t0, int64_t lockedAt)
{
	if (!g_pending.valid)
		return false;
	if (g_pending.thread != GetCurrentThreadId()) {
		Log("xrBeginFrame on thread %lu, xrEndFrame on thread %lu - this session gives the picture to OBS at "
		    "xrEndFrame from now on",
		    GetCurrentThreadId(), g_pending.thread);
		g_handAtBegin = false;
		g_pending.valid = false;
		return false;
	}
	g_pending.valid = false;
	if (!g_shm || !g_sharedTex || !g_spareTex || !OwnsChannel())
		return false;
	const bool inner = g_pending.inner && g_innerTex && g_spareInner;
	const int64_t a0 = Qpc();
	HRESULT acq = g_sharedMutex->AcquireSync(0, kHandoverWaitMs);
	if (acq == S_OK && inner && g_innerMutex->AcquireSync(0, kHandoverWaitMs) != S_OK) {
		g_sharedMutex->ReleaseSync(0);
		acq = E_FAIL;
	}
	const int64_t c0 = Qpc();
	CostStats &c = g_cost;
	c.acquireUs += (double)(c0 - a0) * 1e6 / c.freq;
	if (acq != S_OK) {
		c.busy++;
		return false;
	}
	g_context->CopyResource(g_sharedTex, g_spareTex);
	if (inner)
		g_context->CopyResource(g_innerTex, g_spareInner);
	PoseSnap pose = g_pending.pose;
	if (!inner)
		pose.flags &= ~QVD_POSE_INNER;
	WritePose(pose);
	if (inner)
		g_innerMutex->ReleaseSync(0);
	g_sharedMutex->ReleaseSync(0);
	const int64_t c1 = Qpc();
	g_context->Flush();
	const int64_t c2 = Qpc();
	c.copyUs += (double)(c1 - c0) * 1e6 / c.freq;
	c.flushUs += (double)(c2 - c1) * 1e6 / c.freq;
	const double beginUs = (double)(c2 - t0) * 1e6 / c.freq;
	c.beginUs += beginUs;
	c.hookUs.push_back(g_pending.endUs + beginUs); // the frame's whole cost: xrEndFrame + xrBeginFrame part
	c.lockUs += g_pending.lockUs + (double)(lockedAt - t0) * 1e6 / c.freq;
	FrameDelivered(GetTickCount64());
	if (!g_loggedHandover) {
		Log("first picture given to OBS at xrBeginFrame (thread %lu): AcquireSync %.0f us, copy+pose %.0f us, flush "
		    "%.0f us",
		    GetCurrentThreadId(), (double)(c0 - a0) * 1e6 / c.freq, (double)(c1 - c0) * 1e6 / c.freq,
		    (double)(c2 - c1) * 1e6 / c.freq);
		g_loggedHandover = true;
	}
	return true;
}

// ---------------------------------------------------------------- performance overlay
// User decision 2026-11-06: a key (chosen in the OBS source's Properties, default Ctrl+Shift+F) toggles a
// small panel with the game's FPS + frame time, the game's CPU (all logical CPUs = 100 %), its busiest
// thread (one core = 100 %) and the GPU load (3D engine, all processes, like Task Manager), shown in the
// headset (a quad layer fixed in the cockpit, moved with Ctrl+Shift+L + the ArchanXR move keys, see "moving
// the panel") and in the recording (OBS draws the same panel at the top left).
// A background thread reads the key (every 20 ms) and, only while the overlay is on, measures and draws once a
// second. The game thread only uploads the panel when it changed (once a second).
PFN_xrEnumerateSwapchainFormats next_EnumerateSwapchainFormats = nullptr;
PFN_xrWaitSwapchainImage next_WaitSwapchainImage = nullptr;
PFN_xrGetSystemProperties next_GetSystemProperties = nullptr;

// -1 = not measured yet ("--"), -2 = cannot be measured ("n/a")
struct OverlayStats {
	double fps = -1, frameMs = -1, cpu = -1, thread = -1, gpu = -1;
	// Archan's own cost on the game thread (user request 2026-11-06: users see the hit they take):
	// avg and worst ms per game frame, and its share of the frame time
	double archMs = -1, archMaxMs = -1, archPct = -1;
	bool capturing = false; // OBS took the picture in this interval
};

struct Overlay {
	std::atomic<bool> visible{false};
	std::atomic<bool> enabled{true}; // the on/off switch (settings window); off: never shown, the key does nothing
	std::atomic<uint32_t> key{0};    // vk | ctrl << 8 | shift << 9 | alt << 10
	// game frames (xrEndFrame of the captured session) and the QPC of the last one, as a pair: seqlock frameSeq
	std::atomic<uint32_t> frameSeq{0};
	std::atomic<uint64_t> frames{0};
	std::atomic<int64_t> frameQpc{0};
	// Archan's time on the game thread, QPC ticks: per frame = our part of xrEndFrame (before and after the
	// runtime's call) + our part of the swapchain acquire/release calls since the previous frame
	std::atomic<int64_t> hookTicks{0};                   // acquire/release, taken by the next xrEndFrame
	std::atomic<int64_t> archSum{0}, archMax{0};         // since the last sample (the overlay thread resets them)
	std::atomic<uint64_t> archFrames{0}, captured{0};    // frames measured / frames copied for OBS
	std::atomic<int64_t> archTotal{0};                   // since the start (tests)
	// where the head is in the panel's space (raw LOCAL), from the game's left view: x y z m, yaw pitch deg,
	// the game's space type (-1 = not known); for the log, to see whether the panel is in view
	std::atomic<float> head[5] = {};
	std::atomic<int> headSpace{-1};
	std::atomic<float> spaceOrigin[3] = {}; // the game's space origin in the runtime's (its poseInReferenceSpace)
	// The seat (user decision 2026-11-07, "head at first show"): neither the runtime nor DCS gives one on Pimax
	// (head 58 m from the space origin, straight ahead = yaw 87 deg). Each session, the first frame that shows
	// the panel takes the head's position and level facing; the panel's place is relative to it from then on.
	bool seatSet = false; // game thread
	float seat[4] = {};   // x y z m in the game's space, yaw rad
	std::atomic<bool> reseat{false}; // Ctrl+Shift+L unlock (user decision 2026-11-07): take the seat again now
	std::atomic<uint64_t> archTotalFrames{0};
	// cost on the game thread (panel upload), read by the overlay thread for its log
	std::atomic<uint64_t> uploads{0}, uploadNs{0}, uploadMaxNs{0};
	// the parts of the slowest upload in the log window: acquire+wait, copy, flush/submit, release (ns)
	std::atomic<uint64_t> partNs[4] = {};
	// the panel, drawn by the overlay thread (it also writes it to the shared memory for OBS)
	std::mutex panelMutex;
	std::vector<uint8_t> panel;
	uint64_t panelVersion = 0;
	OverlayStats last;
	// game thread, under g_mutex
	std::vector<uint8_t> upload;
	uint64_t uploadedVersion = 0;
	XrSwapchain swapchain = XR_NULL_HANDLE;
	XrSpace localSpace = XR_NULL_HANDLE; // the game's seated space: the panel is fixed in the cockpit
	// Where the panel stands in LOCAL (x, y, z m, pitch deg), written by the overlay thread, read by the game
	// thread each frame: seqlock placeSeq. Unlocked (Ctrl+Shift+L): the move keys belong to the overlay.
	std::atomic<uint32_t> placeSeq{0};
	std::atomic<float> place[4] = {};
	std::atomic<bool> unlocked{false};
	std::atomic<int> unlockRequest{-1}; // tests: 0 / 1 as if Ctrl+Shift+L was pressed
	std::atomic<bool> bgra{false};      // the headset swapchain is B8G8R8A8: the thread swaps red and blue for it
	std::vector<ID3D11Texture2D *> images; // D3D11 session
	// D3D12 session: the images and our own upload on the game's queue (measured: D3D11On12 UpdateSubresource +
	// Flush took 0.5 + 1.6 ms on the game thread)
	std::vector<ID3D12Resource *> images12;
	ID3D12CommandAllocator *alloc12 = nullptr;
	ID3D12GraphicsCommandList *list12 = nullptr;
	ID3D12Resource *buf12 = nullptr; // upload heap, mapped
	uint8_t *buf12Map = nullptr;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot12 = {};
	ID3D12Fence *fence12 = nullptr;
	uint64_t fence12Value = 0;
	bool haveImage = false, failed = false, loggedLimit = false, firstUploadLogged = false;
	uint32_t maxLayers = 16; // the spec's minimum; the runtime's own value is read at session start
	XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
	std::vector<const XrCompositionLayerBaseHeader *> layers;
	// the thread: a plain handle, not std::thread - a game that exits without ending its session would run
	// ~std::thread on a joinable thread at DLL unload = std::terminate (0xC0000409) at game exit
	HANDLE thread = nullptr;
	std::atomic<bool> stop{false};
	HANDLE wake = nullptr;
};
Overlay g_ovl;

uint32_t PackKey(const QvdKey &k)
{
	return k.vk | (k.ctrl ? 1u << 8 : 0u) | (k.shift ? 1u << 9 : 0u) | (k.alt ? 1u << 10 : 0u);
}
QvdKey UnpackKey(uint32_t p)
{
	return QvdKey{p & 0xFFu, (p & (1u << 8)) != 0, (p & (1u << 9)) != 0, (p & (1u << 10)) != 0};
}

uint64_t FtU64(const FILETIME &f)
{
	return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

// The panel's first place before it was ever moved (user decision 2026-11-07): straight ahead, its centre
// Centre, 10 cm below eye level, 1 m in front of the seated (recentred) position, upright; fixed in the cockpit,
// it does not follow the head (user decision 2026-11-07).
const float kPlaceDefault[4] = {0.0f, -0.10f, -1.0f, 0.0f};

// User decisions 2026-11-07/08: the headset panel is LOCKED TO THE VIEW, 20 % smaller, no seat and no move keys
// (Ctrl+Shift+L does nothing), "where you can see the blue performance overlay from DCS" in the user's OBS
// picture (screenshot 2026-11-08 11:26). That picture was the RIGHT eye (layer.log: every DCS session since
// 2026-11-07 20:02 has "OBS asks for the right eye"); 0.1.7 mapped it as the left eye and the panel sat on the
// left. The spot (85 % of the eye's centre 16:9 crop, at u 0.4927 v 0.6033 of the picture), mapped back to the
// right eye of this Pimax (3612x2900, FOV L 45.54 R 55.08 U/D 44.55 deg): tan x 0.1916, tan y -0.1211 in the
// right eye's own view = 10.8 deg right of and 6.9 deg below that eye's forward. The panel is placed from the
// game's right view pose (view 1) each frame, 1 m along that ray, facing the eye, so it lands on that spot of
// the right eye picture whatever the eye layout is. The cockpit-fixed panel stays in the code for later.
const bool kPanelMovable = false;
const uint32_t kPanelView = 1; // the right eye's view
const float kEyeTanX = 0.1916f, kEyeTanY = -0.1211f, kViewDist = 1.0f;
const float kPanelWidthM = 0.384f * 0.8f; // 0.3072 m at 1 m (was 0.384)

XrQuaternionf QMul(const XrQuaternionf &a, const XrQuaternionf &b)
{
	return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// The panel's pose relative to the right view: 1 m along the ray (kEyeTanX, kEyeTanY, -1), turned to face the eye
// (yaw about y, then pitch about x).
XrPosef PanelInEye()
{
	const float n = sqrtf(kEyeTanX * kEyeTanX + kEyeTanY * kEyeTanY + 1.0f);
	const float dx = kEyeTanX / n, dy = kEyeTanY / n, dz = -1.0f / n;
	const float a = atan2f(-dx, -dz), p = asinf(dy);
	const XrQuaternionf qy = {0.0f, sinf(a * 0.5f), 0.0f, cosf(a * 0.5f)}, qx = {sinf(p * 0.5f), 0.0f, 0.0f, cosf(p * 0.5f)};
	XrPosef r;
	r.orientation = QMul(qy, qx);
	r.position = {dx * kViewDist, dy * kViewDist, dz * kViewDist};
	return r;
}

void SetPlace(const float p[4])
{
	g_ovl.placeSeq++; // odd
	for (int i = 0; i < 4; i++)
		g_ovl.place[i] = p[i];
	g_ovl.placeSeq++; // even
}

void GetPlace(float p[4])
{
	for (;;) {
		const uint32_t s0 = g_ovl.placeSeq;
		for (int i = 0; i < 4; i++)
			p[i] = g_ovl.place[i];
		if (!(s0 & 1) && g_ovl.placeSeq == s0)
			return;
	}
}

// overlay.ini (written by OBS and the settings window) when it changed: the key and the on/off switch.
// Missing file: the default key, on. The panel's place is read once per session (the first read): after
// that the layer's own value is the one that counts (it saves it itself).
void OverlayReadSettings(FILETIME &seen, bool &first)
{
	WIN32_FILE_ATTRIBUTE_DATA a = {};
	const bool exists = GetFileAttributesExW(qvd_overlay_settings_path(), GetFileExInfoStandard, &a) != 0;
	if (!first && exists && CompareFileTime(&a.ftLastWriteTime, &seen) == 0)
		return;
	if (!first && !exists && seen.dwLowDateTime == 0 && seen.dwHighDateTime == 0)
		return; // still no file
	QvdOverlaySettings st;
	bool fileThere = false;
	if (!qvd_overlay_read(&st, &fileThere)) { // being replaced right now: next time (the default until then)
		if (first) {
			QvdKey k;
			qvd_parse_key(QVD_OVERLAY_DEFAULT_KEY, &k);
			g_ovl.key = PackKey(k);
			g_ovl.enabled = true;
			SetPlace(kPlaceDefault);
		}
		return;
	}
	if (first) {
		SetPlace(st.hasPlace ? st.place : kPlaceDefault);
		Log("overlay place: x %.3f y %.3f z %.3f m, pitch %.1f deg (%s)", st.hasPlace ? st.place[0] : kPlaceDefault[0],
		    st.hasPlace ? st.place[1] : kPlaceDefault[1], st.hasPlace ? st.place[2] : kPlaceDefault[2],
		    st.hasPlace ? st.place[3] : kPlaceDefault[3], st.hasPlace ? "saved" : "default: centre, 10 cm down, 1 m");
	}
	seen = exists ? a.ftLastWriteTime : FILETIME{};
	QvdKey k;
	char now[64];
	if (qvd_parse_key(st.key, &k)) {
		g_ovl.key = PackKey(k);
		qvd_format_key(k, now, sizeof(now));
	} else {
		if (first) {
			qvd_parse_key(QVD_OVERLAY_DEFAULT_KEY, &k);
			g_ovl.key = PackKey(k);
		}
		qvd_format_key(UnpackKey(g_ovl.key), now, sizeof(now));
		Log("overlay settings: key '%s' is not a key, using %s", st.key, now);
	}
	g_ovl.enabled = st.enabled;
	Log("overlay settings: %s, key %s (%ls)", st.enabled ? "on" : "OFF (the key does nothing)", now,
	    fileThere ? qvd_overlay_settings_path() : L"default - no settings file yet");
	first = false;
}

// OBS with our plugin is running: the plugin holds the presence mutex (a protected one still counts).
bool ObsRunning()
{
	HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, qvd_obs_presence_name());
	if (h) {
		CloseHandle(h);
		return true;
	}
	return GetLastError() == ERROR_ACCESS_DENIED;
}

// The thread entries follow each process entry (x64 sizes of the real structures).
static_assert(sizeof(SYSTEM_PROCESS_INFORMATION) == 0x100 && sizeof(SYSTEM_THREAD_INFORMATION) == 0x50,
	      "SYSTEM_PROCESS_INFORMATION / SYSTEM_THREAD_INFORMATION layout");

struct OverlaySampler {
	bool started = false;
	double freq = 0;
	int64_t qpc = 0, frameQpc = 0;
	uint64_t frames = 0, procCpu = 0;
	struct Th {
		HANDLE h = nullptr;
		uint64_t cpu = 0;
		bool seen = false;
	};
	std::unordered_map<DWORD, Th> threads;
	uint64_t threadsListedMs = 0;
	std::vector<uint8_t> spiBuf;
	double threadsMs = 0, gpuMs = 0; // this sample's cost
	PDH_HQUERY query = nullptr;
	PDH_HCOUNTER counter = nullptr;
	bool pdhFailed = false;
	std::vector<uint8_t> pdhBuf;
};

void SamplerStop(OverlaySampler &s)
{
	for (auto &kv : s.threads)
		if (kv.second.h)
			CloseHandle(kv.second.h);
	if (s.query)
		PdhCloseQuery(s.query);
	s = OverlaySampler();
}

// GPU load: \GPU Engine(*engtype_3D)\Utilization Percentage summed over all processes per GPU
// (instance names "pid_<p>_luid_<a>_<b>_phys_<n>_eng_<e>_engtype_3D"), the busiest GPU.
double SampleGpu(OverlaySampler &s)
{
	if (s.pdhFailed)
		return -2;
	if (!s.query) {
		PDH_STATUS r = PdhOpenQueryW(nullptr, 0, &s.query);
		if (r == ERROR_SUCCESS)
			r = PdhAddEnglishCounterW(s.query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &s.counter);
		const int64_t o0 = Qpc();
		if (r == ERROR_SUCCESS)
			r = PdhCollectQueryData(s.query);
		if (r == ERROR_SUCCESS)
			Log("overlay: GPU load counter opened, first read %.1f ms (overlay thread)",
			    (double)(Qpc() - o0) * 1e3 / s.freq);
		if (r != ERROR_SUCCESS) {
			Log("overlay: GPU load counter not available (PDH 0x%08lX) - GPU shows n/a", (unsigned long)r);
			if (s.query)
				PdhCloseQuery(s.query);
			s.query = nullptr;
			s.pdhFailed = true;
			return -2;
		}
		return -1; // a rate needs two samples
	}
	if (PdhCollectQueryData(s.query) != ERROR_SUCCESS)
		return -1;
	DWORD size = 0, count = 0;
	PDH_STATUS r = PdhGetFormattedCounterArrayW(s.counter, PDH_FMT_DOUBLE, &size, &count, nullptr);
	if (r != (PDH_STATUS)PDH_MORE_DATA)
		return -1;
	s.pdhBuf.resize(size);
	auto *items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(s.pdhBuf.data());
	if (PdhGetFormattedCounterArrayW(s.counter, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS)
		return -1;
	std::unordered_map<std::wstring, double> perGpu;
	for (DWORD i = 0; i < count; i++) {
		if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
			continue;
		const wchar_t *name = items[i].szName ? items[i].szName : L"";
		const wchar_t *a = wcsstr(name, L"luid_");
		const wchar_t *b = a ? wcsstr(a, L"_eng_") : nullptr;
		perGpu[a && b ? std::wstring(a, b) : std::wstring()] += items[i].FmtValue.doubleValue;
	}
	double best = 0;
	for (auto &kv : perGpu)
		best = std::max(best, kv.second);
	return std::min(best, 100.0);
}

OverlayStats Sample(OverlaySampler &s)
{
	OverlayStats st;
	if (s.freq == 0) {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		s.freq = (double)f.QuadPart;
	}
	const int64_t now = Qpc();
	uint64_t frames = 0;
	int64_t frameQpc = 0;
	for (int i = 0; i < 1000; i++) { // the game thread writes the pair once per frame
		const uint32_t a = g_ovl.frameSeq;
		if (a & 1u)
			continue;
		frames = g_ovl.frames;
		frameQpc = g_ovl.frameQpc;
		if (g_ovl.frameSeq == a)
			break;
	}
	FILETIME c, e, k, u;
	GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
	const uint64_t proc = FtU64(k) + FtU64(u);
	const double dt = s.started ? (double)(now - s.qpc) / s.freq : 0.0;

	// The game's threads (this process): the busiest one in the last interval. Measured on the user's PC
	// (11,200 threads system-wide): a Toolhelp thread snapshot takes 72-86 ms, NtQuerySystemInformation 7 ms,
	// GetThreadTimes on 553 open handles 0.13 ms. So: the thread list every 5 s, the times every second.
	const uint64_t nowMs = GetTickCount64();
	if (!s.threadsListedMs || nowMs - s.threadsListedMs >= 5000) {
		s.threadsListedMs = nowMs;
		static auto ntqsi = reinterpret_cast<NTSTATUS(NTAPI *)(ULONG, PVOID, ULONG, PULONG)>(
			GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
		if (s.spiBuf.empty())
			s.spiBuf.resize(1 << 20);
		ULONG need = 0;
		NTSTATUS r = ntqsi ? ntqsi(SystemProcessInformation, s.spiBuf.data(), (ULONG)s.spiBuf.size(), &need)
				   : (NTSTATUS)0xC0000002L;
		for (int i = 0; i < 4 && r == (NTSTATUS)0xC0000004L /* STATUS_INFO_LENGTH_MISMATCH */; i++) {
			s.spiBuf.resize(need + (1 << 16));
			r = ntqsi(SystemProcessInformation, s.spiBuf.data(), (ULONG)s.spiBuf.size(), &need);
		}
		if (r >= 0) {
			for (auto &kv : s.threads)
				kv.second.seen = false;
			const DWORD pid = GetCurrentProcessId();
			for (size_t off = 0; off + sizeof(SYSTEM_PROCESS_INFORMATION) <= s.spiBuf.size();) {
				const auto *p = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION *>(s.spiBuf.data() + off);
				if ((DWORD)(uintptr_t)p->UniqueProcessId == pid) {
					const auto *t = reinterpret_cast<const SYSTEM_THREAD_INFORMATION *>(p + 1);
					for (ULONG i = 0; i < p->NumberOfThreads; i++) {
						const DWORD tid = (DWORD)(uintptr_t)t[i].ClientId.UniqueThread;
						auto it = s.threads.find(tid);
						if (it == s.threads.end()) {
							HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
							if (!h)
								continue;
							it = s.threads.emplace(tid, OverlaySampler::Th{h, ~0ull, false}).first;
						}
						it->second.seen = true;
					}
					break;
				}
				if (!p->NextEntryOffset)
					break;
				off += p->NextEntryOffset;
			}
			for (auto it = s.threads.begin(); it != s.threads.end();) {
				if (!it->second.seen) {
					CloseHandle(it->second.h);
					it = s.threads.erase(it);
				} else {
					++it;
				}
			}
		}
	}
	uint64_t busiest = 0;
	for (auto &kv : s.threads) {
		FILETIME tc, tx, tk, tu;
		if (!GetThreadTimes(kv.second.h, &tc, &tx, &tk, &tu))
			continue;
		const uint64_t cpu = FtU64(tk) + FtU64(tu);
		if (kv.second.cpu != ~0ull && cpu >= kv.second.cpu) // ~0 = new: no interval yet
			busiest = std::max(busiest, cpu - kv.second.cpu);
		kv.second.cpu = cpu;
	}
	const int64_t tg = Qpc();
	s.threadsMs = (double)(tg - now) * 1e3 / s.freq;
	st.gpu = SampleGpu(s);
	s.gpuMs = (double)(Qpc() - tg) * 1e3 / s.freq;

	if (s.started && dt > 0.05) {
		// FPS from the frames' own times: frames since the last sample / time between the last frame then and
		// the last frame now (counting frames inside a 1 s window would be off by up to one frame)
		const uint64_t df = frames - s.frames;
		const double span = (double)(frameQpc - s.frameQpc) / s.freq;
		if (df && span > 0) {
			st.fps = (double)df / span;
			st.frameMs = span * 1000.0 / (double)df;
		} else {
			st.fps = 0; // no frame in this interval
		}
		const double ncpu = (double)std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
		st.cpu = (double)(proc - s.procCpu) * 1e-7 / dt / ncpu * 100.0;
		st.thread = std::min(100.0, (double)busiest * 1e-7 / dt * 100.0);
	}
	{ // Archan's cost since the last sample
		const int64_t sum = g_ovl.archSum.exchange(0), mx = g_ovl.archMax.exchange(0);
		const uint64_t n = g_ovl.archFrames.exchange(0), cap = g_ovl.captured.exchange(0);
		const double span = (double)(frameQpc - s.frameQpc) / s.freq;
		if (s.started && n) {
			st.archMs = (double)sum * 1e3 / s.freq / (double)n;
			st.archMaxMs = (double)mx * 1e3 / s.freq;
			st.archPct = span > 0 ? (double)sum / s.freq / span * 100.0 : -1;
			st.capturing = cap > 0;
		}
	}
	s.started = true;
	s.qpc = now;
	s.frames = frames;
	s.frameQpc = frameQpc;
	s.procCpu = proc;
	return st;
}

struct PanelGdi {
	HDC dc = nullptr;
	HBITMAP bmp = nullptr, oldBmp = nullptr;
	HFONT font = nullptr, oldFont = nullptr;
	uint8_t *bits = nullptr;
};

void PanelFree(PanelGdi &g)
{
	if (g.dc) {
		if (g.oldFont)
			SelectObject(g.dc, g.oldFont);
		if (g.oldBmp)
			SelectObject(g.dc, g.oldBmp);
		DeleteDC(g.dc);
	}
	if (g.bmp)
		DeleteObject(g.bmp);
	if (g.font)
		DeleteObject(g.font);
	g = PanelGdi();
}

bool PanelInit(PanelGdi &g)
{
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = (LONG)QVD_OVERLAY_W;
	bi.bmiHeader.biHeight = -(LONG)QVD_OVERLAY_H; // top row first
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	g.dc = CreateCompatibleDC(nullptr);
	if (g.dc)
		g.bmp = CreateDIBSection(g.dc, &bi, DIB_RGB_COLORS, (void **)&g.bits, nullptr, 0);
	// grey-scale anti-aliasing (not ClearType): the coverage becomes the alpha
	g.font = CreateFontW(-34, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
			     CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
	if (!g.dc || !g.bmp || !g.bits || !g.font) {
		Log("overlay: drawing the panel failed (GDI error %lu)", GetLastError());
		PanelFree(g);
		return false;
	}
	g.oldBmp = (HBITMAP)SelectObject(g.dc, g.bmp);
	g.oldFont = (HFONT)SelectObject(g.dc, g.font);
	SetBkMode(g.dc, TRANSPARENT);
	SetTextColor(g.dc, RGB(255, 255, 255));
	return true;
}

void FormatStatsLines(const OverlayStats &st, wchar_t lines[4][64])
{
	if (st.fps < 0)
		swprintf_s(lines[0], 64, L"FPS   --");
	else if (st.frameMs < 0)
		swprintf_s(lines[0], 64, L"FPS %5.1f", st.fps);
	else
		swprintf_s(lines[0], 64, L"FPS %5.1f  %5.1f ms", st.fps, st.frameMs);
	if (st.cpu < 0)
		swprintf_s(lines[1], 64, L"CPU   --");
	else
		swprintf_s(lines[1], 64, L"CPU %4.1f%%  thread %3.0f%%", st.cpu, st.thread);
	if (st.gpu == -2)
		swprintf_s(lines[2], 64, L"GPU  n/a");
	else if (st.gpu < 0)
		swprintf_s(lines[2], 64, L"GPU   --");
	else
		swprintf_s(lines[2], 64, L"GPU %3.0f%%", st.gpu);
	if (st.archMs < 0)
		swprintf_s(lines[3], 64, L"Archan --");
	else if (!st.capturing)
		swprintf_s(lines[3], 64, L"Archan not capturing");
	else
		swprintf_s(lines[3], 64, L"Archan %5.2f ms %4.1f%% max %.2f", st.archMs, st.archPct, st.archMaxMs);
}

const uint32_t kUnlockedBorder = 6; // px: an amber frame while unlocked (the move keys are taken)

// white text on a black panel at 60 % opacity, RGBA8 premultiplied
void PanelDraw(PanelGdi &g, const OverlayStats &st, bool unlocked, std::vector<uint8_t> &out)
{
	memset(g.bits, 0, (size_t)QVD_OVERLAY_W * QVD_OVERLAY_H * 4);
	wchar_t lines[4][64];
	FormatStatsLines(st, lines);
	for (int i = 0; i < 4; i++)
		TextOutW(g.dc, 18, 10 + i * 46, lines[i], (int)wcslen(lines[i]));
	GdiFlush();
	const uint32_t bgA = 153;
	out.resize((size_t)QVD_OVERLAY_W * QVD_OVERLAY_H * 4);
	for (size_t i = 0; i < (size_t)QVD_OVERLAY_W * QVD_OVERLAY_H; i++) {
		const uint32_t cov = g.bits[i * 4 + 1]; // green = text coverage (white text on black)
		const uint8_t a = (uint8_t)(cov + (bgA * (255u - cov) + 127u) / 255u);
		out[i * 4 + 0] = out[i * 4 + 1] = out[i * 4 + 2] = (uint8_t)cov;
		out[i * 4 + 3] = a;
	}
	if (unlocked) {
		for (uint32_t y = 0; y < QVD_OVERLAY_H; y++)
			for (uint32_t x = 0; x < QVD_OVERLAY_W; x++) {
				if (x >= kUnlockedBorder && x < QVD_OVERLAY_W - kUnlockedBorder && y >= kUnlockedBorder &&
				    y < QVD_OVERLAY_H - kUnlockedBorder)
					continue;
				uint8_t *p = &out[((size_t)y * QVD_OVERLAY_W + x) * 4];
				p[0] = 255; // amber, opaque
				p[1] = 176;
				p[2] = 0;
				p[3] = 255;
			}
	}
}

// Overlay thread, every 20 ms (not on the game's thread: measured 7-11 us per call there). The key is
// global (GetAsyncKeyState), so it works while the game has the focus. The key, then the modifiers.
bool OverlayPollKey(bool &wasDown)
{
	const QvdKey k = UnpackKey(g_ovl.key);
	if (!k.vk || !g_ovl.enabled)
		return false;
	auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
	const bool now = down((int)k.vk) && down(VK_CONTROL) == k.ctrl && down(VK_SHIFT) == k.shift && down(VK_MENU) == k.alt;
	const bool pressed = now && !wasDown;
	wasDown = now;
	if (pressed) {
		g_ovl.visible = !g_ovl.visible;
		char name[64];
		qvd_format_key(k, name, sizeof(name));
		Log("overlay %s (%s pressed)", g_ovl.visible ? "on" : "off", name);
	}
	return pressed;
}

// ---------------------------------------------------------------- moving the panel
// User decisions 2026-11-07: the panel is fixed in the cockpit. Ctrl+Shift+L unlocks it; while unlocked the
// overlay owns ArchanXR's in-game move keys (ArchanXR OpenXR layer, placement_keys.rs: Del/PgDn left/right,
// End/Home down/up, Ins/PgUp farther/closer, along the panel's own axes, 5 mm; Down/Up pitch, 1 deg about +X;
// only pitch is rotated; held RIGHT Shift = a tenth; no Ctrl or Alt held; one nudge on the press, then after
// 250 ms one every 50 ms). "It will take ownership of those so no other can use it until it's locked again":
// a low-level keyboard hook, installed only while unlocked, swallows them (measured 2026-11-07: a swallowed
// key does not reach GetAsyncKeyState either). Every game starts locked. The place is saved when the keys
// are let go. The face-locked mode (centre, 30 mm down) is not enabled yet (user decision).
enum { MK_LEFT, MK_RIGHT, MK_DOWN, MK_UP, MK_IN, MK_OUT, MK_PITCH_DOWN, MK_PITCH_UP, MK_COUNT };
const uint32_t kMoveVk[MK_COUNT] = {VK_DELETE, VK_NEXT, VK_END, VK_HOME, VK_INSERT, VK_PRIOR, VK_DOWN, VK_UP};

struct MoveKeys {
	HANDLE thread = nullptr;
	DWORD tid = 0;
	HANDLE ready = nullptr;
	bool hooked = false;
	DWORD error = 0;
	std::atomic<uint32_t> down{0};    // held now, and the press was ours
	std::atomic<uint32_t> passing{0}; // held since before the unlock: the game keeps it until it is let go
	std::atomic<uint32_t> presses[MK_COUNT] = {};
	std::atomic<uint64_t> swallowed{0};
	std::atomic<uint64_t> events{0}; // every key event the hook was called for (any key): is it called at all?
	std::atomic<uint32_t> lastVk{0};
};
MoveKeys g_mk;

LRESULT CALLBACK MoveKeysProc(int code, WPARAM wp, LPARAM lp)
{
	if (code == HC_ACTION) {
		const KBDLLHOOKSTRUCT *k = reinterpret_cast<const KBDLLHOOKSTRUCT *>(lp);
		g_mk.events++;
		g_mk.lastVk = k->vkCode;
		for (int i = 0; i < MK_COUNT; i++) {
			if (k->vkCode != kMoveVk[i])
				continue;
			const uint32_t bit = 1u << i;
			const bool isDown = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
			if (g_mk.passing & bit) {
				if (!isDown)
					g_mk.passing &= ~bit;
				break; // the game's own press: its release must reach it too
			}
			if (isDown) {
				if (!(g_mk.down & bit)) { // not the key's auto-repeat
					g_mk.down |= bit;
					g_mk.presses[i]++;
				}
			} else {
				g_mk.down &= ~bit;
			}
			g_mk.swallowed++;
			return 1;
		}
	}
	return CallNextHookEx(nullptr, code, wp, lp);
}

// The hook's own thread: a low-level hook is called on the thread that set it, through its message loop.
DWORD WINAPI MoveKeysThread(void *)
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST); // every key press waits for this proc
	MSG m;
	PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE); // the message queue exists before anyone posts WM_QUIT
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   reinterpret_cast<LPCWSTR>(&MoveKeysProc), &self);
	HHOOK h = SetWindowsHookExW(WH_KEYBOARD_LL, MoveKeysProc, self, 0);
	g_mk.error = h ? 0 : GetLastError();
	g_mk.hooked = h != nullptr;
	SetEvent(g_mk.ready);
	if (!h)
		return 0;
	while (GetMessageW(&m, nullptr, 0, 0) > 0) {
	}
	UnhookWindowsHookEx(h);
	return 0;
}

void MoveKeysStop()
{
	if (!g_mk.thread)
		return;
	PostThreadMessageW(g_mk.tid, WM_QUIT, 0, 0);
	if (WaitForSingleObject(g_mk.thread, 2000) != WAIT_OBJECT_0)
		Log("overlay: the move-key thread did not stop in 2 s");
	CloseHandle(g_mk.thread);
	g_mk.thread = nullptr;
	g_mk.hooked = false;
	g_mk.down = 0;
	g_mk.passing = 0;
}

bool MoveKeysStart()
{
	MoveKeysStop();
	uint32_t held = 0;
	for (int i = 0; i < MK_COUNT; i++)
		if (GetAsyncKeyState((int)kMoveVk[i]) & 0x8000)
			held |= 1u << i;
	g_mk.passing = held;
	g_mk.down = 0;
	for (auto &p : g_mk.presses)
		p = 0;
	if (!g_mk.ready)
		g_mk.ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	ResetEvent(g_mk.ready);
	g_mk.hooked = false;
	g_mk.error = 0;
	g_mk.thread = CreateThread(nullptr, 0, MoveKeysThread, nullptr, 0, &g_mk.tid);
	if (!g_mk.thread) {
		g_mk.error = GetLastError();
		return false;
	}
	WaitForSingleObject(g_mk.ready, 2000);
	if (!g_mk.hooked) {
		WaitForSingleObject(g_mk.thread, 2000);
		CloseHandle(g_mk.thread);
		g_mk.thread = nullptr;
		return false;
	}
	return true;
}

struct MoveState {
	uint32_t seen[MK_COUNT] = {};
	uint64_t next[MK_COUNT] = {};
	bool dirty = false; // moved since the last save
	// diagnostics (2026-11-07: in DCS the hook took 0 keys while move keys were pressed): once a second
	uint64_t diagAt = 0, diagEvents = 0;
	uint32_t diagHeldSeen = 0; // move keys the keyboard state showed held in this second (the hook did not take them)
};

void SavePlace()
{
	float p[4];
	GetPlace(p);
	QvdOverlaySettings st;
	bool exists = false, ok = false;
	for (int i = 0; i < 20 && !(ok = qvd_overlay_read(&st, &exists)); i++)
		Sleep(10);
	if (!ok) {
		Log("overlay moved to x %.3f y %.3f z %.3f m, pitch %.1f deg - NOT saved (%ls cannot be read)", p[0], p[1], p[2],
		    p[3], qvd_overlay_settings_path());
		return;
	}
	st.hasPlace = true;
	for (int i = 0; i < 4; i++)
		st.place[i] = p[i];
	if (qvd_overlay_write(st))
		Log("overlay moved to x %.3f y %.3f z %.3f m, pitch %.1f deg - saved", p[0], p[1], p[2], p[3]);
	else
		Log("overlay moved to x %.3f y %.3f z %.3f m, pitch %.1f deg - NOT saved (error %lu writing %ls)", p[0], p[1],
		    p[2], p[3], GetLastError(), qvd_overlay_settings_path());
}

void OverlaySetUnlocked(bool on, MoveState &m, const char *why)
{
	if (on == g_ovl.unlocked)
		return;
	if (on) {
		if (!MoveKeysStart()) {
			Log("overlay: cannot take the move keys (SetWindowsHookEx error %lu) - stays locked", g_mk.error);
			return;
		}
		m = MoveState();
		// re-seat (user decision 2026-11-07): the middle, 10 cm down, 1 m in front of where the head looks now
		SetPlace(kPlaceDefault);
		g_ovl.reseat = true;
		m.dirty = true; // saved when it is locked again
		g_ovl.unlocked = true;
		Log("overlay UNLOCKED (%s): re-seated in front of the head (middle, 10 cm down, 1 m); Del/PgDn left/right, "
		    "Home/End up/down, Ins/PgUp farther/closer, Up/Down pitch (Right Shift = fine); the game does not get these "
		    "keys until Ctrl+Shift+L locks it again",
		    why);
	} else {
		MoveKeysStop();
		g_ovl.unlocked = false;
		Log("overlay locked (%s): the move keys belong to the game again (%llu key events were taken)", why,
		    (unsigned long long)g_mk.swallowed.load());
		if (m.dirty) {
			SavePlace();
			m.dirty = false;
		}
	}
}

// Overlay thread, every 20 ms while unlocked: the presses and held keys the hook took -> the panel's place.
void OverlayMove(MoveState &m, uint64_t now)
{
	auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
	const bool mods = down(VK_CONTROL) || down(VK_MENU); // ArchanXR's bare move keys: no Ctrl, no Alt
	const bool fine = down(VK_RSHIFT);
	const float step = fine ? 0.0005f : 0.005f, deg = fine ? 0.1f : 1.0f;
	const uint32_t held = g_mk.down;
	float p[4];
	GetPlace(p);
	bool moved = false;
	for (int i = 0; i < MK_COUNT; i++) {
		const uint32_t pr = g_mk.presses[i];
		uint32_t n = 0;
		if (pr != m.seen[i]) { // new presses: one nudge each, then the repeat after 250 ms
			n = std::min<uint32_t>(pr - m.seen[i], 4);
			m.seen[i] = pr;
			m.next[i] = now + 250;
		} else if ((held >> i) & 1u) {
			while (now >= m.next[i] && n < 4) {
				n++;
				m.next[i] += 50;
			}
			if (now >= m.next[i])
				m.next[i] = now + 50;
		}
		if (!n || mods)
			continue;
		const float a = p[3] * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a), d = step * (float)n;
		// the panel's own axes, pitched about x: x = (1,0,0), y = (0,c,s), z = (0,-s,c); in = -z
		switch (i) {
		case MK_LEFT: p[0] -= d; break;
		case MK_RIGHT: p[0] += d; break;
		case MK_DOWN: p[1] -= c * d; p[2] -= s * d; break;
		case MK_UP: p[1] += c * d; p[2] += s * d; break;
		case MK_IN: p[1] += s * d; p[2] -= c * d; break;
		case MK_OUT: p[1] -= s * d; p[2] += c * d; break;
		case MK_PITCH_DOWN: p[3] -= deg * (float)n; break;
		case MK_PITCH_UP: p[3] += deg * (float)n; break;
		}
		if (p[3] > 180.0f)
			p[3] -= 360.0f;
		if (p[3] <= -180.0f)
			p[3] += 360.0f;
		moved = true;
	}
	if (moved) {
		SetPlace(p);
		m.dirty = true;
	}
	if (m.dirty && !held) { // let go: saved
		SavePlace();
		m.dirty = false;
	}
	// A key the hook takes never shows in the keyboard state (proven 2026-11-07), so a move key seen held here
	// went past the hook. Logged once a second while something happens.
	for (int i = 0; i < MK_COUNT; i++)
		if (down((int)kMoveVk[i]))
			m.diagHeldSeen |= 1u << i;
	if (!m.diagAt)
		m.diagAt = now, m.diagEvents = g_mk.events;
	if (now - m.diagAt >= 1000) {
		const uint64_t ev = g_mk.events - m.diagEvents;
		if (ev || m.diagHeldSeen) {
			static const char *const names[MK_COUNT] = {"Del", "PgDn", "End", "Home", "Ins", "PgUp", "Down", "Up"};
			char got[80] = "";
			for (int i = 0; i < MK_COUNT; i++)
				if ((m.diagHeldSeen >> i) & 1u) {
					strcat_s(got, got[0] ? " " : "");
					strcat_s(got, names[i]);
				}
			Log("overlay unlocked, last 1 s: the hook was called for %llu key events (last vk 0x%02X), took %llu move keys "
			    "in all; move keys that went PAST the hook to the game: %s",
			    (unsigned long long)ev, g_mk.lastVk.load(), (unsigned long long)g_mk.swallowed.load(),
			    got[0] ? got : "none");
		}
		m.diagAt = now;
		m.diagEvents = g_mk.events;
		m.diagHeldSeen = 0;
	}
}

void OverlayThread()
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	FILETIME seen = {};
	bool first = true;
	OverlaySampler s;
	PanelGdi g;
	bool gdiTried = false;
	uint64_t nextSample = 0, nextKeyCheck = 0, logWindow = 0;
	double workMs = 0, threadsMs = 0, gpuMs = 0, drawMs = 0;
	uint32_t updates = 0;
	uint64_t upBefore = g_ovl.uploads, upNsBefore = g_ovl.uploadNs;
	double obsMs = 0;
	bool keyDown = false;
	int obsWas = -1; // unknown at the start
	bool enabledWas = true;
	MoveState move;
	bool lockKeyDown = false, unlockedDrawn = false;
	OverlayStats lastSt;
	bool haveLast = false;
	while (!g_ovl.stop) {
		uint64_t now = GetTickCount64();
		if (now >= nextKeyCheck) {
			OverlayReadSettings(seen, first);
			nextKeyCheck = now + 500;
			// User decision 2026-11-07: shown by default while OBS runs, the key hides / shows it; OBS closed:
			// off until the key is pressed. The switch off: never shown.
			const bool obs = ObsRunning(), en = g_ovl.enabled;
			if (!en) {
				if (enabledWas || g_ovl.visible)
					Log("overlay off (switched off in the settings)");
				g_ovl.visible = false;
			} else if (obs != (obsWas == 1) || !enabledWas) {
				if (obs) {
					g_ovl.visible = true;
					Log("OBS is running - overlay shown (the key hides it)");
					nextSample = 0;
				} else if (obsWas == 1) {
					g_ovl.visible = false;
					Log("OBS closed - overlay off (the key shows it)");
				}
			}
			obsWas = obs ? 1 : 0;
			enabledWas = en;
		}
		if (OverlayPollKey(keyDown))
			nextSample = 0; // turned on: draw right away
		if (kPanelMovable) { // Ctrl+Shift+L: locked <-> unlocked (only while the panel is shown)
			auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
			const bool l = down('L') && down(VK_CONTROL) && down(VK_SHIFT) && !down(VK_MENU);
			int want = -1;
			if (l && !lockKeyDown) {
				if (g_ovl.visible)
					want = g_ovl.unlocked ? 0 : 1;
				else
					Log("Ctrl+Shift+L: the overlay is hidden - show it first to move it");
			}
			lockKeyDown = l;
			const int req = g_ovl.unlockRequest.exchange(-1);
			if (req >= 0 && g_ovl.visible)
				want = req;
			if (want >= 0)
				OverlaySetUnlocked(want == 1, move, req >= 0 ? "test" : "Ctrl+Shift+L");
			if (g_ovl.unlocked && !g_ovl.visible)
				OverlaySetUnlocked(false, move, "overlay hidden");
			if (g_ovl.unlocked)
				OverlayMove(move, now);
		}
		const bool vis = g_ovl.visible;
		if (g_shm && OwnsChannel() && g_shm->overlayVisible != (uint32_t)vis)
			g_shm->overlayVisible = vis; // OBS shows / hides the panel
		// the panel to the headset and to OBS
		auto publish = [&](const OverlayStats &st) {
			std::vector<uint8_t> px;
			const bool un = g_ovl.unlocked;
			PanelDraw(g, st, un, px);
			unlockedDrawn = un;
			const int64_t to = Qpc();
			if (g_shm && OwnsChannel()) { // the recording: OBS draws the same panel (RGBA)
				g_shm->overlaySeq = g_shm->overlaySeq + 1; // odd
				MemoryBarrier();
				memcpy(g_shm->overlayPixels, px.data(), px.size());
				g_shm->overlayTickMs = GetTickCount64();
				MemoryBarrier();
				g_shm->overlaySeq = g_shm->overlaySeq + 1; // even
			}
			obsMs += (double)(Qpc() - to) * 1e3 / s.freq;
			if (g_ovl.bgra)
				for (size_t i = 0; i < px.size(); i += 4)
					std::swap(px[i], px[i + 2]);
			std::lock_guard<std::mutex> lk(g_ovl.panelMutex);
			g_ovl.panel.swap(px);
			g_ovl.panelVersion++;
			g_ovl.last = st;
		};
		if (g_ovl.visible && g.dc && haveLast && unlockedDrawn != g_ovl.unlocked && now < nextSample)
			publish(lastSt); // locked / unlocked: the frame right away, with the numbers already shown
		if (g_ovl.visible) {
			if (now >= nextSample) {
				const int64_t t0 = Qpc();
				const OverlayStats st = Sample(s);
				const int64_t td = Qpc();
				threadsMs += s.threadsMs;
				gpuMs += s.gpuMs;
				if (!gdiTried) {
					gdiTried = true;
					PanelInit(g);
				}
				if (g.dc) {
					publish(st);
					lastSt = st;
					haveLast = true;
				}
				const int64_t te = Qpc();
				drawMs += (double)(te - td) * 1e3 / s.freq;
				workMs += (double)(te - t0) * 1e3 / s.freq;
				updates++;
				if (!logWindow) { // the first update (opening the counters) is logged on its own, not averaged
					logWindow = now;
					workMs = threadsMs = gpuMs = drawMs = obsMs = 0;
					updates = 0;
				}
				if (now - logWindow >= 10000) {
					const uint64_t ups = g_ovl.uploads - upBefore, upNs = g_ovl.uploadNs - upNsBefore;
					const uint64_t upMax = g_ovl.uploadMaxNs.exchange(0);
					const double u = updates ? (double)updates : 1.0;
					float pl[4];
					GetPlace(pl);
					Log("overlay (last %.0f s): FPS %.1f, %.2f ms per frame, CPU %.1f %%, busiest thread %.0f %% (%zu "
					    "threads), GPU %.0f %%, Archan %s %.3f ms per frame (%.2f %%, max %.3f ms); cost on the overlay thread %.2f ms per update (threads %.2f, GPU %.2f, "
					    "drawing %.2f; %u updates); on the game thread panel upload avg %.0f us, max %.0f us (%llu uploads; "
					    "slowest: acquire+wait %.0f, copy %.0f, flush/submit %.0f, release %.0f us); for OBS %.0f us "
					    "(overlay thread); in the game's space (type %d): head at x %.2f y %.2f z %.2f m, yaw %.0f pitch "
					    "%.0f deg, panel at x %.2f y %.2f z %.2f m; the game's space origin in the runtime's: x %.2f y "
					    "%.2f z %.2f m",
					    (double)(now - logWindow) / 1000.0, st.fps, st.frameMs, st.cpu, st.thread, s.threads.size(),
					    st.gpu, st.capturing ? "capturing" : "not capturing", st.archMs, st.archPct, st.archMaxMs, workMs / u, threadsMs / u, gpuMs / u, drawMs / u, updates,
					    ups ? (double)upNs / ups / 1000.0 : 0.0, (double)upMax / 1000.0, (unsigned long long)ups,
					    g_ovl.partNs[0] / 1000.0, g_ovl.partNs[1] / 1000.0, g_ovl.partNs[2] / 1000.0,
					    g_ovl.partNs[3] / 1000.0, obsMs * 1000.0 / u, g_ovl.headSpace.load(), g_ovl.head[0].load(),
					    g_ovl.head[1].load(), g_ovl.head[2].load(), g_ovl.head[3].load(), g_ovl.head[4].load(), pl[0],
					    pl[1], pl[2], g_ovl.spaceOrigin[0].load(), g_ovl.spaceOrigin[1].load(),
					    g_ovl.spaceOrigin[2].load());
					logWindow = now;
					workMs = threadsMs = gpuMs = drawMs = obsMs = 0;
					updates = 0;
					upBefore = g_ovl.uploads;
					upNsBefore = g_ovl.uploadNs;
				}
				nextSample = now + 1000;
			}
		} else if (s.started || s.query) {
			SamplerStop(s); // nothing is measured while the overlay is off
			nextSample = 0;
			logWindow = 0;
		}
		WaitForSingleObject(g_ovl.wake, 20); // the key check
	}
	OverlaySetUnlocked(false, move, "game session ended"); // the next one starts locked (user decision)
	SamplerStop(s);
	PanelFree(g);
}

DWORD WINAPI OverlayThreadMain(void *)
{
	OverlayThread();
	return 0;
}

void OverlayStartThread()
{
	if (g_ovl.thread)
		return;
	if (!g_ovl.wake)
		g_ovl.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	g_ovl.stop = false;
	g_ovl.thread = CreateThread(nullptr, 0, OverlayThreadMain, nullptr, 0, nullptr);
	if (!g_ovl.thread)
		Log("overlay: CreateThread failed (%lu) - no overlay in this session", GetLastError());
}

// The thread never takes g_mutex, so this may be called while holding it.
void OverlayStopThread()
{
	if (!g_ovl.thread)
		return;
	g_ovl.stop = true;
	SetEvent(g_ovl.wake);
	WaitForSingleObject(g_ovl.thread, INFINITE);
	CloseHandle(g_ovl.thread);
	g_ovl.thread = nullptr;
}

void OverlayReleaseXr()
{
	g_ovl.seatSet = false; // a new session takes its seat again
	g_ovl.images.clear();
	g_ovl.images12.clear(); // the swapchain's: not ours
	if (g_ovl.fence12 && g_ovl.fence12->GetCompletedValue() < g_ovl.fence12Value) { // our copy done first
		HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (ev && SUCCEEDED(g_ovl.fence12->SetEventOnCompletion(g_ovl.fence12Value, ev)))
			WaitForSingleObject(ev, 1000);
		if (ev)
			CloseHandle(ev);
	}
	if (g_ovl.list12)
		g_ovl.list12->Release();
	if (g_ovl.alloc12)
		g_ovl.alloc12->Release();
	if (g_ovl.buf12)
		g_ovl.buf12->Release();
	if (g_ovl.fence12)
		g_ovl.fence12->Release();
	g_ovl.list12 = nullptr;
	g_ovl.alloc12 = nullptr;
	g_ovl.buf12 = nullptr;
	g_ovl.buf12Map = nullptr;
	g_ovl.fence12 = nullptr;
	g_ovl.fence12Value = 0;
	if (g_ovl.swapchain && next_DestroySwapchain)
		next_DestroySwapchain(g_ovl.swapchain);
	g_ovl.swapchain = XR_NULL_HANDLE;
	if (g_ovl.localSpace && next_DestroySpace)
		next_DestroySpace(g_ovl.localSpace);
	g_ovl.localSpace = XR_NULL_HANDLE;
	g_ovl.haveImage = false;
	g_ovl.firstUploadLogged = false;
	g_ovl.failed = false;
	g_ovl.loggedLimit = false;
	g_ovl.uploadedVersion = 0;
}

// The headset panel: our own swapchain + a VIEW space (head-locked), created on the game's session the
// first time the overlay is shown. Called straight on the runtime (next_*), never through our hooks.
bool OverlayCreateXr()
{
	auto fail = [](const char *what, XrResult r) {
		Log("overlay: %s failed (%d) - the panel is not shown in the headset", what, (int)r);
		OverlayReleaseXr();
		g_ovl.failed = true;
		return false;
	};
	if (!next_EnumerateSwapchainFormats || !next_WaitSwapchainImage)
		return fail("getting the runtime's swapchain functions", XR_ERROR_FUNCTION_UNSUPPORTED);
	uint32_t n = 0;
	XrResult r = next_EnumerateSwapchainFormats(g_session, 0, &n, nullptr);
	std::vector<int64_t> formats(n);
	if (XR_SUCCEEDED(r) && n)
		r = next_EnumerateSwapchainFormats(g_session, n, &n, formats.data());
	if (XR_FAILED(r))
		return fail("xrEnumerateSwapchainFormats", r);
	int64_t format = 0;
	const int64_t want[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM,
				DXGI_FORMAT_B8G8R8A8_UNORM};
	for (int64_t w : want) {
		if (std::find(formats.begin(), formats.end(), w) != formats.end()) {
			format = w;
			break;
		}
	}
	if (!format)
		return fail("finding an 8-bit RGBA/BGRA swapchain format", XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED);
	g_ovl.bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM;

	XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
			XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	ci.format = format;
	ci.sampleCount = 1;
	ci.width = QVD_OVERLAY_W;
	ci.height = QVD_OVERLAY_H;
	ci.faceCount = 1;
	ci.arraySize = 1;
	ci.mipCount = 1;
	r = next_CreateSwapchain(g_session, &ci, &g_ovl.swapchain);
	if (XR_FAILED(r)) {
		g_ovl.swapchain = XR_NULL_HANDLE;
		return fail("xrCreateSwapchain", r);
	}
	r = next_EnumerateSwapchainImages(g_ovl.swapchain, 0, &n, nullptr);
	if (XR_FAILED(r) || n == 0)
		return fail("xrEnumerateSwapchainImages", r);
	if (g_d12Device) {
		std::vector<XrSwapchainImageD3D12KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
		r = next_EnumerateSwapchainImages(g_ovl.swapchain, n, &n,
						  reinterpret_cast<XrSwapchainImageBaseHeader *>(imgs.data()));
		if (XR_FAILED(r))
			return fail("xrEnumerateSwapchainImages", r);
		for (uint32_t i = 0; i < n; i++)
			g_ovl.images12.push_back(imgs[i].texture);
		const D3D12_RESOURCE_DESC td = imgs[0].texture->GetDesc();
		UINT64 total = 0;
		g_d12Device->GetCopyableFootprints(&td, 0, 1, 0, &g_ovl.foot12, nullptr, nullptr, &total);
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC bd = {};
		bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bd.Width = total;
		bd.Height = 1;
		bd.DepthOrArraySize = 1;
		bd.MipLevels = 1;
		bd.SampleDesc.Count = 1;
		bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		HRESULT hr = g_d12Device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
								  nullptr, __uuidof(ID3D12Resource), (void **)&g_ovl.buf12);
		const D3D12_RANGE none = {0, 0};
		if (SUCCEEDED(hr))
			hr = g_ovl.buf12->Map(0, &none, (void **)&g_ovl.buf12Map);
		if (SUCCEEDED(hr))
			hr = g_d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
								  (void **)&g_ovl.alloc12);
		if (SUCCEEDED(hr))
			hr = g_d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_ovl.alloc12, nullptr,
							     __uuidof(ID3D12GraphicsCommandList), (void **)&g_ovl.list12);
		if (SUCCEEDED(hr))
			hr = g_ovl.list12->Close();
		if (SUCCEEDED(hr))
			hr = g_d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&g_ovl.fence12);
		if (FAILED(hr))
			return fail("setting up the D3D12 upload", (XrResult)hr);
	} else {
		g_ovl.images.assign(n, nullptr);
		std::vector<XrSwapchainImageD3D11KHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
		r = next_EnumerateSwapchainImages(g_ovl.swapchain, n, &n,
						  reinterpret_cast<XrSwapchainImageBaseHeader *>(imgs.data()));
		if (XR_FAILED(r))
			return fail("xrEnumerateSwapchainImages", r);
		for (uint32_t i = 0; i < n; i++)
			g_ovl.images[i] = imgs[i].texture;
	}
	XrReferenceSpaceCreateInfo sci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	// locked to the view (kPanelMovable false) or the runtime's LOCAL (the cockpit-fixed panel, kept for later)
	sci.referenceSpaceType = kPanelMovable ? XR_REFERENCE_SPACE_TYPE_LOCAL : XR_REFERENCE_SPACE_TYPE_VIEW;
	sci.poseInReferenceSpace.orientation.w = 1.0f;
	r = next_CreateReferenceSpace(g_session, &sci, &g_ovl.localSpace);
	if (XR_FAILED(r)) {
		g_ovl.localSpace = XR_NULL_HANDLE;
		return fail(kPanelMovable ? "xrCreateReferenceSpace(LOCAL)" : "xrCreateReferenceSpace(VIEW)", r);
	}
	// fixed in the cockpit (the game's seated space, user decision 2026-11-07), 0.384 m x 0.118 m (640 x 196 px
	// at 0.6 mm per px); its place is set every frame from g_ovl.place
	XrCompositionLayerQuad &q = g_ovl.quad;
	q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
	q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; // premultiplied alpha
	q.space = g_ovl.localSpace;
	q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	q.subImage.swapchain = g_ovl.swapchain;
	q.subImage.imageRect = {{0, 0}, {(int32_t)QVD_OVERLAY_W, (int32_t)QVD_OVERLAY_H}};
	q.subImage.imageArrayIndex = 0;
	q.pose.orientation.w = 1.0f;
	q.size = {kPanelWidthM, kPanelWidthM * (float)QVD_OVERLAY_H / (float)QVD_OVERLAY_W};
	Log("overlay: headset panel ready (%ux%u, format %lld, %u images, runtime allows %u layers)", QVD_OVERLAY_W,
	    QVD_OVERLAY_H, (long long)format, n, g_ovl.maxLayers);
	return true;
}

uint64_t PartNs(int64_t a, int64_t b)
{
	return g_cost.freq > 0 ? (uint64_t)((double)(b - a) * 1e9 / g_cost.freq) : 0;
}

void OverlayUpload(uint64_t parts[4])
{
	const int64_t p0 = Qpc();
	uint32_t idx = 0;
	XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
	if (g_ovl.fence12 && g_ovl.fence12->GetCompletedValue() < g_ovl.fence12Value)
		return; // the GPU has not done the copy of a second ago yet: next frame (the upload buffer is in use)
	XrResult r = next_AcquireSwapchainImage(g_ovl.swapchain, &ai, &idx);
	if (XR_FAILED(r) || idx >= (g_d12Device ? g_ovl.images12.size() : g_ovl.images.size())) {
		Log("overlay: xrAcquireSwapchainImage failed (%d)", (int)r);
		return;
	}
	XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
	wi.timeout = XR_INFINITE_DURATION;
	r = next_WaitSwapchainImage(g_ovl.swapchain, &wi);
	const int64_t p1 = Qpc();
	int64_t p2 = p1;
	if (r == XR_SUCCESS && g_d12Device) {
		// our own command list on the game's queue, before the release (khr_d3d12_enable: COLOR_ATTACHMENT images
		// are in RENDER_TARGET while acquired and must be back in it at release)
		for (uint32_t y = 0; y < QVD_OVERLAY_H; y++)
			memcpy(g_ovl.buf12Map + g_ovl.foot12.Offset + (size_t)y * g_ovl.foot12.Footprint.RowPitch,
			       g_ovl.upload.data() + (size_t)y * QVD_OVERLAY_W * 4, QVD_OVERLAY_W * 4);
		ID3D12Resource *img = g_ovl.images12[idx];
		g_ovl.alloc12->Reset();
		g_ovl.list12->Reset(g_ovl.alloc12, nullptr);
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = img;
		b.Transition.Subresource = 0;
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		g_ovl.list12->ResourceBarrier(1, &b);
		D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
		dst.pResource = img;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.SubresourceIndex = 0;
		src.pResource = g_ovl.buf12;
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint = g_ovl.foot12;
		g_ovl.list12->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		g_ovl.list12->ResourceBarrier(1, &b);
		g_ovl.list12->Close();
		p2 = Qpc();
		ID3D12CommandList *lists[] = {g_ovl.list12};
		g_d12Queue->ExecuteCommandLists(1, lists);
		g_d12Queue->Signal(g_ovl.fence12, ++g_ovl.fence12Value);
	} else if (r == XR_SUCCESS) {
		ID3D11Texture2D *t = g_ovl.images[idx];
		g_context->UpdateSubresource(t, 0, nullptr, g_ovl.upload.data(), QVD_OVERLAY_W * 4, 0);
		p2 = Qpc();
		g_context->Flush(); // kept: not proven that every runtime syncs the game's unflushed D3D11 work
	} else {
		Log("overlay: xrWaitSwapchainImage returned %d", (int)r);
	}
	const int64_t p3 = Qpc();
	XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	r = next_ReleaseSwapchainImage(g_ovl.swapchain, &ri);
	if (XR_SUCCEEDED(r))
		g_ovl.haveImage = true;
	const int64_t p4 = Qpc();
	parts[0] = PartNs(p0, p1);
	parts[1] = PartNs(p1, p2);
	parts[2] = PartNs(p2, p3);
	parts[3] = PartNs(p3, p4);
}

// Game thread, under g_mutex. Returns the frame to send on: the game's, or 'mod' = the game's frame with
// the overlay panel added on top.
const XrFrameEndInfo *OverlayFrame(const XrFrameEndInfo *info, XrFrameEndInfo &mod)
{
	g_ovl.frameSeq++; // odd
	g_ovl.frames++;
	g_ovl.frameQpc = Qpc();
	g_ovl.frameSeq++; // even
	if (!g_device)
		return info;
	if (!g_ovl.visible)
		return info;
	// The panel stands in the game's own space (the one its projection layer uses: DCS sets its seat there; the
	// runtime's raw LOCAL origin was measured 58 m from the head in DCS on Pimax, 2026-11-07). The head (left
	// view) in that space goes to the log, to see where the panel is from the head.
	XrSpace gameSpace = XR_NULL_HANDLE;
	for (uint32_t i = 0; info && i < info->layerCount; i++) {
		const XrCompositionLayerBaseHeader *l = info->layers[i];
		if (!l || l->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION)
			continue;
		const auto *pl = reinterpret_cast<const XrCompositionLayerProjection *>(l);
		if (pl->viewCount < 1 || pl->space == XR_NULL_HANDLE)
			break;
		gameSpace = pl->space;
		const XrPosef &v = pl->views[0].pose;
		const float minusZ[3] = {0.0f, 0.0f, -1.0f};
		const XrQuaternionf vinv = {-v.orientation.x, -v.orientation.y, -v.orientation.z, v.orientation.w};
		float fwd[3];
		RotateByInverse(vinv, minusZ, fwd); // = rotate by the view's orientation
		g_ovl.head[0] = v.position.x;
		g_ovl.head[1] = v.position.y;
		g_ovl.head[2] = v.position.z;
		g_ovl.head[3] = atan2f(-fwd[0], -fwd[2]) * 57.29578f; // yaw, 0 = looking along -z (at the panel)
		g_ovl.head[4] = asinf(fwd[1] < -1.0f ? -1.0f : fwd[1] > 1.0f ? 1.0f : fwd[1]) * 57.29578f;
		auto it = g_spaces.find(pl->space);
		g_ovl.headSpace = it != g_spaces.end() ? (int)it->second.type : -1;
		if (it != g_spaces.end()) {
			g_ovl.spaceOrigin[0] = it->second.pose.position.x;
			g_ovl.spaceOrigin[1] = it->second.pose.position.y;
			g_ovl.spaceOrigin[2] = it->second.pose.position.z;
		}
		break;
	}
	bool fresh = false;
	{
		std::lock_guard<std::mutex> l(g_ovl.panelMutex);
		if (g_ovl.panelVersion != g_ovl.uploadedVersion && g_ovl.panel.size() == (size_t)QVD_OVERLAY_W * QVD_OVERLAY_H * 4) {
			g_ovl.upload.swap(g_ovl.panel); // the thread makes a new one each time
			g_ovl.uploadedVersion = g_ovl.panelVersion;
			fresh = true;
		}
	}
	if (fresh) {
		if (!g_ovl.swapchain && !g_ovl.failed) {
			const int64_t c0 = Qpc();
			OverlayCreateXr();
			if (g_cost.freq > 0)
				Log("overlay: headset panel set up in %.0f us (game thread, once)", (double)(Qpc() - c0) * 1e6 / g_cost.freq);
		}
		const int64_t t0 = Qpc();
		uint64_t parts[4] = {};
		if (g_ovl.swapchain)
			OverlayUpload(parts);
		if (g_cost.freq > 0 && !g_ovl.firstUploadLogged) { // first one of the session: logged on its own
			g_ovl.firstUploadLogged = true;
			Log("overlay: first panel upload %.0f us (acquire+wait %.0f, copy %.0f, flush/submit %.0f, release %.0f us; "
			    "game thread, once)",
			    (double)(Qpc() - t0) * 1e6 / g_cost.freq, parts[0] / 1000.0, parts[1] / 1000.0, parts[2] / 1000.0,
			    parts[3] / 1000.0);
		} else if (g_cost.freq > 0) {
			const uint64_t ns = (uint64_t)((double)(Qpc() - t0) * 1e9 / g_cost.freq);
			g_ovl.uploads++;
			g_ovl.uploadNs += ns;
			if (ns > g_ovl.uploadMaxNs) {
				g_ovl.uploadMaxNs = ns;
				for (int i = 0; i < 4; i++)
					g_ovl.partNs[i] = parts[i];
			}
		}
	}
	if (!g_ovl.haveImage || !info)
		return info;
	if (info->layerCount + 1 > g_ovl.maxLayers) {
		if (!g_ovl.loggedLimit) {
			Log("overlay: the game already uses %u of the runtime's %u layers - panel not added in the headset",
			    info->layerCount, g_ovl.maxLayers);
			g_ovl.loggedLimit = true;
		}
		return info;
	}
	if (!kPanelMovable) { // locked to the view: from the right view pose of this frame (see kEyeTanX)
		const XrPosef e = PanelInEye();
		const XrCompositionLayerProjection *pl = nullptr;
		for (uint32_t i = 0; i < info->layerCount && !pl; i++)
			if (info->layers[i] && info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
				pl = reinterpret_cast<const XrCompositionLayerProjection *>(info->layers[i]);
		if (pl && pl->viewCount >= 1 && pl->space != XR_NULL_HANDLE) {
			const XrPosef &v = pl->views[pl->viewCount > kPanelView ? kPanelView : 0].pose;
			const XrQuaternionf inv = {-v.orientation.x, -v.orientation.y, -v.orientation.z, v.orientation.w};
			const float ep[3] = {e.position.x, e.position.y, e.position.z};
			float rp[3];
			RotateByInverse(inv, ep, rp); // = rotate by the view's orientation
			g_ovl.quad.space = pl->space;
			g_ovl.quad.pose.position = {v.position.x + rp[0], v.position.y + rp[1], v.position.z + rp[2]};
			g_ovl.quad.pose.orientation = QMul(v.orientation, e.orientation);
		} else { // no projection layer this frame: the VIEW space (the head, not the eye)
			g_ovl.quad.space = g_ovl.localSpace;
			g_ovl.quad.pose = e;
		}
		g_ovl.layers.assign(info->layers, info->layers + info->layerCount);
		g_ovl.layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&g_ovl.quad));
		mod = *info;
		mod.layerCount = (uint32_t)g_ovl.layers.size();
		mod.layers = g_ovl.layers.data();
		return &mod;
	}
	if (gameSpace != XR_NULL_HANDLE && (!g_ovl.seatSet || g_ovl.reseat.exchange(false))) {
		const bool again = g_ovl.seatSet;
		g_ovl.seat[0] = g_ovl.head[0];
		g_ovl.seat[1] = g_ovl.head[1];
		g_ovl.seat[2] = g_ovl.head[2];
		g_ovl.seat[3] = g_ovl.head[3] * 3.14159265f / 180.0f;
		g_ovl.seatSet = true;
		Log("overlay seat%s: head at x %.2f y %.2f z %.2f m, facing yaw %.0f deg (pitch %.0f deg ignored) in the game's "
		    "space - the panel stands relative to it",
		    again ? " taken again (Ctrl+Shift+L)" : " (first show)", g_ovl.seat[0], g_ovl.seat[1], g_ovl.seat[2],
		    g_ovl.head[3].load(), g_ovl.head[4].load());
	}
	{ // where it stands now (the overlay thread moves it while unlocked): pitch about the panel's own x axis,
	  // all relative to the seat (its position and level facing)
		float p[4];
		GetPlace(p);
		const float h = p[3] * 3.14159265f / 360.0f;
		const float sx = sinf(h), cx = cosf(h);
		if (gameSpace != XR_NULL_HANDLE) {
			const float a = g_ovl.seat[3], ca = cosf(a), sa = sinf(a), sy = sinf(a * 0.5f), cy = cosf(a * 0.5f);
			g_ovl.quad.pose.position = {g_ovl.seat[0] + ca * p[0] + sa * p[2], g_ovl.seat[1] + p[1],
						    g_ovl.seat[2] - sa * p[0] + ca * p[2]};
			g_ovl.quad.pose.orientation = {cy * sx, cx * sy, -sy * sx, cy * cx}; // yaw(seat) * pitch(place)
			g_ovl.quad.space = gameSpace;
		} else { // no projection layer this frame: the runtime's LOCAL, no seat
			g_ovl.quad.pose.position = {p[0], p[1], p[2]};
			g_ovl.quad.pose.orientation = {sx, 0.0f, 0.0f, cx};
			g_ovl.quad.space = g_ovl.localSpace;
		}
	}
	g_ovl.layers.assign(info->layers, info->layers + info->layerCount);
	g_ovl.layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&g_ovl.quad));
	mod = *info;
	mod.layerCount = (uint32_t)g_ovl.layers.size();
	mod.layers = g_ovl.layers.data();
	return &mod;
}

// ---------------------------------------------------------------- hooks
XrResult XRAPI_CALL hook_xrDestroyInstance(XrInstance instance)
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		OverlayStopThread();
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
	OverlayStopThread(); // it writes the shared memory
	OverlayReleaseXr();
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
		g_d12Device = d3d12->device;
		g_d12Device->AddRef();
		g_d12Queue = d3d12->queue;
		g_d12Queue->AddRef();
		if (FAILED(hr) || !g_on12) {
			Log("xrCreateSession: D3D11On12CreateDevice failed hr=0x%08lX - picture capture off for this session", hr);
			ReleaseDevice();
			return r;
		}
	}
	g_session = *session;
	g_loggedFirstCopy = g_loggedMsaa = g_loggedFormat = g_loggedNoProjection = g_loggedPose = false;
	g_readerWasActive = false;
	g_copiedFrames = 0;
	g_pending.valid = false;
	g_eye = 99; // logged on the first copy: which eye OBS asks for
	g_handAtBegin = !g_on12 && next_BeginFrame != nullptr;
	g_loggedHandover = false;
	OpenShm();
	Log("xrCreateSession: %s session ready (app '%s', runtime '%s'); picture given to OBS at %s",
	    g_on12 ? "D3D12" : "D3D11", g_appName.c_str(), g_runtimeName.c_str(),
	    g_handAtBegin ? "the next xrBeginFrame (xrEndFrame only copies it into a spare)" : "xrEndFrame");
	OverlayReleaseXr();
	g_ovl.maxLayers = 16;
	if (next_GetSystemProperties) {
		XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
		if (XR_SUCCEEDED(next_GetSystemProperties(instance, createInfo->systemId, &sp)) &&
		    sp.graphicsProperties.maxLayerCount > 0)
			g_ovl.maxLayers = sp.graphicsProperties.maxLayerCount;
	}
	OverlayStartThread();
	return r;
}

XrResult XRAPI_CALL hook_xrDestroySession(XrSession session)
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (session == g_session) {
			Log("xrDestroySession: %llu frames copied", (unsigned long long)g_copiedFrames);
			OverlayStopThread();
			OverlayReleaseXr(); // before the session goes
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
		const int64_t a = Qpc();
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			auto it = g_swapchains.find(swapchain);
			if (it != g_swapchains.end())
				it->second.acquired.push_back(*index);
		}
		g_ovl.hookTicks += Qpc() - a;
	}
	return r;
}

XrResult XRAPI_CALL hook_xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *ri)
{
	XrResult r = next_ReleaseSwapchainImage(swapchain, ri);
	if (XR_SUCCEEDED(r)) {
		const int64_t a = Qpc();
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			auto it = g_swapchains.find(swapchain);
			if (it != g_swapchains.end() && !it->second.acquired.empty()) {
				// images are released in the order they were acquired
				it->second.lastReleased = (int)it->second.acquired.front();
				it->second.acquired.pop_front();
			}
		}
		g_ovl.hookTicks += Qpc() - a;
	}
	return r;
}

XrResult XRAPI_CALL hook_xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo *ci, XrSpace *space)
{
	XrResult r = next_CreateReferenceSpace(session, ci, space);
	if (XR_SUCCEEDED(r) && ci && space) {
		std::lock_guard<std::mutex> lock(g_mutex);
		g_spaces[*space] = SpaceInfo{ci->referenceSpaceType, ci->poseInReferenceSpace};
	}
	return r;
}

XrResult XRAPI_CALL hook_xrDestroySpace(XrSpace space)
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_spaces.erase(space);
	}
	return next_DestroySpace(space);
}

// Every 10 s while OBS is showing the picture: this layer's time on the game's thread.
void CostLog(int64_t now)
{
	CostStats &c = g_cost;
	if (!c.windowStart) { // OBS just started showing the picture: start a fresh window
		const double freq = c.freq;
		const int64_t last = c.lastEndFrame;
		c = CostStats();
		c.freq = freq;
		c.lastEndFrame = last;
		c.windowStart = now;
		return;
	}
	if ((double)(now - c.windowStart) / c.freq < 10.0)
		return;
	if (c.copies == 0) {
		Log("cost on the game thread (last %.0f s, %u game frames, 0 copied - OBS not showing the picture): game frame "
		    "interval avg %.2f ms; runtime + layers below avg %.2f ms",
		    (double)(now - c.windowStart) / c.freq, c.frames, c.frames ? c.intervalMs / c.frames : 0.0,
		    c.frames ? c.belowMs / c.frames : 0.0);
	} else if (!c.hookUs.empty()) {
		std::vector<double> v = c.hookUs;
		std::sort(v.begin(), v.end());
		double sum = 0;
		for (double x : v)
			sum += x;
		char spare[96] = "";
		if (c.spares)
			sprintf_s(spare, "copy into the spare %.0f us at xrEndFrame, ", c.spareUs / c.spares);
		Log("cost on the game thread (last %.0f s, %u game frames, %u copied, %u skipped - OBS held the picture): this "
		    "layer avg %.0f us, 99%% %.0f us, max %.0f us per copied frame (%slock wait %.0f us, AcquireSync %.0f us, "
		    "copy+pose %.0f us, flush %.0f us; given to OBS at %s); game frame interval avg %.2f ms; runtime + layers "
		    "below avg %.2f ms",
		    (double)(now - c.windowStart) / c.freq, c.frames, c.copies, c.busy, sum / v.size(),
		    v[(size_t)(v.size() * 0.99)], v.back(), spare, c.lockUs / c.copies, c.acquireUs / c.copies,
		    c.copyUs / c.copies, c.flushUs / c.copies, c.spares ? "xrBeginFrame" : "xrEndFrame",
		    c.frames ? c.intervalMs / c.frames : 0.0, c.frames ? c.belowMs / c.frames : 0.0);
	}
	const double freq = c.freq;
	const int64_t last = c.lastEndFrame;
	c = CostStats();
	c.freq = freq;
	c.windowStart = now;
	c.lastEndFrame = last;
}

XrResult XRAPI_CALL hook_xrEndFrame(XrSession session, const XrFrameEndInfo *info)
{
	if (g_cost.freq == 0) {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		g_cost.freq = (double)f.QuadPart;
	}
	const int64_t t0 = Qpc();
	uint32_t copiesBefore, sparesBefore;
	XrFrameEndInfo withOverlay;
	const XrFrameEndInfo *send = info;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_cost.lockedAt = Qpc();
		copiesBefore = g_cost.copies;
		sparesBefore = g_cost.spares;
		if (session == g_session && info) {
			CaptureLeftEye(info);
			send = OverlayFrame(info, withOverlay);
		}
	}
	const int64_t t1 = Qpc();
	const XrResult r = next_EndFrame(session, send);
	const int64_t t2 = Qpc();
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		CostStats &c = g_cost;
		if (c.copies != copiesBefore) {
			c.hookUs.push_back((double)(t1 - t0) * 1e6 / c.freq);
			c.lockUs += (double)(c.lockedAt - t0) * 1e6 / c.freq;
		}
		if (c.spares != sparesBefore && g_pending.valid) { // counted when xrBeginFrame gives it to OBS
			g_pending.endUs = (double)(t1 - t0) * 1e6 / c.freq;
			g_pending.lockUs = (double)(c.lockedAt - t0) * 1e6 / c.freq;
		}
		if (c.lastEndFrame)
			c.intervalMs += (double)(t0 - c.lastEndFrame) * 1e3 / c.freq;
		c.lastEndFrame = t0;
		c.belowMs += (double)(t2 - t1) * 1e3 / c.freq;
		c.frames++;
		if (session == g_session)
			CostLog(t2); // also while nothing is copied: the game's own frame interval as the baseline
	}
	if (session == g_session) { // Archan's time on the game thread for this frame (for the overlay)
		const int64_t ours = (t1 - t0) + (Qpc() - t2) + g_ovl.hookTicks.exchange(0);
		g_ovl.archSum += ours;
		if (ours > g_ovl.archMax)
			g_ovl.archMax = ours;
		g_ovl.archFrames++;
		g_ovl.archTotal += ours;
		g_ovl.archTotalFrames++;
		if (g_cost.copies != copiesBefore)
			g_ovl.captured++;
	}
	return r;
}

// D3D11: the picture of the last frame is given to OBS here (see g_pending), before the runtime's call.
XrResult XRAPI_CALL hook_xrBeginFrame(XrSession session, const XrFrameBeginInfo *info)
{
	const int64_t t0 = Qpc();
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (session == g_session && g_pending.valid) {
			if (HandOver(t0, Qpc()))
				g_ovl.captured++;
			g_ovl.hookTicks += Qpc() - t0; // counted in the overlay's "Archan" with the next xrEndFrame
		}
	}
	return next_BeginFrame(session, info);
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
	HOOK(xrBeginFrame)
	HOOK(xrCreateReferenceSpace)
	HOOK(xrDestroySpace)
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
	GET(BeginFrame);
	GET(CreateReferenceSpace);
	GET(DestroySpace);
	GET(EnumerateSwapchainFormats);
	GET(WaitSwapchainImage);
	GET(GetSystemProperties);
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

// tests only: 1 = give the picture to OBS at xrEndFrame (the old way), 0 = at the next xrBeginFrame (D3D11)
extern "C" __declspec(dllexport) void archan_vr_capture_test_hand_at_end(int on)
{
	g_testHandAtEnd = on != 0;
}

// tests only: show / hide the overlay as if the key was pressed; the last measured values
extern "C" __declspec(dllexport) void archan_vr_capture_test_overlay(int on)
{
	g_ovl.visible = on != 0;
	if (g_ovl.wake)
		SetEvent(g_ovl.wake);
}
// tests only: lock / unlock as if Ctrl+Shift+L was pressed; where the panel is, whether it is unlocked and
// how many key events the hook took
extern "C" __declspec(dllexport) void archan_vr_capture_test_overlay_unlock(int on)
{
	g_ovl.unlockRequest = on ? 1 : 0;
	if (g_ovl.wake)
		SetEvent(g_ovl.wake);
}
extern "C" __declspec(dllexport) void archan_vr_capture_test_overlay_place(float out[4], int *unlocked, uint64_t *swallowed)
{
	GetPlace(out);
	*unlocked = g_ovl.unlocked ? 1 : 0;
	*swallowed = g_mk.swallowed;
}
extern "C" __declspec(dllexport) void archan_vr_capture_test_overlay_stats(double out[11], uint64_t *version)
{
	std::lock_guard<std::mutex> l(g_ovl.panelMutex);
	out[0] = g_ovl.last.fps;
	out[1] = g_ovl.last.frameMs;
	out[2] = g_ovl.last.cpu;
	out[3] = g_ovl.last.thread;
	out[4] = g_ovl.last.gpu;
	out[5] = g_ovl.last.archMs;
	out[6] = g_ovl.last.archPct;
	out[7] = g_ovl.last.archMaxMs;
	out[8] = g_ovl.last.capturing ? 1.0 : 0.0;
	out[9] = (double)g_ovl.archTotal.load() * 1e3 / (g_cost.freq > 0 ? g_cost.freq : 1.0); // ms since the start
	out[10] = (double)g_ovl.archTotalFrames.load();
	*version = g_ovl.panelVersion;
}

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
	static bool cleaned = false; // game start: delete logs older than 48 h (user decision 2026-11-05)
	if (!cleaned) {
		cleaned = true;
		const QvdLogCleanup c = qvd_cleanup_logs_now();
		Log("log cleanup (older than %.0f h): %d files deleted%s, %d old lines removed from layer.log", QVD_LOG_MAX_AGE_HOURS,
		    c.filesDeleted, c.filesFailed ? " (some could not be deleted)" : "", c.linesRemoved);
	}
	return XR_SUCCESS;
}
