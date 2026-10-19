#pragma once
// Bf3DeathCamera: the own death's view (docs/design/m2b.md §4, §2.6 #17; owner cam; W2.0 contract,
// docs/design/m2b-w2.md). After the own death the camera leaves the 1p eye, pulls back and up from the corpse
// (orbit_seconds), then, when MatchState::killer_camera (the Setting "BFServer.IsKillerCameraEnabled") and the killer
// is drawn, turns to the killer's presented position and holds it until kKillCardSeconds after the death; then the
// game shows the spawn screen (gx). A revive or a spawn ends it. Without a known killer (a fall, a suicide, the killer
// not drawn) it stays on the corpse.
//
// A plain Godot-free class owned by the M2b game (gx): positions and the result are in Frostbite world axes (+Y up,
// the fx::Transform convention of M2aFrame::camera), so m2b.contract2 compiles it and cam's rows test it without
// Godot; gx copies the transform onto its Camera3D. The camera never enters the static world: with a CollisionWorld it
// is pulled in along its ray to the corpse's eye point (camera_radius_m off the hit). Every input is checked: a
// non-finite position or time ends the view (counted), never a NaN transform.

#include "bf3/fx/fx_types.h"

#include <cstdint>
#include <type_traits>

namespace bf3::sim {
class CollisionWorld;
} // namespace bf3::sim

namespace bf3::godot_side {

// The view's named defaults [inferred, measured = false: no capture of BF3's killer camera].
struct DeathCameraParams {
	float seconds = 4.0f; // the whole view (host::kKillCardSeconds)
	float orbit_seconds = 1.0f; // corpse -> pulled back and up
	float turn_seconds = 0.75f; // then the turn to the killer
	float back_m = 2.5f; // behind the corpse, against the death's view direction
	float up_m = 1.6f; // above the corpse's feet
	float eye_height_m = 0.4f; // the point on the corpse the camera looks at first (a prone body)
	float camera_radius_m = 0.2f; // kept from the static world
	// cam (as built, docs/design/m2b-cam.md): the point on the killer the camera turns to, above its presented feet.
	float killer_height_m = 1.2f;
};
static_assert(std::is_trivially_copyable_v<DeathCameraParams>);

// What the game knows when the own Death event is drained.
struct DeathCameraStart {
	fx::Vec3 corpse{}; // the own soldier's feet at the death
	float yaw_rad = 0.0f; // the own view yaw at the death (sim convention: 0 looks down +Z)
	fx::Vec3 eye{}; // the 1p camera position at the death (the view starts there)
	uint8_t killer_slot = 0xFF; // 0xFF or the own slot: no killer to look at
	bool killer_camera = true; // MatchState::killer_camera
};
static_assert(std::is_trivially_copyable_v<DeathCameraStart>);

struct DeathCameraView {
	bool active = false; // false: the game draws its own camera (1p, the free camera)
	bool on_killer = false; // the turn to the killer has started
	bool finished = false; // seconds elapsed: the game moves on to the spawn screen
	fx::Transform camera; // world, Frostbite axes (right, up, forward = view direction, trans)
};
static_assert(std::is_trivially_copyable_v<DeathCameraView>);

struct DeathCameraStats {
	uint64_t views = 0; // begin() calls
	uint64_t killer_views = 0; // views that turned to the killer
	uint64_t pulled_in = 0; // frames the static world pulled the camera in
	uint64_t bad_inputs = 0; // non-finite positions or times (the view ended)
};
static_assert(std::is_trivially_copyable_v<DeathCameraStats>);

class Bf3DeathCamera {
public:
	explicit Bf3DeathCamera(const DeathCameraParams &p_params = {});

	// The own death (main thread, the frame its Death event is drained); p_now_s is the presentation clock.
	void begin(const DeathCameraStart &p_start, double p_now_s);
	// A spawn or a revive of the own soldier: the view ends at once.
	void end();
	bool active() const;
	// Every display frame while active: p_killer is the killer's presented position this frame (null when it is not
	// drawn); p_world the static world (null: no pull-in).
	DeathCameraView step(double p_now_s, const fx::Vec3 *p_killer, const sim::CollisionWorld *p_world);
	const DeathCameraParams &params() const { return params_; }
	DeathCameraStats stats() const { return stats_; }

private:
	DeathCameraParams params_;
	DeathCameraStart start_{};
	double start_s_ = 0.0;
	bool active_ = false;
	bool killer_seen_ = false;
	fx::Vec3 last_killer_{};
	bool turned_counted_ = false; // cam: killer_views counted for this view
	DeathCameraStats stats_;
};

} // namespace bf3::godot_side
