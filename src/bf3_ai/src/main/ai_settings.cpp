#include "bf3/ai/main/ai_settings.h"

#include <algorithm>

namespace bf3::ai {

bool AiSettings::domain_enabled(AiDomain p_domain) const {
	switch (p_domain) {
		case AiDomain::Any:
		case AiDomain::Infantry:
			return true;
		case AiDomain::Vehicle:
			return enable_vehicle_ai;
		case AiDomain::Air:
			return enable_air_ai;
	}
	return false;
}

int AiSettings::vehicle(std::string_view p_name) const {
	const auto found = std::find(vehicles.begin(), vehicles.end(), p_name);
	return found != vehicles.end() ? int(found - vehicles.begin()) : -1;
}

void AiSettings::set_vehicle(const std::string &p_name) {
	vehicles.push_back(p_name);
}

const float *AiSettings::personality(int p_side) const {
	if (p_side < 1 || p_side > int(standard_personality.size())) {
		return nullptr;
	}
	const std::vector<float> &weights = standard_personality[size_t(p_side - 1)];
	return weights.empty() ? nullptr : weights.data();
}

}
