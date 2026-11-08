// "Save logs" (user decision 2026-11-08): one zip on the Desktop that a user drops in Discord or an email.
// It holds our layer.log and the OBS logs written in the last 48 h - nothing else, nothing is uploaded.
// Files are read with full sharing, so the log OBS is writing right now and a layer.log a running game
// appends to are read as they are. The zip stores the files uncompressed (method 0): no library needed,
// and every Windows / 7-Zip / tar opens it.
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct QvdZipEntry {
	std::string name; // inside the zip, '/' separated, ASCII
	std::string data;
	FILETIME written; // UTC
};

struct QvdLogZipResult {
	bool ok = false;
	int files = 0;
	unsigned long long bytes = 0; // size of the zip
	std::string error;            // when !ok
	std::vector<std::string> names;
	std::vector<std::string> skipped; // files that could not be read, with the Windows error
};

inline uint32_t qvd_crc32(const std::string &d)
{
	static uint32_t table[256];
	static bool made = false;
	if (!made) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		made = true;
	}
	uint32_t c = 0xFFFFFFFFu;
	for (unsigned char b : d)
		c = table[(c ^ b) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

// Reads a whole file that another process may be writing (shared read/write/delete).
inline bool qvd_read_shared(const std::wstring &path, std::string &out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;
	out.clear();
	char buf[65536];
	DWORD n = 0;
	bool ok = true;
	while (true) {
		if (!ReadFile(h, buf, sizeof(buf), &n, nullptr)) {
			ok = false;
			break;
		}
		if (n == 0)
			break;
		out.append(buf, n);
	}
	CloseHandle(h);
	return ok;
}

inline std::string qvd_narrow(const std::wstring &w)
{
	std::string s;
	for (wchar_t c : w)
		s += (c >= 32 && c < 127) ? (char)c : '_';
	return s;
}

// Writes a stored (uncompressed) zip. Files over 4 GB are not supported (no zip64) and are refused.
inline bool qvd_write_zip(const std::wstring &path, const std::vector<QvdZipEntry> &entries, std::string &error)
{
	auto le16 = [](std::string &b, uint16_t v) {
		b += (char)(v & 0xFF);
		b += (char)(v >> 8);
	};
	auto le32 = [](std::string &b, uint32_t v) {
		for (int i = 0; i < 4; i++)
			b += (char)((v >> (8 * i)) & 0xFF);
	};
	std::string out, central;
	for (const QvdZipEntry &e : entries) {
		if (e.data.size() >= 0xFFFFFFFFull || out.size() >= 0xFFFFFFFFull) {
			error = "a file is too large for this zip: " + e.name;
			return false;
		}
		FILETIME local;
		SYSTEMTIME st;
		uint16_t dosTime = 0, dosDate = (1 << 5) | 1; // 1980-01-01
		if (FileTimeToLocalFileTime(&e.written, &local) && FileTimeToSystemTime(&local, &st) && st.wYear >= 1980) {
			dosTime = (uint16_t)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
			dosDate = (uint16_t)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
		}
		const uint32_t crc = qvd_crc32(e.data), size = (uint32_t)e.data.size(), offset = (uint32_t)out.size();
		le32(out, 0x04034b50); // local file header
		le16(out, 20);         // version needed
		le16(out, 0);          // flags
		le16(out, 0);          // method: stored
		le16(out, dosTime);
		le16(out, dosDate);
		le32(out, crc);
		le32(out, size);
		le32(out, size);
		le16(out, (uint16_t)e.name.size());
		le16(out, 0);
		out += e.name;
		out += e.data;

		le32(central, 0x02014b50); // central directory header
		le16(central, 20);         // made by
		le16(central, 20);         // needed
		le16(central, 0);
		le16(central, 0);
		le16(central, dosTime);
		le16(central, dosDate);
		le32(central, crc);
		le32(central, size);
		le32(central, size);
		le16(central, (uint16_t)e.name.size());
		le16(central, 0); // extra
		le16(central, 0); // comment
		le16(central, 0); // disk
		le16(central, 0); // internal attributes
		le32(central, 0); // external attributes
		le32(central, offset);
		central += e.name;
	}
	const uint32_t centralOffset = (uint32_t)out.size();
	out += central;
	le32(out, 0x06054b50); // end of central directory
	le16(out, 0);
	le16(out, 0);
	le16(out, (uint16_t)entries.size());
	le16(out, (uint16_t)entries.size());
	le32(out, (uint32_t)central.size());
	le32(out, centralOffset);
	le16(out, 0);

	FILE *f = _wfopen(path.c_str(), L"wb");
	if (!f) {
		error = "the zip could not be created (error " + std::to_string(errno) + ")";
		return false;
	}
	const bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
	if (fclose(f) != 0 || !ok) {
		DeleteFileW(path.c_str());
		error = "the zip could not be written completely";
		return false;
	}
	return true;
}

// layerLogDir = %LOCALAPPDATA%\ArchanVRCapture\logs (its layer.log), obsLogDir = OBS's logs folder, obsCrashDir = OBS's
// crash reports folder (user decision 2026-11-08: "include it in our logs"); from both OBS folders the files written
// in the last maxAgeHours. Writes zipPath. nowUtc: tests pass their own.
inline QvdLogZipResult qvd_save_logs_zip(const std::wstring &layerLogDir, const std::wstring &obsLogDir,
					 const std::wstring &zipPath, FILETIME nowUtc, double maxAgeHours = 48.0,
					 const std::wstring &obsCrashDir = std::wstring())
{
	QvdLogZipResult r;
	std::vector<QvdZipEntry> entries;
	const unsigned long long now = ((unsigned long long)nowUtc.dwHighDateTime << 32) | nowUtc.dwLowDateTime;
	const unsigned long long maxAge = (unsigned long long)(maxAgeHours * 3600.0 * 1e7);

	if (!layerLogDir.empty()) {
		const std::wstring p = layerLogDir + L"\\layer.log";
		WIN32_FILE_ATTRIBUTE_DATA a;
		QvdZipEntry e;
		if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) && qvd_read_shared(p, e.data)) {
			e.name = "ArchanVRCapture/layer.log";
			e.written = a.ftLastWriteTime;
			entries.push_back(std::move(e));
		} else {
			r.skipped.push_back(qvd_narrow(p) + " (error " + std::to_string(GetLastError()) + ")");
		}
	}
	// the recent files of one folder, as prefix + name; a folder that does not exist (OBS never crashed) is no error
	auto addFolder = [&](const std::wstring &dir, const char *prefix) {
		if (dir.empty())
			return;
		WIN32_FIND_DATAW fd;
		HANDLE fh = FindFirstFileW((dir + L"\\*").c_str(), &fd);
		if (fh == INVALID_HANDLE_VALUE) {
			const DWORD err = GetLastError();
			if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
				r.skipped.push_back(qvd_narrow(dir) + " (error " + std::to_string(err) + ")");
			return;
		}
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			const unsigned long long w =
				((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
			if (now > w && now - w > maxAge)
				continue;
			QvdZipEntry e;
			if (!qvd_read_shared(dir + L"\\" + fd.cFileName, e.data)) {
				r.skipped.push_back(qvd_narrow(fd.cFileName) + " (error " + std::to_string(GetLastError()) + ")");
				continue;
			}
			e.name = std::string(prefix) + qvd_narrow(fd.cFileName);
			e.written = fd.ftLastWriteTime;
			entries.push_back(std::move(e));
		} while (FindNextFileW(fh, &fd));
		FindClose(fh);
	};
	addFolder(obsLogDir, "OBS/");
	addFolder(obsCrashDir, "OBS crashes/");
	if (entries.empty()) {
		r.error = "no logs were found (no layer.log and no OBS log from the last 48 hours)";
		return r;
	}
	if (!qvd_write_zip(zipPath, entries, r.error))
		return r;
	WIN32_FILE_ATTRIBUTE_DATA za;
	if (GetFileAttributesExW(zipPath.c_str(), GetFileExInfoStandard, &za))
		r.bytes = ((unsigned long long)za.nFileSizeHigh << 32) | za.nFileSizeLow;
	r.files = (int)entries.size();
	for (const QvdZipEntry &e : entries)
		r.names.push_back(e.name);
	r.ok = true;
	return r;
}
