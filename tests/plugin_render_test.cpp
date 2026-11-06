// Proof test for the OBS plugin, run inside real libobs (OBS 32.2.2 DLLs).
// Plays the game layer (shared memory + D3D11 shared texture with keyed mutex)
// with the eye size measured from DCS on VDXR (2496x2688), then:
//  Phase A  mode "1920x1080": canvas 1920x1080. Output must be the centre 16:9
//           crop scaled to fill (gradient check), colours unchanged (B = 128),
//           and the plugin must NOT ask for another resolution.
//  Phase B  mode "hmd": the plugin must ask OBS (UI task) for exactly the crop
//           size 2496x1404. The test applies it like the OBS frontend does
//           (obs_reset_video), renders again, and the output must be the eye
//           pixels 1:1 (unique per-pixel pattern), with no further request.
// Run with the OBS bin\64bit folder as working directory.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <obs.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <util/base.h>

#include "../common/qvd_shared.h"
#include "../obs-plugin/stabilizer.h"

static const uint32_t EYE_W = 2496, EYE_H = 2688; // DCS on VDXR, layer.log 2026-11-05

// libobs log -> stdout, and catch the plugin's fixed crop value and pose recorder file
static unsigned long long g_poseRows = 0;
static double g_shownT = -1, g_newestT = -1; // from the plugin's "stabilization running" line
static std::string g_posePath;
static std::string g_costLine; // the plugin's cost line (every 10 s while a picture is shown)
static void LogHandler(int lvl, const char *fmt, va_list args, void *)
{
	char msg[4096];
	vsnprintf(msg, sizeof(msg), fmt, args);
	printf("%s: %s\n", lvl <= LOG_ERROR ? "error" : lvl <= LOG_WARNING ? "warning" : lvl <= LOG_INFO ? "info" : "debug",
	       msg);
	const char *sr = strstr(msg, "stabilization running: ");
	if (sr)
		sscanf_s(strstr(sr, "shown frame ") + 12, "%lf s, newest frame %lf", &g_shownT, &g_newestT);
	const char *w = strstr(msg, "poses written to ");
	if (w) {
		g_poseRows = strtoull(strstr(msg, "recorder: ") + 10, nullptr, 10);
		g_posePath = w + 17;
	}
	if (strstr(msg, "cost (last "))
		g_costLine = msg;
}

// world-fixed test scene: smooth colour waves over yaw/pitch of the view direction
static uint32_t Scene(stab::V3 d)
{
	const double yaw = std::atan2(d.x, -d.z), pitch = std::asin(std::max(-1.0, std::min(1.0, d.y)));
	auto c = [](double v) { return (uint32_t)std::lround(128.0 + 100.0 * v); };
	return c(std::sin(20 * yaw)) | (c(std::sin(20 * pitch)) << 8) | (c(std::sin(13 * (yaw + pitch))) << 16) |
	       (255u << 24);
}

static uint8_t GradX(uint32_t x) { return (uint8_t)((x * 255u) / (EYE_W - 1)); }
static uint8_t GradY(uint32_t y) { return (uint8_t)((y * 255u) / (EYE_H - 1)); }
static uint32_t Gradient(uint32_t x, uint32_t y) { return GradX(x) | (GradY(y) << 8) | (128u << 16) | (255u << 24); }
static uint32_t Unique(uint32_t x, uint32_t y)
{
	const uint32_t b = ((x >> 8) & 15u) | (((y >> 8) & 15u) << 4);
	return (x & 255u) | ((y & 255u) << 8) | (b << 16) | (255u << 24);
}

#define CHECK(c, ...)                                        \
	do {                                                 \
		if (!(c)) {                                  \
			printf("FAIL line %d: ", __LINE__); \
			printf(__VA_ARGS__);                 \
			printf("\n");                        \
			return 1;                            \
		}                                            \
	} while (0)

// ---------------------------------------------------------------- UI task capture
struct SizeRequestLayout { // must match SizeRequest in plugin.cpp
	uint32_t cx, cy;
};
static std::vector<SizeRequestLayout> g_requests;
static void TestUiTaskHandler(obs_task_t task, void *param, bool)
{
	// libobs itself queues set_ui_thread with param NULL right inside obs_set_ui_task_handler (obs.c:3384)
	if (param)
		g_requests.push_back(*static_cast<SizeRequestLayout *>(param));
	task(param); // runs the plugin task: no frontend here, so it only frees the request
}

static bool ResetVideo(uint32_t cx, uint32_t cy)
{
	struct obs_video_info ovi = {};
	ovi.graphics_module = "C:/Program Files/obs-studio/bin/64bit/libobs-d3d11.dll";
	ovi.fps_num = 30;
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = cx;
	ovi.base_height = ovi.output_height = cy;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.gpu_conversion = true;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	return obs_reset_video(&ovi) == OBS_VIDEO_SUCCESS;
}

static bool Render(obs_source_t *src, uint32_t w, uint32_t h, std::vector<uint8_t> &out)
{
	out.assign((size_t)w * h * 4, 0);
	obs_enter_graphics();
	gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(w, h, GS_RGBA);
	for (int pass = 0; pass < 2; pass++) { // pass 0 opens/copies, pass 1 draws again
		gs_texrender_reset(tr);
		if (gs_texrender_begin(tr, w, h)) {
			struct vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
			obs_source_video_render(src);
			gs_texrender_end(tr);
		}
	}
	gs_stage_texture(stage, gs_texrender_get_texture(tr));
	uint8_t *data = nullptr;
	uint32_t linesize = 0;
	const bool mapped = gs_stagesurface_map(stage, &data, &linesize);
	if (mapped) {
		for (uint32_t y = 0; y < h; y++)
			memcpy(&out[(size_t)y * w * 4], data + (size_t)y * linesize, (size_t)w * 4);
		gs_stagesurface_unmap(stage);
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(tr);
	obs_leave_graphics();
	return mapped;
}

static void SetMode(obs_source_t *src, const char *mode)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "resolution", mode);
	obs_source_update(src, s);
	obs_data_release(s);
	// libobs applies a video source's update on its next tick (obs-source.c:1075/1373),
	// done by the graphics thread every frame (obs-video.c:82). Give it a few frames.
	Sleep(300);
}

int main(int argc, char **argv)
{
	// own channel name for this test and its child processes: never the real one of a running game/OBS
	if (!GetEnvironmentVariableW(L"ARCHAN_VRCAPTURE_TEST_CHANNEL", nullptr, 0))
		SetEnvironmentVariableW(L"ARCHAN_VRCAPTURE_TEST_CHANNEL", L"Local\\ArchanVRCapture_TestChannel");
	setvbuf(stdout, nullptr, _IONBF, 0); // keep every line if something crashes
	if (argc < 3) {
		printf("usage: plugin_render_test <archan-vr-capture.dll> <plugin data dir>\n");
		return 2;
	}

	// Never touch a live channel (a real game or OBS using it).
	if (HANDLE live = OpenFileMappingW(FILE_MAP_READ, FALSE, qvd_channel_name())) {
		QvdShared *v = (QvdShared *)MapViewOfFile(live, FILE_MAP_READ, 0, 0, sizeof(QvdShared));
		printf("NOT RUN: picture channel is in use (writer pid %u, app '%s'). Close the game and OBS, then rerun.\n",
		       v ? v->writerPid : 0, v ? v->appName : "?");
		return 3;
	}

	// ------------------------------------------------ fake game side
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
					  &dev, nullptr, &ctx)),
	      "device");
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = EYE_W;
	td.Height = EYE_H;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	std::vector<uint32_t> px((size_t)EYE_W * EYE_H);
	for (uint32_t y = 0; y < EYE_H; y++)
		for (uint32_t x = 0; x < EYE_W; x++)
			px[(size_t)y * EYE_W + x] = Gradient(x, y);
	D3D11_SUBRESOURCE_DATA init = {px.data(), EYE_W * 4, 0};
	ID3D11Texture2D *tex = nullptr;
	CHECK(SUCCEEDED(dev->CreateTexture2D(&td, &init, &tex)), "shared texture");
	IDXGIResource *res = nullptr;
	HANDLE handle = nullptr;
	tex->QueryInterface(__uuidof(IDXGIResource), (void **)&res);
	CHECK(res && SUCCEEDED(res->GetSharedHandle(&handle)), "shared handle");
	IDXGIKeyedMutex *km = nullptr;
	CHECK(SUCCEEDED(tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&km)), "keyed mutex");
	ctx->Flush();

	HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(QvdShared), qvd_channel_name());
	QvdShared *shm = (QvdShared *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(QvdShared));
	CHECK(shm, "shm");
	shm->magic = QVD_MAGIC;
	shm->version = QVD_VERSION;
	shm->seq = 2;
	shm->sharedHandle = (uint32_t)(uintptr_t)handle;
	shm->width = EYE_W;
	shm->height = EYE_H;
	shm->dxgiFormat = DXGI_FORMAT_R8G8B8A8_TYPELESS;
	shm->swapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	shm->flags = QVD_FLAG_SRGB_FORMAT;
	shm->writerPid = GetCurrentProcessId();
	shm->frameCount = 1;
	strcpy_s(shm->appName, "render test");
	strcpy_s(shm->runtimeName, "none");

	// ------------------------------------------------ libobs
	base_set_log_handler(LogHandler, nullptr);
	CHECK(obs_startup("en-US", nullptr, nullptr), "obs_startup");
	obs_set_ui_task_handler(TestUiTaskHandler);
	CHECK(ResetVideo(1920, 1080), "obs_reset_video 1920x1080");
	// In OBS this DLL sits next to obs64.exe; this test exe lives elsewhere, so preload it.
	CHECK(LoadLibraryW(L"C:/Program Files/obs-studio/bin/64bit/obs-frontend-api.dll"), "obs-frontend-api.dll");
	SetEnvironmentVariableA("ARCHAN_VRCAPTURE_TEST_RECORDING", "1"); // the head motion recorder runs
	obs_module_t *mod = nullptr;
	int mr = obs_open_module(&mod, argv[1], argv[2]);
	CHECK(mr == MODULE_SUCCESS, "obs_open_module %d", mr);
	CHECK(obs_init_module(mod), "obs_init_module");

	obs_data_t *initial = obs_data_create();
	obs_data_set_string(initial, "resolution", "1920x1080");
	obs_source_t *src = obs_source_create("archan_vr_capture", "Archan VR Capture", initial, nullptr);
	obs_data_release(initial);
	CHECK(src, "source create");
	{ // the defaults a new source gets (user decisions: smoothness 100, crop 15 %, delay 0.20 s)
		obs_data_t *def = obs_get_source_defaults("archan_vr_capture");
		const long long sm = obs_data_get_int(def, "stab_smooth");
		const double cr = obs_data_get_double(def, "stab_crop"), dl = obs_data_get_double(def, "stab_delay");
		const bool on = obs_data_get_bool(def, "stab_enable");
		obs_data_release(def);
		printf("defaults: stabilization %s, smoothness %lld, crop %.1f %%, delay %.2f s\n", on ? "on" : "off", sm, cr, dl);
		CHECK(on && sm == 100 && cr == 15.0 && dl == 0.20, "defaults are not on / 100 / 15 / 0.20");
	}

	// rename: new id + display name; the old ids still load (old scenes) but are hidden (deprecated)
	const char *shown = obs_source_get_display_name("archan_vr_capture");
	printf("source 'archan_vr_capture' display name: '%s'\n", shown ? shown : "(null)");
	CHECK(shown && strcmp(shown, "Archan VR Capture") == 0, "display name is not 'Archan VR Capture'");
	CHECK(!(obs_get_source_output_flags("archan_vr_capture") & OBS_SOURCE_DEPRECATED), "new id must not be hidden");
	for (const char *oldId : {"quest_vd_vr_picture", "vr_capture"}) {
		const uint32_t oldFlags = obs_get_source_output_flags(oldId);
		const char *oldName = obs_source_get_display_name(oldId);
		printf("old id '%s' flags 0x%X (deprecated bit %s), name '%s'\n", oldId, oldFlags,
		       (oldFlags & OBS_SOURCE_DEPRECATED) ? "set" : "NOT set", oldName ? oldName : "(null)");
		CHECK((oldFlags & OBS_SOURCE_DEPRECATED) && (oldFlags & OBS_SOURCE_VIDEO), "old id %s not deprecated", oldId);
		obs_source_t *oldSrc = obs_source_create(oldId, "old scene item", nullptr, nullptr);
		CHECK(oldSrc, "old id %s can no longer be created (old scenes would break)", oldId);
		obs_source_release(oldSrc);
	}

	obs_data_t *defaults = obs_get_source_defaults("archan_vr_capture");
	printf("default resolution mode: '%s'\n", obs_data_get_string(defaults, "resolution"));
	CHECK(strcmp(obs_data_get_string(defaults, "resolution"), "hmd") == 0, "default is not HMD res");
	obs_data_release(defaults);

	obs_properties_t *props = obs_source_properties(src);
	obs_property_t *list = obs_properties_get(props, "resolution");
	CHECK(list && obs_property_list_item_count(list) == 3, "picker must have exactly 3 choices");
	for (size_t i = 0; i < obs_property_list_item_count(list); i++)
		printf("picker choice %zu: '%s' = '%s'\n", i, obs_property_list_item_name(list, i),
		       obs_property_list_item_string(list, i));
	CHECK(strcmp(obs_property_list_item_string(list, 0), "hmd") == 0 &&
		      strcmp(obs_property_list_item_string(list, 1), "2560x1440") == 0 &&
		      strcmp(obs_property_list_item_string(list, 2), "1920x1080") == 0,
	      "picker values");
	CHECK(!obs_properties_get(props, "apply"), "Apply button should be gone");
	obs_properties_destroy(props);

	const QvdRect c = qvd_center_crop_16x9(EYE_W, EYE_H);
	printf("crop of %ux%u eye: x=%u y=%u w=%u h=%u\n", EYE_W, EYE_H, c.x, c.y, c.w, c.h);
	CHECK(c.w == 2496 && c.h == 1404 && c.x == 0 && c.y == 642, "crop");

	// ------------------------------------------------ phase A: 1920x1080
	std::vector<uint8_t> out;
	CHECK(Render(src, 1920, 1080, out), "render A");
	CHECK(shm->readerTickMs != 0, "plugin did not write the heartbeat");
	int maxErr = 0;
	uint64_t bad = 0;
	for (uint32_t v = 0; v < 1080; v++) {
		for (uint32_t u = 0; u < 1920; u++) {
			const double sx = c.x + (u + 0.5) * c.w / 1920.0 - 0.5;
			const double sy = c.y + (v + 0.5) * c.h / 1080.0 - 0.5;
			const double er = std::fmin(std::fmax(sx, 0.0), EYE_W - 1.0) * 255.0 / (EYE_W - 1);
			const double eg = std::fmin(std::fmax(sy, 0.0), EYE_H - 1.0) * 255.0 / (EYE_H - 1);
			const uint8_t *p = &out[((size_t)v * 1920 + u) * 4];
			const int e = std::max(std::max((int)std::lround(std::fabs(p[0] - er)),
							(int)std::lround(std::fabs(p[1] - eg))),
					       std::max(std::abs((int)p[2] - 128), std::abs((int)p[3] - 255)));
			maxErr = std::max(maxErr, e);
			if (e > 2 && bad++ < 5)
				printf("  A mismatch at %u,%u got %u,%u,%u,%u\n", u, v, p[0], p[1], p[2], p[3]);
		}
	}
	printf("phase A (1920x1080): max error %d, %llu pixels off by more than 2, size requests %zu\n", maxErr,
	       (unsigned long long)bad, g_requests.size());
	CHECK(bad == 0, "phase A picture");
	CHECK(g_requests.empty(), "1920 mode at 1920x1080 must not ask for a change");

	// ------------------------------------------------ phase A2: 1440p (user request 2026-11-06)
	{
		SetMode(src, "2560x1440");
		CHECK(Render(src, 1920, 1080, out), "render A2a");
		CHECK(g_requests.size() == 1 && g_requests[0].cx == 2560 && g_requests[0].cy == 1440,
		      "1440p mode asked for %zu sizes (%ux%u)", g_requests.size(), g_requests.empty() ? 0 : g_requests[0].cx,
		      g_requests.empty() ? 0 : g_requests[0].cy);
		CHECK(ResetVideo(2560, 1440), "obs_reset_video 1440p");
		g_requests.clear();
		CHECK(Render(src, 2560, 1440, out), "render A2b");
		int maxErr2 = 0;
		uint64_t bad2 = 0;
		for (uint32_t v = 0; v < 1440; v++)
			for (uint32_t u = 0; u < 2560; u++) {
				const double sx = c.x + (u + 0.5) * c.w / 2560.0 - 0.5;
				const double sy = c.y + (v + 0.5) * c.h / 1440.0 - 0.5;
				const double er = std::fmin(std::fmax(sx, 0.0), EYE_W - 1.0) * 255.0 / (EYE_W - 1);
				const double eg = std::fmin(std::fmax(sy, 0.0), EYE_H - 1.0) * 255.0 / (EYE_H - 1);
				const uint8_t *p = &out[((size_t)v * 2560 + u) * 4];
				const int e = std::max(std::max((int)std::lround(std::fabs(p[0] - er)),
								(int)std::lround(std::fabs(p[1] - eg))),
						       std::max(std::abs((int)p[2] - 128), std::abs((int)p[3] - 255)));
				maxErr2 = std::max(maxErr2, e);
				bad2 += e > 2;
			}
		printf("phase A2 (1440p): asked OBS for 2560x1440; picture max error %d, %llu pixels off by more than 2, "
		       "further requests %zu\n",
		       maxErr2, (unsigned long long)bad2, g_requests.size());
		CHECK(bad2 == 0, "phase A2 picture");
		CHECK(g_requests.empty(), "1440p mode at 2560x1440 must not ask again");
		SetMode(src, "1920x1080");
		CHECK(ResetVideo(1920, 1080), "obs_reset_video back to 1080p");
		CHECK(Render(src, 1920, 1080, out), "render A2c");
		g_requests.clear();
	}

	// ------------------------------------------------ phase B: HMD res
	SetMode(src, "hmd");
	CHECK(Render(src, 1920, 1080, out), "render B1");
	CHECK(g_requests.size() == 1, "HMD mode made %zu requests", g_requests.size());
	printf("phase B: HMD mode asked OBS for %ux%u\n", g_requests[0].cx, g_requests[0].cy);
	CHECK(g_requests[0].cx == 2496 && g_requests[0].cy == 1404, "wrong HMD size");

	// apply it the way the OBS frontend does, with a new unique picture
	CHECK(ResetVideo(g_requests[0].cx, g_requests[0].cy), "obs_reset_video HMD");
	for (uint32_t y = 0; y < EYE_H; y++)
		for (uint32_t x = 0; x < EYE_W; x++)
			px[(size_t)y * EYE_W + x] = Unique(x, y);
	CHECK(km->AcquireSync(0, 1000) == S_OK, "acquire");
	ctx->UpdateSubresource(tex, 0, nullptr, px.data(), EYE_W * 4, 0);
	km->ReleaseSync(0);
	ctx->Flush();
	shm->frameCount = 2;

	CHECK(obs_source_get_width(src) == 2496 && obs_source_get_height(src) == 1404, "source size follows canvas");
	g_requests.clear();
	CHECK(Render(src, 2496, 1404, out), "render B2");
	maxErr = 0;
	bad = 0;
	for (uint32_t v = 0; v < 1404; v++) {
		for (uint32_t u = 0; u < 2496; u++) {
			const uint32_t e = Unique(c.x + u, c.y + v);
			const uint8_t *p = &out[((size_t)v * 2496 + u) * 4];
			int d = 0;
			for (int ch = 0; ch < 4; ch++)
				d = std::max(d, std::abs((int)p[ch] - (int)((e >> (8 * ch)) & 255u)));
			maxErr = std::max(maxErr, d);
			if (d > 1 && bad++ < 5)
				printf("  B mismatch at %u,%u got %u,%u,%u,%u expected %08X\n", u, v, p[0], p[1], p[2],
				       p[3], e);
		}
	}
	printf("phase B (2496x1404, 1:1): max error %d, %llu pixels off by more than 1, further requests %zu\n",
	       maxErr, (unsigned long long)bad, g_requests.size());
	CHECK(bad == 0, "phase B picture is not the eye 1:1");
	CHECK(g_requests.empty(), "HMD mode at the right size must not ask again");

	stab::Q lastQaC = {0, 0, 0, 1}; // last pose sent in phase C (checked in the recorder file)
	int framesC = 0;
	// ------------------------------------------------ phase C: look-ahead stabilization, real time
	// (phases A/B had no pose in the channel -> they also proved the plain fallback)
	// The plugin maps OBS's render clock onto the game clock, so this phase runs in real time:
	// 30 game frames per second for 6 s, a world-fixed scene seen with 0.5 deg hand-like shake.
	{
		const uint32_t W = 512, H = 550; // centre 16:9 crop 512x288
		const double DEG = 3.14159265358979 / 180.0;
		const double TL = std::tan(-49 * DEG), TR = std::tan(45 * DEG), TU = std::tan(48 * DEG), TD = std::tan(-50 * DEG);
		const QvdRect cc = qvd_center_crop_16x9(W, H);
		D3D11_TEXTURE2D_DESC td2 = td;
		td2.Width = W;
		td2.Height = H;
		ID3D11Texture2D *tex2 = nullptr;
		CHECK(SUCCEEDED(dev->CreateTexture2D(&td2, nullptr, &tex2)), "phase C texture");
		IDXGIResource *res2 = nullptr;
		HANDLE h2 = nullptr;
		tex2->QueryInterface(__uuidof(IDXGIResource), (void **)&res2);
		CHECK(res2 && SUCCEEDED(res2->GetSharedHandle(&h2)), "phase C handle");
		IDXGIKeyedMutex *km2 = nullptr;
		CHECK(SUCCEEDED(tex2->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&km2)), "phase C mutex");

		CHECK(ResetVideo(cc.w, cc.h), "obs_reset_video phase C");
		const double CROP = 10.0, DELAY = 0.5;
		obs_data_t *sd = obs_data_create();
		obs_data_set_string(sd, "resolution", "hmd");
		obs_data_set_bool(sd, "stab_enable", true);
		obs_data_set_int(sd, "stab_smooth", 100);
		obs_data_set_bool(sd, "stab_horizon", false);
		obs_data_set_double(sd, "stab_crop", CROP);
		obs_data_set_double(sd, "stab_delay", DELAY);
		obs_source_update(src, sd);
		obs_data_release(sd);
		Sleep(300); // video sources apply updates on their next tick

		shm->seq = shm->seq + 1;
		shm->sharedHandle = (uint32_t)(uintptr_t)h2;
		shm->width = W;
		shm->height = H;
		shm->fovTan[0] = (float)TL;
		shm->fovTan[1] = (float)TR;
		shm->fovTan[2] = (float)TU;
		shm->fovTan[3] = (float)TD;
		shm->up[0] = 0;
		shm->up[1] = 1;
		shm->up[2] = 0;
		shm->seq = shm->seq + 1;

		auto shake = [&](double t, int a) {
			const double f[3][2] = {{6.3, 9.7}, {7.1, 11.0}, {8.2, 6.6}};
			return (0.30 * std::sin(2 * 3.14159265 * f[a][0] * t + a) + 0.20 * std::sin(2 * 3.14159265 * f[a][1] * t + 2 * a)) * DEG;
		};
		const stab::Q base = stab::Exp({0, 0.2, 0});
		std::vector<uint32_t> eye((size_t)W * H);
		std::vector<std::vector<uint8_t>> outs;
		std::vector<std::vector<uint32_t>> plains;
		g_requests.clear();
		const double gameT0 = 100.0; // game clock differs from OBS's clock (the plugin maps it)
		LARGE_INTEGER qf, q0, qn;
		QueryPerformanceFrequency(&qf);
		QueryPerformanceCounter(&q0);
		for (int f = 0; f < 330; f++) { // 11 s: also one cost line (every 10 s)
			const double t = f / 30.0;
			for (;;) { // real time: frame f at t seconds
				QueryPerformanceCounter(&qn);
				if ((double)(qn.QuadPart - q0.QuadPart) / qf.QuadPart >= t)
					break;
				Sleep(1);
			}
			const stab::Q qa = stab::Mul(base, stab::Mul(stab::Mul(stab::Exp({0, shake(t, 0), 0}), stab::Exp({shake(t, 1), 0, 0})),
								    stab::Exp({0, 0, shake(t, 2)})));
			lastQaC = qa;
			framesC++;
			for (uint32_t y = 0; y < H; y++)
				for (uint32_t x = 0; x < W; x++) {
					const stab::V3 ray = stab::Normalize({TL + (x + 0.5) / W * (TR - TL), TU - (y + 0.5) / H * (TU - TD), -1.0});
					eye[(size_t)y * W + x] = Scene(stab::Rotate(qa, ray));
				}
			CHECK(km2->AcquireSync(0, 1000) == S_OK, "phase C acquire");
			ctx->UpdateSubresource(tex2, 0, nullptr, eye.data(), W * 4, 0);
			shm->displayTimeNs = (int64_t)((gameT0 + t) * 1e9);
			shm->poseOrient[0] = (float)qa.x;
			shm->poseOrient[1] = (float)qa.y;
			shm->poseOrient[2] = (float)qa.z;
			shm->poseOrient[3] = (float)qa.w;
			shm->poseFlags = QVD_POSE_VALID | QVD_POSE_UP;
			km2->ReleaseSync(0);
			ctx->Flush();
			shm->frameCount = shm->frameCount + 1;
			CHECK(Render(src, cc.w, cc.h, out), "render C");
			if (t >= 2.0) { // delay buffer full and the path settled
				outs.push_back(out);
				std::vector<uint32_t> plain((size_t)cc.w * cc.h);
				for (uint32_t y = 0; y < cc.h; y++)
					memcpy(&plain[(size_t)y * cc.w], &eye[(size_t)(cc.y + y) * W + cc.x], cc.w * 4);
				plains.push_back(plain);
			}
		}
		CHECK(g_requests.empty(), "phase C asked for a size change (%zu)", g_requests.size());
		CHECK(!g_costLine.empty(), "no cost line from the plugin after 11 s");
		printf("phase C cost line: %s\n", g_costLine.c_str());

		// steadiness: mean colour change between consecutive frames, stabilized vs plain crop
		double dStab = 0, dPlain = 0;
		size_t n = 0;
		for (size_t i = 1; i < outs.size(); i++)
			for (size_t p = 0; p < (size_t)cc.w * cc.h; p++)
				for (int ch = 0; ch < 3; ch++) {
					dStab += std::abs((int)outs[i][p * 4 + ch] - (int)outs[i - 1][p * 4 + ch]);
					dPlain += std::abs((int)((plains[i][p] >> (8 * ch)) & 255u) - (int)((plains[i - 1][p] >> (8 * ch)) & 255u));
					n++;
				}
		dStab /= n;
		dPlain /= n;

		// correctness: every stabilized frame vs an independent CPU rendering of the steady camera (base)
		const double k = 1.0 - CROP / 100.0;
		const double Sx = TR - TL, Sy = TU - TD;
		const double cu = (cc.x + cc.w * 0.5) / W, cv = (cc.y + cc.h * 0.5) / H;
		std::vector<uint32_t> ref((size_t)cc.w * cc.h);
		for (uint32_t v = 0; v < cc.h; v++)
			for (uint32_t u = 0; u < cc.w; u++) {
				const double iu = cu + ((u + 0.5) / cc.w - 0.5) * (double)cc.w / W * k;
				const double iv = cv + ((v + 0.5) / cc.h - 0.5) * (double)cc.h / H * k;
				ref[(size_t)v * cc.w + u] = Scene(stab::Rotate(base, stab::Normalize({TL + iu * Sx, TU - iv * Sy, -1.0})));
			}
		double worstMean = 0;
		int worstMax = 0;
		for (const std::vector<uint8_t> &o : outs) {
			double err = 0;
			int mx = 0;
			for (size_t p = 0; p < ref.size(); p++)
				for (int ch = 0; ch < 3; ch++) {
					const int d = std::abs((int)o[p * 4 + ch] - (int)((ref[p] >> (8 * ch)) & 255u));
					err += d;
					mx = std::max(mx, d);
				}
			err /= (double)ref.size() * 3;
			if (err > 0.6)
				printf("phase C frame %zu of %zu: mean error %.2f, max %d\n", (size_t)(&o - &outs[0]), outs.size(), err, mx);
			worstMean = std::max(worstMean, err);
			worstMax = std::max(worstMax, mx);
		}
		printf("phase C (look-ahead, real time, %ux%u, crop %.0f %%, delay %.1f s, 0.5 deg shake): frame-to-frame change "
		       "stabilized %.2f vs plain %.2f (%.0fx steadier); %zu stabilized frames vs steady-camera reference: worst "
		       "mean error %.2f, worst max %d\n",
		       cc.w, cc.h, CROP, DELAY, dStab, dPlain, dPlain / std::max(dStab, 1e-9), outs.size(), worstMean, worstMax);
		printf("phase C delay: shown frame %.3f s, newest frame %.3f s -> video %.3f s behind (setting %.2f s)\n", g_shownT,
		       g_newestT, g_newestT - g_shownT, DELAY);
		CHECK(g_shownT > 0 && std::fabs((g_newestT - g_shownT) - DELAY) < 0.1, "shown frame is not 'delay' old");
		CHECK(dStab * 10.0 < dPlain, "stabilized picture not at least 10x steadier");
		CHECK(worstMean < 2.0, "stabilized picture does not match the steady camera view");
		km2->Release();
		res2->Release();
		tex2->Release();
	}

	// ------------------------------------------------ phase E: quad views, combined like the headset
	// (user decision 2026-11-06). Wide view 200x216, blue channel 0 (a marker); the sharp inner view
	// 440x400 over a narrower FOV, turned 2 deg, the full scene colour. The combined picture must be
	// 1720x1534 (the inner sharpness over the whole wide FOV), HMD res must ask for its 16:9 crop
	// 1696x954, and every output pixel must be the scene seen from the wide view's camera: blue from
	// the inner view inside its area (by an independent CPU calculation of that area), 0 outside.
	uint32_t canvasW = 512, canvasH = 288; // phase C's canvas
	auto quadPhase = [&](const char *name, uint32_t WI, uint32_t HI, double ITL, double ITR, double ITU, double ITD,
			     uint32_t reqW, uint32_t reqH, double CW, double CH, double cx0, double cy0) -> int {
		const uint32_t WM = 200, HM = 216;
		const double DEG = 3.14159265358979 / 180.0;
		const double TL = std::tan(-49 * DEG), TR = std::tan(45 * DEG), TU = std::tan(48 * DEG), TD = std::tan(-50 * DEG);
		const stab::Q qm = stab::Exp({0, 0.2, 0});
		const stab::Q qi = stab::Mul(qm, stab::Exp({0, 2 * DEG, 0}));
		auto makeTex = [&](uint32_t w, uint32_t h, ID3D11Texture2D **t, IDXGIKeyedMutex **k, uint32_t *handle) {
			D3D11_TEXTURE2D_DESC d = td;
			d.Width = w;
			d.Height = h;
			if (FAILED(dev->CreateTexture2D(&d, nullptr, t)))
				return false;
			IDXGIResource *r = nullptr;
			HANDLE hh = nullptr;
			(*t)->QueryInterface(__uuidof(IDXGIResource), (void **)&r);
			const bool ok = r && SUCCEEDED(r->GetSharedHandle(&hh)) &&
					SUCCEEDED((*t)->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)k));
			if (r)
				r->Release();
			*handle = (uint32_t)(uintptr_t)hh;
			return ok;
		};
		ID3D11Texture2D *texM = nullptr, *texI = nullptr;
		IDXGIKeyedMutex *kmM = nullptr, *kmI = nullptr;
		uint32_t hM = 0, hI = 0;
		CHECK(makeTex(WM, HM, &texM, &kmM, &hM) && makeTex(WI, HI, &texI, &kmI, &hI), "phase E textures");
		auto ray = [](double tl, double tr, double tu, double tdn, double u, double v) {
			return stab::Normalize({tl + u * (tr - tl), tu - v * (tu - tdn), -1.0});
		};
		std::vector<uint32_t> pm((size_t)WM * HM), pi((size_t)WI * HI);
		for (uint32_t y = 0; y < HM; y++)
			for (uint32_t x = 0; x < WM; x++)
				pm[(size_t)y * WM + x] =
					Scene(stab::Rotate(qm, ray(TL, TR, TU, TD, (x + 0.5) / WM, (y + 0.5) / HM))) & 0xFF00FFFFu; // B = 0
		for (uint32_t y = 0; y < HI; y++)
			for (uint32_t x = 0; x < WI; x++)
				pi[(size_t)y * WI + x] = Scene(stab::Rotate(qi, ray(ITL, ITR, ITU, ITD, (x + 0.5) / WI, (y + 0.5) / HI)));

		obs_data_t *sd = obs_data_create();
		obs_data_set_string(sd, "resolution", "hmd");
		obs_data_set_bool(sd, "stab_enable", false);
		obs_source_update(src, sd);
		obs_data_release(sd);
		Sleep(300);

		shm->seq = shm->seq + 1;
		shm->sharedHandle = hM;
		shm->width = WM;
		shm->height = HM;
		shm->innerHandle = hI;
		shm->innerWidth = WI;
		shm->innerHeight = HI;
		shm->innerDxgiFormat = shm->dxgiFormat;
		shm->seq = shm->seq + 1;
		auto sendFrame = [&]() {
			if (kmM->AcquireSync(0, 1000) != S_OK)
				return false;
			ctx->UpdateSubresource(texM, 0, nullptr, pm.data(), WM * 4, 0);
			shm->poseOrient[0] = (float)qm.x;
			shm->poseOrient[1] = (float)qm.y;
			shm->poseOrient[2] = (float)qm.z;
			shm->poseOrient[3] = (float)qm.w;
			shm->fovTan[0] = (float)TL;
			shm->fovTan[1] = (float)TR;
			shm->fovTan[2] = (float)TU;
			shm->fovTan[3] = (float)TD;
			shm->innerFovTan[0] = (float)ITL;
			shm->innerFovTan[1] = (float)ITR;
			shm->innerFovTan[2] = (float)ITU;
			shm->innerFovTan[3] = (float)ITD;
			shm->innerPoseOrient[0] = (float)qi.x;
			shm->innerPoseOrient[1] = (float)qi.y;
			shm->innerPoseOrient[2] = (float)qi.z;
			shm->innerPoseOrient[3] = (float)qi.w;
			shm->displayTimeNs = shm->displayTimeNs + 33333333;
			shm->poseFlags = QVD_POSE_VALID | QVD_POSE_UP | QVD_POSE_INNER;
			const bool ok = kmI->AcquireSync(0, 1000) == S_OK;
			if (ok) {
				ctx->UpdateSubresource(texI, 0, nullptr, pi.data(), WI * 4, 0);
				kmI->ReleaseSync(0);
			}
			kmM->ReleaseSync(0);
			ctx->Flush();
			shm->frameCount = shm->frameCount + 1;
			return ok;
		};
		g_requests.clear();
		CHECK(sendFrame(), "phase %s frame 1", name);
		CHECK(Render(src, canvasW, canvasH, out), "render %s1", name);
		CHECK(!g_requests.empty(), "phase %s: HMD mode asked for no size", name);
		printf("phase %s: quad views, HMD mode asked OBS for %ux%u (expected %ux%u)\n", name, g_requests.back().cx,
		       g_requests.back().cy, reqW, reqH);
		CHECK(g_requests.back().cx == reqW && g_requests.back().cy == reqH, "phase %s: wrong size", name);
		CHECK(ResetVideo(reqW, reqH), "obs_reset_video phase %s", name);
		canvasW = reqW;
		canvasH = reqH;
		CHECK(sendFrame(), "phase %s frame 2", name);
		CHECK(Render(src, reqW, reqH, out), "render %s2", name);
		framesC += 2; // the head motion recorder also writes these two frames' poses
		lastQaC = qm;

		// independent check of every output pixel (output = combined pixels cx0.., cy0.. of CW x CH)
		const stab::Mat3 M = stab::MulM(stab::Transpose(stab::RotM(qi)), stab::RotM(qm));
		uint64_t inside = 0, outside = 0, badIn = 0, badOut = 0;
		int maxIn = 0, maxOutRG = 0, maxOutB = 0;
		for (uint32_t v = 0; v < reqH; v++)
			for (uint32_t u = 0; u < reqW; u++) {
				const double cu = (cx0 + u + 0.5) / CW, cv = (cy0 + v + 0.5) / CH;
				const stab::V3 d = {TL + cu * (TR - TL), TU - cv * (TU - TD), -1.0};
				const stab::V3 di = {M.m[0][0] * d.x + M.m[0][1] * d.y + M.m[0][2] * d.z,
						     M.m[1][0] * d.x + M.m[1][1] * d.y + M.m[1][2] * d.z,
						     M.m[2][0] * d.x + M.m[2][1] * d.y + M.m[2][2] * d.z};
				const double ux = (di.x / -di.z - ITL) / (ITR - ITL), uy = (ITU - di.y / -di.z) / (ITU - ITD);
				const double edge = std::min(std::min(ux, 1.0 - ux), std::min(uy, 1.0 - uy));
				const uint32_t e = Scene(stab::Rotate(qm, stab::Normalize(d)));
				const uint8_t *p = &out[((size_t)v * reqW + u) * 4];
				const int dr = std::abs((int)p[0] - (int)(e & 255u)), dg = std::abs((int)p[1] - (int)((e >> 8) & 255u));
				const int db = std::abs((int)p[2] - (int)((e >> 16) & 255u));
				if (edge > 0.03) { // the inner view, past its blend band
					inside++;
					const int m = std::max(dr, std::max(dg, db));
					maxIn = std::max(maxIn, m);
					badIn += m > 6;
				} else if (edge < -0.01) { // the wide view
					outside++;
					maxOutRG = std::max(maxOutRG, std::max(dr, dg));
					maxOutB = std::max(maxOutB, (int)p[2]);
					badOut += std::max(dr, dg) > 12 || p[2] > 2;
				}
			}
		printf("phase %s: %llu pixels inside the inner view: max error %d, %llu off by more than 6; %llu outside: max "
		       "red/green error %d, max blue %d, %llu off\n",
		       name, (unsigned long long)inside, maxIn, (unsigned long long)badIn, (unsigned long long)outside, maxOutRG,
		       maxOutB, (unsigned long long)badOut);
		CHECK(inside > 50000 && outside > 100000, "phase %s: area split looks wrong", name);
		CHECK(badIn == 0, "phase %s: inner view area does not match the scene", name);
		CHECK(badOut == 0, "phase %s: wide view area does not match the scene", name);
		// back to one view for phase D
		shm->seq = shm->seq + 1;
		shm->innerHandle = 0;
		shm->innerWidth = shm->innerHeight = 0;
		shm->seq = shm->seq + 1;
		kmM->Release();
		kmI->Release();
		texM->Release();
		texI->Release();
		return 0;
	};
	// E: combined 1720x1534 (440 px over 0.55 tan, carried over 2.15 tan), HMD asks for its crop 1696x954
	if (quadPhase("E", 440, 400, -0.30, 0.25, 0.28, -0.32, 1696, 954, 1720, 1534, 12, 290))
		return 1;
	// F: the 4096 cap (user decision 2026-11-06). Full sharpness would be 9462x8373; the 8192 limit makes
	// it 8192x7248 and the HMD cap 4096x3624, whose 16:9 crop is 4096x2304 at 0,660.
	if (quadPhase("F", 880, 800, -0.10, 0.10, 0.10, -0.12, 4096, 2304, 4096, 3624, 0, 660))
		return 1;

	// ------------------------------------------------ phase D: audio Sync Offset follows the video delay
	// (user decision 2026-11-05: all audio sources; own offset + delay; off / removed -> own offset back)
	{
		struct obs_source_info ai = {};
		ai.id = "archan_test_audio";
		ai.type = OBS_SOURCE_TYPE_INPUT;
		ai.output_flags = OBS_SOURCE_AUDIO;
		ai.get_name = [](void *) -> const char * { return "test audio"; };
		ai.create = [](obs_data_t *, obs_source_t *) -> void * { return (void *)1; };
		ai.destroy = [](void *) {};
		obs_register_source(&ai);
		typedef void (*SaveLoadFn)(obs_data_t *, bool);
		SaveLoadFn saveLoad =
			(SaveLoadFn)GetProcAddress(GetModuleHandleW(L"archan-vr-capture.dll"), "archan_vr_capture_test_audio_saveload");
		CHECK(saveLoad, "test hook archan_vr_capture_test_audio_saveload");
		auto ms = [](obs_source_t *a) { return (long long)std::llround((double)obs_source_get_sync_offset(a) / 1e6); };
		auto setStab = [&](bool on, double delay) {
			obs_data_t *sd = obs_data_create();
			obs_data_set_bool(sd, "stab_enable", on);
			obs_data_set_double(sd, "stab_delay", delay);
			obs_source_update(src, sd);
			obs_data_release(sd);
			Sleep(1300); // video sources apply updates on their next tick; the plugin re-checks once a second
		};
		// live = in the Program output (user decision 2026-11-06: not live -> own offsets back)
		obs_set_output_source(0, src);
		Sleep(300);
		setStab(false, 0.5);
		obs_source_t *mic = obs_source_create("archan_test_audio", "Mic", nullptr, nullptr);
		obs_source_t *desk = obs_source_create("archan_test_audio", "Desktop", nullptr, nullptr);
		obs_source_set_sync_offset(mic, 50 * 1000000LL); // the user's own offset
		Sleep(1300);
		printf("phase D off: mic %lld ms, desktop %lld ms\n", ms(mic), ms(desk));
		CHECK(ms(mic) == 50 && ms(desk) == 0, "stabilization off changed an audio offset");
		setStab(true, 0.5);
		printf("phase D on, delay 0.5 s: mic %lld ms, desktop %lld ms\n", ms(mic), ms(desk));
		CHECK(ms(mic) == 550 && ms(desk) == 500, "on, delay 0.5 s: expected 550 / 500");
		setStab(true, 0.8);
		printf("phase D delay 0.8 s: mic %lld ms, desktop %lld ms\n", ms(mic), ms(desk));
		CHECK(ms(mic) == 850 && ms(desk) == 800, "delay 0.8 s: expected 850 / 800");
		obs_source_set_sync_offset(desk, 900 * 1000000LL); // the user types 900 -> own 100 + 800
		obs_source_t *game = obs_source_create("archan_test_audio", "Game", nullptr, nullptr); // added while on
		Sleep(1300);
		printf("phase D user typed 900 on desktop, new source game: mic %lld, desktop %lld, game %lld ms\n", ms(mic),
		       ms(desk), ms(game));
		CHECK(ms(mic) == 850 && ms(desk) == 900 && ms(game) == 800, "expected 850 / 900 / 800");
		// scene collection saved now (as at OBS exit, delay still applied) and loaded again
		obs_data_t *mod = obs_data_create();
		saveLoad(mod, true);
		obs_data_array_t *arr = obs_data_get_array(mod, "archan_vr_capture_audio_sync");
		const size_t saved = arr ? obs_data_array_count(arr) : 0;
		obs_data_array_release(arr);
		printf("phase D saved %zu own offsets with the scene collection\n", saved);
		CHECK(saved == 3, "expected 3 saved own offsets");
		saveLoad(mod, false); // load: the plugin forgets everything and takes the own offsets back from the save
		obs_data_release(mod);
		setStab(false, 0.8);
		printf("phase D off after save/load: mic %lld, desktop %lld, game %lld ms\n", ms(mic), ms(desk), ms(game));
		CHECK(ms(mic) == 50 && ms(desk) == 100 && ms(game) == 0, "off: expected own offsets 50 / 100 / 0");
		setStab(true, 1.0);
		CHECK(ms(mic) == 1050 && ms(desk) == 1100 && ms(game) == 1000, "on, 1.0 s: expected 1050 / 1100 / 1000");
		obs_set_output_source(0, nullptr); // left Program (another scene, or the eye icon off)
		Sleep(1300);
		printf("phase D not live: mic %lld, desktop %lld, game %lld ms\n", ms(mic), ms(desk), ms(game));
		CHECK(ms(mic) == 50 && ms(desk) == 100 && ms(game) == 0, "not live: expected own offsets 50 / 100 / 0");
		obs_set_output_source(0, src); // live again
		Sleep(1300);
		printf("phase D live again: mic %lld, desktop %lld, game %lld ms\n", ms(mic), ms(desk), ms(game));
		CHECK(ms(mic) == 1050 && ms(desk) == 1100 && ms(game) == 1000, "live again: expected 1050 / 1100 / 1000");
		obs_source_remove(src); // the VR source is removed from OBS
		Sleep(300);
		printf("phase D VR source removed: mic %lld, desktop %lld, game %lld ms\n", ms(mic), ms(desk), ms(game));
		CHECK(ms(mic) == 50 && ms(desk) == 100 && ms(game) == 0, "removed: expected own offsets 50 / 100 / 0");
		obs_set_output_source(0, nullptr);
		obs_source_release(mic);
		obs_source_release(desk);
		obs_source_release(game);
	}

	obs_source_release(src);
	obs_shutdown();

	// head motion recorder (acting as if OBS recorded): one row per new pose, last row = last pose sent
	{
		CHECK(!g_posePath.empty(), "pose recorder did not report a file");
		FILE *f = fopen(g_posePath.c_str(), "r");
		CHECK(f, "cannot open %s", g_posePath.c_str());
		char line[1024], last[1024] = "";
		unsigned long long rows = 0;
		bool headerOk = fgets(line, sizeof(line), f) && strncmp(line, "obs_time_ns,pose_time_ns,source,qx,qy,qz,qw", 43) == 0;
		while (fgets(line, sizeof(line), f)) {
			strcpy_s(last, line);
			rows++;
		}
		fclose(f);
		unsigned long long obsT = 0;
		long long poseT = 0;
		char src[16] = "";
		double q[4] = {};
		const int n = sscanf_s(last, "%llu,%lld,%15[^,],%lf,%lf,%lf,%lf", &obsT, &poseT, src, (unsigned)sizeof(src), &q[0],
				       &q[1], &q[2], &q[3]);
		printf("pose recorder: %s, %llu rows (plugin said %llu, phases C + E sent %d), last row %s q %.6f %.6f %.6f %.6f\n",
		       g_posePath.c_str(), rows, g_poseRows, framesC, src, q[0], q[1], q[2], q[3]);
		CHECK(headerOk, "recorder header");
		// + 1: the "back to one view" picture after phase F is rendered by OBS's own output in phase D
		// (the source is live there), so it is recorded too
		CHECK(rows == g_poseRows && rows == (unsigned long long)framesC + 1, "row count");
		CHECK(n == 7 && strcmp(src, "openxr") == 0, "last row parse");
		CHECK(std::fabs(q[0] - lastQaC.x) < 1e-6 && std::fabs(q[1] - lastQaC.y) < 1e-6 &&
			      std::fabs(q[2] - lastQaC.z) < 1e-6 && std::fabs(q[3] - lastQaC.w) < 1e-6,
		      "last recorded pose differs from the last pose sent");
		DeleteFileA(g_posePath.c_str()); // test file, not a real recording
	}
	printf("PASS\n");
	return 0;
}
