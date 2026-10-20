#pragma once

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

inline constexpr std::array<float, 6> kGameCheckDistances{ 5.0f, 20.0f, 100.0f, 150.0f, 280.0f, 320.0f };

struct GameAuditionItem {
	std::string name;
	std::string patch;
	std::vector<float> distances_m;

	std::vector<std::pair<float, float>> bands;
};

struct GameCheckStep {
	std::string name;
	float distance_m = 0.0f;
	uint32_t bursts = 2;
	bool audition = false;
	std::string patch;

	bool pin_distance_hpfs = false;
	std::vector<std::pair<float, float>> bands;
};

struct GameCheckScript {
	std::vector<GameCheckStep> steps;
	uint32_t rounds = 10;
	double settle_s = 1.0;
	double burst_period_s = 4.0;
	double timeout_s = 120.0;
	CheckScript analysis;
};

GameCheckScript game_check_script();
std::vector<GameAuditionItem> game_audition_items();

struct GameCheckWindow {
	uint32_t step = 0;
	uint64_t seed = 0;
	uint64_t first_frame = 0;
	uint64_t stop_frame = 0;
	float listener_distance_m = 0.0f;

	bool audition = false;
	bool stopped = false;
	float probe_hpf_hz = -1.0f;
	float probe_pitch = -1.0f;
	float probe_close = -1.0f;
	float probe_far = -1.0f;
	std::array<float, 4> band_amplitude{ -1.0f, -1.0f, -1.0f, -1.0f };
	uint32_t bands_found = 0;

	std::array<std::pair<float, float>, 4> band_fade_in{ { { -1.0f, -1.0f }, { -1.0f, -1.0f }, { -1.0f, -1.0f }, { -1.0f, -1.0f } } };
};

class GameAudioCheck final : public MixTap, public WeaponAudioObserver {
public:
	GameAudioCheck(const GameCheckScript &p_script, uint32_t p_rate, uint64_t p_start_frame);
	~GameAudioCheck() override;

	void set_shooter(uint8_t p_slot);
	void begin_step(uint32_t p_step, float p_listener_distance_m);

	void on_block(const float *p_interleaved_stereo, uint32_t p_frames, uint64_t p_first_frame) override;

	void on_driver_instance(uint8_t p_slot, uint64_t p_seed, uint64_t p_first_round_frame, PatchInstance &r_instance) override;
	void on_trigger_up(uint8_t p_slot, uint64_t p_seed, uint64_t p_stop_frame) override;

	void on_audition(uint32_t p_step, uint64_t p_seed, uint64_t p_frame, PatchInstance &r_instance);

	bool step_done(uint32_t p_step) const;

	void skip_step(uint32_t p_step, std::string p_reason);
	struct Skip {
		uint32_t step = 0;
		std::string reason;
	};
	std::span<const Skip> skipped() const;
	std::span<const GameCheckWindow> windows() const;
	core::Status write_wav(const std::filesystem::path &p_path) const;

	void set_guard(const core::PathGuard &p_guard);
	core::Status write_wav(const core::PathGuard &p_guard, const std::filesystem::path &p_path) const;
	const GameCheckScript &script() const;
	int64_t current_step() const;
	uint32_t rate() const;
	uint64_t start_frame() const;
	uint64_t total_frames() const;
	uint64_t recorded_until() const;

	std::span<const float> mix() const;
	std::span<const float> core_tap() const;
	std::span<const float> noise_tap() const;
	uint64_t dropped_frames() const;

private:
	struct State;
	std::unique_ptr<State> state_;
};

struct GameCheckCounters {
	uint64_t short_blocks = 0, late_blocks = 0, late_commands = 0;
	uint64_t mixer_refusals = 0;
};

CheckReport analyse_game_check(const GameCheckScript &p_script, const GameAudioCheck &p_check, const GameCheckCounters &p_counters,
		const AudioTuning &p_tuning, uint32_t p_rate);

}
