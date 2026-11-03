#pragma once

#include "bf3/core/audio/impulse_response.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/ids.h"
#include "bf3/core/result.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/util/fft.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bf3::audio {

inline constexpr uint32_t kIrPartitionFrames = 512;
inline constexpr uint32_t kIrFftSize = 2 * kIrPartitionFrames;
inline constexpr uint32_t kIrBins = kIrPartitionFrames + 1;
inline constexpr uint32_t kIrChannels = 2;
inline constexpr uint32_t kIrSlots = 2;
inline constexpr uint32_t kIrLanes = 4;
inline constexpr uint32_t kIrCrossfadeBlocks = 8;
inline constexpr uint32_t kNoImpulse = UINT32_MAX;

using Fft = core::Fft<float>;

struct PreparedImpulse {
	core::Guid partition;
	std::string name;
	uint32_t frames = 0;
	uint32_t source_frames = 0;
	uint32_t partitions = 0;
	std::vector<float> re;
	std::vector<float> im;

	const float *bins_re(uint32_t p_channel, uint32_t p_partition) const;
	const float *bins_im(uint32_t p_channel, uint32_t p_partition) const;
};

uint32_t max_reverb_partitions(float p_max_seconds, uint32_t p_rate);

core::Result<PreparedImpulse> prepare_impulse(
		const core::audio::ImpulseResponse &p_ir, uint32_t p_max_partitions, const Fft &p_fft);

struct ImpulseBank {
	uint32_t max_partitions = 0;
	std::vector<PreparedImpulse> impulses;

	std::optional<uint32_t> find(const core::Guid &p_partition) const;
};

struct ImpulseBankLoad {
	size_t assets = 0;
	size_t failed = 0;
	std::string notes;
};

core::Result<ImpulseBank> load_impulse_bank(const core::EbxDb &p_db, const core::LevelMount &p_mount,
		float p_max_seconds, uint32_t p_rate, ImpulseBankLoad *r_load = nullptr);

struct IrZone {
	uint32_t impulse = kNoImpulse;
	float amplitude = 0.0f;
};

struct IrSlot {
	uint32_t impulse = kNoImpulse;
	float amplitude = 0.0f;
	bool operator==(const IrSlot &) const = default;
};

class IrReverbController {
public:
	std::array<IrSlot, kIrSlots> choose(std::span<const IrZone> p_zones);
	const std::array<IrSlot, kIrSlots> &slots() const { return slots_; }
	void reset() { slots_ = {}; }

private:
	std::array<IrSlot, kIrSlots> slots_{};
};

struct IrReverbStats {
	uint64_t blocks = 0;
	uint64_t convolved_blocks = 0;
	uint64_t lane_blocks = 0;
	uint64_t switches = 0;
	uint64_t steals = 0;
	uint64_t refused = 0;
};

class IrReverb {
public:
	IrReverb();
	void configure(std::shared_ptr<const ImpulseBank> p_bank);
	bool active() const { return bank_ != nullptr && partitions_ > 0; }

	bool set_slot(uint32_t p_slot, uint32_t p_impulse, float p_amplitude);
	IrSlot slot(uint32_t p_slot) const;
	void process(float *r_left, float *r_right, uint32_t p_frames);
	void reset();
	const IrReverbStats &stats() const { return stats_; }

private:
	struct Lane {
		const PreparedImpulse *impulse = nullptr;
		uint32_t index = kNoImpulse;
		int32_t slot = -1;
		float gain = 0.0f;
		float target = 0.0f;
		float step = 0.0f;
		uint32_t ramp_left = 0;
		bool live() const { return impulse != nullptr; }
	};

	void push_input(const float *p_left, const float *p_right, bool p_silent);
	void render_lane(const Lane &p_lane, float *r_left, float *r_right);
	void retarget(Lane &r_lane, float p_target);

	std::shared_ptr<const ImpulseBank> bank_;
	Fft fft_;
	uint32_t partitions_ = 0;
	uint32_t head_ = 0;
	uint32_t silent_blocks_ = 0;
	bool previous_silent_ = true;
	std::vector<float> fdl_re_;
	std::vector<float> fdl_im_;
	std::array<float, kIrFftSize> work_re_{};
	std::array<float, kIrFftSize> work_im_{};
	std::array<std::array<float, kIrBins>, kIrChannels> acc_re_{};
	std::array<std::array<float, kIrBins>, kIrChannels> acc_im_{};
	std::array<std::array<float, kIrPartitionFrames>, kIrChannels> previous_{};
	std::array<std::array<float, kIrPartitionFrames>, kIrChannels> wet_{};
	std::array<Lane, kIrLanes> lanes_{};
	IrReverbStats stats_;
};

}
