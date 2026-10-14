// Partition databases. EbxDb is ported from IW4L ebx_bf3 (db.rs), Apache-2.0; EbxLibrary follows
// the ebx-dump probe's load_db (research/probes/tools/ebx-dump/src/main.rs), parallelised.
#include "bf3/core/ebx_db.h"

#include "bf3/core/thread_pool.h"

#include <atomic>
#include <chrono>

namespace bf3::core {

// --- EbxResolver ---------------------------------------------------------------------------------

std::optional<EbxInstance> EbxResolver::instance(const Guid &p_partition, const Guid &p_instance) const {
	const EbxPartition *p = partition(p_partition);
	return p ? p->instance_by_guid(p_instance) : std::nullopt;
}

std::optional<EbxInstance> EbxResolver::resolve(const EbxPartition &p_from, const EbxRef &p_ref) const {
	switch (p_ref.kind) {
		case EbxRef::Kind::Internal:
			return p_from.instance(p_ref.index);
		case EbxRef::Kind::External:
			return instance(p_ref.partition, p_ref.instance);
		default:
			return std::nullopt;
	}
}

std::optional<EbxInstance> EbxResolver::follow(const EbxStruct &p_object, std::string_view p_field) const {
	const std::optional<EbxValue> v = p_object.get(p_field);
	if (!v) {
		return std::nullopt;
	}
	const std::optional<EbxRef> r = v->as_ref();
	return r ? resolve(p_object.partition(), *r) : std::nullopt;
}

std::vector<EbxInstance> EbxResolver::follow_all(const EbxStruct &p_object, std::string_view p_field) const {
	std::vector<EbxInstance> out;
	const std::optional<EbxValue> v = p_object.get(p_field);
	const OptionalEbxArray array = v ? v->as_array() : OptionalEbxArray();
	if (!array) {
		return out;
	}
	for (const EbxValue element : *array) {
		const std::optional<EbxRef> r = element.as_ref();
		if (r) {
			if (const std::optional<EbxInstance> i = resolve(p_object.partition(), *r)) {
				out.push_back(*i);
			}
		}
	}
	return out;
}

// --- EbxDb ---------------------------------------------------------------------------------------

bool EbxDb::insert(std::string_view p_name, std::unique_ptr<EbxPartition> p_partition) {
	std::string key = to_lower_ascii(p_name);
	if (by_name_.contains(key) || by_guid_.contains(p_partition->guid())) {
		return false;
	}
	const uint32_t slot = static_cast<uint32_t>(partitions_.size());
	by_guid_.emplace(p_partition->guid(), slot);
	by_name_.emplace(key, slot);
	partitions_.emplace_back(std::move(key), std::move(p_partition));
	return true;
}

Result<bool> EbxDb::parse_insert(std::string_view p_name, std::vector<uint8_t> p_bytes) {
	Result<EbxPartition> p = EbxPartition::parse(std::move(p_bytes));
	if (!p) {
		return p.error();
	}
	return insert(p_name, std::make_unique<EbxPartition>(std::move(*p)));
}

const EbxPartition *EbxDb::partition(const Guid &p_guid) const {
	const auto it = by_guid_.find(p_guid);
	return it == by_guid_.end() ? nullptr : partitions_[it->second].second.get();
}

const EbxPartition *EbxDb::partition_by_name(std::string_view p_name) const {
	const auto it = by_name_.find(to_lower_ascii(p_name));
	return it == by_name_.end() ? nullptr : partitions_[it->second].second.get();
}

std::optional<std::string_view> EbxDb::name_of(const Guid &p_guid) const {
	const auto it = by_guid_.find(p_guid);
	if (it == by_guid_.end()) {
		return std::nullopt;
	}
	return std::string_view(partitions_[it->second].first);
}

// --- superbundle specs ---------------------------------------------------------------------------

std::vector<std::string> expand_superbundles(const Install &p_install, std::string_view p_spec) {
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= p_spec.size()) {
		const size_t comma = std::min(p_spec.find(',', pos), p_spec.size());
		const std::string_view part = p_spec.substr(pos, comma - pos);
		if (part == "@all-mp") {
			for (const std::string &name : p_install.superbundle_names()) {
				const std::string l = to_lower_ascii(name);
				const bool level = l.rfind("win32/levels/mp_", 0) == 0 || l.rfind("win32/levels/xp", 0) == 0;
				const bool global = l == "win32/globals" || l == "win32/weapons" || l == "win32/ui" || l == "win32/default_settings_win32";
				if (level || global) {
					out.push_back(name);
				}
			}
		} else {
			out.emplace_back(part);
		}
		pos = comma + 1;
	}
	return out;
}

// --- EbxLibrary ----------------------------------------------------------------------------------

EbxLibrary::~EbxLibrary() = default;

Result<std::unique_ptr<EbxLibrary>> EbxLibrary::open(std::shared_ptr<const Install> p_install, const std::vector<std::string> &p_superbundles,
		unsigned p_threads) {
	std::unique_ptr<EbxLibrary> lib(new EbxLibrary());
	lib->install_ = std::move(p_install);
	lib->superbundles_ = p_superbundles;
	lib->threads_ = p_threads;
	const Install &install = *lib->install_;
	std::vector<Result<LevelMount>> mounts = parallel_map<Result<LevelMount>>(
			p_superbundles.size(),
			[&](size_t i) -> Result<LevelMount> {
				Result<LevelMount> m = install.mount_superbundle(p_superbundles[i]);
				if (!m) {
					return error_at("mount " + p_superbundles[i], m.error());
				}
				const Status loaded = m->load_all_bundles();
				if (!loaded) {
					return error_at("load bundles " + p_superbundles[i], loaded.error());
				}
				return m;
			},
			p_threads);
	lib->mounts_.reserve(mounts.size());
	for (Result<LevelMount> &m : mounts) {
		if (!m) {
			return m.error();
		}
		lib->mounts_.push_back(std::move(*m));
	}
	// Catalog: the first superbundle that holds a name wins.
	for (uint32_t s = 0; s < lib->mounts_.size(); ++s) {
		for (std::string &name : lib->mounts_[s].ebx_names()) {
			std::string key = to_lower_ascii(name);
			const auto [it, fresh] = lib->slot_by_key_.try_emplace(key, static_cast<uint32_t>(lib->slots_.size()));
			if (!fresh) {
				continue;
			}
			Slot &slot = lib->slots_.emplace_back();
			slot.key = std::move(key);
			slot.name = std::move(name);
			slot.sb = s;
		}
	}
	return lib;
}

Result<std::unique_ptr<EbxPartition>> EbxLibrary::read_parse(uint32_t p_sb, const std::string &p_name) const {
	Result<std::vector<uint8_t>> bytes = mounts_[p_sb].read_ebx(p_name);
	if (!bytes) {
		return bytes.error();
	}
	Result<EbxPartition> p = EbxPartition::parse(std::move(*bytes));
	if (!p) {
		return error_at("ebx " + p_name, p.error());
	}
	return std::make_unique<EbxPartition>(std::move(*p));
}

const EbxPartition *EbxLibrary::ensure(const Slot &p_slot) const {
	std::call_once(p_slot.once, [&]() {
		Result<std::unique_ptr<EbxPartition>> p = read_parse(p_slot.sb, p_slot.name);
		if (p) {
			p_slot.partition = std::move(*p);
		} else {
			p_slot.error = p.error().message;
		}
	});
	return p_slot.partition.get();
}

const EbxLibrary::Slot *EbxLibrary::slot_of(std::string_view p_name) const {
	const auto it = slot_by_key_.find(to_lower_ascii(p_name));
	return it == slot_by_key_.end() ? nullptr : &slots_[it->second];
}

void EbxLibrary::build_header_guid_index() const {
	std::call_once(guid_once_, [&]() {
		std::vector<std::optional<Guid>> guids = parallel_map<std::optional<Guid>>(
				slots_.size(),
				[&](size_t i) -> std::optional<Guid> {
					const Slot &slot = slots_[i];
					Result<Guid> g = mounts_[slot.sb].ebx_partition_guid(slot.name);
					return g ? std::optional<Guid>(*g) : std::nullopt;
				},
				threads_);
		slot_by_guid_.reserve(slots_.size());
		for (uint32_t i = 0; i < guids.size(); ++i) {
			if (guids[i]) {
				slot_by_guid_.try_emplace(*guids[i], i);
			}
		}
	});
}

EbxLibrary::LoadReport EbxLibrary::load_all() {
	LoadReport report;
	const auto t0 = std::chrono::steady_clock::now();
	std::atomic<uint64_t> bytes{ 0 };
	parallel_for(
			slots_.size(), [&](size_t i) {
				if (const EbxPartition *p = ensure(slots_[i])) {
					bytes += p->byte_size();
				}
			},
			threads_);
	report.parsed = slots_.size();
	const auto t1 = std::chrono::steady_clock::now();

	// Insert in catalog order with EbxDb's rules (the probe's load_db, superbundle by superbundle).
	std::call_once(guid_once_, []() {});
	slot_by_guid_.clear();
	slot_by_guid_.reserve(slots_.size());
	order_.clear();
	order_.reserve(slots_.size());
	std::vector<uint8_t> inserted(slots_.size(), 0);
	for (uint32_t s = 0; s < mounts_.size(); ++s) {
		SuperbundleReport sb_report;
		sb_report.superbundle = superbundles_[s];
		for (const std::string &name : mounts_[s].ebx_names()) {
			const uint32_t index = slot_by_key_.find(to_lower_ascii(name))->second;
			Slot &slot = slots_[index];
			if (inserted[index]) {
				continue;
			}
			if (slot.sb != s) {
				// The first copy did not load (failed or duplicate GUID): retry from this superbundle.
				if (slot.sb > s) {
					continue; // cannot happen: the catalog holds the first superbundle
				}
				Result<std::unique_ptr<EbxPartition>> p = read_parse(s, name);
				++report.parsed;
				slot.sb = s;
				slot.name = name;
				if (p) {
					bytes += (*p)->byte_size();
					slot.partition = std::move(*p);
					slot.error.clear();
				} else {
					slot.partition.reset();
					slot.error = p.error().message;
				}
			}
			if (!slot.partition) {
				++sb_report.failed;
				++report.failed;
				if (report.errors.size() < 20) {
					report.errors.push_back(slot.name + ": " + slot.error);
				}
				slot.dropped = true;
				continue;
			}
			const auto [it, fresh] = slot_by_guid_.try_emplace(slot.partition->guid(), index);
			if (!fresh) {
				++report.duplicate_guids;
				bytes -= slot.partition->byte_size();
				slot.partition.reset();
				slot.dropped = true;
				continue;
			}
			slot.dropped = false;
			inserted[index] = 1;
			order_.push_back(index);
			++sb_report.fresh;
		}
		sb_report.total_after = order_.size();
		report.superbundles.push_back(std::move(sb_report));
	}
	// Bundle of origin for every loaded partition.
	parallel_for(
			order_.size(), [&](size_t i) {
				Slot &slot = slots_[order_[i]];
				slot.bundle = mounts_[slot.sb].ebx_bundle(slot.name).value_or(std::string());
			},
			threads_);
	bulk_ = true;
	report.partitions = order_.size();
	report.bytes = bytes.load();
	report.parse_seconds = std::chrono::duration<double>(t1 - t0).count();
	report.insert_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
	return report;
}

const EbxPartition *EbxLibrary::partition(const Guid &p_guid) const {
	if (!bulk_) {
		build_header_guid_index();
	}
	const auto it = slot_by_guid_.find(p_guid);
	if (it == slot_by_guid_.end()) {
		return nullptr;
	}
	const Slot &slot = slots_[it->second];
	return bulk_ ? slot.partition.get() : ensure(slot);
}

const EbxPartition *EbxLibrary::partition_by_name(std::string_view p_name) const {
	const Slot *slot = slot_of(p_name);
	if (slot == nullptr) {
		return nullptr;
	}
	if (bulk_) {
		return slot->dropped ? nullptr : slot->partition.get();
	}
	return ensure(*slot);
}

std::optional<std::string_view> EbxLibrary::name_of(const Guid &p_guid) const {
	if (!bulk_) {
		build_header_guid_index();
	}
	const auto it = slot_by_guid_.find(p_guid);
	if (it == slot_by_guid_.end()) {
		return std::nullopt;
	}
	return std::string_view(slots_[it->second].key);
}

std::optional<std::string> EbxLibrary::error_of(std::string_view p_name) const {
	const Slot *slot = slot_of(p_name);
	if (slot == nullptr) {
		return std::nullopt;
	}
	if (!bulk_) {
		ensure(*slot);
	}
	return slot->error.empty() ? std::nullopt : std::optional<std::string>(slot->error);
}

std::optional<EbxLibrary::Origin> EbxLibrary::origin(std::string_view p_name) const {
	const Slot *slot = slot_of(p_name);
	if (slot == nullptr || (bulk_ && slot->dropped)) {
		return std::nullopt;
	}
	if (!bulk_) {
		return Origin{ superbundles_[slot->sb], mounts_[slot->sb].ebx_bundle(slot->name).value_or(std::string()) };
	}
	return Origin{ superbundles_[slot->sb], slot->bundle };
}

std::vector<EbxLibrary::Entry> EbxLibrary::entries() const {
	std::vector<Entry> out;
	if (bulk_) {
		out.reserve(order_.size());
		for (uint32_t index : order_) {
			out.push_back(Entry{ slots_[index].key, slots_[index].partition.get() });
		}
		return out;
	}
	out.reserve(slots_.size());
	for (const Slot &slot : slots_) {
		if (const EbxPartition *p = ensure(slot)) {
			out.push_back(Entry{ slot.key, p });
		}
	}
	return out;
}

} // namespace bf3::core
