#include "ui_commands.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/install.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_sources.h"
#include "bf3/core/ui/minimap.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace bf3::core;
using namespace bf3::core::ui;

namespace bf3dump {

namespace {

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

Result<std::unique_ptr<EbxLibrary>> open_spec(const std::shared_ptr<Install> &p_install, std::string_view p_spec) {
	return EbxLibrary::open(p_install, expand_superbundles(*p_install, p_spec));
}

const char *kind_name(ObjectiveKind p_kind) {
	return p_kind == ObjectiveKind::Hq ? "hq" : "flag";
}

int cmd_minimap(const EbxContext &p_ctx, const std::string &p_level, const std::string &p_mode) {
	const auto start = std::chrono::steady_clock::now();
	Result<MinimapLevel> level = load_minimap_level(p_ctx.install, p_level, p_mode);
	if (!level) {
		std::fprintf(stderr, "minimap %s %s: %s\n", p_level.c_str(), p_mode.c_str(), level.error().message.c_str());
		return 1;
	}
	const double t_level = seconds_since(start);
	Result<LevelMount> mount = mount_minimap_level(*p_ctx.install, p_level);
	if (!mount) {
		std::fprintf(stderr, "minimap %s: mount: %s\n", p_level.c_str(), mount.error().message.c_str());
		return 1;
	}
	const double t_mount = seconds_since(start);
	Result<MinimapImages> images = load_minimap_images(*mount, *level);
	if (!images) {
		std::fprintf(stderr, "minimap %s %s: images: %s\n", p_level.c_str(), p_mode.c_str(), images.error().message.c_str());
		return 1;
	}
	const double t_images = seconds_since(start);
	const MinimapData &d = level->data;
	std::printf("minimap\t%s\t%s\tpartition %s\n", d.level_id.c_str(), level->mode.c_str(), d.partition.c_str());
	std::printf("world\tcenter %g %g\tsize %g\trotation %g\trange %g\tair_radar_range %g\n", double(d.world_center[0]), double(d.world_center[1]),
			double(d.world_size), double(d.world_rotation), double(d.world_range), double(d.air_radar_range));
	std::printf("satellite\t%s\t%ux%u L8\ttint %g %g %g %g\n", d.satellite.c_str(), images->satellite.width, images->satellite.height,
			double(d.detail_texture_tint[0]), double(d.detail_texture_tint[1]), double(d.detail_texture_tint[2]), double(d.detail_texture_tint[3]));
	std::printf("detail\t%s\t%ux%u\tthreshold %g\n", d.detail.texture.empty() ? "-" : d.detail.texture.c_str(), images->detail.width, images->detail.height,
			double(d.detail.alpha_threshold));
	std::printf("vegetation\t%s\t%ux%u\tthreshold %g\n", d.vegetation.texture.empty() ? "-" : d.vegetation.texture.c_str(), images->vegetation.width,
			images->vegetation.height, double(d.vegetation.alpha_threshold));
	std::printf("combat_area_params\tthreshold %g\tdistance_scale %g\tcolor %g %g %g %g\tmultiply %s\tfade %s\tstreaming %d\n",
			double(d.combat_area_alpha_threshold), double(d.combat_area_distance_scale), double(d.combat_area_color[0]), double(d.combat_area_color[1]),
			double(d.combat_area_color[2]), double(d.combat_area_color[3]), d.combat_area_multiply_texture.c_str(), d.fade_texture.c_str(),
			d.use_streaming_minimap ? 1 : 0);
	for (size_t i = 0; i < level->combat_areas.size(); ++i) {
		const CombatAreaMask &m = level->combat_areas[i];
		size_t inside = 0;
		for (uint8_t t : images->combat_areas[i].texels) {
			inside += sdf_inside(t, d.combat_area_alpha_threshold) ? 1 : 0;
		}
		std::printf("combat_area\t%s\tprefix %s\tfield %s\t%ux%u\tinside %zu\tsurrounding %s\tentity %s\n", m.asset.c_str(), m.prefix.c_str(), m.texture.c_str(),
				images->combat_areas[i].width, images->combat_areas[i].height, inside, m.surrounding_texture.empty() ? "-" : m.surrounding_texture.c_str(),
				m.entity.c_str());
	}
	const MinimapMapping mapping = minimap_mapping(d);
	Result<std::vector<bool>> uni = combat_area_union(images->combat_areas, d.combat_area_alpha_threshold);
	std::printf("# objective\tkind\tx\ty\tz\tyaw_deg\towner\tlabel\tlocation\tu\tv\tinside(variants 0..7)\tentity\n");
	std::array<size_t, kMinimapMappingVariants> inside_count{};
	for (const MinimapObjective &o : level->objectives) {
		const MinimapUv uv = world_to_minimap_uv(mapping, o.position[0], o.position[2]);
		std::string flags;
		for (uint8_t v = 0; v < kMinimapMappingVariants; ++v) {
			bool in = false;
			if (uni) {
				const uint32_t side = images->combat_areas.front().width;
				const MinimapUv vu = world_to_minimap_uv_variant(mapping, o.position[0], o.position[2], v);
				if (std::isfinite(vu.u) && std::isfinite(vu.v) && vu.u >= 0 && vu.u < 1 && vu.v >= 0 && vu.v < 1) {
					const uint32_t x = std::min(side - 1, uint32_t(vu.u * side));
					const uint32_t y = std::min(side - 1, uint32_t(vu.v * side));
					in = (*uni)[size_t(y) * side + x];
				}
			}
			inside_count[v] += in ? 1 : 0;
			flags += in ? '1' : '0';
		}
		std::printf("objective\t%s\t%.2f\t%.2f\t%.2f\t%.1f\t%d\t%s\t%s\t%.4f\t%.4f\t%s\t%s\n", kind_name(o.kind), double(o.position[0]), double(o.position[1]),
				double(o.position[2]), double(o.yaw) * 57.29577951308232, o.initial_owner, o.label_sid.c_str(), o.location_sid.c_str(), double(uv.u),
				double(uv.v), flags.c_str(), o.entity.c_str());
	}
	std::printf("objectives_inside");
	for (size_t c : inside_count) {
		std::printf("\t%zu/%zu", c, level->objectives.size());
	}
	std::printf("\n");
	std::fprintf(stderr, "minimap: level %.2f s, mount %.2f s, images %.2f s, work %zu units\n", t_level, t_mount - t_level, t_images - t_mount, level->work);
	return 0;
}

int cmd_minimap_census(const EbxContext &p_ctx) {
	const auto start = std::chrono::steady_clock::now();
	Result<std::unique_ptr<EbxLibrary>> lib = open_spec(p_ctx.install, "@all-mp");
	if (!lib) {
		std::fprintf(stderr, "minimap --census: %s\n", lib.error().message.c_str());
		return 1;
	}
	int failures = 0;
	const std::vector<Level> levels = p_ctx.install->levels();
	std::printf("# level\tcenter_x\tcenter_z\tsize\tsatellite\tside\tthreshold\tdistance_scale\tdetail\tstreaming\n");
	for (const Level &l : levels) {
		const std::string key = to_lower_ascii(l.id);
		const EbxPartition *root = (*lib)->partition_by_name("levels/" + key + "/" + key);
		if (root == nullptr) {
			std::printf("level\t%s\tFAIL no LevelData partition\n", l.id.c_str());
			++failures;
			continue;
		}
		level::WorkBudget budget;
		Result<MinimapData> d = read_minimap_data(**lib, root->primary(), l.id, budget);
		if (!d) {
			std::printf("level\t%s\tFAIL %s\n", l.id.c_str(), d.error().message.c_str());
			++failures;
			continue;
		}
		Result<LevelMount> mount = p_ctx.install->mount_level(l.id);
		Result<L8Image> sat = mount ? (mount->load_all_bundles() ? load_l8_texture(*mount, d->satellite) : Result<L8Image>(make_error("bundles")))
									: Result<L8Image>(mount.error());
		std::printf("level\t%s\t%g\t%g\t%g\t%s\t%s\t%g\t%g\t%s\t%d\n", l.id.c_str(), double(d->world_center[0]), double(d->world_center[1]), double(d->world_size),
				d->satellite.c_str(), sat ? (std::to_string(sat->width) + "x" + std::to_string(sat->height)).c_str() : ("FAIL " + sat.error().message).c_str(),
				double(d->combat_area_alpha_threshold), double(d->combat_area_distance_scale), d->detail.texture.empty() ? "-" : d->detail.texture.c_str(),
				d->use_streaming_minimap ? 1 : 0);
		failures += sat ? 0 : 1;
	}
	Result<MinimapCensus> c = minimap_census(**lib);
	if (!c) {
		std::fprintf(stderr, "census: %s\n", c.error().message.c_str());
		return 1;
	}
	std::printf("census\tlevels %zu\tscanned %zu\tcombat_area_assets %zu (read %zu) on %zu levels\tnull_surroundings %zu\tnull_fields %zu\treferenced_fields %zu (decoded %zu)\t"
				"distance_field_assets %zu (decoded %zu)\tfailures %zu\n",
			levels.size(), c->partitions_scanned, c->combat_area_assets, c->combat_area_read, c->levels_with_areas, c->null_surroundings, c->null_distance_fields, c->referenced_fields,
			c->referenced_decoded, c->distance_field_assets, c->distance_field_decoded, c->failures.size());
	for (const std::string &f : c->failures) {
		std::printf("failure\t%s\n", f.c_str());
	}
	std::fprintf(stderr, "minimap --census: %.1f s\n", seconds_since(start));
	return failures == 0 && c->failures.empty() ? 0 : 1;
}

int cmd_hud_icons(const EbxContext &p_ctx) {
	Result<std::unique_ptr<EbxLibrary>> ui = open_spec(p_ctx.install, "Win32/UI");
	if (!ui) {
		std::fprintf(stderr, "hud-icons: %s\n", ui.error().message.c_str());
		return 1;
	}
	Result<HudIconAtlas> atlas = read_hud_icon_atlas(**ui);
	if (!atlas) {
		std::fprintf(stderr, "hud-icons: %s\n", atlas.error().message.c_str());
		return 1;
	}
	Result<Rgba8Image> img = load_hud_icon_texture((*ui)->mount(0), *atlas);
	const uint32_t w = img ? img->width : 256, h = img ? img->height : 1024;
	std::printf("atlas\t%s\ttexture %s\t%s\tentries %zu\twith_states %zu\tunknown_states %zu\n", atlas->partition.c_str(), atlas->texture.c_str(),
			img ? (std::to_string(img->width) + "x" + std::to_string(img->height) + " RGBA8").c_str() : ("FAIL " + img.error().message).c_str(),
			atlas->icons.size(), atlas->with_states(), atlas->unknown_states);
	for (size_t i = 0; i < atlas->icons.size(); ++i) {
		const HudIcon &icon = atlas->icons[i];
		std::printf("icon\t%zu\t%s\tstates %zu\n", i, icon.type.c_str(), icon.states.size());
		for (const HudIconStateFrames &s : icon.states) {
			std::printf("state\t%s\t%s\tfps %g\trotate %d\tframes %zu", icon.type.c_str(), std::string(hud_icon_state_name(s.state)).c_str(), double(s.frame_rate),
					s.should_rotate ? 1 : 0, s.frames.size());
			for (const HudIconUv &uv : s.frames) {
				const HudIconRect r = hud_icon_rect(uv, w, h);
				std::printf("\t(%d,%d,%d,%d)", r.x, r.y, r.width, r.height);
			}
			std::printf("\n");
		}
	}
	return img ? 0 : 1;
}

int cmd_item_icons(const EbxContext &p_ctx, const std::string &p_level) {
	Result<std::unique_ptr<EbxLibrary>> ui = open_spec(p_ctx.install, "Win32/UI");
	Result<std::unique_ptr<EbxLibrary>> lvl = open_spec(p_ctx.install, "Win32/Levels/" + p_level + "/" + p_level);
	if (!ui || !lvl) {
		std::fprintf(stderr, "item-icons: %s\n", (!ui ? ui.error() : lvl.error()).message.c_str());
		return 1;
	}
	Result<ItemIconIndex> index = ItemIconIndex::read(**ui);
	if (!index) {
		std::fprintf(stderr, "item-icons: %s\n", index.error().message.c_str());
		return 1;
	}
	(*lvl)->load_all();
	const std::vector<std::string> variants = gameplay::list_weapon_variants(**lvl);
	size_t weapon = 0, kit = 0, missing = 0;
	std::printf("# variant\tidentifier\tsource\tname_sid\ticon\tformat\tsize\n");
	for (const std::string &v : variants) {
		const std::optional<EbxValue> id = (*lvl)->partition_by_name(v)->primary().get("Identifier");
		const uint32_t identifier = id ? id->as_u32().value_or(0) : 0;
		const ItemIcon *icon = index->find(identifier);
		if (icon == nullptr) {
			std::printf("variant\t%s\t%u\tMISS\n", v.c_str(), identifier);
			++missing;
			continue;
		}
		Result<ItemIconImage> img = load_item_icon((*ui)->mount(0), *icon);
		const char *src = icon->source == ItemIconSource::Weapon ? "weapon" : icon->source == ItemIconSource::KitItem ? "kititem" : "vehicle";
		std::printf("variant\t%s\t%u\t%s\t%s\t%s\t%s\n", v.c_str(), identifier, src, icon->name_sid.c_str(), icon->res_name.c_str(),
				img ? (std::to_string(int(img->format)) + "\t" + std::to_string(img->width) + "x" + std::to_string(img->height)).c_str()
					: ("FAIL " + img.error().message).c_str());
		if (!img) {
			++missing;
		} else if (icon->source == ItemIconSource::Weapon) {
			++weapon;
		} else {
			++kit;
		}
	}
	std::printf("item_icons\tvariants %zu\tweapon %zu\tkititem %zu\tmisses %zu\tindex %zu ids\tduplicate_ids %zu\n", variants.size(), weapon, kit, missing,
			index->icons().size(), index->duplicate_ids());
	return missing == 0 ? 0 : 1;
}

int cmd_hud_sources(const EbxContext &p_ctx, bool p_unbound) {
	const auto start = std::chrono::steady_clock::now();
	Result<std::unique_ptr<EbxLibrary>> lib = open_spec(p_ctx.install, kHudSourceSuperbundles);
	if (!lib) {
		std::fprintf(stderr, "hud-sources: %s\n", lib.error().message.c_str());
		return 1;
	}
	std::vector<const LevelMount *> mounts;
	for (size_t i = 0; i < (*lib)->superbundle_count(); ++i) {
		mounts.push_back(&(*lib)->mount(i));
	}
	const std::vector<std::string> partitions = hud_component_partitions(mounts);
	Result<HudSourceTable> table = read_hud_source_table(**lib, partitions);
	if (!table) {
		std::fprintf(stderr, "hud-sources: %s\n", table.error().message.c_str());
		return 1;
	}
	std::fputs(format_hud_source_table(*table).c_str(), stdout);
	const HudBindingReport r = check_hud_bindings(*table);
	std::printf("bindings\trows %zu\tmatched %zu\tmodel %zu\tclient %zu\tunused %zu\tnot_in_install %zu\tnot_in_rows %zu\tunbound_components %zu\t"
				"unbound_sources %zu\n",
			r.rows, r.matched, r.kinds[0], r.kinds[1], r.kinds[2], r.not_in_install.size(), r.not_in_rows.size(), r.unbound_components.size(), r.unbound_sources);
	for (const std::string &s : r.not_in_install) {
		std::printf("not_in_install\t%s\n", s.c_str());
	}
	for (const std::string &s : r.not_in_rows) {
		std::printf("not_in_rows\t%s\n", s.c_str());
	}
	if (p_unbound) {
		for (const std::string &c : r.unbound_components) {
			const HudComponentType *t = table->find(c);
			std::printf("unbound\t%s\t%zu\t", c.c_str(), t ? t->sources.size() : 0);
			for (size_t i = 0; t && i < t->sources.size(); ++i) {
				std::printf("%s%s", i ? "," : "", t->sources[i].c_str());
			}
			std::printf("\n");
		}
	}
	std::fprintf(stderr, "hud-sources: %zu partitions, %.1f s\n", partitions.size(), seconds_since(start));
	return r.ok() ? 0 : 1;
}

}

const char *ui2_usage() {
	return "  minimap <LEVEL> [MODE] | minimap --census\n"
		   "  hud-icons\n"
		   "  item-icons [LEVEL]\n"
		   "  hud-sources [--unbound]\n";
}

int dispatch_ui2(const EbxContext &p_ctx, const std::string &p_cmd, const std::vector<std::string> &p_rest) {
	if (p_cmd == "minimap" && p_rest.size() == 1 && p_rest[0] == "--census") {
		return cmd_minimap_census(p_ctx);
	}
	if (p_cmd == "minimap" && (p_rest.size() == 1 || p_rest.size() == 2)) {
		return cmd_minimap(p_ctx, p_rest[0], p_rest.size() == 2 ? p_rest[1] : std::string("TeamDeathMatch0"));
	}
	if (p_cmd == "hud-icons" && p_rest.empty()) {
		return cmd_hud_icons(p_ctx);
	}
	if (p_cmd == "item-icons" && p_rest.size() <= 1) {
		return cmd_item_icons(p_ctx, p_rest.empty() ? std::string("MP_Subway") : p_rest[0]);
	}
	if (p_cmd == "hud-sources" && (p_rest.empty() || (p_rest.size() == 1 && p_rest[0] == "--unbound"))) {
		return cmd_hud_sources(p_ctx, !p_rest.empty());
	}
	return -1;
}

}
