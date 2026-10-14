#pragma once
// Partition databases: lookup by partition GUID and by (case-insensitive) name, and reference
// resolution across partitions.
//
// EbxDb is ported from IW4L ebx_bf3 (db.rs), Apache-2.0: owned partitions, the first one inserted
// for a GUID or a name wins (a later duplicate is dropped).
//
// EbxLibrary mounts superbundles of the install and serves their partitions:
//   * catalog: every ebx name of the mounted superbundles, the first superbundle (in the order
//     given) that holds a name wins, names in each mount's index order;
//   * lazy: partition_by_name / partition parse a partition on first use (thread-safe);
//     GUID lookups before load_all() use the 80-byte headers of the catalogued partitions;
//   * bulk: load_all() parses every catalogued partition on all cores, then inserts them in
//     catalog order with EbxDb's rules, which is exactly the ebx-dump probe's load_db (a name
//     whose partition fails to parse or duplicates an earlier GUID is retried from the next
//     superbundle that holds it).
// "@all-mp" (see expand_superbundles) = every MP/XP level superbundle plus Globals, Weapons, UI and
// default_settings_Win32, in layout order.

#include "bf3/core/ebx.h"
#include "bf3/core/ids.h"
#include "bf3/core/install.h"
#include "bf3/core/result.h"
#include "bf3/core/superbundle_mount.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bf3::core {

// Lookup and reference resolution shared by EbxDb and EbxLibrary.
class EbxResolver {
public:
	virtual ~EbxResolver() = default;

	virtual const EbxPartition *partition(const Guid &p_guid) const = 0;
	virtual const EbxPartition *partition_by_name(std::string_view p_name) const = 0;
	// The lowercase name a partition is known under.
	virtual std::optional<std::string_view> name_of(const Guid &p_guid) const = 0;

	std::optional<EbxInstance> instance(const Guid &p_partition, const Guid &p_instance) const;
	// Follows a reference as read from a field of p_from.
	std::optional<EbxInstance> resolve(const EbxPartition &p_from, const EbxRef &p_ref) const;
	std::optional<EbxInstance> follow(const EbxStruct &p_object, std::string_view p_field) const;
	// The instances an array-of-references field points at, skipping null and unresolved entries.
	std::vector<EbxInstance> follow_all(const EbxStruct &p_object, std::string_view p_field) const;
};

class EbxDb final : public EbxResolver {
public:
	EbxDb() = default;
	EbxDb(EbxDb &&) noexcept = default;
	EbxDb &operator=(EbxDb &&) noexcept = default;

	// Keeps the first partition seen for a GUID or name; a later one is dropped and false returned.
	bool insert(std::string_view p_name, std::unique_ptr<EbxPartition> p_partition);
	Result<bool> parse_insert(std::string_view p_name, std::vector<uint8_t> p_bytes);

	size_t size() const { return partitions_.size(); }
	bool empty() const { return partitions_.empty(); }
	// Insertion order: (lowercase name, partition).
	const std::string &name_at(size_t p_index) const { return partitions_[p_index].first; }
	const EbxPartition &partition_at(size_t p_index) const { return *partitions_[p_index].second; }

	const EbxPartition *partition(const Guid &p_guid) const override;
	const EbxPartition *partition_by_name(std::string_view p_name) const override;
	std::optional<std::string_view> name_of(const Guid &p_guid) const override;

private:
	std::vector<std::pair<std::string, std::unique_ptr<EbxPartition>>> partitions_;
	std::unordered_map<Guid, uint32_t, GuidHash> by_guid_;
	std::unordered_map<std::string, uint32_t> by_name_;
};

// The superbundle list of a spec: comma-separated names, "@all-mp" expands as described above.
std::vector<std::string> expand_superbundles(const Install &p_install, std::string_view p_spec);

class EbxLibrary final : public EbxResolver {
public:
	struct SuperbundleReport {
		std::string superbundle;
		size_t fresh = 0; // partitions this superbundle added
		size_t failed = 0; // read or parse failures among names not added earlier
		size_t total_after = 0; // partitions loaded after this superbundle
	};
	struct LoadReport {
		std::vector<SuperbundleReport> superbundles;
		size_t partitions = 0;
		size_t parsed = 0; // parse attempts (catalogued + retried)
		size_t failed = 0;
		size_t duplicate_guids = 0; // dropped: GUID already loaded under another name
		uint64_t bytes = 0; // decoded partition bytes kept
		std::vector<std::string> errors; // the first few failures, "<name>: <message>"
		double parse_seconds = 0;
		double insert_seconds = 0;
	};
	struct Origin {
		std::string superbundle;
		std::string bundle; // toc id
	};

	// Mounts each superbundle (in parallel) and indexes all its bundles; builds the name catalog.
	static Result<std::unique_ptr<EbxLibrary>> open(std::shared_ptr<const Install> p_install, const std::vector<std::string> &p_superbundles,
			unsigned p_threads = 0);
	~EbxLibrary() override;

	size_t superbundle_count() const { return mounts_.size(); }
	const std::string &superbundle(size_t p_index) const { return superbundles_[p_index]; }
	const LevelMount &mount(size_t p_index) const { return mounts_[p_index]; }
	// Distinct ebx names over all mounted superbundles.
	size_t catalog_size() const { return slots_.size(); }

	// Parses every catalogued partition on all cores (see the header comment). It mutates the
	// slots (retries, dropped duplicates), so it must not run concurrently with any other call,
	// lazy lookups included. Afterwards lookups follow the probe's EbxDb exactly.
	LoadReport load_all();
	bool bulk_loaded() const { return bulk_; }

	// Lazy lookups (parse on first use). Null when absent or when the partition does not parse.
	const EbxPartition *partition(const Guid &p_guid) const override;
	const EbxPartition *partition_by_name(std::string_view p_name) const override;
	std::optional<std::string_view> name_of(const Guid &p_guid) const override;
	// The parse error of a name whose partition did not parse.
	std::optional<std::string> error_of(std::string_view p_name) const;
	// Where a name's partition was read: superbundle and bundle toc id.
	std::optional<Origin> origin(std::string_view p_name) const;

	// Loaded partitions in insertion order (after load_all; before it, the catalog order with
	// partitions parsed on demand). Entries whose partition does not parse are skipped.
	struct Entry {
		std::string_view name; // lowercase
		const EbxPartition *partition;
	};
	std::vector<Entry> entries() const;

private:
	struct Slot {
		std::string key; // lowercase name
		std::string name; // as listed in the manifest
		uint32_t sb = 0; // superbundle index the partition is read from
		std::string bundle;
		mutable std::once_flag once;
		mutable std::unique_ptr<EbxPartition> partition;
		mutable std::string error;
		bool dropped = false; // bulk: not inserted (failed / duplicate GUID and no later copy)
	};

	EbxLibrary() = default;
	const EbxPartition *ensure(const Slot &p_slot) const;
	Result<std::unique_ptr<EbxPartition>> read_parse(uint32_t p_sb, const std::string &p_name) const;
	void build_header_guid_index() const;
	const Slot *slot_of(std::string_view p_name) const;

	std::shared_ptr<const Install> install_;
	std::vector<std::string> superbundles_;
	std::vector<LevelMount> mounts_;
	unsigned threads_ = 0;
	std::deque<Slot> slots_;
	std::unordered_map<std::string, uint32_t> slot_by_key_;
	std::vector<uint32_t> order_; // slot indices in insertion order (bulk)
	bool bulk_ = false;
	mutable std::once_flag guid_once_;
	mutable std::unordered_map<Guid, uint32_t, GuidHash> slot_by_guid_;
};

} // namespace bf3::core
