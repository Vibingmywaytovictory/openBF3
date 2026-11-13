#pragma once

#include "bf3/sim/vehicles/vehicle_obstacles.h"

#include <Jolt/Jolt.h>

#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <memory>
#include <vector>

namespace bf3::sim {

struct VehicleObstacles::Impl {
	std::shared_ptr<void> runtime;
	struct Proxy {
		std::shared_ptr<const VehicleModel> model;
		JPH::Ref<JPH::CharacterVirtual> character;
		bool active = false;
		Vec3 com{};
		Vec3 angular{};
	};
	std::vector<Proxy> proxies;
	JPH::CharacterVsCharacterCollisionSimple collision;

	void relist();
};

class ObstacleContacts final : public JPH::CharacterContactListener {
public:
	const VehicleObstacles::Impl *obstacles = nullptr;
	std::vector<ObstacleContact> contacts;

	void OnCharacterContactAdded(const JPH::CharacterVirtual *p_character, const JPH::CharacterContact &p_contact,
			JPH::CharacterContactSettings &r_settings) override;
	void OnCharacterContactPersisted(const JPH::CharacterVirtual *p_character, const JPH::CharacterContact &p_contact,
			JPH::CharacterContactSettings &r_settings) override;

private:
	void record(const JPH::CharacterContact &p_contact);
};

}
