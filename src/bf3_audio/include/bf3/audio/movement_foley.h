#pragma once

#include "bf3/audio/audio_config.h"
#include "bf3/audio/footstep_driver.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/sim/soldier_state.h"

#include <cstdint>
#include <memory>
#include <type_traits>

namespace bf3::audio {

inline constexpr float kMovementSpeedStep = 0.01f;

struct MovementFoleyStats {
	uint64_t starts = 0;
	uint64_t stops = 0;
	uint64_t jumps = 0;
	uint64_t updates = 0;
	uint64_t ended = 0;
	uint64_t push_failures = 0;
	uint64_t create_failures = 0;
	float peak_speed = 0.0f;
};
static_assert(std::is_trivially_copyable_v<MovementFoleyStats>);

class MovementFoley {
public:
	MovementFoley(std::shared_ptr<const core::audio::SoundProgram> p_program,
			std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning);

	void step(MixerCore &r_mixer, const FootstepSoldier *p_own, const ListenerState &p_listener);
	void on_cut(const MixerCutEvent &p_event);
	void stop(MixerCore &r_mixer);
	InstanceHandle handle() const { return handle_; }
	const MovementFoleyStats &stats() const { return stats_; }

private:
	bool start(MixerCore &r_mixer, const FootstepSoldier &p_own, float p_speed, const ListenerState &p_listener);
	bool push(MixerCore &r_mixer, MixerCommandType p_type, uint32_t p_hash, float p_value);

	std::shared_ptr<const core::audio::SoundProgram> program_;
	std::shared_ptr<const core::audio::WaveBank> bank_;
	AudioTuning tuning_;
	MovementFoleyStats stats_;
	InstanceHandle handle_ = kNoInstance;
	uint16_t key_ = 0;
	float speed_ = 0.0f;
	bool first_person_ = false;
	sim::MoveMode mode_ = sim::MoveMode::OnGround;
};

}
