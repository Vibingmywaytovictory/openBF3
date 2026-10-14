// bf3dump weapon / weapons: the WeaponDef resolver and material grid of bf3_core, printed.
#include "weapon_commands.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/ids.h"
#include "bf3/core/thread_pool.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

using namespace bf3::core;
using namespace bf3::core::gameplay;

namespace bf3dump {

namespace {

std::string num(double p_value) {
	if (std::isnan(p_value)) {
		return "-";
	}
	char b[32];
	std::snprintf(b, sizeof(b), "%.7g", p_value);
	return b;
}

std::string fixed(double p_value, int p_digits) {
	char b[32];
	std::snprintf(b, sizeof(b), "%.*f", p_digits, p_value);
	return b;
}

std::string vec(const Vec3 &p_v) {
	return "(" + num(p_v[0]) + ", " + num(p_v[1]) + ", " + num(p_v[2]) + ")";
}

std::string hex32(uint32_t p_value) {
	char b[16];
	std::snprintf(b, sizeof(b), "%08x", p_value);
	return b;
}

std::string modes(const FireFunction &p_fire) {
	std::string out = fire_logic_name(p_fire.logic);
	if (!p_fire.extra_modes.empty()) {
		out += " + [";
		for (size_t i = 0; i < p_fire.extra_modes.size(); ++i) {
			out += i ? ", " : "";
			out += fire_logic_name(p_fire.extra_modes[i]);
		}
		out += "]";
	}
	return out;
}

std::string dispersion(const Dispersion &p_d) {
	return num(p_d.min_angle) + ".." + num(p_d.max_angle) + " +" + num(p_d.increase_per_shot) + " -" + num(p_d.decrease_per_second) + "/s";
}

std::string sway_modifier(const SwayModifier &p_m) {
	return "min x" + num(p_m.min_angle) + " max " + (p_m.max_angle == 0 ? std::string("(unset)") : "x" + num(p_m.max_angle)) + " +shot x" +
			num(p_m.increase_per_shot) + " -sec x" + num(p_m.decrease_per_second) + " recoil up x" + num(p_m.recoil_magnitude) + " horiz x" +
			num(p_m.recoil_angle);
}

struct Libraries {
	std::unique_ptr<EbxLibrary> level;
	std::unique_ptr<EbxLibrary> ui;
	std::unique_ptr<EbxLibrary> fallback; // container pairs for the delta (XP2-4) levels
	std::string level_partition; // "levels/mp_subway/mp_subway"
};

std::unique_ptr<EbxLibrary> open_lazy(const EbxContext &p_ctx, const std::string &p_spec) {
	Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(p_ctx.install, expand_superbundles(*p_ctx.install, p_spec));
	if (!lib) {
		std::fprintf(stderr, "%s: %s\n", p_spec.c_str(), lib.error().message.c_str());
		return nullptr;
	}
	return std::move(*lib);
}

bool open_libraries(const EbxContext &p_ctx, const std::string &p_level, Libraries &r_libs) {
	r_libs.level = open_lazy(p_ctx, p_level);
	r_libs.ui = open_lazy(p_ctx, "Win32/UI");
	std::string lp = to_lower_ascii(p_level);
	if (lp.rfind("win32/", 0) == 0) {
		lp = lp.substr(6);
	}
	r_libs.level_partition = lp;
	return r_libs.level != nullptr;
}

// The level's grid; for a delta level whose container copy lacks pairs, again with MP_Subway's
// container as the fallback (handoff 01 XP2/3/4 caveat).
std::optional<MaterialGrid> load_grid(const EbxContext &p_ctx, Libraries &r_libs) {
	const std::optional<std::string> name = MaterialGrid::grid_of_level(*r_libs.level, r_libs.level_partition);
	if (!name) {
		std::fprintf(stderr, "%s: no RuntimeMaterialGrid\n", r_libs.level_partition.c_str());
		return std::nullopt;
	}
	Result<MaterialGrid> grid = MaterialGrid::load(*r_libs.level, *name);
	if (grid && grid->report().unresolved_in_level > 0) {
		r_libs.fallback = open_lazy(p_ctx, kDefaultWeaponLevel);
		if (r_libs.fallback) {
			const EbxResolver *fb[] = { r_libs.fallback.get() };
			grid = MaterialGrid::load(*r_libs.level, *name, fb);
		}
	}
	if (!grid) {
		std::fprintf(stderr, "%s\n", grid.error().message.c_str());
		return std::nullopt;
	}
	return std::move(*grid);
}

bool parse_args(const std::vector<std::string> &p_args, bool p_need_partition, std::string &r_partition, std::vector<std::string> &r_unlocks,
		std::string &r_level) {
	r_level = kDefaultWeaponLevel;
	for (size_t i = 0; i < p_args.size(); ++i) {
		const std::string &a = p_args[i];
		if ((a == "--unlock" || a == "--level") && i + 1 < p_args.size()) {
			if (a == "--unlock") {
				r_unlocks.push_back(p_args[i + 1]);
			} else {
				r_level = p_args[i + 1];
			}
			++i;
		} else if (p_need_partition && r_partition.empty() && a.rfind("--", 0) != 0) {
			r_partition = a;
		} else {
			std::fprintf(stderr, "unexpected argument: %s\n", a.c_str());
			return false;
		}
	}
	if (p_need_partition && r_partition.empty()) {
		std::fprintf(stderr, "weapon: partition missing\n");
		return false;
	}
	return true;
}

void print_state(std::string &r_out, const char *p_label, const SwayState &p_s) {
	static const char *kMoves[] = { "base", "moving", "jumping", "sprinting", "vault small", "vault medium" };
	r_out += "  ";
	r_out += p_label;
	r_out += '\n';
	for (size_t m = 0; m < size_t(SwayMove::Count); ++m) {
		if (p_s.has_move[m]) {
			r_out += "    " + std::string(kMoves[m]) + std::string(14 - std::string_view(kMoves[m]).size(), ' ') + dispersion(p_s.move[m]) + "\n";
		}
	}
	const Recoil &r = p_s.recoil;
	r_out += "    recoil        up " + num(r.up_per_shot) + " (max " + num(r.up_max) + ")  horizontal " + num(r.horizontal_min) + ".." +
			num(r.horizontal_max) + " (left " + num(r.horizontal_max) + ", right " + num(-r.horizontal_min) + ", max " + num(r.horizontal_amplitude_max) +
			")  decrease " + num(r.decrease_factor) + "\n";
}

std::string explosion(const Explosion &p_e) {
	if (!p_e.present) {
		return "-";
	}
	return num(p_e.blast_damage) + " dmg, inner " + num(p_e.inner_blast_radius) + " m, radius " + num(p_e.blast_radius) + " m, impulse " +
			num(p_e.blast_impulse) + ", shockwave " + num(p_e.shockwave_damage) + " dmg r " + num(p_e.shockwave_radius) + " (" + p_e.partition + ")";
}

std::string format_weapon(const WeaponDef &p_w, const MaterialGrid *p_grid, const std::vector<HitCapsule> *p_capsules) {
	const FireFunction &f = p_w.fire;
	const ProjectileDef &p = p_w.projectile;
	std::string o;
	auto line = [&](const char *p_key, const std::string &p_value) {
		o += p_key;
		o += std::string(p_key[0] ? 13 - std::string_view(p_key).size() : 0, ' ');
		o += p_value;
		o += '\n';
	};
	line("weapon", (p_w.variant.empty() ? std::string("(blueprint only)") : p_w.variant) + "  identifier " + std::to_string(p_w.unlock_identifier));
	line("blueprint", p_w.blueprint + "  \"" + p_w.blueprint_name + "\"  damage giver \"" + p_w.damage_giver_name + "\"");
	line("name sid", p_w.name_sid.empty() ? std::string("-") : p_w.name_sid + "  (hash " + hex32(p_w.name_sid_hash) + ")");
	std::string unlocks;
	for (const std::string &u : p_w.active_unlocks) {
		unlocks += (unlocks.empty() ? "" : ", ") + u;
	}
	line("unlocks", unlocks.empty() ? "-" : unlocks);
	for (const std::string &m : p_w.applied_modifiers) {
		line("modifier", m);
	}
	line("class", p_w.weapon_class + "  anim " + p_w.anim_base_set + (p_w.weapon_anim_type.empty() ? "" : " / " + p_w.weapon_anim_type) + "  render fov " +
					num(p_w.render_fov) + "  zoom fov " + (p_w.zoom_render_fov > 0 ? num(p_w.zoom_render_fov) : "-"));
	line("flags", std::string("silenced ") + (p_w.silenced ? "yes" : "no") + "  breath control " + (p_w.enable_breath_control ? "yes" : "no") +
					"  supported shooting " + (p_w.can_be_in_supported_shooting ? "yes" : "no"));
	line("aiming", p_w.aiming_controller);
	line("sound", p_w.sound);
	line("firing data", p_w.firing_data);
	line("deploy", num(p_w.deploy_time) + " s  alt " + num(p_w.alt_deploy_time) + " s  zoom blocked " + num(p_w.disable_zoom_on_deploy_time) +
					" s  support delay " + num(p_w.support_delay_stand) + "/" + num(p_w.support_delay_prone) + " s");
	line("fire modes", modes(f));
	line("rate", num(f.rate_of_fire) + " rpm  interval " + fixed(shot_interval(f) * 1000.0, 1) + " ms  effective " + fixed(effective_rpm(f), 1) +
					" rpm  client multiplier " + num(f.client_fire_rate_multiplier));
	line("burst", std::to_string(f.bullets_per_burst) + " @ " + num(f.rate_of_fire_for_burst) + " rpm (" + fixed(burst_interval(f) * 1000.0, 1) + " ms)");
	line("bolt", "delay " + num(f.bolt_action_delay) + " + time " + num(f.bolt_action_time) + " s" +
					(f.logic == FireLogic::SingleWithBoltAction ? std::string("  (bolt action)") : std::string("  (inert: not a bolt-action mode)")));
	if (f.logic == FireLogic::HoldAndRelease) {
		line("hold", "delay " + num(f.hold_delay) + " s  max " + num(f.hold_max_time) + " s  killed power x" + num(f.hold_killed_power_modifier));
	}
	line("reload", std::string(reload_type_name(f.reload_type)) + "  tactical " + num(f.reload_time_bullets_left) + " s  empty " + num(f.reload_time) +
					" s  delay " + num(f.reload_delay) + " s  threshold " + num(f.reload_threshold) + "  " + f.reload_logic);
	line("ammo", std::to_string(f.magazine_capacity) + " x " + std::to_string(f.number_of_magazines) + "  trace every " +
					std::to_string(f.trace_frequency) + "  auto replenish " + num(f.auto_replenish_delay) + " s");
	line("shot", "speed " + vec(f.initial_speed) + "  at " + vec(f.initial_position) + "  pellets " + std::to_string(f.bullets_per_shell) +
					"  per shot " + std::to_string(f.bullets_per_shot) + "  spawn delay " + num(f.spawn_delay) + " s  camera spawn " +
					(f.active_force_spawn_to_camera ? "yes" : "no"));
	line("overheat", "heat/bullet " + num(f.heat_per_bullet) + "  drop " + num(f.heat_drop_per_second) + "/s  threshold " +
					num(f.overheat_threshold) + "  penalty " + num(f.overheat_penalty_time) + " s");
	line("pellet cone", "stand " + dispersion(f.weapon_dispersion[0]) + "  crouch " + dispersion(f.weapon_dispersion[1]) + "  prone " +
					dispersion(f.weapon_dispersion[2]));
	line("projectile", std::string(projectile_kind_name(p.kind)) + "  " + p.type + "  " + p.partition + "  \"" + p.ammunition_type + "\"");
	if (p.kind == ProjectileKind::Bullet) {
		line("damage", num(p.start_damage) + " -> " + num(p.end_damage) + " over " + num(p.damage_falloff_start) + ".." + num(p.damage_falloff_end) +
						" m  instant hit " + (p.instant_hit ? "yes" : "no") + "  arm " + num(p.time_to_arm_explosion) + " s");
	}
	if (p.kind == ProjectileKind::Missile) {
		line("missile", "direct " + num(p.missile_damage) + "  engine " + num(p.engine_strength) + "  max speed " + num(p.max_speed) + "  engine ttl " +
						num(p.engine_time_to_live) + "  arm " + num(p.time_to_arm) + " s  drag " + num(p.drag));
	}
	if (p.kind == ProjectileKind::Grenade) {
		line("grenade", p.grenade_type + "  bounce x" + num(p.collision_speed_multiplier) + "  min bounce " + num(p.min_bounce_speed) + " m/s  fuse " +
						num(p.time_to_live) + " s  detonate on timeout " + (p.detonate_on_timeout ? "yes" : "no"));
	}
	const double v0 = muzzle_speed(p_w);
	line("ballistics", "v0 " + num(v0) + " m/s  g " + num(p.gravity) + "  ttl " + num(p.time_to_live) + " s  max range " + fixed(max_range(p_w), 1) +
					" m  drop 100 m " + fixed(drop_at(v0, p.gravity, 100), 3) + " m, 300 m " + fixed(drop_at(v0, p.gravity, 300), 3) + " m");
	line("explosion", explosion(p.explosion));
	line("dud", explosion(p.dud_explosion));
	line("suppression", p.suppression.present ? num(p.suppression.min_multiplier) + ".." + num(p.suppression.max_multiplier) + " over " +
							num(p.suppression.min_distance) + ".." + num(p.suppression.max_distance) + " m"
											  : std::string("-"));
	MaterialPairRef mat = p.material;
	if (p_grid != nullptr) {
		p_grid->complete(mat);
	}
	if (mat.present) {
		line("material", "pair " + mat.instance.to_string() + (mat.resolved ? "  property " + std::to_string(mat.property()) + " \"" +
																			 (p_grid ? std::string(p_grid->material_name(mat.property())) : std::string()) +
																			 "\"  material " + std::to_string(mat.material()) + "  flags " +
																			 hex32(mat.flags_and_index)
															  : std::string("  (unresolved)")));
	}
	if (p_grid != nullptr && p_capsules != nullptr && mat.resolved) {
		const BodyMultipliers b = body_multipliers(*p_grid, *p_capsules, mat.property());
		line("body", "head " + num(b.head) + "  body " + num(b.body) + "  chest " + num(b.chest) + "  legs " + num(b.legs) + "  (" + p_grid->report().grid +
						")");
	}
	const GunSway &s = p_w.sway;
	if (s.partition.empty()) {
		line("sway", "-");
		return o;
	}
	line("sway", s.partition + "  first shot recoil x" + num(s.first_shot_recoil_multiplier) + "  shooting recoil decrease x" +
					num(s.shooting_recoil_decrease_scale) + "  camera spring " + num(s.camera_spring_constant) + "/" + num(s.camera_spring_damping));
	static const char *kLabels[3][2] = { { "stand hip", "stand ads" }, { "crouch hip", "crouch ads" }, { "prone hip", "prone ads" } };
	for (size_t st = 0; st < 3; ++st) {
		for (size_t z = 0; z < 2; ++z) {
			print_state(o, kLabels[st][z], s.state[st][z]);
		}
	}
	static const char *kTransitions[] = { "prone>crouch", "prone>stand", "crouch>prone", "crouch>stand", "stand>crouch", "stand>prone" };
	for (size_t t = 0; t < size_t(StanceTransition::Count); ++t) {
		o += "  " + std::string(kTransitions[t]) + std::string(14 - std::string_view(kTransitions[t]).size(), ' ') +
				dispersion(s.transitions[t].max_penalty) + "  cool down " + num(s.transitions[t].cool_down) + " s\n";
	}
	o += "  suppressed hip " + sway_modifier(s.suppression[0]) + "\n";
	o += "  suppressed ads " + sway_modifier(s.suppression[1]) + "\n";
	o += "  modifiers     " + std::to_string(s.modifiers_total) + " in data, " + std::to_string(s.applied.size()) + " applied, " +
			std::to_string(s.conditional.size()) + " conditional\n";
	for (const SwayGatedModifier &m : s.applied) {
		o += "    applied     " + m.unlock + "  stand ads: " + sway_modifier(m.mod[0][1]) + "\n";
	}
	for (const SwayGatedModifier &m : s.conditional) {
		o += "    conditional " + m.unlock + (m.only_in_supported_shooting ? "  (supported)" : "") + (m.only_on_weapon_light_enabled ? "  (light on)" : "") +
				"  stand ads: " + sway_modifier(m.mod[0][1]) + "\n";
	}
	return o;
}

} // namespace

int cmd_weapon(const EbxContext &p_ctx, const std::vector<std::string> &p_args) {
	std::string partition;
	std::string level;
	WeaponResolveOptions options;
	if (!parse_args(p_args, true, partition, options.unlocks, level)) {
		return 2;
	}
	Libraries libs;
	if (!open_libraries(p_ctx, level, libs)) {
		return 1;
	}
	partition = to_lower_ascii(partition);
	const EbxPartition *root = libs.level->partition_by_name(partition);
	if (root != nullptr && root->primary().is_a("SoldierWeaponBlueprint")) {
		// A blueprint resolves through its base variant (for the identifier and the name SID).
		libs.level->load_all();
		const std::string base = base_variant_of(*libs.level, partition);
		if (!base.empty()) {
			std::fprintf(stderr, "%s: base variant %s\n", partition.c_str(), base.c_str());
			partition = base;
		}
	}
	options.ui = libs.ui.get();
	Result<WeaponDef> w = resolve_weapon(*libs.level, partition, options);
	if (!w) {
		std::fprintf(stderr, "%s\n", w.error().message.c_str());
		return 1;
	}
	const std::optional<MaterialGrid> grid = load_grid(p_ctx, libs);
	Result<std::vector<HitCapsule>> capsules = read_hit_capsules(*libs.level);
	if (!capsules) {
		std::fprintf(stderr, "hit capsules: %s\n", capsules.error().message.c_str());
	}
	if (grid) {
		const MaterialGrid::LoadReport &r = grid->report();
		std::fprintf(stderr, "grid %s: %zu rows, %zu pairs referenced, %zu missing from the level's container, %zu from fallback, %zu unresolved\n",
				r.grid.c_str(), grid->row_count(), r.grid_pairs, r.unresolved_in_level, r.from_fallback, r.unresolved);
	}
	const std::string text = format_weapon(*w, grid ? &*grid : nullptr, capsules ? &*capsules : nullptr);
	std::fwrite(text.data(), 1, text.size(), stdout);
	return 0;
}

int cmd_weapons(const EbxContext &p_ctx, const std::vector<std::string> &p_args) {
	std::string unused;
	std::string level;
	std::vector<std::string> unlocks;
	if (!parse_args(p_args, false, unused, unlocks, level) || !unlocks.empty()) {
		return 2;
	}
	Libraries libs;
	if (!open_libraries(p_ctx, level, libs)) {
		return 1;
	}
	libs.level->load_all();
	const std::vector<std::string> variants = list_weapon_variants(*libs.level);
	const std::optional<MaterialGrid> grid = load_grid(p_ctx, libs);
	Result<std::vector<HitCapsule>> capsules = read_hit_capsules(*libs.level);
	WeaponResolveOptions options;
	options.ui = libs.ui.get();
	const std::vector<std::string> rows = parallel_map<std::string>(variants.size(), [&](size_t i) {
		std::string row = variants[i];
		Result<WeaponDef> r = resolve_weapon(*libs.level, variants[i], options);
		if (!r) {
			return row + "\terror\t" + r.error().message + "\n";
		}
		const WeaponDef &w = *r;
		const FireFunction &f = w.fire;
		const ProjectileDef &p = w.projectile;
		const SwayState &ads = w.sway.state[0][1];
		const SwayState &hip = w.sway.state[0][0];
		BodyMultipliers body{ NAN, NAN, NAN, NAN };
		MaterialPairRef mat = p.material;
		if (grid && capsules && grid->complete(mat)) {
			body = body_multipliers(*grid, *capsules, mat.property());
		}
		const std::string fields[] = {
			"ok", w.blueprint, w.name_sid.empty() ? "-" : w.name_sid, w.weapon_class, modes(f), num(f.rate_of_fire), fixed(effective_rpm(f), 2),
			std::to_string(f.bullets_per_burst) + "@" + num(f.rate_of_fire_for_burst), num(f.bolt_action_delay + f.bolt_action_time),
			num(muzzle_speed(w)), std::to_string(f.bullets_per_shell), std::to_string(f.magazine_capacity), std::to_string(f.number_of_magazines),
			num(f.reload_time_bullets_left), num(f.reload_time), num(f.reload_delay), reload_type_name(f.reload_type), num(f.reload_threshold),
			p.partition.empty() ? "-" : p.partition, projectile_kind_name(p.kind), num(p.start_damage), num(p.end_damage), num(p.damage_falloff_start),
			num(p.damage_falloff_end), num(p.gravity), num(p.time_to_live), fixed(max_range(w), 1), num(p.missile_damage),
			num(p.explosion.present ? p.explosion.blast_damage : 0), num(ads.move[0].min_angle), num(ads.move[0].max_angle),
			num(ads.move[0].increase_per_shot), num(hip.move[0].min_angle), num(hip.move[0].max_angle), num(hip.move[0].increase_per_shot),
			num(ads.recoil.up_per_shot), num(ads.recoil.horizontal_min), num(ads.recoil.horizontal_max), num(ads.recoil.decrease_factor),
			num(hip.recoil.up_per_shot), num(hip.recoil.decrease_factor),
			num(w.sway.first_shot_recoil_multiplier), num(f.weapon_dispersion[0].min_angle), num(p.suppression.max_multiplier), num(w.deploy_time),
			std::to_string(w.sway.applied.size()), num(body.head), num(body.body), num(body.chest), num(body.legs)
		};
		for (const std::string &x : fields) {
			row += '\t';
			row += x;
		}
		return row + "\n";
	});
	std::string out =
			"variant\tstatus\tblueprint\tname_sid\tclass\tmodes\trof\teff_rpm\tburst\tbolt_s\tv0\tpellets\tmag\tspare\treload_tac\treload_empty\treload_delay\t"
			"reload_type\treload_threshold\tprojectile\tkind\tdmg_start\tdmg_end\tfalloff_start\tfalloff_end\tgravity\tttl\tmax_range\tmissile_dmg\tblast_dmg\t"
			"ads_min\tads_max\tads_inc\thip_min\thip_max\thip_inc\trecoil_up\trecoil_hmin\trecoil_hmax\trecoil_dec\thip_recoil_up\thip_recoil_dec\tfsrm\tpellet_cone\tsuppression\tdeploy\t"
			"sway_mods\thead\tbody\tchest\tlegs\n";
	size_t errors = 0;
	for (const std::string &r : rows) {
		out += r;
		errors += r.find("\terror\t") != std::string::npos ? 1 : 0;
	}
	std::fwrite(out.data(), 1, out.size(), stdout);
	std::fprintf(stderr, "%zu weapon variants, %zu resolved, %zu errors\n", variants.size(), variants.size() - errors, errors);
	return errors == 0 ? 0 : 1;
}

} // namespace bf3dump
