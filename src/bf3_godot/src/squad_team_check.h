#pragma once

#include "bf3/core/ui/hud_squads.h"
#include "bf3/net/protocol.h"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::godot_side {

class Bf3UiScreens;

inline constexpr double kSquadTeamCheckSeconds = 110.0;
inline constexpr double kSquadTeamStepSeconds = 15.0;
inline constexpr double kSquadTeamSettleSeconds = 1.0;
inline constexpr double kSquadTeamInputSeconds = 0.4;
inline constexpr double kSquadTeamOfferSeconds = 5.0;
inline constexpr size_t kSquadTeamMenuRow = 2;
inline constexpr std::string_view kSquadTeamPlayer2 = "player2";
inline constexpr std::string_view kSquadTeamJoinButton = "instance1.Button_01";
inline constexpr std::string_view kSquadTeamCreateButton = "instance1.Button_02";
inline constexpr std::string_view kSquadTeamKickButton = "instance1.Button_03";
inline constexpr std::string_view kSquadTeamSwitchButton = "instance1.Button_04";
inline constexpr std::string_view kSquadTeamPopupOk = "instance1.PopupButtonsManager_01.holder.btn0";
inline constexpr std::string_view kSquadTeamSquadRowClass = "Widget.List.Rows.SquadListRow";
inline constexpr std::string_view kSquadTeamMemberRowClass = "Widget.List.Rows.SquadDetailedRow";

struct SquadTeamCheckIo {
	std::function<void(godot::Key, bool)> press;
	std::function<void(godot::Vector2)> move;
	std::function<void(godot::Vector2, bool)> click;
	std::function<bool(std::string *)> join_extra;
	std::function<void(const bf3::net::SquadRequest &)> extra_request;
	std::function<void(const bf3::net::SquadRequest &)> own_request;
	std::function<bool(const std::string &)> shot;
	std::function<std::string(int32_t)> squad_data;
	std::span<const bf3::net::PlayerRow> players;
	uint8_t own_slot = 0xFF;
	const bf3::core::ui::HudSquadNames *names = nullptr;
};

class SquadTeamCheck {
public:
	explicit SquadTeamCheck(std::string p_shots_dir) : shots_dir_(std::move(p_shots_dir)) {}

	bool frame(Bf3UiScreens &r_screens, double p_now_s, const SquadTeamCheckIo &p_io);
	std::string report() const;
	int exit_code() const { return done_ && passed_ && failures_ == 0 ? 0 : 1; }

private:
	enum class Phase : uint8_t {
		Join,
		Teams,
		Menu,
		OpenSquads,
		Initial,
		Create,
		MateJoins,
		SelectOwnSquad,
		SelectMate,
		Kick,
		MateCreates,
		SelectSquad,
		JoinSquad,
		Leave,
		Switch,
		SwitchConfirm,
		Switched,
		Done,
	};

	struct ListRow {
		std::string path;
		std::string label;
		std::string members;
		bool local_tick = false;
	};

	const bf3::net::PlayerRow *own(const SquadTeamCheckIo &p_io) const;
	const bf3::net::PlayerRow *mate(const SquadTeamCheckIo &p_io) const;
	std::vector<ListRow> live_rows(Bf3UiScreens &r_screens, std::string_view p_class) const;
	std::optional<godot::Vector2> row_at(
			Bf3UiScreens &r_screens, std::string_view p_class, const std::string &p_label) const;
	std::string squad_name(uint8_t p_squad, const SquadTeamCheckIo &p_io) const;
	bool visible(Bf3UiScreens &r_screens, std::string_view p_partition, std::string_view p_path) const;
	bool click(Bf3UiScreens &r_screens, std::string_view p_partition, std::string_view p_path,
			const SquadTeamCheckIo &p_io);
	bool click_at(std::optional<godot::Vector2> p_at, const SquadTeamCheckIo &p_io);
	void agree(Bf3UiScreens &r_screens, const std::string &p_name, const SquadTeamCheckIo &p_io);
	void row(const std::string &p_name, bool p_pass, const std::string &p_detail);
	void enter(Phase p_phase, double p_now_s);
	void shot(const SquadTeamCheckIo &p_io, const std::string &p_name);
	std::string state_text(const SquadTeamCheckIo &p_io) const;

	std::string shots_dir_;
	Phase phase_ = Phase::Join;
	double start_s_ = -1.0;
	double phase_s_ = 0.0;
	int32_t sub_ = 0;
	bool agreed_ = false;
	godot::Vector2 click_at_;
	bool extra_joined_ = false;
	uint8_t own_team_ = 0;
	uint8_t created_ = 0;
	uint8_t initial_squad_ = 0;
	bool left_first_ = false;
	uint8_t mate_squad_ = 0;
	std::vector<std::string> lines_;
	uint32_t rows_ = 0;
	uint32_t failures_ = 0;
	bool done_ = false;
	bool passed_ = false;
};

}
