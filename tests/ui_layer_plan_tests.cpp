#include <doctest/doctest.h>

#include "bf3/core/level/level.h"
#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/hud_messages.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/key_bindings.h"
#include "bf3/core/ui/options.h"
#include "bf3/core/ui/options_model.h"
#include "bf3/core/ui/swf_layer_plan.h"
#include "bf3/core/ui/text_db.h"
#include "bf3/core/ui/ui_drawn_screens.h"
#include "bf3/core/ui/ui_graph.h"
#include "bf3/core/ui/ui_stage.h"
#include "bf3/host/hud_feed.h"
#include "bf3/host/ui_flow_session.h"
#include "test_support.h"
#include "ui_retail_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;

namespace {

constexpr double kStepSeconds = 1.0 / 30.0;
constexpr int kSteps = 90;
constexpr int kStepsPerKill = 15;
constexpr int32_t kMenuHintDataKey = 606381875;
constexpr double kClipEdgeTolerance = 1.0 / 64.0;

struct WindowSize {
	int32_t width = 0;
	int32_t height = 0;
};
constexpr std::array<WindowSize, 3> kWindows{ WindowSize{ 1280, 720 }, WindowSize{ 2560, 1440 },
	WindowSize{ 3840, 2160 } };

constexpr std::array<std::pair<std::string_view, uint32_t>, 25> kCensusFilterFloors{ {
		{ "customizeaccessoriesscreen", 5 },
		{ "customizesoldierscreen", 36 },
		{ "customizesoldiersubscreen", 10 },
		{ "eordetailsscoreboardscreen", 9 },
		{ "eordetailsstatsscreen", 32 },
		{ "eordetailstabsscreen", 4 },
		{ "eorwinningteamscreen", 1 },
		{ "hudmpscreen", 9 },
		{ "hudscreen", 1 },
		{ "hudtdmscreen", 4 },
		{ "ingamemenump", 12 },
		{ "killscreen", 10 },
		{ "optionsaudioscreen", 14 },
		{ "optionscontrolsscreen", 17 },
		{ "optionsgameplayscreen", 18 },
		{ "optionskeybindingscreen", 54 },
		{ "optionsvideoscreen", 34 },
		{ "scoreboardbackscreen", 3 },
		{ "scoreboardtwoteamshud32screen", 202 },
		{ "scoreboardtwoteamshud64screen", 200 },
		{ "scoreboardtwoteamsscreen", 166 },
		{ "screenadjustscreen", 3 },
		{ "spawnbuttonscreen", 1 },
		{ "spawnscreenpc", 15 },
		{ "spawnscreenticketcountertdmscreen", 4 },
} };

constexpr std::array<std::string_view, 4> kOwnSurfaceAllowList{ ".scoringMessage", ".scoringMessage.txt",
	".scoringMessage.txtBg", ".multiplayerCompass.cps.#" };
constexpr std::array<std::string_view, 7> kOwnWhenShownAllowList{ ".multiplayerCompass.objective#.obj.#",
	".multiplayerCompass.objective#.obj", ".spinnerMc.#", ".bg", ".container.#", ".tf", ".icon.label" };

std::string digits_to_hash(std::string_view p_path) {
	std::string out;
	for (const char c : p_path) {
		const bool digit = c >= '0' && c <= '9';
		if (!digit || out.empty() || out.back() != '#') {
			out.push_back(digit ? '#' : c);
		}
	}
	return out;
}

template <size_t N>
const std::string_view *allow_list_entry(const std::array<std::string_view, N> &p_list, std::string_view p_path) {
	const std::string path = digits_to_hash(p_path);
	const auto it = std::find_if(p_list.begin(), p_list.end(), [&path](std::string_view p_tail) {
		const std::string tail = digits_to_hash(p_tail);
		return path.size() >= tail.size() && path.compare(path.size() - tail.size(), tail.size(), tail) == 0;
	});
	return it != p_list.end() ? &*it : nullptr;
}

bool allowed_own_surface(std::string_view p_path) {
	return allow_list_entry(kOwnSurfaceAllowList, p_path) != nullptr;
}

struct Rig {
	ui_retail::Retail *retail = nullptr;
	std::unique_ptr<level::LoadedLevel> level;
	std::unique_ptr<bf3::host::UiFlowSession> session;
	std::unique_ptr<OptionsCatalog> catalog;
	std::unique_ptr<OptionsSession> options;
	OptionsModel options_model;
	std::unique_ptr<ItemIconIndex> items;
	std::unique_ptr<HudMessageCompData> messages;
	HudModel model;
};

void add_player(HudModel &r_model, HudPlayerList &r_list, uint8_t p_slot, const std::string &p_name) {
	HudPlayer &p = r_model.players[p_slot];
	std::memcpy(p.name.data(), p_name.data(), std::min(p_name.size(), p.name.size() - 1));
	p.bot = true;
	p.team = uint8_t(&r_list == &r_model.team.friendly_team_players ? 1 : 2);
	p.kills = 10 + p_slot;
	p.deaths = 3 + p_slot;
	p.score = 100 * p_slot + 572;
	p.ping_ms = 31;
	r_model.player_count = uint8_t(std::max<int32_t>(r_model.player_count, p_slot + 1));
	r_list.index[r_list.count++] = p_slot;
}

void set_item(HudItem &r_item, const ItemIcon &p_icon, uint32_t p_name) {
	r_item.item_id = p_icon.item_id;
	r_item.name_sid_hash = p_name;
	bf3::host::hud_text(r_item.image_path, p_icon.path);
}

HudModel play_model(const ItemIconIndex &p_items) {
	const std::vector<ItemIcon> &icons = p_items.icons();
	const auto icon = [&icons](size_t p_i) -> const ItemIcon & { return icons[p_i % icons.size()]; };
	HudModel m{};
	m.local_player = 0;
	m.local_team = 1;
	for (uint8_t i = 0; i < 32; ++i) {
		add_player(m, i < 16 ? m.team.friendly_team_players : m.team.enemy_team1_players, i,
				"BOT_Sgt. Foster " + std::to_string(i));
	}
	m.soldier.health = 100.0f;
	m.weapon.active_weapon_ammo = 30;
	m.weapon.active_weapon_ammo_in_bag = 120;
	m.kit.selected_kit_name = 0x6000u;
	HudMenuList &menu = m.ingame_menu.menu_entries;
	for (size_t i = 0; i < kHudMenuActionSids.size(); ++i) {
		HudMenuEntry &e = menu.entries[menu.count++];
		e.label_sid_hash = text_hash(kHudMenuActionSids[i]);
		e.action = HudMenuAction(i);
		e.enabled = true;
	}
	bf3::host::hud_text(m.settings.current_game_mode, "TeamDeathMatch0");
	m.settings.current_game_mode_name = text_hash(bf3::host::kHudTdmModeSid);
	m.settings.is_pc = true;
	m.level.current_level = text_hash("ID_M_LEVELNAME_MP_SUBWAY");
	m.team.friendly_team_name = text_hash("ID_M_US");
	m.end_of_round.round_duration = 754.0f;
	m.end_of_round.team_results = { 100, 87 };
	HudCustomization &c = m.customization;
	for (size_t l = 0; l < kHudSpawnKitItems; ++l) {
		c.kit_grid_data[l].count = 3;
		for (size_t i = 0; i < 3; ++i) {
			set_item(c.kit_grid_data[l].items[i], icon(l * 3 + i), 0x7000u + uint32_t(l) * 0x10u + uint32_t(i));
		}
		c.kit_grid_selected[l] = 1;
		c.kit_grid_category[l] = 0x6100u + uint32_t(l);
	}
	for (size_t a = 0; a < kHudAccessorySlots; ++a) {
		c.accessory_grid[a].count = 2;
		for (size_t i = 0; i < 2; ++i) {
			set_item(c.accessory_grid[a].items[i], icon(40 + a * 2 + i),
					0x7000u + uint32_t(5 + a) * 0x10u + uint32_t(i));
		}
	}
	for (size_t k = 0; k < kHudKitCount; ++k) {
		c.kits_data[k].name_sid_hash = k == 0 ? 0x6000u : 0x6001u;
		for (size_t i = 0; i < kHudSpawnKitItems; ++i) {
			set_item(c.kits_data[k].items[i], icon(60 + k * kHudSpawnKitItems + i), 0x7000u + uint32_t(i) * 0x10u + 1u);
		}
	}
	return m;
}

void push_kill(HudModel &r_m, uint8_t p_killer, uint8_t p_victim, bool p_score) {
	for (HudKillFeed *f : { &r_m.messages.kill_message, &r_m.messages.local_kill_message }) {
		++f->newest_serial;
		HudKill &k = f->lines[f->newest_serial % f->lines.size()];
		k = HudKill{};
		k.serial = f->newest_serial;
		k.killer = p_killer;
		k.victim = p_victim;
		k.weapon_name_sid_hash = text_hash("ID_P_WNAME_M16A4");
	}
	if (!p_score) {
		return;
	}
	HudScoreFeed &s = r_m.messages.score_message;
	++s.newest_serial;
	HudScoreEvent &e = s.events[s.newest_serial % s.events.size()];
	e.serial = s.newest_serial;
	e.sid_hash = text_hash(kHudScoreHeadshotSid);
	e.points = 100;
}

Rig *rig_or_skip() {
	static Rig r;
	static bool tried = false;
	if (!tried) {
		tried = true;
		r.retail = ui_retail::retail_or_skip();
		if (r.retail == nullptr) {
			return nullptr;
		}
		Result<std::unique_ptr<level::LoadedLevel>> l =
				level::load_level(r.retail->mount->install, "MP_Subway", "TeamDeathMatch0");
		REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
		r.level = std::move(*l);
		bf3::host::UiFlowSessionOptions o;
		o.initialize_screens = false;
		Result<std::unique_ptr<bf3::host::UiFlowSession>> s = bf3::host::UiFlowSession::create(*r.level, o);
		REQUIRE_MESSAGE(s.ok(), (s.ok() ? std::string() : s.error().message));
		r.session = std::move(*s);
		bf3::host::ui_flow_tdm_pc_defaults(*r.session, r.level->mode);
		Result<OptionsCatalog> c = read_options_catalog(*r.retail->level);
		REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message));
		r.catalog = std::make_unique<OptionsCatalog>(std::move(*c));
		Result<KeyBindingMaps> maps = read_key_binding_maps(*r.retail->level);
		Result<std::vector<std::string>> tabs = read_key_binding_tabs(*r.retail->level);
		REQUIRE((maps.ok() && tabs.ok()));
		Result<KeyBindingTable> t = KeyBindingTable::build(*maps, *tabs);
		REQUIRE(t.ok());
		r.options = std::make_unique<OptionsSession>(*r.catalog, *t);
		r.options_model = r.options->model();
		Result<ItemIconIndex> items = ItemIconIndex::read(*r.retail->ui);
		REQUIRE_MESSAGE(items.ok(), (items.ok() ? std::string() : items.error().message));
		r.items = std::make_unique<ItemIconIndex>(std::move(*items));
		Result<HudMessageCompData> data = read_hud_message_comp(*r.retail->level);
		REQUIRE_MESSAGE(data.ok(), (data.ok() ? std::string() : data.error().message));
		r.messages = std::make_unique<HudMessageCompData>(std::move(*data));
		r.model = play_model(*r.items);
	} else if (r.retail == nullptr) {
		bf3::test::skip_install_case("no install");
		return nullptr;
	}
	return &r;
}

HudClientState rig_client(Rig &r_r) {
	HudClientState c = ui_retail::retail_text_client(ui_retail::retail_text_db(*r_r.retail));
	c.flow_data = [&r_r](std::string_view p_category, int32_t p_key) -> std::optional<std::string> {
		if (p_key == kMenuHintDataKey) {
			return std::string("ID_M_MINIMAPTYPE_HINT");
		}
		return r_r.session->data(p_category, p_key);
	};
	c.options = &r_r.options_model;
	c.items = r_r.items.get();
	c.localise_hash = [base = c.localise_hash](uint32_t p_hash) {
		if (p_hash == 0x6000u) {
			return std::string("ASSAULT");
		}
		if (p_hash == 0x6001u) {
			return std::string("ENGINEER");
		}
		if (p_hash >= 0x6100u && p_hash < 0x6110u) {
			return "CATEGORY " + std::to_string(p_hash - 0x6100u);
		}
		if (p_hash >= 0x7000u && p_hash < 0x8000u) {
			return "ITEM " + std::to_string((p_hash - 0x7000u) / 0x10u) + "." + std::to_string(p_hash % 0x10u);
		}
		return base ? base(p_hash) : std::string();
	};
	return c;
}

struct DrivenScreen {
	std::string partition;
	std::string leaf;
	SwfLayerWalk first;
	SwfLayerWalk settled;
};

DrivenScreen drive_screen(Rig &r_r, std::string_view p_partition) {
	HudMessageComp messages(*r_r.messages);
	HudClientState client = rig_client(r_r);
	client.messages = &messages;
	ui_retail::RetailScreen s = ui_retail::open_retail_screen(*r_r.retail, p_partition, client);
	DrivenScreen out;
	out.partition = std::string(p_partition);
	out.leaf = ui_data_category_leaf(p_partition);
	const bool scores = p_partition != kKillScreenPartition;
	HudModel m = r_r.model;
	double now = 0.0;
	messages.update(m, client, now);
	s.hud->initialize(m);
	s.hud->set_focus(true);
	(void)s.scene->take_changes();
	out.first = swf_layer_walk(*s.hud);
	for (int i = 0; i < kSteps; ++i) {
		now += kStepSeconds;
		if (i % kStepsPerKill == 0) {
			push_kill(m, uint8_t(i % 16), uint8_t(16 + i % 16), scores || i == 0);
		}
		messages.update(m, client, now);
		s.hud->refresh(m);
		s.hud->advance(kStepSeconds);
		REQUIRE(s.scene->advance_seconds(kStepSeconds).ok());
	}
	(void)s.scene->take_changes();
	out.settled = swf_layer_walk(*s.hud);
	return out;
}

class StillLoadingImages final : public SwfImageSource {
public:
	SwfImageInfo request(std::string_view) override { return SwfImageInfo{}; }
};

SwfLayerPlan plan_while_icons_load(Rig &r_r, std::string_view p_partition) {
	HudMessageComp messages(*r_r.messages);
	HudClientState client = rig_client(r_r);
	client.messages = &messages;
	ui_retail::RetailScreen s = ui_retail::open_retail_screen(*r_r.retail, p_partition, client);
	s.scene->set_image_source(std::make_shared<StillLoadingImages>());
	const HudModel &m = r_r.model;
	messages.update(m, client, 0.0);
	s.hud->initialize(m);
	s.hud->set_focus(true);
	for (int i = 0; i < 4; ++i) {
		s.hud->refresh(m);
		s.hud->advance(kStepSeconds);
		REQUIRE(s.scene->advance_seconds(kStepSeconds).ok());
	}
	(void)s.scene->take_changes();
	return swf_layer_plan(swf_layer_walk(*s.hud), kWindows[0].width, kWindows[0].height);
}

const std::map<std::string, DrivenScreen> &driven_screens(Rig &r_r) {
	static std::map<std::string, DrivenScreen> screens;
	if (!screens.empty()) {
		return screens;
	}
	std::set<std::string_view> partitions;
	const auto add = [&partitions](const auto &p_list) { partitions.insert(p_list.begin(), p_list.end()); };
	add(kHudDrawnPartitions);
	add(kMenuDrawnPartitions);
	add(kOptionsScreens);
	add(kDeployDrawnPartitions);
	add(kCustomizeScreenPartitions);
	add(kNativeEorScreens);
	add(kOverlayDrawnPartitions);
	partitions.insert(kKillScreenPartition);
	partitions.insert(kPopupBindKeyPartition);
	partitions.insert(kPopupDelayPartition);
	partitions.insert(kScreenAdjustPartition);
	for (const std::string_view p : partitions) {
		DrivenScreen d = drive_screen(r_r, p);
		std::string leaf = d.leaf;
		screens.emplace(std::move(leaf), std::move(d));
	}
	return screens;
}

bool is_hud_family(std::string_view p_leaf) {
	return p_leaf == "hudmpscreen" || p_leaf == "hudtdmscreen" || p_leaf == "hudscreen" ||
			p_leaf == "crosshairdefault" || p_leaf == "killscreen";
}

uint32_t clips_off_whole_pixels(const SwfLayerWalk &p_walk, const WindowSize p_window) {
	const UiStage st = ui_stage_show_all(p_walk.stage, float(p_window.width), float(p_window.height));
	const auto off = [](double p_v) { return std::abs(p_v - std::round(p_v)) > kClipEdgeTolerance; };
	uint32_t out = 0;
	for (const SwfLayerWalkNode &l : p_walk.layers) {
		if (l.route == SwfLayerClass::Clip && l.has_mask) {
			out += off(double(l.mask.x_min) * st.scale + st.offset_x) ||
							off(double(l.mask.x_max) * st.scale + st.offset_x) ||
							off(double(l.mask.y_min) * st.scale + st.offset_y) ||
							off(double(l.mask.y_max) * st.scale + st.offset_y)
					? 1u
					: 0u;
		}
	}
	return out;
}

struct RouteRecorder final : SwfLayerVisitor {
	const SwfScene &scene;
	const SwfFontSet *fonts = nullptr;
	std::vector<SwfLayerClass> opened;

	RouteRecorder(const SwfScene &p_scene, const SwfFontSet *p_fonts) : scene(p_scene), fonts(p_fonts) {}

	void seen(const SwfSceneNode &) override {}
	uint32_t pieces(const SwfSceneNode &p_node) override { return swf_node_pieces(scene, fonts, p_node).count; }
	bool engine_target(const SwfSceneNode &) override { return false; }
	void target(const SwfSceneNode &, uint32_t) override {}
	void layer_opened(uint32_t, const SwfLayerWalkNode &p_layer) override { opened.push_back(p_layer.route); }
	void attach(const SwfSceneNode &, uint32_t, bool, bool, bool) override {}
};

struct NamedSet {
	std::string name;
	std::vector<std::string_view> partitions;
};

std::vector<NamedSet> simultaneous_sets() {
	const std::vector<std::string_view> hud(kHudDrawnPartitions.begin(), kHudDrawnPartitions.end());
	const auto with = [&hud](std::initializer_list<std::string_view> p_more) {
		std::vector<std::string_view> v = hud;
		v.insert(v.end(), p_more);
		return v;
	};
	std::vector<NamedSet> sets;
	sets.push_back({ "play", hud });
	sets.push_back({ "scoreboard", with({ kScoreboardHud32ScreenPartition }) });
	sets.push_back({ "escape", with({ kIngameMenuPartition, kScoreboardHud32ScreenPartition }) });
	sets.push_back({ "escape_scoreboard",
			with({ kIngameMenuPartition, kMenuScoreboardPartition, kMenuScoreboardBackPartition }) });
	for (size_t i = 0; i < kOptionsScreens.size(); ++i) {
		const std::string tab(ui_data_category_leaf(kOptionsScreens[i]));
		sets.push_back({ "options_" + tab, with({ kIngameMenuPartition, kOptionsScreens[i] }) });
		const std::string_view next = kOptionsScreens[(i + 1) % kOptionsScreens.size()];
		sets.push_back({ "options_tab_" + tab, with({ kOptionsScreens[i], next }) });
	}
	const std::vector<std::string_view> deploy{ kSpawnScreenPcPartition, kSpawnButtonScreenPartition,
		kSpawnTicketCounterPartition, kKillScreenPartition };
	std::vector<std::string_view> deploy_set = hud;
	deploy_set.insert(deploy_set.end(), deploy.begin(), deploy.end());
	sets.push_back({ "deploy", deploy_set });
	std::vector<std::string_view> customize_set = deploy_set;
	customize_set.insert(customize_set.end(), kCustomizeScreenPartitions.begin(), kCustomizeScreenPartitions.end());
	sets.push_back({ "customize", customize_set });
	sets.push_back({ "end_of_round",
			with({ kEorWinningTeamScreenPartition, kEorHud64Partition, kEorTabsPartition, kEorScoreboardPartition,
					kEorTwoTeamsPartition }) });
	sets.push_back({ "end_of_round_stats",
			with({ kEorHud64Partition, kEorTabsPartition, kEorStatsPartition, kEorScoreboardPartition }) });
	return sets;
}

SwfNodeId drawn_descendant(const SwfScene &p_scene, const SwfFontSet *p_fonts, SwfNodeId p_root) {
	std::vector<SwfNodeId> stack{ p_root };
	while (!stack.empty()) {
		const SwfSceneNode *n = p_scene.node(stack.back());
		stack.pop_back();
		if (n == nullptr) {
			continue;
		}
		if (swf_node_pieces(p_scene, p_fonts, *n).count != 0) {
			return n->id;
		}
		stack.insert(stack.end(), n->children.begin(), n->children.end());
	}
	return kSwfNoNode;
}
}

TEST_SUITE("ui-layer-plan") {
	TEST_CASE("layer plan: a mask's route is known when its layer opens") {
		Rig *r = rig_or_skip();
		if (r == nullptr) {
			return;
		}
		HudMessageComp messages(*r->messages);
		HudClientState client = rig_client(*r);
		client.messages = &messages;
		ui_retail::RetailScreen s = ui_retail::open_retail_screen(*r->retail, kMenuScoreboardPartition, client);
		s.hud->initialize(r->model);
		for (int i = 0; i < 3; ++i) {
			s.hud->refresh(r->model);
			s.hud->advance(kStepSeconds);
			REQUIRE(s.scene->advance_seconds(kStepSeconds).ok());
		}
		(void)s.scene->take_changes();
		RouteRecorder recorder(*s.scene, s.hud->fonts());
		SwfPlanWalkOptions options;
		options.fonts = s.hud->fonts();
		const SwfLayerWalk walk = swf_walk_layers(*s.scene, options, recorder);
		REQUIRE(recorder.opened.size() == walk.layers.size());
		uint32_t clips = 0;
		for (size_t i = 0; i < walk.layers.size(); ++i) {
			if (walk.layers[i].kind == SwfLayerKind::Mask) {
				clips += walk.layers[i].route == SwfLayerClass::Clip ? 1u : 0u;
				CHECK_MESSAGE(recorder.opened[i] == walk.layers[i].route, walk.layers[i].path, " opened as ",
						swf_layer_class_name(recorder.opened[i]), " and ended as ",
						swf_layer_class_name(walk.layers[i].route));
			}
		}
		CHECK(clips > 0);
	}

	TEST_CASE("layer plan: every drawn screen has its census filter floor, and own surfaces are the allow-listed few") {
		Rig *r = rig_or_skip();
		if (r == nullptr) {
			return;
		}
		const std::map<std::string, DrivenScreen> &screens = driven_screens(*r);
		std::set<std::string_view> own_used;
		std::set<std::string_view> when_shown_used;
		std::printf("screen pages clips own empty non_empty_filters multi_filter non_rect_masks plain_text "
					"plain_glyphs kernels "
					"slot_px own_kib first_own\n");
		for (const auto &[leaf, d] : screens) {
			const SwfLayerPlan plan = swf_layer_plan(d.settled, kWindows[0].width, kWindows[0].height);
			const SwfLayerPlan first = swf_layer_plan(d.first, kWindows[0].width, kWindows[0].height);
			const SwfLayerCounts &c = plan.counts;
			std::printf("%s %u %u %u %u %u %u %u %u %u %u %llu %.1f %u\n", leaf.c_str(), c.pages, c.clips,
					c.own_surfaces, c.empty, c.non_empty_filters, d.settled.multi_filter_layers,
					d.settled.non_rect_masks, d.settled.plain_text_fields, d.settled.plain_text_glyphs,
					c.distinct_kernels, static_cast<unsigned long long>(c.page_slot_px),
					double(c.own_surface_bytes) / 1024.0, first.counts.own_surfaces);
			if (c.clips != 0) {
				std::printf("  clips off whole pixels %s: %u at 720p, %u at 1080p, %u at 1440p, of %u\n", leaf.c_str(),
						clips_off_whole_pixels(d.settled, kWindows[0]),
						clips_off_whole_pixels(d.settled, WindowSize{ 1920, 1080 }),
						clips_off_whole_pixels(d.settled, kWindows[1]),
						uint32_t(std::count_if(
								d.settled.layers.begin(), d.settled.layers.end(), [](const SwfLayerWalkNode &p_l) {
									return p_l.route == SwfLayerClass::Clip && p_l.has_mask;
								})));
			}
			for (const SwfPlannedLayer &l : plan.layers) {
				if (l.klass() == SwfLayerClass::OwnSurface) {
					std::printf("  own %s %s %dx%d\n", swf_layer_kind_name(l.kind), l.path.c_str(), l.width, l.height);
					const std::string_view *entry = allow_list_entry(kOwnSurfaceAllowList, l.path);
					CHECK_MESSAGE(entry != nullptr, leaf, " has an own surface off the allow-list: ", l.path);
					if (entry != nullptr) {
						own_used.insert(*entry);
					}
				}
			}
			for (size_t i = 0; i < plan.layers.size(); ++i) {
				const SwfLayerWalkNode &l = d.settled.layers[i];
				if (l.route == SwfLayerClass::OwnSurface && plan.layers[i].klass() == SwfLayerClass::Empty) {
					std::printf("  own when shown %s %s children %zu tilted %d targets %d\n",
							swf_layer_kind_name(l.kind), l.path.c_str(), l.children.size(), l.tilted_inside ? 1 : 0,
							l.has_targets ? 1 : 0);
					const std::string_view *entry = allow_list_entry(kOwnSurfaceAllowList, l.path);
					const std::string_view *later = allow_list_entry(kOwnWhenShownAllowList, l.path);
					CHECK_MESSAGE((entry != nullptr || later != nullptr), leaf,
							" can take an own surface off the allow-lists: ", l.path);
					if (later != nullptr) {
						when_shown_used.insert(*later);
					}
				}
			}
			if (!is_hud_family(leaf)) {
				CHECK_MESSAGE(c.own_surfaces == 0, leaf, " own surfaces ", c.own_surfaces);
				CHECK_MESSAGE(
						first.counts.own_surfaces == 0, leaf, " first walk own surfaces ", first.counts.own_surfaces);
				CHECK_MESSAGE(d.settled.non_rect_masks == 0, leaf, " non-rect masks ", d.settled.non_rect_masks);
			}
		}
		for (const std::string_view entry : kOwnSurfaceAllowList) {
			CHECK_MESSAGE(own_used.count(entry) != 0, "nothing draws through the allow-list entry ", entry);
		}
		for (const std::string_view entry : kOwnWhenShownAllowList) {
			CHECK_MESSAGE(when_shown_used.count(entry) != 0, "nothing needs the allow-list entry ", entry);
		}
		for (const auto &[leaf, floor] : kCensusFilterFloors) {
			const auto it = screens.find(std::string(leaf));
			REQUIRE_MESSAGE(it != screens.end(), "no driven screen for ", leaf);
			const uint32_t seen =
					swf_layer_plan(it->second.settled, kWindows[0].width, kWindows[0].height).counts.non_empty_filters;
			CHECK_MESSAGE(seen >= floor, leaf, " has ", seen, " non-empty filter layers, the census saw ", floor);
		}
	}

	TEST_CASE("layer plan: while icons load, the image spinners take page slots and add no own surface") {
		Rig *r = rig_or_skip();
		if (r == nullptr) {
			return;
		}
		std::vector<std::string_view> partitions(kCustomizeScreenPartitions.begin(), kCustomizeScreenPartitions.end());
		partitions.push_back(kSpawnScreenPcPartition);
		partitions.push_back(kEorStatsPartition);
		uint32_t spinners_seen = 0;
		for (const std::string_view p : partitions) {
			const SwfLayerPlan plan = plan_while_icons_load(*r, p);
			uint32_t spinners = 0;
			for (const SwfPlannedLayer &l : plan.layers) {
				const bool spinner = l.kind == SwfLayerKind::Blend && l.path.find(".spinnerMc.") != std::string::npos;
				if (spinner && l.has_content) {
					++spinners;
					CHECK_MESSAGE(
							l.klass() == SwfLayerClass::Page, p, " ", l.path, " is ", swf_layer_class_name(l.klass()));
				}
				if (l.klass() == SwfLayerClass::OwnSurface) {
					CHECK_MESSAGE(allowed_own_surface(l.path), p, " ", l.path);
				}
			}
			CHECK_MESSAGE(
					plan.counts.own_surfaces == 0, p, " own surfaces while icons load ", plan.counts.own_surfaces);
			spinners_seen += spinners;
			std::printf("while icons load: %s own %u, spinners %u, pages %u\n",
					std::string(ui_data_category_leaf(p)).c_str(), plan.counts.own_surfaces, spinners,
					plan.counts.pages);
		}
		CHECK(spinners_seen > 0);
	}

	TEST_CASE("layer plan: listMask and GridScrollMaskMc are rectangles, on every screen that has them") {
		Rig *r = rig_or_skip();
		if (r == nullptr) {
			return;
		}
		uint32_t list_masks = 0;
		uint32_t grid_masks = 0;
		for (const auto &[leaf, d] : driven_screens(*r)) {
			for (const SwfLayerWalkNode &l : d.settled.layers) {
				if (l.kind != SwfLayerKind::Mask) {
					continue;
				}
				const bool list = l.path.find("listMask") != std::string::npos;
				const bool grid = l.path.find("GridScrollMaskMc") != std::string::npos;
				list_masks += list ? 1u : 0u;
				grid_masks += grid ? 1u : 0u;
				if (list || grid) {
					CHECK_MESSAGE(l.route == SwfLayerClass::Clip, leaf, " ", l.path, " is not a rectangle");
				}
			}
		}
		std::printf("rect masks: listMask %u GridScrollMaskMc %u\n", list_masks, grid_masks);
		CHECK(list_masks > 0);
		CHECK(grid_masks > 0);
	}

	TEST_CASE("layer plan: a rectangle stays one with alpha 0 and stops being one when rotated or tilted") {
		Rig *r = rig_or_skip();
		if (r == nullptr) {
			return;
		}
		HudMessageComp messages(*r->messages);
		HudClientState client = rig_client(*r);
		client.messages = &messages;
		ui_retail::RetailScreen s = ui_retail::open_retail_screen(*r->retail, kMenuScoreboardPartition, client);
		s.hud->initialize(r->model);
		for (int i = 0; i < 3; ++i) {
			s.hud->refresh(r->model);
			s.hud->advance(kStepSeconds);
			REQUIRE(s.scene->advance_seconds(kStepSeconds).ok());
		}
		(void)s.scene->take_changes();
		const SwfLayerWalk walk = swf_layer_walk(*s.hud);
		std::string mask;
		for (const SwfLayerWalkNode &l : walk.layers) {
			if (l.kind == SwfLayerKind::Mask && l.path.find("listMask") != std::string::npos) {
				mask = l.path;
			}
		}
		REQUIRE(!mask.empty());
		const SwfNodeId id = s.scene->find(mask);
		const SwfFontSet *fonts = s.hud->fonts();
		REQUIRE(swf_rect_mask(*s.scene, fonts, id).has_value());
		const std::vector<SwfNodeId> tilted{ id };
		CHECK_FALSE(swf_rect_mask(*s.scene, fonts, id, tilted).has_value());
		REQUIRE(s.scene->set_alpha(mask, 0.0f).ok());
		(void)s.scene->take_changes();
		CHECK(swf_rect_mask(*s.scene, fonts, id).has_value());
		const SwfNodeId drawn = drawn_descendant(*s.scene, fonts, id);
		REQUIRE(drawn != id);
		const std::vector<SwfNodeId> tilted_below{ drawn };
		CHECK_FALSE(swf_rect_mask(*s.scene, fonts, id, tilted_below).has_value());
		SwfMatrix turned = s.scene->node(id)->matrix;
		turned.b = 0.5f;
		turned.c = -0.5f;
		REQUIRE(s.scene->set_matrix(mask, turned).ok());
		(void)s.scene->take_changes();
		CHECK_FALSE(swf_rect_mask(*s.scene, fonts, id).has_value());
	}

	TEST_CASE("layer plan: a drawn triangle is no rectangle, a drawn rectangle is") {
		Rig *r = rig_or_skip();
		if (r == nullptr) {
			return;
		}
		HudMessageComp messages(*r->messages);
		HudClientState client = rig_client(*r);
		client.messages = &messages;
		ui_retail::RetailScreen s = ui_retail::open_retail_screen(*r->retail, kMenuScoreboardPartition, client);
		const auto drawn_id = [&](std::string_view p_name, int32_t p_depth, std::vector<SwfDrawOp> p_ops) {
			const std::string path = "instance1." + std::string(p_name);
			const Result<SwfNodeId> id = s.scene->create_empty_movie_clip("instance1", p_name, p_depth);
			REQUIRE(id.ok());
			for (const SwfDrawOp &op : p_ops) {
				REQUIRE(s.scene->draw(path, op).ok());
			}
			(void)s.scene->take_changes();
			return *id;
		};
		const SwfColor red{ 255, 0, 0, 255 };
		const SwfNodeId triangle = drawn_id("planTriangle", 16001,
				{ SwfDrawOp::begin_fill(red), SwfDrawOp::move_to(0.0f, 0.0f), SwfDrawOp::line_to(40.0f, 0.0f),
						SwfDrawOp::line_to(0.0f, 30.0f), SwfDrawOp::line_to(0.0f, 0.0f), SwfDrawOp::end_fill() });
		const SwfNodeId rectangle = drawn_id("planRectangle", 16002,
				{ SwfDrawOp::begin_fill(red), SwfDrawOp::move_to(0.0f, 0.0f), SwfDrawOp::line_to(40.0f, 0.0f),
						SwfDrawOp::line_to(40.0f, 30.0f), SwfDrawOp::line_to(0.0f, 30.0f),
						SwfDrawOp::line_to(0.0f, 0.0f), SwfDrawOp::end_fill() });
		CHECK_FALSE(swf_rect_mask(*s.scene, s.hud->fonts(), triangle).has_value());
		CHECK(swf_rect_mask(*s.scene, s.hud->fonts(), rectangle).has_value());
	}

	TEST_CASE("layer plan: the compass masks are not rectangles, so a real non-rectangle is rejected") {
		Rig *r = rig_or_skip();
		if (r == nullptr) {
			return;
		}
		const std::map<std::string, DrivenScreen> &screens = driven_screens(*r);
		const auto it = screens.find("hudtdmscreen");
		REQUIRE(it != screens.end());
		uint32_t compass_masks = 0;
		for (const SwfLayerWalkNode &l : it->second.settled.layers) {
			if (l.kind == SwfLayerKind::Mask && l.path.find("Compass") != std::string::npos) {
				++compass_masks;
				CHECK_MESSAGE(
						l.route == SwfLayerClass::OwnSurface, l.path, " is classed ", swf_layer_class_name(l.route));
			}
		}
		CHECK(compass_masks > 0);
		CHECK(it->second.settled.non_rect_masks == compass_masks);
	}

	TEST_CASE("layer plan: MenuHint's text is a page, not a group around one") {
		Rig *r = rig_or_skip();
		if (r == nullptr) {
			return;
		}
		const std::map<std::string, DrivenScreen> &screens = driven_screens(*r);
		const auto it = screens.find("optionsvideoscreen");
		REQUIRE(it != screens.end());
		bool page = false;
		for (const SwfLayerWalkNode &l : it->second.settled.layers) {
			if (l.path.find("MenuHint_01.hint.text") == std::string::npos) {
				continue;
			}
			CHECK_MESSAGE(l.kind != SwfLayerKind::Blend, l.path);
			page = page || (l.kind == SwfLayerKind::Filter && l.route == SwfLayerClass::Page);
		}
		CHECK(page);
	}

	TEST_CASE("layer plan: every simultaneous set fits its pages with the margin, at 720p, 1440p and 2160p") {
		Rig *r = rig_or_skip();
		if (r == nullptr) {
			return;
		}
		const std::map<std::string, DrivenScreen> &screens = driven_screens(*r);
		for (const WindowSize w : kWindows) {
			const SwfPageSizes pages = swf_page_sizes(w.width, w.height);
			std::printf("pages at %dx%d: R %dx%d (%.1f MiB) S %dx%d (%.1f MiB)\n", w.width, w.height,
					pages.result_width, pages.result_height, double(pages.result_bytes()) / (1024.0 * 1024.0),
					pages.scratch_width, pages.scratch_height, double(pages.scratch_bytes()) / (1024.0 * 1024.0));
			uint64_t largest = 0;
			std::string largest_name;
			uint32_t passes_peak = 0;
			for (const NamedSet &set : simultaneous_sets()) {
				SwfShelfPacker packer(pages.result_width, pages.result_height);
				std::vector<SwfDirtyLayer> dirty;
				uint64_t px = 0;
				for (const std::string_view p : set.partitions) {
					const auto it = screens.find(std::string(ui_data_category_leaf(p)));
					REQUIRE_MESSAGE(it != screens.end(), "no driven screen for ", p);
					for (const SwfPlannedLayer &l : swf_layer_plan(it->second.settled, w.width, w.height).layers) {
						if (l.klass() != SwfLayerClass::Page) {
							continue;
						}
						CHECK_MESSAGE(packer.alloc(l.width, l.height).has_value(), set.name, " ", l.path);
						dirty.push_back(SwfDirtyLayer{ l.width, l.height });
						px += uint64_t(swf_page_slot_side(l.width)) * uint64_t(swf_page_slot_side(l.height));
					}
				}
				CHECK_MESSAGE(packer.failures() == 0, set.name, " failures ", packer.failures());
				CHECK_MESSAGE(double(px) * kSwfPageMargin <= double(pages.result_px()), set.name, " needs ", px, " of ",
						pages.result_px());
				if (px > largest) {
					largest = px;
					largest_name = set.name;
				}
				uint32_t passes = 0;
				while (!dirty.empty()) {
					const SwfScratchPass pass =
							swf_select_scratch_pass(dirty, pages.scratch_width, pages.scratch_height);
					CHECK_MESSAGE(pass.too_large.empty(), set.name, " holds a layer larger than S");
					REQUIRE_MESSAGE(!pass.taken.empty(), set.name, " S pass took nothing");
					std::vector<SwfDirtyLayer> rest;
					for (const size_t i : pass.waiting) {
						rest.push_back(dirty[i]);
					}
					dirty = std::move(rest);
					++passes;
				}
				passes_peak = std::max(passes_peak, passes);
			}
			std::printf("largest set at %dx%d: %s %llu px of %llu, peak S passes %u\n", w.width, w.height,
					largest_name.c_str(), static_cast<unsigned long long>(largest),
					static_cast<unsigned long long>(pages.result_px()), passes_peak);
			if (w.width == kSwfPageBaseWidth) {
				CHECK_MESSAGE(largest <= kSwfPageDemandPxAtBase, largest_name, " needs ", largest,
						" px, the page size plans for ", kSwfPageDemandPxAtBase);
			}
		}
	}
}

TEST_SUITE("ui-layer-plan-unit") {
	TEST_CASE("page sizes: 720p takes R 2048x1024, 1440p 4096x2048, and S is half of R per axis") {
		const SwfPageSizes p720 = swf_page_sizes(1280, 720);
		CHECK(p720.result_width == 2048);
		CHECK(p720.result_height == 1024);
		CHECK(p720.scratch_width == 1024);
		CHECK(p720.scratch_height == 512);
		const SwfPageSizes p1440 = swf_page_sizes(2560, 1440);
		CHECK(p1440.result_width == 4096);
		CHECK(p1440.result_height == 2048);
		CHECK(p1440.scratch_width == 2048);
		CHECK(p1440.scratch_height == 1024);
		for (const WindowSize w : kWindows) {
			const SwfPageSizes p = swf_page_sizes(w.width, w.height);
			const double area_scale =
					double(w.width) * double(w.height) / (double(kSwfPageBaseWidth) * double(kSwfPageBaseHeight));
			CHECK(double(p.result_px()) >= double(kSwfPageDemandPxAtBase) * area_scale * kSwfPageMargin);
		}
	}

	TEST_CASE("target bytes: four bytes a pixel, five times that for a 4x target") {
		CHECK(swf_target_bytes(2048, 1024, kSwfTargetFactor1x) == 8388608u);
		CHECK(swf_target_bytes(1024, 512, kSwfTargetFactor4x) == 10485760u);
		CHECK(swf_target_bytes(-1, 512, kSwfTargetFactor4x) == 0u);
	}

	TEST_CASE("slot sides round up to 8 px after 1 px of padding") {
		CHECK(swf_page_slot_side(0) == 8);
		CHECK(swf_page_slot_side(7) == 8);
		CHECK(swf_page_slot_side(8) == 16);
		CHECK(swf_page_slot_side(884) == 888);
	}

	TEST_CASE("shelf packer: live slots never overlap and stay inside the page, through random alloc and free") {
		SwfShelfPacker packer(512, 256);
		std::mt19937 rng(20261031u);
		std::vector<SwfPageSlot> live;
		const auto overlap = [](const SwfPageSlot &p_a, const SwfPageSlot &p_b) {
			return p_a.x < p_b.x + p_b.width && p_b.x < p_a.x + p_a.width && p_a.y < p_b.y + p_b.height &&
					p_b.y < p_a.y + p_a.height;
		};
		for (int i = 0; i < 5000; ++i) {
			if (!live.empty() && (rng() % 3 == 0 || live.size() > 40)) {
				const size_t at = rng() % live.size();
				packer.release(live[at]);
				live.erase(live.begin() + std::ptrdiff_t(at));
				continue;
			}
			const std::optional<SwfPageSlot> slot = packer.alloc(int32_t(rng() % 120) + 1, int32_t(rng() % 40) + 1);
			if (!slot) {
				continue;
			}
			REQUIRE(slot->x >= 0);
			REQUIRE(slot->y >= 0);
			REQUIRE(slot->x + slot->width <= 512);
			REQUIRE(slot->y + slot->height <= 256);
			for (const SwfPageSlot &o : live) {
				REQUIRE_FALSE(overlap(*slot, o));
			}
			live.push_back(*slot);
		}
		CHECK(packer.live_slots() == live.size());
	}

	TEST_CASE("shelf packer: freeing everything gives the whole page back") {
		SwfShelfPacker packer(256, 128);
		std::vector<SwfPageSlot> live;
		while (const std::optional<SwfPageSlot> slot = packer.alloc(31, 15)) {
			live.push_back(*slot);
		}
		CHECK(live.size() == 8u * 8u);
		CHECK(packer.failures() == 1u);
		std::shuffle(live.begin(), live.end(), std::mt19937(7u));
		for (const SwfPageSlot &s : live) {
			packer.release(s);
		}
		CHECK(packer.used_px() == 0u);
		CHECK(packer.live_slots() == 0u);
		const std::optional<SwfPageSlot> whole = packer.alloc(255, 127);
		REQUIRE(whole.has_value());
		CHECK(whole->x == 0);
		CHECK(whole->y == 0);
	}

	TEST_CASE("shelf packer: a freed slot is reused, and a slot too large for the page counts as a failure") {
		SwfShelfPacker packer(64, 64);
		const std::optional<SwfPageSlot> a = packer.alloc(31, 15);
		const std::optional<SwfPageSlot> b = packer.alloc(31, 15);
		REQUIRE((a.has_value() && b.has_value()));
		packer.release(*a);
		const std::optional<SwfPageSlot> c = packer.alloc(31, 15);
		REQUIRE(c.has_value());
		CHECK(c->x == a->x);
		CHECK(c->y == a->y);
		CHECK(packer.failures() == 0u);
		CHECK_FALSE(packer.alloc(64, 8).has_value());
		CHECK(packer.failures() == 1u);
	}

	TEST_CASE("scratch pass: every dirty layer is drawn in a bounded number of passes, oversized ones are named") {
		std::mt19937 rng(11u);
		std::vector<SwfDirtyLayer> dirty;
		for (int i = 0; i < 2000; ++i) {
			dirty.push_back(SwfDirtyLayer{ int32_t(rng() % 300) + 1, int32_t(rng() % 60) + 1 });
		}
		dirty.push_back(SwfDirtyLayer{ 2000, 16 });
		size_t oversized = 0;
		uint32_t passes = 0;
		while (!dirty.empty()) {
			const SwfScratchPass pass = swf_select_scratch_pass(dirty, 1024, 512);
			oversized += pass.too_large.size();
			std::vector<SwfDirtyLayer> rest;
			for (const size_t i : pass.waiting) {
				rest.push_back(dirty[i]);
			}
			const bool progress = !pass.taken.empty() || !pass.too_large.empty();
			REQUIRE(progress);
			CHECK(pass.taken.size() == pass.slots.size());
			CHECK(pass.taken.size() + pass.waiting.size() + pass.too_large.size() == dirty.size());
			dirty = std::move(rest);
			REQUIRE(++passes < 200u);
		}
		CHECK(oversized == 1u);
		CHECK(passes > 1u);
	}

	TEST_CASE("page churn: opening and closing screens at the planned margin never fails an allocation") {
		const SwfPageSizes pages = swf_page_sizes(1280, 720);
		SwfShelfPacker packer(pages.result_width, pages.result_height);
		std::mt19937 rng(3u);
		const auto screen = [&rng](int32_t p_rows) {
			std::vector<SwfDirtyLayer> layers;
			for (int32_t i = 0; i < p_rows; ++i) {
				layers.push_back(SwfDirtyLayer{ int32_t(rng() % 240) + 16, int32_t(rng() % 28) + 14 });
			}
			return layers;
		};
		const std::array<std::vector<SwfDirtyLayer>, 8> screens{ screen(200), screen(160), screen(54), screen(40),
			screen(30), screen(180), screen(30), screen(100) };
		std::array<std::vector<SwfPageSlot>, 8> open;
		const auto demand = [&screens](size_t p_i) {
			uint64_t px = 0;
			for (const SwfDirtyLayer &l : screens[p_i]) {
				px += uint64_t(swf_page_slot_side(l.width)) * uint64_t(swf_page_slot_side(l.height));
			}
			return px;
		};
		uint64_t live_px = 0;
		for (int i = 0; i < 2000; ++i) {
			const size_t s = rng() % screens.size();
			if (!open[s].empty()) {
				for (const SwfPageSlot &slot : open[s]) {
					packer.release(slot);
				}
				open[s].clear();
				live_px -= demand(s);
				continue;
			}
			if (double(live_px + demand(s)) * kSwfPageMargin > double(pages.result_px())) {
				continue;
			}
			for (const SwfDirtyLayer &l : screens[s]) {
				const std::optional<SwfPageSlot> slot = packer.alloc(l.width, l.height);
				REQUIRE(slot.has_value());
				open[s].push_back(*slot);
			}
			live_px += demand(s);
		}
		CHECK(packer.failures() == 0u);
	}
}
