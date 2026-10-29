#pragma once

#include "bf3/audio/audio_config.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/audio/weapon_foley.h"
#include "bf3/audio/weapon_sound.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"
#include "bf3/present/presentation_events.h"
#include "bf3/present/presented.h"
#include "bf3/sim/weapons/sim_time.h"
#include "bf3/sim/weapons/weapon_rng.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace bf3::audio {

inline constexpr uint32_t kTuningVoices = uint32_t(AudioTuning{}.max_voices);

struct GameMixerSizes {
	uint32_t voices = kTuningVoices;
	uint32_t weapon_voices = kTuningVoices;
	uint32_t one_shots = kTuningVoices;
	uint32_t instances = 2 * kTuningVoices;
	bool operator==(const GameMixerSizes &) const = default;
};

GameMixerSizes game_mixer_sizes(const AudioTuning &p_tuning, uint32_t p_max_voices = 0, uint32_t p_max_instances = 0);

MixerConfig game_mixer_config(const AudioTuning &p_tuning, const GameMixerSizes &p_sizes, uint32_t p_command_capacity);

inline constexpr uint32_t kHashMelee = 210357473u;

constexpr uint64_t sound_seed(uint32_t p_entity, uint32_t p_burst_seq) {
	return sim::weapons::splitmix64((uint64_t(p_entity) << 32) | uint64_t(p_burst_seq));
}

struct SimMixerClockConfig {
	uint32_t rate = kMixRate;
	int64_t bias_frames = 0;
};

class SimMixerClock {
public:
	SimMixerClock(const MixerCore &p_mixer, const SimMixerClockConfig &p_config = {});

	void set_output_latency_frames(uint64_t p_frames);

	void set_now(const present::PresentClock &p_now);

	uint64_t frame_of(sim::weapons::SimTime p_time, present::PresentTimeline p_timeline);
	uint64_t heard_frame() const;
	uint64_t late() const;
	uint64_t max_late_frames() const;

	void set_bias_frames(int64_t p_bias);
	int64_t bias_frames() const;
	uint64_t output_latency_frames() const;
	const present::PresentClock &now() const;
	uint64_t earliest_frame() const;
	uint32_t rate() const;

private:
	const MixerCore *mixer_ = nullptr;
	SimMixerClockConfig config_;
	uint64_t latency_frames_ = 0;
	present::PresentClock now_;
	uint64_t late_ = 0;
	uint64_t max_late_frames_ = 0;
};

enum class WeaponAudioForm : uint8_t {
	None = 0,
	Driver = 1,
	OneShot = 2,
};

struct WeaponAudioDef {
	WeaponAudioForm form = WeaponAudioForm::None;
	std::string patch;
	WeaponSoundBinding binding;
	WeaponFireMode fire_mode = WeaponFireMode::Automatic;
	std::shared_ptr<const core::audio::SoundProgram> program;
	std::shared_ptr<const core::audio::SoundProgram> explosion;
	std::string error;

	uint32_t one_shot_event = 0;
	bool has_reload = false;
	double shot_interval_s = 0.0;
	WeaponFoley foley;
};

struct WeaponAudioTable {
	std::vector<WeaponAudioDef> defs;
	uint32_t compiled = 0;
	uint32_t failed = 0;
};

struct WeaponSoundSource {
	std::string sound;
	std::string explosion_sound;
	double shot_interval_s = 0.0;
	WeaponFireMode fire_mode = WeaponFireMode::Automatic;
	bool one_shot = false;
};

core::Result<WeaponAudioTable> load_weapon_audio(const core::EbxResolver &p_db, std::span<const WeaponSoundSource> p_defs, uint32_t p_mix_rate);

inline constexpr std::array<uint32_t, 3> kOneShotEvents{ kHashFire, kHashStep, kHashMelee };

bool event_reaches_graph(const core::audio::SoundProgram &p_program, uint32_t p_hash);

uint32_t one_shot_event_of(const core::audio::SoundProgram &p_program);

struct OneShotOptions {
	uint32_t event = 0;
	bool set_first_person = false;
	bool first_person = false;
	uint32_t mix_rate = kMixRate;
	std::function<void(PatchInstance &, InstanceHandle)> before_start;
};

inline constexpr double kOneShotStopAfterSeconds = 3.0;
inline constexpr double kOneShotForgetSeconds = 20.0;

class OneShotPool {
public:
	explicit OneShotPool(uint32_t p_limit = kTuningVoices);

	bool start(MixerCore &r_mixer, std::shared_ptr<const core::audio::SoundProgram> p_program, std::shared_ptr<const core::audio::WaveBank> p_bank,
			const AudioTuning &p_tuning, uint64_t p_seed, const EmitterState &p_emitter, const ListenerState &p_listener, uint64_t p_frame);

	bool start(MixerCore &r_mixer, std::shared_ptr<const core::audio::SoundProgram> p_program, std::shared_ptr<const core::audio::WaveBank> p_bank,
			const AudioTuning &p_tuning, uint64_t p_seed, const EmitterState &p_emitter, const ListenerState &p_listener, uint64_t p_frame,
			const OneShotOptions &p_options);

	void on_finished(InstanceHandle p_handle);
	uint32_t limit() const { return limit_; }
	uint32_t live() const;
	uint32_t peak() const;
	uint64_t started() const;
	uint64_t stolen() const;
	uint64_t failed() const;

	InstanceHandle last_handle() const;
	bool contains(InstanceHandle p_handle) const;

	uint32_t resend_emitters(MixerCore &r_mixer) const;

	void update(MixerCore &r_mixer, uint32_t p_rate = kMixRate);

	bool stop(MixerCore &r_mixer, InstanceHandle p_handle);
	uint64_t forgotten() const;

	bool set_emitter(MixerCore &r_mixer, InstanceHandle p_handle, const EmitterState &p_emitter);

private:
	struct Entry {
		InstanceHandle handle = kNoInstance;
		uint64_t start_frame = 0;
		EmitterState emitter;
		bool stopped = false;
	};
	uint32_t limit_ = kTuningVoices;
	std::vector<Entry> live_;
	uint32_t peak_ = 0;
	uint64_t started_ = 0, stolen_ = 0, failed_ = 0, forgotten_ = 0;
	InstanceHandle last_ = kNoInstance;
};

struct MuzzlePoint {
	bool valid = false;
	Vec3 position;
	Vec3 forward{ 0.0f, 0.0f, 1.0f };
};
static_assert(std::is_trivially_copyable_v<MuzzlePoint>);

struct WeaponAudioConfig {
	uint32_t max_weapon_voices = kTuningVoices;
	float max_weapon_distance_m = 1000.0f;
	uint32_t own_slot_unknown = 0xFF;

	double own_hold_intervals = 1.5;

	double other_hold_ticks = 3.0;

	double other_event_hold_ticks = 2.0;

	float standin_muzzle_forward_m = 0.5f;

	double voice_forget_seconds = 15.0;
	uint32_t mix_rate = kMixRate;
};

struct WeaponAudioStats {
	uint64_t bursts = 0;
	uint64_t own_bursts = 0;
	uint64_t rounds = 0;
	uint64_t trigger_ups = 0;
	uint64_t one_shots = 0;
	uint64_t culled = 0;
	uint64_t unbound_defs = 0;
	uint64_t mixer_refusals = 0;
	uint64_t retried_commands = 0;
	uint32_t weapon_voices = 0;
	uint32_t peak_weapon_voices = 0;

	uint64_t reloads = 0;
	uint64_t stolen = 0;
	uint64_t event_rounds = 0;
	uint64_t state_rounds = 0;
	uint64_t forgotten = 0;
	uint64_t emitter_updates = 0;
	uint64_t detonations = 0;
	uint64_t foley = 0;
	uint64_t foley_releases = 0;
	uint64_t foley_out_of_reach = 0;
	uint64_t accessory_sounds = 0;
	uint64_t joins = 0;
};
static_assert(std::is_trivially_copyable_v<WeaponAudioStats>);

class WeaponAudioObserver {
public:
	virtual ~WeaponAudioObserver() = default;
	virtual void on_driver_instance(uint8_t p_slot, uint64_t p_seed, uint64_t p_first_round_frame, PatchInstance &r_instance) = 0;
	virtual void on_trigger_up(uint8_t p_slot, uint64_t p_seed, uint64_t p_stop_frame) = 0;

	virtual void on_foley(const FoleyCue &, PatchInstance *) {}
};

class WeaponAudioRouter {
public:
	WeaponAudioRouter(MixerCore &r_mixer, std::shared_ptr<const WeaponAudioTable> p_table, std::shared_ptr<const core::audio::WaveBank> p_bank,
			const AudioTuning &p_tuning, OneShotPool &r_one_shots, const WeaponAudioConfig &p_config = {});
	~WeaponAudioRouter();
	WeaponAudioRouter(const WeaponAudioRouter &) = delete;
	WeaponAudioRouter &operator=(const WeaponAudioRouter &) = delete;

	void set_own_slot(uint8_t p_slot);

	void set_slot_sound(uint8_t p_slot, uint16_t p_def, uint16_t p_sound);

	void set_own_first_person(bool p_first_person);
	void set_own_trigger(bool p_down);

	void set_others_newest(std::span<const present::PresentedOtherWeapon> p_newest, sim::weapons::SimTime p_horizon);

	void set_listener(const ListenerState &p_listener);

	void set_own_weapons(const present::PresentedOwnWeapons *p_weapons);

	void on_events(std::span<const present::PresentEvent> p_events, SimMixerClock &r_clock);

	void on_others(std::span<const present::PresentedSoldier> p_soldiers, std::span<const present::PresentedOtherWeapon> p_weapons,
			std::span<const MuzzlePoint> p_muzzles, SimMixerClock &r_clock);

	void update(uint64_t p_now_ns);

	void set_observer(WeaponAudioObserver *p_observer);
	const WeaponAudioStats &stats() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
