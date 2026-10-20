#pragma once

#include "bf3/audio/audio_config.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"
#include "bf3/host/presentation_events.h"
#include "bf3/sim/weapons/sim_time.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bf3::audio {

class ImpactSoundCache {
public:

	static core::Result<std::shared_ptr<const ImpactSoundCache>> build(const core::EbxResolver &p_db, std::span<const std::string> p_patches);

	static std::shared_ptr<const ImpactSoundCache> from_programs(
			std::span<const std::pair<std::string, std::shared_ptr<const core::audio::SoundProgram>>> p_programs);

	std::shared_ptr<const core::audio::SoundProgram> find(std::string_view p_patch) const;

	std::vector<std::shared_ptr<const core::audio::SoundProgram>> programs() const;
	size_t compiled() const;
	size_t failed() const;
	std::span<const std::string> errors() const;

private:
	std::unordered_map<std::string, std::shared_ptr<const core::audio::SoundProgram>> programs_;
	std::vector<std::string> errors_;
};

struct ImpactSoundRequest {
	std::string_view patch;
	Vec3 position;
	Vec3 normal{ 0.0f, 1.0f, 0.0f };
	uint64_t seed = 0;
	sim::weapons::SimTime time = 0;
	host::PresentTimeline timeline = host::PresentTimeline::Render;
};

struct EffectSoundRequest {
	std::string_view patch;
	uint64_t effect = 0;
	uint32_t component = 0;
	Vec3 position;
	bool start = true;
};

struct ImpactAudioStats {
	uint64_t impacts = 0;
	uint64_t effect_sounds = 0;
	uint64_t effect_stops = 0;
	uint64_t unknown_patches = 0;
	uint64_t refused = 0;
};
static_assert(std::is_trivially_copyable_v<ImpactAudioStats>);

class ImpactAudio {
public:
	ImpactAudio(MixerCore &r_mixer, std::shared_ptr<const ImpactSoundCache> p_cache, std::shared_ptr<const core::audio::WaveBank> p_bank,
			const AudioTuning &p_tuning, OneShotPool &r_pool);
	~ImpactAudio();
	ImpactAudio(const ImpactAudio &) = delete;
	ImpactAudio &operator=(const ImpactAudio &) = delete;

	void set_listener(const ListenerState &p_listener);
	void play_impact(const ImpactSoundRequest &p_request, SimMixerClock &r_clock);
	void on_effect_sound(const EffectSoundRequest &p_request, SimMixerClock &r_clock);
	const ImpactAudioStats &stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
