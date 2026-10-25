#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

class HudAlertMessageClass final : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		if (const As2Value *t = r_host.construct_param("m_messageType")) {
			message_type_ = t->to_string();
		}
		always_enabled_ = param_true(r_host, "m_alwaysEnabled");
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		for (const char *c : { "alert", "warning", "information" }) {
			r_host.clip(c).set_visible(false);
		}
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		for (const HudWidgetProperty &prop : p_init.properties) {
			if (prop.name == "m_staticDescription" && !prop.value.empty()) {
				static_description_ = prop.value;
			} else if (prop.name == "m_staticTitle" && !prop.value.empty()) {
				static_title_ = prop.value;
			}
		}
		setup_refresh_data(*this, r_host, p_init.data);
		if (message_type_ != "none" && always_enabled_) {
			visible_ = true;
			setup_message();
		}
	}

	void update(HudWidgetHost &, std::string_view p_key, const As2Value &p_value) override {
		for (const auto &[key, type] : kVisibilityKeys) {
			if (p_key == key) {
				show(p_value, type);
				return;
			}
		}
		for (const auto &[key, title, timed] : kInformationKeys) {
			if (p_key == key) {
				show_information(p_value, title, timed);
				return;
			}
		}
		if (p_key == "Description") {
			description_text_ = as2_text(p_value);
			setup_message();
		} else if (p_key == "Title") {
			title_text_ = as2_text(p_value);
			setup_message();
		} else if (p_key == "Timer") {
			const std::string v = p_value.to_string();
			host_->self().set_visible(!(message_type_ == "waiting" && (v == "0" || v == "00:00")));
			if (message_.exists()) {
				message_.child("content").child("timer").set_text(p_value.defined() ? v : std::string());
			}
		}
	}

	void frame_script(HudWidgetHost &r_host, const HudFrameScript &p_fs) override {
		if (!message_.exists() || p_fs.path != message_.path()) {
			return;
		}
		if (p_fs.calls("onShowComplete")) {
			message_.goto_and_play(slow_loop_ ? "slowloop" : "loop");
		}
		if (p_fs.calls("onHideComplete")) {
			r_host.self().set_visible(false);
			message_.set_visible(false);
		}
	}

	void advance(HudWidgetHost &, double p_seconds) override {
		if (timer_left_ <= 0.0) {
			return;
		}
		timer_left_ -= p_seconds;
		if (timer_left_ <= 0.0) {
			timer_left_ = 0.0;
			visible_ = false;
			setup_message();
		}
	}

private:
	static constexpr float kMinimumWidth = 350.0f;
	static constexpr float kYStartTwoRows = 10.0f;
	static constexpr float kInformationTitleY = 4.0f;
	static constexpr double kHideAlertSeconds = 5.0;
	static constexpr int32_t kBackgroundShownFrame = 21;
	static constexpr std::array<std::pair<std::string_view, std::string_view>, 6> kVisibilityKeys{ {
			{ "WarningVisibility", "warning" },
			{ "RevivedVisibility", "revive" },
			{ "WaitingVisibility", "waiting" },
			{ "DeathVisibility", "death" },
			{ "FailedVisibility", "failed" },
			{ "InformationVisibility", "information" },
	} };
	static constexpr std::array<std::tuple<std::string_view, std::string_view, bool>, 4> kInformationKeys{ {
			{ "SquadLeaderVisibility", "ID_H_ALERT_SQUAD_LEADER_PROMOTION", true },
			{ "FlagTransporterVisibility", "ID_H_ALERT_YOU_TRANSPORT_FLAG_CARRIER", false },
			{ "FlagHolderVisibility", "ID_H_ALERT_YOU_HAVE_THE_FLAG", false },
			{ "LastManStandingVisibility", "ID_H_ALERT_LAST_MAN_STANDING", true },
	} };

	void show(const As2Value &p_value, std::string_view p_type) {
		if (!p_value.defined()) {
			return;
		}
		visible_ = p_value.truthy();
		if (visible_) {
			message_type_ = std::string(p_type);
		}
		setup_message();
	}

	void show_information(const As2Value &p_value, std::string_view p_title, bool p_timed) {
		if (!p_value.defined()) {
			return;
		}
		visible_ = p_value.truthy();
		if (visible_) {
			message_type_ = "information";
		}
		title_text_ = std::string(p_title);
		description_text_.clear();
		setup_message();
		if (message_.exists()) {
			message_.child("content").child("title").set_y(kInformationTitleY);
		}
		timer_left_ = p_timed ? kHideAlertSeconds : 0.0;
	}

	void set_text(const As2Clip &p_field, std::string_view p_text) const { p_field.set_text(host_->translate(p_text)); }

	void setup_message() {
		timer_left_ = 0.0;
		slow_loop_ = false;
		std::string_view title;
		std::string_view description;
		std::string_view colour;
		if (message_type_ == "warning") {
			message_ = host_->clip("warning");
			title = "ID_H_ALERT_COMBAT_AREA_WARNING";
			description = "ID_H_ALERT_COMBAT_AREA_WARNING_DESCRIPTION";
			colour = "off";
			bracket_colour_ = "red";
		} else if (message_type_ == "revive") {
			message_ = host_->clip("warning");
			title = "ID_H_ALERT_COOP_REVIVE";
			description = "ID_H_ALERT_COOP_REVIVE_DESCRIPTION";
			colour = "us";
			bracket_colour_ = "blue";
		} else if (message_type_ == "waiting") {
			message_ = host_->clip("alert");
			title = "GET READY";
			description = "ROUND STARTS IN";
			colour = "us";
			bracket_colour_ = "blue";
		} else if (message_type_ == "death" || message_type_ == "failed") {
			message_ = host_->clip("warning");
			title = "ID_H_ALERT_YOU_FAILED";
			description = message_type_ == "death" ? "ID_H_ALERT_YOU_DIED" : "";
			colour = "off";
			bracket_colour_ = "red";
			slow_loop_ = true;
			const As2Clip t = message_.child("content").child("title");
			t.set_y(kYStartTwoRows);
			message_.child("content").child("description").set_y(t.y() + t.height());
		} else if (message_type_ == "information") {
			message_ = host_->clip("information");
			colour = "none";
			bracket_colour_ = "blue";
		} else {
			return;
		}
		const As2Clip content = message_.child("content");
		const As2Clip title_field = content.child("title");
		const As2Clip description_field = content.child("description");
		set_text(title_field, title);
		set_text(description_field, description);
		const As2Clip bg = message_.child("bg");
		if (colour != "none") {
			bg.goto_and_stop(kBackgroundShownFrame);
			bg.set_visible(true);
		} else {
			bg.set_visible(false);
		}
		title_field.set_auto_size(SwfTextAutoSize::Center);
		description_field.set_auto_size(SwfTextAutoSize::Center);
		if (!description_text_.empty()) {
			set_text(description_field, description_text_);
		} else if (!static_description_.empty()) {
			set_text(description_field, static_description_);
		}
		if (!title_text_.empty()) {
			set_text(title_field, title_text_);
		} else if (!static_title_.empty()) {
			set_text(title_field, static_title_);
		}
		update_size();
		set_bracket_colour();
		if (visible_) {
			host_->self().set_visible(true);
			message_.set_visible(true);
			message_.goto_and_play("in");
		} else {
			message_.goto_and_play("out");
		}
	}

	void update_size() {
		const As2Clip content = message_.child("content");
		const As2Clip title = content.child("title");
		const As2Clip description = content.child("description");
		const As2Clip icon_left = content.child("iconLeft");
		const As2Clip icon_right = content.child("iconRight");
		float left = 0.0f;
		if (icon_left.exists()) {
			icon_left.set_x(title.x() - 26.0f);
			icon_right.set_x(title.x() + title.width() + 26.0f);
			if (message_type_ != "information") {
				icon_left.set_y(title.y() + 28.0f);
				icon_right.set_y(title.y() + 28.0f);
			}
			left = icon_left.x() - icon_left.width() / 2.0f;
		} else {
			left = title.x();
		}
		left = std::min(left, description.x());
		if (std::fabs(left) < 150.0f) {
			left = -150.0f;
		}
		content.child("bracketLeft").set_x(left - 25.0f);
		content.child("bracketRight").set_x(-left + 25.0f);
		if (message_type_ == "information") {
			set_bracket_size(30.0f);
		}
		const As2Clip bg = message_.child("bg");
		bg.set_xscale(100.0f);
		const double w = double(content.child("bracketRight").x() - content.child("bracketLeft").x()) + 70.0;
		if (HudBackgroundWidgetApi *api = controller_at<HudBackgroundWidgetApi>(*host_, bg)) {
			api->set_width(w);
		}
		bg.set_x(float(-w / 2.0));
	}

	void set_bracket_colour() const {
		for (const char *side : { "bracketLeft", "bracketRight" }) {
			const As2Clip b = message_.child("content").child(side);
			for (const char *part : { "top", "middle", "bottom" }) {
				b.child(part).goto_and_stop(bracket_colour_);
			}
		}
	}

	void set_bracket_size(float p_height) const {
		const As2Clip l = message_.child("content").child("bracketLeft");
		const As2Clip r = message_.child("content").child("bracketRight");
		l.child("middle").set_height(p_height);
		r.child("middle").set_height(p_height);
		const float y = l.child("middle").y() + l.child("middle").height() + l.child("bottom").height();
		l.child("bottom").set_y(y);
		r.child("bottom").set_y(y);
	}

	HudWidgetHost *host_ = nullptr;
	As2Clip message_;
	std::string message_type_;
	std::string bracket_colour_;
	std::string static_description_;
	std::string static_title_;
	std::string title_text_;
	std::string description_text_;
	bool always_enabled_ = false;
	bool visible_ = false;
	bool slow_loop_ = false;
	double timer_left_ = 0.0;
};

}

void register_hud_widgets_hud_alert_message(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Message.HudAlertMessage", [] { return std::make_unique<HudAlertMessageClass>(); });
}

}
