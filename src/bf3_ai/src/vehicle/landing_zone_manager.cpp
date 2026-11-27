// SOURCE: OpenBF2 Code/BF2/AI/AIDLL/AI/Main/AILandingZoneManager.cpp @c47317b (the inactive-zone name)
#include "bf3/ai/vehicle/landing_zone_manager.h"

namespace bf3::ai {

AILandingZoneManager::AILandingZoneManager() : active_zone_name_(kNoActiveLandingZone) {}

void AILandingZoneManager::reset() {
	zones_by_name_.clear();
	zones_.clear();
	active_zone_name_ = kNoActiveLandingZone;
}

// STAND-IN: a zone is a position and direction and a taken name is refused; the pin's has two corners, a height and a
// beach side and becomes the active zone. Settle: follow AILandingZoneManager.cpp @c47317b with the air AI.
AILandingZone *AILandingZoneManager::create_landing_zone(
		const std::string &p_name, const Vec3 &p_position, int p_direction) {
	if (zones_by_name_.count(p_name) != 0) {
		return nullptr;
	}
	auto zone = std::make_unique<AILandingZone>();
	zone->name = p_name;
	zone->position = p_position;
	zone->direction = p_direction;
	AILandingZone *const created = zone.get();
	zones_.push_back(std::move(zone));
	zones_by_name_.emplace(p_name, created);
	return created;
}

AILandingZone *AILandingZoneManager::landing_zone(std::string_view p_name) const {
	const auto found = zones_by_name_.find(p_name);
	return found != zones_by_name_.end() ? found->second : nullptr;
}

// STAND-IN: an unknown name keeps the active zone, where the pin's activeZone(name) clears it. Settle: follow
// AILandingZoneManager::activeZone @c47317b.
bool AILandingZoneManager::set_active_zone(std::string_view p_name) {
	if (landing_zone(p_name) == nullptr) {
		return false;
	}
	active_zone_name_ = std::string(p_name);
	return true;
}

}
