#pragma once

#include "bf3/core/res/havok_ragdoll.h"
#include "bf3/core/result.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/static_world.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace bf3::sim {

inline constexpr uint8_t kRagdollObjectLayer = 4;
// INFERRED: the ragdoll steps in 60 Hz sub-steps. Settle: a note on the Havok ragdoll step rate.
inline constexpr float kRagdollStepSeconds = 1.0f / 60.0f;
inline constexpr uint32_t kMaxRagdollSubSteps = 8;

struct RagdollBodyPose {
	Vec3 position{};
	std::array<float, 4> rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
};

struct RagdollWorldOptions {
	uint32_t max_ragdolls = 64;
	uint32_t max_body_pairs = 16384;
	uint32_t max_contact_constraints = 4096;
	uint32_t temp_allocator_bytes = 16u << 20;
	// INFERRED: the ragdoll falls at 9.81 m/s^2. Settle: a note on the Havok world gravity.
	float gravity = 9.81f;
};

class RagdollWorld {
public:
	static core::Result<std::unique_ptr<RagdollWorld>> create(std::shared_ptr<const StaticWorldShapes> p_shapes,
			std::shared_ptr<const core::res::RagdollDef> p_def, const RagdollWorldOptions &p_options = {});
	~RagdollWorld();
	RagdollWorld(const RagdollWorld &) = delete;
	RagdollWorld &operator=(const RagdollWorld &) = delete;

	const core::res::RagdollDef &def() const;
	size_t body_count() const;
	std::span<const uint32_t> parents() const;

	core::Status add(uint32_t p_id, std::span<const RagdollBodyPose> p_bodies,
			std::span<const Vec3> p_linear_velocities, std::span<const Vec3> p_angular_velocities);
	bool has(uint32_t p_id) const;
	void remove(uint32_t p_id);
	void freeze(uint32_t p_id);
	bool frozen(uint32_t p_id) const;
	size_t ragdoll_count() const;

	core::Status step(float p_dt);

	bool pose(uint32_t p_id, std::span<RagdollBodyPose> r_bodies) const;
	float max_linear_speed(uint32_t p_id) const;
	float max_angular_speed(uint32_t p_id) const;

	struct Impl;

private:
	explicit RagdollWorld(std::unique_ptr<Impl> p_impl);
	std::unique_ptr<Impl> impl_;
};

}
