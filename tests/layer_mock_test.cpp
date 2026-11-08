// Proof test for the OpenXR layer without a headset.
// Parent process: loads the real layer DLL, plays the OpenXR loader + a mock runtime,
// renders known pixel patterns into a D3D11 swapchain (2 array slices = 2 eyes,
// 3 images), and submits frames with a projection layer whose left view has an
// offset imageRect. Child process (--reader): plays OBS - opens the shared memory,
// keeps the heartbeat, opens the shared texture on its own D3D11 device and checks
// every pixel against the expected left eye / rect / last released image.
// Exit code 0 = pass.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <commctrl.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <dxgi.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_D3D12
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../common/qvd_shared.h"
#include "../common/log_cleanup.h"
#include "../common/log_zip.h"
#include "../common/overlay_key.h"

// ---------------------------------------------------------------- test geometry
static const uint32_t TEX_W = 2300, TEX_H = 2400; // swapchain texture
static const int32_t RECT_X = 100, RECT_Y = 50;   // left view imageRect offset
static const uint32_t RECT_W = 2064, RECT_H = 2208;
static const uint32_t NUM_IMAGES = 3, FRAMES = 10;

// pose the mock game sends with the left view (checked by the reader)
static const XrQuaternionf POSE_Q = {0.0f, 0.38268343f, 0.0f, 0.92387953f}; // 45 deg yaw
static const XrFovf POSE_FOV = {-0.80f, 0.70f, 0.75f, -0.85f};
static const XrQuaternionf SPACE_Q = {0.25881905f, 0.0f, 0.0f, 0.96592583f}; // LOCAL space rotated 30 deg about x
static const XrTime DISPLAY_TIME = 7000000000ll;
// world up in that space = rotate (0,1,0) by -30 deg about x = (0, cos 30, -sin 30)
static const float UP_EXPECTED[3] = {0.0f, 0.86602540f, -0.5f};

// --quad: quad views (foveated rendering). Views 2/3 = the sharp inner views; the inner left view is
// slice 1 of the swapchain at its own rect, with its own FOV and pose (checked by the reader).
static bool g_quad = false;
static bool g_right = false; // --right: the reader (OBS) asks for the right eye
static const int32_t INNER_X = 600, INNER_Y = 700;
static const uint32_t INNER_W = 900, INNER_H = 800;
static const XrFovf INNER_FOV = {-0.30f, 0.25f, 0.28f, -0.32f};
static const XrQuaternionf INNER_Q = {0.0f, 0.39874907f, 0.0f, 0.91706007f}; // 47 deg yaw

static uint32_t Expected(uint32_t imageIndex, uint32_t slice, uint32_t x, uint32_t y)
{
	// RGBA8 bytes: R = x, G = y, B = image/slice marker, A = 255
	const uint8_t r = (uint8_t)(x & 255), g = (uint8_t)(y & 255);
	const uint8_t b = (uint8_t)(10 + imageIndex * 40 + slice * 120);
	return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | (0xFFu << 24);
}

// ---------------------------------------------------------------- mock runtime
static ID3D11Device *g_dev = nullptr;
static ID3D11DeviceContext *g_ctx = nullptr;
static ID3D11Texture2D *g_images[NUM_IMAGES] = {};
static ID3D12Resource *g_images12[NUM_IMAGES] = {};
static bool g_useD3D12 = false;
static uint32_t g_nextAcquire = 0;
static int g_endFrames = 0;
static bool g_costTest = false; // --cost: also check the layer's cost log

// overlay test (--overlay): the layer's own panel swapchain, its own LOCAL space (fixed in the cockpit, user
// decision 2026-11-07; every LOCAL space after the game's first one), and what the runtime receives
static const XrSwapchain OVL_SC = (XrSwapchain)(uintptr_t)0x0E;
static const XrSpace PANEL_SPACE = (XrSpace)(uintptr_t)0x71E3; // the panel's space
static uint32_t g_localCreates = 0, g_viewTypeCreates = 0;
static const uint32_t OVL_IMAGES = 3;
static ID3D11Texture2D *g_ovlImages[OVL_IMAGES] = {};
static ID3D12Resource *g_ovlImages12[OVL_IMAGES] = {};
static uint32_t g_ovlNext = 0, g_ovlLast = 0, g_ovlCreates = 0, g_ovlAcquires = 0, g_ovlWaits = 0, g_ovlReleases = 0,
		g_ovlDestroyed = 0, g_viewCreates = 0, g_viewDestroyed = 0;
static XrSwapchainCreateInfo g_ovlCreate = {};
static uint32_t g_lastLayerCount = 0;
static const XrCompositionLayerBaseHeader *g_lastFirstLayer = nullptr;
static bool g_lastHasQuad = false;
static XrCompositionLayerQuad g_lastQuad = {};

static XrResult XRAPI_CALL mock_GetInstanceProperties(XrInstance, XrInstanceProperties *p)
{
	strcpy_s(p->runtimeName, "MockRuntime");
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL mock_DestroyInstance(XrInstance) { return XR_SUCCESS; }
static XrResult XRAPI_CALL mock_CreateSession(XrInstance, const XrSessionCreateInfo *, XrSession *s)
{
	*s = (XrSession)(uintptr_t)0x5E55;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL mock_DestroySession(XrSession) { return XR_SUCCESS; }
static XrResult XRAPI_CALL mock_CreateSwapchain(XrSession, const XrSwapchainCreateInfo *ci, XrSwapchain *sc)
{
	if (ci && ci->width == QVD_OVERLAY_W && ci->height == QVD_OVERLAY_H) {
		g_ovlCreate = *ci;
		g_ovlCreates++;
		*sc = OVL_SC;
		return XR_SUCCESS;
	}
	*sc = (XrSwapchain)(uintptr_t)0x5C;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL mock_DestroySwapchain(XrSwapchain sc)
{
	if (sc == OVL_SC)
		g_ovlDestroyed++;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL mock_EnumerateSwapchainFormats(XrSession, uint32_t cap, uint32_t *count, int64_t *formats)
{
	const int64_t f[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
	*count = 3;
	for (uint32_t i = 0; i < cap && i < 3; i++)
		formats[i] = f[i];
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL mock_WaitSwapchainImage(XrSwapchain sc, const XrSwapchainImageWaitInfo *)
{
	if (sc == OVL_SC)
		g_ovlWaits++;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL mock_GetSystemProperties(XrInstance, XrSystemId, XrSystemProperties *p)
{
	p->graphicsProperties.maxLayerCount = 16;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL mock_EnumerateSwapchainImages(XrSwapchain sc, uint32_t cap, uint32_t *count,
							  XrSwapchainImageBaseHeader *imgs)
{
	if (sc == OVL_SC) {
		*count = OVL_IMAGES;
		for (uint32_t i = 0; i < OVL_IMAGES && i < cap; i++) {
			if (g_useD3D12)
				reinterpret_cast<XrSwapchainImageD3D12KHR *>(imgs)[i].texture = g_ovlImages12[i];
			else
				reinterpret_cast<XrSwapchainImageD3D11KHR *>(imgs)[i].texture = g_ovlImages[i];
		}
		return XR_SUCCESS;
	}
	*count = NUM_IMAGES;
	if (cap == 0)
		return XR_SUCCESS;
	if (g_useD3D12) {
		auto *d = reinterpret_cast<XrSwapchainImageD3D12KHR *>(imgs);
		for (uint32_t i = 0; i < NUM_IMAGES && i < cap; i++)
			d[i].texture = g_images12[i];
		return XR_SUCCESS;
	}
	auto *d = reinterpret_cast<XrSwapchainImageD3D11KHR *>(imgs);
	for (uint32_t i = 0; i < NUM_IMAGES && i < cap; i++)
		d[i].texture = g_images[i];
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL mock_AcquireSwapchainImage(XrSwapchain sc, const XrSwapchainImageAcquireInfo *, uint32_t *idx)
{
	if (sc == OVL_SC) {
		*idx = g_ovlLast = g_ovlNext;
		g_ovlNext = (g_ovlNext + 1) % OVL_IMAGES;
		g_ovlAcquires++;
		return XR_SUCCESS;
	}
	*idx = g_nextAcquire;
	g_nextAcquire = (g_nextAcquire + 1) % NUM_IMAGES;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL mock_ReleaseSwapchainImage(XrSwapchain sc, const XrSwapchainImageReleaseInfo *)
{
	if (sc == OVL_SC)
		g_ovlReleases++;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL mock_EndFrame(XrSession, const XrFrameEndInfo *fei)
{
	g_endFrames++;
	g_lastLayerCount = fei ? fei->layerCount : 0;
	g_lastFirstLayer = fei && fei->layerCount ? fei->layers[0] : nullptr;
	g_lastHasQuad = false;
	for (uint32_t i = 0; fei && i < fei->layerCount; i++)
		if (fei->layers[i] && fei->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
			g_lastQuad = *reinterpret_cast<const XrCompositionLayerQuad *>(fei->layers[i]);
			g_lastHasQuad = i == fei->layerCount - 1; // on top of the game's layers
		}
	return XR_SUCCESS;
}
static int g_beginFrames = 0;
static XrResult XRAPI_CALL mock_BeginFrame(XrSession, const XrFrameBeginInfo *)
{
	g_beginFrames++;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL mock_CreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo *ci, XrSpace *s)
{
	if (ci && ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
		g_localCreates++;
	if (ci && ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW) { // the panel: locked to the view (2026-11-07)
		g_viewTypeCreates++;
		g_viewCreates++; // the panel's own space
		*s = PANEL_SPACE;
		return XR_SUCCESS;
	}
	*s = (XrSpace)(uintptr_t)0x5BACE;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL mock_DestroySpace(XrSpace sp)
{
	if (sp == PANEL_SPACE)
		g_viewDestroyed++;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL mock_GetInstanceProcAddr(XrInstance, const char *name, PFN_xrVoidFunction *fn)
{
#define M(x) if (strcmp(name, #x) == 0) { *fn = (PFN_xrVoidFunction)mock_##x; return XR_SUCCESS; }
	M(GetInstanceProperties) M(DestroyInstance) M(CreateSession) M(DestroySession) M(CreateSwapchain)
	M(DestroySwapchain) M(EnumerateSwapchainImages) M(AcquireSwapchainImage) M(ReleaseSwapchainImage) M(EndFrame) M(BeginFrame)
	M(CreateReferenceSpace) M(DestroySpace) M(EnumerateSwapchainFormats) M(WaitSwapchainImage) M(GetSystemProperties)
#undef M
	*fn = nullptr;
	return XR_ERROR_FUNCTION_UNSUPPORTED;
}
// The mock names lack the "xr" prefix: map "xrFoo" -> "Foo".
static XrResult XRAPI_CALL mock_GIPA(XrInstance i, const char *name, PFN_xrVoidFunction *fn)
{
	if (strncmp(name, "xr", 2) != 0) {
		*fn = nullptr;
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	}
	return mock_GetInstanceProcAddr(i, name + 2, fn);
}
static XrResult XRAPI_CALL mock_CreateApiLayerInstance(const XrInstanceCreateInfo *, const XrApiLayerCreateInfo *,
						       XrInstance *inst)
{
	*inst = (XrInstance)(uintptr_t)0x1257;
	return XR_SUCCESS;
}

#define CHECK(c, ...)                                         \
	do {                                                  \
		if (!(c)) {                                   \
			printf("FAIL line %d: ", __LINE__);  \
			printf(__VA_ARGS__);                  \
			printf("\n");                         \
			return 1;                             \
		}                                             \
	} while (0)

// ---------------------------------------------------------------- reader (plays OBS)
static int RunReader()
{
	HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\QVDTest_ReaderReady");
	HANDLE done = OpenEventW(SYNCHRONIZE, FALSE, L"Local\\QVDTest_FramesDone");
	CHECK(ready && done, "reader: events missing");

	QvdShared *shm = nullptr;
	HANDLE map = nullptr;
	for (int i = 0; i < 100 && !shm; i++) {
		map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, qvd_channel_name());
		if (map)
			shm = (QvdShared *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(QvdShared));
		if (!shm)
			Sleep(50);
	}
	CHECK(shm, "reader: shared memory not found");
	CHECK(shm->magic == QVD_MAGIC && shm->version == QVD_VERSION, "reader: bad magic/version");
	shm->wantEye = g_right ? 1u : 0u; // v5: the eye this "OBS source" shows
	shm->readerTickMs = GetTickCount64();
	SetEvent(ready);

	// keep heartbeat alive until the writer is done
	while (WaitForSingleObject(done, 20) == WAIT_TIMEOUT) {
		shm->wantEye = g_right ? 1u : 0u;
		shm->readerTickMs = GetTickCount64();
	}
	// right eye: view 1 (slice 1 in this swapchain), inner view 3 (slice 0); left: view 0 (slice 0), inner 2 (slice 1)
	const uint32_t mainSlice = g_right ? 1u : 0u, innerSlice = g_right ? 0u : 1u;
	printf("reader: asked for the %s eye, the picture is the %s eye\n", g_right ? "right" : "left",
	       shm->picEye ? "right" : "left");
	CHECK(shm->picEye == (g_right ? 1u : 0u), "picture eye %u", shm->picEye);

	printf("reader: app '%s' runtime '%s' handle 0x%08X %ux%u fmt %u swapfmt %u flags %u frames %llu\n",
	       shm->appName, shm->runtimeName, shm->sharedHandle, shm->width, shm->height, shm->dxgiFormat,
	       shm->swapchainFormat, shm->flags, (unsigned long long)shm->frameCount);
	CHECK(strcmp(shm->appName, "QVD mock app") == 0, "app name");
	CHECK(strcmp(shm->runtimeName, "MockRuntime") == 0, "runtime name");
	CHECK(shm->writerPid != GetCurrentProcessId() && shm->writerPid != 0, "writer pid");
	CHECK(shm->width == RECT_W && shm->height == RECT_H, "size %ux%u", shm->width, shm->height);
	CHECK(shm->dxgiFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS, "shared format %u", shm->dxgiFormat);
	CHECK(shm->swapchainFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "swapchain format");
	CHECK(shm->flags == QVD_FLAG_SRGB_FORMAT, "flags %u", shm->flags);
	CHECK(shm->frameCount == FRAMES, "frameCount %llu (expected %u)", (unsigned long long)shm->frameCount, FRAMES);

	// v2 pose for stabilization
	printf("reader: pose flags %u, q %.6f %.6f %.6f %.6f, fov tan %.6f %.6f %.6f %.6f, up %.6f %.6f %.6f, time %lld\n",
	       shm->poseFlags, shm->poseOrient[0], shm->poseOrient[1], shm->poseOrient[2], shm->poseOrient[3],
	       shm->fovTan[0], shm->fovTan[1], shm->fovTan[2], shm->fovTan[3], shm->up[0], shm->up[1], shm->up[2],
	       (long long)shm->displayTimeNs);
	CHECK(shm->poseFlags == (QVD_POSE_VALID | QVD_POSE_UP | (g_quad ? QVD_POSE_INNER : 0u)), "pose flags %u",
	      shm->poseFlags);
	if (!g_quad)
		CHECK(shm->innerHandle == 0 && shm->innerWidth == 0, "inner view published without quad views");
	const float eq[4] = {POSE_Q.x, POSE_Q.y, POSE_Q.z, POSE_Q.w};
	const float ef[4] = {tanf(POSE_FOV.angleLeft), tanf(POSE_FOV.angleRight), tanf(POSE_FOV.angleUp), tanf(POSE_FOV.angleDown)};
	for (int i = 0; i < 4; i++) {
		CHECK(fabsf(shm->poseOrient[i] - eq[i]) < 1e-6f, "pose orientation [%d]", i);
		CHECK(fabsf(shm->fovTan[i] - ef[i]) < 1e-6f, "fov tangent [%d]", i);
	}
	for (int i = 0; i < 3; i++)
		CHECK(fabsf(shm->up[i] - UP_EXPECTED[i]) < 1e-5f, "up [%d] %.6f expected %.6f", i, shm->up[i], UP_EXPECTED[i]);
	CHECK(shm->displayTimeNs == DISPLAY_TIME, "display time");

	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
					  &dev, nullptr, &ctx)),
	      "reader device");
	ID3D11Texture2D *shared = nullptr;
	CHECK(SUCCEEDED(dev->OpenSharedResource((HANDLE)(uintptr_t)shm->sharedHandle, __uuidof(ID3D11Texture2D),
						(void **)&shared)),
	      "OpenSharedResource");
	IDXGIKeyedMutex *km = nullptr;
	CHECK(SUCCEEDED(shared->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&km)), "keyed mutex");

	D3D11_TEXTURE2D_DESC d;
	shared->GetDesc(&d);
	d.Usage = D3D11_USAGE_STAGING;
	d.BindFlags = 0;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	d.MiscFlags = 0;
	d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	ID3D11Texture2D *staging = nullptr;
	CHECK(SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &staging)), "staging");
	CHECK(km->AcquireSync(0, 1000) == S_OK, "AcquireSync");
	ctx->CopyResource(staging, shared);
	km->ReleaseSync(0);
	D3D11_MAPPED_SUBRESOURCE m;
	CHECK(SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m)), "map");

	const uint32_t lastImage = (FRAMES - 1) % NUM_IMAGES; // last released image
	uint64_t bad = 0;
	for (uint32_t y = 0; y < RECT_H; y++) {
		const uint32_t *row = (const uint32_t *)((const uint8_t *)m.pData + (size_t)y * m.RowPitch);
		for (uint32_t x = 0; x < RECT_W; x++) {
			const uint32_t e = Expected(lastImage, mainSlice, RECT_X + x, RECT_Y + y);
			if (row[x] != e) {
				if (bad < 5)
					printf("  mismatch at %u,%u got %08X expected %08X\n", x, y, row[x], e);
				bad++;
			}
		}
	}
	ctx->Unmap(staging, 0);
	printf("reader: checked %u x %u pixels against the %s eye (slice %u), image %u, rect offset %d,%d: %llu mismatches\n",
	       RECT_W, RECT_H, g_right ? "right" : "left", mainSlice, lastImage, RECT_X, RECT_Y, (unsigned long long)bad);
	CHECK(bad == 0, "pixel mismatches");

	if (g_quad) { // the inner left view: its own texture, FOV and pose
		printf("reader: inner view handle 0x%08X %ux%u fmt %u, fov tan %.6f %.6f %.6f %.6f, q %.6f %.6f %.6f %.6f\n",
		       shm->innerHandle, shm->innerWidth, shm->innerHeight, shm->innerDxgiFormat, shm->innerFovTan[0],
		       shm->innerFovTan[1], shm->innerFovTan[2], shm->innerFovTan[3], shm->innerPoseOrient[0],
		       shm->innerPoseOrient[1], shm->innerPoseOrient[2], shm->innerPoseOrient[3]);
		CHECK(shm->innerHandle != 0 && shm->innerWidth == INNER_W && shm->innerHeight == INNER_H &&
			      shm->innerDxgiFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS,
		      "inner descriptor");
		const float iq[4] = {INNER_Q.x, INNER_Q.y, INNER_Q.z, INNER_Q.w};
		const float itn[4] = {tanf(INNER_FOV.angleLeft), tanf(INNER_FOV.angleRight), tanf(INNER_FOV.angleUp),
				      tanf(INNER_FOV.angleDown)};
		for (int i = 0; i < 4; i++) {
			CHECK(fabsf(shm->innerPoseOrient[i] - iq[i]) < 1e-6f, "inner orientation [%d]", i);
			CHECK(fabsf(shm->innerFovTan[i] - itn[i]) < 1e-6f, "inner fov tangent [%d]", i);
		}
		ID3D11Texture2D *inner = nullptr;
		CHECK(SUCCEEDED(dev->OpenSharedResource((HANDLE)(uintptr_t)shm->innerHandle, __uuidof(ID3D11Texture2D),
							(void **)&inner)),
		      "OpenSharedResource inner");
		IDXGIKeyedMutex *ikm = nullptr;
		CHECK(SUCCEEDED(inner->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&ikm)), "inner keyed mutex");
		D3D11_TEXTURE2D_DESC d2;
		inner->GetDesc(&d2);
		d2.Usage = D3D11_USAGE_STAGING;
		d2.BindFlags = 0;
		d2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		d2.MiscFlags = 0;
		d2.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		ID3D11Texture2D *staging2 = nullptr;
		CHECK(SUCCEEDED(dev->CreateTexture2D(&d2, nullptr, &staging2)), "inner staging");
		CHECK(km->AcquireSync(0, 1000) == S_OK && ikm->AcquireSync(0, 1000) == S_OK, "AcquireSync main + inner");
		ctx->CopyResource(staging2, inner);
		ikm->ReleaseSync(0);
		km->ReleaseSync(0);
		D3D11_MAPPED_SUBRESOURCE m2;
		CHECK(SUCCEEDED(ctx->Map(staging2, 0, D3D11_MAP_READ, 0, &m2)), "map inner");
		uint64_t bad2 = 0;
		for (uint32_t y = 0; y < INNER_H; y++) {
			const uint32_t *row = (const uint32_t *)((const uint8_t *)m2.pData + (size_t)y * m2.RowPitch);
			for (uint32_t x = 0; x < INNER_W; x++) {
				const uint32_t e = Expected(lastImage, innerSlice, INNER_X + x, INNER_Y + y);
				if (row[x] != e) {
					if (bad2 < 5)
						printf("  inner mismatch at %u,%u got %08X expected %08X\n", x, y, row[x], e);
					bad2++;
				}
			}
		}
		ctx->Unmap(staging2, 0);
		printf("reader: checked %u x %u pixels against the inner %s view (slice %u), rect offset %d,%d: %llu "
		       "mismatches\n",
		       INNER_W, INNER_H, g_right ? "right" : "left", innerSlice, INNER_X, INNER_Y, (unsigned long long)bad2);
		CHECK(bad2 == 0, "inner pixel mismatches");
	}
	return 0;
}

// ---------------------------------------------------------------- writer (plays the game)
static std::string LayerLogLine(const char *needle);
static int RunWriter(const char *exePath, const char *layerPath)
{
	CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
					  &g_dev, nullptr, &g_ctx)),
	      "device");

	// swapchain images: TYPELESS array of 2 slices (left, right), like many runtimes
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = TEX_W;
	td.Height = TEX_H;
	td.MipLevels = 1;
	td.ArraySize = 2;
	td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	std::vector<uint32_t> px((size_t)TEX_W * TEX_H);
	for (uint32_t i = 0; i < NUM_IMAGES; i++) {
		CHECK(SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_images[i])), "swapchain image");
		for (uint32_t slice = 0; slice < 2; slice++) {
			for (uint32_t y = 0; y < TEX_H; y++)
				for (uint32_t x = 0; x < TEX_W; x++)
					px[(size_t)y * TEX_W + x] = Expected(i, slice, x, y);
			g_ctx->UpdateSubresource(g_images[i], D3D11CalcSubresource(0, slice, 1), nullptr, px.data(),
						 TEX_W * 4, 0);
		}
	}

	// Never touch a live channel (a real game or OBS using it).
	if (HANDLE live = OpenFileMappingW(FILE_MAP_READ, FALSE, qvd_channel_name())) {
		QvdShared *v = (QvdShared *)MapViewOfFile(live, FILE_MAP_READ, 0, 0, sizeof(QvdShared));
		printf("NOT RUN: picture channel is in use (writer pid %u, app '%s'). Close the game and OBS, then rerun.\n",
		       v ? v->writerPid : 0, v ? v->appName : "?");
		return 3;
	}

	// 0) a fake other game (live process) owns the channel before our layer starts
	std::string idleCmd = std::string("\"") + exePath + "\" --idle";
	STARTUPINFOA isi = {sizeof(isi)};
	PROCESS_INFORMATION ipi;
	CHECK(CreateProcessA(nullptr, idleCmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &isi, &ipi), "spawn idle");
	HANDLE ownMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(QvdShared), qvd_channel_name());
	QvdShared *view = ownMap ? (QvdShared *)MapViewOfFile(ownMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(QvdShared)) : nullptr;
	CHECK(view, "test mapping");
	view->magic = QVD_MAGIC;
	view->version = QVD_VERSION;
	view->seq = 2;
	view->sharedHandle = 0x1234;
	view->width = 111;
	view->height = 222;
	view->writerPid = ipi.dwProcessId;
	strcpy_s(view->appName, "other game");

	HMODULE layer = LoadLibraryA(layerPath);
	CHECK(layer, "LoadLibrary(%s) error %lu", layerPath, GetLastError());
	auto negotiate = (PFN_xrNegotiateLoaderApiLayerInterface)GetProcAddress(layer, "xrNegotiateLoaderApiLayerInterface");
	CHECK(negotiate, "xrNegotiateLoaderApiLayerInterface not exported");

	XrNegotiateLoaderInfo li = {XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION,
				    sizeof(li), 1, 1, XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 0x3ff, 0xfff)};
	XrNegotiateApiLayerRequest rq = {XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST,
					 XR_API_LAYER_INFO_STRUCT_VERSION, sizeof(rq)};
	CHECK(negotiate(&li, "XR_APILAYER_ARCHAN_vr_capture", &rq) == XR_SUCCESS, "negotiate");
	{ // game start: the layer cleans the log folder and says so in layer.log
		wchar_t lp[MAX_PATH];
		ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\ArchanVRCapture\\logs\\layer.log", lp, MAX_PATH);
		FILE *lf = _wfopen(lp, L"r");
		CHECK(lf, "open layer.log");
		char tag[64], line[2048], found[2048] = "";
		sprintf_s(tag, "[pid %lu ", GetCurrentProcessId());
		while (fgets(line, sizeof(line), lf))
			if (strstr(line, tag) && strstr(line, "log cleanup"))
				strcpy_s(found, line);
		fclose(lf);
		CHECK(found[0], "layer load wrote no 'log cleanup' line");
		printf("writer: layer load -> %s", strstr(found, "log cleanup"));
	}

	XrApiLayerNextInfo next = {XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO, XR_API_LAYER_NEXT_INFO_STRUCT_VERSION,
				   sizeof(next)};
	strcpy_s(next.layerName, "XR_APILAYER_ARCHAN_vr_capture");
	next.nextGetInstanceProcAddr = mock_GIPA;
	next.nextCreateApiLayerInstance = mock_CreateApiLayerInstance;
	XrApiLayerCreateInfo lci = {XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO,
				    XR_API_LAYER_CREATE_INFO_STRUCT_VERSION, sizeof(lci)};
	lci.nextInfo = &next;
	XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy_s(ici.applicationInfo.applicationName, "QVD mock app");
	XrInstance inst = XR_NULL_HANDLE;
	CHECK(rq.createApiLayerInstance(&ici, &lci, &inst) == XR_SUCCESS, "create instance");

#define GET(fn, var)                                                                                       \
	PFN_##fn var = nullptr;                                                                            \
	CHECK(rq.getInstanceProcAddr(inst, #fn, (PFN_xrVoidFunction *)&var) == XR_SUCCESS && var, #fn);
	GET(xrCreateSession, CreateSession)
	GET(xrCreateSwapchain, CreateSwapchain)
	GET(xrEnumerateSwapchainImages, EnumerateSwapchainImages)
	GET(xrAcquireSwapchainImage, AcquireSwapchainImage)
	GET(xrReleaseSwapchainImage, ReleaseSwapchainImage)
	GET(xrEndFrame, EndFrame)
	GET(xrBeginFrame, BeginFrame)
	GET(xrDestroySwapchain, DestroySwapchain)
	GET(xrDestroySession, DestroySession)
	GET(xrDestroyInstance, DestroyInstance)
	GET(xrCreateReferenceSpace, CreateReferenceSpace)
#undef GET
	typedef void (*PFN_TestHandAtEnd)(int);
	auto testHandAtEnd = (PFN_TestHandAtEnd)GetProcAddress(layer, "archan_vr_capture_test_hand_at_end");
	CHECK(testHandAtEnd, "layer export archan_vr_capture_test_hand_at_end");

	XrGraphicsBindingD3D11KHR gb = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
	gb.device = g_dev;
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO, &gb};
	XrSession session;
	CHECK(CreateSession(inst, &sci, &session) == XR_SUCCESS, "session");
	XrReferenceSpaceCreateInfo rci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rci.poseInReferenceSpace = {SPACE_Q, {0, 0, 0}};
	XrSpace space = XR_NULL_HANDLE;
	CHECK(CreateReferenceSpace(session, &rci, &space) == XR_SUCCESS, "reference space");

	XrSwapchainCreateInfo swci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	swci.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	swci.width = TEX_W;
	swci.height = TEX_H;
	swci.arraySize = 2;
	swci.sampleCount = 1;
	swci.mipCount = 1;
	swci.faceCount = 1;
	XrSwapchain sc;
	CHECK(CreateSwapchain(session, &swci, &sc) == XR_SUCCESS, "swapchain");
	uint32_t n = 0;
	CHECK(EnumerateSwapchainImages(sc, 0, &n, nullptr) == XR_SUCCESS && n == NUM_IMAGES, "enum count");
	XrSwapchainImageD3D11KHR imgs[NUM_IMAGES];
	for (auto &i : imgs)
		i = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
	CHECK(EnumerateSwapchainImages(sc, n, &n, (XrSwapchainImageBaseHeader *)imgs) == XR_SUCCESS, "enum");

	// 1) without a reader the layer must not copy anything
	// One game frame: acquire/release, xrEndFrame, then (like xrWaitFrame) waitMs, then the next xrBeginFrame on
	// this thread - where the layer gives a D3D11 picture to OBS.
	int waitMs = 0;
	bool beginElsewhere = false;
	auto EndFrameOnly = [&]() -> XrResult {
		uint32_t idx;
		AcquireSwapchainImage(sc, nullptr, &idx);
		ReleaseSwapchainImage(sc, nullptr);
		XrCompositionLayerProjectionView views[4] = {
			{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
			{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
		for (uint32_t v = 0; v < 2; v++) {
			views[v].subImage.swapchain = sc;
			views[v].subImage.imageRect = {{RECT_X, RECT_Y}, {(int32_t)RECT_W, (int32_t)RECT_H}};
			views[v].subImage.imageArrayIndex = v;
			views[v].pose = {POSE_Q, {0, 0, 0}};
			views[v].fov = POSE_FOV;
		}
		for (uint32_t v = 2; v < 4; v++) { // the inner views (quad views)
			views[v].subImage.swapchain = sc;
			views[v].subImage.imageRect = {{INNER_X, INNER_Y}, {(int32_t)INNER_W, (int32_t)INNER_H}};
			views[v].subImage.imageArrayIndex = v == 2 ? 1u : 0u;
			views[v].pose = {INNER_Q, {0, 0, 0}};
			views[v].fov = INNER_FOV;
		}
		XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		proj.viewCount = g_quad ? 4 : 2;
		proj.space = space;
		proj.views = views;
		const XrCompositionLayerBaseHeader *layers[] = {(const XrCompositionLayerBaseHeader *)&proj};
		XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
		fei.displayTime = DISPLAY_TIME;
		fei.layerCount = 1;
		fei.layers = layers;
		return EndFrame(session, &fei);
	};
	auto submit = [&](void) -> XrResult {
		const XrResult er = EndFrameOnly();
		if (waitMs)
			Sleep(waitMs);
		XrResult br = XR_ERROR_RUNTIME_FAILURE;
		if (beginElsewhere) { // a game that begins its frames on another thread
			std::thread t([&]() { br = BeginFrame(session, nullptr); });
			t.join();
		} else {
			br = BeginFrame(session, nullptr);
		}
		return er != XR_SUCCESS ? er : br;
	};

	auto untouched = [&]() {
		return view->writerPid == ipi.dwProcessId && view->sharedHandle == 0x1234 && view->width == 111 &&
		       view->height == 222 && strcmp(view->appName, "other game") == 0 && view->frameCount == 0;
	};
	CHECK(untouched(), "layer overwrote a live owner at session start (pid %u handle 0x%X)", view->writerPid,
	      view->sharedHandle);
	for (int i = 0; i < 3; i++)
		CHECK(submit() == XR_SUCCESS, "endframe");
	CHECK(untouched(), "changed without a reader (frames %llu)", (unsigned long long)view->frameCount);
	printf("writer: live other owner + no reader -> channel untouched (correct)\n");

	int submits = 3;
	// a) OBS watching for 3 s, the live owner keeps delivering frames -> never taken
	for (uint64_t t0 = GetTickCount64(); GetTickCount64() - t0 < 3000;) {
		view->readerTickMs = GetTickCount64();
		view->writerTickMs = GetTickCount64(); // the other game copies a frame
		CHECK(submit() == XR_SUCCESS, "endframe");
		submits++;
		CHECK(untouched(), "took over from a delivering live owner (pid %u handle 0x%X)", view->writerPid,
		      view->sharedHandle);
		Sleep(50);
	}
	printf("writer: live owner delivering + OBS watching 3 s -> channel untouched (correct)\n");

	// b) the live owner stops delivering (like PimaxHome: session but no picture) -> taken after ~2 s,
	// counted from its last frame (not from here: writing the line above can take 150 ms)
	const uint64_t stopMs = view->writerTickMs;
	uint64_t takenMs = 0;
	while (GetTickCount64() - stopMs < 5000) {
		view->readerTickMs = GetTickCount64();
		CHECK(submit() == XR_SUCCESS, "endframe");
		submits++;
		if (view->writerPid == GetCurrentProcessId()) {
			takenMs = GetTickCount64() - stopMs;
			break;
		}
		Sleep(50);
	}
	CHECK(takenMs >= QVD_READER_TIMEOUT_MS - 100 && takenMs <= QVD_READER_TIMEOUT_MS + 1000,
	      "silent live owner: take-over after %llu ms (expected ~%llu ms)", (unsigned long long)takenMs,
	      (unsigned long long)QVD_READER_TIMEOUT_MS);
	CHECK(view->sharedHandle != 0 && view->sharedHandle != 0x1234 && view->width == RECT_W && view->height == RECT_H &&
		      view->frameCount == 1,
	      "after take-over from silent owner: handle 0x%X %ux%u frames %llu", view->sharedHandle, view->width,
	      view->height, (unsigned long long)view->frameCount);
	printf("writer: live owner silent -> layer took over after %llu ms and published its picture (correct)\n",
	       (unsigned long long)takenMs);

	// c) the owner process has ended -> taken on the next frame
	view->writerPid = ipi.dwProcessId;
	view->sharedHandle = 0x1234;
	view->width = 111;
	view->height = 222;
	view->frameCount = 0;
	strcpy_s(view->appName, "other game");
	TerminateProcess(ipi.hProcess, 0);
	WaitForSingleObject(ipi.hProcess, 5000);
	view->readerTickMs = GetTickCount64();
	CHECK(submit() == XR_SUCCESS, "endframe");
	submits++;
	CHECK(view->writerPid == GetCurrentProcessId() && view->sharedHandle != 0 && view->sharedHandle != 0x1234 &&
		      view->width == RECT_W && view->height == RECT_H && view->frameCount == 1,
	      "no take-over after owner ended (pid %u handle 0x%X %ux%u frames %llu)", view->writerPid,
	      view->sharedHandle, view->width, view->height, (unsigned long long)view->frameCount);
	printf("writer: owner ended -> layer took over and published its picture (correct)\n");

	// --cost: 12 s at ~90 Hz with OBS watching -> the layer must log its cost on the game thread
	if (g_costTest) {
		wchar_t lp[MAX_PATH];
		ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\ArchanVRCapture\\logs\\layer.log", lp, MAX_PATH);
		char tag[64];
		sprintf_s(tag, "[pid %lu ", GetCurrentProcessId());
		auto costLines = [&](char *last, size_t lastSize) {
			int count = 0;
			FILE *lf = _wfopen(lp, L"r");
			if (!lf)
				return -1;
			char line[2048];
			while (fgets(line, sizeof(line), lf))
				if (strstr(line, tag) && strstr(line, "cost on the game thread")) {
					count++;
					if (last)
						strcpy_s(last, lastSize, line);
				}
			fclose(lf);
			return count;
		};
		// game at ~90 Hz until two new cost lines are written (the second one = a full 10 s window)
		// GPU-bound game: every frame queues more GPU work than the frame time (like DCS at 47 fps)
		ID3D11Texture2D *load1 = nullptr, *load2 = nullptr;
		{
			D3D11_TEXTURE2D_DESC ld = {};
			ld.Width = 4000;
			ld.Height = 2250;
			ld.MipLevels = 1;
			ld.ArraySize = 1;
			ld.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			ld.SampleDesc.Count = 1;
			ld.Usage = D3D11_USAGE_DEFAULT;
			CHECK(SUCCEEDED(g_dev->CreateTexture2D(&ld, nullptr, &load1)) &&
				      SUCCEEDED(g_dev->CreateTexture2D(&ld, nullptr, &load2)),
			      "load textures");
		}
		bool gpuBound = false, watching = true;
		int smallOps = 0;     // small commands per frame, like a game's draw calls
		char lastCost[2048] = ""; // the last full cost line of runCost
		auto runCost = [&](const char *label) {
			const int before = costLines(nullptr, 0);
			CHECK(before >= 0, "open layer.log");
			char found[2048] = "";
			const uint64_t c0 = GetTickCount64();
			int costFrames = 0;
			while (GetTickCount64() - c0 < 40000 && costLines(found, sizeof(found)) < before + 2) {
				view->readerTickMs = watching ? GetTickCount64() : 0;
				if (gpuBound)
					for (int i = 0; i < 120; i++)
						g_ctx->CopyResource(load2, load1);
				for (int i = 0; i < smallOps; i++) {
					const UINT x = (UINT)(i % 60) * 64, y = (UINT)((i / 60) % 34) * 64;
					D3D11_BOX sb = {x, y, 0, x + 64, y + 64, 1};
					g_ctx->CopySubresourceRegion(load2, 0, (x + 64) % 3900, y, 0, load1, 0, &sb);
				}
				CHECK(submit() == XR_SUCCESS, "endframe");
				submits++;
				costFrames++;
				if (!gpuBound && !waitMs)
					Sleep(11); // ~90 Hz
			}
			CHECK(costLines(found, sizeof(found)) >= before + 2, "no full 'cost on the game thread' window in 40 s");
			printf("writer %s: %d frames -> layer.log: %s", label, costFrames, strstr(found, "cost on"));
			strcpy_s(lastCost, found);
			return 0;
		};
		if (runCost("(nobody reads the picture)"))
			return 1;
		// an OBS-like reader on its own device: every 33 ms take the picture, copy it, give it back, then
		// keep the GPU busy (OBS renders the canvas and feeds the encoder)
		std::atomic<bool> stopReader{false};
		std::atomic<int> readerCopies{0}, readerError{0};
		const uint32_t handle = view->sharedHandle;
		std::thread reader([&]() {
			ID3D11Device *d = nullptr;
			ID3D11DeviceContext *x = nullptr;
			ID3D11Texture2D *sh = nullptr, *own = nullptr, *b1 = nullptr, *b2 = nullptr;
			IDXGIKeyedMutex *km = nullptr;
			if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d,
						     nullptr, &x))) {
				readerError = 1;
				return;
			}
			if (FAILED(d->OpenSharedResource((HANDLE)(uintptr_t)handle, __uuidof(ID3D11Texture2D), (void **)&sh)) ||
			    FAILED(sh->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&km))) {
				readerError = 2;
			} else {
				D3D11_TEXTURE2D_DESC td;
				sh->GetDesc(&td);
				td.MiscFlags = 0;
				td.BindFlags = 0;
				D3D11_TEXTURE2D_DESC bd = td;
				bd.Width = 4000;
				bd.Height = 2250;
				if (FAILED(d->CreateTexture2D(&td, nullptr, &own)) || FAILED(d->CreateTexture2D(&bd, nullptr, &b1)) ||
				    FAILED(d->CreateTexture2D(&bd, nullptr, &b2))) {
					readerError = 3;
				} else {
					while (!stopReader) {
						if (km->AcquireSync(0, 0) == S_OK) {
							x->CopyResource(own, sh);
							km->ReleaseSync(0);
							readerCopies++;
						}
						for (int i = 0; i < 20; i++)
							x->CopyResource(b2, b1);
						x->Flush();
						Sleep(33);
					}
				}
			}
			if (b2) b2->Release();
			if (b1) b1->Release();
			if (own) own->Release();
			if (km) km->Release();
			if (sh) sh->Release();
			x->Release();
			d->Release();
		});
		const int rc = runCost("(OBS-like reader every 33 ms)");
		stopReader = true;
		reader.join();
		if (rc)
			return 1;
		CHECK(readerError == 0 && readerCopies > 0, "OBS-like reader: error %d, %d copies", readerError.load(),
		      readerCopies.load());
		printf("writer: OBS-like reader took the picture %d times\n", readerCopies.load());
		// "moved or added": the same GPU-bound game with and without the capture copying
		gpuBound = true;
		watching = false;
		if (runCost("(GPU-bound game, capture NOT copying)"))
			return 1;
		watching = true;
		if (runCost("(GPU-bound game, capture copying)"))
			return 1;
		gpuBound = false;
		// The D3D11 hand-over (2026-11-07): a game that queues many small commands per frame (DCS: AcquireSync at
		// xrEndFrame waited 2.7 ms per frame). 8 ms between xrEndFrame and xrBeginFrame like xrWaitFrame.
		// The old way (AcquireSync right after the frame) against the new one (spare at xrEndFrame, hand-over at
		// the next xrBeginFrame): the layer's time per copied frame must be at most a quarter.
		auto layerAvg = [](const char *line) {
			const char *p = strstr(line, "this layer avg ");
			return p ? atof(p + 15) : -1.0;
		};
		smallOps = 20000;
		waitMs = 8;
		testHandAtEnd(1);
		if (runCost("(20000 small commands per frame, given to OBS at xrEndFrame - the old way)"))
			return 1;
		const double oldUs = layerAvg(lastCost);
		CHECK(strstr(lastCost, "given to OBS at xrEndFrame") && oldUs > 0, "old way: cost line wrong: %s", lastCost);
		testHandAtEnd(0);
		if (runCost("(20000 small commands per frame, given to OBS at the next xrBeginFrame)"))
			return 1;
		const double newUs = layerAvg(lastCost);
		CHECK(strstr(lastCost, "given to OBS at xrBeginFrame") && strstr(lastCost, "copy into the spare ") && newUs > 0,
		      "new way: cost line wrong: %s", lastCost);
		CHECK(newUs * 4 <= oldUs, "hand-over at xrBeginFrame: %.0f us per frame, at xrEndFrame %.0f us - not a quarter",
		      newUs, oldUs);
		printf("writer: many small commands -> the layer takes %.0f us per frame on the game thread at xrBeginFrame, "
		       "%.0f us at xrEndFrame (the old way): %.1fx less (correct)\n",
		       newUs, oldUs, oldUs / newUs);
		smallOps = 0;
		waitMs = 0;
		load1->Release();
		load2->Release();
	}

	// back to a clean state for the reader phase
	view->readerTickMs = 0;
	view->frameCount = 0;
	// reset image rotation so the frame math below is simple
	g_nextAcquire = 0;

	// 2) start the reader process
	HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, L"Local\\QVDTest_ReaderReady");
	HANDLE done = CreateEventW(nullptr, TRUE, FALSE, L"Local\\QVDTest_FramesDone");
	std::string cmd = std::string("\"") + exePath + "\" --reader" + (g_quad ? " --quad" : "") + (g_right ? " --right" : "");
	STARTUPINFOA si = {sizeof(si)};
	PROCESS_INFORMATION pi;
	CHECK(CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi), "spawn reader");
	CHECK(WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0, "reader not ready");

	for (uint32_t f = 0; f < FRAMES; f++)
		CHECK(submit() == XR_SUCCESS, "endframe");
	g_ctx->Flush();
	printf("writer: %u frames submitted, layer copied %llu, runtime received %d EndFrame calls\n", FRAMES,
	       (unsigned long long)view->frameCount, g_endFrames);
	CHECK(g_endFrames == submits + (int)FRAMES, "EndFrame not forwarded every time (%d of %d)", g_endFrames,
	      submits + (int)FRAMES);
	SetEvent(done);

	WaitForSingleObject(pi.hProcess, 30000);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CHECK(code == 0, "reader failed (exit %lu)", code);

	// 3b) D3D11: the picture is given to OBS at the next xrBeginFrame on the thread that ended the frame
	CHECK(g_beginFrames == g_endFrames, "xrBeginFrame not forwarded every time (%d of %d)", g_beginFrames, g_endFrames);
	const std::string modeLine = LayerLogLine("session ready");
	CHECK(strstr(modeLine.c_str(), "picture given to OBS at the next xrBeginFrame"), "D3D11 session: %s",
	      modeLine.c_str());
	const std::string firstHand = LayerLogLine("first picture given to OBS at xrBeginFrame");
	CHECK(!firstHand.empty(), "layer.log has no 'first picture given to OBS at xrBeginFrame'");
	printf("writer: D3D11 picture given to OBS at xrBeginFrame; layer.log: %s", strstr(firstHand.c_str(), "first"));
	// a game that begins its frames on another thread: that frame is not given, then xrEndFrame gives them
	view->readerTickMs = GetTickCount64();
	const uint64_t fcBefore = view->frameCount;
	beginElsewhere = true;
	CHECK(submit() == XR_SUCCESS, "endframe (xrBeginFrame on another thread)");
	beginElsewhere = false;
	const uint64_t fcElsewhere = view->frameCount;
	const std::string otherLine = LayerLogLine("xrBeginFrame on thread");
	CHECK(fcElsewhere == fcBefore && strstr(otherLine.c_str(), "gives the picture to OBS at xrEndFrame from now on"),
	      "xrBeginFrame on another thread: frames %llu -> %llu, layer.log: %s", (unsigned long long)fcBefore,
	      (unsigned long long)fcElsewhere, otherLine.c_str());
	view->readerTickMs = GetTickCount64();
	CHECK(EndFrameOnly() == XR_SUCCESS, "endframe without xrBeginFrame");
	CHECK(view->frameCount == fcBefore + 1, "after the switch xrEndFrame did not give the picture (frames %llu -> %llu)",
	      (unsigned long long)fcBefore, (unsigned long long)view->frameCount);
	printf("writer: xrBeginFrame on another thread -> that frame skipped, then given at xrEndFrame (correct); "
	       "layer.log: %s",
	       strstr(otherLine.c_str(), "xrBeginFrame on"));

	DestroySwapchain(sc);
	DestroySession(session);
	CHECK(view->sharedHandle == 0 && view->writerPid == 0, "session end did not clear the descriptor");
	printf("writer: session end cleared the descriptor (correct)\n");
	DestroyInstance(inst);

	// 4) PimaxHome is skipped (user decision): with OBS watching it must never touch the channel
	strcpy_s(ici.applicationInfo.applicationName, "PimaxHome");
	CHECK(rq.createApiLayerInstance(&ici, &lci, &inst) == XR_SUCCESS, "create PimaxHome instance");
	CHECK(CreateSession(inst, &sci, &session) == XR_SUCCESS, "PimaxHome session");
	CHECK(CreateSwapchain(session, &swci, &sc) == XR_SUCCESS, "PimaxHome swapchain");
	n = NUM_IMAGES;
	CHECK(EnumerateSwapchainImages(sc, n, &n, (XrSwapchainImageBaseHeader *)imgs) == XR_SUCCESS, "PimaxHome enum");
	const uint64_t framesBefore = view->frameCount;
	const int endBefore = g_endFrames;
	for (int i = 0; i < 5; i++) {
		view->readerTickMs = GetTickCount64();
		CHECK(submit() == XR_SUCCESS, "PimaxHome endframe");
		Sleep(20);
	}
	CHECK(g_endFrames == endBefore + 5, "PimaxHome EndFrame not forwarded");
	CHECK(view->writerPid == 0 && view->sharedHandle == 0 && view->frameCount == framesBefore,
	      "PimaxHome was sent to OBS (pid %u handle 0x%X frames %llu)", view->writerPid, view->sharedHandle,
	      (unsigned long long)view->frameCount);
	printf("writer: app 'PimaxHome' with OBS watching -> skipped, channel untouched, frames still forwarded (correct)\n");
	DestroySwapchain(sc);
	DestroySession(session);
	DestroyInstance(inst);

	printf("PASS\n");
	return 0;
}

// ---------------------------------------------------------------- D3D12 writer (plays a D3D12 game, e.g. MSFS 2024)
static bool WaitQueueIdle(ID3D12Device *dev, ID3D12CommandQueue *q)
{
	ID3D12Fence *f = nullptr;
	if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&f)))
		return false;
	HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	q->Signal(f, 1);
	f->SetEventOnCompletion(1, ev);
	const bool ok = WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0;
	CloseHandle(ev);
	f->Release();
	return ok;
}

static int RunWriterD3D12(const char *exePath, const char *layerPath)
{
	if (HANDLE live = OpenFileMappingW(FILE_MAP_READ, FALSE, qvd_channel_name())) {
		QvdShared *v = (QvdShared *)MapViewOfFile(live, FILE_MAP_READ, 0, 0, sizeof(QvdShared));
		printf("NOT RUN: picture channel is in use (writer pid %u, app '%s'). Close the game and OBS, then rerun.\n",
		       v ? v->writerPid : 0, v ? v->appName : "?");
		return 3;
	}
	g_useD3D12 = true;
	ID3D12Device *dev12 = nullptr;
	CHECK(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&dev12)),
	      "D3D12 device");
	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	ID3D12CommandQueue *queue = nullptr;
	CHECK(SUCCEEDED(dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&queue)), "queue");

	// swapchain images like a D3D12 runtime: TYPELESS, 2 slices, render target, handed out in RENDER_TARGET
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width = TEX_W;
	rd.Height = TEX_H;
	rd.DepthOrArraySize = 2;
	rd.MipLevels = 1;
	rd.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
	rd.SampleDesc.Count = 1;
	rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	for (uint32_t i = 0; i < NUM_IMAGES; i++)
		CHECK(SUCCEEDED(dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
							       D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
							       __uuidof(ID3D12Resource), (void **)&g_images12[i])),
		      "D3D12 swapchain image %u", i);

	{ // fill the pattern through our own D3D11On12 device on the same queue
		ID3D11Device *d11 = nullptr;
		ID3D11DeviceContext *c11 = nullptr;
		IUnknown *qs[] = {queue};
		CHECK(SUCCEEDED(D3D11On12CreateDevice(dev12, 0, nullptr, 0, qs, 1, 0, &d11, &c11, nullptr)), "test 11on12");
		ID3D11On12Device *on12 = nullptr;
		CHECK(SUCCEEDED(d11->QueryInterface(__uuidof(ID3D11On12Device), (void **)&on12)), "test on12");
		std::vector<uint32_t> px((size_t)TEX_W * TEX_H);
		for (uint32_t i = 0; i < NUM_IMAGES; i++) {
			D3D11_RESOURCE_FLAGS fl = {};
			fl.BindFlags = D3D11_BIND_RENDER_TARGET;
			ID3D11Texture2D *t = nullptr;
			CHECK(SUCCEEDED(on12->CreateWrappedResource(g_images12[i], &fl, D3D12_RESOURCE_STATE_RENDER_TARGET,
								    D3D12_RESOURCE_STATE_RENDER_TARGET,
								    __uuidof(ID3D11Texture2D), (void **)&t)),
			      "test wrap");
			ID3D11Resource *r = t;
			on12->AcquireWrappedResources(&r, 1);
			for (uint32_t slice = 0; slice < 2; slice++) {
				for (uint32_t y = 0; y < TEX_H; y++)
					for (uint32_t x = 0; x < TEX_W; x++)
						px[(size_t)y * TEX_W + x] = Expected(i, slice, x, y);
				c11->UpdateSubresource(t, D3D11CalcSubresource(0, slice, 1), nullptr, px.data(), TEX_W * 4, 0);
			}
			on12->ReleaseWrappedResources(&r, 1);
			c11->Flush();
			t->Release();
		}
		CHECK(WaitQueueIdle(dev12, queue), "fill wait");
		on12->Release();
		c11->Release();
		d11->Release();
	}

	HMODULE layer = LoadLibraryA(layerPath);
	CHECK(layer, "LoadLibrary(%s) error %lu", layerPath, GetLastError());
	auto negotiate = (PFN_xrNegotiateLoaderApiLayerInterface)GetProcAddress(layer, "xrNegotiateLoaderApiLayerInterface");
	CHECK(negotiate, "xrNegotiateLoaderApiLayerInterface not exported");
	XrNegotiateLoaderInfo li = {XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION,
				    sizeof(li), 1, 1, XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 0x3ff, 0xfff)};
	XrNegotiateApiLayerRequest rq = {XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST,
					 XR_API_LAYER_INFO_STRUCT_VERSION, sizeof(rq)};
	CHECK(negotiate(&li, "XR_APILAYER_ARCHAN_vr_capture", &rq) == XR_SUCCESS, "negotiate");
	XrApiLayerNextInfo next = {XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO, XR_API_LAYER_NEXT_INFO_STRUCT_VERSION,
				   sizeof(next)};
	strcpy_s(next.layerName, "XR_APILAYER_ARCHAN_vr_capture");
	next.nextGetInstanceProcAddr = mock_GIPA;
	next.nextCreateApiLayerInstance = mock_CreateApiLayerInstance;
	XrApiLayerCreateInfo lci = {XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO,
				    XR_API_LAYER_CREATE_INFO_STRUCT_VERSION, sizeof(lci)};
	lci.nextInfo = &next;
	XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy_s(ici.applicationInfo.applicationName, "QVD mock app");
	XrInstance inst = XR_NULL_HANDLE;
	CHECK(rq.createApiLayerInstance(&ici, &lci, &inst) == XR_SUCCESS, "create instance");

#define GET(fn, var)                                                                                       \
	PFN_##fn var = nullptr;                                                                            \
	CHECK(rq.getInstanceProcAddr(inst, #fn, (PFN_xrVoidFunction *)&var) == XR_SUCCESS && var, #fn);
	GET(xrCreateSession, CreateSession)
	GET(xrCreateSwapchain, CreateSwapchain)
	GET(xrEnumerateSwapchainImages, EnumerateSwapchainImages)
	GET(xrAcquireSwapchainImage, AcquireSwapchainImage)
	GET(xrReleaseSwapchainImage, ReleaseSwapchainImage)
	GET(xrEndFrame, EndFrame)
	GET(xrBeginFrame, BeginFrame)
	GET(xrDestroySwapchain, DestroySwapchain)
	GET(xrDestroySession, DestroySession)
	GET(xrDestroyInstance, DestroyInstance)
	GET(xrCreateReferenceSpace, CreateReferenceSpace)
#undef GET

	XrGraphicsBindingD3D12KHR gb = {XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
	gb.device = dev12;
	gb.queue = queue;
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO, &gb};
	XrSession session;
	CHECK(CreateSession(inst, &sci, &session) == XR_SUCCESS, "session");
	XrReferenceSpaceCreateInfo rci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rci.poseInReferenceSpace = {SPACE_Q, {0, 0, 0}};
	XrSpace space = XR_NULL_HANDLE;
	CHECK(CreateReferenceSpace(session, &rci, &space) == XR_SUCCESS, "reference space");

	XrSwapchainCreateInfo swci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	swci.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	swci.width = TEX_W;
	swci.height = TEX_H;
	swci.arraySize = 2;
	swci.sampleCount = 1;
	swci.mipCount = 1;
	swci.faceCount = 1;
	XrSwapchain sc;
	CHECK(CreateSwapchain(session, &swci, &sc) == XR_SUCCESS, "swapchain");
	uint32_t n = 0;
	CHECK(EnumerateSwapchainImages(sc, 0, &n, nullptr) == XR_SUCCESS && n == NUM_IMAGES, "enum count");
	XrSwapchainImageD3D12KHR imgs[NUM_IMAGES];
	for (auto &i : imgs)
		i = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
	CHECK(EnumerateSwapchainImages(sc, n, &n, (XrSwapchainImageBaseHeader *)imgs) == XR_SUCCESS, "enum");

	HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, L"Local\\QVDTest_ReaderReady");
	HANDLE done = CreateEventW(nullptr, TRUE, FALSE, L"Local\\QVDTest_FramesDone");
	std::string cmd = std::string("\"") + exePath + "\" --reader" + (g_quad ? " --quad" : "") + (g_right ? " --right" : "");
	STARTUPINFOA si = {sizeof(si)};
	PROCESS_INFORMATION pi;
	CHECK(CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi), "spawn reader");
	CHECK(WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0, "reader not ready");

	for (uint32_t f = 0; f < FRAMES; f++) {
		uint32_t idx;
		AcquireSwapchainImage(sc, nullptr, &idx);
		ReleaseSwapchainImage(sc, nullptr);
		XrCompositionLayerProjectionView views[4] = {
			{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
			{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
		for (uint32_t v = 0; v < 2; v++) {
			views[v].subImage.swapchain = sc;
			views[v].subImage.imageRect = {{RECT_X, RECT_Y}, {(int32_t)RECT_W, (int32_t)RECT_H}};
			views[v].subImage.imageArrayIndex = v;
			views[v].pose = {POSE_Q, {0, 0, 0}};
			views[v].fov = POSE_FOV;
		}
		for (uint32_t v = 2; v < 4; v++) { // the inner views (quad views)
			views[v].subImage.swapchain = sc;
			views[v].subImage.imageRect = {{INNER_X, INNER_Y}, {(int32_t)INNER_W, (int32_t)INNER_H}};
			views[v].subImage.imageArrayIndex = v == 2 ? 1u : 0u;
			views[v].pose = {INNER_Q, {0, 0, 0}};
			views[v].fov = INNER_FOV;
		}
		XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		proj.viewCount = g_quad ? 4 : 2;
		proj.space = space;
		proj.views = views;
		const XrCompositionLayerBaseHeader *layers[] = {(const XrCompositionLayerBaseHeader *)&proj};
		XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
		fei.displayTime = DISPLAY_TIME;
		fei.layerCount = 1;
		fei.layers = layers;
		CHECK(EndFrame(session, &fei) == XR_SUCCESS, "endframe");
		CHECK(BeginFrame(session, nullptr) == XR_SUCCESS, "beginframe");
	}
	CHECK(WaitQueueIdle(dev12, queue), "copies did not finish on the game's queue");
	printf("writer (D3D12): %u frames submitted, runtime received %d EndFrame calls\n", FRAMES, g_endFrames);
	CHECK(g_endFrames == (int)FRAMES && g_beginFrames == (int)FRAMES, "EndFrame/BeginFrame not forwarded every time");
	{
		const std::string modeLine = LayerLogLine("session ready");
		CHECK(strstr(modeLine.c_str(), "D3D12 session ready") &&
			      strstr(modeLine.c_str(), "picture given to OBS at xrEndFrame"),
		      "D3D12 session: %s", modeLine.c_str());
		printf("writer (D3D12): picture given to OBS at xrEndFrame as before (correct)\n");
	}
	SetEvent(done);

	WaitForSingleObject(pi.hProcess, 30000);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);

	DestroySwapchain(sc);
	DestroySession(session);
	DestroyInstance(inst);
	CHECK(code == 0, "reader failed (exit %lu)", code);
	printf("PASS (D3D12)\n");
	return 0;
}

// ---------------------------------------------------------------- log cleanup (older than 48 h)
static int RunCleanupTest()
{
	wchar_t tmp[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp);
	wchar_t dirBuf[MAX_PATH];
	swprintf_s(dirBuf, L"%sarchan_log_cleanup_test_%lu", tmp, GetCurrentProcessId());
	const std::wstring dir = dirBuf;
	CreateDirectoryW(dir.c_str(), nullptr);
	FILETIME nowFt;
	GetSystemTimeAsFileTime(&nowFt);
	const unsigned long long now = qvd_ft64(nowFt), hour = 36000000000ull;
	auto ago = [&](double hours) {
		const unsigned long long v = now - (unsigned long long)(hours * hour);
		FILETIME f;
		f.dwLowDateTime = (DWORD)v;
		f.dwHighDateTime = (DWORD)(v >> 32);
		return f;
	};
	auto makeFile = [&](const wchar_t *name, double hoursAgo) {
		const std::wstring path = dir + L"\\" + name;
		HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		DWORD w;
		WriteFile(h, "x", 1, &w, nullptr);
		const FILETIME f = ago(hoursAgo);
		SetFileTime(h, &f, &f, &f);
		CloseHandle(h);
		return true;
	};
	auto stamp = [&](double hoursAgo) {
		FILETIME l;
		SYSTEMTIME s;
		const FILETIME u = ago(hoursAgo);
		FileTimeToLocalFileTime(&u, &l);
		FileTimeToSystemTime(&l, &s);
		char b[64];
		sprintf_s(b, "%04d-%02d-%02d %02d:%02d:%02d.%03d", s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond,
			  s.wMilliseconds);
		return std::string(b);
	};
	auto exists = [&](const wchar_t *name) { return GetFileAttributesW((dir + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES; };
	CHECK(makeFile(L"poses-3days.csv", 72) && makeFile(L"poses-49h.csv", 49) && makeFile(L"poses-47h.csv", 47) &&
		      makeFile(L"poses-1h.csv", 1),
	      "create test files");
	const std::string log = stamp(72) + " [pid 1 DCS.exe] A 3 days old\n" + "continuation line of A (no time stamp)\n" +
				stamp(49) + " [pid 1 DCS.exe] B 49 h old\n" + stamp(47) + " [pid 2 DCS.exe] C 47 h old\n" +
				"continuation line of C (no time stamp)\n" + stamp(1) + " [pid 3 DCS.exe] D 1 h old\n";
	{
		FILE *f = _wfopen((dir + L"\\layer.log").c_str(), L"wb");
		CHECK(f, "create layer.log");
		fwrite(log.data(), 1, log.size(), f);
		fclose(f);
		HANDLE h = CreateFileW((dir + L"\\layer.log").c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		const FILETIME old = ago(72); // the file itself is old too: it must be trimmed, never deleted
		SetFileTime(h, &old, &old, &old);
		CloseHandle(h);
	}
	const QvdLogCleanup c = qvd_cleanup_logs(dir, nowFt);
	printf("cleanup: %d files deleted, %d failed, layer.log %d lines removed, %d kept\n", c.filesDeleted, c.filesFailed,
	       c.linesRemoved, c.linesKept);
	CHECK(c.filesDeleted == 2 && c.filesFailed == 0, "expected 2 files deleted");
	CHECK(!exists(L"poses-3days.csv") && !exists(L"poses-49h.csv") && exists(L"poses-47h.csv") && exists(L"poses-1h.csv"),
	      "wrong files deleted");
	CHECK(exists(L"layer.log"), "layer.log was deleted (must only be trimmed)");
	std::string after;
	{
		FILE *f = _wfopen((dir + L"\\layer.log").c_str(), L"rb");
		CHECK(f, "open trimmed layer.log");
		char b[4096];
		size_t n;
		while ((n = fread(b, 1, sizeof(b), f)) > 0)
			after.append(b, n);
		fclose(f);
	}
	const std::string expect = stamp(47) + " [pid 2 DCS.exe] C 47 h old\n" + "continuation line of C (no time stamp)\n" +
				   stamp(1) + " [pid 3 DCS.exe] D 1 h old\n";
	CHECK(c.linesRemoved == 3 && c.linesKept == 3 && after == expect, "layer.log trimmed wrong:\n%s", after.c_str());
	const QvdLogCleanup c2 = qvd_cleanup_logs(dir, nowFt);
	CHECK(c2.filesDeleted == 0 && c2.linesRemoved == 0, "second cleanup changed something");
	printf("cleanup: 3-day and 49 h files deleted, 47 h and 1 h kept; layer.log lines A, B (+ A's continuation) "
	       "removed, C, D (+ C's continuation) kept; second run changes nothing\n");
	DeleteFileW((dir + L"\\poses-47h.csv").c_str());
	DeleteFileW((dir + L"\\poses-1h.csv").c_str());
	DeleteFileW((dir + L"\\layer.log").c_str());
	RemoveDirectoryW(dir.c_str());
	printf("cleanup PASS\n");
	return 0;
}

// "Save logs" (user decision 2026-11-08): the zip holds layer.log (even while a game holds it open for writing) and
// the OBS logs of the last 48 h, byte for byte; an older OBS log is left out. Proven by unpacking the zip with
// Windows' own tar.exe (it checks every CRC) and comparing the files.
static int RunLogZipTest()
{
	wchar_t tmp[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp);
	wchar_t base[MAX_PATH];
	swprintf_s(base, L"%sarchan_log_zip_test_%lu", tmp, GetCurrentProcessId());
	const std::wstring root = base, layerDir = root + L"\\layer", obsDir = root + L"\\obs", outDir = root + L"\\out",
			   crashDir = root + L"\\crashes";
	CreateDirectoryW(root.c_str(), nullptr);
	CreateDirectoryW(layerDir.c_str(), nullptr);
	CreateDirectoryW(obsDir.c_str(), nullptr);
	CreateDirectoryW(outDir.c_str(), nullptr);
	CreateDirectoryW(crashDir.c_str(), nullptr);
	FILETIME nowFt;
	GetSystemTimeAsFileTime(&nowFt);
	const unsigned long long now = qvd_ft64(nowFt), hour = 36000000000ull;
	auto write = [&](const std::wstring &path, const std::string &data, double hoursAgo) {
		HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		DWORD w;
		WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr);
		const unsigned long long v = now - (unsigned long long)(hoursAgo * hour);
		FILETIME f = {(DWORD)v, (DWORD)(v >> 32)};
		SetFileTime(h, &f, &f, &f);
		CloseHandle(h);
		return w == data.size();
	};
	std::string layerLog, obsNew, obsOld;
	for (int i = 0; i < 20000; i++)
		layerLog += "2026-11-08 11:18:43.976 [pid 14140 FlightSimulator2024.exe] line " + std::to_string(i) + "\n";
	for (int i = 0; i < 256 * 40; i++)
		obsNew += (char)(i & 0xFF); // every byte value
	obsOld = "old OBS log\n";
	const std::string crashNew = "Unhandled exception: c0000005\nFault address: 7FFB12345678 (test.exe)\n";
	CHECK(write(crashDir + L"\\Crash 2026-11-08 13-09-21.txt", crashNew, 2) &&
		      write(crashDir + L"\\Crash 2026-11-05 10-00-00.txt", "old crash", 72),
	      "create test crash reports");
	CHECK(write(layerDir + L"\\layer.log", layerLog, 0.1) && write(obsDir + L"\\2026-11-08 11-09-23.txt", obsNew, 1) &&
		      write(obsDir + L"\\2026-11-06 09-00-00.txt", obsOld, 49),
	      "create test logs");
	// a running game holds layer.log open for writing (the layer's own share mode)
	HANDLE held = CreateFileW((layerDir + L"\\layer.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
				  nullptr, OPEN_EXISTING, 0, nullptr);
	CHECK(held != INVALID_HANDLE_VALUE, "hold layer.log open");
	const std::wstring zip = root + L"\\logs.zip";
	const QvdLogZipResult r = qvd_save_logs_zip(layerDir, obsDir, zip, nowFt, 48.0, crashDir);
	CloseHandle(held);
	std::string names;
	for (const std::string &n : r.names)
		names += n + "; ";
	for (const std::string &n : r.skipped)
		names += "SKIPPED " + n + "; ";
	printf("log zip: ok %d, %d files, %llu bytes: %s%s\n", r.ok, r.files, r.bytes, names.c_str(), r.error.c_str());
	CHECK(r.ok && r.files == 3 && r.skipped.empty(), "zip not made with exactly 3 files");

	wchar_t sys[MAX_PATH];
	GetSystemDirectoryW(sys, MAX_PATH);
	std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + zip + L"\" -C \"" + outDir + L"\"";
	STARTUPINFOW si = {sizeof(si)};
	PROCESS_INFORMATION pi = {};
	std::vector<wchar_t> cl(cmd.begin(), cmd.end());
	cl.push_back(0);
	CHECK(CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi),
	      "start tar.exe");
	WaitForSingleObject(pi.hProcess, 30000);
	DWORD code = 99;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	printf("log zip: tar.exe -xf exit code %lu\n", code);
	CHECK(code == 0, "tar.exe could not unpack the zip (exit %lu)", code);
	std::string gotLayer, gotObs, gotOld;
	CHECK(qvd_read_shared(outDir + L"\\ArchanVRCapture\\layer.log", gotLayer) && gotLayer == layerLog,
	      "layer.log in the zip differs (%zu vs %zu bytes)", gotLayer.size(), layerLog.size());
	CHECK(qvd_read_shared(outDir + L"\\OBS\\2026-11-08 11-09-23.txt", gotObs) && gotObs == obsNew,
	      "new OBS log in the zip differs (%zu vs %zu bytes)", gotObs.size(), obsNew.size());
	CHECK(!qvd_read_shared(outDir + L"\\OBS\\2026-11-06 09-00-00.txt", gotOld), "49 h old OBS log was included");
	std::string gotCrash, gotOldCrash;
	CHECK(qvd_read_shared(outDir + L"\\OBS crashes\\Crash 2026-11-08 13-09-21.txt", gotCrash) && gotCrash == crashNew,
	      "OBS crash report in the zip differs");
	CHECK(!qvd_read_shared(outDir + L"\\OBS crashes\\Crash 2026-11-05 10-00-00.txt", gotOldCrash), "72 h old crash report was included");
	// OBS never crashed: no crashes folder is no error
	const QvdLogZipResult noCrashDir =
		qvd_save_logs_zip(layerDir, obsDir, root + L"\\nocrash.zip", nowFt, 48.0, root + L"\\no-crashes-folder");
	CHECK(noCrashDir.ok && noCrashDir.files == 2 && noCrashDir.skipped.empty(), "missing crashes folder handled wrong");
	DeleteFileW((root + L"\\nocrash.zip").c_str());
	printf("log zip: the 2 h old OBS crash report included as 'OBS crashes/...', the 72 h old one left out; no crashes "
	       "folder -> 2 files, no error\n");
	printf("log zip: layer.log %zu bytes (held open by a writer) and the 1 h old OBS log %zu bytes identical after "
	       "unpacking; the 49 h old OBS log left out\n",
	       gotLayer.size(), gotObs.size());

	const QvdLogZipResult none = qvd_save_logs_zip(root + L"\\missing", root + L"\\missing", root + L"\\none.zip", nowFt);
	CHECK(!none.ok && GetFileAttributesW((root + L"\\none.zip").c_str()) == INVALID_FILE_ATTRIBUTES,
	      "no logs: should fail without a file");
	printf("log zip: no logs -> '%s', no file written\n", none.error.c_str());

	DeleteFileW((outDir + L"\\ArchanVRCapture\\layer.log").c_str());
	RemoveDirectoryW((outDir + L"\\ArchanVRCapture").c_str());
	DeleteFileW((outDir + L"\\OBS\\2026-11-08 11-09-23.txt").c_str());
	RemoveDirectoryW((outDir + L"\\OBS").c_str());
	DeleteFileW((outDir + L"\\OBS crashes\\Crash 2026-11-08 13-09-21.txt").c_str());
	RemoveDirectoryW((outDir + L"\\OBS crashes").c_str());
	DeleteFileW((crashDir + L"\\Crash 2026-11-08 13-09-21.txt").c_str());
	DeleteFileW((crashDir + L"\\Crash 2026-11-05 10-00-00.txt").c_str());
	RemoveDirectoryW(crashDir.c_str());
	RemoveDirectoryW(outDir.c_str());
	DeleteFileW(zip.c_str());
	DeleteFileW((layerDir + L"\\layer.log").c_str());
	DeleteFileW((obsDir + L"\\2026-11-08 11-09-23.txt").c_str());
	DeleteFileW((obsDir + L"\\2026-11-06 09-00-00.txt").c_str());
	RemoveDirectoryW(layerDir.c_str());
	RemoveDirectoryW(obsDir.c_str());
	RemoveDirectoryW(root.c_str());
	printf("log zip PASS\n");
	return 0;
}

static int RunCropTest()
{
	struct {
		uint32_t w, h, x, y, cw, ch;
	} cases[] = {
		{2496, 2688, 0, 642, 2496, 1404}, // DCS on VDXR, measured 2026-11-05 (layer.log)
		{2064, 2208, 8, 528, 2048, 1152}, // width not a multiple of 32
		{1920, 1080, 0, 0, 1920, 1080},   // already 16:9
		{4000, 1000, 1120, 5, 1760, 990}, // wider than 16:9
		{1832, 1920, 4, 447, 1824, 1026},
	};
	for (auto &c : cases) {
		QvdRect r = qvd_center_crop_16x9(c.w, c.h);
		printf("crop %ux%u -> %u,%u %ux%u\n", c.w, c.h, r.x, r.y, r.w, r.h);
		CHECK(r.x == c.x && r.y == c.y && r.w == c.cw && r.h == c.ch, "crop %ux%u", c.w, c.h);
		CHECK((uint64_t)r.w * 9u == (uint64_t)r.h * 16u, "not exactly 16:9: %ux%u", r.w, r.h);
		CHECK(r.w % 4u == 0 && r.h % 2u == 0, "not OBS aligned: %ux%u", r.w, r.h);
	}
	printf("crop PASS\n");
	return 0;
}

// ---------------------------------------------------------------- performance overlay (user decision 2026-11-06)
// The mock game runs at exactly 50 fps with one extra thread spinning at 100 %. With the overlay on, the layer
// must: create its own 480x150 swapchain (8-bit sRGB) and a VIEW space, add one quad layer on top of the
// game's layer, upload a panel once a second, write the same panel to the shared memory for OBS, and measure
// FPS 50, 20 ms per frame, busiest thread ~100 %, CPU ~1 core. Off: the game's frame goes on unchanged.
// The key file (written by OBS) is read by the layer: its text is the key. Pressing the key itself is not
// simulated here (it would type into the desktop); archan_vr_capture_test_overlay() stands in for it.

// The last line of layer.log from this process that contains 'needle' ("" if none).
static std::string LayerLogLine(const char *needle)
{
	wchar_t lp[MAX_PATH];
	ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\ArchanVRCapture\\logs\\layer.log", lp, MAX_PATH);
	FILE *lf = _wfopen(lp, L"r");
	if (!lf)
		return "";
	char tag[64], line[4096];
	std::string found;
	sprintf_s(tag, "[pid %lu ", GetCurrentProcessId());
	while (fgets(line, sizeof(line), lf))
		if (strstr(line, tag) && strstr(line, needle))
			found = line;
	fclose(lf);
	return found;
}

static bool ReadOverlayImage(uint32_t idx, std::vector<uint8_t> &out, ID3D12Device *dev12, ID3D12CommandQueue *queue)
{
	ID3D11Device *d = g_dev;
	ID3D11DeviceContext *c = g_ctx;
	ID3D11On12Device *on12 = nullptr;
	ID3D11Texture2D *src = g_ovlImages[idx];
	if (dev12) { // our own D3D11On12 device on the game's queue, only to read the image back
		IUnknown *qs[] = {queue};
		if (FAILED(D3D11On12CreateDevice(dev12, 0, nullptr, 0, qs, 1, 0, &d, &c, nullptr)) ||
		    FAILED(d->QueryInterface(__uuidof(ID3D11On12Device), (void **)&on12)))
			return false;
		D3D11_RESOURCE_FLAGS fl = {};
		if (FAILED(on12->CreateWrappedResource(g_ovlImages12[idx], &fl, D3D12_RESOURCE_STATE_RENDER_TARGET,
						       D3D12_RESOURCE_STATE_RENDER_TARGET, __uuidof(ID3D11Texture2D),
						       (void **)&src)))
			return false;
	}
	D3D11_TEXTURE2D_DESC sd;
	src->GetDesc(&sd);
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	ID3D11Texture2D *stage = nullptr;
	bool ok = SUCCEEDED(d->CreateTexture2D(&sd, nullptr, &stage));
	if (ok) {
		ID3D11Resource *r = src;
		if (on12)
			on12->AcquireWrappedResources(&r, 1);
		c->CopyResource(stage, src);
		if (on12)
			on12->ReleaseWrappedResources(&r, 1);
		D3D11_MAPPED_SUBRESOURCE m;
		ok = SUCCEEDED(c->Map(stage, 0, D3D11_MAP_READ, 0, &m));
		if (ok) {
			out.resize((size_t)QVD_OVERLAY_W * QVD_OVERLAY_H * 4);
			for (uint32_t y = 0; y < QVD_OVERLAY_H; y++)
				memcpy(&out[(size_t)y * QVD_OVERLAY_W * 4], (const uint8_t *)m.pData + (size_t)y * m.RowPitch,
				       QVD_OVERLAY_W * 4);
			c->Unmap(stage, 0);
		}
		stage->Release();
	}
	if (on12) {
		src->Release();
		on12->Release();
		c->Release();
		d->Release();
	}
	return ok;
}

static bool WriteText(const wchar_t *path, const char *text)
{
	FILE *f = _wfopen(path, L"w");
	if (!f)
		return false;
	fputs(text, f);
	fclose(f);
	return true;
}

static bool g_exitEarly = false; // --exit-early: the game quits with the overlay on and its session still open

// ---------------------------------------------------------------- moving the panel (user decisions 2026-11-07)
// The test's own low-level keyboard hook, set BEFORE the layer unlocks (a newer hook is called first, so this
// one sees only what got past the layer). It counts the injected move keys that got past and swallows them
// too, so none of the test's key presses reach the user's focused window - except Up while g_tkPassUp is set
// (the "held since before the unlock" check; Up only moves a caret).
static const DWORD kTkKeys[8] = {VK_DELETE, VK_NEXT, VK_END, VK_HOME, VK_INSERT, VK_PRIOR, VK_DOWN, VK_UP};
static std::atomic<uint32_t> g_tkSeen[256];
static std::atomic<bool> g_tkPassUp{false};
static DWORD g_tkTid = 0;
static LRESULT CALLBACK TestKbProc(int code, WPARAM wp, LPARAM lp)
{
	if (code == HC_ACTION) {
		const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
		if (k->flags & LLKHF_INJECTED)
			for (DWORD vk : kTkKeys)
				if (k->vkCode == vk) {
					g_tkSeen[vk]++;
					if (!(vk == VK_UP && g_tkPassUp))
						return 1;
				}
	}
	return CallNextHookEx(nullptr, code, wp, lp);
}
static DWORD WINAPI TestKbThread(void *ready)
{
	MSG m;
	PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);
	HHOOK h = SetWindowsHookExW(WH_KEYBOARD_LL, TestKbProc, GetModuleHandleW(nullptr), 0);
	SetEvent((HANDLE)ready);
	if (!h)
		return 1;
	while (GetMessageW(&m, nullptr, 0, 0) > 0) {
	}
	UnhookWindowsHookEx(h);
	return 0;
}
static void SendKey(WORD vk, bool down)
{
	INPUT in = {};
	in.type = INPUT_KEYBOARD;
	in.ki.wVk = vk;
	const bool ext = vk == VK_DELETE || vk == VK_NEXT || vk == VK_END || vk == VK_HOME || vk == VK_INSERT ||
			 vk == VK_PRIOR || vk == VK_DOWN || vk == VK_UP || vk == VK_RSHIFT;
	in.ki.dwFlags = (ext ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
	SendInput(1, &in, sizeof(in));
}
static void TapKey(WORD vk)
{
	SendKey(vk, true);
	SendKey(vk, false);
}
static std::string ReadAll(const wchar_t *path);
static bool Near(float a, float b, float tol = 1e-4f)
{
	return fabsf(a - b) <= tol;
}

static int RunOverlayTest(const char *layerPath, bool d3d12)
{
	if (HANDLE live = OpenFileMappingW(FILE_MAP_READ, FALSE, qvd_channel_name())) {
		QvdShared *v = (QvdShared *)MapViewOfFile(live, FILE_MAP_READ, 0, 0, sizeof(QvdShared));
		printf("NOT RUN: picture channel is in use (writer pid %u, app '%s'). Close the game and OBS, then rerun.\n",
		       v ? v->writerPid : 0, v ? v->appName : "?");
		return 3;
	}
	printf("overlay test (%s)\n", d3d12 ? "D3D12" : "D3D11");
	// the key file of this test only (never the user's real one); not canonical text on purpose
	wchar_t keyFile[MAX_PATH];
	ExpandEnvironmentStringsW(L"%TEMP%\\archan_layer_test_overlay.ini", keyFile, MAX_PATH);
	SetEnvironmentVariableW(L"ARCHAN_VRCAPTURE_TEST_SETTINGS", keyFile);
	CHECK(WriteText(keyFile, "key=alt + f10\nenabled=1\n"), "write settings file");

	g_useD3D12 = d3d12;
	ID3D12Device *dev12 = nullptr;
	ID3D12CommandQueue *queue = nullptr;
	const uint32_t GW = 64, GH = 64; // the game's own swapchain (not copied here: no OBS reader)
	if (d3d12) {
		CHECK(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&dev12)),
		      "D3D12 device");
		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		CHECK(SUCCEEDED(dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&queue)), "queue");
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC rd = {};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		rd.MipLevels = 1;
		rd.SampleDesc.Count = 1;
		rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		rd.Width = GW;
		rd.Height = GH;
		rd.DepthOrArraySize = 2;
		rd.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
		for (uint32_t i = 0; i < NUM_IMAGES; i++)
			CHECK(SUCCEEDED(dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET,
								       nullptr, __uuidof(ID3D12Resource), (void **)&g_images12[i])),
			      "game image");
		rd.Width = QVD_OVERLAY_W;
		rd.Height = QVD_OVERLAY_H;
		rd.DepthOrArraySize = 1;
		rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		for (uint32_t i = 0; i < OVL_IMAGES; i++)
			CHECK(SUCCEEDED(dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET,
								       nullptr, __uuidof(ID3D12Resource), (void **)&g_ovlImages12[i])),
			      "overlay image");
	} else {
		CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
						  &g_dev, nullptr, &g_ctx)),
		      "device");
		D3D11_TEXTURE2D_DESC td = {};
		td.MipLevels = 1;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		td.Width = GW;
		td.Height = GH;
		td.ArraySize = 2;
		td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
		for (uint32_t i = 0; i < NUM_IMAGES; i++)
			CHECK(SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_images[i])), "game image");
		td.Width = QVD_OVERLAY_W;
		td.Height = QVD_OVERLAY_H;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		for (uint32_t i = 0; i < OVL_IMAGES; i++)
			CHECK(SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_ovlImages[i])), "overlay image");
	}

	HMODULE layer = LoadLibraryA(layerPath);
	CHECK(layer, "LoadLibrary(%s) error %lu", layerPath, GetLastError());
	auto negotiate = (PFN_xrNegotiateLoaderApiLayerInterface)GetProcAddress(layer, "xrNegotiateLoaderApiLayerInterface");
	typedef void (*PFN_TestOverlay)(int);
	typedef void (*PFN_TestStats)(double *, uint64_t *);
	typedef void (*PFN_TestPlace)(float *, int *, uint64_t *);
	auto testOverlay = (PFN_TestOverlay)GetProcAddress(layer, "archan_vr_capture_test_overlay");
	auto testStats = (PFN_TestStats)GetProcAddress(layer, "archan_vr_capture_test_overlay_stats");
	auto testUnlock = (PFN_TestOverlay)GetProcAddress(layer, "archan_vr_capture_test_overlay_unlock");
	auto testPlace = (PFN_TestPlace)GetProcAddress(layer, "archan_vr_capture_test_overlay_place");
	CHECK(negotiate && testOverlay && testStats && testUnlock && testPlace, "layer exports");
	XrNegotiateLoaderInfo li = {XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION,
				    sizeof(li), 1, 1, XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 0x3ff, 0xfff)};
	XrNegotiateApiLayerRequest rq = {XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST,
					 XR_API_LAYER_INFO_STRUCT_VERSION, sizeof(rq)};
	CHECK(negotiate(&li, "XR_APILAYER_ARCHAN_vr_capture", &rq) == XR_SUCCESS, "negotiate");
	XrApiLayerNextInfo next = {XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO, XR_API_LAYER_NEXT_INFO_STRUCT_VERSION,
				   sizeof(next)};
	strcpy_s(next.layerName, "XR_APILAYER_ARCHAN_vr_capture");
	next.nextGetInstanceProcAddr = mock_GIPA;
	next.nextCreateApiLayerInstance = mock_CreateApiLayerInstance;
	XrApiLayerCreateInfo lci = {XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO,
				    XR_API_LAYER_CREATE_INFO_STRUCT_VERSION, sizeof(lci)};
	lci.nextInfo = &next;
	XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy_s(ici.applicationInfo.applicationName, "QVD overlay mock app");
	XrInstance inst = XR_NULL_HANDLE;
	CHECK(rq.createApiLayerInstance(&ici, &lci, &inst) == XR_SUCCESS, "create instance");
#define GET(fn, var)                                                                                       \
	PFN_##fn var = nullptr;                                                                            \
	CHECK(rq.getInstanceProcAddr(inst, #fn, (PFN_xrVoidFunction *)&var) == XR_SUCCESS && var, #fn);
	GET(xrCreateSession, CreateSession)
	GET(xrCreateSwapchain, CreateSwapchain)
	GET(xrEnumerateSwapchainImages, EnumerateSwapchainImages)
	GET(xrAcquireSwapchainImage, AcquireSwapchainImage)
	GET(xrReleaseSwapchainImage, ReleaseSwapchainImage)
	GET(xrEndFrame, EndFrame)
	GET(xrBeginFrame, BeginFrame)
	GET(xrDestroySession, DestroySession)
	GET(xrDestroyInstance, DestroyInstance)
	GET(xrCreateReferenceSpace, CreateReferenceSpace)
#undef GET

	XrGraphicsBindingD3D11KHR gb11 = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
	gb11.device = g_dev;
	XrGraphicsBindingD3D12KHR gb12 = {XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
	gb12.device = dev12;
	gb12.queue = queue;
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO, d3d12 ? (const void *)&gb12 : (const void *)&gb11};
	XrSession session;
	CHECK(CreateSession(inst, &sci, &session) == XR_SUCCESS, "session");
	XrReferenceSpaceCreateInfo rci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rci.poseInReferenceSpace.orientation.w = 1.0f;
	XrSpace space = XR_NULL_HANDLE;
	CHECK(CreateReferenceSpace(session, &rci, &space) == XR_SUCCESS, "reference space");
	XrSwapchainCreateInfo swci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
	swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	swci.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	swci.width = GW;
	swci.height = GH;
	swci.arraySize = 2;
	swci.sampleCount = swci.mipCount = swci.faceCount = 1;
	XrSwapchain sc;
	CHECK(CreateSwapchain(session, &swci, &sc) == XR_SUCCESS, "swapchain");
	uint32_t n = 0;
	CHECK(EnumerateSwapchainImages(sc, 0, &n, nullptr) == XR_SUCCESS && n == NUM_IMAGES, "enum count");
	std::vector<XrSwapchainImageD3D11KHR> i11(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
	std::vector<XrSwapchainImageD3D12KHR> i12(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
	CHECK(EnumerateSwapchainImages(sc, n, &n,
				       d3d12 ? (XrSwapchainImageBaseHeader *)i12.data() : (XrSwapchainImageBaseHeader *)i11.data()) ==
		      XR_SUCCESS,
	      "enum");

	XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
						     {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
	for (uint32_t v = 0; v < 2; v++) {
		views[v].subImage.swapchain = sc;
		views[v].subImage.imageRect = {{0, 0}, {(int32_t)GW, (int32_t)GH}};
		views[v].subImage.imageArrayIndex = v;
		views[v].pose = {POSE_Q, {v ? 0.064f : 0.0f, 0, 0}}; // the right eye 64 mm to the right (the panel uses it)
		views[v].fov = POSE_FOV;
	}
	XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	proj.viewCount = 2;
	proj.space = space;
	proj.views = views;
	const XrCompositionLayerBaseHeader *layers[] = {(const XrCompositionLayerBaseHeader *)&proj};

	// 50 fps game loop: high resolution timer, the last 1.5 ms spun
	LARGE_INTEGER qf;
	QueryPerformanceFrequency(&qf);
	auto qpc = []() {
		LARGE_INTEGER v;
		QueryPerformanceCounter(&v);
		return v.QuadPart;
	};
	const int64_t period = qf.QuadPart / 50;
	int64_t due = qpc();
	HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	CHECK(timer, "timer");
	int64_t callTicks = 0; // the test's own stopwatch around the layer's calls (acquire, release, end frame)
	auto frame = [&]() -> XrResult {
		due += period;
		const double rem = (double)(due - qpc()) / (double)qf.QuadPart;
		if (rem > 0.002) {
			LARGE_INTEGER d;
			d.QuadPart = -(LONGLONG)((rem - 0.0015) * 1e7);
			SetWaitableTimer(timer, &d, 0, nullptr, nullptr, FALSE);
			WaitForSingleObject(timer, INFINITE);
		}
		while (qpc() < due)
			YieldProcessor();
		const int64_t c0 = qpc();
		uint32_t idx;
		AcquireSwapchainImage(sc, nullptr, &idx);
		ReleaseSwapchainImage(sc, nullptr);
		XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
		fei.displayTime = DISPLAY_TIME;
		fei.layerCount = 1;
		fei.layers = layers;
		XrResult fr = EndFrame(session, &fei);
		if (fr == XR_SUCCESS)
			fr = BeginFrame(session, nullptr); // the next frame begins (D3D11: the picture goes to OBS here)
		callTicks += qpc() - c0;
		return fr;
	};
	std::atomic<bool> spin{true};
	std::thread spinner([&]() {
		while (spin.load(std::memory_order_relaxed))
			YieldProcessor();
	});
	spinner.detach(); // a failed CHECK returns early: never std::terminate on a joinable thread

	HANDLE shmMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, qvd_channel_name());
	QvdShared *shm = shmMap ? (QvdShared *)MapViewOfFile(shmMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(QvdShared)) : nullptr;
	CHECK(shm && shm->writerPid == GetCurrentProcessId(), "the layer does not own the test channel");

	// 1) off: the game's frame goes through unchanged, nothing created
	for (int i = 0; i < 30; i++) {
		CHECK(frame() == XR_SUCCESS, "endframe");
		CHECK(g_lastLayerCount == 1 && g_lastFirstLayer == layers[0] && !g_lastHasQuad, "off: frame changed (%u layers)",
		      g_lastLayerCount);
	}
	CHECK(g_ovlCreates == 0 && g_viewCreates == 0 && g_ovlAcquires == 0 && shm->overlayVisible == 0,
	      "off: overlay resources made (%u swapchains, %u spaces, %u acquires)", g_ovlCreates, g_viewCreates, g_ovlAcquires);
	std::string keyLine = LayerLogLine("overlay settings: ");
	printf("1) off, 30 frames: game frame unchanged, no overlay swapchain; layer.log: %s", keyLine.c_str());
	CHECK(strstr(keyLine.c_str(), "overlay settings: on, key Alt+F10 ("), "settings 'alt + f10' not read as Alt+F10");

	// 2) on for 11.5 s
	testOverlay(1);
	const int64_t onStart = qpc();
	while ((double)(qpc() - onStart) / qf.QuadPart < 11.5)
		CHECK(frame() == XR_SUCCESS, "endframe");
	double st[11];
	uint64_t version = 0;
	testStats(st, &version);
	unsigned long long ncpu = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
	printf("2) on 11.5 s: swapchain %u (format %lld, usage 0x%llX, %ux%u array %u mips %u samples %u), view spaces %u, "
	       "acquire/wait/release %u/%u/%u, panel version %llu\n",
	       g_ovlCreates, (long long)g_ovlCreate.format, (unsigned long long)g_ovlCreate.usageFlags, g_ovlCreate.width,
	       g_ovlCreate.height, g_ovlCreate.arraySize, g_ovlCreate.mipCount, g_ovlCreate.sampleCount, g_viewCreates,
	       g_ovlAcquires, g_ovlWaits, g_ovlReleases, (unsigned long long)version);
	printf("   last frame: %u layers, first = game's projection: %s, quad on top: %s, space %s, swapchain %s, rect %d,%d "
	       "%dx%d, flags 0x%llX, eyes %d, pos %.2f %.2f %.2f, size %.3f x %.4f\n",
	       g_lastLayerCount, g_lastFirstLayer == layers[0] ? "yes" : "NO", g_lastHasQuad ? "yes" : "NO",
	       g_lastQuad.space == space ? "the game's space" : "OTHER", g_lastQuad.subImage.swapchain == OVL_SC ? "overlay" : "OTHER",
	       g_lastQuad.subImage.imageRect.offset.x, g_lastQuad.subImage.imageRect.offset.y,
	       g_lastQuad.subImage.imageRect.extent.width, g_lastQuad.subImage.imageRect.extent.height,
	       (unsigned long long)g_lastQuad.layerFlags, (int)g_lastQuad.eyeVisibility, g_lastQuad.pose.position.x,
	       g_lastQuad.pose.position.y, g_lastQuad.pose.position.z, g_lastQuad.size.width, g_lastQuad.size.height);
	printf("   measured: FPS %.2f, %.3f ms per frame, CPU %.2f %% of %llu logical CPUs (= %.2f cores), busiest thread "
	       "%.1f %%, GPU %.1f %%; Archan %s %.4f ms per frame (%.3f %%, max %.4f ms)\n",
	       st[0], st[1], st[2], ncpu, st[2] * ncpu / 100.0, st[3], st[4], st[8] ? "capturing" : "not capturing", st[5],
	       st[6], st[7]);
	CHECK(g_ovlCreates == 1 && g_ovlCreate.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && g_ovlCreate.arraySize == 1 &&
		      g_ovlCreate.mipCount == 1 && g_ovlCreate.sampleCount == 1 && g_ovlCreate.faceCount == 1 &&
		      (g_ovlCreate.usageFlags & (XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT)) ==
			      (XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT),
	      "overlay swapchain");
	CHECK(g_viewCreates == 1 && g_viewTypeCreates == 1, "panel space: one VIEW");
	if (g_exitEarly) {
		printf("exit-early: leaving main() now, overlay on, session and instance not destroyed\n");
		return 0;
	}
	CHECK(g_ovlAcquires == g_ovlWaits && g_ovlWaits == g_ovlReleases && g_ovlAcquires >= 11 && g_ovlAcquires <= 13,
	      "uploads: expected one per second (11..13 in 11.5 s)");
	CHECK(g_lastLayerCount == 2 && g_lastFirstLayer == layers[0] && g_lastHasQuad && g_lastQuad.space == space &&
		      g_lastQuad.subImage.swapchain == OVL_SC && g_lastQuad.subImage.imageRect.extent.width == (int32_t)QVD_OVERLAY_W &&
		      g_lastQuad.subImage.imageRect.extent.height == (int32_t)QVD_OVERLAY_H &&
		      g_lastQuad.layerFlags == XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT &&
		      g_lastQuad.eyeVisibility == XR_EYE_VISIBILITY_BOTH,
	      "quad layer");
	// nothing saved yet: centre, 10 cm down, 1 m ahead of the seated position, upright (user decision 2026-11-07)
	printf("   panel place (nothing saved): pos %.4f %.4f %.4f, orientation %.4f %.4f %.4f %.4f\n",
	       g_lastQuad.pose.position.x, g_lastQuad.pose.position.y, g_lastQuad.pose.position.z,
	       g_lastQuad.pose.orientation.x, g_lastQuad.pose.orientation.y, g_lastQuad.pose.orientation.z,
	       g_lastQuad.pose.orientation.w);
	// locked to the view (user decision 2026-11-08): where DCS's blue overlay is in the right eye picture, ray
	// tan (0.1916, -0.1211) of the right view, 1 m, facing the eye, 20 % smaller. The right view here is at
	// x 0.064 turned 45 deg about y (POSE_Q), so: the ray turned by POSE_Q, from that eye, in the game's space.
	{
		const float tx = 0.1916f, ty = -0.1211f, n = sqrtf(tx * tx + ty * ty + 1.0f);
		const float d[3] = {tx / n, ty / n, -1.0f / n};
		// rotate d by POSE_Q (45 deg about y): x' = x cos45 + z sin45, z' = -x sin45 + z cos45
		const float c45 = 0.70710678f;
		const float ex = d[0] * c45 + d[2] * c45, ey = d[1], ez = -d[0] * c45 + d[2] * c45, eyeX = 0.064f;
		// the panel faces the eye: its +z axis points back along the ray, i.e. rotate (0,0,1) by its orientation = -d'
		const XrQuaternionf &o = g_lastQuad.pose.orientation;
		const float zx = 2.0f * (o.x * o.z + o.w * o.y), zy = 2.0f * (o.y * o.z - o.w * o.x),
			    zz = 1.0f - 2.0f * (o.x * o.x + o.y * o.y);
		// and it stays upright for the eye: its x axis has no y part when the eye is level
		const float xy = 2.0f * (o.x * o.y + o.w * o.z);
		const XrCompositionLayerQuad &q = g_lastQuad;
		printf("   panel locked to the view: pos %.4f %.4f %.4f (expected %.4f %.4f %.4f), faces %.4f %.4f %.4f, %.4f x %.4f m\n",
		       q.pose.position.x, q.pose.position.y, q.pose.position.z, eyeX + ex, ey, ez, zx, zy, zz, q.size.width, q.size.height);
		CHECK(q.space == space && Near(q.pose.position.x, eyeX + ex) && Near(q.pose.position.y, ey) && Near(q.pose.position.z, ez) &&
			      Near(zx, -ex, 1e-3f) && Near(zy, -ey, 1e-3f) && Near(zz, -ez, 1e-3f) && Near(xy, 0.0f, 1e-3f) &&
			      Near(q.size.width, 0.3072f) && Near(q.size.height, 0.3072f * 196.0f / 640.0f),
		      "panel: on DCS's overlay spot of the right eye, 1 m, facing it, 0.3072 m wide");
	}
	CHECK(st[0] > 49.5 && st[0] < 50.5 && st[1] > 19.8 && st[1] < 20.2, "FPS / frame time not 50 / 20 ms");
	CHECK(st[3] >= 95.0, "busiest thread (the spinner) not ~100 %%");
	CHECK(st[2] * ncpu / 100.0 > 0.95 && st[2] * ncpu / 100.0 < 1.5, "process CPU not ~1 core");
	CHECK(st[4] >= 0.0 && st[4] <= 100.0, "GPU load not measured (%.1f)", st[4]);
	CHECK(st[8] == 0.0 && st[5] >= 0.0 && st[5] < 0.5 && st[7] >= st[5], "Archan line while not capturing");
	const std::string costLine = LayerLogLine("overlay (last ");
	printf("   layer.log: %s", costLine.c_str());
	CHECK(!costLine.empty(), "no overlay cost line in layer.log");

	// the panel in the headset image = the panel for OBS, and it looks like a panel
	// (the overlay thread writes OBS's copy, the game thread uploads at its next frame: one frame to catch up)
	std::vector<uint8_t> img;
	bool same = false;
	for (int tries = 0; tries < 3 && !same; tries++) {
		if (tries)
			CHECK(frame() == XR_SUCCESS, "endframe");
		CHECK(ReadOverlayImage(g_ovlLast, img, dev12, queue), "read back the overlay image");
		same = memcmp(img.data(), shm->overlayPixels, img.size()) == 0;
	}
	CHECK(shm->overlayVisible == 1, "shared memory: overlay not marked visible");
	CHECK(same, "headset image differs from the panel sent to OBS");
	uint32_t notGrey = 0, badBg = 0, text = 0, minX = QVD_OVERLAY_W, maxX = 0, minY = QVD_OVERLAY_H, maxY = 0;
	for (uint32_t y = 0; y < QVD_OVERLAY_H; y++)
		for (uint32_t x = 0; x < QVD_OVERLAY_W; x++) {
			const uint8_t *p = &img[((size_t)y * QVD_OVERLAY_W + x) * 4];
			if (p[0] != p[1] || p[1] != p[2] || p[3] < p[0])
				notGrey++;
			if (p[0] == 0 && p[3] != 153)
				badBg++;
			if (p[0] > 0) {
				minX = std::min(minX, x), maxX = std::max(maxX, x), minY = std::min(minY, y), maxY = std::max(maxY, y);
				text += p[0] >= 200;
			}
		}
	wchar_t dump[MAX_PATH];
	ExpandEnvironmentStringsW(d3d12 ? L"%TEMP%\\archan_overlay_panel_d3d12.rgba" : L"%TEMP%\\archan_overlay_panel_d3d11.rgba",
				  dump, MAX_PATH);
	if (FILE *f = _wfopen(dump, L"wb")) {
		fwrite(img.data(), 1, img.size(), f);
		fclose(f);
	}
	printf("   panel image: %u text pixels >= 200, text box x %u..%u y %u..%u of %ux%u, %u not grey/premultiplied, %u "
	       "background pixels not alpha 153; dumped to %ls\n",
	       text, minX, maxX, minY, maxY, QVD_OVERLAY_W, QVD_OVERLAY_H, notGrey, badBg, dump);
	CHECK(notGrey == 0 && badBg == 0, "panel pixels");
	CHECK(text > 1000 && minX >= 10 && maxX <= QVD_OVERLAY_W - 6 && minY >= 4 && maxY <= QVD_OVERLAY_H - 4,
	      "panel text missing or cut off");

	// 2b) OBS takes the picture (the test writes the heartbeat): the layer copies every frame. Archan's own
	// total must match the test's stopwatch around the same calls (which also holds the mock runtime and the
	// call overhead): 80 % .. 100 % of it, minus 10 us per frame of slack.
	{
		double a[11], b[11];
		testStats(a, &version);
		const int64_t callsBefore = callTicks;
		const uint64_t copiedBefore = shm->frameCount;
		uint32_t n = 0;
		const int64_t t = qpc();
		while ((double)(qpc() - t) / qf.QuadPart < 3.2) {
			shm->readerTickMs = GetTickCount64();
			CHECK(frame() == XR_SUCCESS, "endframe");
			n++;
		}
		testStats(b, &version);
		const double layerMs = b[9] - a[9], testMs = (double)(callTicks - callsBefore) * 1e3 / qf.QuadPart;
		const uint64_t layerFrames = (uint64_t)(b[10] - a[10]), copied = shm->frameCount - copiedBefore;
		printf("2b) capturing 3.2 s: %u frames, %llu copied; Archan's own total %.3f ms over %llu frames (%.4f ms per "
		       "frame), test stopwatch %.3f ms (%.4f ms per frame) = %.1f %%; panel: Archan %s %.4f ms per frame (%.3f "
		       "%%, max %.4f ms)\n",
		       n, (unsigned long long)copied, layerMs, (unsigned long long)layerFrames, layerMs / n, testMs, testMs / n,
		       layerMs / testMs * 100.0, b[8] ? "capturing" : "not capturing", b[5], b[6], b[7]);
		CHECK(copied >= n - 2, "the layer did not copy every frame");
		CHECK(layerFrames == n, "Archan counted %llu frames, the test made %u", (unsigned long long)layerFrames, n);
		CHECK(layerMs <= testMs && layerMs >= 0.8 * testMs - 0.01 * n, "Archan's own time does not match the stopwatch");
		CHECK(b[8] == 1.0 && b[5] > 0 && b[7] >= b[5] && b[6] > 0, "panel line while capturing");
		wchar_t capDump[MAX_PATH];
		ExpandEnvironmentStringsW(d3d12 ? L"%TEMP%\\archan_overlay_panel_d3d12_capturing.rgba"
						: L"%TEMP%\\archan_overlay_panel_d3d11_capturing.rgba",
					  capDump, MAX_PATH);
		if (FILE *f = _wfopen(capDump, L"wb")) { // the panel OBS got (the newest)
			fwrite(shm->overlayPixels, 1, sizeof(shm->overlayPixels), f);
			fclose(f);
		}
	}

	// 3) off again: the game's frame unchanged, no more uploads, OBS told
	testOverlay(0);
	const uint32_t acquiresOn = g_ovlAcquires;
	for (int i = 0; i < 30; i++) {
		CHECK(frame() == XR_SUCCESS, "endframe");
		CHECK(g_lastLayerCount == 1 && g_lastFirstLayer == layers[0] && !g_lastHasQuad, "off again: frame changed");
	}
	printf("3) off again, 30 frames: %u layers, uploads %u -> %u, shared memory visible %u\n", g_lastLayerCount, acquiresOn,
	       g_ovlAcquires, shm->overlayVisible);
	CHECK(g_ovlAcquires == acquiresOn && shm->overlayVisible == 0, "off again");

	// 4) the settings file changes while the game runs (OBS / the settings window saved a new key / a broken one)
	CHECK(WriteText(keyFile, "key=Ctrl+Shift+F\nenabled=1\n"), "write settings file");
	for (int i = 0; i < 60; i++)
		frame();
	const std::string k2 = LayerLogLine("overlay settings");
	CHECK(WriteText(keyFile, "key=Ctrl+\nenabled=1\n"), "write settings file");
	for (int i = 0; i < 60; i++)
		frame();
	const std::string k3 = LayerLogLine("overlay settings: key");
	printf("4) settings key 'Ctrl+Shift+F' -> %s   settings key 'Ctrl+' -> %s", k2.c_str(), k3.c_str());
	CHECK(strstr(k2.c_str(), "overlay settings: on, key Ctrl+Shift+F ("), "new key not read");
	CHECK(strstr(k3.c_str(), "overlay settings: key 'Ctrl+' is not a key, using Ctrl+Shift+F"), "bad key not refused");

	// 6) OBS (user decision 2026-11-07): shown by itself while OBS runs, off when OBS closes; the switch off
	// in the settings wins. "OBS runs" = the plugin's presence mutex (this test's own name).
	{
		auto runFor = [&](double secs) {
			const int64_t t = qpc();
			while ((double)(qpc() - t) / qf.QuadPart < secs)
				frame();
		};
		const uint32_t createsBefore = g_ovlCreates;
		HANDLE obs = CreateMutexW(nullptr, FALSE, qvd_obs_presence_name());
		CHECK(obs, "presence mutex");
		runFor(1.2);
		const bool shownWithObs = g_lastHasQuad && shm->overlayVisible == 1;
		const std::string l1 = LayerLogLine("OBS is running");
		CloseHandle(obs);
		runFor(1.2);
		const bool offAfterObs = !g_lastHasQuad && shm->overlayVisible == 0;
		const std::string l2 = LayerLogLine("OBS closed");
		CHECK(WriteText(keyFile, "key=Ctrl+Shift+F\nenabled=0\n"), "write settings file");
		runFor(1.0);
		obs = CreateMutexW(nullptr, FALSE, qvd_obs_presence_name());
		runFor(1.2);
		const bool offWhenSwitchedOff = !g_lastHasQuad && shm->overlayVisible == 0;
		const std::string l3 = LayerLogLine("overlay settings: OFF");
		CHECK(WriteText(keyFile, "key=Ctrl+Shift+F\nenabled=1\n"), "write settings file");
		runFor(1.2);
		const bool backOn = g_lastHasQuad && shm->overlayVisible == 1;
		CloseHandle(obs);
		runFor(1.2);
		printf("6) OBS starts -> shown %s (%s)   OBS closes -> off %s (%s)   switched off + OBS -> off %s (%s)   "
		       "switched on + OBS -> shown %s; panel swapchain kept (%u created)\n",
		       shownWithObs ? "yes" : "NO", l1.empty() ? "no log" : "logged", offAfterObs ? "yes" : "NO",
		       l2.empty() ? "no log" : "logged", offWhenSwitchedOff ? "yes" : "NO", l3.empty() ? "no log" : "logged",
		       backOn ? "yes" : "NO", g_ovlCreates - createsBefore);
		CHECK(shownWithObs && !l1.empty(), "not shown while OBS runs");
		CHECK(offAfterObs && !l2.empty(), "not off after OBS closed");
		CHECK(offWhenSwitchedOff && !l3.empty(), "shown although switched off");
		CHECK(backOn, "not shown again when switched on while OBS runs");
		CHECK(!g_lastHasQuad, "still shown after OBS closed");
	}

	// 7) the panel is locked to the view (2026-11-07): Ctrl+Shift+L (the test export) does nothing
	{
		testUnlock(1);
		for (int i = 0; i < 20; i++)
			frame();
		float p[4];
		int un = -1;
		uint64_t sw = 0;
		testPlace(p, &un, &sw);
		printf("7) Ctrl+Shift+L with the panel locked to the view: unlocked %d (correct: 0)\n", un);
		CHECK(un == 0 && g_lastQuad.space == space, "Ctrl+Shift+L unlocked a view-locked panel");
	}
	// 7 old) moving the panel - off while it is locked to the view (kPanelMovable in the layer)
	if (false) {
		auto runFor = [&](double secs) {
			const int64_t t = qpc();
			while ((double)(qpc() - t) / qf.QuadPart < secs)
				frame();
		};
		auto place = [&](float p[4], int *un = nullptr, uint64_t *sw = nullptr) {
			int u;
			uint64_t s;
			testPlace(p, un ? un : &u, sw ? sw : &s);
		};
		HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		HANDLE tk = CreateThread(nullptr, 0, TestKbThread, ready, 0, &g_tkTid);
		CHECK(tk && WaitForSingleObject(ready, 3000) == WAIT_OBJECT_0, "test keyboard hook");
		struct KeysUp { // never leave a modifier down if a CHECK returns early
			~KeysUp()
			{
				SendKey(VK_RSHIFT, false);
				SendKey(VK_LCONTROL, false);
				SendKey(VK_UP, false);
				PostThreadMessageW(g_tkTid, WM_QUIT, 0, 0);
			}
		} keysUp;
		testOverlay(1);
		runFor(1.2);
		float p0[4], p[4];
		int un = -1;
		uint64_t sw0 = 0, sw = 0;
		place(p0, &un, &sw0);
		// locked: Del is not the overlay's
		const uint32_t delSeen0 = g_tkSeen[VK_DELETE];
		TapKey(VK_DELETE);
		runFor(0.15);
		place(p, &un, &sw);
		const uint32_t lockedPast = g_tkSeen[VK_DELETE] - delSeen0;
		printf("7a) locked: Del got past the layer %u of 2, layer took %llu, place unchanged %s, unlocked %d\n", lockedPast,
		       (unsigned long long)(sw - sw0), memcmp(p, p0, sizeof(p)) == 0 ? "yes" : "NO", un);
		CHECK(lockedPast == 2 && sw == sw0 && memcmp(p, p0, sizeof(p)) == 0 && un == 0, "locked: move key taken");
		// a key held since before the unlock keeps going to the game (its release too). This one Up reaches the
		// focused window, so it is only sent when that is VS Code (Up moves the caret) or this test itself.
		wchar_t fgName[MAX_PATH] = L"?";
		{
			DWORD fgPid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
			if (HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, fgPid)) {
				DWORD len = MAX_PATH;
				wchar_t full[MAX_PATH];
				if (QueryFullProcessImageNameW(ph, 0, full, &len)) {
					const wchar_t *b = wcsrchr(full, L'\\');
					wcscpy_s(fgName, b ? b + 1 : full);
				}
				CloseHandle(ph);
			}
		}
		const bool sendUp = _wcsicmp(fgName, L"Code.exe") == 0 || _wcsicmp(fgName, L"layer_mock_test.exe") == 0 ||
				    _wcsicmp(fgName, L"WindowsTerminal.exe") == 0 || _wcsicmp(fgName, L"conhost.exe") == 0;
		const uint32_t upSeen0 = g_tkSeen[VK_UP];
		if (sendUp) {
			g_tkPassUp = true;
			SendKey(VK_UP, true);
			runFor(0.05);
		}
		testUnlock(1);
		runFor(0.3);
		place(p, &un, &sw0);
		const std::string lu = LayerLogLine("overlay UNLOCKED");
		if (sendUp) {
			SendKey(VK_UP, false);
			runFor(0.15);
			g_tkPassUp = false;
		}
		float pu[4];
		place(pu, &un, &sw);
		const uint32_t upPast = g_tkSeen[VK_UP] - upSeen0;
		if (sendUp)
			printf("7b) unlocked %d (%s); Up held from before: down+up got past %u of 2, pitch %.2f -> %.2f\n", un,
			       lu.empty() ? "no log" : "logged", upPast, p[3], pu[3]);
		else
			printf("7b) unlocked %d (%s); Up held from before: NOT RUN (the focused window is %ls)\n", un,
			       lu.empty() ? "no log" : "logged", fgName);
		CHECK(un == 1 && !lu.empty(), "not unlocked");
		CHECK(!sendUp || (upPast == 2 && pu[3] == p0[3]), "a key held from before the unlock was taken");
		// the amber frame: OBS's copy right away, the headset image at the next upload
		const uint8_t *c0 = shm->overlayPixels;
		const bool obsAmber = c0[0] == 255 && c0[1] == 176 && c0[2] == 0 && c0[3] == 255;
		std::vector<uint8_t> im;
		CHECK(ReadOverlayImage(g_ovlLast, im, dev12, queue), "read back the overlay image");
		const bool hmdAmber = im[0] == 255 && im[1] == 176 && im[2] == 0 && im[3] == 255;
		const uint8_t *mid = &im[((size_t)(QVD_OVERLAY_H / 2) * QVD_OVERLAY_W + 8) * 4];
		printf("   amber frame: OBS %s, headset %s (corner %u,%u,%u,%u; inside %u,%u,%u,%u)\n", obsAmber ? "yes" : "NO",
		       hmdAmber ? "yes" : "NO", im[0], im[1], im[2], im[3], mid[0], mid[1], mid[2], mid[3]);
		CHECK(obsAmber && hmdAmber && mid[0] == mid[2], "unlocked frame");

		// the moves, the expected place computed the same way (the panel's own axes, pitched about x)
		float e[4];
		memcpy(e, p0, sizeof(e));
		auto nudge = [&](int what, float d) {
			const float a = e[3] * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a);
			switch (what) {
			case 0: e[0] += d; break;                 // right (+) / left (-)
			case 1: e[1] += c * d; e[2] += s * d; break; // up (+) / down (-)
			case 2: e[1] -= s * d; e[2] += c * d; break; // out (+) / in (-)
			case 3: e[3] += d; break;                 // pitch
			}
		};
		const uint32_t seen0 = g_tkSeen[VK_DELETE] + g_tkSeen[VK_NEXT] + g_tkSeen[VK_END] + g_tkSeen[VK_HOME] +
				       g_tkSeen[VK_INSERT] + g_tkSeen[VK_PRIOR] + g_tkSeen[VK_DOWN] + g_tkSeen[VK_UP];
		TapKey(VK_DELETE);
		runFor(0.15);
		nudge(0, -0.005f);
		place(p);
		const float xDel = p[0];
		const bool okDel = Near(p[0], e[0]) && Near(p[1], e[1]) && Near(p[2], e[2]);
		TapKey(VK_NEXT);
		runFor(0.1);
		TapKey(VK_NEXT);
		runFor(0.15);
		nudge(0, 0.005f);
		nudge(0, 0.005f);
		place(p);
		const bool okPgDn = Near(p[0], e[0]);
		TapKey(VK_UP);
		runFor(0.15);
		nudge(3, 1.0f);
		place(p);
		const bool okPitch = Near(p[3], e[3]);
		const float h = e[3] * 3.14159265f / 360.0f;
		// yaw(seat 45 deg) * pitch(place)
		const float sy = POSE_Q.y, cy = POSE_Q.w;
		const bool okQuat = Near(g_lastQuad.pose.orientation.x, cy * sinf(h)) &&
				    Near(g_lastQuad.pose.orientation.y, cosf(h) * sy) &&
				    Near(g_lastQuad.pose.orientation.z, -sy * sinf(h)) && Near(g_lastQuad.pose.orientation.w, cy * cosf(h));
		TapKey(VK_INSERT);
		runFor(0.15);
		nudge(2, -0.005f);
		place(p);
		const bool okIns = Near(p[1], e[1]) && Near(p[2], e[2]);
		printf("7c) Del x %.4f, PgDn x2 x %.4f, Up pitch %.2f (quad %.5f %.5f), Ins y %.5f z %.5f - as expected: %s %s %s "
		       "%s %s\n",
		       xDel, e[0], p[3], g_lastQuad.pose.orientation.x, g_lastQuad.pose.orientation.w, p[1], p[2],
		       okDel ? "yes" : "NO", okPgDn ? "yes" : "NO", okPitch ? "yes" : "NO", okQuat ? "yes" : "NO",
		       okIns ? "yes" : "NO");
		CHECK(okDel && okPgDn && okPitch && okQuat && okIns, "a move");
		// held Home for 1 s: one on the press, then one every 50 ms after 250 ms (about 16)
		float b[4];
		place(b);
		SendKey(VK_HOME, true);
		runFor(1.0);
		SendKey(VK_HOME, false);
		runFor(0.15);
		place(p);
		const float upStep = 0.005f * cosf(e[3] * 3.14159265f / 180.0f);
		const float nHome = (p[1] - b[1]) / upStep;
		printf("7d) Home held 1.0 s: %.2f steps of 5 mm (expected about 1 + 15..16)\n", nHome);
		CHECK(nHome > 13.9f && nHome < 19.1f && fabsf(nHome - roundf(nHome)) < 0.01f, "hold repeat");
		memcpy(e, p, sizeof(e));
		// Right Shift: a tenth; Ctrl held: no move
		SendKey(VK_RSHIFT, true);
		runFor(0.05);
		TapKey(VK_END);
		runFor(0.15);
		SendKey(VK_RSHIFT, false);
		runFor(0.05);
		nudge(1, -0.0005f);
		place(p);
		const bool okFine = Near(p[1], e[1], 2e-5f) && Near(p[2], e[2], 2e-5f);
		SendKey(VK_LCONTROL, true);
		runFor(0.05);
		TapKey(VK_DELETE);
		runFor(0.15);
		SendKey(VK_LCONTROL, false);
		runFor(0.05);
		float pc[4];
		place(pc, &un, &sw);
		const bool okCtrl = memcmp(pc, p, sizeof(p)) == 0;
		const uint32_t seen1 = g_tkSeen[VK_DELETE] + g_tkSeen[VK_NEXT] + g_tkSeen[VK_END] + g_tkSeen[VK_HOME] +
				       g_tkSeen[VK_INSERT] + g_tkSeen[VK_PRIOR] + g_tkSeen[VK_DOWN] + g_tkSeen[VK_UP];
		printf("7e) Right Shift + End: y %.5f (fine step %s); Ctrl + Del: no move %s; move keys that got past the "
		       "layer while unlocked: %u\n",
		       p[1], okFine ? "yes" : "NO", okCtrl ? "yes" : "NO", seen1 - seen0);
		CHECK(okFine && okCtrl && seen1 == seen0, "fine step / Ctrl / keys got past");
		// saved when let go
		const std::string file = ReadAll(keyFile);
		float f[4] = {};
		const char *pl = strstr(file.c_str(), "place=");
		const bool parsed = pl && sscanf_s(pl, "place=%f,%f,%f,%f", &f[0], &f[1], &f[2], &f[3]) == 4;
		const std::string ls = LayerLogLine("overlay moved to");
		printf("7f) settings file: %s   layer.log: %s", file.c_str(), ls.c_str());
		CHECK(parsed && Near(f[0], pc[0], 1e-4f) && Near(f[1], pc[1], 1e-4f) && Near(f[2], pc[2], 1e-4f) &&
			      Near(f[3], pc[3], 0.01f) && strstr(file.c_str(), "key=Ctrl+Shift+F\nenabled=1\n") && !ls.empty(),
		      "place not saved");
		// locked again: the keys pass, the frame goes
		testUnlock(0);
		runFor(0.3);
		const uint32_t d0 = g_tkSeen[VK_DELETE];
		TapKey(VK_DELETE);
		runFor(0.15);
		place(p, &un, &sw);
		const std::string ll = LayerLogLine("overlay locked");
		const uint8_t *c1 = shm->overlayPixels;
		printf("7g) locked again (%s): Del got past %u of 2, place unchanged %s, corner %u,%u,%u,%u\n",
		       ll.empty() ? "no log" : "logged", g_tkSeen[VK_DELETE] - d0, memcmp(p, pc, sizeof(p)) == 0 ? "yes" : "NO",
		       c1[0], c1[1], c1[2], c1[3]);
		CHECK(un == 0 && !ll.empty() && g_tkSeen[VK_DELETE] - d0 == 2 && memcmp(p, pc, sizeof(p)) == 0 && c1[3] == 153,
		      "locked again");
		// unlocking again re-seats it (user decision 2026-11-07): the middle, 10 cm down, 1 m in front of the head
		testUnlock(1);
		runFor(0.3);
		place(p, &un, &sw);
		const std::string rs = LayerLogLine("overlay seat taken again (Ctrl+Shift+L)");
		printf("7h) unlocked again: place %.4f %.4f %.4f %.2f (%s)\n", p[0], p[1], p[2], p[3],
		       rs.empty() ? "no re-seat log" : "re-seated, logged");
		CHECK(Near(p[0], 0.0f) && Near(p[1], -0.10f) && Near(p[2], -1.0f) && Near(p[3], 0.0f) && !rs.empty() &&
			      Near(g_lastQuad.pose.position.x, -0.70711f) && Near(g_lastQuad.pose.position.z, -0.70711f),
		      "unlock did not re-seat");
		// hiding the overlay locks it
		const int unBefore = un;
		testOverlay(0);
		runFor(0.3);
		place(p, &un, &sw);
		const std::string lh = LayerLogLine("overlay locked (overlay hidden)");
		printf("7h) unlocked %d, then hidden -> unlocked %d (%s)\n", unBefore, un, lh.empty() ? "no log" : "logged");
		CHECK(unBefore == 1 && un == 0 && !lh.empty(), "hiding did not lock");
	}

	// 5) session end: the layer's own swapchain and space go first
	spin = false;
	CHECK(DestroySession(session) == XR_SUCCESS, "destroy session");
	printf("5) session end: overlay swapchain destroyed %u, panel space destroyed %u\n", g_ovlDestroyed, g_viewDestroyed);
	CHECK(g_ovlDestroyed == 1 && g_viewDestroyed == 1, "session end cleanup");

	// 8) the next session (= the next game start): the saved place, and locked - off with the view-locked panel
	if (false) {
		XrSession s2;
		CHECK(CreateSession(inst, &sci, &s2) == XR_SUCCESS, "second session");
		Sleep(700);
		float p[4];
		int un = -1;
		uint64_t sw = 0;
		testPlace(p, &un, &sw);
		const std::string lp = LayerLogLine("overlay place:");
		const std::string file = ReadAll(keyFile);
		float f[4] = {};
		const char *pl = strstr(file.c_str(), "place=");
		const bool parsed = pl && sscanf_s(pl, "place=%f,%f,%f,%f", &f[0], &f[1], &f[2], &f[3]) == 4;
		printf("8) next session: place %.4f %.4f %.4f %.2f, unlocked %d; layer.log: %s", p[0], p[1], p[2], p[3], un,
		       lp.c_str());
		CHECK(parsed && strstr(lp.c_str(), "(saved)") && Near(p[0], f[0]) && Near(p[1], f[1]) && Near(p[2], f[2]) &&
			      Near(p[3], f[3], 0.01f) && un == 0,
		      "next session: saved place / locked");
		CHECK(DestroySession(s2) == XR_SUCCESS, "destroy second session");
	}
	DestroyInstance(inst);
	DeleteFileW(keyFile);
	printf("PASS overlay (%s)\n", d3d12 ? "D3D12" : "D3D11");
	return 0;
}

// ---------------------------------------------------------------- the settings window (user decision 2026-11-07)
// Starts the real ArchanVRCaptureSettings.exe on a test settings file and drives its controls by id
// (the same messages a click / key press makes): defaults shown, key + switch saved, a key we do not
// support refused, saved values shown again on the next start.
struct FindData {
	DWORD pid;
	HWND hwnd;
};
static BOOL CALLBACK FindTop(HWND h, LPARAM lp)
{
	FindData *f = (FindData *)lp;
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	wchar_t cls[64];
	GetClassNameW(h, cls, 64);
	if (pid == f->pid && wcscmp(cls, L"ArchanVRCaptureSettings") == 0) {
		f->hwnd = h;
		return FALSE;
	}
	return TRUE;
}

static HWND StartSettings(const char *exe, PROCESS_INFORMATION &pi)
{
	STARTUPINFOA si = {sizeof(si)};
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_SHOWNOACTIVATE; // never take the focus from the user
	std::string cmd = std::string("\"") + exe + "\"";
	if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
		return nullptr;
	WaitForInputIdle(pi.hProcess, 5000);
	FindData f = {pi.dwProcessId, nullptr};
	for (int i = 0; i < 50 && !f.hwnd; i++) {
		EnumWindows(FindTop, (LPARAM)&f);
		if (!f.hwnd)
			Sleep(100);
	}
	return f.hwnd;
}

static std::string ReadAll(const wchar_t *path)
{
	std::string t;
	if (FILE *f = _wfopen(path, L"r")) {
		char b[256];
		const size_t n = fread(b, 1, sizeof(b), f);
		fclose(f);
		t.assign(b, n);
	}
	return t;
}

static int RunSettingsAppTest(const char *exe)
{
	enum { ID_ENABLED = 101, ID_KEY = 102, ID_SAVE = 103, ID_STATUS = 105 }; // as in settings_app.cpp
	wchar_t file[MAX_PATH];
	ExpandEnvironmentStringsW(L"%TEMP%\\archan_settings_app_test.ini", file, MAX_PATH);
	DeleteFileW(file);
	SetEnvironmentVariableW(L"ARCHAN_VRCAPTURE_TEST_SETTINGS", file);
	PROCESS_INFORMATION pi;
	HWND w = StartSettings(exe, pi);
	CHECK(w, "settings window did not open");
	wchar_t status[512];
	GetDlgItemTextW(w, ID_STATUS, status, 512);
	const bool defOn = IsDlgButtonChecked(w, ID_ENABLED) == BST_CHECKED;
	const WORD defKey = (WORD)SendDlgItemMessageW(w, ID_KEY, HKM_GETHOTKEY, 0, 0);
	printf("1) first start, no file: switch %s, key vk 0x%02X mods 0x%02X, status '%ls'\n", defOn ? "on" : "off",
	       LOBYTE(defKey), HIBYTE(defKey), status);
	CHECK(!defOn && LOBYTE(defKey) == 'F' && HIBYTE(defKey) == (HOTKEYF_CONTROL | HOTKEYF_SHIFT), "defaults (off)");

	// Alt+F10, switch off, Save
	SendDlgItemMessageW(w, ID_KEY, HKM_SETHOTKEY, MAKEWORD(VK_F10, HOTKEYF_ALT), 0);
	SendDlgItemMessageW(w, ID_ENABLED, BM_SETCHECK, BST_UNCHECKED, 0);
	SendDlgItemMessageW(w, ID_SAVE, BM_CLICK, 0, 0);
	Sleep(200);
	const std::string a = ReadAll(file);
	GetDlgItemTextW(w, ID_STATUS, status, 512);
	printf("2) Alt+F10, switch off, Save -> file '%s' status '%ls'\n", a.c_str(), status);
	CHECK(a == "key=Alt+F10\nenabled=0\n", "saved file");

	// a key our parser does not know (';'): refused, file unchanged
	SendDlgItemMessageW(w, ID_KEY, HKM_SETHOTKEY, MAKEWORD(VK_OEM_1, HOTKEYF_CONTROL), 0);
	SendDlgItemMessageW(w, ID_SAVE, BM_CLICK, 0, 0);
	Sleep(200);
	const std::string b = ReadAll(file);
	GetDlgItemTextW(w, ID_STATUS, status, 512);
	printf("3) Ctrl+; Save -> file unchanged %s, status '%.60ls...'\n", b == a ? "yes" : "NO", status);
	CHECK(b == a && wcsstr(status, L"cannot be used"), "unsupported key not refused");
	PostMessageW(w, WM_CLOSE, 0, 0);
	CHECK(WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0, "window did not close");
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	CHECK(code == 0, "exit code %lu", code);

	// next start shows what was saved
	w = StartSettings(exe, pi);
	CHECK(w, "settings window did not open again");
	const bool on2 = IsDlgButtonChecked(w, ID_ENABLED) == BST_CHECKED;
	const WORD key2 = (WORD)SendDlgItemMessageW(w, ID_KEY, HKM_GETHOTKEY, 0, 0);
	printf("4) next start: switch %s, key vk 0x%02X mods 0x%02X\n", on2 ? "on" : "off", LOBYTE(key2), HIBYTE(key2));
	PostMessageW(w, WM_CLOSE, 0, 0);
	WaitForSingleObject(pi.hProcess, 5000);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	CHECK(!on2 && LOBYTE(key2) == VK_F10 && HIBYTE(key2) == HOTKEYF_ALT, "saved values not shown");
	DeleteFileW(file);
	printf("PASS settings window\n");
	return 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0); // keep every line if something fails or crashes
	// own channel name for this test and its child processes: never the real one of a running game/OBS
	if (!GetEnvironmentVariableW(L"ARCHAN_VRCAPTURE_TEST_CHANNEL", nullptr, 0))
		SetEnvironmentVariableW(L"ARCHAN_VRCAPTURE_TEST_CHANNEL", L"Local\\ArchanVRCapture_TestChannel");
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--quad") == 0)
			g_quad = true;
		if (strcmp(argv[i], "--right") == 0)
			g_right = true;
	}
	if (argc >= 2 && strcmp(argv[1], "--reader") == 0)
		return RunReader();
	if (argc >= 2 && strcmp(argv[1], "--idle") == 0) {
		Sleep(60000); // stands in for another running game
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "--crop") == 0)
		return RunCropTest();
	if (argc >= 2 && strcmp(argv[1], "--cleanup") == 0)
		return RunCleanupTest();
	if (argc >= 2 && strcmp(argv[1], "--logzip") == 0)
		return RunLogZipTest();
	if (argc >= 3 && strcmp(argv[1], "--settings-app") == 0)
		return RunSettingsAppTest(argv[2]);
	if (argc < 2) {
		printf("usage: layer_mock_test <layer.dll> [--d3d12] | --crop\n");
		return 2;
	}
	if (g_quad)
		printf("quad views mode: the mock game submits 4 views\n");
	if (argc >= 3 && strcmp(argv[2], "--cost") == 0)
		g_costTest = true;
	for (int i = 1; i < argc; i++)
		if (strcmp(argv[i], "--exit-early") == 0)
			g_exitEarly = true;
	if (argc >= 3 && strcmp(argv[2], "--overlay") == 0)
		return RunOverlayTest(argv[1], argc >= 4 && strcmp(argv[3], "--d3d12") == 0);
	if (argc >= 3 && strcmp(argv[2], "--d3d12") == 0)
		return RunWriterD3D12(argv[0], argv[1]);
	return RunWriter(argv[0], argv[1]);
}
