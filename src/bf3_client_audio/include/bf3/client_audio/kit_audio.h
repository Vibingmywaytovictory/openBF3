#pragma once

#include "bf3/audio/weapon_audio.h"
#include "bf3/core/ebx_db.h"
#include "bf3/host/gunplay.h"
#include "bf3/net/protocol.h"

#include <cstdint>
#include <span>
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

struct KitAudioVariant {
	uint16_t def = 0;
	uint32_t accessory = 0;
	uint16_t source = 0;
	std::string unlock;
	std::string sound;
};

struct KitAudioSources {
	std::vector<WeaponSoundSource> sources;
	std::vector<KitAudioItem> items;
	std::vector<KitAudioVariant> variants;
};

KitAudioSources kit_audio_sources(const host::GunplayLevel &p_level);

std::vector<std::string> add_accessory_sounds(
		KitAudioSources &r_sources, const core::EbxResolver &p_db, const host::GunplayLevel &p_level);

uint16_t accessory_sound_source(std::span<const KitAudioVariant> p_variants, const net::SoldierGear &p_gear);

std::vector<std::string> kit_audio_report(const KitAudioSources &p_sources, const WeaponAudioTable &p_table);

}
