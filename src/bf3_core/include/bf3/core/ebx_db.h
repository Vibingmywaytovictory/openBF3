#pragma once

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

class EbxResolver {
public:
	virtual ~EbxResolver() = default;

	virtual const EbxPartition *partition(const Guid &p_guid) const = 0;
	virtual const EbxPartition *partition_by_name(std::string_view p_name) const = 0;

	virtual std::optional<std::string_view> name_of(const Guid &p_guid) const = 0;

	std::optional<EbxInstance> instance(const Guid &p_partition, const Guid &p_instance) const;

	std::optional<EbxInstance> resolve(const EbxPartition &p_from, const EbxRef &p_ref) const;
	std::optional<EbxInstance> follow(const EbxStruct &p_object, std::string_view p_field) const;

	std::vector<EbxInstance> follow_all(const EbxStruct &p_object, std::string_view p_field) const;
};

class EbxDb final : public EbxResolver {
public:
	EbxDb() = default;
	EbxDb(EbxDb &&) noexcept = default;
	EbxDb &operator=(EbxDb &&) noexcept = default;

	bool insert(std::string_view p_name, std::unique_ptr<EbxPartition> p_partition);
	Result<bool> parse_insert(std::string_view p_name, std::vector<uint8_t> p_bytes);

	size_t size() const { return partitions_.size(); }
	bool empty() const { return partitions_.empty(); }

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

std::vector<std::string> expand_superbundles(const Install &p_install, std::string_view p_spec);

class EbxLibrary final : public EbxResolver {
public:
	struct SuperbundleReport {
		std::string superbundle;
		size_t fresh = 0;
		size_t failed = 0;
		size_t total_after = 0;
	};
	struct LoadReport {
		std::vector<SuperbundleReport> superbundles;
		size_t partitions = 0;
		size_t parsed = 0;
		size_t failed = 0;
		size_t duplicate_guids = 0;
		uint64_t bytes = 0;
		std::vector<std::string> errors;
		double parse_seconds = 0;
		double insert_seconds = 0;
	};
	struct Origin {
		std::string superbundle;
		std::string bundle;
	};

	static Result<std::unique_ptr<EbxLibrary>> open(std::shared_ptr<const Install> p_install, const std::vector<std::string> &p_superbundles,
			unsigned p_threads = 0);
	~EbxLibrary() override;

	size_t superbundle_count() const { return mounts_.size(); }
	const std::string &superbundle(size_t p_index) const { return superbundles_[p_index]; }
	const LevelMount &mount(size_t p_index) const { return mounts_[p_index]; }

	size_t catalog_size() const { return slots_.size(); }

	LoadReport load_all();
	bool bulk_loaded() const { return bulk_; }

	const EbxPartition *partition(const Guid &p_guid) const override;
	const EbxPartition *partition_by_name(std::string_view p_name) const override;
	std::optional<std::string_view> name_of(const Guid &p_guid) const override;

	std::optional<std::string> error_of(std::string_view p_name) const;

	std::optional<Origin> origin(std::string_view p_name) const;

	struct Entry {
		std::string_view name;
		const EbxPartition *partition;
	};
	std::vector<Entry> entries() const;

private:
	struct Slot {
		std::string key;
		std::string name;
		uint32_t sb = 0;
		std::string bundle;
		mutable std::once_flag once;
		mutable std::unique_ptr<EbxPartition> partition;
		mutable std::string error;
		bool dropped = false;
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
	std::vector<uint32_t> order_;
	bool bulk_ = false;
	mutable std::once_flag guid_once_;
	mutable std::unordered_map<Guid, uint32_t, GuidHash> slot_by_guid_;
};

}
