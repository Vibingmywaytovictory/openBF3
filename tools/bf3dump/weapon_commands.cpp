// bf3dump weapon / weapons: the WeaponDef resolver and material grid of bf3_core, printed. The block
// is bf3_core's format_weapon (gameplay/weapon_format.h), the same one the in-game inspector shows.
#include "weapon_commands.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/weapon_catalog.h"
#include "bf3/core/gameplay/weapon_format.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/ids.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/ui/text_db.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

using namespace bf3::core;
using namespace bf3::core::gameplay;

namespace bf3dump {

namespace {

std::string num(double p_value) {
	return format_number(p_value);
}

std::string fixed(double p_value, int p_digits) {
	return format_fixed(p_value, p_digits);
}

bool open_libraries(const EbxContext &p_ctx, const std::string &p_level, WeaponSources &r_sources) {
	Result<WeaponSources> s = open_weapon_sources(p_ctx.install, p_level);
	if (!s) {
		std::fprintf(stderr, "%s\n", s.error().message.c_str());
		return false;
	}
	r_sources = std::move(*s);
	if (!r_sources.ui_error.empty()) {
		std::fprintf(stderr, "%s\n", r_sources.ui_error.c_str());
	}
	return true;
}

std::optional<MaterialGrid> load_grid(WeaponSources &r_sources) {
	Result<MaterialGrid> grid = load_level_grid(r_sources);
	if (!grid) {
		std::fprintf(stderr, "%s\n", grid.error().message.c_str());
		return std::nullopt;
	}
	return std::move(*grid);
}

bool parse_args(const std::vector<std::string> &p_args, bool p_need_partition, std::string &r_partition, std::vector<std::string> &r_unlocks,
		std::string &r_level, bool *r_name = nullptr) {
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
		} else if (a == "--name" && r_name != nullptr) {
			*r_name = true;
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

} // namespace

int cmd_weapon(const EbxContext &p_ctx, const std::vector<std::string> &p_args) {
	std::string partition;
	std::string level;
	bool with_name = false;
	WeaponResolveOptions options;
	if (!parse_args(p_args, true, partition, options.unlocks, level, &with_name)) {
		return 2;
	}
	WeaponSources libs;
	if (!open_libraries(p_ctx, level, libs)) {
		return 1;
	}
	partition = to_lower_ascii(partition);
	// A blueprint resolves through its base variant (for the identifier and the name SID).
	const std::string target = weapon_resolve_target(libs, partition);
	if (target != partition) {
		std::fprintf(stderr, "%s: base variant %s\n", partition.c_str(), target.c_str());
		partition = target;
	}
	options.ui = libs.ui.get();
	Result<WeaponDef> w = resolve_weapon(*libs.level, partition, options);
	if (!w) {
		std::fprintf(stderr, "%s\n", w.error().message.c_str());
		return 1;
	}
	const std::optional<MaterialGrid> grid = load_grid(libs);
	Result<std::vector<HitCapsule>> capsules = read_hit_capsules(*libs.level);
	if (!capsules) {
		std::fprintf(stderr, "hit capsules: %s\n", capsules.error().message.c_str());
	}
	if (grid) {
		const MaterialGrid::LoadReport &r = grid->report();
		std::fprintf(stderr, "grid %s: %zu rows, %zu pairs referenced, %zu missing from the level's container, %zu from fallback, %zu unresolved\n",
				r.grid.c_str(), grid->row_count(), r.grid_pairs, r.unresolved_in_level, r.from_fallback, r.unresolved);
	}
	// --name: the localised name from the text database (an extra "name" line).
	std::optional<ui::TextDatabase> text;
	if (with_name) {
		Result<ui::TextDatabase> db = ui::open_text_database(p_ctx.install);
		if (!db) {
			std::fprintf(stderr, "text database: %s\n", db.error().message.c_str());
			return 1;
		}
		text = std::move(*db);
	}
	const std::string block = format_weapon(*w, grid ? &*grid : nullptr, capsules ? &*capsules : nullptr, text ? &*text : nullptr);
	std::fwrite(block.data(), 1, block.size(), stdout);
	return 0;
}

int cmd_weapons(const EbxContext &p_ctx, const std::vector<std::string> &p_args) {
	std::string unused;
	std::string level;
	std::vector<std::string> unlocks;
	if (!parse_args(p_args, false, unused, unlocks, level) || !unlocks.empty()) {
		return 2;
	}
	WeaponSources libs;
	if (!open_libraries(p_ctx, level, libs)) {
		return 1;
	}
	libs.level->load_all();
	const std::vector<std::string> variants = list_weapon_variants(*libs.level);
	const std::optional<MaterialGrid> grid = load_grid(libs);
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
			"ok", w.blueprint, w.name_sid.empty() ? "-" : w.name_sid, w.weapon_class, format_fire_modes(f), num(f.rate_of_fire), fixed(effective_rpm(f), 2),
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
