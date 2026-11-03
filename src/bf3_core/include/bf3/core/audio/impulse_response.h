#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"
#include "bf3/core/superbundle_mount.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bf3::core::audio {

inline constexpr uint32_t kIrfMagic = 0x69726631u;
inline constexpr uint32_t kMaxImpulseChannels = 8;
inline constexpr uint32_t kMaxImpulseFrames = 48000u * 30u;
inline constexpr size_t kIrfHeaderBytes = 48;
inline constexpr size_t kIrfBlockHeaderBytes = 16;

struct ImpulseResponseHeader {
	uint32_t version = 0;
	uint32_t channels = 0;
	uint32_t frames = 0;
	uint32_t rate = 0;
	uint32_t block = 0;
	uint32_t bins = 0;
	uint32_t bin_values = 0;
	uint32_t layout = 0;

	size_t block_bytes() const;
	size_t chunk_bytes() const;
};

struct ImpulseResponse {
	Guid partition;
	std::string name;
	ImpulseResponseHeader header;
	uint32_t channels = 0;
	uint32_t rate = 0;
	uint32_t frames = 0;
	std::vector<float> samples;

	std::span<const float> channel(uint32_t p_channel) const;
};

Result<ImpulseResponseHeader> read_impulse_header(std::span<const uint8_t> p_chunk);
Result<ImpulseResponse> decode_impulse_response(std::span<const uint8_t> p_chunk);

Result<ImpulseResponse> read_impulse_response(const EbxInstance &p_asset, const LevelMount &p_mount);

struct LibraryImpulse {
	std::string name;
	Result<ImpulseResponse> ir;
};

std::vector<LibraryImpulse> read_library_impulses(const EbxLibrary &p_library);

}
