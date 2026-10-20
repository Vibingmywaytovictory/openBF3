#pragma once

#include "bf3/audio/mixer_core.h"
#include "bf3/core/audio/sound_program.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace bf3::audio {

inline constexpr uint32_t kHashFire = 2088671869u;
inline constexpr uint32_t kHashIsFirstPerson = 824639024u;
inline constexpr uint32_t kHashReload = 3294210644u;
inline constexpr uint32_t kHashAreaType = 3772263626u;

enum class WeaponFireMode : uint8_t { Single, Burst, Automatic };

struct WeaponSoundBinding {
	std::shared_ptr<const core::audio::SoundProgram> program;
	uint32_t start_event = 0;
	uint32_t stop_event = 0;
	std::optional<uint32_t> step_event;
	std::optional<uint32_t> enable_step_parameter;
	double shot_interval_seconds = 0.0;
	uint32_t mix_rate = 0;
};

core::Result<WeaponSoundBinding> bind_weapon_sound(std::shared_ptr<const core::audio::SoundProgram> p_program, double p_shot_interval_seconds,
		uint32_t p_mix_rate);

class WeaponSoundDriver {
public:
	WeaponSoundDriver(const WeaponSoundBinding &p_binding, WeaponFireMode p_mode, InstanceHandle p_instance);

	void prepare(PatchInstance &r_instance, bool p_first_person) const;

	bool trigger_down(MixerCore &r_mixer, uint64_t p_first_frame);

	bool round_fired(MixerCore &r_mixer, uint32_t p_round);
	bool trigger_up(MixerCore &r_mixer, uint64_t p_frame);
	uint64_t round_frame(uint32_t p_round) const;

private:
	WeaponSoundBinding binding_;
	WeaponFireMode mode_ = WeaponFireMode::Automatic;
	InstanceHandle instance_ = kNoInstance;
	uint64_t first_frame_ = 0;
	bool held_ = false;
};

}
