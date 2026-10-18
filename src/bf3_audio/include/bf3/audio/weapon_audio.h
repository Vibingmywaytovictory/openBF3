#pragma once
// Weapons in the game's mixer (docs/design/m2a.md §12.3, §8.3, §4.4; owner au3; W3.0 contract, docs/design/m2a-w3.md).
// Godot-free, game (main) thread only: the router turns the drained PresentationEvents and the presented others into
// WeaponSoundDriver calls and one-shot PatchInstances on one MixerCore; Bf3GameAudio (src/bf3_godot/src/game_audio.h)
// owns the mixer, the stream and this router, and audio_router_tests.cpp (audio.router.unit) drives it offline.
//
// Who sounds where:
//   - the own soldier: one driver on the active weapon's patch, IsFirstPerson 1, at the listener (the camera);
//     trigger down / rounds / trigger up from the own OwnShot events (exact sub-tick round times, Predicted
//     timeline) and the OwnTimed reload / deploy / empty-click events (one-shots);
//   - every other soldier: a driver with IsFirstPerson 0 at its 3p muzzle (tp's crowd muzzle, else the eye height +
//     the weapon offset), driven by its records' firing bit and burst sequence (state, Render timeline:
//     host::present_other_weapons), so a lost RemoteShot datagram never cuts a burst's sound; RemoteShot events
//     only refine the round frames of a burst already running;
//   - knife and M67: no driver loop; their swing / throw / explosion patches start as one-shots (OwnShot, RemoteShot,
//     Detonation), WeaponAudioDef::form says which form a def uses (E7 item 1 lists it per kit item);
//   - impacts and effect sounds: impact_audio.h (one OneShotPool shared with the router).
// Voice limits until A3 (§16 Q9): at most kMaxWeaponVoices driver instances (nearest first; a culled one stops at
// its next trigger up) and kMaxOneShotVoices one-shots (oldest stolen); Bf3GameAudio creates its mixer with
// MixerConfig::max_instances = kGameMixerInstances, so the mixer's own cap is never what refuses an instance
// (WeaponAudioStats::mixer_refusals is gated 0 in E7).
// Seeds (§4.4): a burst's instance is seeded sound_seed(entity, burst seq) (patch_instance.h): for the others the
// entity is the shooter's slot and the burst seq the record's 4-bit burst_seq (so every client renders the same
// variant, E15); for the own soldier the router's own burst counter (only this client hears the 1p patch).
// Listener: set_listener every frame from the active camera (1p or free); after a SetListener the router re-sends
// every live instance's emitter (au: a SetListener alone does not move the emitters' cones and distances).
#include "bf3/audio/audio_config.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/audio/weapon_sound.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"
#include "bf3/host/client_session.h"
#include "bf3/host/client_wiring.h"
#include "bf3/host/presentation_events.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/weapon_rng.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace bf3::audio {

// Voice limits (§12.3, §16 Q9) and the game mixer's instance cap.
inline constexpr uint32_t kMaxWeaponVoices = 32;
inline constexpr uint32_t kMaxOneShotVoices = 64;
inline constexpr uint32_t kGameMixerInstances = 96; // MixerConfig::max_instances of Bf3GameAudio's mixer
static_assert(kMaxWeaponVoices + kMaxOneShotVoices <= kGameMixerInstances);

// §4.4: a weapon burst's instance seed, splitmix64(entity << 32 | burst seq).
constexpr uint64_t sound_seed(uint32_t p_entity, uint32_t p_burst_seq) {
	return sim::weapons::splitmix64((uint64_t(p_entity) << 32) | uint64_t(p_burst_seq));
}

// Presentation time -> MixerClock frame (§12.3 "Clock"). The frame heard now is MixerCore::clock() minus the output
// latency measured on the stream (frames mixed but not yet played: Bf3AudioStream's played-frame counter, or the
// AudioServer's output latency until that exists); an event at time t on a timeline is heard at
//   heard_now + round((t - now(timeline)) x rate / kTimeUnitsPerSecond) + bias_frames,
// and is never stamped before MixerCore::earliest_frame() (clock + schedule_lead_blocks blocks): an earlier frame is
// clamped there and counted late (late(), max_late_frames()). Immediate events take earliest_frame(). Predicted and
// Render use host::PresentClock's two times of the display frame (set_now once per frame, before frame_of).
struct SimMixerClockConfig {
	uint32_t rate = kMixRate;
	int64_t bias_frames = 0; // a constant added to every mapped frame (au3 tunes; reported)
};

class SimMixerClock {
public:
	SimMixerClock(const MixerCore &p_mixer, const SimMixerClockConfig &p_config = {});

	// Frames between mixed and heard (measured on the stream each frame; 0 offline).
	void set_output_latency_frames(uint64_t p_frames);
	// The display frame's presentation times (host::present_clock).
	void set_now(const host::PresentClock &p_now);
	// The frame an event at p_time on p_timeline is heard at (see above). A timeline whose clock is not valid in the
	// last set_now maps to earliest_frame() (counted in late()).
	uint64_t frame_of(sim::weapons::SimTime p_time, host::PresentTimeline p_timeline);
	uint64_t heard_frame() const; // clock() - latency (0 while the latency exceeds the clock)
	uint64_t late() const;
	uint64_t max_late_frames() const;

private:
	const MixerCore *mixer_ = nullptr;
	SimMixerClockConfig config_;
	uint64_t latency_frames_ = 0;
	host::PresentClock now_;
	uint64_t late_ = 0;
	uint64_t max_late_frames_ = 0;
};

// How a def sounds (E7 item 1).
enum class WeaponAudioForm : uint8_t {
	None = 0, // no patch (WeaponDef::sound empty or failed to compile: counted, reported)
	Driver = 1, // a firearm: bind_weapon_sound succeeded, WeaponSoundDriver loop
	OneShot = 2, // knife swing, M67 throw: the patch starts as a one-shot PatchInstance per use
};

struct WeaponAudioDef {
	WeaponAudioForm form = WeaponAudioForm::None;
	std::string patch; // WeaponDef::sound
	WeaponSoundBinding binding; // Driver
	WeaponFireMode fire_mode = WeaponFireMode::Automatic; // Driver: from the def's default fire mode
	std::shared_ptr<const core::audio::SoundProgram> program; // Driver and OneShot
	std::shared_ptr<const core::audio::SoundProgram> explosion; // M67: the detonation one-shot (null otherwise)
	std::string error; // why form is None
};

// The match's weapon sounds, by WeaponTable def id (GunplayLevel::defs order). Built on the loader thread.
struct WeaponAudioTable {
	std::vector<WeaponAudioDef> defs;
	uint32_t compiled = 0; // defs whose patch compiled (E7 item 1: 12/12 for a kit)
	uint32_t failed = 0;
};

// What load_weapon_audio needs of one def (filled from core::gameplay::WeaponDef by the caller, so bf3_audio needs
// no gameplay headers): the sound partition, the shot interval and the default fire mode, and whether it is a
// melee / thrown item (OneShot form).
struct WeaponSoundSource {
	std::string sound; // WeaponDef::sound
	std::string explosion_sound; // grenades: the explosion's patch partition ("" none)
	double shot_interval_s = 0.0; // gameplay::shot_interval(FireFunction)
	WeaponFireMode fire_mode = WeaponFireMode::Automatic;
	bool one_shot = false; // knife, grenade
};

// Compiles every def's WeaponDef::sound with compile_patch (strict: a failure is the def's error, the table still
// builds) and binds the firearms (bind_weapon_sound at the def's shot interval). p_defs: GunplayLevel::defs.
core::Result<WeaponAudioTable> load_weapon_audio(const core::EbxResolver &p_db, std::span<const WeaponSoundSource> p_defs, uint32_t p_mix_rate);

// Ring of one-shot instances shared by the router and ImpactAudio: at most p_limit live, the oldest stolen (Kill)
// when a new one needs a slot (counted). Owns no PatchInstance after Start (the mixer does).
class OneShotPool {
public:
	explicit OneShotPool(uint32_t p_limit = kMaxOneShotVoices);
	// Creates the instance (seed, emitter, listener), pushes Start at p_frame. False: create failed or ring full
	// (counted; the caller does not retry a one-shot).
	bool start(MixerCore &r_mixer, std::shared_ptr<const core::audio::SoundProgram> p_program, std::shared_ptr<const core::audio::WaveBank> p_bank,
			const AudioTuning &p_tuning, uint64_t p_seed, const EmitterState &p_emitter, const ListenerState &p_listener, uint64_t p_frame);
	// Feedback finished flags (MixerCore::poll_feedback) free their slots.
	void on_finished(InstanceHandle p_handle);
	uint32_t live() const;
	uint32_t peak() const;
	uint64_t started() const;
	uint64_t stolen() const;
	uint64_t failed() const;

private:
	struct Entry {
		InstanceHandle handle = kNoInstance;
		uint64_t start_frame = 0;
	};
	uint32_t limit_ = kMaxOneShotVoices;
	std::vector<Entry> live_;
	uint32_t peak_ = 0;
	uint64_t started_ = 0, stolen_ = 0, failed_ = 0;
};

// A 3p muzzle in world space (tp's CrowdEvaluator::muzzle_model through Bf3GameCrowd; Godot-free mirror of it).
struct MuzzlePoint {
	bool valid = false;
	Vec3 position;
	Vec3 forward{ 0.0f, 0.0f, 1.0f }; // the barrel direction (the emitter's cone axis)
};
static_assert(std::is_trivially_copyable_v<MuzzlePoint>);

struct WeaponAudioConfig {
	uint32_t max_weapon_voices = kMaxWeaponVoices;
	float max_weapon_distance_m = 1000.0f; // a soldier beyond it gets no driver (counted culled)
	uint32_t own_slot_unknown = 0xFF; // set_own_slot not called yet
};

// Plain counters, game thread (the E7 report prints them).
struct WeaponAudioStats {
	uint64_t bursts = 0; // trigger downs (own + others)
	uint64_t own_bursts = 0;
	uint64_t rounds = 0; // round_fired calls
	uint64_t trigger_ups = 0;
	uint64_t one_shots = 0; // knife / M67 / OwnTimed one-shots started through the pool
	uint64_t culled = 0; // a soldier that wanted a driver beyond max_weapon_voices or max_weapon_distance_m
	uint64_t unbound_defs = 0; // a def with form None that fired
	uint64_t mixer_refusals = 0; // a Start the mixer refused (ring full): gated 0 in E7
	uint64_t retried_commands = 0; // a round / trigger up the ring refused, retried next frame
	uint32_t weapon_voices = 0; // live drivers
	uint32_t peak_weapon_voices = 0;
};
static_assert(std::is_trivially_copyable_v<WeaponAudioStats>);

// The in-game check's view of the router (game_audio_check.h): every driver instance before its Start command (taps,
// pins; the rules of PatchInstance::add_voice_tap) and every burst's frames. Never set in play.
class WeaponAudioObserver {
public:
	virtual ~WeaponAudioObserver() = default;
	virtual void on_driver_instance(uint8_t p_slot, uint64_t p_seed, uint64_t p_first_round_frame, PatchInstance &r_instance) = 0;
	virtual void on_trigger_up(uint8_t p_slot, uint64_t p_seed, uint64_t p_stop_frame) = 0;
};

class WeaponAudioRouter {
public:
	WeaponAudioRouter(MixerCore &r_mixer, std::shared_ptr<const WeaponAudioTable> p_table, std::shared_ptr<const core::audio::WaveBank> p_bank,
			const AudioTuning &p_tuning, OneShotPool &r_one_shots, const WeaponAudioConfig &p_config = {});
	~WeaponAudioRouter(); // Kill for every live driver (the mixer outlives the router)
	WeaponAudioRouter(const WeaponAudioRouter &) = delete;
	WeaponAudioRouter &operator=(const WeaponAudioRouter &) = delete;

	void set_own_slot(uint8_t p_slot); // PresentationFrame's own slot (the mapper's adopted life)
	// The active camera; re-sends every live instance's emitter.
	void set_listener(const ListenerState &p_listener);
	// The drained events of this frame, oldest first (the ring's order): OwnShot, OwnTimed, RemoteShot, Detonation,
	// Death (a dead shooter's burst ends), Spawn (a new life resets the slot's driver).
	void on_events(std::span<const host::PresentEvent> p_events, SimMixerClock &r_clock);
	// The others as drawn this frame: present_others' soldiers and present_other_weapons' fields, index for index
	// (same frame, same render time), and each other's 3p muzzle when the crowd has one (r_muzzles[i] valid false:
	// eye height + the weapon offset). Starts, steps and ends the others' bursts from firing and burst_seq changes.
	void on_others(std::span<const host::PresentedSoldier> p_soldiers, std::span<const host::PresentedOtherWeapon> p_weapons,
			std::span<const MuzzlePoint> p_muzzles, SimMixerClock &r_clock);
	// Once per frame after on_events / on_others: retries refused commands, reclaims, drains the mixer feedback into
	// the one-shot pool, culls to the voice limit. p_now_ns: net::monotonic_ns of the frame.
	void update(uint64_t p_now_ns);

	void set_observer(WeaponAudioObserver *p_observer); // null clears
	const WeaponAudioStats &stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::audio
