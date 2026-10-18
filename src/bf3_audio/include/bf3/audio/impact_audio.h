#pragma once
// Impact and effect sounds in the game's mixer (docs/design/m2a.md §12.2, §12.3; owner au3; W3.0 contract,
// docs/design/m2a-w3.md). Godot-free, game thread.
//
// Two sources start one-shot PatchInstances (au §7.2: no driver needed) through the OneShotPool the weapon router
// shares (weapon_audio.h; at most kMaxOneShotVoices, the oldest stolen):
//   - ImpactCell::impact_sound of the cell fx3's triggers select for an Impact / SoldierContact / Detonation event
//     (fx::FxTriggers hands the patch name and the event; one start per static Impact within the cell's cull
//     distance, E8 (7)); seeded with the event's impact seed (PresentEvent::seed, §4.4: the effect seed mixed with
//     the pellet and the impact ordinal), so every client picks the same variant;
//   - FxEvent SoundStart / SoundStop of the running effects (fx3 forwards them; the effect component's sound patch),
//     seeded with the effect instance id (patch_instance.h), stopped at SoundStop.
// The patches compile on the loader thread (Bf3GameAudio::load) from the names fx3 lists for the level
// (fx::impact_sound_names, triggers.h) and the effects' sound components; a name that does not compile is counted
// and plays nothing.
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
#include <vector>

namespace bf3::audio {

// Compiled one-shot patches by partition name (lowercase, as ImpactCell::impact_sound and the effect components
// name them). Built on the loader thread, read-only afterwards.
class ImpactSoundCache {
public:
	// Compiles each name once (compile_patch); failures are kept with their error and counted, never fatal.
	static core::Result<std::shared_ptr<const ImpactSoundCache>> build(const core::EbxResolver &p_db, std::span<const std::string> p_patches);
	// nullptr: unknown or failed to compile.
	std::shared_ptr<const core::audio::SoundProgram> find(std::string_view p_patch) const;
	// Every compiled program (Bf3GameAudio's wave list: read_program_waves).
	std::vector<std::shared_ptr<const core::audio::SoundProgram>> programs() const;
	size_t compiled() const;
	size_t failed() const;
	std::span<const std::string> errors() const; // "<patch>: <error>"

private:
	std::unordered_map<std::string, std::shared_ptr<const core::audio::SoundProgram>> programs_;
	std::vector<std::string> errors_;
};

// One impact sound request (fx3 -> au3): the cell's patch at the event's position and time.
struct ImpactSoundRequest {
	std::string_view patch; // ImpactCell::impact_sound (valid for the call)
	Vec3 position;
	Vec3 normal{ 0.0f, 1.0f, 0.0f };
	uint64_t seed = 0; // PresentEvent::seed (the impact seed)
	sim::weapons::SimTime time = 0; // PresentEvent::time
	host::PresentTimeline timeline = host::PresentTimeline::Render;
};

// One effect sound (FxEvent SoundStart / SoundStop), keyed by the effect instance and component.
struct EffectSoundRequest {
	std::string_view patch;
	uint64_t effect = 0; // fx::EffectHandle packed (index << 32 | generation): the seed and the key
	uint32_t component = 0;
	Vec3 position;
	bool start = true; // false: SoundStop
};

struct ImpactAudioStats {
	uint64_t impacts = 0; // started
	uint64_t effect_sounds = 0;
	uint64_t effect_stops = 0;
	uint64_t unknown_patches = 0; // not in the cache (counted, silent)
	uint64_t refused = 0; // the pool refused (create failed or ring full)
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

} // namespace bf3::audio
