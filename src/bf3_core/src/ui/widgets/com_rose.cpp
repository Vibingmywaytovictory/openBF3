#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

constexpr std::array<std::string_view, 12> kRoseButtons{ "L0", "L1", "L2", "L3", "L4", "R0", "R1", "R2", "R3", "R4",
	"M", "D" };
constexpr int32_t kRoseDiedEvent = 0;
constexpr int32_t kRoseResetEvent = 1;
constexpr size_t kRoseMedicButton = 4;
constexpr size_t kRoseRideButton = 6;
constexpr size_t kRoseAmmoButton = 9;
constexpr size_t kRoseOrderButton = 11;

class ComRoseClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override { r_host.self().set_visible(false); }

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &) override {
		const As2Clip rose = r_host.clip("ComRose");
		for (size_t i = 0; i < kRoseButtons.size(); ++i) {
			buttons_[i] = rose.child(kRoseButtons[i]);
			r_host.renderer(buttons_[i], "t2");
		}
		r_host.self().set_visible(true);
		reset();
	}

	void in_event(HudWidgetHost &, int32_t p_index, const As2Value &) override {
		if (p_index == kRoseResetEvent) {
			reset();
		}
	}

	void bind(const HudModel &p_model, const HudClientState &, HudData &r_out) const override {
		r_out.push_back({ "SelectedButton", As2Value::of(double(p_model.commo_rose.selected_button)) });
		r_out.push_back({ "InVehicle", As2Value::of(p_model.commo_rose.in_vehicle) });
		r_out.push_back({ "IsAlive", As2Value::of(p_model.commo_rose.alive) });
		r_out.push_back({ "IsLeaderInActiveSquad", As2Value::of(p_model.commo_rose.leader) });
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "SelectedButton") {
			select(as2_int(p_value));
		} else if (p_key == "InVehicle") {
			const bool v = p_value.truthy();
			buttons_[kRoseMedicButton].child("txt").set_text(
					r_host.translate(v ? "ID_H_COMMO_NEEDREPAIR" : "ID_H_COMMO_NEEDMEDIC"));
			buttons_[kRoseRideButton].child("txt").set_text(
					r_host.translate(v ? "ID_H_COMMO_GETIN" : "ID_H_COMMO_NEEDRIDE"));
			buttons_[kRoseAmmoButton].child("txt").set_text(
					r_host.translate(v ? "ID_H_COMMO_BAILOUT" : "ID_H_COMMO_NEEDAMMO"));
		} else if (p_key == "IsAlive") {
			if (!p_value.truthy()) {
				r_host.fire_event(kRoseDiedEvent);
			}
		} else if (p_key == "IsLeaderInActiveSquad") {
			buttons_[kRoseOrderButton].set_visible(p_value.truthy());
		}
	}

	int32_t selected() const { return old_; }

private:
	void reset() {
		for (const As2Clip &b : buttons_) {
			b.child("bg").goto_and_stop("idle");
		}
		old_ = -1;
	}

	void select(int32_t p_button) {
		if (p_button < 0 || size_t(p_button) >= buttons_.size() || p_button == old_) {
			return;
		}
		if (old_ != -1) {
			buttons_[size_t(old_)].child("bg").goto_and_play("unselect");
		}
		buttons_[size_t(p_button)].child("bg").goto_and_play("select");
		old_ = p_button;
	}

	std::array<As2Clip, kRoseButtons.size()> buttons_{};
	int32_t old_ = -1;
};

}

void register_hud_widgets_com_rose(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Hud.ComRose", [] { return std::make_unique<ComRoseClass>(); });
}

}
