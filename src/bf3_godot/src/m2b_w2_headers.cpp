// W2.0 contract (docs/design/m2b-w2.md): compiles the M2b wave-2 Godot-side headers together inside the GDExtension,
// so a header that stops compiling breaks the dev build at once. All of them are Godot-free, so
// tests/m2b_contract2_tests.cpp (m2b.contract2) compiles them too, with their pins. No code.

#include "death_camera.h"
#include "game_crowd.h"
#include "hud_tdm.h"
#include "level_session_m2a.h"
#include "level_session_m2b.h"
#include "m2b_check.h"
#include "m2b_game.h"

#include <type_traits>

namespace bf3::godot_side {

static_assert(!std::is_copy_constructible_v<Bf3TdmHud>);
static_assert(!std::is_copy_constructible_v<M2bCheck>);
static_assert(std::is_trivially_copyable_v<TdmHudRecords>);
static_assert(std::is_trivially_copyable_v<DeathCameraView>);

} // namespace bf3::godot_side
