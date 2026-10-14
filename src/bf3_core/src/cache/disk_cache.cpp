// The derived-data cache. See disk_cache.h for the layout and the guarantees, cache_fs.h for the
// handle-relative file operations that make the writes junction-proof.
#include "bf3/core/cache/disk_cache.h"

#include "bf3/core/cache/entry_format.h"
#include "cache_fs.h"
#include "cache_journal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace bf3::core {

namespace cd = cache_detail;
namespace fs = std::filesystem;

namespace {

constexpr wchar_t kJournalName[] = L"index.log";
constexpr wchar_t kLockName[] = L"lock";
constexpr uint64_t kJournalMaxBytes = uint64_t(1) << 30;

std::atomic<uint64_t> g_temp_sequence{ 0 };

std::wstring widen_ascii(std::string_view p_s) {
	return std::wstring(p_s.begin(), p_s.end());
}

// Names in the cache tree are ASCII; anything else is foreign.
bool narrow_ascii(std::wstring_view p_w, std::string &r_out) {
	r_out.clear();
	for (wchar_t c : p_w) {
		if (c <= 0 || c >= 0x80) {
			return false;
		}
		r_out.push_back(static_cast<char>(c));
	}
	return true;
}

bool ends_with(std::wstring_view p_s, std::wstring_view p_suffix) {
	return p_s.size() >= p_suffix.size() && p_s.substr(p_s.size() - p_suffix.size()) == p_suffix;
}

bool is_lower_hex(std::wstring_view p_s) {
	for (wchar_t c : p_s) {
		if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) {
			return false;
		}
	}
	return !p_s.empty();
}

std::wstring layout_dir_name() {
	return L"v" + std::to_wstring(kCacheContainerVersion);
}

std::wstring entry_file_name(const Sha256Digest &p_key) {
	return widen_ascii(p_key.hex() + ".bin");
}

std::wstring fan_dir_name(const Sha256Digest &p_key) {
	return widen_ascii(p_key.hex().substr(0, 2));
}

std::wstring temp_name(std::wstring_view p_base) {
	return std::wstring(p_base) + L"." + std::to_wstring(cd::current_process_id()) + L"-" +
			std::to_wstring(g_temp_sequence.fetch_add(1, std::memory_order_relaxed)) + L".tmp";
}

// The guard check on a location the file system has already resolved (the final path of an open
// handle). This is the authoritative check: it describes the directory object the write goes to.
bool final_path_allowed(const PathGuard &p_guard, const cd::Handle &p_handle) {
	const std::optional<fs::path> p = cd::final_path(p_handle);
	if (!p || p_guard.protected_roots().empty()) {
		return false;
	}
	for (const fs::path &root : p_guard.protected_roots()) {
		if (path_is_within(*p, root)) {
			return false;
		}
	}
	return true;
}

bool is_not_found(uint32_t p_err) {
	return p_err == 2 || p_err == 3; // ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND
}

} // namespace

struct Cache::State {
	struct Slot {
		std::string kind;
		uint64_t payload_size = 0;
		uint64_t file_size = 0;
		uint64_t tick = 0;
		uint64_t persisted_tick = 0;
		uint64_t generation = 0;
		uint32_t refs = 0; // live CacheEntry views
		bool pinned = false;
		bool verified = false; // payload hash checked in this process
	};

	CacheOptions options;
	fs::path root_final;
	cd::Handle root;
	cd::Handle vdir;
	cd::Handle lock;
	cd::Handle journal;

	mutable std::mutex mu;
	std::condition_variable cv;
	std::unordered_map<Sha256Digest, Slot, DigestHash> index;
	std::unordered_set<Sha256Digest, DigestHash> busy; // a put, delete or eviction is in progress
	uint64_t clock = 1;
	uint64_t generation = 0;
	uint64_t total = 0;
	CacheStats stats;
	bool open = false;

	// ---- file operations (no lock needed: they touch only handles and the immutable options)

	// Opens v1/<kind>/<fan> relative to the held v1 handle and checks its final path.
	CacheStatus open_target_dir(std::string_view p_kind, const Sha256Digest &p_key, bool p_create, cd::Handle &r_dir, uint32_t &r_err) const {
		cd::Handle kind_dir = cd::open_child_dir(vdir, widen_ascii(p_kind), p_create, r_err);
		if (!kind_dir.valid()) {
			return r_err == cd::kErrReparsePoint ? CacheStatus::Refused : is_not_found(r_err) ? CacheStatus::Miss : CacheStatus::Io;
		}
		r_dir = cd::open_child_dir(kind_dir, fan_dir_name(p_key), p_create, r_err);
		if (!r_dir.valid()) {
			return r_err == cd::kErrReparsePoint ? CacheStatus::Refused : is_not_found(r_err) ? CacheStatus::Miss : CacheStatus::Io;
		}
		if (!final_path_allowed(options.guard, r_dir)) {
			r_dir.reset();
			return CacheStatus::Refused;
		}
		return CacheStatus::Ok;
	}

	// Deletes one name inside a verified directory: never a hard-linked file, never outside the
	// guard (checked on the handle being deleted). A reparse point is deleted as the link itself.
	CacheStatus delete_child(const cd::Handle &p_dir, std::wstring_view p_name, uint32_t &r_err) const {
		cd::Handle h = cd::open_child_any(p_dir, p_name, r_err);
		if (!h.valid()) {
			return is_not_found(r_err) ? CacheStatus::Ok : CacheStatus::Io;
		}
		cd::FileInfo info;
		if (!cd::query_info(h, info, r_err)) {
			return CacheStatus::Io;
		}
		if (!info.is_directory() && info.links > 1) {
			r_err = cd::kErrHardLinked;
			return CacheStatus::Refused;
		}
		if (!final_path_allowed(options.guard, h)) {
			return CacheStatus::Refused;
		}
		return cd::delete_by_handle(h, r_err) ? CacheStatus::Ok : CacheStatus::Io;
	}

	CacheStatus delete_entry_file(std::string_view p_kind, const Sha256Digest &p_key, uint32_t &r_err) const {
		cd::Handle dir;
		const CacheStatus st = open_target_dir(p_kind, p_key, false, dir, r_err);
		if (st == CacheStatus::Miss) {
			return CacheStatus::Ok;
		}
		if (st != CacheStatus::Ok) {
			return st;
		}
		return delete_child(dir, entry_file_name(p_key), r_err);
	}

	// Writes p_parts to a fresh temp file in p_dir, flushes it and renames it over p_name.
	CacheStatus atomic_replace(const cd::Handle &p_dir, std::wstring_view p_name, std::initializer_list<std::span<const uint8_t>> p_parts,
			uint32_t &r_err) const {
		cd::Handle tmp = cd::create_child_file(p_dir, temp_name(p_name.substr(0, p_name.size() < 64 ? p_name.size() : 64)), r_err);
		if (!tmp.valid()) {
			return CacheStatus::Io;
		}
		bool ok = true;
		for (std::span<const uint8_t> part : p_parts) {
			ok = ok && cd::write_all(tmp, part.data(), part.size(), r_err);
		}
		ok = ok && cd::flush(tmp, r_err);
		CacheStatus st = ok ? CacheStatus::Ok : CacheStatus::Io;
		if (ok) {
			// Never replace a hard link (its other name could be an install file), a directory or a
			// reparse point that someone put at the target name.
			uint32_t probe_err = 0;
			cd::Handle existing = cd::open_child_any(p_dir, p_name, probe_err);
			if (existing.valid()) {
				cd::FileInfo info;
				if (!cd::query_info(existing, info, r_err)) {
					st = CacheStatus::Io;
				} else if (info.is_reparse() || info.is_directory()) {
					r_err = info.is_reparse() ? cd::kErrReparsePoint : cd::kErrNotRegular;
					st = CacheStatus::Refused;
				} else if (info.links > 1) {
					r_err = cd::kErrHardLinked;
					st = CacheStatus::Refused;
				}
			} else if (!is_not_found(probe_err)) {
				r_err = probe_err;
				st = CacheStatus::Io;
			}
		}
		if (st == CacheStatus::Ok && !cd::rename_into(tmp, p_dir, p_name, r_err)) {
			st = CacheStatus::Io;
		}
		if (st != CacheStatus::Ok) {
			uint32_t ignored = 0;
			cd::delete_by_handle(tmp, ignored);
		}
		return st;
	}

	PutResult write_entry(const CacheKey &p_key, std::span<const uint8_t> p_payload) const {
		PutResult r;
		// 1. Path-level check (aliases, 8.3 names, existing junctions, hard-linked target).
		const fs::path target = root_final / p_key.relative_path();
		if (options.guard.check_write_target(target) != PathGuard::Verdict::Allowed) {
			r.status = CacheStatus::Refused;
			return r;
		}
		if (options.test_hook_before_write) {
			options.test_hook_before_write(target);
		}
		// 2. Handle walk below the held v1 handle (no links followed) and a final-path check of the
		//    directory the temp file is created in; nothing after this point resolves a path.
		cd::Handle dir;
		r.status = open_target_dir(p_key.kind, p_key.digest, true, dir, r.win32_error);
		if (r.status != CacheStatus::Ok) {
			r.status = r.status == CacheStatus::Miss ? CacheStatus::Io : r.status;
			return r;
		}
		CacheEntryHeader header;
		header.key = p_key.digest;
		header.kind = p_key.kind;
		header.payload_size = p_payload.size();
		header.payload_sha256 = sha256_of(p_payload);
		std::array<uint8_t, kCacheEntryHeaderSize> head{};
		encode_entry_header(header, head);
		r.status = atomic_replace(dir, entry_file_name(p_key.digest), { std::span<const uint8_t>(head), p_payload }, r.win32_error);
		r.written = r.status == CacheStatus::Ok;
		return r;
	}

	// ---- index operations (caller holds mu)

	void append_locked(const std::vector<uint8_t> &p_record) {
		uint32_t err = 0;
		if (!journal.valid() || !cd::write_all(journal, p_record.data(), p_record.size(), err)) {
			++stats.io_errors; // the open-time scan reconciles a journal that missed a record
			return;
		}
		++stats.journal_records;
	}

	void commit_locked(const CacheKey &p_key, uint64_t p_payload_size, const PutResult &p_result) {
		if (p_result.status != CacheStatus::Ok) {
			++(p_result.status == CacheStatus::Refused ? stats.refused : stats.io_errors);
			return;
		}
		Slot slot;
		slot.kind = p_key.kind;
		slot.payload_size = p_payload_size;
		slot.file_size = p_payload_size + kCacheEntryHeaderSize;
		slot.tick = clock++;
		slot.persisted_tick = slot.tick;
		slot.generation = ++generation;
		slot.verified = true; // written from memory by this process
		total += slot.file_size;
		++stats.puts;
		std::vector<uint8_t> rec;
		cd::journal_put(rec, p_key.digest, slot.kind, p_payload_size, slot.tick);
		index[p_key.digest] = std::move(slot);
		append_locked(rec);
	}

	// Removes a slot from the index (and journals it). Returns its kind and file size.
	void drop_locked(std::unordered_map<Sha256Digest, Slot, DigestHash>::iterator p_it, cd::RemoveReason p_reason) {
		total -= p_it->second.file_size;
		std::vector<uint8_t> rec;
		cd::journal_remove(rec, p_it->first, p_reason);
		index.erase(p_it);
		append_locked(rec);
	}

	void flush_touches_locked() {
		std::vector<uint8_t> rec;
		uint64_t n = 0;
		for (auto &[key, slot] : index) {
			if (slot.tick != slot.persisted_tick) {
				cd::journal_touch(rec, key, slot.tick);
				slot.persisted_tick = slot.tick;
				++n;
			}
		}
		if (!rec.empty()) {
			append_locked(rec);
			stats.journal_records += n - 1;
		}
	}

	// Rewrites the journal from the index: header + one Put (+ Pin) per entry, atomically.
	CacheStatus write_snapshot_locked(uint32_t &r_err) {
		std::vector<std::pair<const Sha256Digest *, Slot *>> order;
		order.reserve(index.size());
		for (auto &[key, slot] : index) {
			order.emplace_back(&key, &slot);
		}
		std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) {
			return a.second->tick != b.second->tick ? a.second->tick < b.second->tick : *a.first < *b.first;
		});
		std::vector<uint8_t> bytes;
		cd::journal_header(bytes);
		uint64_t records = 0;
		for (auto &[key, slot] : order) {
			cd::journal_put(bytes, *key, slot->kind, slot->payload_size, slot->tick);
			slot->persisted_tick = slot->tick;
			++records;
			if (slot->pinned) {
				cd::journal_pin(bytes, *key, true);
				++records;
			}
		}
		journal.reset();
		const CacheStatus st = atomic_replace(vdir, kJournalName, { std::span<const uint8_t>(bytes) }, r_err);
		uint32_t append_err = 0;
		journal = cd::open_child_append(vdir, kJournalName, append_err);
		if (st != CacheStatus::Ok) {
			return st;
		}
		r_err = append_err;
		if (!journal.valid()) {
			return CacheStatus::Io;
		}
		stats.journal_records = records;
		stats.journal_compacted = true;
		return CacheStatus::Ok;
	}

	// ---- open-time reconciliation (single-threaded, before the State is published)

	void scan(cd::JournalReplay &r_replay, bool &r_changed) {
		uint32_t err = 0;
		std::vector<cd::DirEntry> kinds;
		if (!cd::list_dir(vdir, kinds, err)) {
			++stats.io_errors;
			return;
		}
		for (const cd::DirEntry &k : kinds) {
			std::string kind;
			if (!k.is_directory() || k.is_reparse()) {
				if (!k.is_directory() && ends_with(k.name, L".tmp")) {
					if (delete_child(vdir, k.name, err) == CacheStatus::Ok) {
						++stats.stray_temps_removed;
					}
				} else if (k.name != kJournalName && k.name != kLockName) {
					++stats.foreign;
				}
				continue;
			}
			if (!narrow_ascii(k.name, kind) || !is_valid_cache_kind(kind)) {
				++stats.foreign;
				continue;
			}
			cd::Handle kind_dir = cd::open_child_dir(vdir, k.name, false, err);
			std::vector<cd::DirEntry> fans;
			if (!kind_dir.valid() || !cd::list_dir(kind_dir, fans, err)) {
				++stats.foreign;
				continue;
			}
			for (const cd::DirEntry &f : fans) {
				if (!f.is_directory() || f.is_reparse() || f.name.size() != 2 || !is_lower_hex(f.name)) {
					++stats.foreign;
					continue;
				}
				cd::Handle fan_dir = cd::open_child_dir(kind_dir, f.name, false, err);
				std::vector<cd::DirEntry> files;
				if (!fan_dir.valid() || !final_path_allowed(options.guard, fan_dir) || !cd::list_dir(fan_dir, files, err)) {
					++stats.foreign;
					continue;
				}
				for (const cd::DirEntry &x : files) {
					scan_file(fan_dir, f.name, kind, x, r_replay, r_changed);
				}
			}
		}
		// Records whose file is gone.
		stats.missing_dropped = r_replay.entries.size();
		r_changed = r_changed || !r_replay.entries.empty();
	}

	void scan_file(const cd::Handle &p_fan_dir, const std::wstring &p_fan, const std::string &p_kind, const cd::DirEntry &p_file,
			cd::JournalReplay &r_replay, bool &r_changed) {
		uint32_t err = 0;
		if (p_file.is_directory() || p_file.is_reparse()) {
			++stats.foreign;
			return;
		}
		if (ends_with(p_file.name, L".tmp")) {
			r_changed = true;
			if (delete_child(p_fan_dir, p_file.name, err) == CacheStatus::Ok) {
				++stats.stray_temps_removed;
			}
			return;
		}
		std::string hex;
		if (p_file.name.size() != 68 || !ends_with(p_file.name, L".bin") || !is_lower_hex(std::wstring_view(p_file.name).substr(0, 64)) ||
				p_file.name.compare(0, 2, p_fan) != 0 || !narrow_ascii(std::wstring_view(p_file.name).substr(0, 64), hex)) {
			++stats.foreign;
			return;
		}
		const std::optional<Sha256Digest> key = Sha256Digest::from_hex(hex);
		if (!key || index.count(*key) != 0) {
			++stats.foreign;
			return;
		}
		auto j = r_replay.entries.find(*key);
		const bool partial = p_file.size < kCacheEntryHeaderSize || p_file.size - kCacheEntryHeaderSize > kCacheMaxPayload;
		const bool mismatch = j != r_replay.entries.end() &&
				(j->second.kind != p_kind || j->second.payload_size + kCacheEntryHeaderSize != p_file.size);
		if (partial || mismatch) {
			r_changed = true;
			if (j != r_replay.entries.end()) {
				r_replay.entries.erase(j);
			}
			if (delete_child(p_fan_dir, p_file.name, err) == CacheStatus::Ok) {
				++stats.corrupt_dropped;
			} else {
				++stats.refused;
			}
			return;
		}
		Slot slot;
		slot.kind = p_kind;
		slot.file_size = p_file.size;
		slot.payload_size = p_file.size - kCacheEntryHeaderSize;
		slot.generation = ++generation;
		if (j != r_replay.entries.end()) {
			slot.tick = j->second.tick;
			slot.pinned = j->second.pinned;
			r_replay.entries.erase(j);
		} else {
			// Written but not journaled (crash between rename and append, or a lost journal): adopt
			// as the oldest, but only a single-link file whose header names this key, kind and size.
			// A hard link is neither adopted nor deleted (its other name may be an install file).
			r_changed = true;
			cd::Handle f = cd::open_child_file_read(p_fan_dir, p_file.name, err);
			cd::FileInfo info;
			if (!f.valid() || !cd::query_info(f, info, err) || info.links > 1) {
				++stats.foreign;
				return;
			}
			std::array<uint8_t, kCacheEntryHeaderSize> head{};
			CacheEntryHeader header;
			const bool good = cd::read_prefix(f, head.data(), head.size(), err) &&
					decode_entry_head(head, info.size, header) == EntryHeaderCheck::Ok && header.key == *key && header.kind == p_kind;
			f.reset();
			if (!good) {
				if (delete_child(p_fan_dir, p_file.name, err) == CacheStatus::Ok) {
					++stats.corrupt_dropped;
				}
				return;
			}
			slot.tick = 0;
			++stats.adopted;
		}
		slot.persisted_tick = slot.tick;
		total += slot.file_size;
		index.emplace(*key, std::move(slot));
	}
};

// ------------------------------------------------------------------------------------------------
// CacheEntry

struct CacheEntry::Release {
	std::shared_ptr<Cache::State> state;
	Sha256Digest key;
	uint64_t generation = 0;
	const void *view = nullptr;

	~Release() {
		cd::unmap(view);
		std::lock_guard lk(state->mu);
		auto it = state->index.find(key);
		if (it != state->index.end() && it->second.generation == generation && it->second.refs > 0) {
			--it->second.refs;
		}
	}
};

CacheEntry::~CacheEntry() = default;
CacheEntry::CacheEntry(CacheEntry &&p_other) noexcept :
		owner_(std::move(p_other.owner_)), view_(std::exchange(p_other.view_, nullptr)), payload_(std::exchange(p_other.payload_, {})), key_(p_other.key_) {}

CacheEntry &CacheEntry::operator=(CacheEntry &&p_other) noexcept {
	if (this != &p_other) {
		owner_ = std::move(p_other.owner_);
		view_ = std::exchange(p_other.view_, nullptr);
		payload_ = std::exchange(p_other.payload_, {});
		key_ = p_other.key_;
	}
	return *this;
}

void CacheEntry::reset() {
	owner_.reset();
	view_ = nullptr;
	payload_ = {};
}

// ------------------------------------------------------------------------------------------------
// Cache

const char *cache_status_name(CacheStatus p_status) {
	switch (p_status) {
		case CacheStatus::Ok:
			return "ok";
		case CacheStatus::Miss:
			return "miss";
		case CacheStatus::Refused:
			return "refused";
		case CacheStatus::Busy:
			return "busy";
		case CacheStatus::Io:
			return "io error";
		case CacheStatus::InvalidArgument:
			return "invalid argument";
		case CacheStatus::Closed:
			return "closed";
		case CacheStatus::ProducerFailed:
			return "producer failed";
	}
	return "?";
}

Cache::~Cache() {
	close();
}

Cache::Cache(Cache &&p_other) noexcept :
		state_(std::move(p_other.state_)) {}

Cache &Cache::operator=(Cache &&p_other) noexcept {
	if (this != &p_other) {
		close();
		state_ = std::move(p_other.state_);
	}
	return *this;
}

CacheStatus Cache::open(CacheOptions p_options, std::string *r_detail) {
	close();
	auto fail = [&](CacheStatus p_status, std::string p_why, uint32_t p_err = 0) {
		if (r_detail != nullptr) {
			*r_detail = std::move(p_why);
			if (p_err != 0) {
				*r_detail += " (error " + std::to_string(p_err) + ")";
			}
		}
		return p_status;
	};
	if (p_options.guard.protected_roots().empty()) {
		return fail(CacheStatus::Refused, "the guard protects nothing");
	}
	if (p_options.root.empty() || !p_options.root.is_absolute()) {
		return fail(CacheStatus::InvalidArgument, "cache root must be an absolute path");
	}
	const PathGuard::Verdict verdict = p_options.guard.check_write_target(p_options.root);
	if (verdict != PathGuard::Verdict::Allowed) {
		return fail(CacheStatus::Refused, std::string("cache root: ") + PathGuard::verdict_name(verdict));
	}
	const std::optional<fs::path> canonical = canonicalize_path(p_options.root);
	if (!canonical) {
		return fail(CacheStatus::InvalidArgument, "cache root cannot be resolved");
	}

	auto s = std::make_shared<State>();
	s->options = std::move(p_options);
	uint32_t err = 0;

	// Open the deepest existing ancestor (following links, then verified by its final path) and
	// create the rest relative to handles.
	fs::path existing = *canonical;
	std::deque<std::wstring> rest;
	std::error_code ec;
	while (!fs::exists(existing, ec)) {
		const fs::path parent = existing.parent_path();
		if (parent == existing || parent.empty()) {
			return fail(CacheStatus::Io, "no existing ancestor of the cache root");
		}
		rest.push_front(existing.filename().native());
		existing = parent;
	}
	cd::Handle cur = cd::open_dir_path(existing, err);
	if (!cur.valid()) {
		return fail(CacheStatus::Io, "cannot open the cache root", err);
	}
	if (!final_path_allowed(s->options.guard, cur)) {
		return fail(CacheStatus::Refused, "cache root resolves into a protected root");
	}
	for (const std::wstring &component : rest) {
		if (!cd::valid_component(component)) {
			return fail(CacheStatus::InvalidArgument, "invalid cache root component");
		}
		cur = cd::open_child_dir(cur, component, true, err);
		if (!cur.valid()) {
			return fail(err == cd::kErrReparsePoint ? CacheStatus::Refused : CacheStatus::Io, "cannot create the cache root", err);
		}
	}
	s->root = std::move(cur);
	const std::optional<fs::path> root_final = cd::final_path(s->root);
	if (!root_final || !final_path_allowed(s->options.guard, s->root)) {
		return fail(CacheStatus::Refused, "cache root resolves into a protected root");
	}
	s->root_final = *root_final;
	s->vdir = cd::open_child_dir(s->root, layout_dir_name(), true, err);
	if (!s->vdir.valid() || !final_path_allowed(s->options.guard, s->vdir)) {
		return fail(err == 0 || err == cd::kErrReparsePoint ? CacheStatus::Refused : CacheStatus::Io, "cannot open the layout directory", err);
	}
	s->lock = cd::open_child_lock(s->vdir, kLockName, err);
	if (!s->lock.valid()) {
		return fail(err == 32 /* ERROR_SHARING_VIOLATION */ ? CacheStatus::Busy : CacheStatus::Io, "cannot lock the cache", err);
	}

	// Journal replay.
	std::vector<uint8_t> bytes;
	bool journal_exists = false;
	{
		cd::Handle jf = cd::open_child_file_read(s->vdir, kJournalName, err);
		if (jf.valid()) {
			journal_exists = true;
			if (!cd::read_all(jf, bytes, kJournalMaxBytes, err)) {
				bytes.clear(); // unreadable: rebuilt from the scan
			}
		} else if (!is_not_found(err)) {
			return fail(CacheStatus::Io, "cannot read the journal", err);
		}
	}
	cd::JournalReplay replay = cd::replay_journal(bytes);
	const uint64_t replayed_records = replay.records;
	s->stats.journal_torn_bytes = journal_exists ? replay.torn_bytes : 0;
	s->stats.recovered_unclean = journal_exists && !replay.clean_close;
	s->clock = replay.max_tick + 1;

	// Reconcile with the files on disk.
	bool changed = false;
	s->scan(replay, changed);

	const bool compact = !journal_exists || !replay.header_ok || replay.torn_bytes != 0 || changed ||
			replayed_records > 2 * s->index.size() + 1024;
	if (compact) {
		if (s->write_snapshot_locked(err) != CacheStatus::Ok) {
			return fail(CacheStatus::Io, "cannot rewrite the journal", err);
		}
	} else {
		s->journal = cd::open_child_append(s->vdir, kJournalName, err);
		if (!s->journal.valid()) {
			return fail(CacheStatus::Io, "cannot open the journal", err);
		}
		s->stats.journal_records = replayed_records;
	}
	s->open = true;
	state_ = std::move(s);
	return CacheStatus::Ok;
}

bool Cache::is_open() const {
	if (!state_) {
		return false;
	}
	std::lock_guard lk(state_->mu);
	return state_->open;
}

void Cache::close() {
	if (!state_) {
		return;
	}
	State &s = *state_;
	{
		std::unique_lock lk(s.mu);
		if (s.open) {
			// Let in-flight writers finish so their records land before the close marker.
			s.cv.wait(lk, [&] { return s.busy.empty(); });
			s.flush_touches_locked();
			std::vector<uint8_t> rec;
			cd::journal_close(rec, s.clock);
			s.append_locked(rec);
			uint32_t err = 0;
			if (s.journal.valid()) {
				cd::flush(s.journal, err);
			}
			s.open = false;
			s.journal.reset();
			s.lock.reset();
			s.vdir.reset();
			s.root.reset();
		}
		s.cv.notify_all();
	}
	state_.reset();
}

PutResult Cache::put(const CacheKey &p_key, std::span<const uint8_t> p_payload) {
	PutResult r;
	const std::shared_ptr<State> s = state_;
	if (!s) {
		return r;
	}
	if (!is_valid_cache_kind(p_key.kind) || p_payload.size() > kCacheMaxPayload) {
		r.status = CacheStatus::InvalidArgument;
		return r;
	}
	std::unique_lock lk(s->mu);
	bool waited = false;
	for (;;) {
		if (!s->open) {
			return r;
		}
		if (s->index.count(p_key.digest) != 0) {
			++s->stats.dedup_reused;
			r.status = CacheStatus::Ok;
			r.reused = true;
			return r;
		}
		if (s->busy.count(p_key.digest) == 0) {
			break;
		}
		if (!waited) {
			++s->stats.dedup_waits;
			waited = true;
		}
		s->cv.wait(lk);
	}
	s->busy.insert(p_key.digest);
	lk.unlock();
	r = s->write_entry(p_key, p_payload);
	lk.lock();
	s->commit_locked(p_key, p_payload.size(), r);
	s->busy.erase(p_key.digest);
	s->cv.notify_all();
	return r;
}

GetResult Cache::get(const CacheKey &p_key, bool p_verify) {
	GetResult r;
	const std::shared_ptr<State> s = state_;
	if (!s) {
		return r;
	}
	std::unique_lock lk(s->mu);
	s->cv.wait(lk, [&] { return !s->open || s->busy.count(p_key.digest) == 0; });
	if (!s->open) {
		return r;
	}
	auto it = s->index.find(p_key.digest);
	if (it == s->index.end() || it->second.kind != p_key.kind) {
		++s->stats.misses;
		r.status = CacheStatus::Miss;
		return r;
	}
	State::Slot &slot = it->second;
	++slot.refs;
	const uint64_t generation = slot.generation;
	const bool hash = p_verify || s->options.verify == CacheVerify::Always || (s->options.verify == CacheVerify::FirstUse && !slot.verified);
	lk.unlock();

	// Map and validate outside the lock.
	const void *view = nullptr;
	uint64_t file_size = 0;
	bool bad = false;
	cd::Handle dir;
	CacheStatus st = s->open_target_dir(p_key.kind, p_key.digest, false, dir, r.win32_error);
	if (st == CacheStatus::Ok) {
		cd::Handle file = cd::open_child_file_read(dir, entry_file_name(p_key.digest), r.win32_error);
		cd::FileInfo info;
		if (!file.valid()) {
			st = is_not_found(r.win32_error) || r.win32_error == cd::kErrReparsePoint ? CacheStatus::Miss : CacheStatus::Io;
		} else if (!cd::query_info(file, info, r.win32_error)) {
			st = CacheStatus::Io;
		} else if (info.size < kCacheEntryHeaderSize) {
			bad = true;
		} else {
			file_size = info.size;
			view = cd::map_read(file, file_size, r.win32_error);
			st = view != nullptr ? CacheStatus::Ok : CacheStatus::Io;
		}
	}
	if (st == CacheStatus::Miss) {
		bad = true; // indexed but gone or replaced by a link: drop it
	}
	if (view != nullptr) {
		const std::span<const uint8_t> file(static_cast<const uint8_t *>(view), static_cast<size_t>(file_size));
		CacheEntryHeader header;
		const EntryHeaderCheck check = decode_entry_header(file, header);
		bad = check != EntryHeaderCheck::Ok || header.key != p_key.digest || header.kind != p_key.kind;
		if (!bad && hash) {
			bad = sha256_of(file.subspan(kCacheEntryHeaderSize)) != header.payload_sha256;
		}
	}

	lk.lock();
	if (bad || st != CacheStatus::Ok) {
		cd::unmap(view);
		auto again = s->index.find(p_key.digest);
		const bool same = again != s->index.end() && again->second.generation == generation;
		if (same && again->second.refs > 0) {
			--again->second.refs;
		}
		if (!bad) {
			++s->stats.io_errors;
			r.status = st;
			return r;
		}
		++s->stats.misses;
		r.status = CacheStatus::Miss;
		r.dropped_corrupt = true;
		if (!same) {
			return r; // another reader dropped it already
		}
		const std::string kind = again->second.kind;
		s->drop_locked(again, cd::RemoveReason::Corrupt);
		++s->stats.corrupt_dropped;
		s->busy.insert(p_key.digest);
		lk.unlock();
		uint32_t err = 0;
		const CacheStatus del = s->delete_entry_file(kind, p_key.digest, err);
		lk.lock();
		if (del == CacheStatus::Refused) {
			++s->stats.refused;
		} else if (del != CacheStatus::Ok) {
			++s->stats.io_errors;
		}
		s->busy.erase(p_key.digest);
		s->cv.notify_all();
		return r;
	}
	auto again = s->index.find(p_key.digest);
	if (again != s->index.end() && again->second.generation == generation) {
		again->second.tick = s->clock++;
		if (hash) {
			again->second.verified = true;
		}
	}
	if (hash) {
		++s->stats.verified;
	}
	++s->stats.hits;
	lk.unlock();

	auto owner = std::make_shared<CacheEntry::Release>();
	owner->state = s;
	owner->key = p_key.digest;
	owner->generation = generation;
	owner->view = view;
	r.entry.owner_ = std::move(owner);
	r.entry.view_ = view;
	r.entry.payload_ = std::span<const uint8_t>(static_cast<const uint8_t *>(view) + kCacheEntryHeaderSize,
			static_cast<size_t>(file_size - kCacheEntryHeaderSize));
	r.entry.key_ = p_key.digest;
	r.status = CacheStatus::Ok;
	return r;
}

GetResult Cache::get_or_put(const CacheKey &p_key, const std::function<bool(std::vector<uint8_t> &r_payload)> &p_produce) {
	const std::shared_ptr<State> s = state_;
	if (!s) {
		return {};
	}
	if (!is_valid_cache_kind(p_key.kind)) {
		GetResult r;
		r.status = CacheStatus::InvalidArgument;
		return r;
	}
	for (;;) {
		GetResult got = get(p_key);
		if (got.status != CacheStatus::Miss) {
			return got;
		}
		std::unique_lock lk(s->mu);
		if (!s->open) {
			return {};
		}
		if (s->index.count(p_key.digest) != 0) {
			continue; // another thread just put it
		}
		if (s->busy.count(p_key.digest) != 0) {
			++s->stats.dedup_waits;
			s->cv.wait(lk, [&] { return !s->open || s->busy.count(p_key.digest) == 0; });
			continue;
		}
		s->busy.insert(p_key.digest);
		lk.unlock();
		std::vector<uint8_t> payload;
		const bool produced = p_produce(payload);
		PutResult put;
		if (!produced) {
			put.status = CacheStatus::ProducerFailed;
		} else if (payload.size() > kCacheMaxPayload) {
			put.status = CacheStatus::InvalidArgument;
		} else {
			put = s->write_entry(p_key, payload);
		}
		lk.lock();
		if (produced) {
			s->commit_locked(p_key, payload.size(), put);
		}
		s->busy.erase(p_key.digest);
		s->cv.notify_all();
		lk.unlock();
		if (put.status != CacheStatus::Ok) {
			GetResult r;
			r.status = put.status;
			r.win32_error = put.win32_error;
			return r;
		}
		GetResult out = get(p_key);
		out.produced = true;
		return out;
	}
}

bool Cache::contains(const CacheKey &p_key) const {
	if (!state_) {
		return false;
	}
	std::lock_guard lk(state_->mu);
	auto it = state_->index.find(p_key.digest);
	return state_->open && it != state_->index.end() && it->second.kind == p_key.kind;
}

CacheStatus Cache::verify(const CacheKey &p_key) {
	return get(p_key, true).status;
}

CacheStatus Cache::remove(const CacheKey &p_key) {
	const std::shared_ptr<State> s = state_;
	if (!s) {
		return CacheStatus::Closed;
	}
	std::unique_lock lk(s->mu);
	s->cv.wait(lk, [&] { return !s->open || s->busy.count(p_key.digest) == 0; });
	if (!s->open) {
		return CacheStatus::Closed;
	}
	auto it = s->index.find(p_key.digest);
	if (it == s->index.end() || it->second.kind != p_key.kind) {
		return CacheStatus::Miss;
	}
	if (it->second.refs > 0) {
		return CacheStatus::Busy;
	}
	s->drop_locked(it, cd::RemoveReason::Explicit);
	s->busy.insert(p_key.digest);
	lk.unlock();
	uint32_t err = 0;
	const CacheStatus st = s->delete_entry_file(p_key.kind, p_key.digest, err);
	lk.lock();
	s->busy.erase(p_key.digest);
	s->cv.notify_all();
	return st;
}

CacheStatus Cache::set_pinned(const CacheKey &p_key, bool p_pinned) {
	if (!state_) {
		return CacheStatus::Closed;
	}
	State &s = *state_;
	std::lock_guard lk(s.mu);
	if (!s.open) {
		return CacheStatus::Closed;
	}
	auto it = s.index.find(p_key.digest);
	if (it == s.index.end() || it->second.kind != p_key.kind) {
		return CacheStatus::Miss;
	}
	if (it->second.pinned != p_pinned) {
		it->second.pinned = p_pinned;
		std::vector<uint8_t> rec;
		cd::journal_pin(rec, p_key.digest, p_pinned);
		s.append_locked(rec);
	}
	return CacheStatus::Ok;
}

EvictResult Cache::evict_to(uint64_t p_target_bytes) {
	EvictResult r;
	const std::shared_ptr<State> s = state_;
	if (!s) {
		return r;
	}
	struct Victim {
		Sha256Digest key;
		std::string kind;
	};
	std::vector<Victim> victims;
	{
		std::lock_guard lk(s->mu);
		if (!s->open) {
			return r;
		}
		std::vector<std::pair<uint64_t, Sha256Digest>> candidates;
		for (const auto &[key, slot] : s->index) {
			if (slot.pinned) {
				++r.skipped_pinned;
			} else if (slot.refs > 0) {
				++r.skipped_in_use;
			} else if (s->busy.count(key) == 0) {
				candidates.emplace_back(slot.tick, key);
			}
		}
		std::sort(candidates.begin(), candidates.end());
		for (const auto &[tick, key] : candidates) {
			if (s->total <= p_target_bytes) {
				break;
			}
			auto it = s->index.find(key);
			victims.push_back({ key, it->second.kind });
			r.freed_bytes += it->second.file_size;
			s->drop_locked(it, cd::RemoveReason::Evict);
			s->busy.insert(key);
			r.evicted.push_back(key);
		}
		s->stats.evicted += victims.size();
		s->stats.evicted_bytes += r.freed_bytes;
	}
	for (const Victim &v : victims) {
		uint32_t err = 0;
		if (s->delete_entry_file(v.kind, v.key, err) != CacheStatus::Ok) {
			++r.delete_failures;
		}
	}
	std::lock_guard lk(s->mu);
	for (const Victim &v : victims) {
		s->busy.erase(v.key);
	}
	s->cv.notify_all();
	r.status = CacheStatus::Ok;
	return r;
}

EvictResult Cache::enforce_budget() {
	uint64_t budget = 0;
	uint64_t total = 0;
	uint32_t permille = 900;
	if (state_) {
		std::lock_guard lk(state_->mu);
		budget = state_->options.budget_bytes;
		total = state_->total;
		permille = state_->options.low_watermark_permille > 1000 ? 1000 : state_->options.low_watermark_permille;
	}
	if (!state_ || budget == 0 || total <= budget) {
		EvictResult r;
		r.status = state_ ? CacheStatus::Ok : CacheStatus::Closed;
		return r;
	}
	return evict_to(budget / 1000 * permille + budget % 1000 * permille / 1000);
}

CacheStatus Cache::flush() {
	if (!state_) {
		return CacheStatus::Closed;
	}
	State &s = *state_;
	std::lock_guard lk(s.mu);
	if (!s.open) {
		return CacheStatus::Closed;
	}
	s.flush_touches_locked();
	uint32_t err = 0;
	return s.journal.valid() && cd::flush(s.journal, err) ? CacheStatus::Ok : CacheStatus::Io;
}

CacheStatus Cache::compact_journal() {
	if (!state_) {
		return CacheStatus::Closed;
	}
	State &s = *state_;
	std::lock_guard lk(s.mu);
	if (!s.open) {
		return CacheStatus::Closed;
	}
	uint32_t err = 0;
	return s.write_snapshot_locked(err);
}

CacheStats Cache::stats() const {
	if (!state_) {
		return {};
	}
	std::lock_guard lk(state_->mu);
	CacheStats out = state_->stats;
	out.entries = state_->index.size();
	out.bytes = state_->total;
	out.pinned = 0;
	for (const auto &[key, slot] : state_->index) {
		out.pinned += slot.pinned ? 1 : 0;
	}
	return out;
}

fs::path Cache::root() const {
	if (!state_) {
		return {};
	}
	std::lock_guard lk(state_->mu);
	return state_->root_final;
}

} // namespace bf3::core
