#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace bf3::sim {

struct WorldConfig {
	int dynamic_bodies = 8;
	int job_threads = 2;
	uint32_t max_bodies = 1024;
};

struct BodyTransform {
	float position[3] = {};
	float rotation[4] = { 0, 0, 0, 1 };
	uint8_t shape = 0;
	float half_extent = 0.5f;
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

	bool step();
	uint64_t tick() const;

	uint64_t state_hash() const;
	int dynamic_body_count() const;
	int active_body_count() const;
	float lowest_dynamic_y() const;
	void get_dynamic_transforms(std::vector<BodyTransform> &r_out) const;

	static const char *jolt_version();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
