#pragma once

#include "bf3/audio/weapon_audio.h"
#include "bf3/host/gunplay.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bf3::audio {

struct KitAudioItem {
	uint16_t def = 0;
	std::string variant;
	std::string sound;
	uint8_t teams = 0;
	bool one_shot = false;
};

struct KitAudioSources {
	std::vector<WeaponSoundSource> sources;
	std::vector<KitAudioItem> items;
};

KitAudioSources kit_audio_sources(const host::GunplayLevel &p_level);

std::vector<std::string> kit_audio_report(const KitAudioSources &p_sources, const WeaponAudioTable &p_table);

}
