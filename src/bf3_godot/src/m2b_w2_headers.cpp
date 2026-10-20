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

}
