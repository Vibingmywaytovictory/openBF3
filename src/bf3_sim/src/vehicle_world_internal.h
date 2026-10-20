#pragma once

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

namespace synthetic_ground {

inline constexpr float kWallX0 = -20.0f, kWallX1 = 20.0f, kWallZ = 250.0f, kWallThickness = 1.0f, kWallHeight = 1.5f;

inline constexpr float kBumpLineX = 200.0f, kBumpHalfWidth = 6.0f, kBumpZ0 = -40.0f, kBumpSpacing = 3.0f, kBumpLength = 1.0f, kBumpHeight = 0.15f;
inline constexpr size_t kBumpCount = 6;
inline constexpr float kKickerZ0 = 30.0f, kKickerRamp = 4.0f, kKickerTop = 2.0f, kKickerHeight = 0.6f;

inline constexpr float kSlopeX0 = 300.0f, kSlopeX1 = 380.0f, kSlopeZ0 = -40.0f, kSlopeZ1 = 40.0f;
inline constexpr float kSlopeTan = 0.21255656167f;
}

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

class VehicleContactCounter final : public JPH::ContactListener {
public:
	uint32_t static_bodies = 0;
	std::vector<VehicleContacts> *contacts = nullptr;

	void OnContactAdded(const JPH::Body &p_body1, const JPH::Body &p_body2, const JPH::ContactManifold &p_manifold, JPH::ContactSettings &) override {
		count(p_body1, p_body2, p_manifold);
	}
	void OnContactPersisted(const JPH::Body &p_body1, const JPH::Body &p_body2, const JPH::ContactManifold &p_manifold, JPH::ContactSettings &) override {
		count(p_body1, p_body2, p_manifold);
	}

private:
	void count(const JPH::Body &p_body1, const JPH::Body &p_body2, const JPH::ContactManifold &p_manifold);
};

class VehicleSaveFilter final : public JPH::StateRecorderFilter {
public:
	bool ShouldSaveBody(const JPH::Body &p_body) const override;
};

struct VehicleWorld::Impl {

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
	std::unique_ptr<JPH::PhysicsSystem> system;
	std::vector<JPH::BodyID> statics;

	struct Vehicle {
		bool used = false;
		std::shared_ptr<const VehicleModel> model;
		JPH::BodyID body;
		JPH::Ref<JPH::VehicleConstraint> constraint;
		JPH::RefConst<JPH::VehicleCollisionTester> tester;
		VehicleDriverState driver;
	};
	std::vector<Vehicle> vehicles;
	std::vector<VehicleContacts> contacts;
	Tick tick = 0;

	std::vector<VehicleSlot> used_slots() const;

	void save_body_stream(VehicleSlot p_slot, std::vector<uint8_t> &r_bytes) const;
	void save_constraint_stream(VehicleSlot p_slot, std::vector<uint8_t> &r_bytes) const;

	void drive(VehicleSlot p_slot, const VehicleCommand &p_command);
};

}
