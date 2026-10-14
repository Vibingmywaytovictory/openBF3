#pragma once
// The headless M9 reload check ("-- --m9-reload-check" on res://scenes/m9_reload.tscn): after the viewmodel is
// loaded and built, samples ReloadEmpty at several frames and compares the Skeleton3D's global bone poses with
// bf3_anim's model-space pose (and with poses rebuilt from the Ant probe's m9_ReloadEmpty_Anim.json when
// OPENBF3_ANT_ORACLE names its folder), checks the skin binds against bf3_anim's skin matrices, the hands on the
// gun, the magazine and slide motion, the weapon in front of the camera, the 1P material settings and that the
// install is unchanged. Prints a PASS/FAIL table; returns the exit code (0 = all pass).

namespace bf3::godot_side {

class Bf3ViewmodelDemo;

int run_m9_reload_check(Bf3ViewmodelDemo &p_demo);

} // namespace bf3::godot_side
