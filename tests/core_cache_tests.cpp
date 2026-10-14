// Derived-data cache (bf3_core/cache) against scratch trees in %TEMP%. A temp directory plays the
// protected install root; the real install is never involved.
#include <doctest/doctest.h>

#include "bf3/core/cache/cache_key.h"
#include "bf3/core/cache/digest.h"
#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/cache/entry_format.h"
#include "bf3/core/path_guard.h"

#include <windows.h>
#include <winioctl.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace bf3::core;

namespace {

std::atomic<int> g_scratch_counter{ 0 };

std::span<const uint8_t> bytes_of(std::string_view p_s) {
	return { reinterpret_cast<const uint8_t *>(p_s.data()), p_s.size() };
}

std::vector<uint8_t> payload(size_t p_size, uint32_t p_seed) {
	std::vector<uint8_t> out(p_size);
	uint32_t x = p_seed * 2654435761u + 1;
	for (uint8_t &b : out) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		b = uint8_t(x);
	}
	return out;
}

CacheKey key(std::string p_kind, uint32_t p_n) {
	CacheKeyInputs in;
	in.kind = std::move(p_kind);
	const std::string src = "source-" + std::to_string(p_n);
	in.sources.push_back(sha1_of(bytes_of(src)));
	in.converter_version = 1;
	in.engine = "test-engine";
	in.format_version = 1;
	return *make_cache_key(in);
}

std::vector<uint8_t> read_file(const fs::path &p_path) {
	std::vector<uint8_t> out;
	FILE *f = nullptr;
	if (_wfopen_s(&f, p_path.c_str(), L"rb") != 0 || f == nullptr) {
		return out;
	}
	uint8_t buf[65536];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
		out.insert(out.end(), buf, buf + n);
	}
	std::fclose(f);
	return out;
}

bool write_file(const fs::path &p_path, std::span<const uint8_t> p_bytes) {
	FILE *f = nullptr;
	if (_wfopen_s(&f, p_path.c_str(), L"wb") != 0 || f == nullptr) {
		return false;
	}
	const bool ok = std::fwrite(p_bytes.data(), 1, p_bytes.size(), f) == p_bytes.size();
	std::fclose(f);
	return ok;
}

// Creates a directory junction p_link -> p_target (no admin rights needed, unlike symlinks).
bool make_junction(const fs::path &p_link, const fs::path &p_target) {
	if (!CreateDirectoryW(p_link.c_str(), nullptr)) {
		return false;
	}
	HANDLE h = CreateFileW(p_link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}
	const std::wstring substitute = L"\\??\\" + fs::absolute(p_target).native();
	const std::wstring print = fs::absolute(p_target).native();
	const size_t sub_bytes = substitute.size() * sizeof(wchar_t);
	const size_t print_bytes = print.size() * sizeof(wchar_t);
	const size_t data_len = 8 + sub_bytes + 2 + print_bytes + 2;
	std::string buf(8 + data_len, '\0');
	auto put16 = [&](size_t off, uint16_t v) { std::memcpy(&buf[off], &v, 2); };
	const uint32_t tag = IO_REPARSE_TAG_MOUNT_POINT;
	std::memcpy(&buf[0], &tag, 4);
	put16(4, static_cast<uint16_t>(data_len));
	put16(8, 0);
	put16(10, static_cast<uint16_t>(sub_bytes));
	put16(12, static_cast<uint16_t>(sub_bytes + 2));
	put16(14, static_cast<uint16_t>(print_bytes));
	std::memcpy(&buf[16], substitute.data(), sub_bytes);
	std::memcpy(&buf[16 + sub_bytes + 2], print.data(), print_bytes);
	DWORD out = 0;
	const BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), static_cast<DWORD>(buf.size()), nullptr, 0, &out, nullptr);
	CloseHandle(h);
	return ok != 0;
}

// %TEMP%\openbf3-cache-<pid>-<n>\{install (protected, with one file), cache (the cache root)}.
struct Scratch {
	fs::path base;
	fs::path install;
	fs::path root;
	fs::path install_file;

	Scratch() {
		wchar_t tmp[MAX_PATH];
		GetTempPathW(MAX_PATH, tmp);
		base = fs::path(tmp) / (L"openbf3-cache-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" +
									   std::to_wstring(g_scratch_counter++));
		install = base / L"install";
		root = base / L"cache";
		CreateDirectoryW(base.c_str(), nullptr);
		CreateDirectoryW(install.c_str(), nullptr);
		install_file = install / L"layout.toc";
		const std::vector<uint8_t> data = payload(4096, 99);
		write_file(install_file, data);
	}
	~Scratch() { remove_tree(base); }

	// Removes junctions as links (never recursing into their targets), then the rest.
	static void remove_tree(const fs::path &p_dir) {
		WIN32_FIND_DATAW fd;
		HANDLE h = FindFirstFileW((p_dir / L"*").c_str(), &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				const std::wstring name = fd.cFileName;
				if (name == L"." || name == L"..") {
					continue;
				}
				const fs::path child = p_dir / name;
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
					RemoveDirectoryW(child.c_str()) || DeleteFileW(child.c_str());
				} else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
					remove_tree(child);
				} else {
					SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
					DeleteFileW(child.c_str());
				}
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		}
		RemoveDirectoryW(p_dir.c_str());
	}

	PathGuard guard() const {
		PathGuard g;
		g.add_protected_root(install);
		return g;
	}

	CacheOptions options() const {
		CacheOptions o;
		o.root = root;
		o.guard = guard();
		return o;
	}

	// Every file under the protected root with its bytes; compared before and after attacks.
	std::map<std::wstring, std::vector<uint8_t>> install_snapshot() const {
		std::map<std::wstring, std::vector<uint8_t>> out;
		std::error_code ec;
		for (auto it = fs::recursive_directory_iterator(install, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
			if (it->is_regular_file(ec)) {
				out[it->path().native()] = read_file(it->path());
			} else {
				out[it->path().native()] = {};
			}
		}
		return out;
	}

	fs::path entry_path(const CacheKey &p_key) const { return root / p_key.relative_path(); }
};

std::vector<fs::path> files_with_suffix(const fs::path &p_dir, std::wstring_view p_suffix) {
	std::vector<fs::path> out;
	std::error_code ec;
	for (auto it = fs::recursive_directory_iterator(p_dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
		const std::wstring n = it->path().filename().native();
		if (n.size() >= p_suffix.size() && n.compare(n.size() - p_suffix.size(), p_suffix.size(), p_suffix) == 0) {
			out.push_back(it->path());
		}
	}
	return out;
}

} // namespace

TEST_CASE("cache: digests match published test vectors") {
	CHECK(sha1_of(bytes_of("abc")).hex() == "a9993e364706816aba3e25717850c26c9cd0d89d");
	CHECK(sha1_of({}).hex() == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
	CHECK(sha256_of(bytes_of("abc")).hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK(sha256_of({}).hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(crc32(bytes_of("123456789")) == 0xCBF43926u);
	// Incremental equals one-shot.
	const std::vector<uint8_t> data = payload(1 << 20, 7);
	Hasher h(HashAlgorithm::Sha256);
	h.update(std::span(data).first(12345));
	h.update(std::span(data).subspan(12345));
	Sha256Digest inc;
	REQUIRE(h.finish(inc.bytes));
	CHECK(inc == sha256_of(data));
	CHECK(Sha1Digest::from_hex("A9993E364706816ABA3E25717850C26C9CD0D89D") == sha1_of(bytes_of("abc")));
	CHECK(!Sha1Digest::from_hex("a9993e"));
	CHECK(!Sha1Digest::from_hex("g9993e364706816aba3e25717850c26c9cd0d89d"));
}

TEST_CASE("cache: keys are stable and cover every input") {
	CacheKeyInputs in;
	in.kind = "mesh";
	in.sources = { *Sha1Digest::from_hex("d811590bc6753c847682b70c787bb1157d466e9b"), *Sha1Digest::from_hex("1bb223c2421a3f99c7566f7d16b91db011f90437") };
	in.converter_version = 3;
	in.engine = "4.8.dev7.c971f93e7";
	in.format_version = 2;
	const std::optional<CacheKey> k = make_cache_key(in);
	REQUIRE(k);
	// Pinned value: changing the encoding orphans every cache on every machine, so it must be a
	// deliberate container-version bump, never an accident.
	CHECK(k->hex() == "6cdf505b719a76e688ab417ef8089f321711a7091727fbaff429a89ce7f836b4");
	MESSAGE("key: ", k->hex());
	CHECK(make_cache_key(in)->digest == k->digest);
	CHECK(k->relative_path().generic_string() == "v1/mesh/" + k->hex().substr(0, 2) + "/" + k->hex() + ".bin");

	auto differs = [&](auto p_change) {
		CacheKeyInputs c = in;
		p_change(c);
		const std::optional<CacheKey> other = make_cache_key(c);
		return other && other->digest != k->digest;
	};
	CHECK(differs([](CacheKeyInputs &c) { c.kind = "meshx"; }));
	CHECK(differs([](CacheKeyInputs &c) { std::swap(c.sources[0], c.sources[1]); }));
	CHECK(differs([](CacheKeyInputs &c) { c.sources.pop_back(); }));
	CHECK(differs([](CacheKeyInputs &c) { c.sources[0].bytes[19] ^= 1; }));
	CHECK(differs([](CacheKeyInputs &c) { c.converter_version = 4; }));
	CHECK(differs([](CacheKeyInputs &c) { c.engine = "4.8.dev7.c971f93e8"; }));
	CHECK(differs([](CacheKeyInputs &c) { c.format_version = 3; }));
	CHECK(differs([](CacheKeyInputs &c) { c.jolt_version = "5.6.0"; }));
	CHECK(differs([](CacheKeyInputs &c) { c.params = "cs=0.3"; }));
	// Field boundaries are length-prefixed: moving bytes between fields changes the key.
	CHECK(differs([](CacheKeyInputs &c) {
		c.engine = "4.8.dev7.c971f93e75.6.0";
		c.jolt_version = "";
	}));

	for (const char *bad : { "", "Mesh", "a/b", "a.b", "..", "123456789012345678901234567890123" }) {
		CacheKeyInputs c = in;
		c.kind = bad;
		CHECK(!make_cache_key(c));
	}
}

TEST_CASE("cache: round trip, dedup and reopen") {
	Scratch t;
	Cache cache;
	std::string why;
	const CacheStatus opened = cache.open(t.options(), &why);
	if (opened != CacheStatus::Ok) {
		std::fprintf(stderr, "open: %s: %s\n", cache_status_name(opened), why.c_str());
		std::fflush(stderr);
	}
	REQUIRE(opened == CacheStatus::Ok);
	const std::vector<uint8_t> a = payload(100000, 1);
	const std::vector<uint8_t> b = payload(1, 2);
	const CacheKey ka = key("mesh", 1);
	const CacheKey kb = key("mesh", 2);
	const CacheKey kempty = key("nav", 3);

	CHECK(cache.get(ka).status == CacheStatus::Miss);
	PutResult p = cache.put(ka, a);
	CHECK(p.status == CacheStatus::Ok);
	CHECK(p.written);
	CHECK(cache.put(kb, b).written);
	CHECK(cache.put(kempty, {}).written);
	p = cache.put(ka, b); // same key again: not rewritten
	CHECK(p.status == CacheStatus::Ok);
	CHECK(p.reused);
	CHECK(!p.written);
	CHECK(fs::exists(t.entry_path(ka)));
	CHECK(fs::file_size(t.entry_path(ka)) == a.size() + kCacheEntryHeaderSize);

	{
		GetResult g = cache.get(ka);
		REQUIRE(g.status == CacheStatus::Ok);
		CHECK(g.entry.payload().size() == a.size());
		CHECK(std::memcmp(g.entry.payload().data(), a.data(), a.size()) == 0);
		CHECK(reinterpret_cast<uintptr_t>(g.entry.payload().data()) % 64 == 0);
		GetResult e = cache.get(kempty);
		REQUIRE(e.status == CacheStatus::Ok);
		CHECK(e.entry.payload().empty());
		// The same digest under another kind is a different key, never a hit.
		CacheKey wrong = ka;
		wrong.kind = "nav";
		CHECK(cache.get(wrong).status == CacheStatus::Miss);
	}
	// Regression: a rename must stay relative to the target directory handle. (kernel32 resolves
	// a bare rename target against the current directory; that put entries into the CWD once.)
	{
		wchar_t cwd[MAX_PATH];
		GetCurrentDirectoryW(MAX_PATH, cwd);
		SetCurrentDirectoryW(t.base.c_str());
		CHECK(cache.put(key("mesh", 77), b).written);
		SetCurrentDirectoryW(cwd);
		CHECK(fs::exists(t.entry_path(key("mesh", 77))));
		CHECK(!fs::exists(t.base / (key("mesh", 77).hex() + ".bin")));
		CHECK(cache.remove(key("mesh", 77)) == CacheStatus::Ok);
	}
	CacheStats s = cache.stats();
	CHECK(s.entries == 3);
	CHECK(s.puts == 4);
	CHECK(s.dedup_reused == 1);
	CHECK(s.bytes == a.size() + b.size() + 3 * kCacheEntryHeaderSize);
	cache.close();
	CHECK(!cache.is_open());

	REQUIRE(cache.open(t.options(), &why) == CacheStatus::Ok);
	s = cache.stats();
	CHECK(s.entries == 3);
	CHECK(!s.recovered_unclean);
	CHECK(s.adopted == 0);
	CHECK(s.stray_temps_removed == 0);
	GetResult g = cache.get(kb);
	REQUIRE(g.status == CacheStatus::Ok);
	CHECK(g.entry.payload().size() == 1);
	CHECK(g.entry.payload()[0] == b[0]);
	CHECK(cache.stats().verified == 1); // first use in this process hashes the payload
	g.entry.reset();
	CHECK(cache.get(kb).status == CacheStatus::Ok);
	CHECK(cache.stats().verified == 1); // not again
	CHECK(cache.remove(kb) == CacheStatus::Ok);
	CHECK(!fs::exists(t.entry_path(kb)));
	CHECK(cache.get(kb).status == CacheStatus::Miss);

	// One process (one Cache) per root.
	Cache second;
	CHECK(second.open(t.options(), &why) == CacheStatus::Busy);
}

TEST_CASE("cache: damaged and partial entries are detected, deleted and reported as misses") {
	Scratch t;
	const CacheKey kflip = key("mesh", 10);
	const CacheKey ktrunc = key("mesh", 11);
	const CacheKey khead = key("mesh", 12);
	const CacheKey kgrow = key("mesh", 13);
	const CacheKey klive = key("mesh", 14);
	const std::vector<uint8_t> data = payload(50000, 3);
	{
		Cache cache;
		REQUIRE(cache.open(t.options()) == CacheStatus::Ok);
		for (const CacheKey &k : { kflip, ktrunc, khead, kgrow, klive }) {
			REQUIRE(cache.put(k, data).written);
		}
		// In-session damage: the entry is verified (written by us), a forced verify still hashes.
		std::vector<uint8_t> f = read_file(t.entry_path(klive));
		f[kCacheEntryHeaderSize + 777] ^= 0x01;
		REQUIRE(write_file(t.entry_path(klive), f));
		CHECK(cache.get(klive).status == CacheStatus::Ok); // header-valid, already verified
		GetResult forced = cache.get(klive, true);
		CHECK(forced.status == CacheStatus::Miss);
		CHECK(forced.dropped_corrupt);
		CHECK(!fs::exists(t.entry_path(klive)));
		CHECK(cache.stats().corrupt_dropped == 1);
	}
	// Offline damage.
	{
		std::vector<uint8_t> f = read_file(t.entry_path(kflip));
		f[kCacheEntryHeaderSize + 12345] ^= 0x80; // one payload bit
		REQUIRE(write_file(t.entry_path(kflip), f));
		f = read_file(t.entry_path(ktrunc));
		f.pop_back(); // torn write of the last byte
		REQUIRE(write_file(t.entry_path(ktrunc), f));
		f = read_file(t.entry_path(khead));
		f[3] = 'X'; // magic
		REQUIRE(write_file(t.entry_path(khead), f));
		f = read_file(t.entry_path(kgrow));
		f.push_back(0); // appended garbage
		REQUIRE(write_file(t.entry_path(kgrow), f));
	}
	Cache cache;
	REQUIRE(cache.open(t.options()) == CacheStatus::Ok);
	CacheStats s = cache.stats();
	CHECK(s.corrupt_dropped == 2); // truncated and grown: size disagrees with the journal at open
	CHECK(!fs::exists(t.entry_path(ktrunc)));
	CHECK(!fs::exists(t.entry_path(kgrow)));
	CHECK(cache.get(ktrunc).status == CacheStatus::Miss);

	GetResult g = cache.get(kflip); // first use hashes: the flipped bit is found
	CHECK(g.status == CacheStatus::Miss);
	CHECK(g.dropped_corrupt);
	CHECK(!fs::exists(t.entry_path(kflip)));
	g = cache.get(khead);
	CHECK(g.status == CacheStatus::Miss);
	CHECK(g.dropped_corrupt);
	CHECK(!fs::exists(t.entry_path(khead)));
	CHECK(cache.stats().entries == 0);
	CHECK(cache.stats().corrupt_dropped == 4);
	// A fresh put after a drop works.
	CHECK(cache.put(kflip, data).written);
	CHECK(cache.get(kflip).status == CacheStatus::Ok);
}

TEST_CASE("cache: crash recovery (stray temp files, torn journal, unjournaled and missing entries)") {
	Scratch t;
	const CacheKey k1 = key("shape", 1);
	const CacheKey k2 = key("shape", 2);
	const CacheKey k3 = key("shape", 3); // written but its journal record is lost
	const CacheKey k4 = key("shape", 4); // journaled but its file is lost
	const std::vector<uint8_t> data = payload(3000, 5);
	const fs::path journal = t.root / L"v1" / L"index.log";
	uintmax_t journal_size_before_k3 = 0;
	{
		Cache cache;
		REQUIRE(cache.open(t.options()) == CacheStatus::Ok);
		REQUIRE(cache.put(k1, data).written);
		REQUIRE(cache.put(k2, data).written);
		REQUIRE(cache.put(k4, data).written);
		REQUIRE(cache.flush() == CacheStatus::Ok);
		journal_size_before_k3 = fs::file_size(journal);
		REQUIRE(cache.put(k3, data).written);
		cache.close();
	}
	// Simulate the crash: drop k3's record and the close marker, then a half-written record.
	{
		std::vector<uint8_t> j = read_file(journal);
		REQUIRE(j.size() > journal_size_before_k3);
		j.resize(static_cast<size_t>(journal_size_before_k3));
		const uint8_t torn[] = { 60, 0, 0, 0, 0x12, 0x34, 0x56, 0x78, 1, 2, 3 }; // length 60, only 3 body bytes
		j.insert(j.end(), std::begin(torn), std::end(torn));
		REQUIRE(write_file(journal, j));
		fs::remove(t.entry_path(k4));
		// Temp files of interrupted writes (an entry and a journal compaction).
		REQUIRE(write_file(t.entry_path(k1).parent_path() / (k1.hex() + ".4242-7.tmp"), data));
		REQUIRE(write_file(t.root / L"v1" / L"index.log.4242-8.tmp", data));
	}
	Cache cache;
	REQUIRE(cache.open(t.options()) == CacheStatus::Ok);
	CacheStats s = cache.stats();
	CHECK(s.recovered_unclean);
	CHECK(s.journal_torn_bytes == 11);
	CHECK(s.stray_temps_removed == 2);
	CHECK(s.adopted == 1);
	CHECK(s.missing_dropped == 1);
	CHECK(s.journal_compacted);
	CHECK(s.entries == 3);
	CHECK(files_with_suffix(t.root, L".tmp").empty());
	CHECK(cache.get(k1).status == CacheStatus::Ok);
	CHECK(cache.get(k3).status == CacheStatus::Ok); // adopted, verified on first use
	CHECK(cache.get(k4).status == CacheStatus::Miss);
	cache.close();

	// The rewritten journal is clean.
	REQUIRE(cache.open(t.options()) == CacheStatus::Ok);
	s = cache.stats();
	CHECK(!s.recovered_unclean);
	CHECK(s.journal_torn_bytes == 0);
	CHECK(s.adopted == 0);
	CHECK(!s.journal_compacted);
	CHECK(s.entries == 3);

	// A journal with a damaged header is rebuilt from the files.
	cache.close();
	std::vector<uint8_t> j = read_file(journal);
	j[0] = 'X';
	REQUIRE(write_file(journal, j));
	REQUIRE(cache.open(t.options()) == CacheStatus::Ok);
	CHECK(cache.stats().entries == 3);
	CHECK(cache.stats().adopted == 3);
}

TEST_CASE("cache: guard refuses a root under the protected root") {
	Scratch t;
	Cache cache;
	std::string why;
	CacheOptions o = t.options();
	o.root = t.install / L"cache";
	CHECK(cache.open(o, &why) == CacheStatus::Refused);
	o.root = t.install;
	CHECK(cache.open(o, &why) == CacheStatus::Refused);
	o.root = t.install.native() + L"\\sub\\..\\cache";
	CHECK(cache.open(o, &why) == CacheStatus::Refused);
	CHECK(!fs::exists(t.install / L"cache"));

	const fs::path link = t.base / L"link";
	if (make_junction(link, t.install)) {
		o.root = link / L"cache";
		CHECK(cache.open(o, &why) == CacheStatus::Refused);
		o.root = link;
		CHECK(cache.open(o, &why) == CacheStatus::Refused);
		CHECK(!fs::exists(t.install / L"cache"));
		CHECK(!fs::exists(t.install / L"v1"));
	} else {
		MESSAGE("could not create a junction; junction root not exercised");
	}

	CacheOptions none;
	none.root = t.root;
	CHECK(cache.open(none, &why) == CacheStatus::Refused); // a guard without roots
	o = t.options();
	o.root = L"relative\\cache";
	CHECK(cache.open(o, &why) == CacheStatus::InvalidArgument);

	// A target inside a protected subtree of the cache root is refused per write.
	o = t.options();
	o.guard.add_protected_root(t.root / L"v1" / L"mesh");
	REQUIRE(cache.open(o, &why) == CacheStatus::Ok);
	CHECK(cache.put(key("mesh", 1), payload(10, 1)).status == CacheStatus::Refused);
	CHECK(cache.put(key("nav", 1), payload(10, 1)).status == CacheStatus::Ok);
	CHECK(cache.stats().refused == 1);
}

TEST_CASE("cache: junctions swapped in after the check cannot redirect writes") {
	Scratch t;
	const auto before = t.install_snapshot();
	const fs::path stash = t.base / L"stash";
	CreateDirectoryW(stash.c_str(), nullptr);
	fs::path attack_at;
	bool attacked = false;
	CacheOptions o = t.options();
	o.test_hook_before_write = [&](const fs::path &) {
		if (attack_at.empty()) {
			return;
		}
		// Move the real directory aside (if any) and put a junction into the install in its place.
		if (fs::exists(attack_at)) {
			MoveFileW(attack_at.c_str(), (stash / std::to_wstring(GetTickCount64())).c_str());
		}
		attacked = make_junction(attack_at, t.install);
		attack_at.clear();
	};
	Cache cache;
	REQUIRE(cache.open(o) == CacheStatus::Ok);

	// 1. The kind directory does not exist yet: a junction appears at v1/mesh.
	attack_at = t.root / L"v1" / L"mesh";
	PutResult p = cache.put(key("mesh", 1), payload(1000, 1));
	if (!attacked) {
		MESSAGE("could not create a junction; attack not exercised");
		return;
	}
	CHECK(p.status == CacheStatus::Refused);
	CHECK(t.install_snapshot() == before);
	REQUIRE(RemoveDirectoryW((t.root / L"v1" / L"mesh").c_str())); // the junction itself, never its target

	// 2. The kind directory exists; the fan-out directory of the next key is swapped.
	REQUIRE(cache.put(key("nav", 1), payload(1000, 2)).written);
	const CacheKey k2 = key("nav", 2);
	REQUIRE(cache.put(k2, payload(10, 3)).written); // creates its fan-out directory
	REQUIRE(cache.remove(k2) == CacheStatus::Ok);
	attacked = false;
	attack_at = t.entry_path(k2).parent_path();
	p = cache.put(k2, payload(1000, 4));
	CHECK(attacked);
	CHECK(p.status == CacheStatus::Refused);
	CHECK(t.install_snapshot() == before);

	// 3. A hard link to an install file appears at the target name after the path check.
	const CacheKey k3 = key("nav", 3);
	o.test_hook_before_write = nullptr;
	cache.close();
	o.test_hook_before_write = [&](const fs::path &p_target) {
		fs::create_directories(p_target.parent_path());
		CreateHardLinkW(p_target.c_str(), t.install_file.c_str(), nullptr);
	};
	REQUIRE(cache.open(o) == CacheStatus::Ok);
	p = cache.put(k3, payload(1000, 5));
	CHECK(p.status == CacheStatus::Refused);
	CHECK(t.install_snapshot() == before);
	// The planted hard link is never deleted through the cache either.
	CHECK(cache.remove(k3) == CacheStatus::Miss);
	DeleteFileW(t.entry_path(k3).c_str());

	// 4. A hard link that exists before the write is refused by the path-level check.
	const CacheKey k5 = key("nav", 5);
	fs::create_directories(t.entry_path(k5).parent_path());
	REQUIRE(CreateHardLinkW(t.entry_path(k5).c_str(), t.install_file.c_str(), nullptr));
	o.test_hook_before_write = nullptr;
	cache.close();
	REQUIRE(cache.open(o) == CacheStatus::Ok); // the scan leaves (or refuses to delete) the link
	CHECK(cache.put(k5, payload(100, 6)).status != CacheStatus::Ok);
	CHECK(t.install_snapshot() == before);
	DeleteFileW(t.entry_path(k5).c_str());

	// 5. The cache root itself is moved away and replaced by a junction after open: the held
	//    handles keep pointing at the real directory; nothing reaches the install.
	cache.close();
	o.test_hook_before_write = nullptr;
	REQUIRE(cache.open(o) == CacheStatus::Ok);
	const fs::path moved = t.base / L"moved-cache";
	if (MoveFileW(t.root.c_str(), moved.c_str())) {
		REQUIRE(make_junction(t.root, t.install));
		const CacheStatus st = cache.put(key("nav", 6), payload(100, 7)).status;
		CHECK((st == CacheStatus::Refused || st == CacheStatus::Ok));
		CHECK(t.install_snapshot() == before);
		cache.close();
		RemoveDirectoryW(t.root.c_str());
		MoveFileW(moved.c_str(), t.root.c_str());
	} else {
		// Windows refuses to move a directory while files below it are open (the lock file), which
		// already blocks this swap.
		MESSAGE("the OS refuses to move the open cache root (error ", GetLastError(), "); swap blocked before it can happen");
	}
	CHECK(t.install_snapshot() == before);
}

TEST_CASE("cache: concurrent puts and gets, same-key puts deduplicated") {
	Scratch t;
	Cache cache;
	REQUIRE(cache.open(t.options()) == CacheStatus::Ok);
	constexpr int kThreads = 16;
	constexpr int kOwn = 40;
	constexpr int kShared = 8;
	std::atomic<int> written_shared{ 0 };
	std::atomic<int> reused_shared{ 0 };
	std::atomic<int> produced{ 0 };
	std::atomic<int> failures{ 0 };
	std::atomic<int> ready{ 0 };
	std::vector<std::thread> threads;
	for (int ti = 0; ti < kThreads; ++ti) {
		threads.emplace_back([&, ti] {
			ready.fetch_add(1);
			while (ready.load() < kThreads) {
				std::this_thread::yield();
			}
			for (int i = 0; i < kOwn; ++i) {
				const uint32_t n = uint32_t(1000 + ti * kOwn + i);
				const std::vector<uint8_t> data = payload(size_t(n % 7) * 9000 + 1, n);
				if (cache.put(key("mesh", n), data).status != CacheStatus::Ok) {
					failures.fetch_add(1);
				}
				// Every thread also hammers the shared keys.
				const uint32_t shared = uint32_t(i % kShared);
				PutResult p = cache.put(key("shared", shared), payload(20000, 500 + shared));
				if (p.written) {
					written_shared.fetch_add(1);
				} else if (p.reused) {
					reused_shared.fetch_add(1);
				} else {
					failures.fetch_add(1);
				}
				GetResult g = cache.get_or_put(key("produced", uint32_t(i % kShared)), [&](std::vector<uint8_t> &r_out) {
					produced.fetch_add(1);
					r_out = payload(30000, 900 + uint32_t(i % kShared));
					return true;
				});
				if (g.status != CacheStatus::Ok || g.entry.payload().size() != 30000) {
					failures.fetch_add(1);
				}
				GetResult own = cache.get(key("mesh", n));
				if (own.status != CacheStatus::Ok || own.entry.payload().size() != data.size() ||
						std::memcmp(own.entry.payload().data(), data.data(), data.size()) != 0) {
					failures.fetch_add(1);
				}
			}
		});
	}
	for (std::thread &th : threads) {
		th.join();
	}
	CHECK(failures.load() == 0);
	CHECK(written_shared.load() == kShared);
	CHECK(reused_shared.load() == kThreads * kOwn - kShared);
	CHECK(produced.load() == kShared);
	CacheStats s = cache.stats();
	CHECK(s.entries == uint64_t(kThreads * kOwn + 2 * kShared));
	CHECK(s.puts == uint64_t(kThreads * kOwn + 2 * kShared));
	MESSAGE("dedup waits: ", s.dedup_waits, ", reused: ", s.dedup_reused);
	cache.close();
	CHECK(files_with_suffix(t.root, L".tmp").empty());

	REQUIRE(cache.open(t.options()) == CacheStatus::Ok);
	CHECK(cache.stats().entries == uint64_t(kThreads * kOwn + 2 * kShared));
	CHECK(!cache.stats().journal_compacted);
	std::atomic<int> bad{ 0 };
	threads.clear();
	for (int ti = 0; ti < kThreads; ++ti) {
		threads.emplace_back([&, ti] {
			for (int i = 0; i < kOwn; ++i) {
				if (cache.get(key("mesh", uint32_t(1000 + ti * kOwn + i))).status != CacheStatus::Ok) {
					bad.fetch_add(1);
				}
			}
		});
	}
	for (std::thread &th : threads) {
		th.join();
	}
	CHECK(bad.load() == 0);
	CHECK(cache.stats().verified == uint64_t(kThreads * kOwn));
}

TEST_CASE("cache: LRU eviction order, pins and in-use entries") {
	Scratch t;
	Scratch t2; // declared before the cache: its tree is removed only after the cache has closed
	const std::vector<uint8_t> data = payload(1000, 1);
	const uint64_t entry = data.size() + kCacheEntryHeaderSize;
	const CacheKey a = key("mesh", 1), b = key("mesh", 2), c = key("mesh", 3), d = key("mesh", 4), e = key("mesh", 5);
	CacheOptions o = t.options();
	o.budget_bytes = 4 * entry;
	Cache cache;
	REQUIRE(cache.open(o) == CacheStatus::Ok);
	for (const CacheKey &k : { a, b, c, d, e }) {
		REQUIRE(cache.put(k, data).written);
	}
	CHECK(cache.get(a).status == CacheStatus::Ok); // a becomes the most recent
	REQUIRE(cache.set_pinned(c, true) == CacheStatus::Ok);
	GetResult held = cache.get(d); // d stays mapped: in use
	REQUIRE(held.status == CacheStatus::Ok);

	EvictResult r = cache.evict_to(2 * entry);
	REQUIRE(r.status == CacheStatus::Ok);
	REQUIRE(r.evicted.size() == 3);
	CHECK(r.evicted[0] == b.digest);
	CHECK(r.evicted[1] == e.digest);
	CHECK(r.evicted[2] == a.digest);
	CHECK(r.freed_bytes == 3 * entry);
	CHECK(r.skipped_pinned == 1);
	CHECK(r.skipped_in_use == 1);
	CHECK(cache.contains(c));
	CHECK(cache.contains(d));
	CHECK(!fs::exists(t.entry_path(a)));
	CHECK(!fs::exists(t.entry_path(b)));
	CHECK(held.entry.payload()[10] == data[10]); // the in-use view is untouched
	held.entry.reset();
	CHECK(cache.stats().bytes == 2 * entry);

	// enforce_budget: no-op under the budget; above it, down to 90 %.
	CHECK(cache.enforce_budget().evicted.empty());
	REQUIRE(cache.set_pinned(c, false) == CacheStatus::Ok);
	for (uint32_t n = 10; n < 13; ++n) {
		REQUIRE(cache.put(key("mesh", n), data).written);
	}
	CHECK(cache.stats().bytes == 5 * entry);
	r = cache.enforce_budget(); // budget 4 entries, low watermark 3.6 → keep 3
	CHECK(r.evicted.size() == 2);
	CHECK(r.evicted[0] == c.digest);
	CHECK(r.evicted[1] == d.digest);
	CHECK(cache.stats().bytes == 3 * entry);
	cache.close();

	// Recency and pins survive a reopen (touch and pin records).
	o = t2.options();
	REQUIRE(cache.open(o) == CacheStatus::Ok);
	for (const CacheKey &k : { a, b, c, d }) {
		REQUIRE(cache.put(k, data).written);
	}
	CHECK(cache.get(a).status == CacheStatus::Ok);
	CHECK(cache.get(b).status == CacheStatus::Ok);
	REQUIRE(cache.set_pinned(c, true) == CacheStatus::Ok);
	cache.close();
	REQUIRE(cache.open(o) == CacheStatus::Ok);
	CHECK(cache.stats().pinned == 1);
	r = cache.evict_to(entry);
	REQUIRE(r.evicted.size() == 3);
	CHECK(r.evicted[0] == d.digest);
	CHECK(r.evicted[1] == a.digest);
	CHECK(r.evicted[2] == b.digest);
	CHECK(cache.contains(c));
}

TEST_CASE("cache: close and reopen while other threads put, get, evict and remove") {
	Scratch t;
	Cache cache;
	REQUIRE(cache.open(t.options()) == CacheStatus::Ok);
	constexpr int kThreads = 8;
	constexpr uint32_t kKeys = 64;
	auto data_for = [](uint32_t p_n) { return payload(1000 + p_n * 37, p_n); };
	std::atomic<bool> stop{ false };
	std::atomic<int> failures{ 0 };
	std::atomic<int> closed_seen{ 0 };
	std::atomic<int> ok_reads{ 0 };
	std::atomic<int> ok_puts{ 0 };
	auto expect = [&](bool p_good) {
		if (!p_good) {
			failures.fetch_add(1);
		}
	};
	auto check_entry = [&](const GetResult &p_got, uint32_t p_n) {
		if (p_got.status == CacheStatus::Closed) {
			closed_seen.fetch_add(1);
			std::this_thread::yield();
			return;
		}
		if (p_got.status != CacheStatus::Ok) {
			expect(p_got.status == CacheStatus::Miss);
			return;
		}
		const std::vector<uint8_t> want = data_for(p_n);
		expect(p_got.entry.payload().size() == want.size() && std::memcmp(p_got.entry.payload().data(), want.data(), want.size()) == 0);
		ok_reads.fetch_add(1);
	};
	std::vector<std::thread> threads;
	for (int ti = 0; ti < kThreads; ++ti) {
		threads.emplace_back([&, ti] {
			uint32_t x = uint32_t(ti) * 7919u + 1;
			while (!stop.load()) {
				x = x * 1664525u + 1013904223u;
				const uint32_t n = (x >> 8) % kKeys;
				const CacheKey k = key("race", n);
				switch ((x >> 20) % 8) {
					case 0:
					case 1: {
						const PutResult p = cache.put(k, data_for(n));
						expect(p.status == CacheStatus::Ok || p.status == CacheStatus::Closed);
						ok_puts.fetch_add(p.written ? 1 : 0);
						break;
					}
					case 2:
					case 3:
						check_entry(cache.get(k), n);
						break;
					case 4:
						check_entry(cache.get_or_put(k, [&](std::vector<uint8_t> &r_out) {
							r_out = data_for(n);
							return true;
						}),
								n);
						break;
					case 5: {
						const CacheStatus st = cache.remove(k);
						expect(st == CacheStatus::Ok || st == CacheStatus::Miss || st == CacheStatus::Busy || st == CacheStatus::Closed);
						break;
					}
					case 6: {
						const EvictResult r = cache.evict_to(uint64_t(kKeys) * 1500);
						expect(r.status == CacheStatus::Ok || r.status == CacheStatus::Closed);
						expect(r.delete_failures == 0);
						break;
					}
					default: {
						const CacheStatus st = cache.set_pinned(k, (x & 1) != 0);
						expect(st == CacheStatus::Ok || st == CacheStatus::Miss || st == CacheStatus::Closed);
						(void)cache.contains(k);
						(void)cache.stats();
						break;
					}
				}
			}
		});
	}
	for (int round = 0; round < 24; ++round) {
		std::this_thread::sleep_for(std::chrono::milliseconds(8));
		if (round % 4 == 3) {
			// Two closers at once: both return after the handles are closed, neither double-closes.
			std::thread other([&] { cache.close(); });
			cache.close();
			other.join();
		} else {
			cache.close();
		}
		CHECK(!cache.is_open());
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		std::string why;
		const CacheStatus st = cache.open(t.options(), &why);
		CHECK_MESSAGE(st == CacheStatus::Ok, why);
		CHECK(!cache.stats().recovered_unclean); // every close wrote its close record last
		CHECK(cache.stats().journal_torn_bytes == 0);
	}
	stop.store(true);
	for (std::thread &th : threads) {
		th.join();
	}
	CHECK(failures.load() == 0);
	MESSAGE("entries written: ", ok_puts.load(), ", reads ok: ", ok_reads.load(), ", reads that saw Closed: ", closed_seen.load());
	CHECK(ok_reads.load() > 0);
	CHECK(ok_puts.load() > 0);

	// Whatever survived is intact and fully accounted for.
	cache.close();
	REQUIRE(cache.open(t.options()) == CacheStatus::Ok);
	const CacheStats s = cache.stats();
	CHECK(s.adopted == 0);
	CHECK(s.missing_dropped == 0);
	CHECK(s.corrupt_dropped == 0);
	uint64_t found = 0;
	for (uint32_t n = 0; n < kKeys; ++n) {
		GetResult g = cache.get(key("race", n), true);
		if (g.status == CacheStatus::Ok) {
			++found;
			const std::vector<uint8_t> want = data_for(n);
			CHECK(g.entry.payload().size() == want.size());
			CHECK(std::memcmp(g.entry.payload().data(), want.data(), want.size()) == 0);
		} else {
			CHECK(g.status == CacheStatus::Miss);
			CHECK(!g.dropped_corrupt);
		}
	}
	CHECK(found == s.entries);
	cache.close();
	CHECK(files_with_suffix(t.root, L".tmp").empty());
}

TEST_CASE("cache: a hand-built key naming one digest under two kinds is rejected") {
	Scratch t;
	Cache cache;
	REQUIRE(cache.open(t.options()) == CacheStatus::Ok);
	const CacheKey k = key("mesh", 1);
	REQUIRE(cache.put(k, payload(100, 1)).written);
	CacheKey other = k;
	other.kind = "nav";
	const PutResult p = cache.put(other, payload(100, 2));
	CHECK(p.status == CacheStatus::InvalidArgument);
	CHECK(!p.reused);
	bool ran = false;
	const GetResult g = cache.get_or_put(other, [&](std::vector<uint8_t> &r_out) {
		ran = true;
		r_out = payload(100, 2);
		return true;
	});
	CHECK(g.status == CacheStatus::InvalidArgument);
	CHECK(!ran);
	CHECK(cache.get(other).status == CacheStatus::Miss);
	CHECK(cache.get(k).status == CacheStatus::Ok);
	cache.close();
}

TEST_CASE("cache: a hard link planted at a journaled entry name is never served or deleted") {
	Scratch t;
	const auto before = t.install_snapshot();
	Cache cache;
	REQUIRE(cache.open(t.options()) == CacheStatus::Ok);
	// Same file size as the install file, so the open-time scan's size check accepts the record.
	const CacheKey k = key("mesh", 7);
	REQUIRE(cache.put(k, payload(4096 - kCacheEntryHeaderSize, 7)).written);
	cache.close();
	REQUIRE(DeleteFileW(t.entry_path(k).c_str()));
	REQUIRE(CreateHardLinkW(t.entry_path(k).c_str(), t.install_file.c_str(), nullptr));
	REQUIRE(cache.open(t.options()) == CacheStatus::Ok);
	const GetResult g = cache.get(k);
	CHECK(g.status == CacheStatus::Miss);
	CHECK(g.dropped_corrupt);
	CHECK(cache.stats().refused >= 1); // the delete of the link was refused
	CHECK(!cache.contains(k));
	CHECK(fs::exists(t.entry_path(k))); // left alone
	CHECK(fs::exists(t.install_file));
	CHECK(t.install_snapshot() == before);
	cache.close();
	DeleteFileW(t.entry_path(k).c_str());
}
