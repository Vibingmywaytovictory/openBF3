#include "bf3/core/install_snapshot.h"

#include "win32_text.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cwctype>

namespace bf3::core {

namespace {

bool ends_with_ci(const std::wstring &p_name, const wchar_t *p_suffix) {
	const size_t n = wcslen(p_suffix);
	if (p_name.size() < n) {
		return false;
	}
	return CompareStringOrdinal(p_name.c_str() + (p_name.size() - n), static_cast<int>(n), p_suffix, static_cast<int>(n), TRUE) == CSTR_EQUAL;
}

struct Scanner {
	std::filesystem::path root;
	InstallSnapshot *snap = nullptr;

	void add(const std::wstring &p_rel, const WIN32_FIND_DATAW &p_fd) {
		FileStamp s;
		std::wstring rel = p_rel;
		std::replace(rel.begin(), rel.end(), L'\\', L'/');
		s.relative = detail::narrow(std::wstring_view(rel));
		s.size = (static_cast<uint64_t>(p_fd.nFileSizeHigh) << 32) | p_fd.nFileSizeLow;
		s.mtime = (static_cast<uint64_t>(p_fd.ftLastWriteTime.dwHighDateTime) << 32) | p_fd.ftLastWriteTime.dwLowDateTime;
		snap->total_bytes += s.size;
		snap->newest_mtime = std::max(snap->newest_mtime, s.mtime);
		snap->files.push_back(std::move(s));
	}

	// Lists one directory: files matching the filter, and recursion into sub-directories if asked.
	template <typename Filter>
	void scan(const std::wstring &p_rel_dir, bool p_recurse, Filter p_filter) {
		const std::wstring pattern = (root / p_rel_dir / L"*").native();
		WIN32_FIND_DATAW fd{};
		HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
		if (h == INVALID_HANDLE_VALUE) {
			return;
		}
		do {
			const std::wstring name = fd.cFileName;
			if (name == L"." || name == L"..") {
				continue;
			}
			const std::wstring rel = p_rel_dir.empty() ? name : p_rel_dir + L"\\" + name;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
				if (p_recurse && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
					scan(rel, true, p_filter);
				}
			} else if (p_filter(name)) {
				add(rel, fd);
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}

	void scan_package(const std::wstring &p_package) { // "" (base game) or "Update\\Patch"
		const std::wstring data = p_package.empty() ? std::wstring(L"Data") : p_package + L"\\Data";
		scan(data, false, [](const std::wstring &n) { return ends_with_ci(n, L"cas.cat") || ends_with_ci(n, L".toc"); }); // cas.cat, layout.toc
		scan(data + L"\\Win32", true, [](const std::wstring &n) { return ends_with_ci(n, L".toc") || ends_with_ci(n, L".sb"); });
		if (!p_package.empty()) {
			scan(p_package, false, [](const std::wstring &n) { return ends_with_ci(n, L"package.mft"); });
		}
	}
};

} // namespace

std::string SnapshotDiff::summary() const {
	if (clean()) {
		return "no differences";
	}
	std::string s = std::to_string(changed.size()) + " changed, " + std::to_string(added.size()) + " added, " +
			std::to_string(removed.size()) + " removed";
	const std::string *first = !changed.empty() ? &changed.front() : (!added.empty() ? &added.front() : &removed.front());
	return s + " (first: " + *first + ")";
}

InstallSnapshot take_install_snapshot(const std::filesystem::path &p_root) {
	const auto t0 = std::chrono::steady_clock::now();
	InstallSnapshot snap;
	if (GetFileAttributesW(p_root.c_str()) == INVALID_FILE_ATTRIBUTES) {
		return snap;
	}
	Scanner sc;
	sc.root = p_root;
	sc.snap = &snap;
	sc.scan_package(L"");
	// Update\Patch and Update\Xpack1..5 (whatever exists).
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileExW((p_root / L"Update" / L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			const std::wstring name = fd.cFileName;
			if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name != L"." && name != L"..") {
				sc.scan_package(L"Update\\" + name);
			}
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	std::sort(snap.files.begin(), snap.files.end(), [](const FileStamp &a, const FileStamp &b) { return a.relative < b.relative; });
	for (const FileStamp &f : snap.files) {
		const std::string &r = f.relative;
		if (r.size() >= 4 && _stricmp(r.c_str() + r.size() - 4, ".toc") == 0) {
			snap.toc_count++;
		} else if (r.size() >= 3 && _stricmp(r.c_str() + r.size() - 3, ".sb") == 0) {
			snap.sb_count++;
		} else if (r.size() >= 7 && _stricmp(r.c_str() + r.size() - 7, "cas.cat") == 0) {
			snap.cat_count++;
		}
	}
	snap.valid = !snap.files.empty();
	snap.scan_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	return snap;
}

SnapshotDiff diff_snapshots(const InstallSnapshot &p_before, const InstallSnapshot &p_after) {
	SnapshotDiff d;
	size_t i = 0;
	size_t j = 0;
	const auto &a = p_before.files;
	const auto &b = p_after.files;
	while (i < a.size() || j < b.size()) {
		if (j == b.size() || (i < a.size() && a[i].relative < b[j].relative)) {
			d.removed.push_back(a[i++].relative);
		} else if (i == a.size() || b[j].relative < a[i].relative) {
			d.added.push_back(b[j++].relative);
		} else {
			if (a[i].size != b[j].size || a[i].mtime != b[j].mtime) {
				d.changed.push_back(a[i].relative);
			}
			++i;
			++j;
		}
	}
	return d;
}

} // namespace bf3::core
