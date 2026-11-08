// Archan VR Capture - shared protocol between the OpenXR layer (inside the game)
// and the OBS source plugin. Picture only: the left eye image of the game's
// projection layer, copied into a D3D11 shared texture with a keyed mutex.
#pragma once
#include <stdint.h>
#include <stdio.h>

#define QVD_SHM_NAME L"Local\\ArchanVRCapture_Picture_v4"

/* Channel name. Tests set ARCHAN_VRCAPTURE_TEST_CHANNEL in their own processes so they never
 * touch the real channel of a running game or OBS. Requires <windows.h> before this header. */
static inline const wchar_t *qvd_channel_name(void)
{
	static wchar_t name[128];
	if (!name[0]) {
		const DWORD n = GetEnvironmentVariableW(L"ARCHAN_VRCAPTURE_TEST_CHANNEL", name, 128);
		if (n == 0 || n >= 128)
			wcscpy_s(name, 128, QVD_SHM_NAME);
	}
	return name;
}
/* "OBS is running": the OBS plugin holds a mutex with this name while OBS runs (user decision 2026-11-07:
 * the overlay is shown by default while OBS runs). Derived from the channel name, so tests stay apart. */
static inline const wchar_t *qvd_obs_presence_name(void)
{
	static wchar_t name[160];
	if (!name[0])
		swprintf_s(name, 160, L"%s_OBS", qvd_channel_name());
	return name;
}

#define QVD_MAGIC 0x31445651u /* "QVD1" */
#define QVD_VERSION 5u /* v2: per-frame pose for stabilization; v3: quad views inner view; v4: performance overlay;
                          v5: left / right eye */

/* Performance overlay panel (v4): RGBA8, premultiplied alpha, top row first. */
#define QVD_OVERLAY_W 640u
#define QVD_OVERLAY_H 196u
/* OBS stops drawing the panel when the layer has not updated it for this long (it updates once a second). */
#define QVD_OVERLAY_TIMEOUT_MS 3000ull

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

	/* v2: pose of the copied left eye image, for stabilization. Written while the writer
	 * holds the keyed mutex, so a reader holding the mutex gets the pose of the texture
	 * content. OpenXR conventions: x right, y up, -z forward (the view looks along -z). */
	int64_t displayTimeNs; /* predicted display time of the frame (XrTime, nanoseconds) */
	float poseOrient[4];   /* x y z w: view 0 orientation in the projection layer's space */
	float posePos[3];
	float fovTan[4]; /* tan(angleLeft) tan(angleRight) tan(angleUp) tan(angleDown) of the image */
	float up[3];     /* world up (against gravity) in that space; valid with QVD_POSE_UP */
	uint32_t poseFlags;

	/* v3: quad views (foveated rendering, 4 views): the sharp inner left view (view 2), copied into
	 * its own shared texture. Handle/size/format are descriptor fields (seqlock, like the above); the
	 * inner view's FOV and orientation are per frame, written while the writer holds BOTH keyed
	 * mutexes (main first, then inner), valid with QVD_POSE_INNER. */
	uint32_t innerHandle; /* legacy D3D11 shared handle, 0 = no inner view */
	uint32_t innerWidth;
	uint32_t innerHeight;
	uint32_t innerDxgiFormat;
	float innerFovTan[4];     /* tan(left) tan(right) tan(up) tan(down) of the inner image */
	float innerPoseOrient[4]; /* x y z w, same space as poseOrient */

	/* v4: performance overlay (user decision 2026-11-06: toggled by a key in the game, shown in the headset
	 * and in the recording). overlayVisible: the game's overlay is on. The panel pixels are written when they
	 * change (once a second) under their own seqlock overlaySeq (odd while writing). */
	volatile uint32_t overlayVisible;
	volatile uint32_t overlaySeq;
	volatile uint64_t overlayTickMs; /* GetTickCount64() at the last panel write */
	uint8_t overlayPixels[QVD_OVERLAY_W * QVD_OVERLAY_H * 4];

	/* v5: which eye (user decision 2026-11-07: an option in the OBS source). wantEye: written by OBS with
	 * readerTickMs, 0 = left (views 0 and inner 2), 1 = right (views 1 and inner 3). picEye: the eye of the
	 * texture content, written by the layer with the pose (under the keyed mutex). */
	volatile uint32_t wantEye;
	volatile uint32_t picEye;
};
#pragma pack(pop)

enum QvdPoseFlags : uint32_t {
	QVD_POSE_VALID = 1u, /* poseOrient/fovTan/displayTimeNs belong to the current texture */
	QVD_POSE_UP = 2u,    /* up[] is known (LOCAL/STAGE/LOCAL_FLOOR reference space) */
	QVD_POSE_INNER = 4u, /* innerFovTan/innerPoseOrient belong to the current inner texture */
};

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
