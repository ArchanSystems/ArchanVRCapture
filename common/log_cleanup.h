// Log folder cleanup (user decision 2026-11-05): on every OBS start (plugin load) and every game
// start (layer load), in %LOCALAPPDATA%\ArchanVRCapture\logs only:
//  - every file last written more than 48 h ago is deleted (head motion recordings poses-*.csv ...),
//  - layer.log (one file all game sessions append to) loses its lines older than 48 h.
// layer.log writers and this cleanup share a cross-process lock, so no line is lost while trimming.
#pragma once

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <string>

#define QVD_LOG_MAX_AGE_HOURS 48.0

// Cross-process lock for layer.log (all games with the layer, the cleanup in OBS and in games).
class QvdLogLock {
public:
	QvdLogLock()
	{
		static HANDLE h = CreateMutexW(nullptr, FALSE, L"Local\\ArchanVRCapture_LogFiles");
		h_ = h;
		if (h_) {
			const DWORD w = WaitForSingleObject(h_, 2000);
			owned_ = w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
		}
	}
	~QvdLogLock()
	{
		if (owned_)
			ReleaseMutex(h_);
	}
	QvdLogLock(const QvdLogLock &) = delete;
	QvdLogLock &operator=(const QvdLogLock &) = delete;

private:
	HANDLE h_ = nullptr;
	bool owned_ = false;
};

// %LOCALAPPDATA%\ArchanVRCapture\logs (created if missing); empty on failure.
inline std::wstring qvd_log_dir()
{
	std::wstring dir;
	PWSTR base = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
		dir = std::wstring(base) + L"\\ArchanVRCapture";
		CreateDirectoryW(dir.c_str(), nullptr);
		dir += L"\\logs";
		CreateDirectoryW(dir.c_str(), nullptr);
	}
	CoTaskMemFree(base);
	return dir;
}

struct QvdLogCleanup {
	int filesDeleted = 0;
	int filesFailed = 0; // too old but could not be deleted (e.g. still open)
	int linesRemoved = 0;
	int linesKept = 0;
};

inline unsigned long long qvd_ft64(FILETIME f)
{
	return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime;
}

// True for a line that starts with "YYYY-MM-DD HH:MM:SS" (the layer.log time stamp, local time).
inline bool qvd_has_stamp(const char *s, size_t n)
{
	if (n < 19)
		return false;
	static const char pat[] = "dddd-dd-dd dd:dd:dd";
	for (int i = 0; i < 19; i++)
		if (pat[i] == 'd' ? (s[i] < '0' || s[i] > '9') : s[i] != pat[i])
			return false;
	return true;
}

// dir = the logs folder. nowUtc = current time (tests pass their own).
inline QvdLogCleanup qvd_cleanup_logs(const std::wstring &dir, FILETIME nowUtc,
				      double maxAgeHours = QVD_LOG_MAX_AGE_HOURS)
{
	QvdLogCleanup r;
	if (dir.empty())
		return r;
	const unsigned long long now = qvd_ft64(nowUtc);
	const unsigned long long maxAge = (unsigned long long)(maxAgeHours * 3600.0 * 1e7);
	QvdLogLock lock;

	// files older than maxAge (layer.log is trimmed instead)
	WIN32_FIND_DATAW fd;
	HANDLE fh = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (fh != INVALID_HANDLE_VALUE) {
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			if (_wcsicmp(fd.cFileName, L"layer.log") == 0)
				continue;
			const unsigned long long w = qvd_ft64(fd.ftLastWriteTime);
			if (now > w && now - w > maxAge) {
				if (DeleteFileW((dir + L"\\" + fd.cFileName).c_str()))
					r.filesDeleted++;
				else
					r.filesFailed++;
			}
		} while (FindNextFileW(fh, &fd));
		FindClose(fh);
	}

	// layer.log: drop the lines older than maxAge; a line without a time stamp goes with the one before
	const std::wstring path = dir + L"\\layer.log";
	FILE *f = _wfopen(path.c_str(), L"rb");
	if (!f)
		return r;
	std::string text;
	char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		text.append(buf, n);
	fclose(f);

	FILETIME cutUtc, cutLocal;
	const unsigned long long cut = now > maxAge ? now - maxAge : 0;
	cutUtc.dwLowDateTime = (DWORD)cut;
	cutUtc.dwHighDateTime = (DWORD)(cut >> 32);
	SYSTEMTIME st;
	if (!FileTimeToLocalFileTime(&cutUtc, &cutLocal) || !FileTimeToSystemTime(&cutLocal, &st))
		return r;
	char cutStr[32];
	snprintf(cutStr, sizeof(cutStr), "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
		 st.wMinute, st.wSecond);

	std::string kept;
	kept.reserve(text.size());
	bool keep = true;
	size_t pos = 0;
	while (pos < text.size()) {
		size_t end = text.find('\n', pos);
		end = end == std::string::npos ? text.size() : end + 1;
		const char *line = text.data() + pos;
		if (qvd_has_stamp(line, end - pos))
			keep = strncmp(line, cutStr, 19) >= 0;
		if (keep) {
			kept.append(line, end - pos);
			r.linesKept++;
		} else {
			r.linesRemoved++;
		}
		pos = end;
	}
	if (r.linesRemoved == 0)
		return r;
	const std::wstring tmp = path + L".tmp";
	FILE *o = _wfopen(tmp.c_str(), L"wb");
	if (!o)
		return r;
	const bool ok = fwrite(kept.data(), 1, kept.size(), o) == kept.size();
	fclose(o);
	if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		DeleteFileW(tmp.c_str());
		r.linesKept += r.linesRemoved;
		r.linesRemoved = 0;
	}
	return r;
}

inline QvdLogCleanup qvd_cleanup_logs_now()
{
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	return qvd_cleanup_logs(qvd_log_dir(), now);
}
