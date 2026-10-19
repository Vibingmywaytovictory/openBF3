// Scope reader (sound_scope.h; docs/design/m2b.md §6.2, docs/design/m2b-au.md). Fields by the data's own descriptor
// names; every instance examined is charged before it is read.

#include "bf3/core/audio/sound_scope.h"

#include <algorithm>
#include <cmath>

namespace bf3::core::audio {

namespace {

using gameplay::WorkBudget;

Error scope_error(const std::string &p_what) {
	return make_error("sound scopes: " + p_what);
}

Result<uint16_t> read_count(const EbxStruct &p_s, std::string_view p_field, const std::string &p_where) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	if (!v) {
		return uint16_t(0);
	}
	const std::optional<int64_t> n = v->as_i64();
	if (!n || *n < 0 || *n > int64_t(kMaxScopeCount)) {
		return scope_error(p_where + ": " + std::string(p_field) + " out of [0, " + std::to_string(kMaxScopeCount) + "]");
	}
	return uint16_t(*n);
}

Result<float> read_positive(const EbxStruct &p_s, std::string_view p_field, const std::string &p_where) {
	const std::optional<EbxValue> v = p_s.get(p_field);
	if (!v) {
		return 0.0f;
	}
	const float f = v->as_f32().value_or(float(v->as_f64().value_or(-1.0)));
	if (!std::isfinite(f) || f < 0.0f || f > 1.0e4f) {
		return scope_error(p_where + ": " + std::string(p_field) + " out of [0, 10000]");
	}
	return f;
}

} // namespace

uint16_t ScopeTable::scope_of(const Guid &p_patch) const {
	const auto it = std::lower_bound(patch_scopes.begin(), patch_scopes.end(), p_patch, [](const std::pair<Guid, uint16_t> &e, const Guid &g) { return e.first < g; });
	return it != patch_scopes.end() && it->first == p_patch ? it->second : kNoScope;
}

uint16_t ScopeTable::scope_index(const Guid &p_scope) const {
	for (size_t i = 0; i < scopes.size(); ++i) {
		if (scopes[i].guid == p_scope) {
			return uint16_t(i);
		}
	}
	return kNoScope;
}

Result<ScopeTable> read_scopes(const EbxResolver &p_db, gameplay::WorkBudget *p_budget) {
	WorkBudget local;
	WorkBudget &budget = p_budget != nullptr ? *p_budget : local;
	const EbxPartition *part = p_db.partition_by_name(kAudioSystemPartition);
	if (part == nullptr) {
		return scope_error(std::string(kAudioSystemPartition) + " is not in the database");
	}
	const EbxInstance primary = part->primary();
	BF3_TRY(budget.charge_type(primary.data()));
	if (!primary.is_a("AudioSystemAsset")) {
		return scope_error(std::string(kAudioSystemPartition) + "'s primary instance is " + std::string(primary.type_name()));
	}
	const std::optional<EbxValue> list = primary.get("Scopes");
	if (!list || !list->as_array()) {
		return scope_error("AudioSystemAsset has no Scopes array");
	}
	if (list->as_array()->size() > kMaxScopes) {
		return scope_error(std::to_string(list->as_array()->size()) + " scopes (at most " + std::to_string(kMaxScopes) + ")");
	}
	ScopeTable table;
	for (EbxValue e : *list->as_array()) {
		BF3_TRY(budget.charge(1));
		const std::optional<EbxRef> ref = e.as_ref();
		if (!ref || ref->is_null()) {
			continue;
		}
		const std::optional<EbxInstance> si = p_db.resolve(*part, *ref);
		if (!si) {
			return scope_error("a Scopes entry does not resolve");
		}
		BF3_TRY(budget.charge_type(si->data()));
		if (!si->is_a("SoundScopeData")) {
			return scope_error("a Scopes entry is a " + std::string(si->type_name()));
		}
		SoundScope s;
		s.guid = si->guid();
		if (const std::optional<EbxValue> n = si->get("Name")) {
			const std::string_view name = n->as_str().value_or("");
			BF3_TRY(budget.charge_bytes(name.size()));
			s.name = std::string(name.substr(0, 256));
		}
		const std::string where = s.name.empty() ? s.guid.to_string() : s.name;
		const std::optional<EbxValue> strat = si->get("DefaultStrategy");
		const std::optional<EbxRef> sref = strat ? strat->as_ref() : std::nullopt;
		if (sref && !sref->is_null()) {
			const std::optional<EbxInstance> st = p_db.resolve(si->partition(), *sref);
			if (!st) {
				return scope_error(where + ": DefaultStrategy does not resolve");
			}
			BF3_TRY(budget.charge_type(st->data()));
			if (const std::optional<EbxValue> n = st->get("Name")) {
				const std::string_view name = n->as_str().value_or("");
				BF3_TRY(budget.charge_bytes(name.size()));
				s.strategy = std::string(name.substr(0, 256));
			}
			if (st->is_a("ClosestInstancesScopeStrategyData")) {
				{
					auto r_ = read_count(st->data(), "Count", where);
					if (!r_) {
						return r_.error();
					}
					s.closest = *r_;
				}
			} else if (st->is_a("MaxInstancesScopeStrategyData")) {
				uint16_t count = 0;
				{
					auto r_ = read_count(st->data(), "Count", where);
					if (!r_) {
						return r_.error();
					}
					count = *r_;
				}
				const std::optional<EbxValue> keep = st->get("KeepOldest");
				if (keep && keep->as_bool().value_or(false)) {
					s.max_instances = count;
				} else {
					s.latest = count;
				}
			} else if (st->is_a("DuplicateSpawnScopeStrategyData")) {
				{
					auto r_ = read_count(st->data(), "ClosestCount", where);
					if (!r_) {
						return r_.error();
					}
					s.closest = *r_;
				}
				{
					auto r_ = read_positive(st->data(), "Distance", where);
					if (!r_) {
						return r_.error();
					}
					s.duplicate_radius_m = *r_;
				}
				{
					auto r_ = read_positive(st->data(), "Time", where);
					if (!r_) {
						return r_.error();
					}
					s.duplicate_time_s = *r_;
				}
				const std::optional<EbxValue> g = st->get("GroupTypes");
				s.group_types = g && g->as_bool().value_or(false);
			} else {
				return scope_error(where + ": unknown strategy type " + std::string(st->type_name()));
			}
		}
		table.scopes.push_back(std::move(s));
	}
	BF3_TRY(budget.status());
	return table;
}

Status map_patch_scopes(const EbxResolver &p_db, std::span<const Guid> p_patches, ScopeTable &r_table, gameplay::WorkBudget *p_budget, size_t *r_skipped) {
	WorkBudget local;
	WorkBudget &budget = p_budget != nullptr ? *p_budget : local;
	size_t skipped = 0;
	for (const Guid &g : p_patches) {
		BF3_TRY(budget.charge(1));
		const EbxPartition *part = p_db.partition(g);
		if (part == nullptr) {
			++skipped;
			continue;
		}
		const EbxInstance primary = part->primary();
		BF3_TRY(budget.charge_type(primary.data()));
		if (!primary.is_a("SoundPatchAsset") && !primary.is_a("SoundMasterPatchAsset")) {
			++skipped;
			continue;
		}
		const std::optional<EbxValue> v = primary.get("Scope");
		const std::optional<EbxRef> ref = v ? v->as_ref() : std::nullopt;
		if (!ref || ref->is_null()) {
			continue; // no scope (811 of 1,295 patches)
		}
		const Guid scope_guid = ref->kind == EbxRef::Kind::External ? ref->instance : Guid{};
		uint16_t index = kNoScope;
		if (ref->kind == EbxRef::Kind::External) {
			index = r_table.scope_index(scope_guid);
		} else if (std::optional<EbxInstance> si = part->resolve(*ref)) {
			index = r_table.scope_index(si->guid());
		}
		if (index == kNoScope) {
			++skipped; // names a scope the table does not hold
			continue;
		}
		r_table.patch_scopes.emplace_back(part->guid(), index);
	}
	std::sort(r_table.patch_scopes.begin(), r_table.patch_scopes.end());
	r_table.patch_scopes.erase(std::unique(r_table.patch_scopes.begin(), r_table.patch_scopes.end(),
									   [](const std::pair<Guid, uint16_t> &a, const std::pair<Guid, uint16_t> &b) { return a.first == b.first; }),
			r_table.patch_scopes.end());
	if (r_skipped != nullptr) {
		*r_skipped = skipped;
	}
	return budget.status();
}

} // namespace bf3::core::audio
