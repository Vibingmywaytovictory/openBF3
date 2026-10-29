#pragma once

#include "bf3/core/audio/sound_program.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::audio {

inline constexpr std::string_view kMasterPatchPartition = "sound/master";

enum class SpeakerRole : uint8_t { L, R, C, Ls, Rs, Lfe, Lb, Rb, Mono, Count };
inline constexpr size_t kSpeakerRoles = size_t(SpeakerRole::Count);
inline constexpr size_t kMasterAttributes = 8;

std::span<const SpeakerRole> speaker_layout(uint32_t p_channels);

enum class MasterStepKind : uint8_t {
	Pass,
	Gain,
	HighPass,
	LowPass,
	HighShelf,
	LowShelf,
	Peaking,
	Compressor,
	Clip,
	Send,
	Route,
	MatrixPan,
	Silence
};

struct MasterStep {
	MasterStepKind kind = MasterStepKind::Pass;
	std::array<float, kMasterAttributes> values{};
	uint16_t to_voice = kNoRegister;
	std::array<int8_t, kSpeakerRoles> map{};
};

struct MasterVoice {
	int8_t stage = 0;
	MasterBus bus = MasterBus::Unknown;
	uint8_t channels = 0;
	std::vector<MasterStep> steps;
};

struct MasterMixPlan {
	std::string preset;
	uint32_t speakers = 0;
	std::vector<MasterVoice> voices;
	std::vector<uint16_t> order;
	uint16_t output_voice = kNoRegister;
};

struct MasterMixData {
	std::vector<std::string> presets;
	std::array<std::vector<MasterMixPlan>, 2> plans;
	uint32_t default_preset = 0;
	uint32_t default_stereo_mode = 0;
	float low_pass_hz = 0.0f;

	const MasterMixPlan *plan(uint32_t p_stereo_mode, uint32_t p_preset) const;
};

Result<float> read_sound_state_low_pass(const EbxResolver &p_db, std::string_view p_state);

Result<MasterMixData> read_master_mix(
		const EbxResolver &p_db, float p_low_pass_hz, uint32_t p_default_preset, uint32_t p_default_stereo_mode);

}
