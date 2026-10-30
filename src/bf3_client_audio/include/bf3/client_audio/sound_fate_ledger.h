#pragma once

#include "bf3/audio/mixer_core.h"
#include "bf3/audio/weapon_audio.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::audio {

enum class SoundCategory : uint8_t { OwnGun, BotGun, ImpactSurface, ImpactSoldier, Explosion, Other, Count };
inline constexpr std::array<std::string_view, size_t(SoundCategory::Count)> kSoundCategoryNames{ "own gun", "bot gun",
	"surface impact", "soldier impact", "explosion", "other" };
inline constexpr float kOwnSoundMetres = 1.0f;
inline constexpr uint64_t kSoundFateForgetFrames = uint64_t(60) * kMixRate;

enum class SoundPatchClass : uint8_t { Weapon, Explosive, Other };

class SoundCategoryTable {
public:
	void add_weapons(const WeaponAudioTable &p_table);
	void add(std::string_view p_patch, SoundPatchClass p_class);
	SoundPatchClass patch_class(std::string_view p_patch) const;
	SoundCategory category(std::string_view p_patch, std::string_view p_scope, float p_distance_m) const;

private:
	std::vector<std::pair<std::string, SoundPatchClass>> patches_;
};

enum class SoundDropReason : uint8_t {
	BelowWindow,
	Scope,
	VoiceCap,
	ParkedByData,
	ParkedAtVoiceCap,
	StoppedParkedByData,
	StoppedParkedAtVoiceCap,
	Refused,
	Killed,
	CoalescedEvents,
	NeverStarted,
	Count
};
inline constexpr std::array<std::string_view, size_t(SoundDropReason::Count)> kSoundDropReasonNames{
	"below the HDR window", "scope", "voice cap", "parked mid-sound by the data", "parked mid-sound at the voice cap",
	"stopped while parked by the data", "stopped while parked at the voice cap", "refused: no mixer slot",
	"killed by the pool or the router", "events dropped at a resume", "requested, never started or dropped"
};

constexpr bool sound_drop_by_data(SoundDropReason p_reason) {
	return p_reason == SoundDropReason::BelowWindow || p_reason == SoundDropReason::Scope ||
			p_reason == SoundDropReason::ParkedByData || p_reason == SoundDropReason::StoppedParkedByData;
}

struct SoundFateCounts {
	uint64_t requested = 0, started = 0, finished = 0, joined = 0;
	uint64_t data_drops = 0, other_drops = 0;
	std::array<uint64_t, size_t(SoundDropReason::Count)> reasons{};
};

struct SoundFateTotals {
	std::array<SoundFateCounts, size_t(SoundCategory::Count)> categories{};
	uint64_t data_drops() const;
	uint64_t other_drops() const;
	uint64_t requested() const;
};

class SoundFateLedger {
public:
	explicit SoundFateLedger(size_t p_capacity = 8192);

	void set_categories(const SoundCategoryTable *p_table);
	void note(const MixerCutEvent &p_event, std::string_view p_scope);

	SoundFateTotals totals(uint64_t p_now_frame = UINT64_MAX, uint64_t p_grace_frames = 0) const;
	uint64_t untracked() const { return untracked_; }
	uint64_t overflows() const { return overflows_; }
	uint64_t expired() const { return expired_; }
	std::string format(const SoundFateTotals &p_totals) const;

private:
	struct Entry {
		InstanceHandle handle = kNoInstance;
		SoundCategory category = SoundCategory::Other;
		bool started = false;
		bool fated = false;
		bool coalesced = false;
		uint64_t request_frame = 0;
		uint8_t park = 0;
		uint32_t parks = 0;
	};
	Entry *find(InstanceHandle p_handle);
	Entry *insert(InstanceHandle p_handle, uint64_t p_frame);
	void expire(uint64_t p_now_frame);
	void erase(Entry &r_entry);
	void drop(SoundCategory p_category, SoundDropReason p_reason, uint64_t p_count = 1);
	void resolve_parks(const Entry &p_entry);

	std::vector<Entry> table_;
	size_t live_ = 0;
	const SoundCategoryTable *categories_ = nullptr;
	SoundFateTotals totals_;
	uint64_t untracked_ = 0, overflows_ = 0, expired_ = 0;
};

}
