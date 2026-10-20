#pragma once

#include "bf3/audio/audio_analysis.h"
#include "bf3/audio/audio_config.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/audio/weapon_sound.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::audio {

inline constexpr std::string_view kM16Patch = "sound/weapons/handheld/m16a3/weapon_assaultrifle_m16a4_02";
inline constexpr core::audio::SoundPluginRef kM16NoiseHpf{ 14, 2 };
inline constexpr core::audio::SoundPluginRef kM16CoreHpf{ 6, 1 };
inline constexpr std::array<uint16_t, 4> kM16CoreTapVoices{ 26, 27, 28, 29 };
inline constexpr std::array<uint16_t, 1> kM16NoiseTapVoices{ 14 };

struct CheckSegment {
	std::string name;
	float distance_m = 0.0f;
	bool first_person = false;
	uint32_t bursts = 1;
	uint64_t seed = 0;
	bool pin_distance_hpfs = false;
	bool pin_noise_hpf = false;
};

struct CheckScript {
	std::vector<CheckSegment> segments;
	uint32_t rounds = 10;
	double shot_interval_s = 0.075;
	double stop_after_last_round_s = 0.0375;
	double burst_period_s = 4.0;
	double start_s = 0.25;
	WeaponFireMode fire_mode = WeaponFireMode::Automatic;
	double expected_spacing_frames = 3600.0;

	double spacing_tolerance_frames = 96.0;
};

CheckScript m16_check_script();

struct ScheduledBurst {
	uint32_t segment = 0;
	uint32_t index = 0;
	uint64_t seed = 0;
	uint64_t first_round_frame = 0;
	uint64_t stop_frame = 0;
};
std::vector<ScheduledBurst> schedule_check(const CheckScript &p_script, uint32_t p_rate);

class TapRecorder final : public VoiceTap {
public:
	TapRecorder(uint64_t p_start_frame, uint64_t p_frames);
	void on_voices(const float *p_mono, uint32_t p_frames, uint64_t p_first_frame) override;
	std::span<const float> samples() const;
	uint64_t dropped_frames() const;

private:
	uint64_t start_ = 0;
	std::vector<float> buffer_;
	uint64_t dropped_ = 0;
};

class CheckDriver {
public:
	static core::Result<std::unique_ptr<CheckDriver>> create(const CheckScript &p_script, std::shared_ptr<const core::audio::SoundProgram> p_program,
			std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning, uint64_t p_start_frame, uint32_t p_rate);
	~CheckDriver();
	core::Status pump(MixerCore &r_mixer);
	bool done(const MixerCore &p_mixer) const;
	uint64_t start_frame() const;
	uint64_t total_frames() const;
	std::span<const ScheduledBurst> bursts() const;
	const TapRecorder &core_tap() const;
	const TapRecorder &noise_tap() const;
	std::span<const SamplerTrace> traces() const;

	float far_noise_hpf_hz() const;

private:
	CheckDriver() = default;
	struct State;
	std::unique_ptr<State> state_;
};

struct CheckRow {
	std::string name;
	double value = 0.0;
	std::string rule;
	bool pass = false;
};
struct CheckReport {
	std::vector<CheckRow> rows;
	bool pass() const;
	std::string table() const;
};

CheckReport analyse_check(const CheckScript &p_script, std::span<const ScheduledBurst> p_bursts, std::span<const float> p_mix_stereo,
		std::span<const float> p_core_tap, std::span<const float> p_noise_tap, float p_far_noise_hpf_hz, uint32_t p_rate, const AudioTuning &p_tuning);

struct OfflineCheckOptions {
	uint64_t start_frame = 0;
	std::vector<uint32_t> call_frames{ kBlockFrames };
	MixerConfig mixer;
};
struct OfflineCheck {
	std::vector<ScheduledBurst> bursts;
	uint64_t start_frame = 0;
	uint64_t total_frames = 0;
	std::vector<float> mix;
	std::vector<float> core_tap, noise_tap;
	std::vector<SamplerTrace> traces;
	float far_noise_hpf_hz = 0.0f;
	uint64_t short_blocks = 0, late_commands = 0, ring_overflows = 0, starved_voices = 0, decode_errors = 0, deferred_events = 0;
	uint64_t tap_dropped = 0, trace_dropped = 0, record_dropped = 0;
	CheckReport report;
};
core::Result<OfflineCheck> run_check_offline(const CheckScript &p_script, std::shared_ptr<const core::audio::SoundProgram> p_program,
		std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning, const OfflineCheckOptions &p_options = {});

void add_model_trace_rows(CheckReport &r_report, const OfflineCheck &p_check, const CheckScript &p_script, const core::audio::SoundProgram &p_program,
		const core::audio::WaveBank &p_bank, uint32_t p_rate);

}
