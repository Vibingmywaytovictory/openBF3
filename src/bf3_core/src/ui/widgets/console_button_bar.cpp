#include "hud_widget_support.h"

#include <map>

namespace bf3::core::ui {

namespace {

constexpr int32_t kActivate = 0;
constexpr int32_t kDeactivate = 1;
constexpr int32_t kEdit = 2;
constexpr int32_t kView = 3;
constexpr int32_t kLThumb = 4;
constexpr int32_t kRThumb = 5;
constexpr int32_t kLTab = 6;
constexpr int32_t kRTab = 7;
constexpr int32_t kUpdateData = 8;
constexpr int32_t kShow = 9;
constexpr int32_t kHide = 10;
constexpr int32_t kMenu = 11;
constexpr float kIconButtonSpace = 5.0f;
constexpr float kButtonSpace = 15.0f;
constexpr float kPremiumIconX = 45.0f;
constexpr float kPremiumIconY = 4.0f;
constexpr float kPremiumShift = 19.0f;
constexpr float kRowOverlap = 10.0f;
constexpr float kDisabledAlpha = 25.0f;

struct ConsoleButtonLayout {
	std::string label;
	std::string id;
	std::string disabled;
	double row = 0.0;
	bool premium = false;
};

class ConsoleButtonBarClass final : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override { r_host.clip("layoutGuide").set_visible(false); }

	void on_clip_load(HudWidgetHost &r_host) override { r_host.self().set_visible(false); }

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		platform_ = r_host.client().platform;
		if (hud_platform_win32(r_host.client())) {
			return;
		}
		setup_refresh_data(*this, r_host, p_init.data);
		const As2Value *visible = nullptr;
		for (const HudDataEntry &e : p_init.data) {
			if (e.key == "DefaultButtonSet") {
				default_layout_ = layouts_of(e.value);
				update_buttons(r_host, default_layout_);
			} else if (e.key == "Visible") {
				visible = &e.value;
			}
		}
		r_host.self().set_visible(visible == nullptr || !visible->defined() || visible->truthy());
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Buttons") {
			if (p_value.kind == As2Value::Kind::String) {
				update_buttons(r_host, parse(p_value.to_string()));
			} else if (p_value.kind == As2Value::Kind::List) {
				update_buttons(r_host, layouts_of(p_value));
			}
		} else if (p_key == "InputOnRelease") {
			input_on_release_ = p_value.truthy();
		}
	}

	void in_event(HudWidgetHost &r_host, int32_t p_index, const As2Value &p_value) override {
		if (p_index == kUpdateData) {
			update_buttons(r_host, parse(p_value.to_string()));
		} else if (p_index == kShow) {
			r_host.self().set_visible(true);
		} else if (p_index == kHide) {
			r_host.self().set_visible(false);
		}
	}

	bool input_concept(HudWidgetHost &r_host, int32_t p_concept, bool p_pressed) override {
		if (p_pressed == input_on_release_) {
			return false;
		}
		switch (UiInputConcept(p_concept)) {
			case UiInputConcept::Activate:
				fire_button_event(r_host, kActivate, "IDB_Rdown");
				break;
			case UiInputConcept::Deactivate:
				fire_button_event(r_host, kDeactivate, "IDB_Rright");
				break;
			case UiInputConcept::Edit:
				fire_button_event(r_host, kEdit, "IDB_Rup");
				break;
			case UiInputConcept::View:
				fire_button_event(r_host, kView, "IDB_Rleft");
				break;
			case UiInputConcept::LThumb:
				fire_button_event(r_host, kLThumb, "IDB_Lthumb");
				break;
			case UiInputConcept::RThumb:
				fire_button_event(r_host, kRThumb, "IDB_Rthumb");
				break;
			case UiInputConcept::TabLeft:
				fire_button_event(r_host, kLTab, "IDB_Rtrigger2");
				break;
			case UiInputConcept::TabRight:
				fire_button_event(r_host, kRTab, "IDB_Ltrigger2");
				break;
			case UiInputConcept::Menu:
				fire_button_event(r_host, kMenu, "IDB_start");
				break;
			default:
				break;
		}
		return false;
	}

	const std::vector<std::string> &buttons() const { return buttons_; }

private:
	static std::vector<ConsoleButtonLayout> layouts_of(const As2Value &p_value) {
		std::vector<ConsoleButtonLayout> out;
		for (size_t i = 0; i < p_value.size(); ++i) {
			const As2Value &b = p_value.at(i);
			ConsoleButtonLayout l;
			l.label = b.field("ButtonLabel").to_string();
			l.id = b.field("ButtonId").to_string();
			l.disabled = b.field("Disabled").to_string();
			const As2Value &row = b.field("Row");
			l.row = row.defined() ? row.to_number() : 0.0;
			l.premium = b.field("IsPremium").truthy();
			out.push_back(std::move(l));
		}
		return out;
	}

	std::vector<ConsoleButtonLayout> parse(const std::string &p_text) const {
		if (p_text.empty()) {
			return default_layout_;
		}
		std::vector<ConsoleButtonLayout> out;
		for (const std::string &item : as2_split(p_text, ";")) {
			const std::vector<std::string> parts = as2_split(item, ",");
			ConsoleButtonLayout l;
			l.label = parts[0];
			l.id = parts.size() > 1 ? parts[1] : std::string("undefined");
			l.disabled = parts.size() > 2 ? parts[2] : std::string("undefined");
			l.row = parts.size() > 3 ? As2Value::of(parts[3]).to_number() : 0.0;
			if (!std::isfinite(l.row)) {
				l.row = 0.0;
			}
			out.push_back(std::move(l));
		}
		return out;
	}

	void clear(HudWidgetHost &r_host) {
		for (const std::string &path : buttons_) {
			r_host.remove_movie_clip(r_host.screen().clip(path));
		}
		buttons_.clear();
	}

	void update_buttons(HudWidgetHost &r_host, const std::vector<ConsoleButtonLayout> &p_layouts) {
		clear(r_host);
		if (p_layouts.empty()) {
			return;
		}
		actions_.clear();
		std::map<double, float> row_x;
		const As2Clip self = r_host.self();
		for (const ConsoleButtonLayout &l : p_layouts) {
			const As2Clip mc = r_host.attach_movie(self, "mc_button", "button_" + l.id, self.next_highest_depth());
			buttons_.push_back(mc.path());
			const As2Clip icon = mc.child("btnIcon");
			const As2Clip label = mc.child("btnLabel");
			const As2Clip label_bg = mc.child("btnLabelBg");
			icon.goto_and_stop(std::string_view(platform_));
			icon.child("icon").goto_and_stop(std::string_view(l.id));
			icon.set_x(float(as2_round(icon.width() * 0.5)));
			label.set_x(float(as2_round(icon.width())));
			label.set_auto_size(true);
			label.set_text(r_host.translate(l.label));
			label_bg.set_x(label.x());
			label_bg.set_auto_size(true);
			if (l.premium) {
				const As2Clip premium =
						r_host.attach_movie(mc, "PremiumItemIcon", "premiumIcon", mc.next_highest_depth());
				premium.set_x(kPremiumIconX);
				premium.set_y(kPremiumIconY);
				label.set_x(label.x() + kPremiumShift);
				label_bg.set_x(label_bg.x() + kPremiumShift);
			}
			label_bg.set_text(label.text());
			self.set_alpha(100.0f);
			float &x = row_x[l.row];
			mc.set_x(x);
			mc.set_y((mc.height() - kRowOverlap) * float(l.row));
			x += icon.width() + kIconButtonSpace + label.text_width() + kButtonSpace;
			if (l.disabled == "true") {
				mc.set_alpha(kDisabledAlpha);
				actions_.erase(l.id);
			} else {
				actions_[l.id] = l.label;
			}
		}
	}

	void fire_button_event(HudWidgetHost &r_host, int32_t p_event, const std::string &p_button) const {
		const auto it = actions_.find(p_button);
		if (it != actions_.end()) {
			r_host.fire_event(p_event, As2Value::of(it->second));
		}
	}

	std::string platform_;
	std::vector<ConsoleButtonLayout> default_layout_;
	std::vector<std::string> buttons_;
	std::map<std::string, std::string> actions_;
	bool input_on_release_ = false;
};

}

void register_hud_widgets_console_button_bar(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Button.ConsoleButtonBar", [] { return std::make_unique<ConsoleButtonBarClass>(); });
}

}
