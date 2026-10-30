#pragma once

#include "game_frame.h"

#include "bf3/audio/audio_config.h"
#include "bf3/audio/game_audio_check.h"
#include "bf3/audio/impact_audio.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/client_audio/area_feed.h"
#include "bf3/client_audio/game_audio_frame.h"
#include "bf3/client_audio/sound_fate_ledger.h"
#include "bf3/core/path_guard.h"
#include "bf3/core/result.h"
#include "bf3/fx/triggers.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace godot {
class Node;
}

namespace bf3::godot_side {

struct GameAudioStats {
	audio::WeaponAudioStats router;
	audio::ImpactAudioStats impacts;
	uint64_t mixer_frames = 0;
	uint64_t short_blocks = 0, late_blocks = 0, late_commands = 0, ring_overflows = 0;
	uint32_t peak_instances = 0;
	uint64_t clock_late = 0;
	uint64_t output_latency_frames = 0;
	uint32_t programs = 0, program_failures = 0;

	uint32_t one_shots_live = 0, one_shots_peak = 0;
	uint64_t one_shots_stolen = 0, one_shots_failed = 0, one_shots_forgotten = 0;
	uint64_t starved_voices = 0, feedback_drops = 0, max_block_ns = 0;
	int64_t bias_frames = 0;
	uint64_t auditions = 0;
};

class Bf3GameAudio {
public:
	Bf3GameAudio();
	~Bf3GameAudio();
	Bf3GameAudio(const Bf3GameAudio &) = delete;
	Bf3GameAudio &operator=(const Bf3GameAudio &) = delete;

	core::Status load(const GameLoadInputs &p_in, std::span<const std::string> p_impact_sounds);

	void set_load_options(const audio::GameAudioLoadOptions &p_options);
	bool loaded() const;

	bool build(godot::Node *p_parent, std::string *r_error);
	bool built() const;

	void step(const GameFrame &p_frame, std::span<const fx::ImpactSoundCue> p_impact_cues);

	void step(const GameFrame &p_frame, std::span<const fx::ImpactSoundCue> p_impact_cues, std::span<const audio::EffectSoundRequest> p_effect_sounds);

	bool audition(std::string_view p_patch, const fx::Vec3 &p_position, std::string *r_error);

	void set_check(audio::GameAudioCheck *p_check);
	void set_mix_tap(audio::MixTap *p_tap);
	void set_sum_tap(audio::MixTap *p_tap);
	void forward_cuts(bool p_on);
	void take_cuts(std::vector<audio::MixerCutEvent> &r_out);
	void drain_events();
	const audio::SoundCategoryTable &sound_categories() const;
	bool one_shot_tracked(audio::InstanceHandle p_handle) const;
	uint64_t one_shot_join_requests() const;
	std::vector<std::string> weapon_patches(bool p_explosive, uint32_t p_one_shot_event) const;
	audio::SoundFateTotals sound_fates() const;
	std::string_view scope_name(uint16_t p_scope) const;
	const core::PathGuard &write_guard() const;
	audio::MixerCore *mixer() const;
	bool weapon_bound(uint16_t p_def) const;
	std::span<const audio::KitAudioVariant> sound_variants() const;
	GameAudioStats stats() const;
	std::vector<std::string> load_report() const;
	void set_sound_areas(const sim::logic::SoundAreaNatives *p_areas);
	const audio::AreaFeedStats &area_feed() const;
	std::optional<float> mixer_value(uint32_t p_name_hash) const;
#if OPENBF3_BUILD_CHECKS
	core::Result<std::vector<std::string>> observe_area_waves(std::string_view p_patch, float p_area_type) const;
#endif
	void shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

audio::MixerProfile game_audio_profile(const Bf3GameAudio &p_audio);
std::string game_audio_profile_text(const Bf3GameAudio &p_audio);

}
