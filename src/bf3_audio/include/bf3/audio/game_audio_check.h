#pragma once
// The in-game audio check (docs/design/m2a.md E7, §12.3; owner au3; W3.0 contract, docs/design/m2a-w3.md). Godot-free.
// No au file is edited (§16 review "au3 edited au's check"): this file drives the recording schedule of the game run
// and judges it with au's public analyse_check (audio_check.h), so the in-game recording is measured with exactly the
// rules of G7 / G8.
//
// The run (gx's m2a_check case "audio" drives it; the level session's Bf3GameAudio records):
//   - a server bot (the firefight observer pair, or a dev SpawnAt'd bot) fires M16A3 bursts of `rounds` rounds;
//     the free camera, the listener, holds at each of kGameCheckDistances on the shooter's forward axis, facing it,
//     for `bursts_per_distance` bursts (third person: the others' drivers are IsFirstPerson 0);
//   - the check sees each burst through WeaponAudioObserver (the shooter's driver instance gets the core and noise
//     taps of audio_check.h before its Start; the burst's first round and stop frames become a ScheduledBurst), and
//     the full mix through its MixTap on the game mixer;
//   - then item 4: Bf3GameAudio::audition of each kGameAuditionItems patch at its band distance (client-local, no
//     server request), recorded the same way (one-shot windows instead of bursts);
//   - analyse_game_check maps the run onto a CheckScript (one segment per distance) and calls analyse_check, plus
//     the in-game rows: short_blocks = late_blocks = late_commands = 0, mixer_refusals = 0 (WeaponAudioStats), and
//     the listener distance of every burst within 0.25 m of its step's.
#include "bf3/audio/audio_check.h"
#include "bf3/audio/audio_config.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/core/path_guard.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::audio {

// E7: the listener distances of the M16A3 bursts, metres (near / close-layer fade / distant HPF band / far bands).
inline constexpr std::array<float, 6> kGameCheckDistances{ 5.0f, 20.0f, 100.0f, 150.0f, 280.0f, 320.0f };

// E7 item 4: the client-local auditions (au §10.2 bands). The patch partitions and band distances are au3's to
// resolve from the data (game_check_script fills them; an item the game cannot load or play is skipped through
// GameAudioCheck::skip_step, and analyse_game_check FAILS its step: E7 item 4 is not met with a skipped item).
struct GameAuditionItem {
	std::string name; // "m98b", "rpg7", "m1a2"
	std::string patch; // the SoundPatchAsset partition ("" until resolved)
	std::vector<float> distances_m; // the band distances to audition at
	// au3 addition: the crossfader bands (GroupFadeBegin, GroupFadeEnd) of au §10.2 / G5.4 the item is judged by.
	std::vector<std::pair<float, float>> bands;
};

struct GameCheckStep {
	std::string name; // "d5", "d20", ... or the audition item's name
	float distance_m = 0.0f;
	uint32_t bursts = 2;
	bool audition = false; // a GameAuditionItem one-shot instead of a bot burst
	std::string patch; // audition only
	// au3 additions: the bursts of this step get both M16A3 distance HPFs pinned to 0 Hz (au's far_pinned segment,
	// the reference of the hpf rows); an audition step's item bands (GameAuditionItem::bands).
	bool pin_distance_hpfs = false;
	std::vector<std::pair<float, float>> bands;
};

struct GameCheckScript {
	std::vector<GameCheckStep> steps;
	uint32_t rounds = 10; // per burst (the bot's burst length in the check)
	double settle_s = 1.0; // after the camera moved, before the next burst counts
	double burst_period_s = 4.0; // > the longest tail (audio_check.h)
	double timeout_s = 120.0; // the whole run
	CheckScript analysis; // the rules (spacing, tolerances) handed to analyse_check; segments are filled per run
};

// The six distances x 2 bursts, then the audition items (au3 resolves their patches; unresolved: failing rows).
GameCheckScript game_check_script();
std::vector<GameAuditionItem> game_audition_items();

// One recorded burst or audition window, in MixerClock frames.
struct GameCheckWindow {
	uint32_t step = 0;
	uint64_t seed = 0;
	uint64_t first_frame = 0;
	uint64_t stop_frame = 0;
	float listener_distance_m = 0.0f; // measured at the first round
	// au3 additions: the window's patch probed at listener_distance_m with the window's seed (a Godot-free control
	// evaluation of a fresh PatchInstance on the same program, au's G5 method; -1: not probed). Bursts: the distant
	// HPF v46 (kM16NoiseHpf), the per-burst pitch v28 after the Fire, the 0-20 m close group and the 250-300 m far
	// groups (mean of the three). Auditions: each step band's group amplitude (bands_found of them matched).
	bool audition = false;
	bool stopped = false; // the stop frame is known (on_trigger_up; auditions: first_frame + their window)
	float probe_hpf_hz = -1.0f;
	float probe_pitch = -1.0f;
	float probe_close = -1.0f;
	float probe_far = -1.0f;
	std::array<float, 4> band_amplitude{ -1.0f, -1.0f, -1.0f, -1.0f };
	uint32_t bands_found = 0;
	// The band of the group before each matched one in its crossfader (the group fades in over it, au G5.4's rule;
	// { -1, -1 } for a first group): the amplitude is partial there too.
	std::array<std::pair<float, float>, 4> band_fade_in{ { { -1.0f, -1.0f }, { -1.0f, -1.0f }, { -1.0f, -1.0f }, { -1.0f, -1.0f } } };
};

// Records the run: a MixTap for the full mix (set on the game mixer before its stream starts), TapRecorders on the
// check shooter's driver instances (WeaponAudioObserver), and the windows. Preallocated for timeout_s.
class GameAudioCheck final : public MixTap, public WeaponAudioObserver {
public:
	GameAudioCheck(const GameCheckScript &p_script, uint32_t p_rate, uint64_t p_start_frame);
	~GameAudioCheck() override;

	void set_shooter(uint8_t p_slot); // only this slot's bursts are recorded
	void begin_step(uint32_t p_step, float p_listener_distance_m); // the camera is in place
	// MixTap (audio thread).
	void on_block(const float *p_interleaved_stereo, uint32_t p_frames, uint64_t p_first_frame) override;
	// WeaponAudioObserver (game thread).
	void on_driver_instance(uint8_t p_slot, uint64_t p_seed, uint64_t p_first_round_frame, PatchInstance &r_instance) override;
	void on_trigger_up(uint8_t p_slot, uint64_t p_seed, uint64_t p_stop_frame) override;
	// An audition one-shot (Bf3GameAudio::audition) of step p_step started at p_frame.
	void on_audition(uint32_t p_step, uint64_t p_seed, uint64_t p_frame, PatchInstance &r_instance);

	bool step_done(uint32_t p_step) const; // its bursts recorded and their tails past, or skipped
	// au3 addition: a step the driver cannot run (an audition whose patch loaded from no source). step_done is then
	// true; analyse_game_check fails it ("skipped.<step>" and its "windows.<step>" row), never a silent pass.
	void skip_step(uint32_t p_step, std::string p_reason);
	struct Skip {
		uint32_t step = 0;
		std::string reason;
	};
	std::span<const Skip> skipped() const;
	std::span<const GameCheckWindow> windows() const;
	core::Status write_wav(const std::filesystem::path &p_path) const; // --audio-record (scratch or user cache only)
	// au3 additions. write_wav above goes through the guard set here (none set: refused, PathGuard NoRoots).
	void set_guard(const core::PathGuard &p_guard);
	core::Status write_wav(const core::PathGuard &p_guard, const std::filesystem::path &p_path) const;
	const GameCheckScript &script() const;
	int64_t current_step() const; // begin_step's, -1 before the first or after an out-of-range step
	uint32_t rate() const;
	uint64_t start_frame() const;
	uint64_t total_frames() const;
	uint64_t recorded_until() const; // the end of the newest mixed block seen (absolute MixerClock frame)
	// Read after the run (the audio thread no longer mixes into the game mixer or the tap is cleared).
	std::span<const float> mix() const; // stereo, [start_frame, start_frame + total_frames)
	std::span<const float> core_tap() const;
	std::span<const float> noise_tap() const;
	uint64_t dropped_frames() const; // mix blocks outside the preallocated span

private:
	struct State;
	std::unique_ptr<State> state_;
};

struct GameCheckCounters {
	uint64_t short_blocks = 0, late_blocks = 0, late_commands = 0; // MixerStats
	uint64_t mixer_refusals = 0; // WeaponAudioStats
};

// The E7 rows: analyse_check over the recorded windows (one CheckScript segment per distance step) and the in-game
// rows above; audition steps are judged against au §10.2's bands of their item.
CheckReport analyse_game_check(const GameCheckScript &p_script, const GameAudioCheck &p_check, const GameCheckCounters &p_counters,
		const AudioTuning &p_tuning, uint32_t p_rate);

} // namespace bf3::audio
