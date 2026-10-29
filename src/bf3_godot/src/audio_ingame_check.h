#pragma once

#include "bf3/audio/mixer_core.h"
#include "bf3/audio/wav_io.h"
#include "bf3/client_audio/sound_fate_ledger.h"

#include <godot_cpp/classes/audio_effect_capture.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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

enum class AudioIngamePhase : uint8_t { Mix, Pool };

bool parse_audio_ingame_phase(std::string_view p_text, AudioIngamePhase &r_phase);

inline constexpr double kAudioIngameGhostSeconds = 1.0;
inline constexpr double kAudioIngameLimitedBlocksShare = 0.01;
inline constexpr float kAudioIngameLimitFloorDb = -1.0f;

struct AudioIngameIo {
	bool alive = false;
	float time_to_respawn = 0.0f;
	std::function<void(bool)> hold_fire;
	std::function<void()> reload;
	std::function<void()> grenade;
	std::function<bool(std::span<const std::string>)> pick_primary;
	std::function<void()> deploy;
	std::function<void(float, float)> view;
	std::function<float()> yaw;
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
	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);
	void mix_row(const std::string &p_name, bool p_pass, const std::string &p_detail);

	std::string out_dir_;
	double seconds_ = kAudioIngameSeconds;
	AudioIngamePhase kind_ = AudioIngamePhase::Mix;
	audio::MixerLevels levels_at_start_;
	std::map<audio::InstanceHandle, double> refused_;
	std::map<audio::InstanceHandle, float> sound_peaks_;
	uint64_t pool_joins_at_start_ = 0;
	uint32_t last_step_ = UINT32_MAX;
	uint32_t grenades_ = 0;
	bool primary_picked_ = false;
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

	bool done_ = false;
	uint32_t rows_ = 0, failures_ = 0;
	std::vector<std::string> lines_;
};

}
