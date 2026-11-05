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

#include <openvr.h> // OpenVR SDK v2.15.6 header; openvr_api.dll is loaded at runtime

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/dstr.h>

#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "../common/qvd_shared.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("archan-vr-capture", "en-US")

#define LOG(level, fmt, ...) blog(level, "[archan-vr-capture] " fmt, ##__VA_ARGS__)

namespace {

// Recording resolution modes (source setting "resolution")
#define MODE_HMD "hmd"         // automatic: left eye native 16:9 crop size (1:1 pixels)
#define MODE_1080 "1920x1080"  // fixed 1920x1080

struct QvdSource {
	obs_source_t *source = nullptr;
	std::mutex shmLock; // render thread vs. properties (UI thread)

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

	volatile bool modeHmd = true; // set from settings (UI thread), read by render thread
	uint64_t lastSizeRequest = 0;

	// SteamVR mirror picture (used when no OpenXR layer picture is there)
	gs_texture_t *mirrorCopy = nullptr;
	uint64_t mirrorGen = ~0ull;
	bool mirrorFrame = false;
	uint32_t mirrorFlags = 0;

	// picture currently shown: 0 none, 1 OpenXR layer, 2 SteamVR mirror
	volatile int picKind = 0;
	volatile uint32_t picW = 0, picH = 0;
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
		if (g_mirror.srv && comp)
			comp->ReleaseMirrorTextureD3D11(g_mirror.srv);
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
		result = (int)comp->GetMirrorTextureD3D11(vr::Eye_Left, dev, &srv);
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
				LOG(LOG_INFO, "SteamVR mirror left eye %ux%u dxgi format %d (mips %u, array %u, samples %u)",
				    d.Width, d.Height, (int)d.Format, d.MipLevels, d.ArraySize, d.SampleDesc.Count);
			} else {
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
			while (sys->PollNextEvent(&ev, sizeof(ev)))
				if (ev.eventType == vr::VREvent_Quit)
					quit = true;
			if (quit) {
				LOG(LOG_INFO, "SteamVR is quitting - disconnecting");
				MirrorDrop(comp);
				vr_Shutdown();
				sys = nullptr;
				comp = nullptr;
				connected = false;
				nextTry = now + 5000;
				continue;
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
		vr_Shutdown();
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
	s->haveFrame = false;
	s->handle = 0;
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
		return true;
	}
	return false;
}

void PollLayer(QvdSource *s)
{
	const uint64_t now = GetTickCount64();
	if (!TryOpenShm(s, now))
		return;

	s->shm->readerTickMs = now; // tells the layer that OBS is showing the picture

	if (ReadDescriptor(s) && s->handle) {
		s->shared = gs_texture_open_shared(s->handle);
		if (s->shared) {
			s->copy = gs_texture_create(gs_texture_get_width(s->shared), gs_texture_get_height(s->shared),
						    gs_texture_get_color_format(s->shared), 1, nullptr, 0);
			LOG(LOG_INFO, "left eye picture %ux%u dxgi format %u flags %u", s->width, s->height,
			    s->dxgiFormat, s->flags);
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
				gs_texture_release_sync(s->shared, 0);
				s->lastFrameCount = fc;
				s->haveFrame = true;
			}
		}
	}
}

// Picks the picture: the OpenXR layer (game frames) first, else SteamVR's mirror.
void Poll(QvdSource *s)
{
	PollLayer(s);
	int kind = 0;
	uint32_t w = 0, h = 0;
	if (s->shared && s->copy && s->haveFrame) {
		kind = 1;
		w = s->width;
		h = s->height;
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

// ---------------------------------------------------------------- source callbacks
const char *qvd_get_name(void *)
{
	return obs_module_text("ArchanVRCapture");
}

void qvd_update(void *data, obs_data_t *settings)
{
	QvdSource *s = static_cast<QvdSource *>(data);
	const char *mode = obs_data_get_string(settings, "resolution");
	const bool is1080 = mode && strcmp(mode, MODE_1080) == 0;
	if (!is1080 && !(mode && strcmp(mode, MODE_HMD) == 0))
		LOG(LOG_INFO, "resolution setting '%s' is not a current choice -> HMD res", mode ? mode : "");
	s->modeHmd = !is1080;
	s->lastSizeRequest = 0; // act on the new choice right away
	LOG(LOG_INFO, "recording resolution mode: %s", s->modeHmd ? "HMD res (automatic)" : "1920x1080");
}

void *qvd_create(obs_data_t *settings, obs_source_t *source)
{
	QvdSource *s = new QvdSource();
	s->source = source;
	qvd_update(s, settings);
	return s;
}

void qvd_destroy(void *data)
{
	QvdSource *s = static_cast<QvdSource *>(data);
	obs_enter_graphics();
	CloseTextures(s);
	if (s->mirrorCopy)
		gs_texture_destroy(s->mirrorCopy);
	s->mirrorCopy = nullptr;
	obs_leave_graphics();
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

void qvd_video_render(void *data, gs_effect_t *)
{
	QvdSource *s = static_cast<QvdSource *>(data);
	Poll(s);
	if (s->picKind == 0)
		return;
	gs_texture_t *pic = s->picKind == 1 ? s->copy : s->mirrorCopy;
	const uint32_t picFlags = s->picKind == 1 ? s->flags : s->mirrorFlags;
	KeepVideoSize(s);

	const QvdRect c = qvd_center_crop_16x9(s->picW, s->picH);
	const float cx = (float)qvd_get_width(s), cy = (float)qvd_get_height(s);

	// Colour: 8-bit swapchains hold display-ready (sRGB-encoded) bytes -> show bytes as they are.
	// Float swapchains hold linear light -> encode to sRGB on output.
	const gs_color_format f = gs_texture_get_color_format(pic);
	const bool hasSrgbView = f == GS_RGBA || f == GS_BGRA || f == GS_BGRX;
	const bool linear = (picFlags & QVD_FLAG_LINEAR_FLOAT) != 0;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
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

	gs_matrix_push();
	gs_matrix_scale3f(cx / (float)c.w, cy / (float)c.h, 1.0f);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite_subregion(pic, 0, c.x, c.y, c.w, c.h);
	gs_matrix_pop();

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
	uint32_t cx = 1920, cy = 1080;
	if (s->modeHmd) {
		const QvdRect c = qvd_center_crop_16x9(s->picW, s->picH);
		cx = c.w;
		cy = c.h;
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
			    s->shm->appName, s->shm->runtimeName, obs_module_text("Status.LeftEye"), s->width,
			    s->height, c.w, c.h);
	} else if (s && s->picKind == 2) {
		const uint32_t w = s->picW, h = s->picH;
		const QvdRect c = qvd_center_crop_16x9(w, h);
		dstr_printf(&status, "%s: %s | %s %ux%u | 16:9 %ux%u", obs_module_text("Status.Connected"),
			    obs_module_text("Status.SteamVR"), obs_module_text("Status.LeftEye"), w, h, c.w, c.h);
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
	obs_property_list_add_string(list, obs_module_text("Mode.1080"), MODE_1080);
	return props;
}

void qvd_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "resolution", MODE_HMD);
}

} // namespace

bool obs_module_load(void)
{
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
	info.get_properties = qvd_get_properties;
	info.get_defaults = qvd_get_defaults;
	info.update = qvd_update;
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
	LOG(LOG_INFO,
	    "loaded (source 'archan_vr_capture', old ids 'quest_vd_vr_picture' 'vr_capture' kept; SteamVR mirror %s)",
	    g_openvr ? "on" : "off");
	return true;
}

void obs_module_unload(void)
{
	if (g_vrThread.joinable()) {
		g_vrStop = true;
		g_vrThread.join();
	}
	if (g_openvr) {
		FreeLibrary(g_openvr);
		g_openvr = nullptr;
	}
}
