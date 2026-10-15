// The WeaponDef text block shared by bf3dump and the in-game inspector (moved from
// tools/bf3dump/weapon_commands.cpp; the output is unchanged byte for byte).
#include "bf3/core/gameplay/weapon_format.h"

#include "bf3/core/ui/text_db.h"

#include <cmath>
#include <cstdio>
#include <string_view>

namespace bf3::core::gameplay {

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
			num(p_e.blast_impulse) + ", shockwave " + num(p_e.shockwave_damage) + " dmg r " + num(p_e.shockwave_radius) + " (" + p_e.partition + ")" +
			(p_e.material.present ? (p_e.material.resolved ? "  pair property " + std::to_string(p_e.material.property()) : std::string("  pair unresolved"))
								 : std::string());
}

} // namespace

std::string format_number(double p_value) {
	return num(p_value);
}

std::string format_fixed(double p_value, int p_digits) {
	return fixed(p_value, p_digits);
}

std::string format_fire_modes(const FireFunction &p_fire) {
	return modes(p_fire);
}

std::string weapon_display_name(const WeaponDef &p_weapon, const ui::TextDatabase *p_text) {
	if (p_text == nullptr || p_weapon.name_sid.empty()) {
		return {};
	}
	return std::string(p_text->translate(p_weapon.name_sid));
}

std::string format_weapon(const WeaponDef &p_w, const MaterialGrid *p_grid, const std::vector<HitCapsule> *p_capsules, const ui::TextDatabase *p_text) {
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
	if (p_text != nullptr) {
		// The localised name; a SID without a usable entry shows as itself (plan §2.1).
		line("name", p_w.name_sid.empty() ? std::string("-")
				: p_text->resolves(p_w.name_sid) ? weapon_display_name(p_w, p_text)
												  : p_w.name_sid + "  (not in the text database)");
	}
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
					" s  delay " + num(f.reload_delay) + " s  threshold " + num(f.reload_threshold) + "  " + f.reload_logic +
					(f.hold_off_reload_until_fire_release ? "  hold off until fire release" : "") +
					(f.force_reload_action_on_fire_trigger ? "  force on fire trigger" : ""));
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

} // namespace bf3::core::gameplay
