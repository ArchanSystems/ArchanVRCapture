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
#include <string>
#include <vector>

#include "../common/qvd_shared.h"

// ---------------------------------------------------------------- test geometry
static const uint32_t TEX_W = 2300, TEX_H = 2400; // swapchain texture
static const int32_t RECT_X = 100, RECT_Y = 50;   // left view imageRect offset
static const uint32_t RECT_W = 2064, RECT_H = 2208;
static const uint32_t NUM_IMAGES = 3, FRAMES = 10;

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
static XrResult XRAPI_CALL mock_CreateSwapchain(XrSession, const XrSwapchainCreateInfo *, XrSwapchain *sc)
{
	*sc = (XrSwapchain)(uintptr_t)0x5C;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL mock_DestroySwapchain(XrSwapchain) { return XR_SUCCESS; }
static XrResult XRAPI_CALL mock_EnumerateSwapchainImages(XrSwapchain, uint32_t cap, uint32_t *count,
							  XrSwapchainImageBaseHeader *imgs)
{
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
static XrResult XRAPI_CALL mock_AcquireSwapchainImage(XrSwapchain, const XrSwapchainImageAcquireInfo *, uint32_t *idx)
{
	*idx = g_nextAcquire;
	g_nextAcquire = (g_nextAcquire + 1) % NUM_IMAGES;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL mock_ReleaseSwapchainImage(XrSwapchain, const XrSwapchainImageReleaseInfo *) { return XR_SUCCESS; }
static XrResult XRAPI_CALL mock_EndFrame(XrSession, const XrFrameEndInfo *)
{
	g_endFrames++;
	return XR_SUCCESS;
}

static XrResult XRAPI_CALL mock_GetInstanceProcAddr(XrInstance, const char *name, PFN_xrVoidFunction *fn)
{
#define M(x) if (strcmp(name, #x) == 0) { *fn = (PFN_xrVoidFunction)mock_##x; return XR_SUCCESS; }
	M(GetInstanceProperties) M(DestroyInstance) M(CreateSession) M(DestroySession) M(CreateSwapchain)
	M(DestroySwapchain) M(EnumerateSwapchainImages) M(AcquireSwapchainImage) M(ReleaseSwapchainImage) M(EndFrame)
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
	shm->readerTickMs = GetTickCount64();
	SetEvent(ready);

	// keep heartbeat alive until the writer is done
	while (WaitForSingleObject(done, 20) == WAIT_TIMEOUT)
		shm->readerTickMs = GetTickCount64();

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
			const uint32_t e = Expected(lastImage, 0, RECT_X + x, RECT_Y + y);
			if (row[x] != e) {
				if (bad < 5)
					printf("  mismatch at %u,%u got %08X expected %08X\n", x, y, row[x], e);
				bad++;
			}
		}
	}
	ctx->Unmap(staging, 0);
	printf("reader: checked %u x %u pixels against left eye (slice 0), image %u, rect offset %d,%d: %llu mismatches\n",
	       RECT_W, RECT_H, lastImage, RECT_X, RECT_Y, (unsigned long long)bad);
	CHECK(bad == 0, "pixel mismatches");
	return 0;
}

// ---------------------------------------------------------------- writer (plays the game)
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
	GET(xrDestroySwapchain, DestroySwapchain)
	GET(xrDestroySession, DestroySession)
	GET(xrDestroyInstance, DestroyInstance)
#undef GET

	XrGraphicsBindingD3D11KHR gb = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
	gb.device = g_dev;
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO, &gb};
	XrSession session;
	CHECK(CreateSession(inst, &sci, &session) == XR_SUCCESS, "session");

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
	auto submit = [&](void) -> XrResult {
		uint32_t idx;
		AcquireSwapchainImage(sc, nullptr, &idx);
		ReleaseSwapchainImage(sc, nullptr);
		XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
							      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
		for (uint32_t v = 0; v < 2; v++) {
			views[v].subImage.swapchain = sc;
			views[v].subImage.imageRect = {{RECT_X, RECT_Y}, {(int32_t)RECT_W, (int32_t)RECT_H}};
			views[v].subImage.imageArrayIndex = v;
		}
		XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		proj.viewCount = 2;
		proj.views = views;
		const XrCompositionLayerBaseHeader *layers[] = {(const XrCompositionLayerBaseHeader *)&proj};
		XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
		fei.layerCount = 1;
		fei.layers = layers;
		return EndFrame(session, &fei);
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

	// b) the live owner stops delivering (like PimaxHome: session but no picture) -> taken after ~2 s
	const uint64_t stopMs = GetTickCount64();
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

	// back to a clean state for the reader phase
	view->readerTickMs = 0;
	view->frameCount = 0;
	// reset image rotation so the frame math below is simple
	g_nextAcquire = 0;

	// 2) start the reader process
	HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, L"Local\\QVDTest_ReaderReady");
	HANDLE done = CreateEventW(nullptr, TRUE, FALSE, L"Local\\QVDTest_FramesDone");
	std::string cmd = std::string("\"") + exePath + "\" --reader";
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

	DestroySwapchain(sc);
	DestroySession(session);
	CHECK(view->sharedHandle == 0 && view->writerPid == 0, "session end did not clear the descriptor");
	printf("writer: session end cleared the descriptor (correct)\n");
	DestroyInstance(inst);
	CHECK(code == 0, "reader failed (exit %lu)", code);

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
	GET(xrDestroySwapchain, DestroySwapchain)
	GET(xrDestroySession, DestroySession)
	GET(xrDestroyInstance, DestroyInstance)
#undef GET

	XrGraphicsBindingD3D12KHR gb = {XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
	gb.device = dev12;
	gb.queue = queue;
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO, &gb};
	XrSession session;
	CHECK(CreateSession(inst, &sci, &session) == XR_SUCCESS, "session");

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
	std::string cmd = std::string("\"") + exePath + "\" --reader";
	STARTUPINFOA si = {sizeof(si)};
	PROCESS_INFORMATION pi;
	CHECK(CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi), "spawn reader");
	CHECK(WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0, "reader not ready");

	for (uint32_t f = 0; f < FRAMES; f++) {
		uint32_t idx;
		AcquireSwapchainImage(sc, nullptr, &idx);
		ReleaseSwapchainImage(sc, nullptr);
		XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
							      {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
		for (uint32_t v = 0; v < 2; v++) {
			views[v].subImage.swapchain = sc;
			views[v].subImage.imageRect = {{RECT_X, RECT_Y}, {(int32_t)RECT_W, (int32_t)RECT_H}};
			views[v].subImage.imageArrayIndex = v;
		}
		XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
		proj.viewCount = 2;
		proj.views = views;
		const XrCompositionLayerBaseHeader *layers[] = {(const XrCompositionLayerBaseHeader *)&proj};
		XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
		fei.layerCount = 1;
		fei.layers = layers;
		CHECK(EndFrame(session, &fei) == XR_SUCCESS, "endframe");
	}
	CHECK(WaitQueueIdle(dev12, queue), "copies did not finish on the game's queue");
	printf("writer (D3D12): %u frames submitted, runtime received %d EndFrame calls\n", FRAMES, g_endFrames);
	CHECK(g_endFrames == (int)FRAMES, "EndFrame not forwarded every time");
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

int main(int argc, char **argv)
{
	// own channel name for this test and its child processes: never the real one of a running game/OBS
	if (!GetEnvironmentVariableW(L"ARCHAN_VRCAPTURE_TEST_CHANNEL", nullptr, 0))
		SetEnvironmentVariableW(L"ARCHAN_VRCAPTURE_TEST_CHANNEL", L"Local\\ArchanVRCapture_TestChannel");
	if (argc >= 2 && strcmp(argv[1], "--reader") == 0)
		return RunReader();
	if (argc >= 2 && strcmp(argv[1], "--idle") == 0) {
		Sleep(60000); // stands in for another running game
		return 0;
	}
	if (argc >= 2 && strcmp(argv[1], "--crop") == 0)
		return RunCropTest();
	if (argc < 2) {
		printf("usage: layer_mock_test <layer.dll> [--d3d12] | --crop\n");
		return 2;
	}
	if (argc >= 3 && strcmp(argv[2], "--d3d12") == 0)
		return RunWriterD3D12(argv[0], argv[1]);
	return RunWriter(argv[0], argv[1]);
}
