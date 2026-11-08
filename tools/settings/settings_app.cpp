// Archan VR Capture Settings - the small settings window (user decision 2026-11-07), for everyone and
// especially the "Overlay only" install (no OBS): the performance overlay on/off switch and its key.
// Writes %LOCALAPPDATA%\ArchanVRCapture\overlay.ini (common/overlay_key.h); the layer in a running game
// picks a change up within half a second. The key field is Windows' hotkey control: press the keys.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>

#include <cstdio>
#include <cstring>

#include "../../common/overlay_key.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
			"version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// control ids (the settings test drives the window through them)
enum { ID_ENABLED = 101, ID_KEY = 102, ID_SAVE = 103, ID_CLOSE = 104, ID_STATUS = 105 };

static HFONT g_font = nullptr;
static int g_dpi = 96;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static HWND Add(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id)
{
	HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), parent,
				 (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
	SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
	return c;
}

static void SetStatus(HWND w, const wchar_t *text) { SetDlgItemTextW(w, ID_STATUS, text); }

// the hotkey control's value <-> our key
static WORD ToHotkey(const QvdKey &k)
{
	BYTE mods = 0;
	if (k.ctrl)
		mods |= HOTKEYF_CONTROL;
	if (k.shift)
		mods |= HOTKEYF_SHIFT;
	if (k.alt)
		mods |= HOTKEYF_ALT;
	if (k.vk == VK_INSERT || k.vk == VK_DELETE || k.vk == VK_HOME || k.vk == VK_END || k.vk == VK_PRIOR ||
	    k.vk == VK_NEXT || k.vk == VK_UP || k.vk == VK_DOWN || k.vk == VK_LEFT || k.vk == VK_RIGHT || k.vk == VK_DIVIDE)
		mods |= HOTKEYF_EXT;
	return MAKEWORD((BYTE)k.vk, mods);
}

static bool FromHotkey(WORD hk, QvdKey *k, char *text, size_t size)
{
	const BYTE vk = LOBYTE(hk), mods = HIBYTE(hk);
	QvdKey t = {vk, (mods & HOTKEYF_CONTROL) != 0, (mods & HOTKEYF_SHIFT) != 0, (mods & HOTKEYF_ALT) != 0};
	if (!vk)
		return false;
	qvd_format_key(t, text, size);
	QvdKey back;
	if (!qvd_parse_key(text, &back) || back.vk != t.vk) // a key our parser does not know (e.g. ';')
		return false;
	*k = t;
	return true;
}

static void Load(HWND w)
{
	QvdOverlaySettings st;
	bool exists = false;
	for (int i = 0; i < 20 && !qvd_overlay_read(&st, &exists); i++)
		Sleep(10);
	CheckDlgButton(w, ID_ENABLED, st.enabled ? BST_CHECKED : BST_UNCHECKED);
	QvdKey k;
	if (!qvd_parse_key(st.key, &k))
		qvd_parse_key(QVD_OVERLAY_DEFAULT_KEY, &k);
	SendDlgItemMessageW(w, ID_KEY, HKM_SETHOTKEY, ToHotkey(k), 0);
	SetStatus(w, exists ? L"" : L"Not saved yet: these are the defaults.");
}

static void Save(HWND w)
{
	QvdKey k;
	char text[64];
	const WORD hk = (WORD)SendDlgItemMessageW(w, ID_KEY, HKM_GETHOTKEY, 0, 0);
	if (!FromHotkey(hk, &k, text, sizeof(text))) {
		SetStatus(w, L"This key cannot be used. Use a letter, a digit, F1-F24, Num0-Num9, Insert, Delete, Home, "
			     L"End, Page Up/Down, an arrow key or Pause, with Ctrl / Shift / Alt if you like.");
		return;
	}
	QvdOverlaySettings st;
	bool exists = false;
	for (int i = 0; i < 20 && !qvd_overlay_read(&st, &exists); i++)
		Sleep(10);
	strcpy_s(st.key, sizeof(st.key), text);
	st.enabled = IsDlgButtonChecked(w, ID_ENABLED) == BST_CHECKED;
	wchar_t msg[200];
	if (qvd_overlay_write(st))
		swprintf_s(msg, L"Saved: overlay %s, key %hs. A running game uses it within a second.", st.enabled ? L"on" : L"off",
			   text);
	else
		swprintf_s(msg, L"Could not save %s (error %lu).", qvd_overlay_settings_path(), GetLastError());
	SetStatus(w, msg);
}

static LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
	switch (m) {
	case WM_CREATE: {
		Add(w, L"BUTTON", L"Performance overlay on", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 14, 380, 24, ID_ENABLED);
		Add(w, L"STATIC", L"Key to show or hide it in the game (click the box, then press the keys):", 0, 16, 46, 420, 40, 0);
		Add(w, HOTKEY_CLASSW, L"", WS_BORDER | WS_TABSTOP, 16, 88, 220, 26, ID_KEY);
		Add(w, L"STATIC",
		    L"Shows FPS, CPU, GPU and the time Archan adds per frame, in the headset (OpenXR games). "
		    L"While OBS runs with Archan VR Capture it is shown by itself and also in the recording; "
		    L"the key hides or shows it.",
		    0, 16, 126, 420, 76, 0);
		Add(w, L"STATIC",
		    L"In the headset it sits in your view where DCS shows its own performance graph, and moves with your "
		    L"head.",
		    0, 16, 206, 420, 58, 0);
		Add(w, L"STATIC", L"", 0, 16, 270, 420, 40, ID_STATUS);
		Add(w, L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 256, 318, 86, 28, ID_SAVE);
		Add(w, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, 350, 318, 86, 28, ID_CLOSE);
		SendDlgItemMessageW(w, ID_KEY, HKM_SETRULES, 0, 0); // any combination, also a single F key
		Load(w);
		return 0;
	}
	case WM_COMMAND:
		if (LOWORD(wp) == ID_SAVE)
			Save(w);
		else if (LOWORD(wp) == ID_CLOSE || LOWORD(wp) == IDCANCEL)
			DestroyWindow(w);
		return 0;
	case WM_CTLCOLORSTATIC:
		SetBkMode((HDC)wp, TRANSPARENT);
		return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(w, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES};
	InitCommonControlsEx(&icc);
	g_dpi = (int)GetDpiForSystem();
	NONCLIENTMETRICSW ncm = {sizeof(ncm)};
	SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, (UINT)g_dpi);
	g_font = CreateFontIndirectW(&ncm.lfMessageFont);

	WNDCLASSEXW wc = {sizeof(wc)};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
	wc.lpszClassName = L"ArchanVRCaptureSettings";
	wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
	RegisterClassExW(&wc);
	RECT r = {0, 0, S(452), S(360)};
	const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
	AdjustWindowRectExForDpi(&r, style, FALSE, 0, (UINT)g_dpi);
	HWND w = CreateWindowExW(0, wc.lpszClassName, L"Archan VR Capture Settings", style, CW_USEDEFAULT, CW_USEDEFAULT,
				 r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
	if (!w)
		return 1;
	ShowWindow(w, show);
	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
		if (!IsDialogMessageW(w, &msg)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}
	DeleteObject(g_font);
	return 0;
}
