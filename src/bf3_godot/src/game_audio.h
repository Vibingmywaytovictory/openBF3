#pragma once
// Bf3GameAudio: the game's audio (docs/design/m2a.md §12.3, E7; owner au3; W3.0 contract, docs/design/m2a-w3.md).
// Owns one MixerCore (MixerConfig::max_instances = audio::kGameMixerInstances), one Bf3AudioStream / playback and one
// AudioStreamPlayer (au §7.2), the WeaponAudioRouter, ImpactAudio and their shared OneShotPool. A plain class owned by
// the level session (gx), not a registered Godot class: it has no editor surface, and the session drives every call.
//   load      loader thread, no Godot API (m2a_game.h order: after Bf3GameFx::load, with its impact sound names)
//   build     main thread: the mixer, the stream and the player under p_parent; playback starts
//   step      main thread, every display frame (m2a_game.h order: after Bf3GameFx::step)
//   audition  client-local one-shot of any compiled patch at a world position (E7 item 4; only m2a_check calls it,
//             never the server or the wire); a patch not loaded yet is compiled on the calling thread
// The listener is M2aFrame::camera (1p or free). project.godot's audio/driver/mix_rate is 48000 (au).

#include "m2a_game.h"

#include "bf3/audio/audio_config.h"
#include "bf3/audio/game_audio_check.h"
#include "bf3/audio/impact_audio.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/core/result.h"
#include "bf3/fx/triggers.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace godot {
class Node;
} // namespace godot

namespace bf3::godot_side {

struct GameAudioStats {
	audio::WeaponAudioStats router;
	audio::ImpactAudioStats impacts;
	uint64_t mixer_frames = 0;
	uint64_t short_blocks = 0, late_blocks = 0, late_commands = 0, ring_overflows = 0; // MixerStats (E7 gates 0)
	uint32_t peak_instances = 0;
	uint64_t clock_late = 0; // SimMixerClock::late()
	uint64_t output_latency_frames = 0;
	uint32_t programs = 0, program_failures = 0; // weapon + impact patches compiled at load
	// au3 additions.
	uint32_t one_shots_live = 0, one_shots_peak = 0;
	uint64_t one_shots_stolen = 0, one_shots_failed = 0;
	uint64_t starved_voices = 0, feedback_drops = 0, max_block_ns = 0; // MixerStats (max_block_ns: report only)
	int64_t bias_frames = 0;
	uint64_t auditions = 0;
};

class Bf3GameAudio {
public:
	Bf3GameAudio();
	~Bf3GameAudio(); // shutdown()
	Bf3GameAudio(const Bf3GameAudio &) = delete;
	Bf3GameAudio &operator=(const Bf3GameAudio &) = delete;

	// Loader thread: the kit items' patches (load_weapon_audio over the SharedLevel's GunplayLevel::defs), the impact
	// sounds (p_impact_sounds: Bf3GameFx::impact_sound_names), the wave bank over every compiled program.
	core::Status load(const M2aLoadInputs &p_in, std::span<const std::string> p_impact_sounds);
	bool loaded() const;
	// Main thread, after load returned success.
	bool build(godot::Node *p_parent, std::string *r_error);
	bool built() const;
	// Main thread, every display frame.
	void step(const M2aFrame &p_frame, std::span<const fx::ImpactSoundCue> p_impact_cues);
	// au3 addition (m2a-w3.md §4.1 "effect sounds"): the same, with the running effects' SoundStart / SoundStop of
	// this frame (fx3's accessor), keyed by effect and component. Their patches must be among p_impact_sounds (compiled
	// at load); any other is counted in ImpactAudioStats::unknown_patches and stays silent (no compile on the main
	// thread).
	void step(const M2aFrame &p_frame, std::span<const fx::ImpactSoundCue> p_impact_cues, std::span<const audio::EffectSoundRequest> p_effect_sounds);
	// E7 item 4 (client-local). p_position in world space, Frostbite axes.
	bool audition(std::string_view p_patch, const fx::Vec3 &p_position, std::string *r_error);
	// E7: the check's recorder (MixTap + router observer); null detaches. Set before the check's first step.
	void set_check(audio::GameAudioCheck *p_check);
	audio::MixerCore *mixer() const; // null before build
	GameAudioStats stats() const;
	std::vector<std::string> load_report() const; // per kit item: form (driver / one-shot / none) and errors (E7 item 1)
	void shutdown(); // stops the player, then the mixer goes (idempotent; main thread)

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::godot_side
