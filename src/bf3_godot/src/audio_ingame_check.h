#pragma once

#include "bf3/anim/viewmodel_anim.h"
#include "bf3/audio/footstep_driver.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/wav_io.h"
#include "bf3/client_audio/sound_fate_ledger.h"
#include "bf3/present/presented.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/spawn.h"
#include "game_audio.h"

#include <godot_cpp/classes/audio_effect_capture.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::godot_side {

class Bf3GameAudio;

inline constexpr double kAudioIngameSeconds = 25.0;
inline constexpr double kAudioIngameSpawnTimeoutSeconds = 120.0;
inline constexpr double kAudioIngameSettleSeconds = 1.0;
inline constexpr double kAudioIngameHoldSeconds = 1.2;
inline constexpr double kAudioIngamePauseSeconds = 0.8;
inline constexpr double kAudioIngameReloadPauseSeconds = 3.0;
inline constexpr float kAudioIngameTurnDegPerSecond = 150.0f;
inline constexpr uint32_t kAudioIngameAlignSearchFrames = 16384;
inline constexpr uint32_t kAudioIngameAlignWindowFrames = 4096;
inline constexpr float kAudioIngameSameSample = 1.0e-6f;
inline constexpr uint32_t kAudioIngameZeroRunFrames = 64;
inline constexpr float kAudioIngameSoundLevel = 1.0e-3f;

struct AudioIngameAim {
	float yaw_deg = 0.0f;
	float pitch_deg = 0.0f;
};

inline constexpr std::array<AudioIngameAim, 10> kAudioIngameAims{ { { 0, -70 }, { 0, -35 }, { 0, -5 }, { 90, -5 },
		{ 180, -5 }, { 270, -5 }, { 45, -20 }, { 135, -50 }, { 225, 15 }, { 315, -10 } } };

enum class AudioIngamePhase : uint8_t { Mix, Pool, Steps, Flyby };

enum class AudioIngameLeg : uint8_t { Stand, Walk, Sprint };
inline constexpr size_t kAudioIngameLegs = 3;
inline constexpr std::array<std::string_view, kAudioIngameLegs> kAudioIngameLegNames{ "standing", "walking",
	"sprinting" };
inline constexpr std::array<double, kAudioIngameLegs> kAudioIngameLegSeconds{ 1.5, 3.0, 3.0 };
inline constexpr double kAudioIngameLegSettleSeconds = 0.6;
inline constexpr uint32_t kAudioIngameMinLegs = 2;
inline constexpr uint32_t kAudioIngameMinLegSteps = 3;
inline constexpr uint32_t kAudioIngameRunYaws = 16;
inline constexpr float kAudioIngameRunMeters = 32.0f;
inline constexpr float kAudioIngameRunRadiusShare = 0.6f;
inline constexpr float kAudioIngameRunOpenMeters = 24.0f;
inline constexpr float kAudioIngameArriveMeters = 1.0f;
inline constexpr double kAudioIngameArriveSeconds = 5.0;
inline constexpr double kAudioIngameClipCadenceShare = 0.025;

struct AudioIngameSpot {
	sim::Vec3 feet{};
	float yaw = 0.0f;
	float clear_m = 0.0f;
};
inline constexpr uint64_t kAudioIngameMinBotShots = 10;
inline constexpr float kAudioIngameFightMeters = 15.0f;

bool parse_audio_ingame_phase(std::string_view p_text, AudioIngamePhase &r_phase);
std::optional<double> audio_ingame_clip_steps_per_second(
		const anim::VmLocomotion &p_loco, const audio::FootstepParams &p_params, bool p_sprint, float p_speed);
float audio_ingame_clearance(
		const sim::CollisionWorld &p_world, const sim::MotorParams &p_motor, const sim::Vec3 &p_feet, float p_yaw);
float audio_ingame_clearest_yaw(const std::function<float(float)> &p_clearance, float p_from, float &r_clear_m);
std::optional<AudioIngameSpot> audio_ingame_open_spot(
		const sim::CollisionWorld &p_world, const sim::MotorParams &p_motor, std::span<const sim::SpawnPoint> p_spawns);
std::optional<AudioIngameSpot> audio_ingame_fight_spot(std::span<const present::PresentedSoldier> p_others,
		std::span<const uint8_t> p_slot_team, uint8_t p_own_team, const sim::Vec3 &p_feet);

inline constexpr double kAudioIngameGhostSeconds = 1.0;
inline constexpr double kAudioIngameLimitedBlocksShare = 0.01;
inline constexpr float kAudioIngameLimitFloorDb = -1.0f;
inline constexpr double kAudioIngameBalanceDb = 3.0;
inline constexpr double kAudioIngameOwnTailSeconds = 0.35;
inline constexpr double kAudioIngameOwnGuardSeconds = 0.8;
inline constexpr double kAudioIngameRestWindowSeconds = 0.25;
inline constexpr float kAudioIngameRestFloor = 1.0e-3f;
inline constexpr double kAudioIngameSameShotSeconds = 0.05;

struct AudioIngameIo {
	bool alive = false;
	float time_to_respawn = 0.0f;
	std::function<void(bool)> hold_fire;
	std::function<void()> reload;
	std::function<void()> grenade;
	std::function<std::string(std::span<const std::string>)> pick_primary;
	std::function<void()> deploy;
	std::function<void(float, float)> view;
	std::function<float()> yaw;
	std::function<void(bool, bool)> move;
	std::function<std::optional<anim::VmLocomotion>()> locomotion;
	std::function<float(float)> clearance;
	std::function<std::optional<AudioIngameSpot>()> open_spot;
	std::function<void(const AudioIngameSpot &)> teleport;
	std::function<std::optional<AudioIngameSpot>()> fight_spot;
	sim::Vec3 feet{};
	uint8_t own_slot = 0xFF;
};

struct AudioIngameLegTally {
	uint32_t legs = 0;
	uint64_t steps = 0;
	uint64_t animated = 0;
	uint64_t starts = 0;
	uint64_t stops = 0;
	double seconds = 0.0;
	double speed_sum = 0.0;
	double moved_m = 0.0;
	float speed_min = 0.0f;
	float speed_max = 0.0f;
	std::vector<std::pair<float, double>> intervals;
	std::vector<double> clip_intervals;
};

struct AudioSoundFate {
	audio::SoundCategory category = audio::SoundCategory::Other;
	std::string program;
	double request_s = 0.0;
	float distance_m = 0.0f;
	uint16_t scope = 0xFFFF;
	bool started = false;
	bool finished = false;
	bool refused = false;
	bool killed = false;
	bool joined = false;
	bool dropped_while_parked = false;
	uint8_t cull = 0;
	uint8_t park = 0;
	uint32_t parks = 0;
	uint32_t coalesced = 0;
};

struct AudioCategoryCounts {
	uint64_t requested = 0, started = 0, finished = 0, joined = 0;
	uint64_t data_drops = 0, other_drops = 0;
	std::map<std::string, uint64_t> reasons;
};

class AudioIngameCheck {
public:
	AudioIngameCheck(std::string p_out_dir, double p_seconds, AudioIngamePhase p_phase);
	~AudioIngameCheck();
	AudioIngameCheck(const AudioIngameCheck &) = delete;
	AudioIngameCheck &operator=(const AudioIngameCheck &) = delete;

	bool frame(double p_now_s, Bf3GameAudio &r_audio, const AudioIngameIo &p_io);
	std::string report() const;
	int exit_code() const { return done_ && failures_ == 0 ? 0 : 1; }

private:
	enum class Phase : uint8_t { WaitSpawn, Settle, Record, Done };
	void begin(double p_now_s, Bf3GameAudio &r_audio);
	void end(Bf3GameAudio &r_audio);
	void pull();
	void take(Bf3GameAudio &r_audio);
	void note(const audio::MixerCutEvent &p_event, Bf3GameAudio &r_audio);
	void script(double p_now_s, const AudioIngameIo &p_io);
	void analyse(Bf3GameAudio &r_audio);
	void analyse_mix(Bf3GameAudio &r_audio);
	void analyse_pool(Bf3GameAudio &r_audio);
	void analyse_steps(Bf3GameAudio &r_audio);
	void analyse_flyby(Bf3GameAudio &r_audio);
	void pick_run_yaw(const AudioIngameIo &p_io);
	void script_steps(double p_now_s, const AudioIngameIo &p_io);
	void tally_steps(double p_now_s, Bf3GameAudio &r_audio, const AudioIngameIo &p_io);
	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);
	void mix_row(const std::string &p_name, bool p_pass, const std::string &p_detail);

	std::string out_dir_;
	double seconds_ = kAudioIngameSeconds;
	AudioIngamePhase kind_ = AudioIngamePhase::Mix;
	audio::MixerLevels levels_at_start_;
	std::map<audio::InstanceHandle, double> refused_;
	std::map<audio::InstanceHandle, float> sound_peaks_;
	std::unique_ptr<audio::WavRecorder> sum_tap_;
	std::vector<std::pair<double, double>> own_fire_;
	std::vector<std::pair<audio::InstanceHandle, audio::InstanceHandle>> joins_;
	uint64_t pool_joins_at_start_ = 0;
	uint32_t last_step_ = UINT32_MAX;
	uint32_t grenades_ = 0;
	bool primary_picked_ = false;
	std::string primary_name_;
	std::vector<std::string> step_patches_;
	Phase phase_ = Phase::WaitSpawn;
	double first_s_ = -1.0, spawn_s_ = -1.0, record_s_ = -1.0;
	godot::Ref<godot::AudioEffectCapture> capture_;
	std::unique_ptr<audio::WavRecorder> mix_tap_;
	std::vector<float> heard_;
	int64_t discarded_ = 0;
	std::vector<audio::MixerCutEvent> events_;
	std::map<audio::InstanceHandle, AudioSoundFate> fates_;
	uint64_t unknown_handles_ = 0;
	audio::SoundFateLedger ledger_;
	std::vector<audio::MixerCutEvent> forwarded_;
	double now_s_ = 0.0;

	struct Snapshot {
		uint64_t takeover_cuts = 0, abrupt_gain = 0, limit_edge_steps = 0, overs_out = 0, held_triggers = 0,
				 event_drops = 0, pan_steps = 0;
		uint64_t late_blocks = 0, late_commands = 0, ring_overflows = 0, feedback_drops = 0, decode_errors = 0,
				 deferred_events = 0;
		uint64_t pool_stolen = 0, pool_forgotten = 0, pool_failed = 0, router_stolen = 0, router_forgotten = 0,
				 router_refusals = 0;
		uint64_t stale_events = 0;
	};
	static Snapshot snapshot(const Bf3GameAudio &p_audio);
	Snapshot at_start_;

	bool was_alive_ = false;
	double alive_s_ = -1.0;
	double last_deploy_s_ = -10.0;
	float base_yaw_ = 0.0f, yaw_ = 0.0f, pitch_ = 0.0f;
	bool have_view_ = false, firing_ = false;
	double last_s_ = 0.0;
	uint32_t bursts_ = 0, deaths_ = 0;
	AudioIngameLeg leg_ = AudioIngameLeg::Stand;
	double leg_s_ = -1.0;
	double tally_s_ = -1.0;
	uint32_t leg_index_ = 0;
	std::array<AudioIngameLegTally, kAudioIngameLegs> legs_{};
	uint64_t other_steps_ = 0;
	uint32_t stands_after_moving_ = 0;
	double last_step_s_ = -1.0;
	std::optional<sim::Vec3> last_feet_;
	std::optional<audio::FootstepParams> step_params_;
	float run_clear_min_m_ = kAudioIngameRunMeters;
	uint32_t run_picks_ = 0;
	bool spot_checked_ = false;
	std::optional<AudioIngameSpot> spot_;
	double spot_s_ = -1.0;
	uint32_t teleports_ = 0;
	double fight_after_s_ = 0.0;
	std::optional<AudioIngameSpot> fight_;
	uint32_t fights_ = 0;
	float spot_clear_m_ = 0.0f;
	GameAudioStats stats_at_start_;

	bool done_ = false;
	uint32_t rows_ = 0, failures_ = 0;
	std::vector<std::string> lines_;
};

}
