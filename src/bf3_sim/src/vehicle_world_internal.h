#pragma once
// bf3_sim internal (Jolt stays PRIVATE to bf3_sim): VehicleWorld::Impl (docs/design/s2.md §6). The tests include it
// for the seams the public API does not offer: putting a vehicle to sleep with BodyInterface::DeactivateBody (the
// wake-rule test, s2.md §11) and reading Jolt's own SaveBodyState / VehicleConstraint::SaveState sizes.
//
// Layers: object layers 0..2 are the static layers (StaticLayer), 3 CollisionWorld's query layer (never a body's
// layer here), 4 kVehicleObjectLayer. Two broadphase layers: 0 "static" (the static bodies, built and optimised in
// create() exactly as CollisionWorld does), 1 "moving" (vehicles), so adding or moving a vehicle never touches the
// static tree. The vehicle layer collides with StaticLayer::Collision and with other vehicles only.
//
// Destruction order: the members are declared so that the system dies before the layer interfaces, the listener
// and the job system it references, and `runtime` last; ~VehicleWorld removes the constraints, step listeners and
// bodies first.

#include "bf3/sim/vehicle_world.h"

#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorder.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bf3::sim {

// The features of build_synthetic_vehicle_ground (s2.md §8.1), shared with synthetic_vehicle_courses
// (vehicle_course.cpp). Metres, relative to the ground's y; all away from the origin, where the straight lane runs.
namespace synthetic_ground {
// collision course: a 1.5 m box wall across +z
inline constexpr float kWallX0 = -20.0f, kWallX1 = 20.0f, kWallZ = 250.0f, kWallThickness = 1.0f, kWallHeight = 1.5f;
// bumps course along x = 200: six 0.15 m ridges (1 m long, 3 m apart), then a 0.6 m crest (the kicker)
inline constexpr float kBumpLineX = 200.0f, kBumpHalfWidth = 6.0f, kBumpZ0 = -40.0f, kBumpSpacing = 3.0f, kBumpLength = 1.0f, kBumpHeight = 0.15f;
inline constexpr size_t kBumpCount = 6;
inline constexpr float kKickerZ0 = 30.0f, kKickerRamp = 4.0f, kKickerTop = 2.0f, kKickerHeight = 0.6f;
// slope_idle: a 12 degree plane rising along +x over x 300..380, z -40..40
inline constexpr float kSlopeX0 = 300.0f, kSlopeX1 = 380.0f, kSlopeZ0 = -40.0f, kSlopeZ1 = 40.0f;
inline constexpr float kSlopeTan = 0.21255656167f; // tan 12 degrees
} // namespace synthetic_ground

// A StateRecorder over a byte vector (writes) or a byte span (reads): no stringstream, and a read past the end
// fails (zero-filled) instead of reading garbage.
class ByteStateRecorder final : public JPH::StateRecorder {
public:
	explicit ByteStateRecorder(std::vector<uint8_t> &r_out) :
			out_(&r_out) {}
	explicit ByteStateRecorder(std::span<const uint8_t> p_in) :
			in_(p_in) {}

	void WriteBytes(const void *p_data, size_t p_size) override;
	void ReadBytes(void *p_data, size_t p_size) override;
	bool IsEOF() const override { return at_ >= in_.size(); }
	bool IsFailed() const override { return failed_; }
	size_t consumed() const { return at_; }

private:
	std::vector<uint8_t> *out_ = nullptr;
	std::span<const uint8_t> in_;
	size_t at_ = 0;
	bool failed_ = false;
};

// Counts chassis contacts per slot (counts only: callback order cannot matter).
class VehicleContactCounter final : public JPH::ContactListener {
public:
	uint32_t static_bodies = 0;
	std::vector<VehicleContacts> *contacts = nullptr; // per slot

	void OnContactAdded(const JPH::Body &p_body1, const JPH::Body &p_body2, const JPH::ContactManifold &p_manifold, JPH::ContactSettings &) override {
		count(p_body1, p_body2, p_manifold);
	}
	void OnContactPersisted(const JPH::Body &p_body1, const JPH::Body &p_body2, const JPH::ContactManifold &p_manifold, JPH::ContactSettings &) override {
		count(p_body1, p_body2, p_manifold);
	}

private:
	void count(const JPH::Body &p_body1, const JPH::Body &p_body2, const JPH::ContactManifold &p_manifold);
};

// The save filter: every non-static body (statics never change).
class VehicleSaveFilter final : public JPH::StateRecorderFilter {
public:
	bool ShouldSaveBody(const JPH::Body &p_body) const override;
};

struct VehicleWorld::Impl {
	// Keeps the Jolt runtime alive while any system or shape of this world exists. Declared first: destroyed last.
	std::shared_ptr<void> runtime;
	std::shared_ptr<const StaticWorldShapes> shapes;
	VehicleWorldOptions options;

	std::unique_ptr<JPH::BroadPhaseLayerInterface> broadphase_layers;
	std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilter> object_vs_broadphase;
	std::unique_ptr<JPH::ObjectLayerPairFilter> object_pairs;
	std::unique_ptr<VehicleContactCounter> listener;
	VehicleSaveFilter save_filter;
	std::unique_ptr<JPH::TempAllocatorImpl> temp;
	std::unique_ptr<JPH::JobSystemSingleThreaded> jobs;
	std::unique_ptr<JPH::PhysicsSystem> system; // declared after what it references: dies first
	std::vector<JPH::BodyID> statics; // (tile, layer) order, = CollisionWorld's

	struct Vehicle {
		bool used = false;
		std::shared_ptr<const VehicleModel> model;
		JPH::BodyID body;
		JPH::Ref<JPH::VehicleConstraint> constraint;
		JPH::RefConst<JPH::VehicleCollisionTester> tester;
		VehicleDriverState driver;
	};
	std::vector<Vehicle> vehicles; // by slot, options.max_vehicles entries
	std::vector<VehicleContacts> contacts; // by slot, the last step's counts
	Tick tick = 0;

	// The used slots in slot order.
	std::vector<VehicleSlot> used_slots() const;
	// Jolt's own streams of one vehicle (PhysicsSystem::SaveBodyState, VehicleConstraint::SaveState).
	void save_body_stream(VehicleSlot p_slot, std::vector<uint8_t> &r_bytes) const;
	void save_constraint_stream(VehicleSlot p_slot, std::vector<uint8_t> &r_bytes) const;
	// The driver pass of one vehicle (s2.md §5.4): inputs, filters, reverse, stand-still brake, wake rule,
	// SetDriverInput, drag.
	void drive(VehicleSlot p_slot, const VehicleCommand &p_command);
};

} // namespace bf3::sim
