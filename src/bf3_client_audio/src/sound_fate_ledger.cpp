#include "bf3/client_audio/sound_fate_ledger.h"

#include "bf3/audio/voice_cull.h"
#include "bf3/core/util/format.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <cstring>

namespace bf3::audio {

namespace {

size_t table_size(size_t p_capacity) {
	size_t n = 16;
	while (n < p_capacity * 2) {
		n <<= 1;
	}
	return n;
}

}

void SoundCategoryTable::add_weapons(const WeaponAudioTable &p_table) {
	for (const WeaponAudioDef &d : p_table.defs) {
		const SoundPatchClass c = d.explosive ? SoundPatchClass::Explosive : SoundPatchClass::Weapon;
		if (d.program) {
			add(d.program->name, c);
		}
		if (d.explosion) {
			add(d.explosion->name, SoundPatchClass::Explosive);
		}
	}
}

void SoundCategoryTable::add(std::string_view p_patch, SoundPatchClass p_class) {
	if (p_patch.empty()) {
		return;
	}
	for (auto &[name, c] : patches_) {
		if (name == p_patch) {
			c = c == SoundPatchClass::Explosive ? c : p_class;
			return;
		}
	}
	patches_.emplace_back(std::string(p_patch), p_class);
	std::sort(patches_.begin(), patches_.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
}

SoundPatchClass SoundCategoryTable::patch_class(std::string_view p_patch) const {
	const auto it = std::lower_bound(patches_.begin(), patches_.end(), p_patch,
			[](const std::pair<std::string, SoundPatchClass> &a, std::string_view b) { return a.first < b; });
	return it != patches_.end() && it->first == p_patch ? it->second : SoundPatchClass::Other;
}

SoundCategory SoundCategoryTable::category(
		std::string_view p_patch, std::string_view p_scope, float p_distance_m) const {
	switch (patch_class(p_patch)) {
		case SoundPatchClass::Explosive:
			return SoundCategory::Explosion;
		case SoundPatchClass::Weapon:
			return p_distance_m < kOwnSoundMetres ? SoundCategory::OwnGun : SoundCategory::BotGun;
		case SoundPatchClass::Other:
			break;
	}
	if (core::icontains_ascii(p_patch, "bullet_impacts")) {
		return core::icontains_ascii(p_patch, "soldier") || core::icontains_ascii(p_scope, "soldier")
				? SoundCategory::ImpactSoldier
				: SoundCategory::ImpactSurface;
	}
	return SoundCategory::Other;
}

uint64_t SoundFateTotals::data_drops() const {
	uint64_t n = 0;
	for (const SoundFateCounts &c : categories) {
		n += c.data_drops;
	}
	return n;
}

uint64_t SoundFateTotals::other_drops() const {
	uint64_t n = 0;
	for (const SoundFateCounts &c : categories) {
		n += c.other_drops;
	}
	return n;
}

uint64_t SoundFateTotals::requested() const {
	uint64_t n = 0;
	for (const SoundFateCounts &c : categories) {
		n += c.requested;
	}
	return n;
}

SoundFateLedger::SoundFateLedger(size_t p_capacity) : table_(table_size(std::max<size_t>(p_capacity, 1))) {}

void SoundFateLedger::set_categories(const SoundCategoryTable *p_table) {
	categories_ = p_table;
}

SoundFateLedger::Entry *SoundFateLedger::find(InstanceHandle p_handle) {
	if (p_handle == kNoInstance) {
		return nullptr;
	}
	const size_t mask = table_.size() - 1;
	for (size_t i = size_t(p_handle) & mask, probes = 0; probes < table_.size(); i = (i + 1) & mask, ++probes) {
		if (table_[i].handle == p_handle) {
			return &table_[i];
		}
		if (table_[i].handle == kNoInstance) {
			return nullptr;
		}
	}
	return nullptr;
}

SoundFateLedger::Entry *SoundFateLedger::insert(InstanceHandle p_handle) {
	if (p_handle == kNoInstance || live_ * 2 >= table_.size()) {
		++overflows_;
		return nullptr;
	}
	const size_t mask = table_.size() - 1;
	for (size_t i = size_t(p_handle) & mask;; i = (i + 1) & mask) {
		if (table_[i].handle == kNoInstance || table_[i].handle == p_handle) {
			live_ += table_[i].handle == kNoInstance ? 1u : 0u;
			table_[i] = Entry{};
			table_[i].handle = p_handle;
			return &table_[i];
		}
	}
}

void SoundFateLedger::erase(Entry &r_entry) {
	const size_t mask = table_.size() - 1;
	size_t hole = size_t(&r_entry - table_.data());
	table_[hole] = Entry{};
	--live_;
	for (size_t i = (hole + 1) & mask; table_[i].handle != kNoInstance; i = (i + 1) & mask) {
		const size_t home = size_t(table_[i].handle) & mask;
		const bool between = hole <= i ? (home > hole && home <= i) : (home > hole || home <= i);
		if (!between) {
			table_[hole] = table_[i];
			table_[i] = Entry{};
			hole = i;
		}
	}
}

void SoundFateLedger::drop(SoundCategory p_category, SoundDropReason p_reason, uint64_t p_count) {
	SoundFateCounts &c = totals_.categories[size_t(p_category)];
	(sound_drop_by_data(p_reason) ? c.data_drops : c.other_drops) += 1;
	c.reasons[size_t(p_reason)] += p_count;
}

void SoundFateLedger::resolve_parks(const Entry &p_entry) {
	if (p_entry.fated || p_entry.parks == 0) {
		return;
	}
	drop(p_entry.category,
			p_entry.park == uint8_t(CullVerdict::Cap) ? SoundDropReason::ParkedAtVoiceCap
													  : SoundDropReason::ParkedByData);
}

void SoundFateLedger::note(const MixerCutEvent &p_event, std::string_view p_scope) {
	if (p_event.kind == MixerCutKind::Request) {
		const std::string_view program(p_event.program, strnlen(p_event.program, sizeof(p_event.program)));
		const SoundCategory category = categories_ != nullptr
				? categories_->category(program, p_scope, p_event.distance_m)
				: SoundCategory::Other;
		if (Entry *e = insert(p_event.handle)) {
			e->category = category;
			e->request_frame = p_event.frame;
			++totals_.categories[size_t(category)].requested;
		}
		return;
	}
	if (p_event.kind == MixerCutKind::Join) {
		if (Entry *next = find(InstanceHandle(p_event.count)); next != nullptr && !next->started) {
			next->started = true;
			++totals_.categories[size_t(next->category)].started;
		}
	}
	Entry *e = find(p_event.handle);
	if (e == nullptr) {
		switch (p_event.kind) {
			case MixerCutKind::Start:
			case MixerCutKind::Resume:
			case MixerCutKind::Finish:
			case MixerCutKind::Cull:
			case MixerCutKind::Park:
			case MixerCutKind::Refused:
			case MixerCutKind::Join:
				untracked_ += p_event.handle != kNoInstance ? 1u : 0u;
				break;
			default:
				break;
		}
		return;
	}
	SoundFateCounts &c = totals_.categories[size_t(e->category)];
	switch (p_event.kind) {
		case MixerCutKind::Start:
		case MixerCutKind::Resume:
			if (!e->started) {
				e->started = true;
				++c.started;
			}
			break;
		case MixerCutKind::Finish:
			++c.finished;
			resolve_parks(*e);
			erase(*e);
			break;
		case MixerCutKind::Join:
			++c.joined;
			resolve_parks(*e);
			erase(*e);
			break;
		case MixerCutKind::Park:
			e->park = p_event.reason;
			++e->parks;
			break;
		case MixerCutKind::Cull:
			if (p_event.reason == uint8_t(CullVerdict::Pending)) {
				drop(e->category,
						e->park == uint8_t(CullVerdict::Cap) ? SoundDropReason::StoppedParkedAtVoiceCap
															 : SoundDropReason::StoppedParkedByData);
			} else {
				drop(e->category,
						p_event.reason == uint8_t(CullVerdict::Precull)			? SoundDropReason::BelowWindow
								: p_event.reason == uint8_t(CullVerdict::Scope) ? SoundDropReason::Scope
																				: SoundDropReason::VoiceCap);
			}
			e->fated = true;
			erase(*e);
			break;
		case MixerCutKind::Refused:
			drop(e->category, SoundDropReason::Refused);
			e->fated = true;
			erase(*e);
			break;
		case MixerCutKind::Kill:
			resolve_parks(*e);
			drop(e->category, SoundDropReason::Killed);
			erase(*e);
			break;
		case MixerCutKind::Coalesce:
			if (e->coalesced) {
				c.reasons[size_t(SoundDropReason::CoalescedEvents)] += p_event.count;
			} else {
				drop(e->category, SoundDropReason::CoalescedEvents, p_event.count);
				e->coalesced = true;
			}
			break;
		default:
			break;
	}
}

SoundFateTotals SoundFateLedger::totals(uint64_t p_now_frame, uint64_t p_grace_frames) const {
	SoundFateTotals t = totals_;
	for (const Entry &e : table_) {
		if (e.handle == kNoInstance || e.fated) {
			continue;
		}
		SoundFateCounts &c = t.categories[size_t(e.category)];
		if (e.parks > 0) {
			const SoundDropReason r = e.park == uint8_t(CullVerdict::Cap) ? SoundDropReason::ParkedAtVoiceCap
																		  : SoundDropReason::ParkedByData;
			(sound_drop_by_data(r) ? c.data_drops : c.other_drops) += 1;
			++c.reasons[size_t(r)];
		} else if (!e.started && e.request_frame + p_grace_frames <= p_now_frame) {
			++c.other_drops;
			++c.reasons[size_t(SoundDropReason::NeverStarted)];
		}
	}
	return t;
}

std::string SoundFateLedger::format(const SoundFateTotals &p_totals) const {
	std::string out =
			core::fmt("sounds dropped: %llu outside the data's rules, %llu by the data's rules, of %llu requested",
					(unsigned long long)p_totals.other_drops(), (unsigned long long)p_totals.data_drops(),
					(unsigned long long)p_totals.requested());
	for (size_t k = 0; k < p_totals.categories.size(); ++k) {
		const SoundFateCounts &c = p_totals.categories[k];
		if (c.data_drops + c.other_drops == 0) {
			continue;
		}
		out += core::fmt("; %s %llu/%llu", std::string(kSoundCategoryNames[k]).c_str(),
				(unsigned long long)c.other_drops, (unsigned long long)c.data_drops);
		for (size_t r = 0; r < c.reasons.size(); ++r) {
			if (c.reasons[r] > 0) {
				out += core::fmt(
						", %s %llu", std::string(kSoundDropReasonNames[r]).c_str(), (unsigned long long)c.reasons[r]);
			}
		}
	}
	if (untracked_ + overflows_ > 0) {
		out += core::fmt("; %llu events of untracked sounds, %llu requests over the ledger's capacity",
				(unsigned long long)untracked_, (unsigned long long)overflows_);
	}
	return out;
}

}
