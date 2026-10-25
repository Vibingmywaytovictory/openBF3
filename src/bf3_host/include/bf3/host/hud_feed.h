#pragma once

#include "bf3/core/gameplay/death_timeline.h"
#include "bf3/core/gameplay/eor_timeline.h"
#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/level/spawn_points.h"
#include "bf3/core/result.h"
#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/swf_layout.h"
#include "bf3/host/loadout.h"
#include "bf3/host/presentation_events.h"
#include "bf3/host/presentation_weapon.h"
#include "bf3/host/round_stats.h"
#include "bf3/net/protocol.h"
#include "bf3/net/weapons_wire.h"
#include "bf3/present/presented.h"
#include "bf3/sim/sim_types.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bf3::host {

inline constexpr double kHitIndicatorSeconds = 0.3;
inline constexpr float kHitIndicatorLevel = 1.0f;

inline constexpr float kCriticalAmmoFraction = 0.25f;

enum class HudCrosshairKind : uint8_t { None = 0, Rifle = 1, Pistol = 2, Shotgun = 3, Launcher = 4, Grenade = 5, Knife = 6 };

enum class HudFireMode : int8_t { None = -1, Single = 0, SingleBolt = 1, Auto = 2, Burst = 3, HoldAndRelease = 4, Detonated = 5 };
HudFireMode hud_fire_mode(core::gameplay::FireLogic p_logic);

inline constexpr std::array<std::string_view, 16> kHudWeaponClassNames{ "wc556x45mmNATO", "wc545x45mmWP", "wc762x51mmNATO", "wc762x54mmR",
	"wc762x39mmWP", "wc9x19mm", "wc45cal", "wc44mag", "wc12gauge", "wc338Magnum", "wc50cal", "wcHgr", "wcKnife", "wcUgl", "wcRocket",
	"wcPowerTool" };
int32_t hud_weapon_class(std::string_view p_weapon_class);

struct HudStaticRow {
	uint32_t name_sid_hash = 0;

	uint32_t item_id = 0;
	int32_t weapon_class = 0;
	int32_t clip_capacity = 0;
	uint32_t fire_modes = 0;
	HudCrosshairKind crosshair = HudCrosshairKind::None;
	bool show_ammo = true;

	std::array<HudFireMode, 4> mode_kinds{ HudFireMode::None, HudFireMode::None, HudFireMode::None, HudFireMode::None };
	int32_t magazines = 0;
	bool unlimited_ammo = false;
};
static_assert(std::is_trivially_copyable_v<HudStaticRow>);

struct HudStaticItem {
	uint32_t item_id = 0;
	uint32_t name_sid_hash = 0;
	core::ui::HudText image_path{};
	core::ui::HudText texture_path{};
};
static_assert(std::is_trivially_copyable_v<HudStaticItem>);

struct HudStatic {
	std::vector<HudStaticRow> rows;
	std::vector<HudStaticItem> items;
	const HudStaticRow *row(uint16_t p_def) const { return p_def < rows.size() ? &rows[p_def] : nullptr; }
	const HudStaticItem *item(uint32_t p_item_id) const;
};

core::Result<HudStatic> make_hud_static(const sim::weapons::WeaponTable &p_table, std::span<const core::gameplay::WeaponDef> p_defs);

size_t apply_item_names(HudStatic &r_static, const core::ui::ItemIconIndex &p_items);

size_t apply_item_table(HudStatic &r_static, const core::ui::ItemIconIndex &p_items);

core::ui::HudItem hud_item(const HudStatic &p_static, uint32_t p_item_id);

void hud_text(core::ui::HudText &r_out, std::string_view p_text);

struct KitTable;

struct HudKillCardSource {
	bool killer_seen = false;
	float distance_m = -1.0f;
};

struct HudFeedInput {
	PresentedWeapon weapon;
	const net::WeaponsWire *weapons = nullptr;
	bool alive = false;
	float health = 0;
	float view_yaw_rad = 0;
	sim::Pose pose = sim::Pose::Stand;
	double now_s = 0;
	uint8_t damage_own_slot = 0xFF;
	sim::Vec3 own_position{};
	std::span<const present::PresentedSoldier> others;

	bool tdm = false;
	bool match_valid = false;
	net::MatchState match;
	std::span<const net::PlayerRow> players;
	uint8_t own_slot = 0xFF;
	uint8_t own_team = 0;
	sim::Tick server_tick = 0;
	sim::Tick own_respawn_tick = 0;
	sim::Tick own_revive_until = 0;
	bool own_unspawned = false;
	bool spawned_this_round = false;
	uint8_t spawn_kit = 0;
	bool scoreboard_held = false;
	const KitTable *kits = nullptr;
	HudKillCardSource kill_card_source;
	std::optional<bool> kill_screen_up;
	int32_t score_kill = 0;
	int32_t score_headshot = 0;
	const core::gameplay::EorTimeline *eor = nullptr;
	const core::gameplay::DeathTimeline *death = nullptr;
	const LoadoutCatalog *loadouts = nullptr;
	std::array<LoadoutChoice, kKitCount> loadout_choices{};
	bool in_menu = false;
	std::string_view game_mode;
	uint32_t level_name_sid_hash = 0;
	std::span<const core::level::CharacterSpawnerDef> spawners;
};

struct HudFeedState {
	double hit_indicator_until_s = 0;
	uint64_t hits_shown = 0;
	uint64_t damage_shown = 0;
	uint64_t damage_unplaced = 0;

	uint64_t hits_ignored = 0;
	uint64_t frames = 0;
};

void fill_hud(const HudFeedInput &p_input, std::span<const PresentEvent> p_drained, const HudStatic &p_static, HudFeedState &r_state,
		core::ui::HudModel &r_model);

inline constexpr float kHudStageWidth = 1280.0f;
inline constexpr float kHudStageHeight = 720.0f;

float hud_scale(float p_width, float p_height);

struct HudPoint {
	float x = 0;
	float y = 0;
};

HudPoint hud_stage_to_window(core::ui::HudAnchor p_anchor, float p_stage_x, float p_stage_y, float p_width, float p_height);

float hud_compass_heading_deg(float p_view_yaw_rad);

float hud_bearing_deg(const sim::Vec3 &p_from, const sim::Vec3 &p_to);

int32_t hud_grenade_count(const net::WeaponsWire &p_weapons, const HudStatic &p_static);

}

namespace bf3::host {

inline constexpr std::string_view kDefaultBasicDogtag = "ui/art/persistence/dogtag/defaulttag_left";
inline constexpr std::string_view kDefaultAdvancedDogtag = "ui/art/persistence/dogtag/defaulttag_right";

inline constexpr std::array<uint8_t, core::ui::kHudSpawnKitItems> kHudSpawnSlots{ 0, 1, 2, 6, 7 };

inline constexpr std::array<std::string_view, 5> kHudMenuSids{ "ID_M_IGMMP_RESUME", "ID_M_IGMMP_SCOREBOARD",
	"ID_M_IGMMP_SUICIDE", "ID_M_IGMMP_OPTIONS", "ID_M_IGMMP_QUIT" };
inline constexpr std::string_view kHudTdmModeSid = "ID_M_GAMEMODE_TEAMDEATHMATCH";

struct HudTdmFeedState {
	uint32_t kill_serial = 0;
	uint32_t local_kill_serial = 0;
	uint32_t score_serial = 0;
	uint32_t kill_card_serial = 0;
	double kill_card_until_s = 0;
	double kill_card_from_s = 0;
	sim::Tick own_death_respawn_tick = 0;
	sim::Tick own_death_tick = 0;

	uint64_t deaths_fed = 0;
	uint64_t own_deaths = 0;
	uint64_t unknown_defs = 0;
	uint64_t bad_inputs = 0;
	uint64_t rows_dropped = 0;
	uint64_t frames = 0;

	RoundStats stats;
	net::MatchPhase stats_phase = net::MatchPhase::Loading;
};
static_assert(std::is_trivially_copyable_v<HudTdmFeedState>);

void fill_hud_tdm(const HudFeedInput &p_input, std::span<const PresentEvent> p_drained, const HudStatic &p_static, HudTdmFeedState &r_state,
		core::ui::HudModel &r_model);

}
