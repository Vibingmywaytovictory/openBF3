#include "list.h"

namespace bf3::core::ui {

namespace {

constexpr float kPlusGap = 12.0f;

class BaseRowClass final : public RowBaseClass {
public:
	void construct(HudWidgetHost &r_host) override {
		RowBaseClass::construct(r_host);
		swap_text_fields(r_host);
	}

	void set_data(const As2Value &p_data) override {
		set_mouse_enabled(true);
		if (as2_loose_equals(p_data.field("useDirectAccess"), As2Value::of(true))) {
			set_label("- " + p_data.field("Index").to_string());
			register_direct_access_array(
					{ ListAccessItem{ "Label", "", UiDirectAccessOp::Text, label_clip().path() } });
		} else {
			set_label(p_data.field("Label").to_string());
		}
		RowBaseClass::set_data(p_data);
	}

	void set_width(double p_width) override {
		RowBaseClass::set_width(p_width);
		if (HudListLabel *l = label_controller()) {
			l->set_label_width(float(p_width) - label_clip().x());
		}
	}

	As2Value label() const override { return As2Value::of(label_text_); }

	void reset(bool p_destroy) override {
		RowBaseClass::reset(p_destroy);
		if (p_destroy) {
			host_->remove_movie_clip(label_clip());
		}
	}

protected:
	void check_for_new_content() const override {
		RowBaseClass::check_for_new_content();
		const As2Clip plus = host_->clip("plus");
		if (!plus.exists() || !plus.visible()) {
			return;
		}
		const HudListLabel *l = label_controller();
		const float text_width = l != nullptr ? l->text_width() : 0.0f;
		plus.set_x(std::min(float(width()) - plus.width(), label_clip().x() + text_width + kPlusGap));
	}

private:
	As2Clip label_clip() const { return host_->clip("Label"); }

	HudListLabel *label_controller() const {
		return dynamic_cast<HudListLabel *>(host_->screen().controller(label_clip().path()));
	}

	void swap_text_fields(HudWidgetHost &r_host) {
		const As2Clip dummy = r_host.clip("dummyTextField");
		if (!dummy.exists()) {
			return;
		}
		dummy.set_visible(false);
		const As2Clip self = r_host.self();
		const As2Clip label = r_host.attach_movie(self, "mc_label", "Label", self.next_highest_depth());
		label.set_x(dummy.x());
		label.set_y(dummy.y());
		if (HudListLabel *l = label_controller()) {
			if (const std::optional<float> size = dummy.text_size()) {
				l->set_text_size(*size);
			}
			l->set_interactive(true);
			l->set_type("bold");
		}
	}

	void set_label(const std::string &p_text) {
		label_text_ = p_text;
		if (HudListLabel *l = label_controller()) {
			l->set_text(host_->translate(p_text));
		}
	}

	std::string label_text_;
};

}

void register_hud_widgets_base_row(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.List.Rows.BaseRow", [] { return std::make_unique<BaseRowClass>(); });
}

}
