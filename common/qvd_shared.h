// VR Capture - shared protocol between the OpenXR layer (inside the game)
// and the OBS source plugin. Picture only: the left eye image of the game's
// projection layer, copied into a D3D11 shared texture with a keyed mutex.
#pragma once
#include <stdint.h>

#define QVD_SHM_NAME L"Local\\VRCapture_Picture_v1"

/* Channel name. Tests set VRCAPTURE_TEST_CHANNEL in their own processes so they never
 * touch the real channel of a running game or OBS. Requires <windows.h> before this header. */
static inline const wchar_t *qvd_channel_name(void)
{
	static wchar_t name[128];
	if (!name[0]) {
		const DWORD n = GetEnvironmentVariableW(L"VRCAPTURE_TEST_CHANNEL", name, 128);
		if (n == 0 || n >= 128)
			wcscpy_s(name, 128, QVD_SHM_NAME);
	}
	return name;
}
#define QVD_MAGIC 0x31445651u /* "QVD1" */
#define QVD_VERSION 1u

/* Writer stops copying when OBS has not rendered the source for this long. */
#define QVD_READER_TIMEOUT_MS 2000ull

enum QvdFlags : uint32_t {
	QVD_FLAG_SRGB_FORMAT = 1u,  /* app swapchain format is *_SRGB: bytes are sRGB encoded */
	QVD_FLAG_LINEAR_FLOAT = 2u, /* float format: values are linear light */
};

#pragma pack(push, 8)
struct QvdShared {
	uint32_t magic;
	uint32_t version;
	/* seqlock: odd while the writer changes the descriptor below */
	volatile uint32_t seq;
	uint32_t sharedHandle; /* legacy D3D11 shared handle, 0 = no picture */
	uint32_t width;        /* size of the shared texture = left eye imageRect */
	uint32_t height;
	uint32_t dxgiFormat;      /* DXGI format of the shared texture */
	uint32_t swapchainFormat; /* DXGI format the app asked for in xrCreateSwapchain */
	uint32_t flags;           /* QvdFlags */
	uint32_t writerPid;
	volatile uint64_t frameCount;   /* +1 for every frame copied */
	volatile uint64_t writerTickMs; /* GetTickCount64() at last copy */
	volatile uint64_t readerTickMs; /* GetTickCount64() at last OBS render */
	char appName[128];
	char runtimeName[128];
};
#pragma pack(pop)

/* Centre 16:9 rectangle inside a w x h image.
 * Exactly 16:9: width is a multiple of 32, height = width * 9 / 16 (a multiple of 18).
 * That also meets OBS's output alignment (width multiple of 4, height even),
 * so the crop size can be used directly as the OBS canvas/output size (1:1 pixels). */
struct QvdRect {
	uint32_t x, y, w, h;
};

static inline QvdRect qvd_center_crop_16x9(uint32_t w, uint32_t h)
{
	QvdRect r = {0, 0, w, h};
	if (w < 32 || h < 18)
		return r;
	if ((uint64_t)w * 9u <= (uint64_t)h * 16u) {
		/* image is taller than 16:9 (VR eyes are): use (almost) the full width */
		r.w = w & ~31u;
		r.h = r.w / 16u * 9u;
	} else {
		r.h = (h / 18u) * 18u;
		r.w = r.h / 9u * 16u;
	}
	r.x = (w - r.w) / 2u;
	r.y = (h - r.h) / 2u;
	return r;
}
