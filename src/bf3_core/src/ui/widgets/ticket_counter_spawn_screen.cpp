#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class TicketCounterSpawnScreenClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override { r_host.self().set_visible(false); }

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		mode_ = param_string(r_host, "m_gamemode");
		setup_gamemodes(r_host);
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		const As2Clip ticket = r_host.clip("Ticket");
		if (p_key == "SDM_PlayerTeam") {
			player_team_ = p_value.to_number() - 1.0;
		} else if (p_key == "TDM_FriendlyTickets") {
			ticket.child(player_team_ == 0.0 ? "txtTeam1Tickets" : "txtTeam2Tickets").set_text(p_value.to_string());
		} else if (p_key == "TDM_EnemyTickets") {
			ticket.child(player_team_ == 0.0 ? "txtTeam2Tickets" : "txtTeam1Tickets").set_text(p_value.to_string());
		} else if (p_key == "TDM_FriendlyTeamName") {
			if (friendly_name_ != p_value.to_string()) {
				friendly_name_ = p_value.to_string();
				ticket.child("txtTeam1Team").set_text(r_host.translate(friendly_name_));
				setup_gamemodes(r_host);
			}
		} else if (p_key == "TDM_EnemyTeamName") {
			if (enemy_name_ != p_value.to_string()) {
				enemy_name_ = p_value.to_string();
				ticket.child("txtTeam2Team").set_text(r_host.translate(enemy_name_));
				setup_gamemodes(r_host);
			}
		}
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		const HudKillCounter &k = p_model.kill_counter;
		if (k.player_team > 0) {
			r_out.push_back({ "SDM_PlayerTeam", As2Value::of(k.player_team) });
		}
		r_out.push_back({ "TDM_FriendlyTickets", As2Value::of(k.player_team_kills) });
		r_out.push_back({ "TDM_EnemyTickets", As2Value::of(k.enemy_team_kills) });
		if (p_client.localise_hash) {
			if (p_model.team.friendly_team_name != 0) {
				r_out.push_back({ "TDM_FriendlyTeamName",
						As2Value::of(p_client.localise_hash(p_model.team.friendly_team_name)) });
			}
			if (p_model.team.enemy_team1_name != 0) {
				r_out.push_back(
						{ "TDM_EnemyTeamName", As2Value::of(p_client.localise_hash(p_model.team.enemy_team1_name)) });
			}
		}
	}

private:
	void setup_gamemodes(HudWidgetHost &r_host) {
		const As2Clip ticket = r_host.clip("Ticket");
		ticket.goto_and_stop(mode_);
		if (mode_ == "sdm") {
			for (int32_t i = 0; i < kSdmTeams; ++i) {
				const std::string name = r_host.translate("ID_H_SDM_TEAM_" + std::to_string(i) + "_SHORT");
				ticket.child("myTeam_" + std::to_string(i)).child("name").set_text(name);
				ticket.child("otherTeam_" + std::to_string(i)).child("name").set_text(name);
			}
		} else if (mode_ == "conquest" || mode_ == "rush" || mode_ == "tdm" || mode_ == "ctf") {
			ticket.child("txtTeam1Team").set_text(r_host.translate(friendly_name_));
			ticket.child("txtTeam2Team").set_text(r_host.translate(enemy_name_));
		}
	}

	static constexpr int32_t kSdmTeams = 4;
	std::string mode_;
	std::string friendly_name_ = "YOU";
	std::string enemy_name_ = "ID_H_RUSH_DEFENDERS";
	double player_team_ = std::nan("");
};

}

void register_hud_widgets_ticket_counter_spawn_screen(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.TicketCounter.TicketCounterSpawnScreen",
			[] { return std::make_unique<TicketCounterSpawnScreenClass>(); });
}

}
