#pragma once

#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/vehicle_model.h"
#include "bf3/sim/vehicle_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bf3::sim {

inline constexpr uint8_t kVehicleObjectLayer = 4;
inline constexpr uint32_t kMaxVehicleSlots = 64;

struct VehicleWorldOptions {
	uint32_t max_vehicles = 16;
	uint32_t temp_allocator_bytes = 16u << 20;
	uint32_t max_body_pairs = 65536;
	uint32_t max_contact_constraints = 16384;

	VehicleTuning tuning;
};

using VehicleSlot = uint32_t;

struct VehiclePlacement {
	Vec3 position{};
	std::array<float, 4> rotation{ 0, 0, 0, 1 };
};

namespace vehicle_restore {
inline constexpr uint32_t kBody = 1u << 0;
inline constexpr uint32_t kController = 1u << 1;
inline constexpr uint32_t kControllerInputs = 1u << 2;
inline constexpr uint32_t kWheels = 1u << 3;
inline constexpr uint32_t kWheelContacts = 1u << 4;
inline constexpr uint32_t kLambdas = 1u << 5;
inline constexpr uint32_t kPitchRoll = 1u << 6;
inline constexpr uint32_t kStep = 1u << 7;
inline constexpr uint32_t kDriver = 1u << 8;
inline constexpr uint32_t kBodySleep = 1u << 9;

inline constexpr uint32_t kGhost = kBody | kController | kWheels | kDriver;
inline constexpr uint32_t kAll = (1u << 10) - 1u;
}

enum class ContactCachePolicy : uint8_t {
	Keep = 0,
	Invalidate = 1,
};

struct VehicleContacts {
	uint8_t wheels_in_contact = 0;
	uint16_t chassis_static_contacts = 0;
	uint16_t chassis_vehicle_contacts = 0;
	float max_penetration = 0;
};

enum class VehicleSnapshotSection : uint8_t { Global = 0, Bodies = 1, Contacts = 2, Constraints = 3 };
inline constexpr size_t kVehicleSnapshotSectionCount = 4;
const char *vehicle_snapshot_section_name(VehicleSnapshotSection p_section);

struct VehicleWorldSnapshot {
	Tick tick = 0;
	std::vector<uint8_t> jolt;
	std::vector<VehicleDriverState> drivers;
	uint64_t hash = 0;
};

class VehicleWorld {
public:
	static core::Result<std::unique_ptr<VehicleWorld>> create(std::shared_ptr<const StaticWorldShapes> p_shapes,
			const VehicleWorldOptions &p_options = {});
	~VehicleWorld();
	VehicleWorld(const VehicleWorld &) = delete;
	VehicleWorld &operator=(const VehicleWorld &) = delete;

	const std::shared_ptr<const StaticWorldShapes> &shapes() const;
	size_t static_body_count() const;

	uint32_t static_body_id(size_t p_index) const;
	uint32_t vehicle_body_id(VehicleSlot p_slot) const;

	core::Status add_vehicle(VehicleSlot p_slot, std::shared_ptr<const VehicleModel> p_model, const VehiclePlacement &p_placement);
	bool has_vehicle(VehicleSlot p_slot) const;

	core::Status step(std::span<const VehicleSlot> p_slots, std::span<const VehicleCommand> p_commands);
	Tick tick() const;

	VehicleState state(VehicleSlot p_slot) const;
	VehicleContacts contacts(VehicleSlot p_slot) const;

	void save(VehicleWorldSnapshot &r_snapshot) const;

	void save_section(VehicleSnapshotSection p_section, std::vector<uint8_t> &r_bytes) const;

	core::Status restore(const VehicleWorldSnapshot &p_snapshot);

	core::Status restore_vehicle_state(VehicleSlot p_slot, const VehicleState &p_state, uint32_t p_groups, ContactCachePolicy p_contacts);
	uint64_t state_hash() const;

	struct Impl;
	Impl &impl() const { return *impl_; }

private:
	explicit VehicleWorld(std::unique_ptr<Impl> p_impl);
	std::unique_ptr<Impl> impl_;
};

struct SyntheticVehicleGround {
	float half_size = 512.0f;
	float y = 0.0f;
	bool features = true;
};
core::Result<std::shared_ptr<const StaticWorldShapes>> build_synthetic_vehicle_ground(const SyntheticVehicleGround &p_ground = {});

}
