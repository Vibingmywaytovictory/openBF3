// bf3dump gameplay commands: print the soldier, mode, kit, vehicle and scoring tables that
// bf3_core's gameplay readers resolve (include/bf3/core/gameplay/*). The output is BF3 data: keep it
// out of the repo. Each command runs on one WorkBudget and reports the units it used on stderr.
//
//   soldier [<sb>]                 SoldierDef of characters/soldiers/mpsoldier (default sb: MP_001)
//   mode <level> [<mode>]          without a mode: the level's GameMode options; else the ModeDef
//   kits <level> <mode> [--all]    the mode's teams and kits; --all lists every selectable unlock
//   vehicle <partition> [<sb>]     VehicleDef of a VehicleBlueprint (default sb: @all-mp, lazy)
//   scoring [<sb>]                 persistence/scoring/scoring (default sb: MP_001)
#include "gameplay_commands.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/kit_def.h"
#include "bf3/core/gameplay/mode_def.h"
#include "bf3/core/gameplay/soldier_def.h"
#include "bf3/core/gameplay/vehicle_def.h"
#include "bf3/core/ids.h"

#include <chrono>
#include <cstdio>
#include <memory>

using namespace bf3::core;
using namespace bf3::core::gameplay;

namespace bf3dump {

namespace {

constexpr const char *kDefaultSuperbundle = "Win32/Levels/MP_001/MP_001";

std::unique_ptr<EbxLibrary> open_library(const EbxContext &p_ctx, const std::string &p_spec) {
	const auto start = std::chrono::steady_clock::now();
	Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(p_ctx.install, expand_superbundles(*p_ctx.install, p_spec));
	if (!lib) {
		std::fprintf(stderr, "%s\n", lib.error().message.c_str());
		return nullptr;
	}
	std::fprintf(stderr, "%s: %zu partitions catalogued in %.2f s\n", p_spec.c_str(), (*lib)->catalog_size(),
			std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
	return std::move(*lib);
}

std::unique_ptr<EbxLibrary> open_level(const EbxContext &p_ctx, const std::string &p_level) {
	const std::optional<Level> level = p_ctx.install->level(p_level);
	if (!level) {
		std::fprintf(stderr, "no multiplayer level `%s`\n", p_level.c_str());
		return nullptr;
	}
	return open_library(p_ctx, level->superbundle);
}

std::string pose_times(const PoseDef &p_pose) {
	std::string out;
	for (size_t i = 0; i < kSoldierPoseCount; ++i) {
		if (p_pose.transition_to[i]) {
			char b[64];
			std::snprintf(b, sizeof(b), "%s%s %g", out.empty() ? "" : ", ", soldier_pose_name(static_cast<SoldierPose>(i)), *p_pose.transition_to[i]);
			out += b;
		}
	}
	return out;
}

std::string curve_text(const std::vector<CurvePoint> &p_points) {
	std::string out;
	for (const CurvePoint &p : p_points) {
		char b[64];
		std::snprintf(b, sizeof(b), "(%g,%g)", p.x, p.y);
		out += b;
	}
	return out;
}

void print_scalars(const char *p_prefix, const std::vector<std::pair<std::string, double>> &p_values) {
	for (const auto &[name, value] : p_values) {
		std::printf("%s.%s\t%g\n", p_prefix, name.c_str(), value);
	}
}

int cmd_soldier(const EbxContext &p_ctx, const std::string &p_spec) {
	std::unique_ptr<EbxLibrary> lib = open_library(p_ctx, p_spec);
	if (!lib) {
		return 1;
	}
	WorkBudget budget;
	Result<SoldierDef> s = read_soldier_def(*lib, "characters/soldiers/mpsoldier", &budget);
	if (!s) {
		std::fprintf(stderr, "%s\n", s.error().message.c_str());
		return 1;
	}
	std::fprintf(stderr, "work %zu of %zu units\n", budget.used(), budget.limit());
	std::printf("blueprint\t%s\nphysics\t%s\n", s->blueprint.c_str(), s->physics.c_str());
	std::printf("max_health\t%g\nlow_health_threshold\t%g\nexplosion_damage_modifier\t%g\nregeneration_delay_modifier\t%g\nuse_prediction\t%d\n",
			s->max_health, s->low_health_threshold, s->explosion_damage_modifier, s->regeneration_delay_modifier, s->use_prediction ? 1 : 0);
	std::printf("regeneration_delay\t%g\nregeneration_rate\t%g\nimmortal_time_after_spawn\t%g\nman_down_time\t%g\nman_down_health_points\t%g\n"
				"post_revive_health\t%g\ntime_for_corpse\t%g\ninteractive_man_down\t%d\n",
			s->regeneration_delay, s->regeneration_rate, s->immortal_time_after_spawn, s->man_down_time, s->man_down_health_points,
			s->post_revive_health, s->time_for_corpse, s->interactive_man_down ? 1 : 0);
	std::string inputs;
	for (const int32_t i : s->abort_spawn_immortality_inputs) {
		inputs += (inputs.empty() ? "" : ",") + std::to_string(i);
	}
	std::printf("abort_spawn_immortality_inputs\t%s\n", inputs.c_str());
	std::printf("mass\t%g\nradius\t%g\nmax_ascend_angle\t%g\nwater_depth_limit\t%g\njump_penalty_time\t%g\njump_penalty_factor\t%g\n"
				"ladder_accept_angle\t%g\nladder_accept_angle_pitch\t%g\ninput_acceleration\t%g\nradius_to_predict_collision_on_characters\t%g\n"
				"allow_pose_change_during_transition\t%d\ndefault_state\t%s\n",
			s->mass, s->radius, s->max_ascend_angle, s->water_depth_limit, s->jump_penalty_time, s->jump_penalty_factor, s->ladder_accept_angle,
			s->ladder_accept_angle_pitch, s->input_acceleration, s->radius_to_predict_collision_on_characters,
			s->allow_pose_change_during_transition ? 1 : 0, s->default_state.c_str());
	std::printf("sprint.continuous\t%d\nsprint.power_decrease_per_second\t%g\nsprint.power_increase_per_second\t%g\nsprint.minimum_power\t%g\n",
			s->continuous_sprint ? 1 : 0, s->sprint_power_decrease_per_second, s->sprint_power_increase_per_second, s->sprint_minimum_power);
	for (const PoseDef &p : s->poses) {
		std::printf("pose.%s\theight %g\teye %g\tstep %g\t%s\tbox_min (%g,%g,%g)\tbox_max (%g,%g,%g)\tyaw %g..%g\tpitch %g..%g\tthrottle %s\tto %s\n",
				soldier_pose_name(p.pose), p.height, p.eye_height, p.step_height, p.collision_type.c_str(), p.box_min_expand[0], p.box_min_expand[1],
				p.box_min_expand[2], p.box_max_expand[0], p.box_max_expand[1], p.box_max_expand[2], p.min_look_yaw, p.max_look_yaw, p.min_look_pitch,
				p.max_look_pitch, curve_text(p.throttle_curve).c_str(), pose_times(p).c_str());
	}
	for (const MovementStateDef &st : s->states) {
		for (const StatePoseSpeed &p : st.poses) {
			std::printf("state.%s.%s\tvelocity %g\tfwd %g back %g left %g right %g\taccel %g decel %g\tsprint_gain %g\tsprint_multiplier %g\n",
					st.type.c_str(), soldier_pose_name(p.pose), p.velocity, p.forward, p.backward, p.left, p.right, p.acceleration_gain,
					p.deceleration_gain, p.sprint_gain, p.sprint_multiplier);
		}
		print_scalars(("state." + st.type).c_str(), st.scalars);
	}
	std::printf("fall_damage.vertical\t%s\nfall_damage.horizontal\t%s\n", curve_text(s->fall_damage_vertical).c_str(),
			curve_text(s->fall_damage_horizontal).c_str());
	print_scalars("sprint_settings", s->sprint_settings);
	std::printf("body.sprint_multiplier\t%g\n", s->body_sprint_multiplier);
	print_scalars("suppression", s->suppression);
	print_scalars("spotting", s->spotting);
	print_scalars("spotting_target", s->spotting_target);
	for (const SpecializationDef &sp : s->specializations) {
		std::printf("specialization\t%s\t%s\t%g (default %g)\tsquad %d\t%s.%s\n", sp.unlock.c_str(), sp.id.c_str(), sp.value, sp.default_value,
				sp.squad ? 1 : 0, sp.target_type.c_str(), sp.target_field.empty() ? "?" : sp.target_field.c_str());
	}
	std::printf("derived.walk_speed\t%g\nderived.sprint_speed\t%g\nderived.crouch_speed\t%g\nderived.prone_speed\t%g\nderived.jump_height\t%g\n",
			s->ground_speed(SoldierPose::Stand).value_or(-1), s->sprint_speed().value_or(-1), s->ground_speed(SoldierPose::Crouch).value_or(-1),
			s->ground_speed(SoldierPose::Prone).value_or(-1), s->jump_height().value_or(-1));
	return 0;
}

std::string opt_text(const std::optional<double> &p_value) {
	if (!p_value) {
		return "-";
	}
	char b[32];
	std::snprintf(b, sizeof(b), "%g", *p_value);
	return b;
}

int cmd_mode(const EbxContext &p_ctx, const std::string &p_level, const std::string *p_mode) {
	std::unique_ptr<EbxLibrary> lib = open_level(p_ctx, p_level);
	if (!lib) {
		return 1;
	}
	if (!p_mode) {
		Result<std::vector<std::string>> modes = level_modes(*lib, p_level);
		if (!modes) {
			std::fprintf(stderr, "%s\n", modes.error().message.c_str());
			return 1;
		}
		for (const std::string &m : *modes) {
			std::printf("%s\n", m.c_str());
		}
		return 0;
	}
	const auto start = std::chrono::steady_clock::now();
	WorkBudget budget;
	Result<ModeDef> mode = read_mode_def(*lib, p_level, *p_mode, &budget);
	if (!mode) {
		std::fprintf(stderr, "%s\n", mode.error().message.c_str());
		return 1;
	}
	std::fprintf(stderr, "resolved in %.2f s, work %zu of %zu units\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
			budget.used(), budget.limit());
	const ModeDef &m = *mode;
	std::printf("level\t%s\nmode\t%s\nscopes\t%zu\nobjects\t%zu\nwalk_steps\t%zu\nquery_steps\t%zu\nunresolved_connections\t%zu\n", m.level_id.c_str(),
			m.mode_id.c_str(), m.scopes, m.objects, m.walk_steps, m.query_steps, m.unresolved_connections);
	for (const std::string &s : m.subworlds) {
		std::printf("subworld\t%s\n", s.c_str());
	}
	for (const InterfaceConstantDef &c : m.interface_constants) {
		if (c.value.rfind("LinearTransform", 0) == 0) {
			continue;
		}
		std::printf("interface\t%s\t%d\t%s\t%s\n", c.subworld.c_str(), c.id, c.name.empty() ? "-" : c.name.c_str(), c.value.c_str());
	}
	for (const TeamDef &t : m.teams) {
		std::printf("team\t%d\t%s\t%s\tkits %zu\n", t.team, t.team_data.c_str(), t.faction.c_str(), t.kits.size());
	}
	for (const TicketCounterDef &t : m.ticket_counters) {
		std::printf("tickets\tteam %d\tinitial %lld\tloss_per_min %g\tloss_start %g\tlost_all %g\tlost_all_points %g\thalt_equal %d\t%s\t%s\n", t.team,
				static_cast<long long>(t.initial_tickets), t.loss_per_min, t.loss_start, t.loss_when_lost_all, t.loss_when_lost_all_control_points,
				t.halt_on_equal_point_count ? 1 : 0, t.decrease_tickets.c_str(), t.prefab.c_str());
	}
	for (const CapturePointDef &c : m.capture_points) {
		std::printf("capture\t%s\t%s\towner %d\tarea %g\tget %g\tlose %g\tper_player %g\tmax %g\treturn %g\tradius %g\tmin_nr %lld\tenemy_loss %lld\t"
					"seesaw %d\tenabled %d\t%s\t%s\n",
				c.capturable_type.c_str(), c.location_sid.empty() ? "-" : c.location_sid.c_str(), c.initial_owner, c.area_value, c.time_to_get_control,
				c.time_to_lose_control, c.added_multiplier_per_player, c.max_capture_multiplier, c.return_multiplier, c.capture_radius,
				static_cast<long long>(c.min_nr_to_take_control), static_cast<long long>(c.enemy_ticket_loss_when_captured), c.seesaw ? 1 : 0,
				c.enabled ? 1 : 0, c.prefab.c_str(), c.is_hq() ? "hq" : "flag");
	}
	std::printf("flags\t%zu\tarea_value %s\n", m.flag_count(), opt_text(m.flag_area_value()).c_str());
	for (const LifeCounterDef &l : m.life_counters) {
		std::printf("lives\tteam %d\tattacker %d\tstart %lld\tbase_count %lld\tfallback %g\tsimple_reinforce %d\t%s\n", l.team, l.attacker ? 1 : 0,
				static_cast<long long>(l.starting_lives), static_cast<long long>(l.base_count), l.fallback_time, l.simple_reinforce ? 1 : 0,
				l.prefab.c_str());
	}
	for (const KillCounterDef &k : m.kill_counters) {
		std::printf("kills\tteam %d\tmax %lld\tenemy_weight %g\t%s\t%s\n", k.team, static_cast<long long>(k.max_kills), k.enemy_weight, k.type.c_str(),
				k.prefab.c_str());
	}
	for (const McomDef &c : m.mcoms) {
		std::printf("mcom\tarm %s\tdisarm %s\tradius %s\tfuse %s\tlong_fuse %s\t%s\n", opt_text(c.arm_time).c_str(), opt_text(c.disarm_time).c_str(),
				opt_text(c.radius).c_str(), opt_text(c.fuse).c_str(), c.long_fuse_switch ? (*c.long_fuse_switch ? "1" : "0") : "-", c.prefab.c_str());
	}
	for (const VehicleSpawnDef &v : m.vehicle_spawns) {
		std::printf("vehicle\t%s\tteam %d\tspawn_delay %g\tinitial %g\tenabled %d\n", v.blueprint.c_str(), v.team, v.spawn_delay, v.initial_spawn_delay,
				v.enabled ? 1 : 0);
	}
	if (m.size) {
		std::string teams;
		for (const GameModeTeamSizeDef &t : m.size->teams) {
			teams += (teams.empty() ? "" : " ") + std::to_string(t.players) + "/" + std::to_string(t.squad_size);
		}
		std::printf("size\t%s\t%s\tplayers %lld\trounds %lld\tforce_squad %d\tsquad %lld\tteams %s%s\n", m.size->name.c_str(),
				m.size->short_name.c_str(), static_cast<long long>(m.size->player_count), static_cast<long long>(m.size->rounds_per_map),
				m.size->force_squad ? 1 : 0, static_cast<long long>(m.size->squad_size()), teams.c_str(), m.size->fallback ? "\tfallback" : "");
	} else {
		std::printf("size\t-\n");
	}
	return 0;
}

int cmd_kits(const EbxContext &p_ctx, const std::string &p_level, const std::string &p_mode, bool p_all) {
	std::unique_ptr<EbxLibrary> lib = open_level(p_ctx, p_level);
	if (!lib) {
		return 1;
	}
	// One budget for the mode, the progression tables and every kit the teams list.
	WorkBudget budget;
	Result<ModeDef> mode = read_mode_def(*lib, p_level, p_mode, &budget);
	if (!mode) {
		std::fprintf(stderr, "%s\n", mode.error().message.c_str());
		return 1;
	}
	Result<UnlockProgression> read_progression = read_unlock_progression(*lib, &budget);
	if (!read_progression) {
		std::fprintf(stderr, "%s\n", read_progression.error().message.c_str());
		return 1;
	}
	const UnlockProgression &progression = *read_progression;
	std::printf("progression\t%zu tables\t%zu gated unlocks\n", progression.tables, progression.gates.size());
	int failed = 0;
	for (const TeamDef &t : mode->teams) {
		std::printf("team\t%d\t%s\t%s\tsoldier %s\n", t.team, t.team_data.c_str(), t.faction.c_str(), t.soldier.c_str());
		for (const std::string &asset : t.kits) {
			Result<KitDef> kit = read_kit_def(*lib, asset, &progression, &budget);
			if (!kit) {
				std::printf("kit\t%s\terror: %s\n", asset.c_str(), kit.error().message.c_str());
				++failed;
				continue;
			}
			std::printf("kit\t%s\t%s\t%s\tspecializations %zu\tvisuals %zu\n", kit->asset.c_str(), kit->label_sid.c_str(), kit->hud_icon.c_str(),
					kit->specializations.size(), kit->visual_unlocks);
			for (size_t i = 0; i < kit->weapon_slots.size(); ++i) {
				const KitSlot &slot = kit->weapon_slots[i];
				const char *slot_name = i < kWeaponSlotCount ? weapon_slot_name(static_cast<WeaponSlot>(i)) : "extra";
				if (slot.unlocks.empty()) {
					std::printf("slot\t%zu\t%s\t-\n", i, slot_name);
					continue;
				}
				const KitUnlock *d = i < kWeaponSlotCount ? kit->default_unlock(static_cast<WeaponSlot>(i)) : nullptr;
				const char *none = kit->progression_known ? "none" : "unknown";
				std::printf("slot\t%zu\t%s\t%zu options\tdefault %s\t%s\t%s\n", i, slot_name, slot.unlocks.size(), d ? d->unlock.c_str() : none,
						(d && !d->weapon.empty()) ? d->weapon.c_str() : "-", slot.ui_category_sid.c_str());
				if (p_all) {
					for (const KitUnlock &u : slot.unlocks) {
						std::printf("unlock\t%zu\t%s\t%s\t%s\t%s%s\n", i, u.unlock.c_str(), u.type.c_str(), u.weapon.empty() ? "-" : u.weapon.c_str(),
								unlock_gate_name(u.gate), u.variant ? " variant" : "");
					}
				}
			}
			if (p_all) {
				for (const KitUnlock &u : kit->specializations) {
					std::printf("specialization\t%s\n", u.unlock.c_str());
				}
			}
		}
	}
	std::fprintf(stderr, "work %zu of %zu units\n", budget.used(), budget.limit());
	return failed == 0 ? 0 : 1;
}

int cmd_vehicle(const EbxContext &p_ctx, const std::string &p_partition, const std::string &p_spec) {
	std::unique_ptr<EbxLibrary> lib = open_library(p_ctx, p_spec);
	if (!lib) {
		return 1;
	}
	WorkBudget budget;
	Result<VehicleDef> v = read_vehicle_def(*lib, p_partition, &budget);
	if (!v) {
		std::fprintf(stderr, "%s\n", v.error().message.c_str());
		return 1;
	}
	std::fprintf(stderr, "work %zu of %zu units\n", budget.used(), budget.limit());
	std::printf("blueprint\t%s\nname_sid\t%s\nminimap_icon\t%s\ncomponents\t%zu\n", v->blueprint.c_str(), v->name_sid.c_str(), v->minimap_icon.c_str(),
			v->components);
	std::printf("body_mass\t%g\ngravity_modifier\t%g\ncenter_of_mass\t%g %g %g\ninertia_modifier\t%g %g %g\n", v->body_mass, v->gravity_modifier,
			v->center_of_mass[0], v->center_of_mass[1], v->center_of_mass[2], v->inertia_modifier[0], v->inertia_modifier[1], v->inertia_modifier[2]);
	const auto zone = [](const char *p_name, const VehicleHealthZoneDef &p_zone) {
		std::printf("zone.%s\thealth %g\tshield %g\tangle %g\tmultiplier %g\tuse_angle %d\n", p_name, p_zone.max_health, p_zone.max_shield_health,
				p_zone.min_damage_angle, p_zone.damage_angle_multiplier, p_zone.use_damage_angle ? 1 : 0);
	};
	zone("front", v->front);
	zone("rear", v->rear);
	zone("left", v->left);
	zone("right", v->right);
	std::printf("zones_share_damage\t%d\ndisabled_damage_threshold\t%g\nemergency_repair_health\t%g\nregeneration\t%g s\t%g/s\ndecay\t%g s\t%g/s\n"
				"armor_multiplier\t%g\nvelocity_damage\t%g\t%g\nwater_damage\t%g\nupside_down\t%g deg\t%g\t%g s\nkill_soldier_speed\t%g\n",
			v->health_zones_share_damage ? 1 : 0, v->disabled_damage_threshold, v->emergency_repair_health, v->regeneration_delay, v->regeneration_rate,
			v->decay_delay, v->decay_rate, v->armor_multiplier, v->velocity_damage_threshold, v->velocity_damage_magnifier, v->water_damage,
			v->upside_down_angle, v->upside_down_damage, v->upside_down_damage_delay, v->kill_soldier_collision_speed_threshold);
	for (size_t i = 0; i < v->seats.size(); ++i) {
		const VehicleSeatDef &s = v->seats[i];
		std::printf("seat\t%zu\torder %d\t%s\t%s\tradius %g\tshows_soldier %d\town_weapon %d\tweapons %zu\n", i, s.entry_order, s.seat_type.c_str(),
				s.component_type.c_str(), s.entry_radius, s.shows_soldier ? 1 : 0, s.own_weapon ? 1 : 0, s.weapons.size());
		for (const VehicleCameraDef &c : s.cameras) {
			std::printf("camera\t%zu\t%s\t%s\tfov %g\tfirst_person %d\tyaw %g/%g\tpitch %g/%g\tzoom %s\n", i, c.camera_type.c_str(), c.camera.c_str(),
					c.field_of_view, c.first_person ? 1 : 0, c.left_yaw, c.right_yaw, c.up_pitch, c.down_pitch,
					c.zoom_field_of_view ? std::to_string(*c.zoom_field_of_view).c_str() : "-");
		}
	}
	for (size_t i = 0; i < v->turrets.size(); ++i) {
		const VehicleTurretDef &t = v->turrets[i];
		std::printf("turret\t%zu\tparent %d\tseat %d\taxis %d\t%s\tconstrained %d\t%g..%g\tworld_lock %g\tmomentum %g/%g/%g\n", i, t.parent, t.seat,
				t.rotation_axis, t.rotation_input.c_str(), t.constrained ? 1 : 0, t.min_angle, t.max_angle, t.world_space_lock_efficiency,
				t.angular_momentum_multiplier, t.angular_momentum_threshold, t.angular_momentum_damping);
	}
	for (size_t i = 0; i < v->weapons.size(); ++i) {
		const VehicleWeaponDef &w = v->weapons[i];
		std::printf("weapon\t%zu\t%s\t%s\t%s\tseat %d\tturret %d\n", i, w.damage_giver_name.c_str(), w.firing.c_str(), w.classification.c_str(), w.seat,
				w.turret);
	}
	return 0;
}

int cmd_scoring(const EbxContext &p_ctx, const std::string &p_spec) {
	std::unique_ptr<EbxLibrary> lib = open_library(p_ctx, p_spec);
	if (!lib) {
		return 1;
	}
	Result<ScoringTable> table = read_scoring(*lib);
	if (!table) {
		std::fprintf(stderr, "%s\n", table.error().message.c_str());
		return 1;
	}
	for (const ScoringEntryDef &e : table->entries) {
		std::printf("%s\t%g\t%s\t%s\n", e.sid.c_str(), e.score, e.measuring.c_str(), e.bucket.c_str());
	}
	return 0;
}

} // namespace

const char *gameplay_usage() {
	return "  soldier [<sb>]\n"
		   "  mode <level> [<mode>]\n"
		   "  kits <level> <mode> [--all]\n"
		   "  vehicle <partition> [<sb[,sb..]|@all-mp>]\n"
		   "  scoring [<sb>]\n";
}

int dispatch_gameplay(const EbxContext &p_ctx, const std::string &p_cmd, const std::vector<std::string> &p_args) {
	if (p_cmd == "soldier" && p_args.size() <= 1) {
		return cmd_soldier(p_ctx, p_args.empty() ? kDefaultSuperbundle : p_args[0]);
	}
	if (p_cmd == "mode" && (p_args.size() == 1 || p_args.size() == 2)) {
		return cmd_mode(p_ctx, p_args[0], p_args.size() == 2 ? &p_args[1] : nullptr);
	}
	if (p_cmd == "kits" && (p_args.size() == 2 || (p_args.size() == 3 && p_args[2] == "--all"))) {
		return cmd_kits(p_ctx, p_args[0], p_args[1], p_args.size() == 3);
	}
	if (p_cmd == "vehicle" && (p_args.size() == 1 || p_args.size() == 2)) {
		return cmd_vehicle(p_ctx, p_args[0], p_args.size() == 2 ? p_args[1] : std::string("@all-mp"));
	}
	if (p_cmd == "scoring" && p_args.size() <= 1) {
		return cmd_scoring(p_ctx, p_args.empty() ? kDefaultSuperbundle : p_args[0]);
	}
	return -1;
}

} // namespace bf3dump
