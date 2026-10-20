#pragma once

#include "bf3/core/audio/sound_program.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace bf3::audio {

inline constexpr uint32_t kHashIsShortReload = 45329532u;
inline constexpr uint32_t kHashAction = 2484178491u;
inline constexpr uint32_t kHashActionType = 1114728387u;
inline constexpr uint32_t kHashBolt = 2088812592u;

enum class FoleyAction : uint8_t {
	Deploy = 0,
	Zoom = 1,
	DryFire = 2,
	BipodDeploy = 3,
	BipodUndeploy = 4,
	Light = 5,
	FireMode = 6,
};
inline constexpr uint8_t kFoleyActionCount = 7;
const char *foley_action_name(FoleyAction p_action);

inline constexpr float kFoleyUngated = std::numeric_limits<float>::infinity();

struct FoleyEvent {
	bool present = false;
	uint32_t hash = 0;
	uint16_t reg = core::audio::kNoRegister;
	std::vector<uint32_t> samplers;

	float reach_m = kFoleyUngated;

	uint32_t release_event = 0;

	std::vector<uint16_t> outputs;
};

struct WeaponFoley {
	FoleyEvent reload;
	FoleyEvent action;
	FoleyEvent bolt;
	bool short_reload_param = false;
	bool action_type_param = false;

	std::vector<std::string> action_waves;
	std::vector<std::string> reload_waves;
};

enum class FoleyCueKind : uint8_t {
	Reload,
	ReloadRelease,
	Action,
	Bolt,
};
struct FoleyCue {
	FoleyCueKind kind = FoleyCueKind::Reload;
	bool own = false;
	bool first_person = false;
	uint8_t slot = 0;
	uint16_t def = 0xFFFF;
	FoleyAction action = FoleyAction::Deploy;
	int8_t short_reload = -1;
	int64_t time = 0;
	uint64_t frame = 0;
	float distance_m = 0.0f;
};

FoleyEvent trace_foley_event(const core::audio::SoundProgram &p_program, uint32_t p_hash);

WeaponFoley read_weapon_foley(const core::audio::SoundProgram &p_program);

std::vector<uint16_t> voice_outputs(const core::audio::SoundProgram &p_program, uint16_t p_voice);

std::string describe_weapon_foley(const core::audio::SoundProgram &p_program, const WeaponFoley &p_foley);

}
