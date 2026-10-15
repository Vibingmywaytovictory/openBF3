#pragma once
// The headless collision overlay check ("-- --collision-check" on res://scenes/metro_collision.tscn; ctest
// godot.collision): after the overlay has loaded the level and created every tile mesh, compares its soup with
// an independent Godot-free computation (a second load_level_collision) and, for the levels it knows, with the
// numbers research/probes/tools/level-oracle `collision` prints through IW4L's crates; checks that every (tile,
// body) array became one RenderingServer mesh with one surface, the right vertex count, the soup's triangles at
// the first and last slot with the reversed winding, a valid instance and the shared material; that the camera
// starts at a TDM spawn point; and that the install is unchanged. Prints a PASS/FAIL table and
// "COLLISION CHECK: PASS|FAIL (n/m)"; returns the exit code (0 = all pass).

namespace bf3::godot_side {

class Bf3CollisionOverlay;

int run_collision_check(Bf3CollisionOverlay &p_overlay);

} // namespace bf3::godot_side
