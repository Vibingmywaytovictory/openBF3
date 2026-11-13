#include "vehicle_obstacles_internal.h"

#include "bf3/core/util/finite.h"
#include "bf3/sim/vehicle_world.h"

#include "../jolt_convert.h"
#include "../jolt_runtime.h"
#include "../vehicle_world_internal.h"

#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollisionDispatch.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <string>

namespace bf3::sim {

using core::finite;

VehicleObstacles::VehicleObstacles() : impl_(std::make_unique<Impl>()) {
	impl_->runtime = jolt_runtime_acquire();
}

VehicleObstacles::~VehicleObstacles() = default;

void VehicleObstacles::Impl::relist() {
	collision.mCharacters.clear();
	for (Proxy &p : proxies) {
		if (p.active) {
			collision.mCharacters.push_back(p.character.GetPtr());
		}
	}
}

core::Status VehicleObstacles::place(
		uint32_t p_slot, const std::shared_ptr<const VehicleModel> &p_model, const VehicleBodyState &p_body) {
	Impl &o = *impl_;
	const std::string where = "vehicle obstacles: slot " + std::to_string(p_slot);
	if (p_slot >= kMaxVehicleSlots) {
		return core::make_error(where + " outside 0.." + std::to_string(kMaxVehicleSlots - 1));
	}
	if (p_model == nullptr || p_model->chassis.empty()) {
		return core::make_error(where + ": no usable model");
	}
	if (!finite(p_body.com_position) || !finite(p_body.rotation) || !finite(p_body.linear_velocity) ||
			!finite(p_body.angular_velocity)) {
		return core::make_error(where + ": non-finite body state");
	}
	if (p_slot >= o.proxies.size()) {
		o.proxies.resize(size_t(p_slot) + 1u);
	}
	Impl::Proxy &p = o.proxies[p_slot];
	if (p.character == nullptr || p.model != p_model) {
		core::Result<JPH::RefConst<JPH::Shape>> shape = vehicle_chassis_shape(*p_model);
		if (!shape) {
			return core::make_error(where + ": " + shape.error().message);
		}
		JPH::CharacterVirtualSettings settings;
		settings.mShape = *shape;
		settings.mUp = JPH::Vec3::sAxisY();
		settings.mCharacterPadding = 0.0f;
		p.character =
				new JPH::CharacterVirtual(&settings, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), p_slot, nullptr);
		p.model = p_model;
		p.active = false;
	}
	if (!p.active) {
		p.active = true;
		o.relist();
	}
	const JPH::Quat q = to_jolt(p_body.rotation).Normalized();
	p.character->SetRotation(q);
	p.character->SetPosition(JPH::RVec3(to_jolt(p_body.com_position) - q * p.character->GetShape()->GetCenterOfMass()));
	p.character->SetLinearVelocity(to_jolt(p_body.linear_velocity));
	p.com = p_body.com_position;
	p.angular = p_body.angular_velocity;
	return core::Status::success();
}

void VehicleObstacles::remove(uint32_t p_slot) {
	Impl &o = *impl_;
	if (p_slot >= o.proxies.size() || !o.proxies[p_slot].active) {
		return;
	}
	o.proxies[p_slot].active = false;
	o.relist();
}

void VehicleObstacles::clear() {
	for (Impl::Proxy &p : impl_->proxies) {
		p.active = false;
	}
	impl_->relist();
}

size_t VehicleObstacles::count() const {
	return impl_->collision.mCharacters.size();
}

bool VehicleObstacles::holds(uint32_t p_slot) const {
	return p_slot < impl_->proxies.size() && impl_->proxies[p_slot].active;
}

bool VehicleObstacles::overlaps_capsule(const Vec3 &p_feet, float p_radius, float p_height) const {
	if (!(p_radius > 0.0f) || !(p_height >= 2.0f * p_radius) || !finite(p_feet)) {
		return false;
	}
	const float half = 0.5f * p_height - p_radius;
	JPH::RefConst<JPH::Shape> soldier;
	if (half > 1e-6f) {
		soldier = new JPH::CapsuleShape(half, p_radius);
	} else {
		soldier = new JPH::SphereShape(p_radius);
	}
	const JPH::Mat44 at = JPH::Mat44::sTranslation(to_jolt(p_feet) + JPH::Vec3(0.0f, 0.5f * p_height, 0.0f));
	JPH::CollideShapeSettings settings;
	settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
	for (const Impl::Proxy &p : impl_->proxies) {
		if (!p.active) {
			continue;
		}
		JPH::AnyHitCollisionCollector<JPH::CollideShapeCollector> hit;
		JPH::CollisionDispatch::sCollideShapeVsShape(soldier.GetPtr(), p.character->GetShape(), JPH::Vec3::sOne(),
				JPH::Vec3::sOne(), at, JPH::Mat44(p.character->GetCenterOfMassTransform()), JPH::SubShapeIDCreator(),
				JPH::SubShapeIDCreator(), settings, hit);
		if (hit.HadHit()) {
			return true;
		}
	}
	return false;
}

void ObstacleContacts::OnCharacterContactAdded(
		const JPH::CharacterVirtual *, const JPH::CharacterContact &p_contact, JPH::CharacterContactSettings &) {
	record(p_contact);
}

void ObstacleContacts::OnCharacterContactPersisted(
		const JPH::CharacterVirtual *, const JPH::CharacterContact &p_contact, JPH::CharacterContactSettings &) {
	record(p_contact);
}

void ObstacleContacts::record(const JPH::CharacterContact &p_contact) {
	if (obstacles == nullptr || p_contact.mCharacterB == nullptr) {
		return;
	}
	for (const VehicleObstacles::Impl::Proxy &p : obstacles->proxies) {
		if (!p.active || p.character.GetPtr() != p_contact.mCharacterB) {
			continue;
		}
		const JPH::Vec3 at(p_contact.mPosition);
		ObstacleContact c;
		c.slot = uint32_t(p.character->GetUserData());
		c.position = from_jolt(at);
		c.normal = from_jolt(p_contact.mContactNormal);
		c.velocity = from_jolt(p.character->GetLinearVelocity() + to_jolt(p.angular).Cross(at - to_jolt(p.com)));
		contacts.push_back(c);
		return;
	}
}

}
