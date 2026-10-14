#include "cache_fs.h"

#include <windows.h>

#include <winternl.h>

#include <cstring>

namespace bf3::core::cache_detail {

namespace {

using NtCreateFileFn = NTSTATUS(NTAPI *)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG,
		ULONG, ULONG, PVOID, ULONG);
using NtSetInformationFileFn = NTSTATUS(NTAPI *)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
using RtlNtStatusToDosErrorFn = ULONG(NTAPI *)(NTSTATUS);

struct NtApi {
	NtCreateFileFn create = nullptr;
	NtSetInformationFileFn set_information = nullptr;
	RtlNtStatusToDosErrorFn to_dos = nullptr;
};

// FILE_INFORMATION_CLASS values (ntifs.h) and FILE_RENAME_INFORMATION (same layout as FILE_RENAME_INFO).
constexpr ULONG kFileRenameInformation = 10;
constexpr ULONG kFileRenameInformationEx = 65;

const NtApi &nt() {
	static const NtApi api = [] {
		NtApi a;
		if (HMODULE m = GetModuleHandleW(L"ntdll.dll")) {
			a.create = reinterpret_cast<NtCreateFileFn>(reinterpret_cast<void *>(GetProcAddress(m, "NtCreateFile")));
			a.set_information =
					reinterpret_cast<NtSetInformationFileFn>(reinterpret_cast<void *>(GetProcAddress(m, "NtSetInformationFile")));
			a.to_dos = reinterpret_cast<RtlNtStatusToDosErrorFn>(reinterpret_cast<void *>(GetProcAddress(m, "RtlNtStatusToDosError")));
		}
		return a;
	}();
	return api;
}

constexpr ULONG kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr ACCESS_MASK kDirAccess =
		FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE;

// NtCreateFile relative to p_dir. The name is one component; reparse points are never followed.
HANDLE open_relative(HANDLE p_dir, std::wstring_view p_name, ACCESS_MASK p_access, ULONG p_attributes, ULONG p_share,
		ULONG p_disposition, ULONG p_options, uint32_t &r_err) {
	if (p_dir == nullptr || !valid_component(p_name)) {
		r_err = ERROR_INVALID_NAME;
		return nullptr;
	}
	if (nt().create == nullptr || nt().to_dos == nullptr) {
		r_err = kErrNtUnavailable;
		return nullptr;
	}
	UNICODE_STRING name;
	name.Buffer = const_cast<PWSTR>(p_name.data());
	name.Length = static_cast<USHORT>(p_name.size() * sizeof(wchar_t));
	name.MaximumLength = name.Length;
	OBJECT_ATTRIBUTES oa{};
	oa.Length = sizeof oa;
	oa.RootDirectory = p_dir;
	oa.ObjectName = &name;
	oa.Attributes = OBJ_CASE_INSENSITIVE;
	IO_STATUS_BLOCK io{};
	HANDLE h = nullptr;
	const NTSTATUS st = nt().create(&h, p_access | SYNCHRONIZE, &oa, &io, nullptr, p_attributes, p_share, p_disposition,
			p_options | FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT, nullptr, 0);
	if (!NT_SUCCESS(st)) {
		r_err = nt().to_dos(st);
		if (r_err == 0) {
			r_err = ERROR_GEN_FAILURE;
		}
		return nullptr;
	}
	r_err = 0;
	return h;
}

std::wstring strip_verbatim(std::wstring p_path) {
	if (p_path.rfind(L"\\\\?\\UNC\\", 0) == 0) {
		return L"\\\\" + p_path.substr(8);
	}
	if (p_path.rfind(L"\\\\?\\", 0) == 0) {
		return p_path.substr(4);
	}
	return p_path;
}

// Rejects a handle that turned out to be a reparse point (we opened the link itself).
Handle refuse_reparse(HANDLE p_handle, uint32_t &r_err) {
	Handle h(p_handle);
	if (!h.valid()) {
		return h;
	}
	FileInfo info;
	if (!query_info(h, info, r_err)) {
		return Handle();
	}
	if (info.is_reparse()) {
		r_err = kErrReparsePoint;
		return Handle();
	}
	return h;
}

} // namespace

void Handle::reset() {
	if (handle_ != nullptr) {
		CloseHandle(handle_);
		handle_ = nullptr;
	}
}

bool valid_component(std::wstring_view p_name) {
	if (p_name.empty() || p_name.size() > 255 || p_name == L"." || p_name == L"..") {
		return false;
	}
	for (wchar_t c : p_name) {
		if (c < 0x20 || c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' ||
				c == L'|') {
			return false;
		}
	}
	return p_name.back() != L'.' && p_name.back() != L' ';
}

Handle open_dir_path(const std::filesystem::path &p_path, uint32_t &r_err) {
	HANDLE h = CreateFileW(p_path.c_str(), kDirAccess, kShareAll, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		r_err = GetLastError();
		return Handle();
	}
	Handle out(h);
	FileInfo info;
	if (!query_info(out, info, r_err)) {
		return Handle();
	}
	if (!info.is_directory()) {
		r_err = ERROR_DIRECTORY;
		return Handle();
	}
	r_err = 0;
	return out;
}

Handle open_child_dir(const Handle &p_parent, std::wstring_view p_name, bool p_create, uint32_t &r_err) {
	// No FILE_SHARE_DELETE: while we hold a verified directory handle, nobody can rename or move
	// that directory elsewhere (e.g. into the install) between the final-path check and the write.
	HANDLE h = open_relative(p_parent.get(), p_name, kDirAccess, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
			p_create ? FILE_OPEN_IF : FILE_OPEN,
			FILE_DIRECTORY_FILE, r_err);
	return refuse_reparse(h, r_err);
}

Handle create_child_file(const Handle &p_dir, std::wstring_view p_name, uint32_t &r_err) {
	return Handle(open_relative(p_dir.get(), p_name, GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES, FILE_ATTRIBUTE_NORMAL, 0,
			FILE_CREATE, FILE_NON_DIRECTORY_FILE, r_err));
}

Handle open_child_file_read(const Handle &p_dir, std::wstring_view p_name, uint32_t &r_err) {
	HANDLE h = open_relative(p_dir.get(), p_name, GENERIC_READ, 0, FILE_SHARE_READ | FILE_SHARE_DELETE, FILE_OPEN,
			FILE_NON_DIRECTORY_FILE, r_err);
	return refuse_reparse(h, r_err);
}

Handle open_child_any(const Handle &p_dir, std::wstring_view p_name, uint32_t &r_err) {
	return Handle(open_relative(p_dir.get(), p_name, DELETE | FILE_READ_ATTRIBUTES, 0, kShareAll, FILE_OPEN, 0, r_err));
}

Handle open_child_append(const Handle &p_dir, std::wstring_view p_name, uint32_t &r_err) {
	HANDLE h = open_relative(p_dir.get(), p_name, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | FILE_READ_DATA, FILE_ATTRIBUTE_NORMAL,
			FILE_SHARE_READ, FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE, r_err);
	return refuse_reparse(h, r_err);
}

Handle open_child_lock(const Handle &p_dir, std::wstring_view p_name, uint32_t &r_err) {
	HANDLE h = open_relative(p_dir.get(), p_name, GENERIC_READ | GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN_IF,
			FILE_NON_DIRECTORY_FILE, r_err);
	return refuse_reparse(h, r_err);
}

bool query_info(const Handle &p_handle, FileInfo &r_info, uint32_t &r_err) {
	BY_HANDLE_FILE_INFORMATION bhfi{};
	if (!GetFileInformationByHandle(p_handle.get(), &bhfi)) {
		r_err = GetLastError();
		return false;
	}
	r_info.size = (uint64_t(bhfi.nFileSizeHigh) << 32) | bhfi.nFileSizeLow;
	r_info.links = bhfi.nNumberOfLinks;
	r_info.attributes = bhfi.dwFileAttributes;
	r_err = 0;
	return true;
}

std::optional<std::filesystem::path> final_path(const Handle &p_handle) {
	std::wstring out(512, L'\0');
	DWORD got = GetFinalPathNameByHandleW(p_handle.get(), out.data(), static_cast<DWORD>(out.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if (got >= out.size()) {
		out.resize(got + 1);
		got = GetFinalPathNameByHandleW(p_handle.get(), out.data(), static_cast<DWORD>(out.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	}
	if (got == 0 || got >= out.size()) {
		return std::nullopt;
	}
	out.resize(got);
	return std::filesystem::path(strip_verbatim(out));
}

bool write_all(const Handle &p_file, const void *p_data, size_t p_size, uint32_t &r_err) {
	const uint8_t *p = static_cast<const uint8_t *>(p_data);
	size_t done = 0;
	constexpr size_t kPiece = size_t(64) << 20;
	while (done < p_size) {
		const DWORD n = static_cast<DWORD>(p_size - done < kPiece ? p_size - done : kPiece);
		DWORD wrote = 0;
		if (!WriteFile(p_file.get(), p + done, n, &wrote, nullptr) || wrote != n) {
			r_err = GetLastError();
			if (r_err == 0) {
				r_err = ERROR_WRITE_FAULT;
			}
			return false;
		}
		done += n;
	}
	r_err = 0;
	return true;
}

bool flush(const Handle &p_file, uint32_t &r_err) {
	if (!FlushFileBuffers(p_file.get())) {
		r_err = GetLastError();
		return false;
	}
	r_err = 0;
	return true;
}

bool read_prefix(const Handle &p_file, void *r_dst, size_t p_size, uint32_t &r_err) {
	OVERLAPPED ov{};
	DWORD got = 0;
	if (!ReadFile(p_file.get(), r_dst, static_cast<DWORD>(p_size), &got, &ov) || got != p_size) {
		r_err = GetLastError();
		if (r_err == 0) {
			r_err = ERROR_HANDLE_EOF;
		}
		return false;
	}
	r_err = 0;
	return true;
}

bool read_all(const Handle &p_file, std::vector<uint8_t> &r_out, uint64_t p_max, uint32_t &r_err) {
	LARGE_INTEGER size{};
	if (!GetFileSizeEx(p_file.get(), &size)) {
		r_err = GetLastError();
		return false;
	}
	if (size.QuadPart < 0 || uint64_t(size.QuadPart) > p_max) {
		r_err = ERROR_FILE_TOO_LARGE;
		return false;
	}
	r_out.resize(static_cast<size_t>(size.QuadPart));
	size_t done = 0;
	while (done < r_out.size()) {
		const DWORD want = static_cast<DWORD>(r_out.size() - done < (size_t(64) << 20) ? r_out.size() - done : (size_t(64) << 20));
		DWORD got = 0;
		OVERLAPPED ov{};
		ov.Offset = static_cast<DWORD>(done);
		ov.OffsetHigh = static_cast<DWORD>(uint64_t(done) >> 32);
		if (!ReadFile(p_file.get(), r_out.data() + done, want, &got, &ov) || got == 0) {
			r_err = GetLastError();
			if (r_err == 0) {
				r_err = ERROR_HANDLE_EOF;
			}
			return false;
		}
		done += got;
	}
	r_err = 0;
	return true;
}

bool rename_into(const Handle &p_file, const Handle &p_dir, std::wstring_view p_name, uint32_t &r_err) {
	if (!valid_component(p_name) || !p_dir.valid()) {
		r_err = ERROR_INVALID_NAME;
		return false;
	}
	if (nt().set_information == nullptr || nt().to_dos == nullptr) {
		r_err = kErrNtUnavailable;
		return false;
	}
	const size_t name_bytes = p_name.size() * sizeof(wchar_t);
	const size_t total = offsetof(FILE_RENAME_INFO, FileName) + name_bytes + sizeof(wchar_t);
	std::vector<uint64_t> storage(total / 8 + 1, 0);
	auto *info = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
	info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
	info->RootDirectory = p_dir.get();
	info->FileNameLength = static_cast<DWORD>(name_bytes);
	std::memcpy(info->FileName, p_name.data(), name_bytes);
	IO_STATUS_BLOCK io{};
	NTSTATUS st = nt().set_information(p_file.get(), &io, info, static_cast<ULONG>(total), kFileRenameInformationEx);
	if (st == static_cast<NTSTATUS>(0xC0000003L) /* INVALID_INFO_CLASS */ || st == static_cast<NTSTATUS>(0xC00000BBL) /* NOT_SUPPORTED */ ||
			st == static_cast<NTSTATUS>(0xC000000DL) /* INVALID_PARAMETER */) {
		// Volumes without POSIX rename (FAT, older NTFS): classic replace.
		info->Flags = 0;
		info->ReplaceIfExists = TRUE;
		st = nt().set_information(p_file.get(), &io, info, static_cast<ULONG>(total), kFileRenameInformation);
	}
	if (!NT_SUCCESS(st)) {
		r_err = nt().to_dos(st);
		if (r_err == 0) {
			r_err = ERROR_GEN_FAILURE;
		}
		return false;
	}
	r_err = 0;
	return true;
}

bool delete_by_handle(const Handle &p_handle, uint32_t &r_err) {
	FILE_DISPOSITION_INFO_EX ex{};
	ex.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS | FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
	if (SetFileInformationByHandle(p_handle.get(), FileDispositionInfoEx, &ex, sizeof ex)) {
		r_err = 0;
		return true;
	}
	FILE_DISPOSITION_INFO classic{};
	classic.DeleteFile = TRUE;
	if (SetFileInformationByHandle(p_handle.get(), FileDispositionInfo, &classic, sizeof classic)) {
		r_err = 0;
		return true;
	}
	r_err = GetLastError();
	return false;
}

bool list_dir(const Handle &p_dir, std::vector<DirEntry> &r_out, uint32_t &r_err) {
	r_out.clear();
	std::vector<uint64_t> buffer(64 * 1024 / 8);
	FILE_INFO_BY_HANDLE_CLASS cls = FileFullDirectoryRestartInfo;
	for (;;) {
		if (!GetFileInformationByHandleEx(p_dir.get(), cls, buffer.data(), static_cast<DWORD>(buffer.size() * 8))) {
			const DWORD e = GetLastError();
			if (e == ERROR_NO_MORE_FILES) {
				break;
			}
			r_err = e;
			return false;
		}
		cls = FileFullDirectoryInfo;
		const uint8_t *base = reinterpret_cast<const uint8_t *>(buffer.data());
		const size_t limit = buffer.size() * 8;
		size_t off = 0;
		for (;;) {
			if (off + sizeof(FILE_FULL_DIR_INFO) > limit) {
				break;
			}
			const auto *e = reinterpret_cast<const FILE_FULL_DIR_INFO *>(base + off);
			const size_t name_off = off + offsetof(FILE_FULL_DIR_INFO, FileName);
			if (name_off + e->FileNameLength <= limit) {
				std::wstring name(e->FileName, e->FileNameLength / sizeof(wchar_t));
				if (name != L"." && name != L"..") {
					DirEntry d;
					d.name = std::move(name);
					d.size = uint64_t(e->EndOfFile.QuadPart);
					d.attributes = e->FileAttributes;
					r_out.push_back(std::move(d));
				}
			}
			if (e->NextEntryOffset == 0) {
				break;
			}
			off += e->NextEntryOffset;
		}
	}
	r_err = 0;
	return true;
}

const void *map_read(const Handle &p_file, uint64_t p_size, uint32_t &r_err) {
	if (p_size == 0) {
		r_err = ERROR_FILE_INVALID;
		return nullptr;
	}
	HANDLE m = CreateFileMappingW(p_file.get(), nullptr, PAGE_READONLY, static_cast<DWORD>(p_size >> 32), static_cast<DWORD>(p_size), nullptr);
	if (m == nullptr) {
		r_err = GetLastError();
		return nullptr;
	}
	const void *view = MapViewOfFile(m, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(p_size));
	r_err = view != nullptr ? 0 : GetLastError();
	CloseHandle(m); // the view keeps the section alive
	return view;
}

void unmap(const void *p_view) {
	if (p_view != nullptr) {
		UnmapViewOfFile(p_view);
	}
}

uint32_t current_process_id() {
	return GetCurrentProcessId();
}

} // namespace bf3::core::cache_detail
