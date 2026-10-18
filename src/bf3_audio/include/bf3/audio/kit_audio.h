#pragma once
// The match's kit items as weapon sound sources (owner au3; docs/design/m2a-au3.md "Loading"). Godot-free: shared by
// Bf3GameAudio::load and the install rows of audio_router_tests.cpp, so both compile exactly the game's set.
//   - every def of GunplayLevel::defs gets a WeaponSoundSource at its index (the WeaponTable def id); only the kit
//     items of the two teams (kits.h slots 0, 1, 6, 7) carry their WeaponDef::sound, the others stay empty (form
//     None: the M2a match never hands them out);
//   - the knife (weapon_class wcKnife) and thrown items (projectile Grenade or FireLogic HoldAndRelease) are one-shots;
//     the firearms a driver at gameplay::shot_interval in their FireLogic's mode (Single / SingleWithBoltAction ->
//     Single, Burst -> Burst, else Automatic). The M67's explosion sound is its DetonationEffect's (fx3's effect
//     sounds), so explosion_sound stays empty.
#include "bf3/audio/weapon_audio.h"
#include "bf3/host/gunplay.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bf3::audio {

struct KitAudioItem {
	uint16_t def = 0; // the WeaponTable def id
	std::string variant; // WeaponDef::variant (the unlock partition)
	std::string sound;
	uint8_t teams = 0; // bit 0 US, bit 1 RU
	bool one_shot = false;
};

struct KitAudioSources {
	std::vector<WeaponSoundSource> sources; // one per def, index = def id
	std::vector<KitAudioItem> items; // the distinct kit items, in def order (E7 item 1 lists 12 on Metro TDM)
};

KitAudioSources kit_audio_sources(const host::GunplayLevel &p_level);

// "<variant>: driver (automatic, 0.075 s) | one-shot (event #h) | none (<error>)" per kit item.
std::vector<std::string> kit_audio_report(const KitAudioSources &p_sources, const WeaponAudioTable &p_table);

} // namespace bf3::audio
