#pragma once

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::godot_side {

class Bf3UiScreens;

inline constexpr double kDeployCheckSeconds = 200.0;
inline constexpr double kDeployCheckStepSeconds = 40.0;
inline constexpr double kDeployCheckSettleSeconds = 1.0;
inline constexpr double kDeployCheckInputSeconds = 0.3;
inline constexpr double kDeployCheckRefuseSeconds = 1.5;
inline constexpr double kDeployCheckSpawnEndSeconds = 2.5;
inline constexpr std::array<double, 6> kDeployCheckSpawnShots{ 0.0, 0.1, 0.25, 0.5, 1.0, 2.0 };
inline constexpr uint8_t kDeployCheckClickKit = 2;
inline constexpr uint8_t kDeployCheckNoKit = 0xFF;
inline constexpr uint8_t kDeployCheckKeyKit = 3;
inline constexpr std::string_view kDeployCheckKitBar = "instance1.KitBar_01.kitButton_";
inline constexpr std::string_view kDeployCheckDeployButton = "instance1.SpawnTimer_01.btnDeploy";
inline constexpr std::string_view kDeployCheckPrimarySlot = "instance1.KitBar_01.slotButton_0";
inline constexpr std::string_view kDeployCheckGridCell = "instance1.Grid_01.gridItems.cell-";
inline constexpr double kDeployCheckGridSeconds = 0.8;
inline constexpr double kDeployCheckKillCardShotSeconds = 2.5;
inline constexpr double kDeployCheckCleanSeconds = 0.5;
inline constexpr std::string_view kDeployCheckOverviewMap = "instance1.OverviewMap_01";
inline constexpr std::string_view kDeployCheckLightBars = "instance1.SpawnTimer_01.lightbars";
inline constexpr std::string_view kDeployCheckDeployStatus = "instance1.SpawnTimer_01.deployStatus";
inline constexpr std::string_view kDeployCheckTeamColourFrame = "blue";

struct DeployCheckTab {
	std::string_view name;
	std::string_view button;
};

inline constexpr std::array<DeployCheckTab, 2> kDeployCheckTabs{ {
		{ "customize", "instance1.TabBar_01.tabBarHolder.button_2" },
		{ "squad", "instance1.TabBar_01.tabBarHolder.button_1" },
} };
inline constexpr double kDeployCheckTabSeconds = 1.5;
inline constexpr uint32_t kCustomizeFirstStep = 10;

struct DeployCheckList {
	uint32_t count = 0;
	int32_t selected = -1;
	std::array<uint32_t, 32> items{};
	uint32_t chosen = 0;
};

inline constexpr size_t kDeployCheckGadget2List = 0;
inline constexpr size_t kDeployCheckOpticList = 1;
inline constexpr std::string_view kDeployCheckSelectorRight = ".mcContainer.Slot_0.buttonRight";

struct DeployCheckState {
	bool alive = false;
	uint32_t own_spawns = 0;
	uint32_t own_deaths = 0;
	uint8_t spawn_kit = 0;
	uint8_t own_kit = 0;
	float time_to_respawn = 0.0f;
	uint64_t spawn_actions = 0;
	uint64_t kits_from_flow = 0;
	uint64_t glitches = 0;
	uint64_t dim_outs = 0;
	uint32_t glitch_triggers = 0;
	float glitch_power = 0.0f;
	uint64_t picks_from_flow = 0;
	uint32_t loadout_requests = 0;
	uint32_t primary_count = 0;
	int32_t primary_selected = -1;
	std::array<uint32_t, 32> primary_items{};
	uint32_t chosen_primary = 0;
	uint32_t spawned_primary = 0;
	uint64_t concepts_found = 0;
	std::array<DeployCheckList, 2> lists{};
};

struct DeployCheckIo {
	std::function<void(godot::Key, bool)> press;
	std::function<void(godot::Vector2)> move;
	std::function<void(godot::Vector2, bool)> click;
	std::function<void()> suicide;
	std::function<void(bool)> hold_scoreboard;
	std::function<void(uint8_t)> kit_key;
	std::function<bool(const std::string &)> shot;
	std::function<DeployCheckState()> state;
};

class DeployCheck {
public:
	explicit DeployCheck(std::string p_shots_dir) : shots_dir_(std::move(p_shots_dir)) {}

	bool frame(Bf3UiScreens &r_screens, double p_now_s, const DeployCheckIo &p_io);
	std::string report() const;
	int exit_code() const { return passed_ && failures_ == 0 ? 0 : 1; }

private:
	enum class Phase : uint8_t {
		Join,
		JoinDeploy,
		SpawnEnd,
		Suicide,
		Counter,
		Counting,
		KitKey,
		KitClick,
		GridOpen,
		GridPick,
		TabProbe,
		Ready,
		TabHeld,
		ClickDeploy,
		EnterBlocked,
		EnterDeploy,
		Done,
	};
	void step(Bf3UiScreens &r_screens, double p_now_s, const DeployCheckIo &p_io, const DeployCheckState &p_st);
	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);
	void enter(Phase p_phase, double p_now_s, const DeployCheckState &p_st);
	void spawned(const std::string &p_row, std::string p_how, uint8_t p_kit, Phase p_next, double p_now_s);
	bool click_clip(
			Bf3UiScreens &r_screens, std::string_view p_partition, std::string_view p_path, const DeployCheckIo &p_io);
	void tap(const DeployCheckIo &p_io, godot::Key p_key);
	void shot(const DeployCheckIo &p_io, const std::string &p_name, double p_now_s);
	static bool up(const Bf3UiScreens &p_screens, std::string_view p_partition);
	static bool shown(const Bf3UiScreens &p_screens, std::string_view p_partition);
	static std::string node_fact(Bf3UiScreens &r_screens, std::string_view p_partition, std::string_view p_path);
	static bool spawn_row_drawn(Bf3UiScreens &r_screens, std::string &r_fact);

	std::string shots_dir_;
	Phase phase_ = Phase::Join;
	Phase after_ = Phase::Done;
	double start_s_ = -1.0;
	double phase_s_ = 0.0;
	double ready_s_ = -1.0;
	int32_t sub_ = 0;
	size_t spawn_shot_ = 0;
	size_t log_seen_ = 0;
	uint32_t spawn_ends_ = 0;
	DeployCheckState mark_;
	std::vector<std::string> lines_;
	uint32_t rows_ = 0;
	uint32_t failures_ = 0;
	bool done_ = false;
	bool passed_ = false;
	bool alive_while_counting_ = false;
	godot::Vector2 click_at_;
	std::string spawn_row_;
	std::string spawn_how_;
	uint8_t spawn_kit_ = kDeployCheckNoKit;
	uint32_t spawn_primary_ = 0;
	uint32_t picked_ = 0;
	int32_t pick_index_ = -1;
	size_t tab_ = 0;
	uint32_t probe_step_ = 0;
	double probe_s_ = 0.0;
	std::string tab_opened_;
	DeployCheckState cust_mark_;
	uint32_t cust_expected_ = 0;
	void customize(Bf3UiScreens &r_screens, double p_now_s, const DeployCheckIo &p_io, const DeployCheckState &p_st);
};

}
