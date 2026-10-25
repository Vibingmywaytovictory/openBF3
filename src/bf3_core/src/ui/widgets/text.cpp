#include "hud_widget_support.h"

#include <optional>

namespace bf3::core::ui {

namespace {

constexpr int32_t kTextFieldShow = 0;
constexpr int32_t kTextFieldHide = 1;
constexpr double kChatLogHideSeconds = 4.0;
constexpr std::string_view kChatLogMode = "ChatLogMode";

class TextFieldClass final : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override {
		const As2Clip self = r_host.self();
		start_w_ = self.width();
		start_h_ = self.height();
		row_type_ = param_string(r_host, "m_rowType");
		std::string linkage = "mc_" + row_type_;
		if (row_type_.find("Vehicle") != std::string::npos) {
			linkage += hud_vehicle_russian(r_host.client()) ? "_ru" : "_us";
		}
		text_field_ = r_host.attach_movie(self, linkage, row_type_, self.next_highest_depth()).path();
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		const As2Clip self = r_host.self();
		const As2Clip field = r_host.screen().clip(text_field_);
		field.set_x(0.0f);
		field.set_y(0.0f);
		const float w = self.width();
		const float h = self.height();
		self.set_yscale(100.0f);
		self.set_xscale(100.0f);
		const As2Clip display = field.child("txtDisplay");
		display.set_width(w);
		display.set_height(h);
		display.set_text(param_true(r_host, "showInitializedText") ? "NO DATA" : "");
		if (param_true(r_host, "m_useBorder")) {
			const As2Clip border = r_host.attach_movie(self, "BorderMc", "border", self.next_highest_depth());
			border.set_width(start_w_);
			border.set_height(start_h_);
			if (const std::optional<uint32_t> c = display.text_color()) {
				border.set_rgb(*c);
			}
		}
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Text") {
			const As2Clip display = r_host.screen().clip(text_field_).child("txtDisplay");
			if (!p_value.defined()) {
				display.set_text("");
				return;
			}
			display.set_text(p_value.to_string());
			if (row_type_ == kChatLogMode) {
				r_host.self().set_visible(true);
				hide_in_ = kChatLogHideSeconds;
			}
		} else if (p_key == "Visible" && p_value.defined()) {
			r_host.self().set_visible(p_value.truthy());
		}
	}

	void advance(HudWidgetHost &r_host, double p_seconds) override {
		if (!hide_in_) {
			return;
		}
		*hide_in_ -= p_seconds;
		if (*hide_in_ <= 0.0) {
			hide_in_.reset();
			r_host.self().set_visible(false);
		}
	}

	void in_event(HudWidgetHost &r_host, int32_t p_index, const As2Value &) override {
		if (p_index == kTextFieldShow) {
			r_host.self().set_visible(true);
		} else if (p_index == kTextFieldHide) {
			r_host.self().set_visible(false);
		}
	}

	const std::string &row_type() const { return row_type_; }

private:
	std::string row_type_;
	std::string text_field_;
	std::optional<double> hide_in_;
	float start_w_ = 0.0f;
	float start_h_ = 0.0f;
};

class PageHeaderClass final : public HudWidgetController {
public:
	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		if (const std::string *c = p_init.property("HideOnConsole")) {
			hide_on_console_ = *c == "true";
		}
		if (const std::string *pc = p_init.property("HideOnPC")) {
			hide_on_pc_ = *pc == "true";
		}
		r_host.clip("heading").set_auto_size(true);
		r_host.clip("subHeading").set_auto_size(true);
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(hud_platform_win32(r_host.client()) ? !hide_on_pc_ : !hide_on_console_);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Header") {
			r_host.clip("heading").set_text(p_value.defined() ? p_value.to_string() : std::string());
		} else if (p_key == "SubHeader") {
			r_host.clip("subHeading").set_text(p_value.defined() ? p_value.to_string() : std::string());
		}
	}

private:
	bool hide_on_console_ = false;
	bool hide_on_pc_ = false;
};

}

void register_hud_widgets_text(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Text.TextField", [] { return std::make_unique<TextFieldClass>(); });
	r_registry.add("Widget.Text.PageHeader", [] { return std::make_unique<PageHeaderClass>(); });
}

}
