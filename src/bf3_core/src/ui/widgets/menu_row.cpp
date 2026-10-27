#include "list.h"

#include "bf3/core/ui/ui_graph.h"

namespace bf3::core::ui {

namespace {

constexpr std::string_view kIngameMenuCategory = "uiingamemenucomp";
constexpr int32_t kMenuEntriesKey = 366660527;
constexpr std::string_view kPremiumSid = "ID_M_PREMIUM";
constexpr uint32_t kInactiveColor = 10066329u;
constexpr uint32_t kPremiumColor = 15302420u;
constexpr uint32_t kStandardColor = 16777215u;
constexpr uint32_t kStandardGlowColor = 7586815u;
constexpr float kPlusGap = 12.0f;

class MenuRowClass final : public RowBaseClass {
public:
	void construct(HudWidgetHost &r_host) override {
		RowBaseClass::construct(r_host);
		swap_text_fields(r_host);
	}

	void set_data(const As2Value &p_data) override {
		set_mouse_enabled(true);
		if (as2_loose_equals(p_data.field("useDirectAccess"), As2Value::of(true))) {
			set_label("- " + p_data.field("Index").to_string());
			setup_direct_access();
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
			l->set_interactive(true);
			l->set_type("bold");
			l->set_state_style("inactive", kInactiveColor, as2_glow_filter(kInactiveColor, 1.0, 8.0f, 8.0f, 0.75f, 1));
		}
	}

	void set_label(const std::string &p_sid) {
		label_text_ = p_sid;
		HudListLabel *l = label_controller();
		if (l == nullptr) {
			return;
		}
		l->set_text(host_->translate(p_sid));
		const bool premium = p_sid == kPremiumSid;
		if (premium == premium_) {
			return;
		}
		premium_ = premium;
		const uint32_t color = premium_ ? kPremiumColor : kStandardColor;
		const uint32_t glow = premium_ ? kPremiumColor : kStandardGlowColor;
		l->set_state_style("standard", color, as2_glow_filter(glow, 1.0, 8.0f, 8.0f, 1.45f, 1));
	}

	void setup_direct_access() {
		register_direct_access_array({ ListAccessItem{ "Label", "", UiDirectAccessOp::Text, label_clip().path() } });
	}

	std::string label_text_;
	bool premium_ = false;
};

}

std::optional<As2Value> hud_menu_list_rows(const HudDataSource &p_query, const HudModel &p_model) {
	if (ui_data_category_leaf(p_query.category) != kIngameMenuCategory || p_query.key != kMenuEntriesKey) {
		return std::nullopt;
	}
	As2Value rows = As2Value::array();
	const HudMenuList &list = p_model.ingame_menu.menu_entries;
	for (size_t i = 0; i < std::min<size_t>(list.count, list.entries.size()); ++i) {
		const HudMenuEntry &e = list.entries[i];
		const size_t action = size_t(e.action);
		As2Value row = As2Value::object();
		row.set("Label",
				As2Value::of(std::string(action < kHudMenuActionSids.size() ? kHudMenuActionSids[action] : "")));
		row.set("Disabled", As2Value::of(!e.enabled));
		rows.push(std::move(row));
	}
	return rows;
}

void register_hud_widgets_menu_row(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.List.Rows.MenuRow", [] { return std::make_unique<MenuRowClass>(); });
}

}
