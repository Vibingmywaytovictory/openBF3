#include "deploy_check.h"

#include "deploy_screens.h"
#include "game_audio.h"
#include "game_crowd.h"
#include "game_fx.h"
#include "level_session_game.h"
#include "native_screens.h"
#include "viewmodel.h"

#include "bf3/host/client_session.h"
#include "bf3/host/client_wiring.h"

#include <algorithm>
#include <cstdio>
#include <memory>

namespace bf3::godot_side {

namespace {

constexpr std::string_view kTwelveGauge = "wc12gauge";

std::string fire_seconds(double p_seconds) {
	char b[32];
	std::snprintf(b, sizeof(b), "%.2f s", p_seconds);
	return b;
}

}

DeployFireClass deploy_fire_class(const bf3::host::LoadoutCatalog &p_catalog,
		std::span<const bf3::core::gameplay::WeaponDef> p_defs, uint8_t p_team, uint32_t p_item) {
	const size_t primary = size_t(bf3::core::gameplay::WeaponSlot::Primary);
	uint16_t def = 0xFFFF;
	for (uint8_t k = 0; k < bf3::host::kKitCount && def == 0xFFFF; ++k) {
		if (const bf3::host::LoadoutKit *kit = p_catalog.kit(p_team, k)) {
			if (const bf3::host::LoadoutItem *item = kit->find(primary, p_item)) {
				def = item->def;
			}
		}
	}
	if (def >= p_defs.size()) {
		return DeployFireClass::Other;
	}
	if (p_defs[def].weapon_class == kTwelveGauge) {
		return DeployFireClass::Shotgun;
	}
	uint8_t kits = 0;
	for (uint8_t k = 0; k < bf3::host::kKitCount; ++k) {
		const bf3::host::LoadoutKit *kit = p_catalog.kit(p_team, k);
		if (kit == nullptr) {
			continue;
		}
		for (const bf3::host::LoadoutItem &item : kit->slots[primary]) {
			if (item.def == def) {
				kits |= uint8_t(1u << k);
				break;
			}
		}
	}
	using bf3::host::KitRole;
	if (kits == (1u << bf3::host::kKitCount) - 1u) {
		return DeployFireClass::Pdw;
	}
	if (kits == 1u << uint8_t(KitRole::Assault)) {
		return DeployFireClass::Assault;
	}
	if (kits == 1u << uint8_t(KitRole::Support)) {
		return DeployFireClass::Lmg;
	}
	if (kits == 1u << uint8_t(KitRole::Recon)) {
		return DeployFireClass::Sniper;
	}
	return DeployFireClass::Other;
}

void DeployCheck::fire_next(uint32_t p_step, double p_now_s) {
	fire_step_ = p_step;
	fire_s_ = p_now_s;
	phase_s_ = p_now_s;
	sub_ = 0;
}

void DeployCheck::fire_rows(const DeployCheckState &p_st) {
	const std::string name = "fire." + std::string(kDeployFireClassNames[fire_class_]);
	const bool picked_vm =
			!p_st.primary_unlock.empty() && p_st.vm_unlock == p_st.primary_unlock && !p_st.vm_mesh.empty();
	row(name + ".viewmodel", picked_vm,
			"item " + std::to_string(fire_item_) + " def " + std::to_string(p_st.primary_def) + " " +
					p_st.primary_unlock + ": viewmodel item " + std::to_string(p_st.vm_item) + " (" +
					(p_st.vm_item >= p_st.vm_preloaded ? "loaded when picked" : "preloaded") + ") " + p_st.vm_unlock +
					", mesh " + p_st.vm_mesh + ", ready " + fire_seconds(fire_vm_wait_s_) + " after the spawn");
	const uint32_t shots = p_st.vm_shots - fire_mark_.vm_shots;
	const bool kicked = p_st.vm_kick_tuning > 0.0f && fire_kick_peak_ >= kDeployFireKickShare * p_st.vm_kick_tuning;
	char kick[96];
	std::snprintf(kick, sizeof(kick), "kick peak %.4f m of %.4f", fire_kick_peak_, p_st.vm_kick_tuning);
	row(name + ".fire_anim", shots > 0 && (fire_action_seen_ || (p_st.vm_fire_source != "clip" && kicked)),
			std::to_string(shots) + " shot cue(s); Fire role " + p_st.vm_fire_source + ": Fire clip " +
					(fire_action_seen_ ? std::string(fire_procedural_seen_ ? "played procedurally" : "played")
									   : "not played") +
					", " + kick);
	const uint64_t bursts = p_st.audio_own_bursts - fire_mark_.audio_own_bursts;
	const uint64_t one_shots = p_st.audio_one_shots - fire_mark_.audio_one_shots;
	const uint64_t unbound = p_st.audio_unbound - fire_mark_.audio_unbound;
	row(name + ".audio", (bursts > 0 || one_shots > 0) && unbound == 0,
			std::to_string(bursts) + " own burst(s), " + std::to_string(one_shots) + " one-shot(s), " +
					std::to_string(unbound) + " unbound");
	if (p_st.vm_bolt_source == "clip") {
		row(name + ".bolt", fire_bolt_seen_,
				std::string("Bolt role clip ") + (fire_bolt_seen_ ? "played after the shot" : "not played"));
	}
	const uint64_t fx_shots = p_st.fx_own_shots - fire_mark_.fx_own_shots;
	const uint64_t muzzles = p_st.fx_muzzle_starts - fire_mark_.fx_muzzle_starts;
	row(name + ".fire_fx", fx_shots > 0 && muzzles > 0,
			std::to_string(fx_shots) + " own shot(s), " + std::to_string(muzzles) + " muzzle effect(s) started");
	row(name + ".second_client", fire_extra_seen_ && fire_extra_def_ == p_st.primary_def && p_st.crowd_mesh,
			std::string("player2 ") + (fire_extra_seen_ ? "saw the shots" : "saw no shots") + " with def " +
					std::to_string(fire_extra_def_) + " (" + fire_extra_fact_ + "); the 3p mesh for def " +
					std::to_string(p_st.primary_def) + (p_st.crowd_mesh ? " is drawable" : " is not loaded"));
}

void DeployCheck::fire(
		Bf3UiScreens &r_screens, double p_now_s, const DeployCheckIo &p_io, const DeployCheckState &p_st) {
	const double in = p_now_s - fire_s_;
	const std::string name =
			fire_class_ < kDeployFireClassNames.size() ? std::string(kDeployFireClassNames[fire_class_]) : "";
	const uint8_t kit = fire_class_ < kDeployFireKits.size() ? kDeployFireKits[fire_class_] : 0;
	switch (fire_step_) {
		case 0: {
			std::string error;
			fire_joined_ = p_io.join && p_io.join(&error);
			row("fire.second_client_joined", fire_joined_, fire_joined_ ? "player2 joined" : error);
			fire_next(1, p_now_s);
			break;
		}
		case 1:
			fire_mark_ = p_st;
			p_io.suicide();
			fire_next(2, p_now_s);
			break;
		case 2:
			if (p_st.own_deaths > fire_mark_.own_deaths && !p_st.alive) {
				fire_next(3, p_now_s);
			}
			break;
		case 3:
			if (!shown(r_screens, kSpawnScreenPcPartition)) {
				fire_s_ = p_now_s;
			} else if (in >= kDeployCheckSettleSeconds) {
				p_io.kit_key(kit);
				fire_next(4, p_now_s);
			}
			break;
		case 4:
			if (in >= kDeployCheckInputSeconds &&
					click_clip(r_screens, kSpawnScreenPcPartition, kDeployCheckPrimarySlot, p_io)) {
				fire_next(5, p_now_s);
			}
			break;
		case 5:
			if (in >= kDeployCheckGridSeconds) {
				fire_cell_ = -1;
				for (uint32_t i = 0; i < p_st.primary_count && fire_cell_ < 0; ++i) {
					if (p_st.primary_classes[i] == DeployFireClass(fire_class_) &&
							int32_t(i) != p_st.primary_selected) {
						fire_cell_ = int32_t(i);
					}
				}
				if (p_st.spawn_kit != kit || fire_cell_ < 0) {
					row("fire." + name + ".pick", false,
							"spawn kit " + std::to_string(unsigned(p_st.spawn_kit)) + ", " +
									std::to_string(p_st.primary_count) + " primaries and none of class " + name +
									" besides the selected one");
					enter(Phase::Done, p_now_s, p_st);
					break;
				}
				fire_item_ = p_st.primary_items[size_t(fire_cell_)];
				fire_mark_ = p_st;
				fire_next(6, p_now_s);
			}
			break;
		case 6:
			if (click_clip(r_screens, kSpawnScreenPcPartition,
						std::string(kDeployCheckGridCell) + std::to_string(fire_cell_) + "-s-0", p_io)) {
				fire_next(7, p_now_s);
			}
			break;
		case 7:
			if (in >= kDeployCheckGridSeconds) {
				shot(p_io, "fire_" + name + "_grid", p_now_s);
				row("fire." + name + ".pick",
						p_st.chosen_primary == fire_item_ && p_st.loadout_requests > fire_mark_.loadout_requests,
						"kit " + std::to_string(unsigned(kit)) + " cell " + std::to_string(fire_cell_) + ": item " +
								std::to_string(fire_item_) + ", kit bar primary " +
								std::to_string(p_st.chosen_primary));
				fire_next(kDeployFireAccessories[fire_class_] ? 30 : 8, p_now_s);
			}
			break;
		case 8:
			if (p_st.time_to_respawn > 0.0f) {
				fire_s_ = p_now_s;
			} else if (in >= kDeployCheckSettleSeconds) {
				fire_mark_ = p_st;
				tap(p_io, godot::KEY_ENTER);
				fire_next(9, p_now_s);
			}
			break;
		case 9:
			if (p_st.alive && p_st.own_spawns > fire_mark_.own_spawns) {
				fire_next(10, p_now_s);
			}
			break;
		case 10: {
			const bool ready = !p_st.primary_unlock.empty() && p_st.vm_unlock == p_st.primary_unlock;
			if ((ready && in >= kDeployCheckSpawnEndSeconds) || in >= kDeployFireViewmodelSeconds) {
				fire_vm_wait_s_ = in;
				shot(p_io, "fire_" + name + "_idle", p_now_s);
				fire_mark_ = p_st;
				fire_action_seen_ = fire_procedural_seen_ = fire_extra_seen_ = fire_shot_taken_ = fire_bolt_seen_ =
						false;
				fire_first_shot_s_ = -1.0;
				fire_kick_peak_ = 0.0f;
				fire_extra_def_ = 0xFFFF;
				p_io.hold_fire(true);
				fire_next(11, p_now_s);
			}
			break;
		}
		case 11:
		case 12:
			fire_kick_peak_ = std::max(fire_kick_peak_, p_st.vm_kick_back);
			fire_extra_fact_ = p_st.extra_fact;
			if (p_st.vm_fire_action) {
				fire_action_seen_ = true;
				fire_procedural_seen_ = fire_procedural_seen_ || p_st.vm_fire_procedural;
			}
			if (p_st.extra_valid && (p_st.extra_firing || p_st.extra_burst_seq != fire_mark_.extra_burst_seq)) {
				fire_extra_seen_ = true;
				fire_extra_def_ = p_st.extra_def;
			}
			fire_bolt_seen_ = fire_bolt_seen_ || p_st.vm_bolt_action;
			if (fire_first_shot_s_ < 0.0 && p_st.vm_shots > fire_mark_.vm_shots) {
				fire_first_shot_s_ = p_now_s;
			}
			if (!fire_shot_taken_ && fire_first_shot_s_ >= 0.0 &&
					p_now_s - fire_first_shot_s_ >= kDeployFireShotSeconds) {
				shot(p_io, "fire_" + name + "_shot", p_now_s);
				fire_shot_taken_ = true;
			}
			if (fire_step_ == 11 && in >= kDeployFireHoldSeconds) {
				p_io.hold_fire(false);
				fire_next(12, p_now_s);
			} else if (fire_step_ == 12 && in >= kDeployFireAfterSeconds && kDeployFireAccessories[fire_class_]) {
				fire_hip_ = p_st;
				p_io.hold_zoom(true);
				fire_next(41, p_now_s);
			} else if (fire_step_ == 12 && in >= kDeployFireAfterSeconds) {
				fire_rows(p_st);
				++fire_class_;
				if (fire_class_ >= kDeployFireClassNames.size()) {
					enter(Phase::Done, p_now_s, p_st);
				} else {
					fire_next(1, p_now_s);
				}
			}
			break;
		default:
			if (fire_step_ >= 30) {
				fire_accessories(r_screens, p_now_s, p_io, p_st);
			}
			break;
	}
}

void DeployCheck::fire_accessories(
		Bf3UiScreens &r_screens, double p_now_s, const DeployCheckIo &p_io, const DeployCheckState &p_st) {
	const double in = p_now_s - fire_s_;
	const std::string cls(kDeployFireClassNames[fire_class_]);
	const std::string name = "fire." + cls;
	const uint8_t kit = kDeployFireKits[fire_class_];
	const std::string_view soldier = kCustomizeScreenPartitions[0];
	const std::string_view sub = kCustomizeScreenPartitions[1];
	const std::string_view accessories = kCustomizeScreenPartitions[2];
	const DeployCheckList &barrel = p_st.lists[kDeployCheckBarrelList];
	if (fire_class_ == size_t(DeployFireClass::Assault)) {
		if (!fire_extra_requested_ && p_st.extra_pick_valid && p_io.extra_loadout) {
			p_io.extra_loadout(p_st.extra_pick);
			fire_extra_requested_ = true;
			fire_extra_spawn_s_ = p_now_s - kDeployFireExtraSpawnSeconds;
		}
		if (fire_extra_requested_ && p_io.extra_spawn &&
				p_now_s - fire_extra_spawn_s_ >= kDeployFireExtraSpawnSeconds) {
			p_io.extra_spawn(p_st.extra_pick.kit);
			fire_extra_spawn_s_ = p_now_s;
		}
	}
	switch (fire_step_) {
		case 30:
			if (in >= kDeployCheckSettleSeconds &&
					click_clip(r_screens, kSpawnScreenPcPartition, kDeployCheckTabs[0].button, p_io)) {
				fire_next(31, p_now_s);
			}
			break;
		case 31:
			if (in >= kDeployCheckTabSeconds &&
					click_clip(r_screens, soldier,
							std::string(kDeployCheckSoldierKitButton) + std::to_string(unsigned(kit)) + ".button1",
							p_io)) {
				fire_next(32, p_now_s);
			}
			break;
		case 32:
			if (in >= kDeployCheckGridSeconds &&
					click_clip(r_screens, sub, std::string(kDeployCheckAccessoriesButton), p_io)) {
				fire_next(33, p_now_s);
			}
			break;
		case 33:
			if (in >= kDeployCheckGridSeconds) {
				const DeployCheckList &optic = p_st.lists[kDeployCheckOpticList];
				fire_optic_expected_ = optic.count < 2
						? 0
						: optic.items[size_t((std::max(optic.selected, 0) + 1) % int32_t(optic.count))];
				fire_mark_ = p_st;
				if (click_clip(r_screens, accessories,
							std::string(kDeployCheckOpticSelector) + std::string(kDeployCheckSelectorRight), p_io)) {
					fire_next(34, p_now_s);
				}
			}
			break;
		case 34:
			if (in >= kDeployCheckGridSeconds) {
				shot(p_io, "fire_" + cls + "_optic", p_now_s);
				const uint32_t chosen = p_st.lists[kDeployCheckOpticList].chosen;
				row(name + ".pick_optic",
						fire_optic_expected_ != 0 && chosen == fire_optic_expected_ &&
								p_st.loadout_requests > fire_mark_.loadout_requests,
						"ACCESSORIES optic " + std::to_string(fire_mark_.lists[kDeployCheckOpticList].chosen) + " -> " +
								std::to_string(chosen) + " (expected " + std::to_string(fire_optic_expected_) + "), " +
								std::to_string(p_st.loadout_requests - fire_mark_.loadout_requests) +
								" loadout request(s)");
				fire_clicks_ = 0;
				fire_next(fire_class_ == size_t(DeployFireClass::Assault) ? 35 : 37, p_now_s);
			}
			break;
		case 35:
			if (p_st.suppressor_item != 0 && barrel.chosen == p_st.suppressor_item) {
				shot(p_io, "fire_" + cls + "_suppressor", p_now_s);
				row(name + ".pick_suppressor", true,
						"ACCESSORIES barrel item " + std::to_string(barrel.chosen) +
								" (the accessory with a WeaponSoundModifier) after " + std::to_string(fire_clicks_) +
								" click(s)");
				fire_next(37, p_now_s);
			} else if (p_st.suppressor_item == 0 || fire_clicks_ > barrel.count) {
				row(name + ".pick_suppressor", false,
						"suppressor item " + std::to_string(p_st.suppressor_item) + ", barrel list of " +
								std::to_string(barrel.count) + " at " + std::to_string(barrel.chosen) + " after " +
								std::to_string(fire_clicks_) + " click(s)");
				fire_next(37, p_now_s);
			} else if (click_clip(r_screens, accessories,
							   std::string(kDeployCheckBarrelSelector) + std::string(kDeployCheckSelectorRight),
							   p_io)) {
				++fire_clicks_;
				fire_next(36, p_now_s);
			}
			break;
		case 36:
			if (in >= kDeployCheckGridSeconds) {
				fire_next(35, p_now_s);
			}
			break;
		case 37:
		case 38:
		case 39:
			if (in >= kDeployCheckGridSeconds) {
				tap(p_io, godot::KEY_ESCAPE);
				fire_next(fire_step_ + 1, p_now_s);
			}
			break;
		case 40:
			if (in >= kDeployCheckGridSeconds) {
				row(name + ".customize_back", shown(r_screens, kSpawnScreenPcPartition) && !up(r_screens, soldier),
						std::string("three Escapes from ACCESSORIES: spawnscreenpc ") +
								(shown(r_screens, kSpawnScreenPcPartition) ? "shown" : "not shown"));
				fire_mark_ = p_st;
				fire_next(8, p_now_s);
			}
			break;
		case 41:
			if ((p_st.vm_fov_progress >= 1.0f && in >= kDeployFireAfterSeconds && !p_st.vm_fire_action &&
						!p_st.vm_bolt_action) ||
					in >= kDeployFireAdsSeconds) {
				fire_ads_ = p_st;
				shot(p_io, "fire_" + cls + "_ads", p_now_s);
				p_io.hold_zoom(false);
				fire_next(42, p_now_s);
			}
			break;
		case 42:
			if (in >= kDeployFireAfterSeconds) {
				fire_rows(p_st);
				fire_accessory_rows(p_st);
				++fire_class_;
				if (fire_class_ >= kDeployFireClassNames.size()) {
					enter(Phase::Done, p_now_s, p_st);
				} else {
					fire_next(1, p_now_s);
				}
			}
			break;
		default:
			break;
	}
}

void DeployCheck::fire_accessory_rows(const DeployCheckState &p_st) {
	const std::string name = "fire." + std::string(kDeployFireClassNames[fire_class_]);
	const auto joined = [](const std::vector<std::string> &p_list) {
		std::string out;
		for (const std::string &s : p_list) {
			if (!s.empty()) {
				out += (out.empty() ? "" : ", ") + s;
			}
		}
		return out.empty() ? std::string("none") : out;
	};
	const auto drawn = [](const std::vector<std::string> &p_list, const std::string &p_unlock) {
		return std::any_of(
				p_list.begin(), p_list.end(), [&](const std::string &s) { return s.starts_with(p_unlock + " "); });
	};
	bool all = false;
	for (const std::string &u : fire_hip_.picked_unlocks) {
		if (u.empty()) {
			continue;
		}
		all = drawn(fire_hip_.vm_attached, u) || drawn(fire_ads_.vm_attached, u);
		if (!all) {
			break;
		}
	}
	row(name + ".attachments", all,
			"picked " + joined(fire_hip_.picked_unlocks) + "; drawn at hip: " + joined(fire_hip_.vm_attached) +
					"; drawn at ADS: " + joined(fire_ads_.vm_attached));
	char fov[256];
	std::snprintf(fov, sizeof(fov), "ADS camera FOV %.2f deg, the picked set's zoom level %.2f deg (%s), progress %.2f",
			double(fire_ads_.camera_fov), double(fire_ads_.vm_zoom_fov), fire_ads_.vm_zoom_controller.c_str(),
			double(fire_ads_.vm_fov_progress));
	row(name + ".ads_fov",
			fire_ads_.vm_zoom_fov > 0.0f && fire_ads_.vm_fov_progress >= 1.0f &&
					std::abs(fire_ads_.camera_fov - fire_ads_.vm_zoom_fov) < kDeployFireFovTolerance,
			fov);
	const std::string optic = fire_hip_.picked_unlocks.empty() ? std::string() : fire_hip_.picked_unlocks[0];
	const SightSurfaces &surfaces = fire_ads_.vm_sight_surfaces;
	char sight[256];
	std::snprintf(sight, sizeof(sight),
			"; sight centre %s (%.1f, %.1f) px from the screen centre, %zu of %zu unlit reticle surfaces textured, %zu "
			"of %zu "
			"lit lens surfaces see-through",
			fire_ads_.vm_sight ? "at" : "not found,", double(fire_ads_.vm_sight_px.x), double(fire_ads_.vm_sight_px.y),
			surfaces.reticles_textured, surfaces.reticles, surfaces.lenses_clear, surfaces.lenses);
	row(name + ".sight_picture",
			!optic.empty() && drawn(fire_ads_.vm_attached, optic) && fire_ads_.vm_sight &&
					fire_ads_.vm_sight_px.length() <= kDeployFireSightPx && surfaces.reticles > 0 &&
					surfaces.reticles_textured == surfaces.reticles && surfaces.lenses_clear == surfaces.lenses,
			"optic " + (optic.empty() ? std::string("not picked") : optic) +
					" at ADS: " + joined(fire_ads_.vm_attached) + sight + "; " + fire_ads_.vm_pose);
	if (fire_class_ == size_t(DeployFireClass::Assault)) {
		const uint64_t sounds = fire_hip_.audio_accessory_sounds - fire_mark_.audio_accessory_sounds;
		const uint64_t effects = fire_hip_.fx_accessory_effects - fire_mark_.fx_accessory_effects;
		row(name + ".suppressed", sounds > 0 && effects > 0,
				std::to_string(sounds) + " shot sound(s) from the suppressor's WeaponSoundModifier entry, " +
						std::to_string(effects) + " muzzle start(s) from its WeaponFiringEffectsModifier entry");
	}
	const std::string seen = std::string("player2's gear for the own slot ") +
			(p_st.extra_gear_match ? "names" : "does not name") + " the own picks";
	if (fire_class_ != size_t(DeployFireClass::Assault)) {
		row(name + ".second_client_sees_gear", p_st.extra_gear_match, seen);
		return;
	}
	row(name + ".second_client_gear",
			p_st.extra_gear_match && p_st.extra_pick_valid && p_st.extra_gear_named && p_st.crowd_extra_meshes > 0,
			seen + "; player2 picked " + (p_st.extra_pick_valid ? p_st.extra_pick_unlock : std::string("nothing")) +
					", which the own gear for its slot " + std::to_string(unsigned(p_st.extra_slot)) +
					(p_st.extra_gear_named ? " names" : " does not name") + ", and the own crowd draws player2 with " +
					std::to_string(p_st.crowd_extra_meshes) + " accessory meshes");
}

void fill_deploy_fire_state(DeployCheckState &r_state, GameState &r_game, const bf3::host::PresentationFrame &p_frame,
		const bf3::host::SharedLevel *p_level, const bf3::host::ClientSession *p_extra, const godot::Camera3D *p_eye) {
	const uint8_t team = r_game.own_team;
	if (p_level != nullptr && p_level->gunplay != nullptr) {
		for (size_t i = 0; i < r_state.primary_count; ++i) {
			r_state.primary_classes[i] = deploy_fire_class(
					p_level->gunplay->loadouts, p_level->gunplay->defs, team, r_state.primary_items[i]);
		}
	}
	const size_t primary = size_t(bf3::core::gameplay::WeaponSlot::Primary);
	if (p_frame.own_weapons_valid) {
		r_state.primary_def = p_frame.own_weapons_curr.slots[primary].def;
	}
	const bool team_ok = team == 1 || team == 2;
	if (team_ok) {
		const auto it = r_game.vm_catalog[size_t(team - 1)].find(r_state.primary_def);
		if (it != r_game.vm_catalog[size_t(team - 1)].end()) {
			r_state.primary_unlock = it->second;
		}
	}
	if (const Bf3Viewmodel *vm = r_game.own_viewmodel()) {
		if (const bf3::render::ViewmodelItem *item = vm->current_item()) {
			r_state.vm_unlock = item->unlock;
			r_state.vm_mesh = item->mesh;
		}
		r_state.vm_item = vm->item();
		r_state.vm_preloaded = team_ok && r_game.vm_sets[size_t(team - 1)] != nullptr
				? int32_t(r_game.vm_sets[size_t(team - 1)]->items.size())
				: 0;
		const bf3::anim::ViewmodelOutput &out = vm->output();
		r_state.vm_shots = out.shots;
		r_state.vm_fire_action = out.action_clip == uint32_t(bf3::anim::VmClip::Fire) ||
				out.action_clip == uint32_t(bf3::anim::VmClip::FireSingle);
		r_state.vm_fire_procedural = out.action_procedural;
		r_state.vm_kick_back = out.kick_back;
		r_state.vm_kick_tuning = vm->tuning().kick_back;
		r_state.vm_bolt_action = out.action_clip == uint32_t(bf3::anim::VmClip::Bolt);
		if (const bf3::render::ViewmodelItem *item = vm->current_item()) {
			r_state.vm_fire_source = bf3::render::viewmodel_clip_source_name(
					item->clips[size_t(bf3::render::ViewmodelClipRole::Fire)].source);
			r_state.vm_bolt_source = bf3::render::viewmodel_clip_source_name(
					item->clips[size_t(bf3::render::ViewmodelClipRole::Bolt)].source);
		}
	}
	if (r_game.audio != nullptr) {
		const GameAudioStats a = r_game.audio->stats();
		r_state.audio_own_bursts = a.router.own_bursts;
		r_state.audio_one_shots = a.router.one_shots;
		r_state.audio_unbound = a.router.unbound_defs;
	}
	if (r_game.fx != nullptr) {
		const GameFxStats x = r_game.fx->stats();
		r_state.fx_own_shots = x.triggers.own_shots;
		r_state.fx_muzzle_starts = x.triggers.muzzle_starts;
	}
	if (p_extra != nullptr && r_game.own_slot != 0xFF) {
		const auto other = std::make_unique<bf3::host::PresentationFrame>();
		if (p_extra->read_presentation(*other)) {
			const double ticks = std::max(other->render_ticks_at_publish, double(other->newest_tick));
			const bf3::host::PresentedOtherWeapon w = bf3::host::present_other_weapon(*other, r_game.own_slot, ticks);
			r_state.extra_fact = "player2 slot " + std::to_string(unsigned(other->stats.slot)) + ", " +
					std::to_string(other->other_count) + " others, own slot " +
					std::to_string(unsigned(r_game.own_slot)) + (w.valid ? ", presented" : ", not presented");
			r_state.extra_valid = w.valid;
			r_state.extra_def = w.def;
			r_state.extra_firing = w.firing;
			r_state.extra_burst_seq = w.burst_seq;
		}
	}
	if (r_game.crowd != nullptr && r_state.primary_def != 0xFFFF) {
		r_state.crowd_mesh = r_game.crowd->has_weapon_mesh(team, r_state.primary_def);
	}
	const bf3::host::LoadoutCatalog *catalog =
			p_level != nullptr && p_level->gunplay != nullptr ? &p_level->gunplay->loadouts : nullptr;
	if (catalog != nullptr && team_ok && r_game.audio != nullptr) {
		const bf3::host::LoadoutKit *kit = catalog->kit(team, r_state.spawn_kit);
		const bf3::host::LoadoutItem *item = kit != nullptr ? kit->find(primary, r_state.chosen_primary) : nullptr;
		for (size_t i = 0; item != nullptr && i < item->accessories[2].size() && r_state.suppressor_item == 0; ++i) {
			for (const bf3::audio::KitAudioVariant &v : r_game.audio->sound_variants()) {
				if (v.def == item->def && v.accessory == item->accessories[2][i]) {
					r_state.suppressor_item = v.accessory;
				}
			}
		}
	}
	if (catalog != nullptr && team_ok && r_game.own_slot < p_frame.gear.size()) {
		const bf3::net::SoldierGear &gear = p_frame.gear[r_game.own_slot];
		r_state.own_picks = gear.accessories;
		if (const bf3::host::LoadoutItem *item = bf3::host::loadout_primary_item(*catalog, team, gear.def)) {
			const bf3::host::LoadoutAccessorySet set = bf3::host::loadout_accessory_set(*item, gear.accessories);
			r_state.picked_unlocks.assign(bf3::host::kLoadoutAccessories, std::string());
			for (size_t a = 0; a < bf3::host::kLoadoutAccessories; ++a) {
				for (size_t i = 0; set.picked[a] != 0 && i < item->accessories[a].size(); ++i) {
					if (item->accessories[a][i] == set.picked[a] && i < item->accessory_unlocks[a].size()) {
						r_state.picked_unlocks[a] = item->accessory_unlocks[a][i];
					}
				}
			}
		}
		if (r_game.crowd != nullptr) {
			r_state.crowd_gear_meshes = r_game.crowd->gear_accessory_meshes(team, gear);
		}
		if (p_extra != nullptr) {
			const auto other = std::make_unique<bf3::host::PresentationFrame>();
			r_state.extra_gear_match = p_extra->read_presentation(*other) &&
					gear.accessories != std::array<uint32_t, bf3::host::kLoadoutAccessories>{} &&
					other->gear[r_game.own_slot] == gear;
		}
	}
	if (const Bf3Viewmodel *vm = r_game.own_viewmodel()) {
		r_state.vm_attached = vm->visible_attachments();
		r_state.vm_zoom_fov = vm->ads_params().zoom_fov;
		r_state.vm_fov_progress = vm->ads().fov_progress;
		if (const bf3::render::ViewmodelItem *item = vm->current_item()) {
			r_state.vm_zoom_controller = item->ads.controller;
		}
		if (const std::optional<godot::Vector2> px = vm->sight_offset_px(p_eye)) {
			r_state.vm_sight = true;
			r_state.vm_sight_px = *px;
		}
		char pose[160];
		const bf3::anim::ViewmodelOutput &out = vm->output();
		std::snprintf(pose, sizeof(pose),
				"action clip %u at %.2f s weight %.2f, base clip %u, ADS blend %.2f, offset (%.3f, %.3f, %.3f)",
				out.action_clip, double(out.action_time_s), double(out.action_weight), out.base_clip,
				double(vm->ads().blend), double(out.weapon_offset.t.x), double(out.weapon_offset.t.y),
				double(out.weapon_offset.t.z));
		r_state.vm_pose = pose;
		r_state.vm_sight_surfaces = vm->sight_surfaces();
	}
	if (catalog != nullptr && p_extra != nullptr) {
		const auto other = std::make_unique<bf3::host::PresentationFrame>();
		uint8_t extra_team = 0;
		if (p_extra->read_presentation(*other) && other->stats.connected) {
			r_state.extra_slot = other->stats.slot;
			for (size_t i = 0; i < p_frame.player_count && i < p_frame.players.size(); ++i) {
				if (p_frame.players[i].slot == r_state.extra_slot) {
					extra_team = p_frame.players[i].team;
				}
			}
		}
		if (extra_team == 1 || extra_team == 2) {
			const uint8_t kit_index = kDeployFireKits[size_t(DeployFireClass::Assault)];
			const bf3::host::LoadoutKit *kit = catalog->kit(extra_team, kit_index);
			const bf3::host::LoadoutItem *item = kit != nullptr ? kit->find(primary, kit->defaults[primary]) : nullptr;
			for (size_t i = 0; item != nullptr && i < item->accessories[0].size() && !r_state.extra_pick_valid; ++i) {
				if (item->accessories[0][i] != item->default_accessories[0] &&
						!item->accessory_unlocks[0][i].starts_with(bf3::render::kDefaultSocketUnlockPrefix)) {
					r_state.extra_pick.kit = kit_index;
					r_state.extra_pick.accessories = { item->accessories[0][i], 0, 0, 0 };
					r_state.extra_pick_unlock = item->accessory_unlocks[0][i];
					r_state.extra_pick_valid = true;
				}
			}
		}
		if (r_state.extra_pick_valid && r_state.extra_slot < p_frame.gear.size()) {
			r_state.extra_gear_named = p_frame.gear[r_state.extra_slot].accessories == r_state.extra_pick.accessories;
		}
		if (r_game.crowd != nullptr && r_state.extra_slot != 0xFF) {
			r_state.crowd_extra_meshes = r_game.crowd->slot_accessory_meshes(r_state.extra_slot);
		}
	}
	if (r_game.audio != nullptr) {
		r_state.audio_accessory_sounds = r_game.audio->stats().router.accessory_sounds;
	}
	if (r_game.fx != nullptr) {
		r_state.fx_accessory_effects = r_game.fx->stats().triggers.accessory_effects;
	}
}
}
