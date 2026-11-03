#pragma once

#include "bf3/audio/ir_reverb.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/sim/logic/ir_reverb_natives.h"
#include "bf3/sim/logic/sound_area_natives.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bf3::audio {

struct AreaFeedStats {
	uint64_t steps = 0;
	uint64_t posts = 0;
	uint64_t push_failures = 0;
	bool posted = false;
	float area_type = 0.0f;
};

class AreaFeed {
public:
	void step(MixerCore &r_mixer, const sim::logic::SoundAreaNatives *p_areas);
	const AreaFeedStats &stats() const { return stats_; }

private:
	AreaFeedStats stats_;
};

struct IrReverbFeedStats {
	uint64_t steps = 0;
	uint64_t posts = 0;
	uint64_t push_failures = 0;
	size_t zones = 0;
	size_t unmatched = 0;
	std::array<IrSlot, kIrSlots> slots{};
};

class IrReverbFeed {
public:
	void step(MixerCore &r_mixer, const sim::logic::IrReverbNatives *p_reverbs, const ImpulseBank *p_bank);
	const IrReverbFeedStats &stats() const { return stats_; }

private:
	const sim::logic::IrReverbNatives *bound_ = nullptr;
	const ImpulseBank *bound_bank_ = nullptr;
	std::vector<uint32_t> impulse_of_;
	std::vector<IrZone> zones_;
	IrReverbController controller_;
	std::array<IrSlot, kIrSlots> posted_{};
	std::array<bool, kIrSlots> sent_{};
	IrReverbFeedStats stats_;
};

}
