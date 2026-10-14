#pragma once
// Our own Jolt world (plan §2.3, §2.5): the server simulates in it, never in Godot's physics.
// S0 content: a static ground box and a few dynamic boxes and spheres, stepped at a fixed 60 Hz.
// Jolt is built with CROSS_PLATFORM_DETERMINISTIC, so equal inputs give bit-equal states.

#include <cstdint>
#include <memory>
#include <vector>

namespace bf3::sim {

struct WorldConfig {
	int dynamic_bodies = 8;
	int job_threads = 2; // 0 = single-threaded job system (no worker threads)
	uint32_t max_bodies = 1024;
};

struct BodyTransform {
	float position[3] = {};
	float rotation[4] = { 0, 0, 0, 1 }; // x, y, z, w
	uint8_t shape = 0; // 0 = box, 1 = sphere
	float half_extent = 0.5f; // box half size or sphere radius (m)
};

class PhysicsWorld {
public:
	static constexpr int kTickHz = 60;
	static constexpr float kStepSeconds = 1.0f / kTickHz;

	PhysicsWorld();
	~PhysicsWorld();
	PhysicsWorld(const PhysicsWorld &) = delete;
	PhysicsWorld &operator=(const PhysicsWorld &) = delete;

	bool init(const WorldConfig &p_config);
	void shutdown();
	bool is_initialized() const;

	// One fixed step of kStepSeconds. Returns false if Jolt reported an update error.
	bool step();
	uint64_t tick() const;

	// FNV-1a over position, rotation and velocities of every dynamic body, in creation order.
	uint64_t state_hash() const;
	int dynamic_body_count() const;
	int active_body_count() const;
	float lowest_dynamic_y() const;
	void get_dynamic_transforms(std::vector<BodyTransform> &r_out) const;

	static const char *jolt_version(); // "5.6.0"

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::sim
