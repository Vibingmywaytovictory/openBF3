#include "list.h"

namespace bf3::core::ui {

namespace {

constexpr uint32_t kRowTextGlow = 0x73C3FFu;
constexpr uint32_t kWhite = 0xFFFFFFu;
constexpr std::string_view kReadySid = "ID_M_SCOREBOARD_READY";

std::string dash_if_undefined(const As2Value &p_v, std::string_view p_dash) {
	const std::string s = p_v.to_string();
	return s == "undefined" ? std::string(p_dash) : s;
}

class ScoreBoardRowClass final : public RowBaseClass {
public:
	void construct(HudWidgetHost &r_host) override {
		RowBaseClass::construct(r_host);
		register_state_callback(r_host.path());
		if (hud_platform_win32(r_host.client())) {
			return;
		}
		const float ping = r_host.clip("PingLabel").width();
		for (const std::string_view name : { "KillsLabel", "DeathsLabel", "ScoreLabel" }) {
			const As2Clip c = r_host.clip(name);
			c.set_x(c.x() + ping);
		}
		r_host.clip("PingLabel").set_visible(false);
	}

	void set_data(const As2Value &p_data) override {
		if (as2_equals_true(p_data.field("useDirectAccess"))) {
			set_text("Label", "-");
			set_text("NumLabel", As2Value::of(row_index() + 1.0).to_string());
			set_text("KillsLabel", "-");
			set_text("DeathsLabel", "-");
			set_text("ScoreLabel", "-");
			host_->clip("voipIcon").goto_and_stop(0);
			set_text("PingLabel", "");
			set_local_squad(As2Value::of(0));
			set_ready_state(As2Value::of(false));
			setup_direct_access();
		} else {
			set_text("Label", p_data.field("Label").to_string());
			set_text("NumLabel", As2Value::of(row_index()).to_string());
			set_text("KillsLabel", dash_if_undefined(p_data.field("Kills"), "-"));
			set_text("DeathsLabel", dash_if_undefined(p_data.field("Deaths"), "-"));
			set_text("ScoreLabel", dash_if_undefined(p_data.field("Score"), "-"));
			const As2Value &voip = p_data.field("Voip");
			if (voip.kind == As2Value::Kind::String) {
				host_->clip("voipIcon").goto_and_stop(std::string_view(voip.text));
			} else {
				goto_and_stop_number(host_->clip("voipIcon"), voip.to_number());
			}
			set_text("PingLabel", dash_if_undefined(p_data.field("Ping"), ""));
			set_ready_state(p_data.field("MatchReady"));
			set_local_squad(As2Value::of(p_data.field("LocalSquad").to_number()));
		}
		set_mouse_enabled(true);
		RowBaseClass::set_data(p_data);
	}

	As2Value label() const override { return As2Value::of(host_->clip("Label").text()); }

	bool local_player() const { return local_player_.truthy(); }

protected:
	void on_state(const std::string &p_state) override {
		if (local_player_.truthy()) {
			host_->clip("bg").goto_and_stop(std::string_view(p_state + "Player"));
		} else if (local_squad_.truthy()) {
			host_->clip("bg").goto_and_stop(std::string_view(p_state + "Squad"));
		}
		const bool inverted = p_state == "selected" || p_state == "overSelected" || p_state == "press" ||
				p_state == "pressSelected" || local_player_.truthy();
		for (const std::string_view name :
				{ "NumLabel", "Label", "KillsLabel", "DeathsLabel", "ScoreLabel", "PingLabel", "ReadyLabel" }) {
			set_text_color(host_->clip(name), inverted ? 0u : kWhite);
		}
		set_ready_state(match_ready_);
	}

	void member(std::string_view p_name, const As2Value &p_value) override {
		if (p_name == "isLocalSquad") {
			set_local_squad(p_value);
		} else if (p_name == "isLocalPlayer") {
			if (!as2_loose_equals(p_value, local_player_)) {
				local_player_ = p_value;
				state_changed();
			}
		} else if (p_name == "isMatchReady") {
			match_ready_ = p_value;
			state_changed();
		} else {
			RowBaseClass::member(p_name, p_value);
		}
	}

private:
	void set_text(std::string_view p_name, std::string_view p_text) const { host_->clip(p_name).set_text(p_text); }

	void set_local_squad(const As2Value &p_value) {
		if (as2_loose_equals(p_value, local_squad_)) {
			return;
		}
		local_squad_ = p_value;
		state_changed();
	}

	static void set_text_color(const As2Clip &p_label, uint32_t p_color) {
		if (!p_label.exists()) {
			return;
		}
		p_label.set_text_color(p_color);
		const bool black = p_color == 0u;
		p_label.set_filters({ as2_glow_filter(black ? 0u : kRowTextGlow, 1.0, 8.0f, 8.0f, black ? 0.75f : 1.0f, 1) });
	}

	void set_ready_state(const As2Value &p_state) const {
		const As2Clip ready = host_->clip("ReadyLabel");
		if (p_state.defined() && p_state.truthy()) {
			ready.set_visible(true);
			ready.set_text(host_->translate(kReadySid));
		} else {
			ready.set_visible(false);
		}
	}

	void setup_direct_access() {
		const std::string &self = host_->path();
		const auto at = [&](std::string_view p_name) { return host_->clip(p_name).path(); };
		register_direct_access_array({
				ListAccessItem{ "Label", "", UiDirectAccessOp::Text, at("Label") },
				ListAccessItem{ "Rank", "", UiDirectAccessOp::GotoAndStop, at("rankIcon") },
				ListAccessItem{ "Voip", "", UiDirectAccessOp::GotoAndStop, at("voipIcon") },
				ListAccessItem{ "Kills", "", UiDirectAccessOp::Text, at("KillsLabel") },
				ListAccessItem{ "Deaths", "", UiDirectAccessOp::Text, at("DeathsLabel") },
				ListAccessItem{ "Score", "", UiDirectAccessOp::Text, at("ScoreLabel") },
				ListAccessItem{ "Ping", "", UiDirectAccessOp::Text, at("PingLabel") },
				ListAccessItem{ "LocalSquad", "isLocalSquad", UiDirectAccessOp::Member, self },
				ListAccessItem{ "LocalPlayer", "isLocalPlayer", UiDirectAccessOp::Member, self },
				ListAccessItem{ "MatchReady", "isMatchReady", UiDirectAccessOp::Member, self },
		});
	}

	As2Value local_squad_ = As2Value::of(false);
	As2Value local_player_ = As2Value::of(false);
	As2Value match_ready_ = As2Value::of(false);
};

enum class HeaderSide : uint8_t {
	Friendly,
	Enemy,
};

enum class HeaderField : uint8_t {
	TeamName,
	Tickets,
	Kills,
	GameMode,
};

struct HeaderKey {
	int32_t data_key = 0;
	HeaderField field = HeaderField::TeamName;
	HeaderSide side = HeaderSide::Friendly;
};

constexpr std::array<HeaderKey, 7> kHeaderKeys{ {
		{ -909259276, HeaderField::TeamName, HeaderSide::Friendly },
		{ -1549914696, HeaderField::TeamName, HeaderSide::Enemy },
		{ -1863080887, HeaderField::Tickets, HeaderSide::Friendly },
		{ -1208210764, HeaderField::Tickets, HeaderSide::Enemy },
		{ -1885752899, HeaderField::Kills, HeaderSide::Friendly },
		{ 1935098548, HeaderField::Kills, HeaderSide::Enemy },
		{ 1733092213, HeaderField::GameMode, HeaderSide::Friendly },
} };

constexpr std::string_view kModeFriendly = "friendly";
constexpr std::string_view kModeSquad = "squad";
constexpr uint32_t kFriendlyColour = 7586815u;
constexpr uint32_t kSquadColour = 8429618u;
constexpr uint32_t kEnemyColour = 9843250u;
constexpr int32_t kHeaderRelease = 0;

bool mode_rush(const As2Value &p_mode) {
	return as2_game_mode_is(p_mode, "Rush");
}

bool mode_squad_rush(const As2Value &p_mode) {
	return as2_game_mode_is(p_mode, "SquadRush");
}

bool mode_gun_master(const As2Value &p_mode) {
	return as2_game_mode_is(p_mode, "GunMaster");
}

class ScoreboardHeaderClass final : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		if (const As2Value *m = r_host.construct_param("mode"); m != nullptr) {
			mode_ = m->to_string();
		}
	}

	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override { sources_ = p_node.sources; }

	void on_clip_load(HudWidgetHost &r_host) override {
		set_mode(mode_);
		r_host.clip("winnerText").set_visible(false);
		r_host.clip("ticketText").set_visible(false);
		r_host.clip("ObjectivesIcon").set_visible(false);
		r_host.clip("winnerText").set_text(r_host.translate("ID_H_SCOREBOARD_WINNER"));
		r_host.clip("killsText").set_text(r_host.translate("ID_H_SCOREBOARD_KILLS"));
		r_host.clip("deathsText").set_text(r_host.translate("ID_H_SCOREBOARD_DEATH"));
		r_host.clip("scoreText").set_text(r_host.translate("ID_H_SCOREBOARD_SCORE"));
		r_host.clip("pingText").set_text(r_host.translate("ID_M_SB_CONNECTION"));
		r_host.self().set_visible(false);
		if (mode_ == kModeSquad || !hud_platform_win32(r_host.client())) {
			const float ping = r_host.clip("pingText").width();
			for (const std::string_view name : { "killsText", "deathsText", "scoreText" }) {
				const As2Clip c = r_host.clip(name);
				c.set_x(c.x() + ping);
			}
			r_host.clip("pingText").set_visible(false);
		}
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		for (const HudDataSource &src : sources_) {
			const auto key = std::find_if(
					kHeaderKeys.begin(), kHeaderKeys.end(), [&](const HeaderKey &k) { return k.data_key == src.key; });
			if (key == kHeaderKeys.end() || key->field == HeaderField::TeamName && !p_client.localise_hash) {
				continue;
			}
			const bool friendly = key->side == HeaderSide::Friendly;
			As2Value v;
			switch (key->field) {
				case HeaderField::TeamName: {
					const uint32_t sid = friendly ? p_model.team.friendly_team_name : p_model.team.enemy_team1_name;
					if (sid != 0) {
						v = As2Value::of(p_client.localise_hash(sid));
					}
					break;
				}
				case HeaderField::Tickets:
					v = As2Value::of(
							friendly ? p_model.tickets.friendly_team_tickets : p_model.tickets.enemy_team_tickets);
					break;
				case HeaderField::Kills:
					v = As2Value::of(
							friendly ? p_model.kill_counter.player_team_kills : p_model.kill_counter.enemy_team_kills);
					break;
				case HeaderField::GameMode:
					v = As2Value::of(hud_string(p_model.settings.current_game_mode));
					break;
			}
			r_out.push_back({ src.name, std::move(v), src.on_initialize });
		}
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		const As2Value *mode = p_init.value("GameMode");
		game_mode_ = mode != nullptr ? *mode : As2Value();
		const As2Value *attacker = p_init.value("RushIsAttacker");
		update_rush_is_attacker(attacker != nullptr ? *attacker : As2Value());
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "TeamName") {
			update_team_name(p_value);
		} else if (p_key == "TeamIsWinner") {
			r_host.clip("winnerText").set_visible(as2_equals_true(p_value));
			update_winner_pos();
		} else if (p_key == "TeamID") {
			if (p_value.defined()) {
				const std::string sid = "ID_H_SDM_TEAM_" + As2Value::of(p_value.to_number() - 1.0).to_string();
				text("teamText").set_text(r_host.translate(sid));
				update_ticket_pos();
				update_winner_pos();
			}
		} else if (p_key == "Tickets" || p_key == "Kills") {
			if (mode_squad_rush(game_mode_) || mode_rush(game_mode_) || mode_gun_master(game_mode_)) {
				return;
			}
			text("ticketText").set_text(p_value.to_string());
			text("ticketText").set_visible(true);
			update_ticket_pos();
			update_winner_pos();
		} else if (p_key == "GameMode") {
			game_mode_ = p_value;
			update_team_name(As2Value::of(text("teamText").text()));
		} else if (p_key == "RushIsAttacker") {
			update_rush_is_attacker(p_value);
		} else if (p_key == "RushAttackerLife") {
			attacker_info_ = p_value;
			if (mode_rush(game_mode_) || mode_squad_rush(game_mode_)) {
				update_rush();
			}
		} else if (p_key == "RushDefenderInfo") {
			defender_info_ = p_value;
			if (mode_rush(game_mode_) || mode_squad_rush(game_mode_)) {
				update_rush();
			}
		} else if (p_key == "BackgroundMode") {
			set_mode(p_value.to_string());
		}
	}

	bool input_concept(HudWidgetHost &r_host, int32_t p_concept, bool p_pressed) override {
		if (!p_pressed && UiInputConcept(p_concept) == UiInputConcept::Tab) {
			r_host.fire_event(kHeaderRelease, As2Value::of(0));
		}
		return false;
	}

private:
	As2Clip text(std::string_view p_name) const { return host_->clip(p_name); }

	void update_team_name(const As2Value &p_data) {
		if (!p_data.defined()) {
			return;
		}
		text("teamText").set_text(host_->translate(p_data.to_string()));
		update_ticket_pos();
		update_winner_pos();
	}

	void update_rush_is_attacker(const As2Value &p_data) {
		is_attacker_ = p_data;
		if (mode_rush(game_mode_) || mode_squad_rush(game_mode_)) {
			update_rush();
		}
	}

	void update_rush() {
		if (!attacker_info_.defined() || !is_attacker_.defined() || !defender_info_.defined()) {
			return;
		}
		const As2Clip icons = host_->clip("ObjectivesIcon");
		if (is_attacker_.truthy() == (mode_ == kModeFriendly)) {
			icons.set_visible(false);
			text("ticketText").set_text(attacker_info_.to_string());
			text("ticketText").set_visible(true);
		} else {
			text("ticketText").set_visible(false);
			icons.goto_and_stop("right");
			icons.set_visible(true);
			const size_t count = defender_info_.size();
			for (size_t i = 0; i < 2; ++i) {
				const As2Value &info = defender_info_.at(i);
				const As2Clip icon =
						icons.child("icon_" + As2Value::of(info.field("Index").to_number() + 1.0).to_string());
				if (i >= count) {
					icon.set_visible(false);
					continue;
				}
				icon.set_visible(true);
				const As2Value &frame = info.field("Icon");
				if (frame.kind == As2Value::Kind::String) {
					icon.child("icon").goto_and_stop(std::string_view(frame.text));
				} else {
					goto_and_stop_number(icon.child("icon"), frame.to_number());
				}
				if (info.field("IsAttacked").truthy()) {
					icon.goto_and_play("blink");
				} else {
					icon.goto_and_stop("idle");
				}
			}
		}
		update_team_name(As2Value::of(text("teamText").text()));
		update_objectives_pos();
	}

	void update_winner_pos() const {
		const As2Clip winner = text("winnerText");
		if (!winner.visible()) {
			return;
		}
		const As2Clip ticket = text("ticketText");
		const As2Clip icons = host_->clip("ObjectivesIcon");
		if (ticket.visible()) {
			winner.set_x(ticket.x() + ticket.text_width() + 25.0f);
		} else if (icons.visible()) {
			winner.set_x(icons.x() + icons.width() + 15.0f);
		} else {
			const As2Clip team = text("teamText");
			winner.set_x(team.x() + team.text_width() + 25.0f);
		}
	}

	void update_ticket_pos() const {
		const As2Clip ticket = text("ticketText");
		if (ticket.visible()) {
			const As2Clip team = text("teamText");
			ticket.set_x(team.x() + team.text_width() + 25.0f);
		}
	}

	void update_objectives_pos() const {
		const As2Clip icons = host_->clip("ObjectivesIcon");
		if (icons.visible()) {
			const As2Clip team = text("teamText");
			icons.set_x(team.x() + team.text_width() + 25.0f);
		}
	}

	void set_mode(const std::string &p_mode) const {
		uint32_t colour = kEnemyColour;
		if (p_mode == kModeFriendly) {
			colour = kFriendlyColour;
		} else if (p_mode == kModeSquad) {
			colour = kSquadColour;
		}
		set_tint(host_->clip("bg"), colour);
		for (const std::string_view name : { "scoreText", "deathsText", "killsText", "teamText" }) {
			text(name).set_text_color(kWhite);
		}
		for (const std::string_view name : { "teamText", "killsText", "deathsText", "scoreText" }) {
			text(name).set_filters({ as2_glow_filter(colour, 1.0, 7.0f, 7.0f, 3.0f, 2) });
		}
	}

	static void set_tint(const As2Clip &p_target, uint32_t p_rgb) {
		As2ColorTransformPatch t;
		t.ra = 0.0;
		t.rb = double((p_rgb >> 16) & 0xffu);
		t.ga = 0.0;
		t.gb = double((p_rgb >> 8) & 0xffu);
		t.ba = 0.0;
		t.bb = double(p_rgb & 0xffu);
		t.aa = 100.0;
		t.ab = 0.0;
		p_target.set_color_transform(t);
	}

	HudWidgetHost *host_ = nullptr;
	std::vector<HudDataSource> sources_;
	std::string mode_ = std::string(kModeFriendly);
	As2Value game_mode_;
	As2Value is_attacker_;
	As2Value attacker_info_;
	As2Value defender_info_;
};

class ScoreboardSpectatorBoxClass final : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override { r_host.self().set_visible(false); }

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key != "Spectators") {
			return;
		}
		const size_t count = p_value.size();
		if (count > 0) {
			std::string label;
			std::string separator;
			for (size_t i = 0; i < count; ++i) {
				label = separator + p_value.at(i).field("Name").to_string();
				separator = ", ";
			}
			r_host.clip("Label0").set_text(label);
		}
		r_host.self().set_visible(count > 0);
	}
};

constexpr std::string_view kTeamCategory = "ui/uicomponents/uiteamcomp";
constexpr int32_t kFriendlyTeamPlayersKey = 644095251;
constexpr int32_t kEnemyTeam1PlayersKey = -163624481;
constexpr int32_t kRankZeroFrame = 1;
constexpr int32_t kVoipOffFrame = 2;
constexpr const char *kNoPing = "-";

As2Value scoreboard_row(const HudModel &p_model, uint8_t p_slot) {
	const HudPlayer &p = p_model.players[p_slot];
	As2Value r = As2Value::object();
	r.set("Index", As2Value::of(int32_t(p_slot)));
	r.set("Label", As2Value::of(hud_player_name(p_model, p_slot)));
	r.set("Rank", As2Value::of(kRankZeroFrame));
	r.set("Voip", As2Value::of(kVoipOffFrame));
	r.set("Kills", As2Value::of(int32_t(p.kills)));
	r.set("Deaths", As2Value::of(int32_t(p.deaths)));
	r.set("Score", As2Value::of(p.score));
	r.set("Ping", p.bot ? As2Value::of(kNoPing) : As2Value::of(int32_t(p.ping_ms)));
	r.set("LocalSquad", As2Value::of(false));
	r.set("LocalPlayer", As2Value::of(p_slot == p_model.local_player));
	r.set("MatchReady", As2Value::of(false));
	return r;
}
}

std::optional<std::vector<As2Value>> hud_model_direct_rows(const HudDataSource &p_query, const HudModel &p_model) {
	if (p_query.category != kTeamCategory) {
		return std::nullopt;
	}
	const bool friendly = p_query.key == kFriendlyTeamPlayersKey;
	if (!friendly && p_query.key != kEnemyTeam1PlayersKey) {
		return std::nullopt;
	}
	const HudPlayerList *list = friendly ? &p_model.team.friendly_team_players : &p_model.team.enemy_team1_players;
	std::vector<As2Value> rows;
	for (size_t i = 0; i < std::min<size_t>(list->count, list->index.size()); ++i) {
		const uint8_t slot = list->index[i];
		if (slot < p_model.players.size()) {
			rows.push_back(scoreboard_row(p_model, slot));
		}
	}
	return rows;
}

void register_hud_widgets_scoreboard(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.List.Rows.ScoreBoardRow", [] { return std::make_unique<ScoreBoardRowClass>(); });
	r_registry.add("Widget.List.ScoreboardHeader", [] { return std::make_unique<ScoreboardHeaderClass>(); });
	r_registry.add("Widget.ScreenSpecific.ScoreboardSpectatorBox",
			[] { return std::make_unique<ScoreboardSpectatorBoxClass>(); });
}

}
