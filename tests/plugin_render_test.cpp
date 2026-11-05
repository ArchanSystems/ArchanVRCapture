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
#include <vector>

#include "../common/qvd_shared.h"

static const uint32_t EYE_W = 2496, EYE_H = 2688; // DCS on VDXR, layer.log 2026-11-05

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
	if (!GetEnvironmentVariableW(L"VRCAPTURE_TEST_CHANNEL", nullptr, 0))
		SetEnvironmentVariableW(L"VRCAPTURE_TEST_CHANNEL", L"Local\\VRCapture_TestChannel");
	setvbuf(stdout, nullptr, _IONBF, 0); // keep every line if something crashes
	if (argc < 3) {
		printf("usage: plugin_render_test <vr-capture.dll> <plugin data dir>\n");
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
	CHECK(obs_startup("en-US", nullptr, nullptr), "obs_startup");
	obs_set_ui_task_handler(TestUiTaskHandler);
	CHECK(ResetVideo(1920, 1080), "obs_reset_video 1920x1080");
	// In OBS this DLL sits next to obs64.exe; this test exe lives elsewhere, so preload it.
	CHECK(LoadLibraryW(L"C:/Program Files/obs-studio/bin/64bit/obs-frontend-api.dll"), "obs-frontend-api.dll");
	obs_module_t *mod = nullptr;
	int mr = obs_open_module(&mod, argv[1], argv[2]);
	CHECK(mr == MODULE_SUCCESS, "obs_open_module %d", mr);
	CHECK(obs_init_module(mod), "obs_init_module");

	obs_data_t *initial = obs_data_create();
	obs_data_set_string(initial, "resolution", "1920x1080");
	obs_source_t *src = obs_source_create("vr_capture", "VR Capture", initial, nullptr);
	obs_data_release(initial);
	CHECK(src, "source create");

	// rename: new id + display name; the old id still loads (old scenes) but is hidden (deprecated)
	const char *shown = obs_source_get_display_name("vr_capture");
	printf("source 'vr_capture' display name: '%s'\n", shown ? shown : "(null)");
	CHECK(shown && strcmp(shown, "VR Capture") == 0, "display name is not 'VR Capture'");
	const uint32_t oldFlags = obs_get_source_output_flags("quest_vd_vr_picture");
	printf("old id 'quest_vd_vr_picture' flags 0x%X (deprecated bit %s)\n", oldFlags,
	       (oldFlags & OBS_SOURCE_DEPRECATED) ? "set" : "NOT set");
	CHECK((oldFlags & OBS_SOURCE_DEPRECATED) && (oldFlags & OBS_SOURCE_VIDEO), "old id not registered as deprecated");
	obs_source_t *oldSrc = obs_source_create("quest_vd_vr_picture", "old scene item", nullptr, nullptr);
	CHECK(oldSrc, "old id can no longer be created (old scenes would break)");
	obs_source_release(oldSrc);

	obs_data_t *defaults = obs_get_source_defaults("vr_capture");
	printf("default resolution mode: '%s'\n", obs_data_get_string(defaults, "resolution"));
	CHECK(strcmp(obs_data_get_string(defaults, "resolution"), "hmd") == 0, "default is not HMD res");
	obs_data_release(defaults);

	obs_properties_t *props = obs_source_properties(src);
	obs_property_t *list = obs_properties_get(props, "resolution");
	CHECK(list && obs_property_list_item_count(list) == 2, "picker must have exactly 2 choices");
	for (size_t i = 0; i < obs_property_list_item_count(list); i++)
		printf("picker choice %zu: '%s' = '%s'\n", i, obs_property_list_item_name(list, i),
		       obs_property_list_item_string(list, i));
	CHECK(strcmp(obs_property_list_item_string(list, 0), "hmd") == 0 &&
		      strcmp(obs_property_list_item_string(list, 1), "1920x1080") == 0,
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

	obs_source_release(src);
	obs_shutdown();
	printf("PASS\n");
	return 0;
}
