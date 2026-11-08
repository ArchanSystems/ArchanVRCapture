// Archan VR Capture - performance overlay settings (user decisions 2026-11-06/07): the key (chosen in the
// OBS source's Properties or in the settings window) and the overlay on/off switch (settings window), saved
// to %LOCALAPPDATA%\ArchanVRCapture\overlay.ini and read by the layer in the game, so they work with OBS
// closed too. Key text form: modifiers and one key joined by '+', e.g. "Ctrl+Shift+F", "Alt+F10", "F11".
// Requires <windows.h> before this header.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <locale.h>

#define QVD_OVERLAY_DEFAULT_KEY "Ctrl+Shift+F" /* user decision 2026-11-06 */

struct QvdKey {
	uint32_t vk; /* 0 = none */
	bool ctrl, shift, alt;
};

struct QvdKeyName {
	const char *name;
	uint32_t vk;
};

static const QvdKeyName kQvdKeyNames[] = {
	{"Space", VK_SPACE},      {"Tab", VK_TAB},          {"Insert", VK_INSERT},      {"Delete", VK_DELETE},
	{"Home", VK_HOME},        {"End", VK_END},          {"PageUp", VK_PRIOR},       {"PageDown", VK_NEXT},
	{"Up", VK_UP},            {"Down", VK_DOWN},        {"Left", VK_LEFT},          {"Right", VK_RIGHT},
	{"Pause", VK_PAUSE},      {"ScrollLock", VK_SCROLL}, {"Num*", VK_MULTIPLY},     {"Num+", VK_ADD},
	{"Num-", VK_SUBTRACT},    {"Num/", VK_DIVIDE},      {"Num.", VK_DECIMAL},
};

/* Parses "Ctrl+Shift+F". Case and spaces do not matter. Exactly one non-modifier key. */
static inline bool qvd_parse_key(const char *text, QvdKey *out)
{
	QvdKey k = {0, false, false, false};
	if (!text)
		return false;
	char buf[128];
	size_t n = 0;
	for (const char *p = text; *p && n + 1 < sizeof(buf); p++)
		if (!isspace((unsigned char)*p))
			buf[n++] = *p;
	buf[n] = 0;
	if (n == 0)
		return false;
	char *part = buf;
	for (;;) {
		/* "Num+" contains '+': a part ending in "Num" followed by '+' is that key */
		char *plus = strchr(part, '+');
		if (plus && plus - part == 3 && _strnicmp(part, "Num", 3) == 0)
			plus = strchr(plus + 1, '+');
		if (plus)
			*plus = 0;
		if (!*part)
			return false;
		if (_stricmp(part, "Ctrl") == 0 || _stricmp(part, "Control") == 0) {
			k.ctrl = true;
		} else if (_stricmp(part, "Shift") == 0) {
			k.shift = true;
		} else if (_stricmp(part, "Alt") == 0) {
			k.alt = true;
		} else {
			if (k.vk)
				return false; /* two keys */
			const size_t len = strlen(part);
			uint32_t vk = 0;
			if (len == 1 && isalnum((unsigned char)part[0])) {
				vk = (uint32_t)toupper((unsigned char)part[0]); /* VK_A..VK_Z, VK_0..VK_9 = ASCII */
			} else if ((part[0] == 'F' || part[0] == 'f') && len >= 2 && len <= 3 && isdigit((unsigned char)part[1]) &&
				   (len == 2 || isdigit((unsigned char)part[2]))) {
				const int f = atoi(part + 1);
				if (f >= 1 && f <= 24)
					vk = VK_F1 + (uint32_t)(f - 1);
			} else if (len == 4 && _strnicmp(part, "Num", 3) == 0 && isdigit((unsigned char)part[3])) {
				vk = VK_NUMPAD0 + (uint32_t)(part[3] - '0');
			} else {
				for (const QvdKeyName &kn : kQvdKeyNames)
					if (_stricmp(part, kn.name) == 0)
						vk = kn.vk;
			}
			if (!vk)
				return false;
			k.vk = vk;
		}
		if (!plus)
			break;
		part = plus + 1;
	}
	if (!k.vk)
		return false;
	*out = k;
	return true;
}

/* Canonical text, e.g. "Ctrl+Shift+F". */
static inline void qvd_format_key(const QvdKey &k, char *out, size_t size)
{
	char key[16] = "?";
	if ((k.vk >= 'A' && k.vk <= 'Z') || (k.vk >= '0' && k.vk <= '9'))
		snprintf(key, sizeof(key), "%c", (char)k.vk);
	else if (k.vk >= VK_F1 && k.vk <= VK_F24)
		snprintf(key, sizeof(key), "F%u", k.vk - VK_F1 + 1);
	else if (k.vk >= VK_NUMPAD0 && k.vk <= VK_NUMPAD9)
		snprintf(key, sizeof(key), "Num%u", k.vk - VK_NUMPAD0);
	else
		for (const QvdKeyName &kn : kQvdKeyNames)
			if (kn.vk == k.vk)
				snprintf(key, sizeof(key), "%s", kn.name);
	snprintf(out, size, "%s%s%s%s", k.ctrl ? "Ctrl+" : "", k.shift ? "Shift+" : "", k.alt ? "Alt+" : "", key);
}

/* %LOCALAPPDATA%\ArchanVRCapture\overlay.ini. Tests set ARCHAN_VRCAPTURE_TEST_SETTINGS to their own file. */
static inline const wchar_t *qvd_overlay_settings_path(void)
{
	static wchar_t path[MAX_PATH];
	if (!path[0]) {
		const DWORD n = GetEnvironmentVariableW(L"ARCHAN_VRCAPTURE_TEST_SETTINGS", path, MAX_PATH);
		if (n == 0 || n >= MAX_PATH) {
			wchar_t dir[MAX_PATH];
			ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\ArchanVRCapture", dir, MAX_PATH);
			CreateDirectoryW(dir, nullptr);
			swprintf_s(path, MAX_PATH, L"%s\\overlay.ini", dir);
		}
	}
	return path;
}

/* The file:  key=Ctrl+Shift+F
 *            enabled=1
 *            place=0.0000,-0.1000,-1.0000,0.00     (only once the panel has been moved)
 * key: the text as written (check it with qvd_parse_key). Missing file or line: the default key / OFF.
 * place: where the headset panel stands in the game's seated space (OpenXR LOCAL): x, y, z in metres and
 * the pitch in degrees (user decision 2026-11-07: fixed in the cockpit, moved with the ArchanXR keys while
 * unlocked with Ctrl+Shift+L, saved). Written by the layer; the others keep it as it is. */
struct QvdOverlaySettings {
	char key[64];
	bool enabled;
	bool hasPlace;
	float place[4];
};

/* numbers always with a '.' whatever the process's locale */
static inline _locale_t qvd_c_locale(void)
{
	static _locale_t loc = _create_locale(LC_NUMERIC, "C");
	return loc;
}

/* false: the file exists but cannot be opened right now (being replaced) - try again later. */
static inline bool qvd_overlay_read(QvdOverlaySettings *s, bool *exists)
{
	strcpy_s(s->key, sizeof(s->key), QVD_OVERLAY_DEFAULT_KEY);
	s->enabled = false; /* off by default (user decision 2026-11-07) */
	s->hasPlace = false;
	s->place[0] = s->place[1] = s->place[2] = s->place[3] = 0.0f;
	*exists = GetFileAttributesW(qvd_overlay_settings_path()) != INVALID_FILE_ATTRIBUTES;
	if (!*exists)
		return true;
	FILE *f = _wfopen(qvd_overlay_settings_path(), L"r");
	if (!f)
		return false;
	char line[160];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (_strnicmp(line, "key=", 4) == 0)
			strcpy_s(s->key, sizeof(s->key), line + 4);
		else if (_strnicmp(line, "enabled=", 8) == 0)
			s->enabled = atoi(line + 8) != 0;
		else if (_strnicmp(line, "place=", 6) == 0) {
			float p[4];
			if (_sscanf_l(line + 6, "%f,%f,%f,%f", qvd_c_locale(), &p[0], &p[1], &p[2], &p[3]) == 4) {
				memcpy(s->place, p, sizeof(p));
				s->hasPlace = true;
			}
		}
	}
	fclose(f);
	return true;
}

/* Replaced in one step (temp file + rename), so a reader never sees half a file. */
static inline bool qvd_overlay_write(const QvdOverlaySettings &s)
{
	wchar_t tmp[MAX_PATH + 8];
	swprintf_s(tmp, MAX_PATH + 8, L"%s.tmp", qvd_overlay_settings_path());
	FILE *f = _wfopen(tmp, L"w");
	if (!f)
		return false;
	fprintf(f, "key=%s\nenabled=%d\n", s.key, s.enabled ? 1 : 0);
	if (s.hasPlace)
		_fprintf_l(f, "place=%.4f,%.4f,%.4f,%.2f\n", qvd_c_locale(), s.place[0], s.place[1], s.place[2], s.place[3]);
	fclose(f);
	for (int i = 0; i < 20; i++) { /* the game may be reading it this very moment */
		if (MoveFileExW(tmp, qvd_overlay_settings_path(), MOVEFILE_REPLACE_EXISTING))
			return true;
		Sleep(10);
	}
	DeleteFileW(tmp);
	return false;
}
