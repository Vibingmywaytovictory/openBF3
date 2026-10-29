#include "bf3/client_audio/kit_audio.h"

#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/host/kits.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bf3::audio {

namespace gp = core::gameplay;

namespace {

WeaponFireMode fire_mode_of(gp::FireLogic p_logic) {
	switch (p_logic) {
		case gp::FireLogic::Single:
		case gp::FireLogic::SingleWithBoltAction:
			return WeaponFireMode::Single;
		case gp::FireLogic::Burst:
			return WeaponFireMode::Burst;
		default:
			return WeaponFireMode::Automatic;
	}
}

bool is_one_shot(const gp::WeaponDef &p_def) {
	return p_def.weapon_class == "wcKnife" || p_def.projectile.kind == gp::ProjectileKind::Grenade || p_def.fire.logic == gp::FireLogic::HoldAndRelease;
}

const char *mode_name(WeaponFireMode p_mode) {
	switch (p_mode) {
		case WeaponFireMode::Single:
			return "single";
		case WeaponFireMode::Burst:
			return "burst";
		default:
			return "automatic";
	}
}

}

KitAudioSources kit_audio_sources(const host::GunplayLevel &p_level) {
	KitAudioSources out;
	out.sources.resize(p_level.defs.size());
	std::vector<uint8_t> teams(p_level.defs.size(), 0);
	for (uint8_t team = 0; team < host::kKitTeams; ++team) {
		for (uint8_t kit = 0; kit < host::kKitCount; ++kit) {
			const host::KitLoadout &l = p_level.kits.loadouts[team][kit];
			for (uint8_t slot : host::kKitSlots) {
				const uint16_t def = l.defs[slot];
				if (def < teams.size()) {
					teams[def] = uint8_t(teams[def] | (1u << team));
				}
				for (const host::LoadoutItem &item : p_level.loadouts.kits[team][kit].slots[slot]) {
					if (item.def < teams.size()) {
						teams[item.def] = uint8_t(teams[item.def] | (1u << team));
					}
				}
			}
		}
	}
	for (size_t i = 0; i < p_level.defs.size(); ++i) {
		if (teams[i] == 0) {
			continue;
		}
		const gp::WeaponDef &d = p_level.defs[i];
		WeaponSoundSource &s = out.sources[i];
		s.sound = d.sound;
		s.one_shot = is_one_shot(d);
		s.explosive = d.projectile.kind == gp::ProjectileKind::Grenade && d.fire.logic == gp::FireLogic::HoldAndRelease;
		s.fire_mode = fire_mode_of(d.fire.logic);
		const double interval = gp::shot_interval(d.fire);
		s.shot_interval_s = std::isfinite(interval) && interval > 0.0 ? interval : 0.1;
		out.items.push_back(KitAudioItem{ uint16_t(i), d.variant, d.sound, teams[i], s.one_shot });
	}
	return out;
}

std::vector<std::string> add_accessory_sounds(
		KitAudioSources &r_sources, const core::EbxResolver &p_db, const host::GunplayLevel &p_level) {
	std::vector<std::string> notes;
	std::vector<uint16_t> done;
	const size_t primary = size_t(gp::WeaponSlot::Primary);
	for (const auto &team : p_level.loadouts.kits) {
		for (const host::LoadoutKit &kit : team) {
			for (const host::LoadoutItem &item : kit.slots[primary]) {
				if (item.def >= r_sources.sources.size() || r_sources.sources[item.def].sound.empty() ||
						std::find(done.begin(), done.end(), item.def) != done.end()) {
					continue;
				}
				done.push_back(item.def);
				const core::Result<std::vector<gp::WeaponUnlockSound>> sounds =
						gp::weapon_unlock_sounds(p_db, item.unlock);
				if (!sounds) {
					notes.push_back(item.unlock + ": " + sounds.error().message);
					continue;
				}
				for (const gp::WeaponUnlockSound &s : *sounds) {
					for (size_t a = 0; a < host::kLoadoutAccessories; ++a) {
						for (size_t i = 0; i < item.accessory_unlocks[a].size() && i < item.accessories[a].size();
								++i) {
							if (item.accessory_unlocks[a][i] != s.unlock || s.sound.empty() ||
									s.sound == r_sources.sources[item.def].sound) {
								continue;
							}
							WeaponSoundSource source = r_sources.sources[item.def];
							source.sound = s.sound;
							r_sources.variants.push_back(KitAudioVariant{ item.def, item.accessories[a][i],
									uint16_t(r_sources.sources.size()), s.unlock, s.sound });
							r_sources.sources.push_back(std::move(source));
						}
					}
				}
			}
		}
	}
	return notes;
}

uint16_t accessory_sound_source(std::span<const KitAudioVariant> p_variants, const net::SoldierGear &p_gear) {
	for (const KitAudioVariant &v : p_variants) {
		if (v.def == p_gear.def && v.accessory != 0 &&
				std::find(p_gear.accessories.begin(), p_gear.accessories.end(), v.accessory) !=
						p_gear.accessories.end()) {
			return v.source;
		}
	}
	return p_gear.def;
}

std::vector<std::string> kit_audio_report(const KitAudioSources &p_sources, const WeaponAudioTable &p_table) {
	std::vector<std::string> out;
	for (const KitAudioItem &item : p_sources.items) {
		std::string line = item.variant + " [" + item.sound + "]: ";
		if (item.def >= p_table.defs.size()) {
			out.push_back(line + "none (no table entry)");
			continue;
		}
		const WeaponAudioDef &d = p_table.defs[item.def];
		char buf[160];
		switch (d.form) {
			case WeaponAudioForm::Driver:
				std::snprintf(buf, sizeof(buf), "driver (%s, %.4f s, step %s, reload %s)", mode_name(d.fire_mode), d.binding.shot_interval_seconds,
						d.binding.step_event ? "yes" : "no", d.has_reload ? "yes" : "no");
				line += buf;
				break;
			case WeaponAudioForm::OneShot:
				std::snprintf(buf, sizeof(buf), "one-shot (event %u%s)", d.one_shot_event,
						d.one_shot_event == kHashFire			 ? " Fire"
								: d.one_shot_event == kHashStep	 ? " Step"
								: d.one_shot_event == kHashMelee ? " Melee"
																 : "");
				line += buf;
				break;
			default:
				line += "none (" + d.error + ")";
				break;
		}
		if (d.form != WeaponAudioForm::None && !d.error.empty()) {
			line += "; " + d.error;
		}
		out.push_back(line);
	}
	return out;
}

}
