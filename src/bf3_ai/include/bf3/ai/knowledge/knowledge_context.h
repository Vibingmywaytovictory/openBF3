#pragma once

#include "bf3/ai/knowledge/ai_object.h"

namespace bf3::ai {

class InformationFactory;
class InformationManager;
class WeaponInfoManager;

// SOURCE: OpenBF2 Code/BF2/AI/AIDLL/AI/Main/AISettings.cpp @c47317b (6 battle and 2 strategic strengths).
inline constexpr int kBattleStrengthCount = 6;
inline constexpr int kStrategicStrengthCount = 2;

// WHY: OpenBF2's timer and information singletons are injected here; game time is in sim ticks, not seconds.
class AiTimer {
public:
	virtual ~AiTimer() = default;
	virtual sim::Tick game_tick() const = 0;
	virtual double tick_seconds() const = 0;
	virtual double update_time() const = 0;
};

inline double game_time(const AiTimer *p_timer) {
	return p_timer != nullptr ? double(p_timer->game_tick()) * p_timer->tick_seconds() : 0.0;
}

class AltitudeSource {
public:
	virtual ~AltitudeSource() = default;
	virtual float altitude(const Vec3 &p_position) const = 0;
};

class InformationEnvironmentModifiers {
public:
	virtual ~InformationEnvironmentModifiers() = default;
	virtual void add_and_update_position(int p_side, bool p_unit, uint32_t p_handle) = 0;
};

struct KnowledgeContext {
	InformationManager *information = nullptr;
	WeaponInfoManager *weapons = nullptr;
	const InformationFactory *factory = nullptr;
	const AiTimer *timer = nullptr;
	const AltitudeSource *altitude = nullptr;
	InformationEnvironmentModifiers *environment = nullptr;
	int infantry_map = 0;

	sim::Tick now() const { return timer ? timer->game_tick() : 0; }
	double tick_seconds() const { return timer ? timer->tick_seconds() : 0.0; }
};

enum class PlugInType : int { Physical = 0, Cover = 1, Mobile = 2, Unit = 3, Armament = 4, ControlInfo = 5 };

enum class PlugInRequest : int { Armament = 0, ControlInfo = 1, Cover = 2, Mobile = 3, Physical = 4, Unit = 5 };

}
