#pragma once

#include "bf3/core/audio/sound_program.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bf3::core::audio {

inline constexpr size_t kMaxMixerAssetNodes = 1024;
inline constexpr size_t kMaxMixerAssetEntries = 1024;

enum class MixerAccumulate : uint8_t { None, Max };

enum class MixerNodeKind : uint8_t { Input, SimpleTransform, MinMaxValueSelector, ScaleClamp, Output };

struct MixerAssetInput {
	uint32_t name_hash = 0;
	float default_value = 0.0f;
	MixerAccumulate accumulate = MixerAccumulate::None;
	uint16_t out = kNoRegister;
};

struct MixerAssetOutput {
	uint32_t name_hash = 0;
	SoundPort value;
};

struct MixerAssetNode {
	MixerNodeKind kind = MixerNodeKind::Input;
	uint8_t option = 0;
	std::array<SoundPort, 5> in{};
	std::array<uint16_t, 4> out{ kNoRegister, kNoRegister, kNoRegister, kNoRegister };
	uint32_t entries_begin = 0;
	uint32_t entries_count = 0;
};

struct MixerAsset {
	std::string name;
	Guid partition;
	uint32_t registers = 0;
	std::vector<MixerAssetInput> inputs;
	std::vector<MixerAssetOutput> outputs;
	std::vector<MixerAssetNode> nodes;
	std::vector<SoundPort> entries;

	std::optional<size_t> input_of(uint32_t p_name_hash) const;
	std::optional<size_t> output_of(uint32_t p_name_hash) const;

	float accumulate(size_t p_input, std::span<const float> p_contributions) const;

	void evaluate(std::span<const float> p_inputs, std::span<float> r_registers, std::span<float> r_outputs) const;
};

Result<MixerAsset> read_mixer_asset(const EbxResolver &p_db, const EbxPartition &p_partition);

}
