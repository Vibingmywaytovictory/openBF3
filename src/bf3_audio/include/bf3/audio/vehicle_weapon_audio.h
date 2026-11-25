#pragma once

#include "bf3/audio/audio_config.h"
#include "bf3/audio/mixer_core.h"
#include "bf3/audio/vehicle_weapon_sound.h"
#include "bf3/audio/weapon_audio.h"
#include "bf3/core/audio/wave_bank.h"
#include "bf3/present/presentation_events.h"
#include "bf3/present/presented.h"
#include "bf3/sim/vehicles/vehicle_weapon_rules.h"
#include "bf3/sim/weapons/sim_time.h"

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace bf3::audio {

class VehicleWeaponAudio {
public:
	VehicleWeaponAudio(MixerCore &r_mixer, std::shared_ptr<const WeaponAudioTable> p_table,
			std::shared_ptr<const core::audio::WaveBank> p_bank, const AudioTuning &p_tuning, uint32_t p_mix_rate,
			WeaponAudioStats &r_stats);
	~VehicleWeaponAudio();
	VehicleWeaponAudio(const VehicleWeaponAudio &) = delete;
	VehicleWeaponAudio &operator=(const VehicleWeaponAudio &) = delete;

	void set_crews(uint8_t p_own_team, std::span<const uint32_t> p_vehicles, std::span<const VehicleCrewSeat> p_seats,
			std::span<const VehicleWeaponPose> p_poses);
	void on_shot(const present::PresentEvent &p_event, uint8_t p_own_slot, bool p_own_first_person);
	void slot_gone(uint8_t p_slot);
	void step(const present::PresentClock &p_now, const ListenerState &p_listener);
	bool on_finished(InstanceHandle p_handle);

	void set_observer(WeaponAudioObserver *p_observer);
	void set_trace(bool p_on);
	void take_ops(std::vector<VehicleWeaponOp> &r_out);

private:
	struct Due {
		sim::weapons::SimTime time = 0;
		WeaponSoundOwner owner;
	};
	struct Weapon {
		uint64_t key = 0;
		uint32_t vehicle = 0;
		bool crewed = false;
		bool gone = false;
		uint8_t slot = 0xFF;
		uint16_t def = 0xFFFF;
		const WeaponAudioDef *sound = nullptr;
		WeaponSoundState state;
		InstanceHandle handle = kNoInstance;
		bool playing = false;
		std::vector<Due> due;
		bool pulling = false;
		sim::weapons::SimTime last_shot = 0;
		sim::vehicles::OverheatState heat;
		sim::weapons::SimTime heat_time = 0;
		bool heat_known = false;
		EmitterState emitter;
		bool emitter_dirty = false;
		bool pose_known = false;
		sim::weapons::SimTime pose_time = 0;
		WeaponSoundOwner last_owner;
		int32_t magazine = 0;
		bool reload_pending = false;
		bool reloading = false;
		sim::weapons::SimTime reload_at = 0;
		std::vector<std::pair<uint32_t, float>> sent;
	};
	struct Detached {
		InstanceHandle handle = kNoInstance;
		EmitterState emitter;
		bool dirty = false;
	};

	Weapon *weapon_of(uint64_t p_key);
	void heat_to(Weapon &r_w, sim::weapons::SimTime p_time);
	void fire(Weapon &r_w, const Due &p_due);
	void reload(Weapon &r_w, const present::PresentClock &p_now);
	void follow(Weapon &r_w, const present::PresentClock &p_now);
	bool create(Weapon &r_w, const ListenerState &p_listener);
	void apply(Weapon &r_w, std::span<const WeaponSoundOp> p_ops);
	void destroy(Weapon &r_w);
	bool push(MixerCommandType p_type, InstanceHandle p_handle, uint32_t p_hash, float p_value);

	MixerCore &mixer_;
	std::shared_ptr<const WeaponAudioTable> table_;
	std::shared_ptr<const core::audio::WaveBank> bank_;
	AudioTuning tuning_;
	uint32_t mix_rate_ = kMixRate;
	WeaponAudioStats &stats_;
	WeaponAudioObserver *observer_ = nullptr;
	uint8_t own_team_ = 0;
	bool crews_known_ = false;
	std::vector<uint32_t> vehicles_;
	std::vector<VehicleCrewSeat> seats_;
	std::vector<VehicleWeaponPose> poses_;
	std::vector<Weapon> weapons_;
	std::vector<Detached> detached_;
	std::vector<WeaponSoundOp> ops_;
	uint64_t updates_ = 0;
	uint32_t created_ = 0;
	bool trace_ = false;
	std::vector<VehicleWeaponOp> trace_ops_;
};

}
