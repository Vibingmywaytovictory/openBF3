#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class MatchPreroundInformationClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		background_ = r_host.clip("background");
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key != "PreroundStatus") {
			return;
		}
		const std::string pattern = r_host.translate(kPlayersReadySid);
		const std::array<std::string, 2> args = { std::to_string(as2_int(p_value.field("PlayersReady"))),
			std::to_string(as2_int(p_value.field("PlayersRequired"))) };
		r_host.clip("playersJoined").set_text(hud_format_text(pattern, args));
		r_host.clip("timer").set_text(p_value.field("CountdownSeconds").to_string());
		std::string_view status = kSpectatorSid;
		if (!p_value.field("IsSpectator").truthy()) {
			status = p_value.field("IsLocalPlayerReady").truthy() ? kReadySid : kNotReadySid;
		}
		r_host.clip("playerStatus").set_text(r_host.translate(status));
	}

	void in_event(HudWidgetHost &r_host, int32_t p_index, const As2Value &) override {
		if (p_index == kShowInformation) {
			r_host.self().set_visible(true);
			if (HudBackgroundWidgetApi *bg = controller_at<HudBackgroundWidgetApi>(r_host, background_)) {
				bg->initialize_without_data();
			}
		} else if (p_index == kHideInformation) {
			r_host.self().set_visible(false);
		}
	}

private:
	static constexpr int32_t kShowInformation = 0;
	static constexpr int32_t kHideInformation = 1;
	static constexpr std::string_view kPlayersReadySid = "ID_M_PREROUND_HUD_PLAYERSREADY";
	static constexpr std::string_view kSpectatorSid = "ID_M_PREROUND_HUD_SPECTATOR";
	static constexpr std::string_view kReadySid = "ID_M_PREROUND_HUD_READYSTATE";
	static constexpr std::string_view kNotReadySid = "ID_M_PREROUND_HUD_NOTREADYSTATE";

	As2Clip background_;
};

}

void register_hud_widgets_match_preround_information(HudWidgetRegistry &r_registry) {
	r_registry.add(
			"Widget.Hud.MatchPreroundInformation", [] { return std::make_unique<MatchPreroundInformationClass>(); });
}

}
