#include "game_audio.h"
#include "game_crowd.h"
#include "game_fx.h"
#include "level_session_m2a.h"
#include "m2a_check.h"
#include "m2a_game.h"
#include "viewmodel_muzzle.h"

#include <type_traits>

namespace bf3::godot_side {

static_assert(!std::is_copy_constructible_v<Bf3GameAudio>);
static_assert(!std::is_copy_constructible_v<Bf3GameFx>);
static_assert(!std::is_copy_constructible_v<Bf3GameCrowd>);
static_assert(std::is_trivially_copyable_v<GameCrowdStats>);
static_assert(std::is_trivially_copyable_v<GameFxStats>);
static_assert(std::is_trivially_copyable_v<GameAudioStats>);
static_assert(uint8_t(ViewmodelMuzzleSource::WeaponRoot) == 1 && uint8_t(ViewmodelMuzzleSource::CameraStandIn) == 2);

}
