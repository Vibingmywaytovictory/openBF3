#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class CompassClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		type_ = param_string(r_host, "m_compassType");
		mp_ = type_ == "multiplayerCompass";
		const std::string linkage = mp_ || type_ == "singlePlayerCompass" ? std::string("mc_singlePlayerCompass") : "mc_" + type_ + "_us";
		heading_ = r_host.attach_movie(r_host.self(), linkage, type_, r_host.self().next_highest_depth());
		heading_.set_x(0.0f);
		heading_.set_y(0.0f);
		if (linkage == "mc_singlePlayerCompass") {
			r_host.renderer(heading_, "t2");
			r_host.tilt(heading_, -15.0f, -15.0f, kHudPerspectiveFov);
		}
		objectives_.clear();
		for (int32_t i = 1;; ++i) {
			const As2Clip obj = heading_.child("objective" + std::to_string(i)).child("obj");
			if (!obj.exists()) {
				break;
			}
			objectives_.push_back(obj);
			obj.goto_and_stop(kObjectiveIdleFrame);
		}
		if (mp_) {
			heading_.child("bg").set_visible(false);
			for (const As2Clip &obj : objectives_) {
				obj.set_visible(false);
			}
		}
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		heading_.child("bg").set_visible(false);
		heading_.goto_and_stop("us");
		if (p_init.value("Visible") == nullptr && p_init.value("VisibleAccess") == nullptr) {
			r_host.self().set_visible(true);
		}
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Heading") {
			heading_.child("cps").goto_and_stop(int32_t(std::trunc(p_value.to_number())));
		} else if (p_key == "Visible") {
			r_host.self().set_visible(p_value.truthy());
		}
	}

	void drive(HudWidgetHost &, const HudModel &p_model, const HudClientState &) override {
		heading_.child("cps").goto_and_stop(hud_compass_frame(p_model.soldier.compass_angle) + 1);
	}

	const As2Clip &heading_clip() const { return heading_; }
	const std::vector<As2Clip> &objectives() const { return objectives_; }

private:
	static constexpr int32_t kObjectiveIdleFrame = 180;
	std::vector<As2Clip> objectives_;
	std::string type_;
	bool mp_ = false;
	As2Clip heading_;
};

}

void register_hud_widgets_compass(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Hud.Compass", [] { return std::make_unique<CompassClass>(); });
}

}
