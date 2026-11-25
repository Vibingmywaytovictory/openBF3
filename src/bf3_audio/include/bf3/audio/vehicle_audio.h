#pragma once

#include "bf3/audio/audio_config.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/patch_instance.h"
#include "bf3/core/audio/sound_program.h"
#include "bf3/core/audio/wave_bank.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace bf3::audio {

enum class VehicleSoundParam : uint8_t {
	Throttle,
	Rpm,
	Gear,
	Load,
	Velocity,
	AngularVelocity,
	RelativeVelocity,
	Slip,
	Tilt,
	Contact,
	Boost,
	SpringCompression,
	InteriorCam,
	LocalPlayerInVehicle,
	Health,
	DisabledDamageThreshold,
	InDriverPos,
	Roll,
	GForce,
	Count
};
inline constexpr size_t kVehicleSoundParams = size_t(VehicleSoundParam::Count);

uint32_t vehicle_sound_param_hash(VehicleSoundParam p_param);

inline constexpr uint16_t kNoVehicleProgram = 0xFFFF;

enum class VehicleSoundKind : uint8_t { Engine, Spin };

struct VehicleSoundSource {
	uint32_t key = 0;
	uint16_t program = kNoVehicleProgram;
	VehicleSoundKind kind = VehicleSoundKind::Engine;
	bool running = false;
	float start_rpm = 0.0f;
	float stop_rpm = 0.0f;
	float previous_rpm = 0.0f;
	EmitterState emitter;
	std::array<float, kVehicleSoundParams> params{};
};
static_assert(std::is_trivially_copyable_v<VehicleSoundSource>);

struct VehicleAudioStats {
	uint64_t starts = 0;
	uint64_t start_events = 0;
	uint64_t stops = 0;
	uint64_t ended = 0;
	uint64_t updates = 0;
	uint64_t push_failures = 0;
	uint64_t create_failures = 0;
	uint32_t live = 0;
	uint32_t peak_live = 0;
	uint64_t interior_frames = 0;
	float peak_velocity = 0.0f;
	float peak_throttle = 0.0f;
	float peak_rpm = 0.0f;
};
static_assert(std::is_trivially_copyable_v<VehicleAudioStats>);

class VehicleAudio {
public:
	VehicleAudio(std::vector<std::shared_ptr<const core::audio::SoundProgram>> p_programs,
			std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning);

	void step(MixerCore &r_mixer, std::span<const VehicleSoundSource> p_sources, const ListenerState &p_listener);
	void on_cut(const MixerCutEvent &p_event);
	void stop_all(MixerCore &r_mixer);
	const VehicleAudioStats &stats() const { return stats_; }
	size_t programs() const { return programs_.size(); }
	InstanceHandle handle_of(uint32_t p_key, uint16_t p_program) const;

private:
	struct Voice {
		uint32_t key = 0;
		uint16_t program = kNoVehicleProgram;
		InstanceHandle handle = kNoInstance;
		std::array<float, kVehicleSoundParams> sent{};
		bool seen = false;
		bool running = false;
	};

	struct Activation {
		uint32_t key = 0;
		bool running = false;
		bool seen = false;
		bool lost = false;
	};

	bool start(MixerCore &r_mixer, const VehicleSoundSource &p_source, const ListenerState &p_listener);
	bool post(MixerCore &r_mixer, Voice &r_voice, bool p_start);
	void update(MixerCore &r_mixer, Voice &r_voice, const VehicleSoundSource &p_source);
	void stop(MixerCore &r_mixer, Voice &r_voice);
	void mark_lost(uint32_t p_key, bool p_lost);
	bool push(MixerCore &r_mixer, MixerCommandType p_type, InstanceHandle p_handle, uint32_t p_hash, float p_value);

	std::vector<std::shared_ptr<const core::audio::SoundProgram>> programs_;
	std::vector<std::array<bool, kVehicleSoundParams>> connected_;
	std::shared_ptr<const core::audio::WaveBank> bank_;
	AudioTuning tuning_;
	std::vector<Voice> voices_;
	std::vector<Activation> activations_;
	std::vector<InstanceHandle> stopping_;
	VehicleAudioStats stats_;
};

}
