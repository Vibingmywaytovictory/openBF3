#pragma once
// Bf3GameCrowd: the other soldiers as 3p soldiers in the game (docs/design/m2a.md §9, R5, E18 (b); owner tp; W3.0
// contract, docs/design/m2a-w3.md). The S3 glue of s3_crowd.cpp (loader, team wiring, lanes, evaluator, uploader,
// step, shutdown) rebuilt for the level session: Bf3Crowd's members are private to the S3 check node, so this is a
// new class, and S3's files are not touched. A plain class owned by the level session (gx).
//   load   loader thread, no Godot API: bf3::render::load_soldier_assets for both teams, every kit item's 3p weapon
//          (anim::CrowdWeapon per team: binds, reload clip, weapon joints, muzzle joint), the crowd clips (stance via
//          lc's ServerPoseClipNames, sneak / crawl, fire, death)
//   build  main thread: the CrowdEvaluator on anim::crowd_lanes(hardware_concurrency, true) lanes (§3, §16 Q11: a
//          listen server; the 4-lane re-measure is the integrator's), the SkinUploader's meshes under p_parent
//   step   main thread, every display frame (m2a_game.h order: before the FX and the audio): M2aFrame::others and
//          other_weapons -> CrowdSoldierInput (team = anim::crowd_team_of_wire(team), weapon = the CrowdTeam::weapons
//          index of the def, fire_seq from burst_seq / firing changes, ads, dead from the life), publish, upload, kick;
//          then muzzles() for this frame
// Until tp lands, the session keeps drawing capsules (draw_others) and the FX / audio use fx::fallback_muzzle.

#include "m2a_game.h"

#include "bf3/anim/crowd_eval.h"
#include "bf3/core/result.h"
#include "bf3/fx/triggers.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace godot {
class Node3D;
} // namespace godot

namespace bf3::godot_side {

struct GameCrowdStats {
	uint32_t lanes = 0;
	uint32_t soldiers = 0; // drawn this frame
	uint32_t muzzles = 0; // valid muzzles this frame
	uint32_t unknown_weapons = 0; // a def with no CrowdWeapon for the team (drawn with weapon index 0)
	uint64_t frames = 0;
	uint64_t frozen_frames = 0; // the pose job was not done in time (published last frame's)
};

class Bf3GameCrowd {
public:
	Bf3GameCrowd();
	~Bf3GameCrowd(); // shutdown()
	Bf3GameCrowd(const Bf3GameCrowd &) = delete;
	Bf3GameCrowd &operator=(const Bf3GameCrowd &) = delete;

	core::Status load(const M2aLoadInputs &p_in);
	bool loaded() const;
	bool build(godot::Node3D *p_parent, std::string *r_error);
	bool built() const;
	void step(const M2aFrame &p_frame);
	// The 3p muzzles of the soldiers drawn in the last step (world, Frostbite axes; CrowdEvaluator::muzzle_model x the
	// soldier's root), one per drawn soldier with a weapon. Valid until the next step.
	std::span<const fx::MuzzleSample> muzzles() const;
	GameCrowdStats stats() const;
	void shutdown(); // waits for the pose job, frees the uploader's RIDs and nodes (idempotent; main thread)

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace bf3::godot_side
