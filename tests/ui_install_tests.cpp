#include <doctest/doctest.h>

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_sources.h"
#include "bf3/core/ui/minimap.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/format.h"
#include "ui2_test_support.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;
namespace t2 = bf3::test::ui2;

namespace {

using bf3::core::fmt;
using bf3::core::seconds_since;

struct LevelMinimap {
	MinimapData data;
	MinimapProfile profile;
	uint32_t sat_w = 0;
	uint32_t sat_h = 0;
	std::string error;
};

LevelMinimap read_level_minimap(const std::shared_ptr<Install> &p_install, const Level &p_level) {
	LevelMinimap out;
	Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(p_install, { p_level.superbundle });
	if (!lib) {
		out.error = lib.error().message;
		return out;
	}
	const std::string key = to_lower_ascii(p_level.id);
	const EbxPartition *root = (*lib)->partition_by_name("levels/" + key + "/" + key);
	if (root == nullptr) {
		out.error = "no LevelData partition";
		return out;
	}
	level::WorkBudget budget;
	Result<MinimapData> d = read_minimap_data(**lib, root->primary(), p_level.id, budget);
	if (!d) {
		out.error = d.error().message;
		return out;
	}
	out.data = *d;
	Result<MinimapProfile> profile = read_minimap_profile(**lib, budget);
	if (!profile) {
		out.error = "profile: " + profile.error().message;
		return out;
	}
	out.profile = *profile;
	Result<L8Image> sat = load_l8_texture((*lib)->mount(0), d->satellite);
	if (!sat) {
		out.error = "satellite: " + sat.error().message;
		return out;
	}
	out.sat_w = sat->width;
	out.sat_h = sat->height;
	return out;
}

struct ModeMinimap {
	MinimapLevel level;
	MinimapImages images;
	std::vector<bool> inside;
	uint32_t side = 0;
};

ModeMinimap load_mode(const std::shared_ptr<Install> &p_install, std::string_view p_level, std::string_view p_mode) {
	ModeMinimap out;
	Result<MinimapLevel> level = load_minimap_level(p_install, p_level, p_mode);
	REQUIRE_MESSAGE(level.ok(), (level.ok() ? std::string() : level.error().message));
	out.level = std::move(*level);
	Result<LevelMount> mount = mount_minimap_level(*p_install, p_level);
	REQUIRE_MESSAGE(mount.ok(), (mount.ok() ? std::string() : mount.error().message));
	Result<MinimapImages> images = load_minimap_images(*mount, out.level);
	REQUIRE_MESSAGE(images.ok(), (images.ok() ? std::string() : images.error().message));
	out.images = std::move(*images);
	Result<std::vector<bool>> u = combat_area_union(out.images.combat_areas, out.level.data.combat_area_alpha_threshold);
	REQUIRE_MESSAGE(u.ok(), (u.ok() ? std::string() : u.error().message));
	out.inside = std::move(*u);
	for (const L8Image &m : out.images.combat_areas) {
		if (!m.empty()) {
			out.side = m.width;
			break;
		}
	}
	return out;
}

size_t objectives_inside(const ModeMinimap &p_m, uint8_t p_variant) {
	const MinimapMapping mapping = minimap_mapping(p_m.level.data);
	size_t n = 0;
	for (const MinimapObjective &o : p_m.level.objectives) {
		const MinimapUv uv = world_to_minimap_uv_variant(mapping, o.position[0], o.position[2], p_variant);
		if (!(uv.u >= 0.0f && uv.u < 1.0f && uv.v >= 0.0f && uv.v < 1.0f)) {
			continue;
		}
		const uint32_t x = std::min(p_m.side - 1, uint32_t(uv.u * float(p_m.side)));
		const uint32_t y = std::min(p_m.side - 1, uint32_t(uv.v * float(p_m.side)));
		n += p_m.inside[size_t(y) * p_m.side + x] ? 1 : 0;
	}
	return n;
}

std::shared_ptr<Install> install_or_skip() {
	t2::InstallFixture *f = t2::install();
	return f != nullptr ? f->install : nullptr;
}

}

TEST_SUITE("ui-minimap") {
	TEST_CASE("G1 minimap.reference: Metro, Caspian, Bazaar") {
		const std::shared_ptr<Install> install = install_or_skip();
		if (!install) {
			return;
		}
		struct Expect {
			const char *id;
			float cx, cz, size;
			uint32_t side;
			float wrap;
		};
		const Expect expects[] = { { "MP_Subway", 0.0f, -162.0f, 1024.0f, 2048, 70.0f }, { "MP_007", 0.0f, 0.0f, 2048.0f, 2048, 70.0f }, { "MP_001", 0.0f, 0.0f, 512.0f, 2048, 70.0f } };
		bool all = true;
		std::string detail;
		for (const Expect &e : expects) {
			const std::optional<Level> lvl = install->level(e.id);
			REQUIRE(lvl.has_value());
			const LevelMinimap m = read_level_minimap(install, *lvl);
			const MinimapProfile &pr = m.profile;
			const bool profile_ok = pr.texture_opacity == 0.5f && pr.zoom_small_step == 0 && pr.zoom_large_step == 2 && pr.zoom_nodes == 2 &&
					pr.zoom_steps == std::vector<float>{ 0.0f, 0.4f, 1.2f } && pr.zoom_small() == 0.0f && pr.init_small && pr.max_zoom_step == 2.0f &&
					minimap_zoom(pr, true).zoom == 0.0f && minimap_zoom(pr, true).player_is_center && minimap_zoom(pr, false).zoom == 1.2f &&
					!minimap_zoom(pr, false).player_is_center;
			const bool ok = m.error.empty() && m.data.world_center[0] == e.cx && m.data.world_center[1] == e.cz && m.data.world_size == e.size &&
					m.sat_w == e.side && m.sat_h == e.side && profile_ok && m.data.combat_area_multiply_wrap[0] == e.wrap && m.data.combat_area_multiply_wrap[1] == e.wrap;
			all = all && ok;
			detail += std::string(e.id) + " (" + fmt("%g, %g)/%g", m.data.world_center[0], m.data.world_center[1], m.data.world_size) + " satellite " +
					std::to_string(m.sat_w) + "x" + std::to_string(m.sat_h) + fmt(" opacity %g, MapZoom steps %zu x%zu, small step %d -> zoom %g, large step %d -> zoom %g, InitSmall %d, IsMaxZoom >= %g", pr.texture_opacity,
							pr.zoom_steps.size(), pr.zoom_nodes, pr.zoom_small_step, pr.zoom_small(), pr.zoom_large_step, minimap_zoom(pr, false).zoom, int(pr.init_small), pr.max_zoom_step) +
					fmt(", CombatAreaMultiplyWrapAmount %g x %g", m.data.combat_area_multiply_wrap[0], m.data.combat_area_multiply_wrap[1]) +
					(m.error.empty() ? "" : " ERROR " + m.error) + "; ";
		}
		t2::row("minimap.reference", all, detail);
		CHECK(all);
	}

	TEST_CASE("G2 minimap.all_levels: 29 / 29 MP levels") {
		const std::shared_ptr<Install> install = install_or_skip();
		if (!install) {
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		const std::vector<Level> levels = install->levels();
		size_t good = 0;
		std::string bad;
		for (const Level &l : levels) {
			const LevelMinimap m = read_level_minimap(install, l);
			const bool ok = m.error.empty() && m.data.partition == "levels/" + to_lower_ascii(l.id) + "/" + to_lower_ascii(l.id) && m.data.world_size > 0.0f &&
					m.data.world_rotation == 0.0f && m.data.combat_area_alpha_threshold >= 0.0f && m.data.combat_area_alpha_threshold <= 1.0f && m.sat_w > 0 &&
					m.sat_w <= 4096 && m.sat_h > 0 && m.sat_h <= 4096;
			good += ok ? 1 : 0;
			if (!ok) {
				bad += " " + l.id + (m.error.empty() ? "" : " (" + m.error + ")");
			}
		}
		const bool pass = levels.size() == 29 && good == 29;
		t2::row("minimap.all_levels", pass, std::to_string(good) + "/" + std::to_string(levels.size()) + " levels" + (bad.empty() ? "" : "; failed:" + bad) +
				fmt(" (%.1f s)", seconds_since(start)));
		CHECK(levels.size() == 29);
		CHECK(good == levels.size());
	}

	TEST_CASE("G3 minimap.masks: every combat-area asset and distance field over @all-mp") {
		const std::shared_ptr<Install> install = install_or_skip();
		if (!install) {
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(install, expand_superbundles(*install, "@all-mp"));
		REQUIRE(lib.ok());
		Result<MinimapCensus> c = minimap_census(**lib);
		REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message));
		const bool pass = c->combat_area_assets == 455 && c->combat_area_read == 455 && c->referenced_fields == 498 && c->referenced_decoded == 498 &&
				c->distance_field_assets == 562 && c->distance_field_decoded == 562 && c->failures.empty();
		std::string detail = std::to_string(c->combat_area_read) + "/" + std::to_string(c->combat_area_assets) + " VeniceUICombatAreaAsset read on " +
				std::to_string(c->levels_with_areas) + " levels; " + std::to_string(c->referenced_decoded) + "/" + std::to_string(c->referenced_fields) +
				" referenced distance fields decode L8; " + std::to_string(c->distance_field_decoded) + "/" + std::to_string(c->distance_field_assets) +
				" UIDistanceFieldAssets resolve to a decodable L8 texture; " + std::to_string(c->null_surroundings) + " null SurroundingDistanceField, " +
				std::to_string(c->null_distance_fields) + " null DistanceField; " + std::to_string(c->failures.size()) + " failures" +
				fmt(" (%.1f s)", seconds_since(start));
		for (const std::string &f : c->failures) {
			detail += "; " + f;
		}
		t2::row("minimap.masks", pass, detail);
		CHECK(pass);
	}

	TEST_CASE("G4 minimap.objectives: Conquest flags and HQs inside the mode's mask union") {
		const std::shared_ptr<Install> install = install_or_skip();
		if (!install) {
			return;
		}

		const ModeMinimap metro = load_mode(install, "MP_Subway", "ConquestSmall0");
		const ModeMinimap caspian = load_mode(install, "MP_007", "ConquestSmall0");
		const ModeMinimap caspian_large = load_mode(install, "MP_007", "ConquestLarge0");
		const auto hqs = [](const ModeMinimap &p_m) {
			size_t n = 0;
			for (const MinimapObjective &o : p_m.level.objectives) {
				n += o.kind == ObjectiveKind::Hq ? 1 : 0;
			}
			return n;
		};
		std::array<size_t, kMinimapMappingVariants> m{}, c{}, l{};
		bool others_below = true;
		std::string variants;
		for (uint8_t v = 0; v < kMinimapMappingVariants; ++v) {
			m[v] = objectives_inside(metro, v);
			c[v] = objectives_inside(caspian, v);
			l[v] = objectives_inside(caspian_large, v);
			others_below = others_below && (v == 0 || m[v] < metro.level.objectives.size());
			variants += " " + std::to_string(m[v]) + "/" + std::to_string(c[v]) + "/" + std::to_string(l[v]);
		}
		const bool pass = metro.level.objectives.size() == 5 && hqs(metro) == 2 && metro.level.combat_areas.size() == 2 && m[0] == 5 &&
				caspian.level.objectives.size() == 6 && hqs(caspian) == 2 && c[0] == 6 && caspian_large.level.objectives.size() == 7 &&
				hqs(caspian_large) == 2 && l[0] == 7 && others_below;
		const auto line = [](const char *p_name, const ModeMinimap &p_m, size_t p_in, size_t p_hq) {
			return std::string(p_name) + " " + std::to_string(p_in) + "/" + std::to_string(p_m.level.objectives.size()) + " (" + std::to_string(p_hq) +
					" HQs, " + std::to_string(p_m.level.combat_areas.size()) + " masks)";
		};
		t2::row("minimap.objectives", pass,
				line("Metro ConquestSmall0", metro, m[0], hqs(metro)) + ", " + line("Caspian ConquestSmall0", caspian, c[0], hqs(caspian)) + ", " +
						line("Caspian ConquestLarge0", caspian_large, l[0], hqs(caspian_large)) + "; Metro/CaspianCQS/CaspianCQL inside per variant 0..7:" +
						variants);
		CHECK(pass);
	}

	TEST_CASE("G5 minimap.orientation: Metro TDM mask against the gameplay area polygon") {
		const std::shared_ptr<Install> install = install_or_skip();
		if (!install) {
			return;
		}
		Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(install, "MP_Subway", "TeamDeathMatch0");
		REQUIRE(loaded.ok());
		Result<std::unique_ptr<level::World>> world = level::World::traverse(**loaded);
		REQUIRE(world.ok());
		Result<MinimapLevel> ml = read_minimap_level(**loaded, **world);
		REQUIRE_MESSAGE(ml.ok(), (ml.ok() ? std::string() : ml.error().message));
		Result<std::vector<std::array<float, 2>>> poly = read_area_polygon(**loaded, **world, "levels/mp_subway/teamdeathmatch/layer0_teamdeathmatch_logic");
		REQUIRE_MESSAGE(poly.ok(), (poly.ok() ? std::string() : poly.error().message));
		Result<LevelMount> mount = mount_minimap_level(*install, "MP_Subway");
		REQUIRE(mount.ok());
		Result<MinimapImages> images = load_minimap_images(*mount, *ml);
		REQUIRE(images.ok());
		REQUIRE(ml->combat_areas.size() == 1);
		Result<std::vector<bool>> mask = combat_area_union(images->combat_areas, ml->data.combat_area_alpha_threshold);
		REQUIRE(mask.ok());
		const uint32_t side = images->combat_areas[0].width;
		std::array<float, 4> box{ 1e9f, -1e9f, 1e9f, -1e9f };
		for (const std::array<float, 2> &p : *poly) {
			box[0] = std::min(box[0], p[0]);
			box[1] = std::max(box[1], p[0]);
			box[2] = std::min(box[2], p[1]);
			box[3] = std::max(box[3], p[1]);
		}
		const MinimapMapping mapping = minimap_mapping(ml->data);
		std::array<double, kMinimapMappingVariants> box_iou{}, poly_iou{};
		bool others = true;
		std::string per;
		for (uint8_t v = 0; v < kMinimapMappingVariants; ++v) {
			box_iou[v] = mask_box_iou(*mask, side, mapping, v, box);
			poly_iou[v] = mask_polygon_iou(*mask, side, mapping, v, *poly);
			if (v > 0) {
				others = others && box_iou[v] <= 0.05 && poly_iou[v] <= 0.05;
			}
			per += fmt(" %.3f/%.3f", box_iou[v], poly_iou[v]);
		}
		const bool pass = poly->size() == 23 && box_iou[0] >= 0.65 && poly_iou[0] >= 0.95 && others;
		t2::row("minimap.orientation", pass,
				"polygon " + std::to_string(poly->size()) + " points, bbox " + fmt("x %.1f..%.1f z %.1f..%.1f", box[0], box[1], box[2], box[3]) + ", mask " +
						ml->combat_areas[0].asset + " " + std::to_string(side) + "^2; box/polygon IoU per variant 0..7:" + per);
		CHECK(pass);
	}

	TEST_CASE("G13 install.unchanged") {
		t2::check_install_unchanged();
	}
}

TEST_SUITE("ui-hud-icons") {
	TEST_CASE("G6 icons.atlas: hud3dicons entries, states, rects, pixels") {
		const std::shared_ptr<Install> install = install_or_skip();
		if (!install) {
			return;
		}
		Result<std::unique_ptr<EbxLibrary>> ui = EbxLibrary::open(install, { "Win32/UI" });
		REQUIRE(ui.ok());
		Result<HudIconAtlas> atlas = read_hud_icon_atlas(**ui);
		REQUIRE_MESSAGE(atlas.ok(), (atlas.ok() ? std::string() : atlas.error().message));
		Result<Rgba8Image> img = load_hud_icon_texture((*ui)->mount(0), *atlas);
		REQUIRE_MESSAGE(img.ok(), (img.ok() ? std::string() : img.error().message));
		std::set<std::string> types;
		size_t frames = 0, bad = 0, flipped = 0;
		std::map<HudIconState, size_t> states;
		for (const HudIcon &icon : atlas->icons) {
			types.insert(icon.type);
			for (const HudIconStateFrames &s : icon.states) {
				++states[s.state];
				for (const HudIconUv &uv : s.frames) {
					++frames;
					const HudIconRect r = hud_icon_rect(uv, img->width, img->height);
					const bool ok = r.width > 0 && r.height > 0 && r.x >= 0 && r.y >= 0 && r.x + r.width <= int32_t(img->width) && r.y + r.height <= int32_t(img->height);
					bad += ok ? 0 : 1;
					flipped += (r.flip_x || r.flip_y) ? 1 : 0;
				}
			}
		}
		const HudIcon *local = atlas->find("UIHudIcon_LocalPlayer");
		const HudIconStateFrames *local_default = local ? local->state(HudIconState::Default) : nullptr;
		HudIconRect lr;
		if (local_default != nullptr && !local_default->frames.empty()) {
			lr = hud_icon_rect(local_default->frames[0], img->width, img->height);
		}

		bool drawn = true;
		for (const auto &[name, state] : { std::pair{ "UIHudIcon_LocalPlayer", HudIconState::Default }, std::pair{ "UIHudIcon_Player", HudIconState::Enemy },
					 std::pair{ "UIHudIcon_NeutralFlag", HudIconState::Neutral }, std::pair{ "UIHudIcon_FriendlyFlag", HudIconState::Friendly },
					 std::pair{ "UIHudIcon_EnemyFlag", HudIconState::Enemy }, std::pair{ "UIHudIcon_Base", HudIconState::Friendly },
					 std::pair{ "UIHudIcon_Base", HudIconState::Enemy } }) {
			const HudIcon *i = atlas->find(name);
			drawn = drawn && i != nullptr && i->state(state) != nullptr && !i->state(state)->frames.empty();
		}
		const bool pass = atlas->icons.size() == 161 && types.size() == 159 && atlas->with_states() == 93 && bad == 0 && flipped == 8 && atlas->unknown_states == 0 &&
				img->width == 256 && img->height == 1024 && img->rgba.size() == size_t(256) * 1024 * 4 && lr.x == 35 && lr.y == 633 && lr.width == 17 &&
				lr.height == 19 && drawn;
		std::string detail = std::to_string(atlas->icons.size()) + " entries, " + std::to_string(types.size()) + " distinct types, " +
				std::to_string(atlas->with_states()) + " with states, " + std::to_string(frames) + " frames (" + std::to_string(bad) + " bad rects, " + std::to_string(flipped) + " mirrored), states";
		for (const auto &[s, n] : states) {
			detail += " " + std::string(hud_icon_state_name(s)).substr(12) + " " + std::to_string(n);
		}
		detail += "; atlas " + atlas->texture + " " + std::to_string(img->width) + "x" + std::to_string(img->height) + " RGBA8; LocalPlayer Default (" +
				std::to_string(lr.x) + "," + std::to_string(lr.y) + "," + std::to_string(lr.width) + "," + std::to_string(lr.height) + "); drawn icons " +
				(drawn ? "all resolve" : "MISSING");
		t2::row("icons.atlas", pass, detail);
		CHECK(pass);
	}

	TEST_CASE("G7 icons.weapons: a _small icon for every MP_Subway catalog variant") {
		const std::shared_ptr<Install> install = install_or_skip();
		if (!install) {
			return;
		}
		Result<std::unique_ptr<EbxLibrary>> ui = EbxLibrary::open(install, { "Win32/UI" });
		REQUIRE(ui.ok());
		Result<std::unique_ptr<EbxLibrary>> lvl = EbxLibrary::open(install, { "Win32/Levels/MP_Subway/MP_Subway" });
		REQUIRE(lvl.ok());
		(*lvl)->load_all();
		const std::vector<std::string> variants = gameplay::list_weapon_variants(**lvl);
		Result<ItemIconIndex> index = ItemIconIndex::read(**ui);
		REQUIRE_MESSAGE(index.ok(), (index.ok() ? std::string() : index.error().message));

		bool sorted = std::is_sorted(index->icons().begin(), index->icons().end(), [](const ItemIcon &a, const ItemIcon &b) { return a.item_id < b.item_id; });
		size_t weapon = 0, kit = 0, name_matches = 0;
		std::map<std::string, size_t> formats;
		std::vector<std::string> misses;
		for (const std::string &v : variants) {
			const EbxPartition *p = (*lvl)->partition_by_name(v);
			const std::optional<EbxValue> id = p ? p->primary().get("Identifier") : std::nullopt;
			const uint32_t identifier = id ? id->as_u32().value_or(0) : 0;
			const ItemIcon *icon = index->find(identifier);
			if (icon == nullptr) {
				misses.push_back(v + " (id " + std::to_string(identifier) + ")");
				continue;
			}
			Result<ItemIconImage> img = load_item_icon((*ui)->mount(0), *icon);
			if (!img) {
				misses.push_back(v + " (" + img.error().message + ")");
				continue;
			}
			++formats[std::to_string(int(img->format)) + " " + std::to_string(img->width) + "x" + std::to_string(img->height)];
			if (icon->source == ItemIconSource::Weapon) {
				++weapon;
				const std::string sid = gameplay::weapon_name_sid(**ui, identifier);
				name_matches += sid == icon->name_sid ? 1 : 0;
			} else {
				++kit;
			}
		}
		const bool pass = variants.size() == 172 && misses.empty() && sorted && index->duplicate_ids() == 2;
		std::string detail = std::to_string(weapon + kit) + "/" + std::to_string(variants.size()) + " variants resolve by unlock identifier to a decoded _small icon (" +
				std::to_string(weapon) + " weapon metadata, " + std::to_string(kit) + " kit-item metadata; weapon name SID equal on " + std::to_string(name_matches) +
				"); index " + std::to_string(index->icons().size()) + " ids, " + std::to_string(index->duplicate_ids()) + " duplicate; formats (code wxh):";
		for (const auto &[f, n] : formats) {
			detail += " " + f + " x" + std::to_string(n);
		}
		detail += "; " + std::to_string(misses.size()) + " misses";
		for (const std::string &m : misses) {
			detail += "; MISS " + m;
		}
		t2::row("icons.weapons", pass, detail);
		CHECK(pass);
	}

	TEST_CASE("G13 install.unchanged") {
		t2::check_install_unchanged();
	}
}

TEST_SUITE("ui-hud-sources") {
	TEST_CASE("G8 hud.sources: the DataSources table and the binding rows") {
		const std::shared_ptr<Install> install = install_or_skip();
		if (!install) {
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		Result<std::unique_ptr<EbxLibrary>> lib = EbxLibrary::open(install, expand_superbundles(*install, kHudSourceSuperbundles));
		REQUIRE(lib.ok());
		std::vector<const LevelMount *> mounts;
		for (size_t i = 0; i < (*lib)->superbundle_count(); ++i) {
			mounts.push_back(&(*lib)->mount(i));
		}
		const std::vector<std::string> partitions = hud_component_partitions(mounts);
		Result<HudSourceTable> table = read_hud_source_table(**lib, partitions);
		REQUIRE_MESSAGE(table.ok(), (table.ok() ? std::string() : table.error().message));
		const auto count = [&](std::string_view p_type) -> size_t {
			const HudComponentType *t = table->find(p_type);
			return t ? t->sources.size() : size_t(-1);
		};
		size_t empty_in_state = 0;
		for (uint32_t i : table->state_components) {
			empty_in_state += table->types[i].sources.empty() ? 1 : 0;
		}
		const bool table_ok = partitions.size() == 84 && table->types.size() == 84 && table->state_components.size() == 58 && count("UIWeaponCompData") == 63 &&
				count("UISoldierCompData") == 16 && count("UIVehicleCompData") == 82 && empty_in_state == 7 && table->version == kHudSourceTableVersion;
		t2::row("hud.sources", table_ok,
				std::to_string(partitions.size()) + " partitions, " + std::to_string(table->types.size()) + " types, " + std::to_string(table->source_count()) +
						" sources, " + std::to_string(table->state_components.size()) + " state components (" + std::to_string(empty_in_state) +
						" with no DataSources); UIWeaponCompData " + std::to_string(count("UIWeaponCompData")) + ", UISoldierCompData " +
						std::to_string(count("UISoldierCompData")) + ", UIVehicleCompData " + std::to_string(count("UIVehicleCompData")) +
						fmt(" (%.1f s)", seconds_since(start)));
		CHECK(table_ok);
		const HudBindingReport r = check_hud_bindings(*table);
		const bool rows_ok = r.ok() && r.rows == 515 && r.matched == 515 && r.kinds[0] == 275 && r.kinds[1] == 88 &&
				r.kinds[2] == 152 && r.unbound_components.size() == 37;
		std::string detail = std::to_string(r.rows) + " rows, " + std::to_string(r.matched) + " in the install, " +
				std::to_string(r.not_in_install.size()) + " missing from the install, " +
				std::to_string(r.not_in_rows.size()) +
				" install sources of the 21 bound types without a row; Model/Client/Unused " +
				std::to_string(r.kinds[0]) + "/" + std::to_string(r.kinds[1]) + "/" + std::to_string(r.kinds[2]) +
				"; " + std::to_string(r.unbound_components.size()) + " unbound in-game components with " +
				std::to_string(r.unbound_sources) + " sources";
		for (const std::string &s : r.not_in_install) {
			detail += "; not in install: " + s;
		}
		for (const std::string &s : r.not_in_rows) {
			detail += "; no row: " + s;
		}
		t2::row("hud.bindings", rows_ok, detail);
		CHECK(rows_ok);
	}

	TEST_CASE("G13 install.unchanged") {
		t2::check_install_unchanged();
	}
}
