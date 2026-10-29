#pragma once

#include "bf3/ai/knowledge/ai_object.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::ai {

inline constexpr std::string_view kNoActiveLandingZone = "No valid zone is active";

struct AILandingZone {
	std::string name;
	Vec3 position{};
	int direction = 0;
};

class AILandingZoneManager {
public:
	AILandingZoneManager();

	void reset();
	int number_of_landing_zones() const { return int(zones_.size()); }
	AILandingZone *create_landing_zone(const std::string &p_name, const Vec3 &p_position, int p_direction);
	AILandingZone *landing_zone(std::string_view p_name) const;
	bool set_active_zone(std::string_view p_name);
	const std::string &active_zone_name() const { return active_zone_name_; }

private:
	std::map<std::string, AILandingZone *, std::less<>> zones_by_name_;
	std::vector<std::unique_ptr<AILandingZone>> zones_;
	std::string active_zone_name_;
};

}
