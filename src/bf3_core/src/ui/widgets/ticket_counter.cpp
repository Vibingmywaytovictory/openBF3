#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class TicketCounterClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		r_host.renderer(r_host.self(), "t2");
		r_host.tilt(r_host.self(), -15.0f, -15.0f, kHudPerspectiveFov);
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		mode_ = param_string(r_host, "m_gamemode");
		setup_gamemodes(r_host);
		setup_refresh_data(*this, r_host, p_init.data);
		if (const As2Value *hc = p_init.value("IsHardcore"); hc != nullptr && hc->truthy()) {
			r_host.self().set_y(r_host.self().y() + float(kMinimapSize));
		}
		layout_gamemodes(r_host);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Team1TDMKills") {
			team1_ = p_value;
			if (player_team_ && *player_team_ == 0.0) {
				friendly(r_host, p_value);
			} else {
				enemy(r_host, p_value);
			}
		} else if (p_key == "Team2TDMKills") {
			team2_ = p_value;
			if (player_team_ && *player_team_ == 1.0) {
				friendly(r_host, p_value);
			} else {
				enemy(r_host, p_value);
			}
		} else if (p_key == "PlayerTeam") {
			const double t = p_value.to_number() - 1.0;
			if (!player_team_ || *player_team_ != t) {
				player_team_ = t;
				if (mode_ == "tdm") {
					update(r_host, "Team1TDMKills", team1_);
					update(r_host, "Team2TDMKills", team2_);
				}
			}
		} else if (p_key == "FriendlyTDMTeamName" || p_key == "FriendlyTeamName") {
			const std::string s = p_value.to_string();
			if (s != friendly_name_) {
				friendly_name_ = s;
				r_host.clip(friendly_row_).child("team").set_text(s);
			}
		} else if (p_key == "EnemyTDMTeamName" || p_key == "EnemyTeamName") {
			const std::string s = p_value.to_string();
			if (s != enemy_name_) {
				enemy_name_ = s;
				r_host.clip(enemy_row_).child("team").set_text(s);
			}
		} else if (p_key == "FriendlyTickets") {
			friendly(r_host, p_value);
		} else if (p_key == "EnemyTickets") {
			enemy(r_host, p_value);
		} else if (p_key == "MaxTickets") {
			if (!max_tickets_ || !(*max_tickets_ == p_value)) {
				max_tickets_ = p_value;
				setup_gamemodes(r_host);
			}
		}
	}

	void drive(HudWidgetHost &r_host, const HudModel &p_model, const HudClientState &p_client) override {
		r_host.self().set_visible(p_client.minimap_small && !p_model.user.is_in_menu);
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		const HudTeam &t = p_model.team;
		const HudKillCounter &k = p_model.kill_counter;
		if (p_client.localise_hash) {
			if (t.friendly_team_name != 0) {
				r_out.push_back({ "FriendlyTDMTeamName", As2Value::of(p_client.localise_hash(t.friendly_team_name)) });
			}
			if (t.enemy_team1_name != 0) {
				r_out.push_back({ "EnemyTDMTeamName", As2Value::of(p_client.localise_hash(t.enemy_team1_name)) });
			}
		}
		r_out.push_back({ "Team1TDMKills", As2Value::of(k.team1_kills) });
		r_out.push_back({ "Team2TDMKills", As2Value::of(k.team2_kills) });
		if (k.player_team > 0) {
			r_out.push_back({ "PlayerTeam", As2Value::of(k.player_team) });
		}
		if (k.team1_max_kills > 0) {
			r_out.push_back({ "MaxTickets", As2Value::of(k.team1_max_kills) });
		}
		r_out.push_back({ "IsHardcore", As2Value::of(p_client.hardcore) });
	}

	const std::string &friendly_row() const { return friendly_row_; }
	const std::string &enemy_row() const { return enemy_row_; }

private:
	void show_teams(HudWidgetHost &r_host, int32_t p_n) {
		for (int32_t i = 0; i < 4; ++i) {
			if (i >= p_n) {
				r_host.clip("tickets" + std::to_string(i)).set_visible(false);
			}
		}
	}

	void setup_gamemodes(HudWidgetHost &r_host) {
		if (mode_ != "tdm") {
			return;
		}
		show_teams(r_host, 2);
		friendly_row_ = "tickets1";
		enemy_row_ = "tickets0";
		const As2Clip enemy_row = r_host.clip(enemy_row_);
		enemy_row.goto_and_stop("enemy");
		row_frame_script(enemy_row, 2);
		row_frame_script(r_host.clip(friendly_row_), 1);
		r_host.clip(friendly_row_).child("team").set_text(friendly_name_);
		enemy_row.child("team").set_text(enemy_name_);
		update(r_host, "Team1TDMKills", team1_);
		update(r_host, "Team2TDMKills", team2_);
	}

	static void row_frame_script(const As2Clip &p_row, int32_t p_frame) {
		const As2Clip meter = p_row.child("meter");
		for (const char *n : { "meter", "leftEdge", "rightEdge", "bg" }) {
			meter.child(n).goto_and_stop(p_frame);
		}
	}

	void layout_gamemodes(HudWidgetHost &r_host) {
		if (mode_ != "tdm") {
			return;
		}
		const As2Clip first = r_host.clip(friendly_row_);
		const As2Clip second = r_host.clip(enemy_row_);
		const float name_w = std::max(first.child("team").text_width(), second.child("team").text_width());
		const float tick_w = std::max(first.child("tickets").text_width(), second.child("tickets").text_width());
		const float right = std::clamp(kNameFieldOffset + name_w + kMinPadding + tick_w, kMinRightBound, kMaxRightBound);
		const float bar_x = std::clamp(right - kBarOverlap, kBarRightEnd - kBarNormalWidth, 100000.0f);
		for (const As2Clip &row : { first, second }) {
			const As2Clip tickets = row.child("tickets");
			tickets.set_x(right - tick_w - kTicketMargin);
			tickets.set_width(tick_w + kTicketMargin);
		}
		for (const As2Clip &row : { first, second }) {
			const As2Clip meter = row.child("meter");
			meter.set_xscale((kBarRightEnd - bar_x) / kBarNormalWidth * 100.0f);
			meter.set_x(bar_x);
			update_tickets(row, As2Value::of(As2Value::of(row.child("tickets").text()).to_number()));
		}
	}

	void friendly(HudWidgetHost &r_host, const As2Value &p_v) {
		if (!friendly_row_.empty()) {
			update_tickets(r_host.clip(friendly_row_), p_v);
		}
	}

	void enemy(HudWidgetHost &r_host, const As2Value &p_v) {
		if (!enemy_row_.empty()) {
			update_tickets(r_host.clip(enemy_row_), p_v);
		}
	}

	void update_tickets(const As2Clip &p_row, const As2Value &p_v) const {
		p_row.child("tickets").set_text(p_v.to_string());
		const As2Clip m = p_row.child("meter");
		const double max = max_tickets_ ? max_tickets_->to_number() : std::nan("");
		const double pct = std::max(std::min(100.0, as2_round(p_v.to_number() / max * 100.0)), 0.0);
		if (!std::isnan(as2_round(p_v.to_number() / max * 100.0))) {
			m.child("meter").set_xscale(float(pct));
		}
		m.child("rightEdge").set_x(m.child("meter").x() + m.child("meter").width());
		if (p_v.to_number() == 0.0) {
			m.child("leftEdge").set_visible(false);
			m.child("rightEdge").set_visible(false);
		}
	}

	static constexpr float kMinimapSize = 170.0f;
	static constexpr float kNameFieldOffset = 8.0f;
	static constexpr float kMinRightBound = 80.0f;
	static constexpr float kMaxRightBound = 105.0f;
	static constexpr float kBarNormalWidth = 90.0f;
	static constexpr float kBarRightEnd = 170.0f;
	static constexpr float kMinPadding = 4.0f;
	static constexpr float kBarOverlap = 4.0f;
	static constexpr float kTicketMargin = 20.0f;
	std::string mode_;
	std::string friendly_row_;
	std::string enemy_row_;
	std::string friendly_name_ = "YOU";
	std::string enemy_name_ = "THEM";
	As2Value team1_ = As2Value::of(0);
	As2Value team2_ = As2Value::of(0);
	std::optional<double> player_team_;
	std::optional<As2Value> max_tickets_;
};

}

void register_hud_widgets_ticket_counter(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.TicketCounter.TicketCounter", [] { return std::make_unique<TicketCounterClass>(); });
}

}
