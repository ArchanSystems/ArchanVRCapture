// Archan VR Capture - OBS source plugin (picture only).
//
// Source "Archan VR Capture": shows the centre 16:9 crop of the game's left eye.
// Picture sources, in this order:
//  1. OpenXR games (VDXR, Pimax, SteamVR OpenXR...): the game's own left eye, delivered
//     by the XR_APILAYER_ARCHAN_vr_capture OpenXR layer through a D3D11 shared texture.
//  2. Otherwise, if SteamVR runs (OpenVR games): SteamVR's left eye mirror texture
//     (IVRCompositor::GetMirrorTextureD3D11), read as a background OpenVR app.
//     No mirror window is used.
// The crop is drawn to fill the OBS canvas.
// Recording resolution (source Properties): "HMD res" (automatic: canvas + output =
// the exact 16:9 crop of the left eye, 1:1 pixels) or 1920x1080. While the game
// picture is shown the plugin keeps OBS at that size (canvas + output of the
// active profile only; waits while recording/streaming).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>

#include <openvr.h> // OpenVR SDK v2.15.6 header; openvr_api.dll is loaded at runtime

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../common/qvd_shared.h"
#include "../common/log_cleanup.h"
#include "../common/log_zip.h"
#include "../common/overlay_key.h"
#include "stabilizer.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("archan-vr-capture", "en-US")

#define LOG(level, fmt, ...) blog(level, "[archan-vr-capture] " fmt, ##__VA_ARGS__)

namespace {

// Recording resolution modes (source setting "resolution")
#define MODE_HMD "hmd"         // automatic: left eye native 16:9 crop size (1:1 pixels)
#define MODE_1080 "1920x1080"  // fixed 1920x1080
#define MODE_1440 "2560x1440"  // fixed 2560x1440 (user request 2026-11-06)
// HMD res never asks for more than this width (16:9: 4096x2304). User decision 2026-11-06: NVENC H.264
// encodes at most 4096 wide (obs-nvenc-test on an RTX 3090: h264 max_width 4096, hevc 8192); a quad
// views picture of 5888x3312 failed to record ("Frame Dimension greater than the maximum").
#define HMD_MAX_WIDTH 4096u

// While OBS records, every new head pose of the shown picture is written to
// %LOCALAPPDATA%\ArchanVRCapture\logs\poses-YYYYMMDD-HHMMSS.csv (to measure real head motion).
struct PoseRecorder {
	FILE *f = nullptr;
	uint64_t lastSeq[3] = {0, 0, 0};
	uint64_t rows = 0;
	std::string path;
};

struct QvdSource {
	obs_source_t *source = nullptr;
	std::mutex shmLock; // render thread vs. properties (UI thread)
	std::atomic<uint32_t> eye{0}; // 0 left, 1 right (the source's "Eye" setting)

	HANDLE shmHandle = nullptr;
	QvdShared *shm = nullptr;
	uint64_t lastOpenTry = 0;
	uint64_t lastPidCheck = 0;

	uint32_t seenSeq = 0xFFFFFFFFu;
	uint32_t handle = 0;
	uint32_t width = 0, height = 0, dxgiFormat = 0, flags = 0, writerPid = 0;
	gs_texture_t *shared = nullptr; // opened from the game's shared handle
	gs_texture_t *copy = nullptr;   // our own copy, so the game is never blocked
	uint64_t lastFrameCount = 0;
	bool haveFrame = false;

	// quad views (foveated rendering): the sharp inner left view, combined into the wide view
	uint32_t innerHandle = 0, innerW = 0, innerH = 0, innerFmt = 0;
	gs_texture_t *innerShared = nullptr;
	gs_texture_t *innerCopy = nullptr;
	bool innerFrame = false;      // the last copied frame came with its inner view
	double innerTan[4] = {-1, 1, 1, -1};
	stab::Q innerQ = {0, 0, 0, 1};
	bool newLayerFrame = false;   // a new frame was copied this render
	gs_texrender_t *composite = nullptr;
	uint32_t compW = 0, compH = 0;
	gs_effect_t *compEffect = nullptr;
	bool compEffectFailed = false;
	gs_texture_t *layerPic = nullptr; // the layer picture to show: the composite, or the copy

	// performance overlay: the game's panel (layer, v4), drawn on top while the game shows it
	gs_texture_t *overlayTex = nullptr;
	uint32_t overlaySeen = 0xFFFFFFFFu;
	bool overlayShow = false;
	std::vector<uint8_t> overlayPx;

	volatile bool modeHmd = true; // set from settings (UI thread), read by render thread
	volatile uint32_t fixedW = 1920, fixedH = 1080; // the fixed size when not HMD res
	uint64_t lastSizeRequest = 0;

	// SteamVR mirror picture (used when no OpenXR layer picture is there)
	gs_texture_t *mirrorCopy = nullptr;
	uint64_t mirrorGen = ~0ull;
	bool mirrorFrame = false;
	uint32_t mirrorFlags = 0;

	// picture currently shown: 0 none, 1 OpenXR layer, 2 SteamVR mirror
	volatile int picKind = 0;
	volatile uint32_t picW = 0, picH = 0;

	// head pose of the picture (for stabilization), per source of the picture
	struct Pose {
		bool valid = false, upKnown = false;
		stab::Q q = {0, 0, 0, 1};
		stab::V3 up = {0, 1, 0};
		double tan[4] = {-1, 1, 1, -1}; // left right up down
		int64_t tNs = 0;
		uint64_t seq = 0; // +1 for every new frame's pose
	};
	Pose layerPose, mirrorPose;
	uint32_t mirrorFrameIndex = 0;

	// stabilization (look-ahead): the last 'delay' seconds of frames with their poses
	struct BufFrame {
		gs_texture_t *tex = nullptr;
		bool inUse = false;
		double t = 0; // seconds (the frame's pose time)
		Pose pose;
		uint32_t w = 0, h = 0, flags = 0;
		gs_color_format fmt = GS_UNKNOWN;
	};
	std::mutex stabLock; // settings + status: UI thread vs render thread
	stab::Settings stabSettings;
	bool stabDirty = true;
	char stabStatus[400] = "";
	stab::LookAhead look;      // render thread only
	std::vector<BufFrame> buf; // render thread only
	uint64_t bufSeq = 0;
	bool haveClock = false;
	double clockOffset = 0; // game frame clock - OBS clock (s), slowly filtered
	int bufKind = 0;
	uint32_t bufWriter = 0, bufW = 0, bufH = 0;
	gs_effect_t *stabEffect = nullptr;
	bool stabEffectFailed = false;
	bool stabLoggedNoPose = false;
	bool stabLoggedStart = false;
	PoseRecorder recorder; // render thread only
	std::atomic<bool> removed{false}; // removed from OBS (audio sync goes back)
	// live = in OBS's Program output (activate/deactivate; a hidden scene item is not live). User decision
	// 2026-11-06: not live -> audio sync goes back; no VR picture does not count.
	std::atomic<bool> live{false};
	std::atomic<int64_t> delayNs{0}; // video delay while stabilization is on, else 0
	// cost measurement (render thread): this source's CPU time per render, logged every 10 s
	struct Cost {
		uint64_t windowNs = 0;
		uint32_t renders = 0, plans = 0, totalFrames = 0, laggedFrames = 0;
		double sumMs = 0, maxMs = 0, planSumMs = 0, planMaxMs = 0;
	} cost;
	uint64_t audioCheckNs = 0;         // tick thread
};

// ---------------------------------------------------------------- SteamVR mirror (OpenVR games)
// One connection for the whole module, kept by a worker thread. The worker also
// watches for VREvent_Quit all the time (not only while the source renders): SteamVR
// kills apps that do not leave in time when it quits (openvr.h, AcknowledgeQuit_Exiting).
typedef uint32_t(VR_CALLTYPE *PFN_InitInternal2)(vr::EVRInitError *, vr::EVRApplicationType, const char *);
typedef void(VR_CALLTYPE *PFN_ShutdownInternal)();
typedef void *(VR_CALLTYPE *PFN_GetGenericInterface)(const char *, vr::EVRInitError *);
typedef const char *(VR_CALLTYPE *PFN_InitErrorText)(vr::EVRInitError);

struct MirrorState {
	std::mutex lock; // texture fields: worker vs. render thread (order: graphics, then this lock)
	ID3D11Texture2D *tex = nullptr; // SteamVR left eye mirror, opened on OBS's D3D11 device
	void *srv = nullptr;            // from GetMirrorTextureD3D11, given back with ReleaseMirrorTextureD3D11
	uint32_t w = 0, h = 0, samples = 0;
	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
	uint64_t gen = 0; // +1 whenever tex changes
};
MirrorState g_mirror;
// the eye the sources ask for (0 left, 1 right) and the eye of the mirror texture / geometry now
std::atomic<int> g_wantEye{0};
int g_mirrorEye = 0; // worker thread
vr::EVREye VrEye(int eye) { return eye == 1 ? vr::Eye_Right : vr::Eye_Left; }

// OpenVR interfaces for the render thread (frame pose of the mirror picture). Every OpenVR call
// is made under callLock. Lock order: graphics, then g_mirror.lock, then g_vr.callLock.
struct VrShared {
	std::mutex callLock;
	vr::IVRSystem *sys = nullptr;
	vr::IVRCompositor *comp = nullptr;
	stab::Q eyeToHead = {0, 0, 0, 1}; // left eye orientation in the HMD
	double tan[4] = {-1, 1, 1, -1};    // left eye FOV tangents: left right up down
};
VrShared g_vr;

stab::Q RotationOf(const vr::HmdMatrix34_t &m)
{
	return stab::FromColumns({m.m[0][0], m.m[1][0], m.m[2][0]}, {m.m[0][1], m.m[1][1], m.m[2][1]},
				 {m.m[0][2], m.m[1][2], m.m[2][2]});
}

// worker thread, under g_vr.callLock: what the mirror picture's pose needs besides the frame pose
void VrReadEyeGeometry()
{
	g_mirrorEye = g_wantEye;
	g_vr.eyeToHead = RotationOf(g_vr.sys->GetEyeToHeadTransform(VrEye(g_mirrorEye)));
	float l = -1, r = 1, t = -1, b = 1;
	g_vr.sys->GetProjectionRaw(VrEye(g_mirrorEye), &l, &r, &t, &b);
	// The header does not state the sign convention of top/bottom; the image's top row is 'top':
	// with y down (top < bottom) tan(up) = -top, with y up tan(up) = top.
	g_vr.tan[0] = l;
	g_vr.tan[1] = r;
	g_vr.tan[2] = (t < b) ? -t : t;
	g_vr.tan[3] = (t < b) ? -b : b;
	LOG(LOG_INFO, "SteamVR %s eye: projection raw l %.4f r %.4f t %.4f b %.4f -> tan up %.4f down %.4f",
	    g_mirrorEye ? "right" : "left", l, r, t, b, g_vr.tan[2], g_vr.tan[3]);
}
std::thread g_vrThread;
std::atomic<bool> g_vrStop{false};
HMODULE g_openvr = nullptr;
PFN_InitInternal2 vr_Init = nullptr;
PFN_ShutdownInternal vr_Shutdown = nullptr;
PFN_GetGenericInterface vr_GetInterface = nullptr;
PFN_InitErrorText vr_ErrorText = nullptr;

bool LoadOpenVR()
{
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   reinterpret_cast<LPCWSTR>(&LoadOpenVR), &self);
	wchar_t path[MAX_PATH] = {};
	GetModuleFileNameW(self, path, MAX_PATH);
	std::wstring dll = path;
	dll = dll.substr(0, dll.find_last_of(L'\\') + 1) + L"openvr_api.dll";
	g_openvr = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!g_openvr) {
		LOG(LOG_WARNING, "SteamVR mirror off: could not load %ls (error %lu)", dll.c_str(), GetLastError());
		return false;
	}
	vr_Init = (PFN_InitInternal2)GetProcAddress(g_openvr, "VR_InitInternal2");
	vr_Shutdown = (PFN_ShutdownInternal)GetProcAddress(g_openvr, "VR_ShutdownInternal");
	vr_GetInterface = (PFN_GetGenericInterface)GetProcAddress(g_openvr, "VR_GetGenericInterface");
	vr_ErrorText = (PFN_InitErrorText)GetProcAddress(g_openvr, "VR_GetVRInitErrorAsEnglishDescription");
	if (!vr_Init || !vr_Shutdown || !vr_GetInterface || !vr_ErrorText) {
		LOG(LOG_WARNING, "SteamVR mirror off: openvr_api.dll is missing functions");
		FreeLibrary(g_openvr);
		g_openvr = nullptr;
		return false;
	}
	return true;
}

bool GraphicsReady()
{
	struct obs_video_info ovi;
	return obs_get_video_info(&ovi);
}

// worker thread
void MirrorDrop(vr::IVRCompositor *comp)
{
	obs_enter_graphics();
	{
		std::lock_guard<std::mutex> l(g_mirror.lock);
		if (g_mirror.srv && comp) {
			std::lock_guard<std::mutex> c(g_vr.callLock);
			comp->ReleaseMirrorTextureD3D11(g_mirror.srv);
		}
		if (g_mirror.tex)
			g_mirror.tex->Release();
		g_mirror.srv = nullptr;
		g_mirror.tex = nullptr;
		g_mirror.w = g_mirror.h = g_mirror.samples = 0;
		g_mirror.fmt = DXGI_FORMAT_UNKNOWN;
		g_mirror.gen++;
	}
	obs_leave_graphics();
}

// worker thread; returns the compositor error (0 = got it)
int MirrorGet(vr::IVRCompositor *comp)
{
	if (!GraphicsReady())
		return -1;
	int result = -1;
	obs_enter_graphics();
	ID3D11Device *dev = static_cast<ID3D11Device *>(gs_get_device_obj());
	void *srv = nullptr;
	if (dev) {
		{
			std::lock_guard<std::mutex> c(g_vr.callLock);
			result = (int)comp->GetMirrorTextureD3D11(VrEye(g_mirrorEye), dev, &srv);
		}
		if (result == 0 && srv) {
			ID3D11Resource *res = nullptr;
			ID3D11Texture2D *t = nullptr;
			static_cast<ID3D11ShaderResourceView *>(srv)->GetResource(&res);
			if (res) {
				res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t);
				res->Release();
			}
			if (t) {
				D3D11_TEXTURE2D_DESC d;
				t->GetDesc(&d);
				std::lock_guard<std::mutex> l(g_mirror.lock);
				g_mirror.tex = t;
				g_mirror.srv = srv;
				g_mirror.w = d.Width;
				g_mirror.h = d.Height;
				g_mirror.samples = d.SampleDesc.Count;
				g_mirror.fmt = d.Format;
				g_mirror.gen++;
				LOG(LOG_INFO, "SteamVR mirror %s eye %ux%u dxgi format %d (mips %u, array %u, samples %u)",
				    g_mirrorEye ? "right" : "left", d.Width, d.Height, (int)d.Format, d.MipLevels, d.ArraySize, d.SampleDesc.Count);
			} else {
				std::lock_guard<std::mutex> c(g_vr.callLock);
				comp->ReleaseMirrorTextureD3D11(srv);
				result = -2;
			}
		}
	}
	obs_leave_graphics();
	return result;
}

void VrThread()
{
	vr::IVRSystem *sys = nullptr;
	vr::IVRCompositor *comp = nullptr;
	bool connected = false;
	uint64_t nextTry = 0, nextMirrorTry = 0;
	int lastInitErr = -999, lastMirrorErr = -999;

	while (!g_vrStop) {
		const uint64_t now = GetTickCount64();
		if (!connected && now >= nextTry) {
			nextTry = now + 3000;
			vr::EVRInitError e = vr::VRInitError_None;
			std::unique_lock<std::mutex> c(g_vr.callLock);
			vr_Init(&e, vr::VRApplication_Background, nullptr); // never starts SteamVR
			if (e == vr::VRInitError_None) {
				sys = static_cast<vr::IVRSystem *>(vr_GetInterface(vr::IVRSystem_Version, &e));
				if (e == vr::VRInitError_None)
					comp = static_cast<vr::IVRCompositor *>(vr_GetInterface(vr::IVRCompositor_Version, &e));
				if (!sys || !comp || e != vr::VRInitError_None) {
					LOG(LOG_WARNING, "SteamVR interfaces not available: %s", vr_ErrorText(e));
					vr_Shutdown();
					sys = nullptr;
					comp = nullptr;
				} else {
					connected = true;
					lastInitErr = -999;
					lastMirrorErr = -999;
					nextMirrorTry = 0;
					g_vr.sys = sys;
					g_vr.comp = comp;
					VrReadEyeGeometry();
					LOG(LOG_INFO, "connected to SteamVR (background app, %s / %s)", vr::IVRSystem_Version,
					    vr::IVRCompositor_Version);
				}
			} else if ((int)e != lastInitErr) {
				lastInitErr = (int)e;
				LOG(LOG_INFO, "SteamVR not connected: %s (%d) - retrying every 3 s", vr_ErrorText(e), (int)e);
			}
		}
		if (connected) {
			vr::VREvent_t ev;
			bool quit = false;
			{
				std::lock_guard<std::mutex> c(g_vr.callLock);
				while (sys->PollNextEvent(&ev, sizeof(ev)))
					if (ev.eventType == vr::VREvent_Quit)
						quit = true;
			}
			if (quit) {
				LOG(LOG_INFO, "SteamVR is quitting - disconnecting");
				MirrorDrop(comp);
				{
					std::lock_guard<std::mutex> c(g_vr.callLock);
					g_vr.sys = nullptr;
					g_vr.comp = nullptr;
					vr_Shutdown();
				}
				sys = nullptr;
				comp = nullptr;
				connected = false;
				nextTry = now + 5000;
				continue;
			}
			if (g_wantEye != g_mirrorEye) { // the source switched eyes: the other mirror texture and geometry
				LOG(LOG_INFO, "SteamVR mirror: switching to the %s eye", g_wantEye ? "right" : "left");
				MirrorDrop(comp);
				{
					std::lock_guard<std::mutex> c(g_vr.callLock);
					VrReadEyeGeometry();
				}
				nextMirrorTry = 0;
			}
			bool have;
			{
				std::lock_guard<std::mutex> l(g_mirror.lock);
				have = g_mirror.tex != nullptr;
			}
			if (!have && now >= nextMirrorTry) {
				nextMirrorTry = now + 1000;
				const int err = MirrorGet(comp);
				if (err != 0 && err != lastMirrorErr)
					LOG(LOG_INFO, "SteamVR mirror texture not available yet (error %d) - retrying", err);
				lastMirrorErr = err;
			}
		}
		Sleep(100);
	}
	if (connected) {
		MirrorDrop(comp);
		{
			std::lock_guard<std::mutex> c(g_vr.callLock);
			g_vr.sys = nullptr;
			g_vr.comp = nullptr;
			vr_Shutdown();
		}
		LOG(LOG_INFO, "disconnected from SteamVR");
	}
}

gs_color_format GsFormatFor(DXGI_FORMAT f, uint32_t *flags)
{
	*flags = 0;
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		*flags = QVD_FLAG_SRGB_FORMAT;
		break;
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		*flags = QVD_FLAG_LINEAR_FLOAT;
		break;
	default:
		break;
	}
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		return GS_RGBA;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return GS_BGRA;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		return GS_BGRX;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return GS_R10G10B10A2;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return GS_RGBA16F;
	default:
		return GS_UNKNOWN;
	}
}

// Render thread (inside the graphics context): copy SteamVR's mirror into our own texture.
// Returns true if s->mirrorCopy holds a picture.
bool CopyMirror(QvdSource *s)
{
	std::lock_guard<std::mutex> l(g_mirror.lock);
	if (g_mirror.gen != s->mirrorGen) {
		s->mirrorGen = g_mirror.gen;
		if (s->mirrorCopy) {
			gs_texture_destroy(s->mirrorCopy);
			s->mirrorCopy = nullptr;
		}
		s->mirrorFrame = false;
		if (g_mirror.tex) {
			const gs_color_format gf = GsFormatFor(g_mirror.fmt, &s->mirrorFlags);
			if (gf == GS_UNKNOWN || g_mirror.samples != 1)
				LOG(LOG_WARNING, "SteamVR mirror format %d / %u samples not supported", (int)g_mirror.fmt,
				    g_mirror.samples);
			else
				s->mirrorCopy = gs_texture_create(g_mirror.w, g_mirror.h, gf, 1, nullptr, 0);
		}
	}
	if (!g_mirror.tex || !s->mirrorCopy)
		return false;
	ID3D11Texture2D *dst = static_cast<ID3D11Texture2D *>(gs_texture_get_obj(s->mirrorCopy));
	ID3D11Device *dev = static_cast<ID3D11Device *>(gs_get_device_obj());
	if (!dst || !dev)
		return false;
	ID3D11DeviceContext *ctx = nullptr;
	dev->GetImmediateContext(&ctx);
	ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, g_mirror.tex, 0, nullptr);
	ctx->Release();
	s->mirrorFrame = true;

	// Pose of that picture: the HMD pose SteamVR says the app rendered the latest frame with
	// (Compositor_FrameTiming::m_HmdPose) times the left eye's offset. Which frame the mirror
	// texture shows at this moment is not documented: NOT PROVEN to match exactly.
	std::lock_guard<std::mutex> c(g_vr.callLock);
	if (g_vr.comp) {
		vr::Compositor_FrameTiming ft = {};
		ft.m_nSize = sizeof(ft);
		if (g_vr.comp->GetFrameTiming(&ft, 0) && ft.m_HmdPose.bPoseIsValid) {
			if (!s->mirrorPose.valid || ft.m_nFrameIndex != s->mirrorFrameIndex) {
				s->mirrorFrameIndex = ft.m_nFrameIndex;
				QvdSource::Pose &p = s->mirrorPose;
				p.q = stab::Mul(RotationOf(ft.m_HmdPose.mDeviceToAbsoluteTracking), g_vr.eyeToHead);
				p.up = {0, 1, 0}; // SteamVR tracking universes are y-up
				p.upKnown = true;
				for (int i = 0; i < 4; i++)
					p.tan[i] = g_vr.tan[i];
				p.tNs = (int64_t)(ft.m_flSystemTimeInSeconds * 1e9);
				p.valid = true;
				p.seq++;
			}
		}
	}
	return true;
}

void CloseTextures(QvdSource *s)
{
	if (s->shared) {
		gs_texture_destroy(s->shared);
		s->shared = nullptr;
	}
	if (s->copy) {
		gs_texture_destroy(s->copy);
		s->copy = nullptr;
	}
	if (s->innerShared) {
		gs_texture_destroy(s->innerShared);
		s->innerShared = nullptr;
	}
	if (s->innerCopy) {
		gs_texture_destroy(s->innerCopy);
		s->innerCopy = nullptr;
	}
	if (s->composite) {
		gs_texrender_destroy(s->composite);
		s->composite = nullptr;
	}
	s->compW = s->compH = 0;
	s->innerFrame = false;
	s->layerPic = nullptr;
	s->haveFrame = false;
	s->handle = 0;
	s->innerHandle = 0;
}

void CloseShm(QvdSource *s)
{
	std::lock_guard<std::mutex> lock(s->shmLock);
	if (s->shm) {
		UnmapViewOfFile(s->shm);
		s->shm = nullptr;
	}
	if (s->shmHandle) {
		CloseHandle(s->shmHandle);
		s->shmHandle = nullptr;
	}
	s->seenSeq = 0xFFFFFFFFu;
	s->overlaySeen = 0xFFFFFFFFu;
	s->overlayShow = false;
}

bool TryOpenShm(QvdSource *s, uint64_t now)
{
	if (s->shm)
		return true;
	if (now - s->lastOpenTry < 1000)
		return false;
	s->lastOpenTry = now;
	HANDLE h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, qvd_channel_name());
	if (!h)
		return false;
	QvdShared *m = static_cast<QvdShared *>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(QvdShared)));
	if (!m || m->magic != QVD_MAGIC || m->version != QVD_VERSION) {
		if (m)
			UnmapViewOfFile(m);
		CloseHandle(h);
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(s->shmLock);
		s->shmHandle = h;
		s->shm = m;
	}
	
	LOG(LOG_INFO, "connected to game layer (app '%s', runtime '%s')", s->shm->appName, s->shm->runtimeName);
	return true;
}

bool WriterAlive(uint32_t pid)
{
	if (!pid)
		return false;
	HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
	if (!p)
		return GetLastError() == ERROR_ACCESS_DENIED; // exists but protected
	const bool alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
	CloseHandle(p);
	return alive;
}

// Read the descriptor with the seqlock. Returns true if it changed.
bool ReadDescriptor(QvdSource *s)
{
	for (int tries = 0; tries < 16; tries++) {
		const uint32_t a = s->shm->seq;
		if (a & 1u) {
			YieldProcessor();
			continue;
		}
		MemoryBarrier();
		const uint32_t handle = s->shm->sharedHandle;
		const uint32_t w = s->shm->width, h = s->shm->height;
		const uint32_t fmt = s->shm->dxgiFormat, flags = s->shm->flags, pid = s->shm->writerPid;
		const uint32_t ih = s->shm->innerHandle, iw = s->shm->innerWidth, ihh = s->shm->innerHeight,
			       ifmt = s->shm->innerDxgiFormat;
		MemoryBarrier();
		if (s->shm->seq != a)
			continue;
		if (a == s->seenSeq)
			return false;
		s->seenSeq = a;
		CloseTextures(s);
		s->handle = handle;
		s->width = w;
		s->height = h;
		s->dxgiFormat = fmt;
		s->flags = flags;
		s->writerPid = pid;
		s->innerHandle = ih;
		s->innerW = iw;
		s->innerH = ihh;
		s->innerFmt = ifmt;
		return true;
	}
	return false;
}

// The game's performance overlay panel (user decision 2026-11-06: in the headset and in the recording).
// Shown while the game has it on and keeps updating it (once a second).
void PollOverlay(QvdSource *s, uint64_t now)
{
	QvdShared *m = s->shm;
	s->overlayShow = false;
	if (!m || !m->overlayVisible || !s->shared)
		return;
	const uint64_t tick = m->overlayTickMs;
	if (tick == 0 || (now > tick && now - tick >= QVD_OVERLAY_TIMEOUT_MS))
		return;
	const uint32_t a = m->overlaySeq;
	if (!(a & 1u) && a != s->overlaySeen) {
		const size_t bytes = (size_t)QVD_OVERLAY_W * QVD_OVERLAY_H * 4;
		s->overlayPx.resize(bytes);
		MemoryBarrier();
		memcpy(s->overlayPx.data(), m->overlayPixels, bytes);
		MemoryBarrier();
		if (m->overlaySeq == a) {
			if (!s->overlayTex)
				s->overlayTex = gs_texture_create(QVD_OVERLAY_W, QVD_OVERLAY_H, GS_RGBA, 1, nullptr, GS_DYNAMIC);
			if (s->overlayTex)
				gs_texture_set_image(s->overlayTex, s->overlayPx.data(), QVD_OVERLAY_W * 4, false);
			if (s->overlaySeen == 0xFFFFFFFFu)
				LOG(LOG_INFO, "performance overlay: the game shows it - drawn at the top left of the video");
			s->overlaySeen = a;
		}
	}
	s->overlayShow = s->overlayTex != nullptr && s->overlaySeen != 0xFFFFFFFFu;
}

void PollLayer(QvdSource *s)
{
	const uint64_t now = GetTickCount64();
	if (!TryOpenShm(s, now))
		return;

	s->shm->wantEye = s->eye;   // v5: the eye this source shows
	s->shm->readerTickMs = now; // tells the layer that OBS is showing the picture

	if (ReadDescriptor(s) && s->handle) {
		s->shared = gs_texture_open_shared(s->handle);
		if (s->shared) {
			s->copy = gs_texture_create(gs_texture_get_width(s->shared), gs_texture_get_height(s->shared),
						    gs_texture_get_color_format(s->shared), 1, nullptr, 0);
			LOG(LOG_INFO, "left eye picture %ux%u dxgi format %u flags %u", s->width, s->height,
			    s->dxgiFormat, s->flags);
			if (s->innerHandle) {
				s->innerShared = gs_texture_open_shared(s->innerHandle);
				if (s->innerShared) {
					s->innerCopy = gs_texture_create(gs_texture_get_width(s->innerShared),
									 gs_texture_get_height(s->innerShared),
									 gs_texture_get_color_format(s->innerShared), 1, nullptr, 0);
					LOG(LOG_INFO, "quad views: inner left view %ux%u dxgi format %u - combined into the wide view",
					    s->innerW, s->innerH, s->innerFmt);
				} else {
					LOG(LOG_WARNING, "could not open the inner view's shared texture 0x%08X", s->innerHandle);
				}
			}
		} else {
			LOG(LOG_WARNING, "could not open shared texture 0x%08X", s->handle);
		}
		s->lastFrameCount = 0;
	}

	if (now - s->lastPidCheck >= 1000) {
		s->lastPidCheck = now;
		if (s->shared && !WriterAlive(s->writerPid)) {
			LOG(LOG_INFO, "game process %u ended", s->writerPid);
			CloseTextures(s);
			CloseShm(s);
			return;
		}
	}

	if (s->shared && s->copy) {
		const uint64_t fc = s->shm->frameCount;
		if (fc != s->lastFrameCount) {
			if (gs_texture_acquire_sync(s->shared, 0, 0) == 0) {
				gs_copy_texture(s->copy, s->shared);
				// the layer writes the pose while it holds this mutex: it belongs to the copied picture
				QvdShared *m = s->shm;
				QvdSource::Pose &p = s->layerPose;
				p.valid = (m->poseFlags & QVD_POSE_VALID) != 0;
				if (p.valid) {
					p.q = {m->poseOrient[0], m->poseOrient[1], m->poseOrient[2], m->poseOrient[3]};
					p.upKnown = (m->poseFlags & QVD_POSE_UP) != 0;
					p.up = {m->up[0], m->up[1], m->up[2]};
					for (int i = 0; i < 4; i++)
						p.tan[i] = m->fovTan[i];
					p.tNs = m->displayTimeNs;
					p.seq++;
				}
				// the inner view (quad views): taken after the main one, as the layer does
				s->innerFrame = false;
				if (s->innerShared && s->innerCopy && (m->poseFlags & QVD_POSE_INNER) &&
				    gs_texture_acquire_sync(s->innerShared, 0, 0) == 0) {
					gs_copy_texture(s->innerCopy, s->innerShared);
					for (int i = 0; i < 4; i++)
						s->innerTan[i] = m->innerFovTan[i];
					s->innerQ = {m->innerPoseOrient[0], m->innerPoseOrient[1], m->innerPoseOrient[2],
						     m->innerPoseOrient[3]};
					s->innerFrame = true;
					gs_texture_release_sync(s->innerShared, 0);
				}
				gs_texture_release_sync(s->shared, 0);
				s->lastFrameCount = fc;
				s->haveFrame = true;
				s->newLayerFrame = true;
			}
		}
	}
	PollOverlay(s, now);
}

// ---------------------------------------------------------------- quad views: combine like the headset
// User decision 2026-11-06: with quad views (foveated rendering) the recording is the wide view scaled
// up with the sharp inner view on top where it belongs, as the headset shows it (eye-tracked: the
// sharp area moves with the eyes). For each output pixel: its ray in the wide view, turned into the
// inner view's orientation; inside the inner view -> the inner picture (blended over a thin edge band),
// else the wide picture. The result covers exactly the wide view's FOV, so everything after this
// (16:9 crop, HMD res, stabilization) works on it unchanged.
const char *kCompEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform texture2d inner;
uniform float4 mainTan;
uniform float4 innerTan;
uniform float4 r0;
uniform float4 r1;
uniform float4 r2;
uniform float feather;

sampler_state linearSampler {
	Filter   = Linear;
	AddressU = Clamp;
	AddressV = Clamp;
};

struct VertData {
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
};

VertData VSDefault(VertData v_in)
{
	VertData vert_out;
	vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
	vert_out.uv  = v_in.uv;
	return vert_out;
}

float4 PSCompose(VertData v_in) : TARGET
{
	float3 d = float3(mainTan.x + v_in.uv.x * (mainTan.y - mainTan.x),
	                  mainTan.z - v_in.uv.y * (mainTan.z - mainTan.w), -1.0);
	float3 di = float3(dot(r0.xyz, d), dot(r1.xyz, d), dot(r2.xyz, d));
	float2 ui = float2(0.0, 0.0);
	float w = 0.0;
	if (di.z < -0.000001) {
		float xi = di.x / -di.z;
		float yi = di.y / -di.z;
		ui = float2((xi - innerTan.x) / (innerTan.y - innerTan.x), (innerTan.z - yi) / (innerTan.z - innerTan.w));
		float edge = min(min(ui.x, 1.0 - ui.x), min(ui.y, 1.0 - ui.y));
		w = saturate(edge / feather);
	}
	float4 c = image.Sample(linearSampler, v_in.uv);
	float4 c2 = inner.Sample(linearSampler, saturate(ui));
	return lerp(c, c2, w);
}

technique Draw
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSCompose(v_in);
	}
}
)";

// Draws the combined picture (graphics context). false = show the wide view alone.
bool Compose(QvdSource *s)
{
	if (!s->compEffect && !s->compEffectFailed) {
		char *err = nullptr;
		s->compEffect = gs_effect_create(kCompEffect, "archan_vr_capture_compose.effect", &err);
		if (!s->compEffect) {
			LOG(LOG_WARNING, "quad views shader failed: %s - the wide view is shown alone", err ? err : "?");
			s->compEffectFailed = true;
		}
		bfree(err);
	}
	if (!s->compEffect || !s->copy || !s->innerCopy)
		return false;
	const QvdSource::Pose &mp = s->layerPose;
	const double *mt = mp.tan, *it = s->innerTan;
	if (!(mt[1] > mt[0] && mt[2] > mt[3] && it[1] > it[0] && it[2] > it[3]))
		return false;
	// size: the inner view's sharpness over the whole wide FOV (fixed while the image sizes stay)
	if (s->compW == 0) {
		double cw = (double)s->innerW * (mt[1] - mt[0]) / (it[1] - it[0]);
		double ch = (double)s->innerH * (mt[2] - mt[3]) / (it[2] - it[3]);
		const double sc = std::min(1.0, 8192.0 / std::max(cw, ch));
		cw *= sc;
		ch *= sc;
		s->compW = std::max(s->width, (uint32_t)std::lround(cw) & ~1u);
		s->compH = std::max(s->height, (uint32_t)std::lround(ch) & ~1u);
		// no larger than HMD res can use: its 16:9 crop at most HMD_MAX_WIDTH wide (less GPU memory)
		for (int i = 0; i < 4; i++) {
			const QvdRect c = qvd_center_crop_16x9(s->compW, s->compH);
			if (c.w <= HMD_MAX_WIDTH)
				break;
			const double k = (double)HMD_MAX_WIDTH / c.w;
			s->compW = (uint32_t)std::lround(s->compW * k) & ~1u;
			s->compH = (uint32_t)std::lround(s->compH * k) & ~1u;
		}
		LOG(LOG_INFO,
		    "quad views: combined picture %ux%u (wide view %ux%u, inner view %ux%u over FOV tan L %.3f R %.3f U %.3f "
		    "D %.3f of L %.3f R %.3f U %.3f D %.3f)",
		    s->compW, s->compH, s->width, s->height, s->innerW, s->innerH, it[0], it[1], it[2], it[3], mt[0], mt[1],
		    mt[2], mt[3]);
	}
	if (!s->composite)
		s->composite = gs_texrender_create(gs_texture_get_color_format(s->copy), GS_ZS_NONE);
	if (!s->composite)
		return false;
	// wide view -> inner view directions: R_inner^-1 * R_main
	const stab::Mat3 M = stab::MulM(stab::Transpose(stab::RotM(stab::NormalizeQ(s->innerQ))),
					stab::RotM(stab::NormalizeQ(mp.q)));
	gs_texrender_reset(s->composite);
	if (!gs_texrender_begin(s->composite, s->compW, s->compH))
		return false;
	gs_ortho(0.0f, (float)s->compW, 0.0f, (float)s->compH, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_enable_blending(false);
	const bool prevSrgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false); // bytes as they are (the picture is shown later as before)
	gs_effect_t *e = s->compEffect;
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), s->copy);
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "inner"), s->innerCopy);
	struct vec4 v;
	vec4_set(&v, (float)mt[0], (float)mt[1], (float)mt[2], (float)mt[3]);
	gs_effect_set_vec4(gs_effect_get_param_by_name(e, "mainTan"), &v);
	vec4_set(&v, (float)it[0], (float)it[1], (float)it[2], (float)it[3]);
	gs_effect_set_vec4(gs_effect_get_param_by_name(e, "innerTan"), &v);
	const char *rows[3] = {"r0", "r1", "r2"};
	for (int i = 0; i < 3; i++) {
		vec4_set(&v, (float)M.m[i][0], (float)M.m[i][1], (float)M.m[i][2], 0.0f);
		gs_effect_set_vec4(gs_effect_get_param_by_name(e, rows[i]), &v);
	}
	gs_effect_set_float(gs_effect_get_param_by_name(e, "feather"), 0.02f);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(s->copy, 0, s->compW, s->compH);
	gs_enable_framebuffer_srgb(prevSrgb);
	gs_blend_state_pop();
	gs_texrender_end(s->composite);
	return true;
}

// Picks the picture: the OpenXR layer (game frames) first, else SteamVR's mirror.
void Poll(QvdSource *s)
{
	s->newLayerFrame = false;
	PollLayer(s);
	int kind = 0;
	uint32_t w = 0, h = 0;
	if (s->shared && s->copy && s->haveFrame) {
		kind = 1;
		if (s->newLayerFrame || !s->layerPic) {
			const bool composed = s->innerFrame && Compose(s);
			s->layerPic = composed ? gs_texrender_get_texture(s->composite) : s->copy;
		}
		if (s->layerPic != s->copy && s->layerPic) {
			w = s->compW;
			h = s->compH;
		} else {
			w = s->width;
			h = s->height;
		}
	} else if (CopyMirror(s)) {
		kind = 2;
		w = gs_texture_get_width(s->mirrorCopy);
		h = gs_texture_get_height(s->mirrorCopy);
	}
	if (kind != s->picKind || w != s->picW || h != s->picH) {
		if (kind == 1)
			LOG(LOG_INFO, "picture: OpenXR layer, app '%s', left eye %ux%u", s->shm ? s->shm->appName : "?", w, h);
		else if (kind == 2)
			LOG(LOG_INFO, "picture: SteamVR mirror, left eye %ux%u", w, h);
		else
			LOG(LOG_INFO, "picture: none (waiting for a VR game)");
	}
	s->picKind = kind;
	s->picW = w;
	s->picH = h;
}

void KeepVideoSize(QvdSource *s);

// ---------------------------------------------------------------- audio sync (user decision 2026-11-05)
// While a VR source has stabilization on, every audio source's Sync Offset = its own offset + the
// video delay (the largest delay of the VR sources that are on and live in Program). Stabilization off,
// the VR source not live (2026-11-06) or removed: every audio source gets its own offset back. Own offset = what is shown minus the delay
// the plugin added, so an offset the user types meanwhile is kept as theirs (+ the delay).
// OBS saves the scene collection at exit while the delay is still applied, so the own offsets are
// saved with it (module data) and taken back on load, only where the saved Sync Offset is still
// exactly the one the plugin set.
#define AUDIO_SAVE_KEY "archan_vr_capture_audio_sync"
struct AudioSync {
	std::mutex lock;                      // want + queued: any thread
	std::map<const void *, int64_t> want; // per VR source: delay ns (only sources that are on)
	bool queued = false;
	// UI thread only:
	struct Managed {
		int64_t own = 0, applied = 0;
		std::string name;
	};
	std::map<std::string, Managed> managed; // by source uuid
	bool ready = false;                     // scene collection loaded (no frontend: from module load)
};
AudioSync g_audio;

long long Ms(int64_t ns)
{
	return std::llround((double)ns / 1e6);
}

void AudioApplyTask(void *)
{
	int64_t d = 0;
	{
		std::lock_guard<std::mutex> l(g_audio.lock);
		g_audio.queued = false;
		for (const auto &w : g_audio.want)
			d = std::max(d, w.second);
	}
	if (!g_audio.ready)
		return;
	struct Ctx {
		int64_t d;
		std::set<std::string> seen;
	} ctx{d, {}};
	obs_enum_sources(
		[](void *p, obs_source_t *src) {
			Ctx &c = *static_cast<Ctx *>(p);
			if (!(obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) || obs_source_removed(src))
				return true;
			const char *u = obs_source_get_uuid(src);
			if (!u)
				return true;
			const std::string uuid = u;
			const char *nm = obs_source_get_name(src);
			const int64_t cur = obs_source_get_sync_offset(src);
			auto it = g_audio.managed.find(uuid);
			const int64_t own = it == g_audio.managed.end() ? cur : cur - (it->second.applied - it->second.own);
			if (c.d > 0) {
				c.seen.insert(uuid);
				const int64_t target = own + c.d;
				if (cur != target) {
					obs_source_set_sync_offset(src, target);
					LOG(LOG_INFO, "audio sync: '%s' %lld ms -> %lld ms (own %lld ms + video delay %lld ms)",
					    nm ? nm : "", Ms(cur), Ms(obs_source_get_sync_offset(src)), Ms(own), Ms(c.d));
				}
				AudioSync::Managed &m = g_audio.managed[uuid];
				m.own = own;
				m.applied = obs_source_get_sync_offset(src);
				m.name = nm ? nm : "";
			} else if (it != g_audio.managed.end()) {
				obs_source_set_sync_offset(src, own);
				LOG(LOG_INFO, "audio sync: '%s' %lld ms -> %lld ms (own offset back)", nm ? nm : "", Ms(cur),
				    Ms(obs_source_get_sync_offset(src)));
				g_audio.managed.erase(it);
			}
			return true;
		},
		&ctx);
	if (d > 0) {
		for (auto it = g_audio.managed.begin(); it != g_audio.managed.end();)
			it = ctx.seen.count(it->first) ? std::next(it) : g_audio.managed.erase(it); // source is gone
	} else {
		g_audio.managed.clear();
	}
}

void AudioQueue()
{
	{
		std::lock_guard<std::mutex> l(g_audio.lock);
		if (g_audio.queued)
			return;
		g_audio.queued = true;
	}
	obs_queue_task(OBS_TASK_UI, AudioApplyTask, nullptr, false);
}

// key = the VR source; ns = its video delay, 0 = stabilization off / source removed
void AudioWant(const void *key, int64_t ns)
{
	{
		std::lock_guard<std::mutex> l(g_audio.lock);
		if (ns > 0)
			g_audio.want[key] = ns;
		else
			g_audio.want.erase(key);
	}
	AudioQueue();
}

// this VR source's want: its delay only while stabilization is on, it is live and not removed
void AudioRefresh(QvdSource *s)
{
	AudioWant(s, s->live && !s->removed ? s->delayNs.load() : 0);
}

// Scene collection save / load (UI thread). 'data' = this collection's module data.
void AudioSaveLoad(obs_data_t *data, bool saving, void *)
{
	if (saving) {
		obs_data_array_t *arr = obs_data_array_create();
		for (const auto &m : g_audio.managed) {
			obs_data_t *o = obs_data_create();
			obs_data_set_string(o, "uuid", m.first.c_str());
			obs_data_set_string(o, "name", m.second.name.c_str());
			obs_data_set_int(o, "own", m.second.own);
			obs_data_set_int(o, "applied", m.second.applied);
			obs_data_array_push_back(arr, o);
			obs_data_release(o);
		}
		obs_data_set_array(data, AUDIO_SAVE_KEY, arr);
		obs_data_array_release(arr);
		return;
	}
	g_audio.managed.clear();
	obs_data_array_t *arr = obs_data_get_array(data, AUDIO_SAVE_KEY);
	const size_t n = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *o = obs_data_array_item(arr, i);
		const char *uuid = obs_data_get_string(o, "uuid");
		const int64_t own = obs_data_get_int(o, "own"), applied = obs_data_get_int(o, "applied");
		obs_source_t *src = uuid && *uuid ? obs_get_source_by_uuid(uuid) : nullptr;
		if (src) {
			const int64_t cur = obs_source_get_sync_offset(src);
			if (cur == applied) {
				AudioSync::Managed &m = g_audio.managed[uuid];
				m.own = own;
				m.applied = applied;
				m.name = obs_source_get_name(src) ? obs_source_get_name(src) : "";
				LOG(LOG_INFO, "audio sync: '%s' was saved with %lld ms = own %lld ms + video delay", m.name.c_str(),
				    Ms(cur), Ms(own));
			} else {
				LOG(LOG_INFO, "audio sync: '%s' is %lld ms, not the %lld ms the plugin set - kept as the own offset",
				    obs_source_get_name(src) ? obs_source_get_name(src) : "", Ms(cur), Ms(applied));
			}
			obs_source_release(src);
		}
		obs_data_release(o);
	}
	obs_data_array_release(arr);
	AudioQueue();
}

void AudioFrontendEvent(enum obs_frontend_event e)
{
	switch (e) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		g_audio.ready = true;
		AudioQueue();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_EXIT:
		g_audio.ready = false; // the collection was saved with the own offsets recorded
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		g_audio.ready = false;
		g_audio.managed.clear(); // its sources are gone
		break;
	default: break;
	}
}

// ---------------------------------------------------------------- stabilization
// Recording/streaming state from frontend events (cheap to read from the render thread).
std::atomic<bool> g_recActive{false}, g_streamActive{false}, g_replayActive{false}, g_vcamActive{false};
bool OutputsRunning()
{
	return g_recActive || g_streamActive || g_replayActive || g_vcamActive;
}
void OnFrontendEvent(enum obs_frontend_event e, void *)
{
	switch (e) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTED: g_recActive = true; break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED: g_recActive = false; break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED: g_streamActive = true; break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED: g_streamActive = false; break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED: g_replayActive = true; break;
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED: g_replayActive = false; break;
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED: g_vcamActive = true; break;
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED: g_vcamActive = false; break;
	default: break;
	}
	AudioFrontendEvent(e);
}

void PoseRecord(QvdSource *s, PoseRecorder &r)
{
	if (!g_recActive) {
		if (r.f) {
			fclose(r.f);
			r.f = nullptr;
			LOG(LOG_INFO, "head motion recorder: %llu poses written to %s", (unsigned long long)r.rows, r.path.c_str());
		}
		return;
	}
	if (s->picKind == 0)
		return;
	const QvdSource::Pose &p = s->picKind == 1 ? s->layerPose : s->mirrorPose;
	if (!p.valid || p.seq == r.lastSeq[s->picKind])
		return;
	r.lastSeq[s->picKind] = p.seq;
	if (!r.f) {
		char dir[MAX_PATH] = {};
		if (!GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH))
			return;
		std::string d = std::string(dir) + "\\ArchanVRCapture";
		CreateDirectoryA(d.c_str(), nullptr);
		d += "\\logs";
		CreateDirectoryA(d.c_str(), nullptr);
		SYSTEMTIME t;
		GetLocalTime(&t);
		char name[64];
		snprintf(name, sizeof(name), "\\poses-%04d%02d%02d-%02d%02d%02d.csv", t.wYear, t.wMonth, t.wDay, t.wHour,
			 t.wMinute, t.wSecond);
		r.path = d + name;
		r.f = fopen(r.path.c_str(), "w");
		r.rows = 0;
		if (!r.f) {
			LOG(LOG_WARNING, "head motion recorder: cannot write %s", r.path.c_str());
			return;
		}
		fprintf(r.f, "obs_time_ns,pose_time_ns,source,qx,qy,qz,qw,tan_left,tan_right,tan_up,tan_down,up_x,up_y,"
			     "up_z,up_known,eye_w,eye_h\n");
		LOG(LOG_INFO, "head motion recorder: writing %s", r.path.c_str());
	}
	fprintf(r.f, "%llu,%lld,%s,%.9f,%.9f,%.9f,%.9f,%.7f,%.7f,%.7f,%.7f,%.6f,%.6f,%.6f,%d,%u,%u\n",
		(unsigned long long)os_gettime_ns(), (long long)p.tNs, s->picKind == 1 ? "openxr" : "steamvr", p.q.x, p.q.y,
		p.q.z, p.q.w, p.tan[0], p.tan[1], p.tan[2], p.tan[3], p.up.x, p.up.y, p.up.z, p.upKnown ? 1 : 0, s->picW,
		s->picH);
	r.rows++;
}


// Output pixel (u,v) -> eye image texture coordinates through the homography rows h0..h2.
const char *kStabEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform float4 h0;
uniform float4 h1;
uniform float4 h2;

sampler_state linearSampler {
	Filter   = Linear;
	AddressU = Clamp;
	AddressV = Clamp;
};

struct VertData {
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
};

VertData VSDefault(VertData v_in)
{
	VertData vert_out;
	vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
	vert_out.uv  = v_in.uv;
	return vert_out;
}

float4 PSStab(VertData v_in) : TARGET
{
	float3 p = float3(v_in.uv, 1.0);
	float3 q = float3(dot(h0.xyz, p), dot(h1.xyz, p), dot(h2.xyz, p));
	return image.Sample(linearSampler, q.xy / max(q.z, 0.000001));
}

technique Draw
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSStab(v_in);
	}
}
)";

void SetStabStatus(QvdSource *s, const char *fmt, ...)
{
	char buf[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	std::lock_guard<std::mutex> l(s->stabLock);
	strncpy_s(s->stabStatus, buf, _TRUNCATE);
}

void FreeBuffer(QvdSource *s) // graphics context
{
	for (QvdSource::BufFrame &b : s->buf)
		if (b.tex)
			gs_texture_destroy(b.tex);
	s->buf.clear();
}

struct DrawPick {
	gs_texture_t *tex = nullptr;
	uint32_t flags = 0;
	stab::Mat3 h;
};

// Render thread (graphics context). Buffers the newest frame with its pose and picks the frame
// to show: the one 'delay' seconds old, re-projected to the look-ahead camera. false = show the
// newest frame plainly (stabilization off / no head pose).
bool StabPick(QvdSource *s, gs_texture_t *latest, uint32_t latestFlags, DrawPick *out)
{
	stab::Settings st;
	bool dirty;
	{
		std::lock_guard<std::mutex> l(s->stabLock);
		st = s->stabSettings;
		dirty = s->stabDirty;
		s->stabDirty = false;
	}
	if (dirty)
		s->look.Configure(st);
	if (!st.enable) {
		if (!s->buf.empty())
			FreeBuffer(s);
		SetStabStatus(s, "%s", obs_module_text("Stab.Status.Off"));
		return false;
	}
	const QvdSource::Pose &p = s->picKind == 1 ? s->layerPose : s->mirrorPose;
	if (!p.valid) {
		if (!s->stabLoggedNoPose) {
			LOG(LOG_INFO, "stabilization: this picture has no head pose - shown without stabilization");
			s->stabLoggedNoPose = true;
		}
		if (!s->buf.empty())
			FreeBuffer(s);
		SetStabStatus(s, "%s", obs_module_text("Stab.Status.NoPose"));
		return false;
	}
	s->stabLoggedNoPose = false;

	// another game / source / eye size: start over
	const uint32_t writer = s->picKind == 1 ? s->writerPid : 0;
	if (s->picKind != s->bufKind || writer != s->bufWriter || s->picW != s->bufW || s->picH != s->bufH) {
		FreeBuffer(s);
		s->look = stab::LookAhead();
		s->haveClock = false;
		s->look.Configure(st);
		s->bufKind = s->picKind;
		s->bufWriter = writer;
		s->bufW = s->picW;
		s->bufH = s->picH;
	}

	// buffer the newest frame
	if (p.seq != s->bufSeq) {
		s->bufSeq = p.seq;
		const double t = (double)p.tNs * 1e-9;
		s->look.AddPose(t, p.q, p.up, p.upKnown);
		const double off = t - (double)os_gettime_ns() * 1e-9;
		if (!s->haveClock || std::fabs(off - s->clockOffset) > 1.0) {
			s->clockOffset = off; // first frame, or the game clock jumped (new session / stale frame)
			s->haveClock = true;
		} else {
			s->clockOffset += (off - s->clockOffset) * 0.02;
		}
		const gs_color_format fmt = gs_texture_get_color_format(latest);
		QvdSource::BufFrame *slot = nullptr;
		for (QvdSource::BufFrame &b : s->buf)
			if (!b.inUse && b.tex && b.w == s->picW && b.h == s->picH && b.fmt == fmt) {
				slot = &b;
				break;
			}
		if (!slot) {
			s->buf.push_back(QvdSource::BufFrame());
			slot = &s->buf.back();
			slot->tex = gs_texture_create(s->picW, s->picH, fmt, 1, nullptr, 0);
			slot->w = s->picW;
			slot->h = s->picH;
			slot->fmt = fmt;
		}
		if (slot->tex) {
			gs_copy_texture(slot->tex, latest);
			slot->inUse = true;
			slot->t = t;
			slot->pose = p;
			slot->flags = latestFlags;
		}
	}

	// output time T on the game frame clock: OBS render time mapped with a slowly filtered offset,
	// minus the delay. Using the even OBS render time (not the jittery capture time of each frame)
	// keeps static scenery smooth.
	// OBS's own time of the frame being rendered (even steps, same clock as os_gettime_ns); a render
	// of the same frame again gets the same time.
	const uint64_t frameNs = obs_get_video_frame_time();
	const double now = (double)(frameNs ? frameNs : os_gettime_ns()) * 1e-9;
	const double T = now + s->clockOffset - st.delay;
	struct obs_video_info ovi;
	double obsFps = 30.0;
	if (obs_get_video_info(&ovi) && ovi.fps_den && ovi.fps_num)
		obsFps = (double)ovi.fps_num / ovi.fps_den;
	s->look.SetFrameRate(obsFps);

	// free frames no longer needed: stale frames from an earlier session, and every frame that no
	// future output time can show (stab::FramesToKeep; replayed on all recorded sessions 2026-11-05
	// at 30/60 fps and 0.2/1.0 s delay: the shown frame's time is the same in all 200686 OBS frames)
	const double latestT = s->look.Latest();
	std::vector<QvdSource::BufFrame *> live;
	for (QvdSource::BufFrame &b : s->buf) {
		if (b.inUse && b.t > latestT)
			b.inUse = false;
		if (b.inUse)
			live.push_back(&b);
	}
	std::sort(live.begin(), live.end(),
		  [](const QvdSource::BufFrame *a, const QvdSource::BufFrame *b) { return a->t < b->t; });
	std::vector<double> liveT(live.size());
	for (size_t i = 0; i < live.size(); i++)
		liveT[i] = live[i]->t;
	std::vector<char> keep;
	stab::FramesToKeep(liveT, T, 1.0 / obsFps, 0.003, 0.02, keep);
	size_t inUse = 0;
	for (size_t i = 0; i < live.size(); i++) {
		if (!keep[i])
			live[i]->inUse = false;
		inUse += live[i]->inUse;
	}
	// drop unused textures beyond a few spare ones (reused for the next frames)
	size_t spare = 0;
	for (size_t i = 0; i < s->buf.size();) {
		if (!s->buf[i].inUse && ++spare > 2) {
			if (s->buf[i].tex)
				gs_texture_destroy(s->buf[i].tex);
			s->buf.erase(s->buf.begin() + (ptrdiff_t)i);
		} else {
			i++;
		}
	}

	// Delay buffer still filling: the frame nearest to T is the oldest one; it is shown unstabilized
	// (zoomed like the stabilized picture), so the video is 'delay' behind from the start, the same
	// as the audio sync offset.
	const bool ready = s->look.Ready(T);
	if (!ready) {
		SetStabStatus(s, "%s", obs_module_text("Stab.Status.Filling"));
		s->stabLoggedStart = false;
	}
	// frame to show: the one nearest to T
	const QvdSource::BufFrame *pick = nullptr;
	for (const QvdSource::BufFrame &b : s->buf)
		if (b.inUse && (!pick || std::fabs(b.t - T) < std::fabs(pick->t - T)))
			pick = &b;
	if (!pick)
		return false;

	stab::Image img;
	const QvdRect c = qvd_center_crop_16x9(pick->w, pick->h);
	img.W = pick->w;
	img.H = pick->h;
	img.cropX = c.x;
	img.cropY = c.y;
	img.cropW = c.w;
	img.cropH = c.h;
	img.tl = pick->pose.tan[0];
	img.tr = pick->pose.tan[1];
	img.tu = pick->pose.tan[2];
	img.td = pick->pose.tan[3];
	const stab::Q head = stab::NormalizeQ(pick->pose.q);
	if (!ready) {
		out->tex = pick->tex;
		out->flags = pick->flags;
		out->h = stab::Homography(img, head, head, s->look.ZoomK());
		return true;
	}
	const uint64_t p0 = os_gettime_ns();
	const stab::Q cam = s->look.Camera(img, T, head);
	const double planMs = (double)(os_gettime_ns() - p0) * 1e-6;
	s->cost.plans++;
	s->cost.planSumMs += planMs;
	s->cost.planMaxMs = std::max(s->cost.planMaxMs, planMs);
	if (!s->stabLoggedStart) {
		LOG(LOG_INFO,
		    "stabilization running: output time %.3f s, shown frame %.3f s, newest frame %.3f s (video %.3f s behind the "
		    "newest frame; setting %.2f s), crop %.1f %%, %zu frames buffered",
		    T, pick->t, latestT, latestT - pick->t, st.delay, st.crop, inUse);
		s->stabLoggedStart = true;
	}
	out->tex = pick->tex;
	out->flags = pick->flags;
	out->h = stab::Homography(img, head, cam, s->look.ZoomK());

	const double mb = (double)inUse * pick->w * pick->h * 4.0 / (1024.0 * 1024.0);
	SetStabStatus(s, obs_module_text("Stab.Status.On"), st.crop, st.delay, (int)std::lround(st.delay * 1000.0),
		      (unsigned)inUse, mb);
	return true;
}

bool EnsureStabEffect(QvdSource *s)
{
	if (s->stabEffect || s->stabEffectFailed)
		return s->stabEffect != nullptr;
	char *err = nullptr;
	s->stabEffect = gs_effect_create(kStabEffect, "archan_vr_capture_stabilize.effect", &err);
	if (!s->stabEffect) {
		LOG(LOG_WARNING, "stabilization shader failed: %s - shown without stabilization", err ? err : "?");
		s->stabEffectFailed = true;
	}
	bfree(err);
	return s->stabEffect != nullptr;
}

// ---------------------------------------------------------------- source callbacks
const char *qvd_get_name(void *)
{
	return obs_module_text("ArchanVRCapture");
}

// The overlay key and the on/off checkbox (user decision 2026-11-07: a checkbox in the source, off by default)
// go to overlay.ini, which the layer in the game reads (so it works with OBS closed too). A key that is not
// valid is not saved (the last good one stays); the checkbox is always saved.
void SaveOverlaySettings(const char *text, bool enabled)
{
	QvdOverlaySettings st;
	bool exists = false;
	for (int i = 0; i < 20 && !qvd_overlay_read(&st, &exists); i++)
		Sleep(10); // being replaced right now
	QvdKey k;
	char canon[64] = "";
	if (qvd_parse_key(text, &k))
		qvd_format_key(k, canon, sizeof(canon));
	else
		LOG(LOG_WARNING, "overlay key '%s' is not a key - %s kept", text ? text : "", st.key);
	const bool keyChange = canon[0] && strcmp(st.key, canon) != 0;
	if (exists && !keyChange && st.enabled == enabled)
		return;
	if (keyChange)
		strcpy_s(st.key, sizeof(st.key), canon);
	st.enabled = enabled;
	if (qvd_overlay_write(st))
		LOG(LOG_INFO, "performance overlay %s, key %s - saved to %ls", enabled ? "ON" : "OFF", st.key,
		    qvd_overlay_settings_path());
	else
		LOG(LOG_WARNING, "overlay settings: cannot write %ls (error %lu)", qvd_overlay_settings_path(), GetLastError());
}

void qvd_update(void *data, obs_data_t *settings)
{
	QvdSource *s = static_cast<QvdSource *>(data);
	const char *mode = obs_data_get_string(settings, "resolution");
	const bool is1080 = mode && strcmp(mode, MODE_1080) == 0;
	const bool is1440 = mode && strcmp(mode, MODE_1440) == 0;
	s->fixedW = is1440 ? 2560 : 1920;
	s->fixedH = is1440 ? 1440 : 1080;
	if (!is1080 && !is1440 && !(mode && strcmp(mode, MODE_HMD) == 0))
		LOG(LOG_INFO, "resolution setting '%s' is not a current choice -> HMD res", mode ? mode : "");
	s->modeHmd = !is1080 && !is1440;
	s->lastSizeRequest = 0; // act on the new choice right away
	LOG(LOG_INFO, "recording resolution mode: %s",
	    s->modeHmd ? "HMD res (automatic)" : (is1440 ? "2560x1440" : "1920x1080"));
	// which eye (user decision 2026-11-07): sent to the layer with every poll, and to the SteamVR mirror
	const uint32_t eye = obs_data_get_int(settings, "eye") == 1 ? 1u : 0u;
	if (eye != s->eye.load())
		LOG(LOG_INFO, "eye: %s", eye ? "right" : "left");
	s->eye = eye;
	g_wantEye = (int)eye;

	stab::Settings st;
	st.enable = obs_data_get_bool(settings, "stab_enable");
	st.smooth = (double)obs_data_get_int(settings, "stab_smooth");
	st.horizon = obs_data_get_bool(settings, "stab_horizon");
	st.crop = obs_data_get_double(settings, "stab_crop");
	st.delay = obs_data_get_double(settings, "stab_delay");
	{
		std::lock_guard<std::mutex> l(s->stabLock);
		s->stabSettings = st;
		s->stabDirty = true;
	}
	s->delayNs = st.enable ? (int64_t)std::llround(st.delay * 1e9) : 0;
	AudioRefresh(s);
	SaveOverlaySettings(obs_data_get_string(settings, "overlay_key"), obs_data_get_bool(settings, "overlay_enabled"));
	LOG(LOG_INFO, "stabilization: %s, smoothness %.0f, keep horizon level %s, crop %.1f %%, video delay %.2f s",
	    st.enable ? "on" : "off", st.smooth, st.horizon ? "yes" : "no", st.crop, st.delay);
}

void qvd_removed(void *data, calldata_t *)
{
	QvdSource *s = static_cast<QvdSource *>(data);
	s->removed = true;
	AudioWant(s, 0);
}

// OBS video thread: the source entered / left the Program output (scene switch, eye icon)
void qvd_activate(void *data)
{
	QvdSource *s = static_cast<QvdSource *>(data);
	s->live = true;
	LOG(LOG_INFO, "live in Program: audio sync follows the video delay");
	AudioRefresh(s);
}

void qvd_deactivate(void *data)
{
	QvdSource *s = static_cast<QvdSource *>(data);
	s->live = false;
	LOG(LOG_INFO, "not live in Program: audio sources get their own offsets back");
	AudioRefresh(s);
}

void *qvd_create(obs_data_t *settings, obs_source_t *source)
{
	QvdSource *s = new QvdSource();
	s->source = source;
	signal_handler_connect(obs_source_get_signal_handler(source), "remove", qvd_removed, s);
	qvd_update(s, settings);
	return s;
}

// once a second while stabilization is on: audio sources added meanwhile / offsets typed by the user
void qvd_video_tick(void *data, float)
{
	QvdSource *s = static_cast<QvdSource *>(data);
	bool on;
	{
		std::lock_guard<std::mutex> l(s->stabLock);
		on = s->stabSettings.enable;
	}
	const uint64_t now = os_gettime_ns();
	if (on && s->live && !s->removed && now - s->audioCheckNs >= 1000000000ull) {
		s->audioCheckNs = now;
		AudioQueue();
	}
}

void qvd_destroy(void *data)
{
	QvdSource *s = static_cast<QvdSource *>(data);
	signal_handler_disconnect(obs_source_get_signal_handler(s->source), "remove", qvd_removed, s);
	AudioWant(s, 0);
	obs_enter_graphics();
	CloseTextures(s);
	if (s->mirrorCopy)
		gs_texture_destroy(s->mirrorCopy);
	s->mirrorCopy = nullptr;
	if (s->stabEffect)
		gs_effect_destroy(s->stabEffect);
	s->stabEffect = nullptr;
	if (s->compEffect)
		gs_effect_destroy(s->compEffect);
	s->compEffect = nullptr;
	if (s->overlayTex)
		gs_texture_destroy(s->overlayTex);
	s->overlayTex = nullptr;
	FreeBuffer(s);
	obs_leave_graphics();
	if (s->recorder.f) {
		fclose(s->recorder.f);
		LOG(LOG_INFO, "head motion recorder: %llu poses written to %s", (unsigned long long)s->recorder.rows,
		    s->recorder.path.c_str());
	}
	CloseShm(s);
	delete s;
}

uint32_t qvd_get_width(void *)
{
	struct obs_video_info ovi;
	return obs_get_video_info(&ovi) ? ovi.base_width : 1920;
}

uint32_t qvd_get_height(void *)
{
	struct obs_video_info ovi;
	return obs_get_video_info(&ovi) ? ovi.base_height : 1080;
}

void qvd_video_render_inner(QvdSource *s);

// Every 10 s while a picture is shown: this source's CPU time on OBS's render thread, the planner's
// share, and OBS's own average frame render time and lagged frames.
void CostAccount(QvdSource *s, double ms)
{
	QvdSource::Cost &c = s->cost;
	const uint64_t now = os_gettime_ns();
	if (!c.windowNs || s->picKind == 0) {
		c = QvdSource::Cost();
		c.windowNs = now;
		c.totalFrames = obs_get_total_frames();
		c.laggedFrames = obs_get_lagged_frames();
		return;
	}
	c.renders++;
	c.sumMs += ms;
	c.maxMs = std::max(c.maxMs, ms);
	const double secs = (double)(now - c.windowNs) * 1e-9;
	if (secs < 10.0)
		return;
	const uint32_t total = obs_get_total_frames(), lagged = obs_get_lagged_frames();
	size_t bufUsed = 0;
	double bufMb = 0;
	for (const QvdSource::BufFrame &b : s->buf) {
		bufUsed += b.inUse;
		bufMb += (double)b.w * b.h * 4.0 / (1024.0 * 1024.0);
	}
	LOG(LOG_INFO,
	    "cost (last %.0f s): this source on OBS's render thread avg %.2f ms, max %.2f ms per frame (%u renders); "
	    "stabilization planner avg %.2f ms, max %.2f ms (%u plans); delay buffer %zu frames in use + %zu spare = %.0f MB "
	    "GPU memory; OBS frame render avg %.2f ms, lagged frames %u of %u",
	    secs, c.renders ? c.sumMs / c.renders : 0.0, c.maxMs, c.renders, c.plans ? c.planSumMs / c.plans : 0.0,
	    c.planMaxMs, c.plans, bufUsed, s->buf.size() - bufUsed, bufMb, (double)obs_get_average_frame_time_ns() * 1e-6,
	    lagged - c.laggedFrames, total - c.totalFrames);
	c = QvdSource::Cost();
	c.windowNs = now;
	c.totalFrames = total;
	c.laggedFrames = lagged;
}

void qvd_video_render(void *data, gs_effect_t *)
{
	QvdSource *s = static_cast<QvdSource *>(data);
	const uint64_t t0 = os_gettime_ns();
	qvd_video_render_inner(s);
	CostAccount(s, (double)(os_gettime_ns() - t0) * 1e-6);
}

// The panel at the top left of the video: 384 x 120 at 1080p, scaled with the video height.
// Premultiplied alpha, bytes drawn as they are (display-ready grey levels).
void DrawOverlay(QvdSource *s, float cy)
{
	const float scale = cy / 1080.0f * 0.8f, margin = cy / 1080.0f * 24.0f;
	gs_effect_t *e = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), s->overlayTex);
	gs_matrix_push();
	gs_matrix_translate3f(margin, margin, 0.0f);
	gs_matrix_scale3f(scale, scale, 1.0f);
	while (gs_effect_loop(e, "Draw"))
		gs_draw_sprite(s->overlayTex, 0, QVD_OVERLAY_W, QVD_OVERLAY_H);
	gs_matrix_pop();
	gs_blend_state_pop();
}

void qvd_video_render_inner(QvdSource *s)
{
	Poll(s);
	PoseRecord(s, s->recorder);
	if (s->picKind == 0)
		return;
	gs_texture_t *pic = s->picKind == 1 ? (s->layerPic ? s->layerPic : s->copy) : s->mirrorCopy;
	uint32_t picFlags = s->picKind == 1 ? s->flags : s->mirrorFlags;
	KeepVideoSize(s);

	const QvdRect c = qvd_center_crop_16x9(s->picW, s->picH);
	const float cx = (float)qvd_get_width(s), cy = (float)qvd_get_height(s);

	// stabilization: draw the delayed frame through the look-ahead camera
	DrawPick pick;
	const bool stabilized = StabPick(s, pic, picFlags, &pick) && EnsureStabEffect(s);
	const stab::Mat3 &h = pick.h;
	if (stabilized) {
		pic = pick.tex;
		picFlags = pick.flags;
	}

	// Colour: 8-bit swapchains hold display-ready (sRGB-encoded) bytes -> show bytes as they are.
	// Float swapchains hold linear light -> encode to sRGB on output.
	const gs_color_format f = gs_texture_get_color_format(pic);
	const bool hasSrgbView = f == GS_RGBA || f == GS_BGRA || f == GS_BGRX;
	const bool linear = (picFlags & QVD_FLAG_LINEAR_FLOAT) != 0;

	gs_effect_t *effect = stabilized ? s->stabEffect : obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	const bool prevSrgb = gs_framebuffer_srgb_enabled();
	if (linear) {
		gs_enable_framebuffer_srgb(true);
		gs_effect_set_texture(image, pic);
	} else if (hasSrgbView) {
		gs_enable_framebuffer_srgb(true);
		gs_effect_set_texture_srgb(image, pic);
	} else {
		gs_enable_framebuffer_srgb(false);
		gs_effect_set_texture(image, pic);
	}

	if (stabilized) {
		const char *names[3] = {"h0", "h1", "h2"};
		for (int i = 0; i < 3; i++) {
			struct vec4 row;
			vec4_set(&row, (float)h.m[i][0], (float)h.m[i][1], (float)h.m[i][2], 0.0f);
			gs_effect_set_vec4(gs_effect_get_param_by_name(effect, names[i]), &row);
		}
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(pic, 0, (uint32_t)cx, (uint32_t)cy);
	} else {
		gs_matrix_push();
		gs_matrix_scale3f(cx / (float)c.w, cy / (float)c.h, 1.0f);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite_subregion(pic, 0, c.x, c.y, c.w, c.h);
		gs_matrix_pop();
	}

	if (s->picKind == 1 && s->overlayShow)
		DrawOverlay(s, cy);
	gs_enable_framebuffer_srgb(prevSrgb);
}

// ---------------------------------------------------------------- automatic resolution
struct SizeRequest {
	uint32_t cx, cy;
};

bool OutputsActive()
{
	return obs_frontend_recording_active() || obs_frontend_streaming_active() ||
	       obs_frontend_replay_buffer_active() || obs_frontend_virtualcam_active();
}

// Runs on the OBS UI thread (queued from the render thread).
void SetVideoSizeTask(void *param)
{
	SizeRequest *r = static_cast<SizeRequest *>(param);
	struct obs_video_info ovi = {};
	if (obs_get_video_info(&ovi) && ovi.base_width == r->cx && ovi.base_height == r->cy &&
	    ovi.output_width == r->cx && ovi.output_height == r->cy) {
		delete r;
		return;
	}
	if (OutputsActive()) {
		// OBS cannot change resolution while an output runs; the render thread asks again later.
		static uint64_t lastLog = 0;
		const uint64_t now = GetTickCount64();
		if (now - lastLog > 60000) {
			LOG(LOG_INFO, "resolution %ux%u waits until recording/streaming stops", r->cx, r->cy);
			lastLog = now;
		}
		delete r;
		return;
	}
	config_t *cfg = obs_frontend_get_profile_config();
	if (cfg) {
		config_set_uint(cfg, "Video", "BaseCX", r->cx);
		config_set_uint(cfg, "Video", "BaseCY", r->cy);
		config_set_uint(cfg, "Video", "OutputCX", r->cx);
		config_set_uint(cfg, "Video", "OutputCY", r->cy);
		config_save_safe(cfg, "tmp", nullptr);
		obs_frontend_reset_video();
		const bool ok = obs_get_video_info(&ovi) && ovi.base_width == r->cx && ovi.base_height == r->cy &&
				ovi.output_width == r->cx && ovi.output_height == r->cy;
		LOG(ok ? LOG_INFO : LOG_WARNING, "resolution %ux%u requested -> OBS now base %ux%u output %ux%u", r->cx,
		    r->cy, ovi.base_width, ovi.base_height, ovi.output_width, ovi.output_height);
	}
	delete r;
}

// Render thread: while the game picture is shown, keep OBS at the chosen size.
void KeepVideoSize(QvdSource *s)
{
	uint32_t cx = s->fixedW, cy = s->fixedH;
	if (s->modeHmd) {
		const QvdRect c = qvd_center_crop_16x9(s->picW, s->picH);
		cx = c.w;
		cy = c.h;
		if (cx > HMD_MAX_WIDTH) { // keep 16:9 (the crop is exactly 16:9)
			cx = HMD_MAX_WIDTH;
			cy = HMD_MAX_WIDTH / 16u * 9u;
		}
	}
	struct obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || cx < 32 || cy < 18)
		return;
	if (ovi.base_width == cx && ovi.base_height == cy && ovi.output_width == cx && ovi.output_height == cy)
		return;
	const uint64_t now = GetTickCount64();
	if (s->lastSizeRequest && now - s->lastSizeRequest < 2000)
		return;
	s->lastSizeRequest = now;
	obs_queue_task(OBS_TASK_UI, SetVideoSizeTask, new SizeRequest{cx, cy}, false);
}

// Under the key field: what it does, or that the text is not a key.
bool OverlayKeyModified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	obs_property_t *info = obs_properties_get(props, "overlay_info");
	if (!info)
		return false;
	const char *text = obs_data_get_string(settings, "overlay_key");
	QvdKey k;
	struct dstr d = {};
	if (qvd_parse_key(text, &k)) {
		char canon[64];
		qvd_format_key(k, canon, sizeof(canon));
		dstr_printf(&d, obs_module_text("Overlay.Help.Key"), canon);
		obs_property_set_description(info, d.array);
		obs_property_text_set_info_type(info, OBS_TEXT_INFO_NORMAL);
	} else {
		dstr_printf(&d, obs_module_text("Overlay.Help.Bad"), text ? text : "");
		obs_property_set_description(info, d.array);
		obs_property_text_set_info_type(info, OBS_TEXT_INFO_ERROR);
	}
	dstr_free(&d);
	return true;
}

// "Save logs" button (user decision 2026-11-08): one zip on the Desktop with our layer.log and the OBS logs and OBS
// crash reports of the last 48 h, then Explorer opens with the zip selected. Nothing is uploaded; the user sends the file themselves.
bool SaveLogsClicked(obs_properties_t *, obs_property_t *, void *)
{
	std::wstring desktop;
	PWSTR d = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &d)))
		desktop = d;
	CoTaskMemFree(d);
	auto configDir = [](const char *sub) {
		std::wstring dir;
		if (char *u = os_get_config_path_ptr(sub)) {
			wchar_t *w = nullptr;
			if (os_utf8_to_wcs_ptr(u, 0, &w) && w)
				dir = w;
			bfree(w);
			bfree(u);
		}
		return dir;
	};
	const std::wstring obsLogs = configDir("obs-studio/logs"), obsCrashes = configDir("obs-studio/crashes");
	SYSTEMTIME t;
	GetLocalTime(&t);
	wchar_t name[96];
	swprintf_s(name, L"\\ArchanVRCapture-logs-%04u%02u%02u-%02u%02u%02u.zip", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
		   t.wSecond);
	const std::wstring zip = desktop + name;
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	QvdLogZipResult r;
	if (desktop.empty())
		r.error = "the Desktop folder was not found";
	else
		r = qvd_save_logs_zip(qvd_log_dir(), obsLogs, zip, now, 48.0, obsCrashes);
	if (r.ok) {
		std::string list;
		for (const std::string &n : r.names)
			list += (list.empty() ? "" : ", ") + n;
		LOG(LOG_INFO, "save logs: %s, %d files, %llu bytes: %s", qvd_narrow(zip).c_str(), r.files, r.bytes, list.c_str());
		ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + zip + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
	} else {
		LOG(LOG_WARNING, "save logs failed: %s", r.error.c_str());
		const std::string msg = std::string(obs_module_text("SaveLogs.Failed")) + "\n\n" + r.error;
		MessageBoxA(GetActiveWindow(), msg.c_str(), obs_module_text("ArchanVRCapture"), MB_OK | MB_ICONWARNING);
	}
	return false;
}

obs_properties_t *qvd_get_properties(void *data)
{
	QvdSource *s = static_cast<QvdSource *>(data);
	obs_properties_t *props = obs_properties_create();

	struct dstr status = {};
	std::unique_lock<std::mutex> lock;
	if (s)
		lock = std::unique_lock<std::mutex>(s->shmLock);
	if (s && s->picKind == 1 && s->shm && s->handle) {
		const QvdRect c = qvd_center_crop_16x9(s->width, s->height);
		dstr_printf(&status, "%s: %s | %s | %s %ux%u | 16:9 %ux%u", obs_module_text("Status.Connected"),
			    s->shm->appName, s->shm->runtimeName,
			    obs_module_text(s->shm->picEye == 1 ? "Status.RightEye" : "Status.LeftEye"), s->width,
			    s->height, c.w, c.h);
	} else if (s && s->picKind == 2) {
		const uint32_t w = s->picW, h = s->picH;
		const QvdRect c = qvd_center_crop_16x9(w, h);
		dstr_printf(&status, "%s: %s | %s %ux%u | 16:9 %ux%u", obs_module_text("Status.Connected"),
			    obs_module_text("Status.SteamVR"),
			    obs_module_text(s->eye == 1 ? "Status.RightEye" : "Status.LeftEye"), w, h, c.w, c.h);
	} else {
		dstr_copy(&status, obs_module_text("Status.Waiting"));
	}
	if (lock.owns_lock())
		lock.unlock();
	obs_property_t *p = obs_properties_add_text(props, "status", status.array, OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(p, true);
	dstr_free(&status);

	obs_property_t *list = obs_properties_add_list(props, "resolution", obs_module_text("RecordResolution"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, obs_module_text("Mode.Hmd"), MODE_HMD);
	obs_property_list_add_string(list, obs_module_text("Mode.1440"), MODE_1440);
	obs_property_list_add_string(list, obs_module_text("Mode.1080"), MODE_1080);
	obs_property_t *eyeList = obs_properties_add_list(props, "eye", obs_module_text("Eye"), OBS_COMBO_TYPE_LIST,
							  OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(eyeList, obs_module_text("Eye.Left"), 0);
	obs_property_list_add_int(eyeList, obs_module_text("Eye.Right"), 1);

	obs_properties_add_bool(props, "overlay_enabled", obs_module_text("Overlay.Enabled"));
	obs_property_t *key = obs_properties_add_text(props, "overlay_key", obs_module_text("Overlay.Key"), OBS_TEXT_DEFAULT);
	obs_property_t *keyInfo = obs_properties_add_text(props, "overlay_info", obs_module_text("Overlay.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(keyInfo, true);
	obs_property_set_modified_callback(key, OverlayKeyModified);

	// stabilization: the group's checkbox is On / Off
	obs_properties_t *sp = obs_properties_create();
	char stabStatus[400] = "";
	if (s) {
		std::lock_guard<std::mutex> l(s->stabLock);
		strncpy_s(stabStatus, s->stabStatus, _TRUNCATE);
	}
	if (stabStatus[0]) {
		obs_property_t *t = obs_properties_add_text(sp, "stab_status", stabStatus, OBS_TEXT_INFO);
		obs_property_text_set_info_word_wrap(t, true);
	}
	obs_properties_add_int_slider(sp, "stab_smooth", obs_module_text("Stab.Smooth"), 0, 100, 1);
	obs_properties_add_bool(sp, "stab_horizon", obs_module_text("Stab.Horizon"));
	obs_property_t *crop = obs_properties_add_float_slider(sp, "stab_crop", obs_module_text("Stab.Crop"), 1.0, 40.0, 0.5);
	obs_property_float_set_suffix(crop, " %");
	obs_property_t *delay = obs_properties_add_float_slider(sp, "stab_delay", obs_module_text("Stab.Delay"), 0.1, 2.0, 0.05);
	obs_property_float_set_suffix(delay, " s");
	obs_properties_add_group(props, "stab_enable", obs_module_text("Stab.Group"), OBS_GROUP_CHECKABLE, sp);

	obs_properties_add_button2(props, "save_logs", obs_module_text("SaveLogs"), SaveLogsClicked, s);
	obs_property_t *logsInfo = obs_properties_add_text(props, "save_logs_info", obs_module_text("SaveLogs.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(logsInfo, true);
	return props;
}

void qvd_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "resolution", MODE_HMD);
	obs_data_set_default_int(settings, "eye", 0); // left (user decision 2026-11-07)
	obs_data_set_default_bool(settings, "stab_enable", true);
	obs_data_set_default_int(settings, "stab_smooth", 100); // user decision 2026-11-06
	obs_data_set_default_bool(settings, "stab_horizon", false);
	obs_data_set_default_double(settings, "stab_crop", 15.0);  // user decision 2026-11-05 after VD/Q3 test
	obs_data_set_default_double(settings, "stab_delay", 0.20); // user decision 2026-11-05 after VD/Q3 test
	obs_data_set_default_string(settings, "overlay_key", QVD_OVERLAY_DEFAULT_KEY); // user decision 2026-11-06
	obs_data_set_default_bool(settings, "overlay_enabled", false);                 // user decision 2026-11-07
}

} // namespace

// "OBS is running" for the layer in the game (user decision 2026-11-07: the overlay is shown by default
// while OBS runs). Held from module load to unload; any integrity level may open it.
static HANDLE g_obsPresence = nullptr;

bool obs_module_load(void)
{
	{
		SECURITY_DESCRIPTOR sd;
		InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
		SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
		SECURITY_ATTRIBUTES sa = {sizeof(sa), &sd, FALSE};
		g_obsPresence = CreateMutexW(&sa, FALSE, qvd_obs_presence_name());
		if (!g_obsPresence)
			blog(LOG_WARNING, "[archan-vr-capture] presence mutex failed (error %lu): the overlay will not show by "
			     "itself while OBS runs", GetLastError());
	}
	struct obs_source_info info = {};
	info.id = "archan_vr_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.get_name = qvd_get_name;
	info.create = qvd_create;
	info.destroy = qvd_destroy;
	info.get_width = qvd_get_width;
	info.get_height = qvd_get_height;
	info.video_render = qvd_video_render;
	info.video_tick = qvd_video_tick;
	info.get_properties = qvd_get_properties;
	info.get_defaults = qvd_get_defaults;
	info.update = qvd_update;
	info.activate = qvd_activate;
	info.deactivate = qvd_deactivate;
	info.icon_type = OBS_ICON_TYPE_GAME_CAPTURE;
	obs_register_source(&info);

	// Old ids from before the renames ("Quest VR Picture", "VR Capture"): hidden from
	// "Add source", but scenes that already contain them keep loading (as Archan VR Capture).
	static const char *const oldIds[] = {"quest_vd_vr_picture", "vr_capture"};
	for (const char *id : oldIds) {
		struct obs_source_info old = info;
		old.id = id;
		old.output_flags |= OBS_SOURCE_DEPRECATED;
		obs_register_source(&old);
	}

	if (LoadOpenVR()) {
		g_vrStop = false;
		g_vrThread = std::thread(VrThread);
	}
	obs_frontend_add_event_callback(OnFrontendEvent, nullptr);
	obs_frontend_add_save_callback(AudioSaveLoad, nullptr);
	// without the OBS frontend (tests) there is no scene collection to wait for
	g_audio.ready = obs_frontend_get_main_window() == nullptr;
	char testRec[8] = "";
	if (GetEnvironmentVariableA("ARCHAN_VRCAPTURE_TEST_RECORDING", testRec, sizeof(testRec)) && testRec[0] == '1') {
		g_recActive = true; // tests only: behave as if OBS were recording
		LOG(LOG_INFO, "test mode: acting as if OBS is recording");
	}
	LOG(LOG_INFO,
	    "loaded (source 'archan_vr_capture', old ids 'quest_vd_vr_picture' 'vr_capture' kept; SteamVR mirror %s)",
	    g_openvr ? "on" : "off");
	// OBS start: delete logs older than 48 h (user decision 2026-11-05)
	const QvdLogCleanup c = qvd_cleanup_logs_now();
	LOG(LOG_INFO, "log cleanup (%%LOCALAPPDATA%%\\ArchanVRCapture\\logs, older than %.0f h): %d files deleted%s, %d old lines removed from layer.log",
	    QVD_LOG_MAX_AGE_HOURS, c.filesDeleted, c.filesFailed ? " (some could not be deleted)" : "", c.linesRemoved);
	return true;
}

// tests only: the scene collection save / load step of the audio sync (no OBS frontend in tests)
extern "C" __declspec(dllexport) void archan_vr_capture_test_audio_saveload(obs_data_t *data, bool saving)
{
	AudioSaveLoad(data, saving, nullptr);
}

void obs_module_unload(void)
{
	if (g_obsPresence) {
		CloseHandle(g_obsPresence);
		g_obsPresence = nullptr;
	}
	obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
	obs_frontend_remove_save_callback(AudioSaveLoad, nullptr);
	if (g_vrThread.joinable()) {
		g_vrStop = true;
		g_vrThread.join();
	}
	if (g_openvr) {
		FreeLibrary(g_openvr);
		g_openvr = nullptr;
	}
}
