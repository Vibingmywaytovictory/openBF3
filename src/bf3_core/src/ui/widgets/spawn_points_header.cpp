#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class SpawnPointsHeaderClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		const float w = r_host.self().width();
		r_host.self().set_xscale(100.0f);
		r_host.clip("bg").set_width(w);
		r_host.clip("txt1").set_auto_size(SwfTextAutoSize::Left);
		r_host.clip("txt2").set_auto_size(SwfTextAutoSize::Left);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		if (const std::string *s = p_init.property("IsSpawnScreen"); s != nullptr && !s->empty()) {
			is_spawn_screen_ = *s == "true";
		}
		setup_refresh_data(*this, r_host, p_init.data);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (!p_value.defined()) {
			return;
		}
		if (p_key == "GameMode") {
			game_mode_ = p_value.to_string();
		} else if (p_key == "IsAttacker") {
			is_attacker_ = p_value.truthy();
		} else if (p_key == "TeamName") {
			team_name_ = p_value.to_string();
		} else if (p_key == "TeamNumber") {
			if (std::isnan(p_value.to_number())) {
				return;
			}
			team_number_ = p_value.to_number();
		} else {
			return;
		}
		update_text(r_host);
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		r_out.push_back({ "GameMode", As2Value::of(hud_string(p_model.settings.current_game_mode)) });
		r_out.push_back({ "IsAttacker", As2Value::of(false) });
		if (p_client.localise_hash && p_model.team.friendly_team_name != 0) {
			r_out.push_back({ "TeamName", As2Value::of(p_client.localise_hash(p_model.team.friendly_team_name)) });
		}
		if (p_model.team.friendly_team > 0) {
			r_out.push_back({ "TeamNumber", As2Value::of(p_model.team.friendly_team) });
		}
	}

private:
	void update_text(HudWidgetHost &r_host) {
		const std::string name = team_name_.value_or(std::string());
		if (team_name_ && team_name_->empty()) {
			return;
		}
		if (is_spawn_screen_) {
			set_text(r_host, name, "ID_M_SPAWN_DEPLOY_POINTS");
		} else {
			set_text(r_host, "ID_M_TEAM", name);
		}
	}

	static void set_text(HudWidgetHost &r_host, std::string_view p_text1, std::string_view p_text2) {
		const As2Clip t1 = r_host.clip("txt1");
		const As2Clip t2 = r_host.clip("txt2");
		t1.set_text(r_host.translate(p_text1));
		t2.set_text(r_host.translate(p_text2));
		t2.set_x(t1.x() + t1.text_width() + kTextGap);
	}

	static constexpr float kTextGap = 10.0f;
	bool is_spawn_screen_ = true;
	std::string game_mode_;
	bool is_attacker_ = false;
	std::optional<std::string> team_name_;
	double team_number_ = 0.0;
};

}

void register_hud_widgets_spawn_points_header(HudWidgetRegistry &r_registry) {
	r_registry.add(
			"Widget.ScreenSpecific.SpawnPointsHeader", [] { return std::make_unique<SpawnPointsHeaderClass>(); });
}

}
