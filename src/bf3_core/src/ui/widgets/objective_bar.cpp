#include "hud_widget_support.h"

#include "bf3/core/ui/hud_icons.h"

namespace bf3::core::ui {

namespace {

class ObjectiveBarClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		mode_ = param_string(r_host, "m_gamemode");
		const As2Clip self = r_host.self();
		if (mode_ != "conquest" && (mode_ == "rush" || mode_ == "tdm" || mode_ == "sdm")) {
			self.child("conquest").set_visible(false);
		}
		if (mode_ != "rush" && (mode_ == "conquest" || mode_ == "tdm" || mode_ == "sdm")) {
			self.child("rush").set_visible(false);
		}
		r_host.renderer(self, "t2");
		r_host.tilt(self, -15.0f, -15.0f, kHudPerspectiveFov);
		self.set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		if (const As2Value *hc = p_init.value("IsHardcore"); hc != nullptr && hc->truthy()) {
			r_host.self().set_y(r_host.self().y() + kMinimapSize);
		}
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "ObjectiveStatus") {
			update_objective_status(r_host, p_value);
		}
	}

	void drive(HudWidgetHost &r_host, const HudModel &p_model, const HudClientState &p_client) override {
		r_host.self().set_visible(p_client.minimap_small && !p_model.user.is_in_menu);
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		r_out.push_back({ "ObjectiveStatus", hud_objective_status(p_model.capture_points.capturepoints_status_data) });
		r_out.push_back({ "IsHardcore", As2Value::of(p_client.hardcore) });
	}

private:
	void update_objective_status(HudWidgetHost &r_host, const As2Value &p_data) {
		std::vector<As2Value> entries = p_data.items;
		std::stable_sort(entries.begin(), entries.end(), [](const As2Value &a, const As2Value &b) {
			return last_char(a.field("Label")) < last_char(b.field("Label"));
		});
		const As2Clip objectives = r_host.self().child(mode_);
		const size_t n = entries.size();
		float width = 0.0f;
		for (size_t i = 0; i < kMaxObjectives; ++i) {
			const As2Clip icon = objectives.child("icon" + std::to_string(i));
			if (i >= n) {
				icon.set_visible(false);
				continue;
			}
			const As2Value &e = entries[i];
			icon.set_visible(true);
			const double frame = e.field("Icon").to_number();
			goto_and_stop_number(icon.child("icon"), frame);
			if (const std::string_view label = label_frame(frame); !label.empty()) {
				icon.child("label").goto_and_stop(std::string(label));
			}
			icon.child("label").child("nameTxt").set_text(as2_text(e.field("Label")));
			width = kObjectiveIconWidth * float(i);
			if (e.field("IsAttacked").truthy()) {
				icon.goto_and_play("blink");
			} else {
				icon.goto_and_stop("idle");
			}
		}
		if (n > 0 && n <= kObjectivesOffset.size()) {
			objectives.set_x(float(as2_round(kBarWidth / 2.0 - width / 2.0)) + kObjectivesOffset[n - 1]);
		}
	}

	static std::string last_char(const As2Value &p_label) {
		const std::string s = p_label.to_string();
		return s.empty() ? s : s.substr(s.size() - 1);
	}

	static std::string_view label_frame(double p_icon) {
		if (p_icon == double(kUiHudIconEnemyFlag)) {
			return "enemy";
		}
		if (p_icon == double(kUiHudIconFriendlyFlag)) {
			return "friendly";
		}
		if (p_icon == double(kUiHudIconNeutralFlag)) {
			return "neutral";
		}
		return {};
	}

	static constexpr float kMinimapSize = 170.0f;
	static constexpr size_t kMaxObjectives = 7;
	static constexpr float kObjectiveIconWidth = 22.0f;
	static constexpr double kBarWidth = 154.0;
	static constexpr std::array<float, 7> kObjectivesOffset{ 6.0f, 4.0f, 3.0f, 2.0f, 1.0f, 0.0f, 0.0f };
	std::string mode_;
};

}

As2Value hud_objective_status(const HudCapturePointList &p_points) {
	As2Value out = As2Value::array();
	char letter = 'A';
	for (size_t i = 0; i < p_points.count && i < p_points.points.size(); ++i) {
		const HudCapturePoint &p = p_points.points[i];
		if (p.hq) {
			continue;
		}
		As2Value e = As2Value::object();
		e.set("Index", As2Value::of(int32_t(i)));
		e.set("Team", As2Value::of(int32_t(p.owner)));
		e.set("Icon",
				As2Value::of(p.owner == 0 ? kUiHudIconNeutralFlag
										  : (p.owner == 1 ? kUiHudIconFriendlyFlag : kUiHudIconEnemyFlag)));
		e.set("Label", As2Value::of(std::string(1, letter++)));
		e.set("IsAttacked", As2Value::of(p.contested));
		out.push(std::move(e));
	}
	return out;
}

std::vector<HudFlagIcon> hud_minimap_flags(const HudCapturePointList &p_points) {
	std::vector<HudFlagIcon> out;
	char letter = 'A';
	for (size_t i = 0; i < p_points.count && i < p_points.points.size(); ++i) {
		const HudCapturePoint &p = p_points.points[i];
		HudFlagIcon f;
		f.icon = objective_icon(p.hq, p.owner, 1);
		f.x = p.x;
		f.z = p.z;
		f.letter = p.hq ? char(0) : letter++;
		out.push_back(f);
	}
	return out;
}

void register_hud_widgets_objective_bar(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Hud.ObjectiveBar", [] { return std::make_unique<ObjectiveBarClass>(); });
}

}
