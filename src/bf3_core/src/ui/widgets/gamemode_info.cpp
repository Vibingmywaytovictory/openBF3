#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

constexpr int32_t kGamemodeKey = 1733092213;
constexpr std::string_view kGamemodeSource = "Gamemode";

class GamemodeInfoClass final : public HudWidgetController {
public:
	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override { sources_ = p_node.sources; }

	void on_clip_load(HudWidgetHost &r_host) override { r_host.self().set_visible(false); }

	void bind(const HudModel &p_model, const HudClientState &, HudData &r_out) const override {
		for (const HudDataSource &s : sources_) {
			if (s.key == kGamemodeKey || s.name == kGamemodeSource) {
				r_out.push_back(HudDataEntry{
						s.name, As2Value::of(hud_string(p_model.settings.current_game_mode)), s.on_initialize });
			}
		}
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Gamemode") {
			update_gamemode(r_host, p_value.to_string());
		} else if (p_key == "IsAttacker") {
			attacker_ = p_value.truthy();
			if (!gamemode_.empty()) {
				update_gamemode(r_host, gamemode_);
			}
		}
	}

private:
	void update_gamemode(HudWidgetHost &r_host, const std::string &p_mode) {
		gamemode_ = p_mode;
		for (const std::string_view name : { "conquest", "rushAttacker", "rushDefender", "ctf" }) {
			r_host.clip(name).set_visible(false);
		}
		if (gamemode_.starts_with("Conquest")) {
			r_host.clip("conquest").set_visible(true);
		} else if (gamemode_.starts_with("CTF")) {
			r_host.clip("ctf").set_visible(true);
		} else if (gamemode_.starts_with("Rush")) {
			r_host.clip(attacker_ ? "rushAttacker" : "rushDefender").set_visible(true);
		}
	}

	std::vector<HudDataSource> sources_;
	std::string gamemode_;
	bool attacker_ = true;
};

}

void register_hud_widgets_gamemode_info(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Info.GamemodeInfo", [] { return std::make_unique<GamemodeInfoClass>(); });
}

}
