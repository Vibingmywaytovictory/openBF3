#pragma once

#include "game_frame.h"

#include "bf3/anim/crowd_eval.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/result.h"
#include "bf3/fx/triggers.h"
#include "bf3/render/crowd_weapons.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace godot {
class Node3D;
}

namespace bf3::godot_side {

struct GameCrowdStats {
	uint32_t lanes = 0;
	uint32_t soldiers = 0;
	uint32_t muzzles = 0;
	uint32_t unknown_weapons = 0;
	uint32_t weapon_instances = 0;
	uint32_t weapon_rebases = 0;
	uint32_t throwing = 0;
	uint64_t frames = 0;
	uint64_t frozen_frames = 0;
};

class Bf3GameCrowd {
public:
	Bf3GameCrowd();
	~Bf3GameCrowd();
	Bf3GameCrowd(const Bf3GameCrowd &) = delete;
	Bf3GameCrowd &operator=(const Bf3GameCrowd &) = delete;

	core::Status load(const GameLoadInputs &p_in);

	core::Status load_body(const core::InstallReader &p_reader, core::Cache *p_cache, const std::atomic<bool> *p_stop);
	bool loaded() const;

	void set_corpse_seconds(float p_seconds);
	bool build(godot::Node3D *p_parent, std::string *r_error);
	bool built() const;
	void step(const GameFrame &p_frame);

	std::span<const fx::MuzzleSample> muzzles() const;
	GameCrowdStats stats() const;
	void shutdown();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}
