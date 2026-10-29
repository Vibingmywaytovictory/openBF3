#pragma once

#include "bf3/audio/dsp.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/core/audio/master_patch.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace bf3::audio {

inline constexpr float kStereoDownmix = 0.70710678f;

struct MasterMixStats {
	float compressor_min_db = 0.0f;
	uint64_t ceiling_clipped_samples = 0;
	uint64_t other_clipped_samples = 0;
};

class MasterMixer {
public:
	MasterMixer() = default;
	void configure(std::shared_ptr<const core::audio::MasterMixData> p_data, uint32_t p_rate, float p_iir2_q);
	bool select(uint32_t p_stereo_mode, uint32_t p_preset);
	bool active() const { return plan_ != nullptr; }
	const core::audio::MasterMixPlan *plan() const { return plan_; }

	void process(const BusBuffers &p_buses, const float *p_bus_gain, float *r_interleaved_stereo, uint32_t p_frames);
	const MasterMixStats &stats() const { return stats_; }
	void reset_stats() { stats_ = MasterMixStats{}; }

private:
	struct StepState {
		std::array<Biquad, core::audio::kSpeakerRoles> biquads;
		float gain_prev = -1.0f;
		float compressor_db = 0.0f;
	};
	using RoleBuffer = std::array<std::array<float, 512>, core::audio::kSpeakerRoles>;

	std::shared_ptr<const core::audio::MasterMixData> data_;
	const core::audio::MasterMixPlan *plan_ = nullptr;
	uint32_t rate_ = 48000;
	float iir2_q_ = 0.70710678f;
	float ceiling_ = 0.0f;
	std::vector<RoleBuffer> buffers_;
	std::vector<std::array<uint8_t, core::audio::kSpeakerRoles>> live_;
	std::vector<std::vector<StepState>> states_;
	MasterMixStats stats_;
};

}
