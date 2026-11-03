#include "bf3/core/ui/debug_screen.h"
#include "bf3/core/util/format.h"
#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

constexpr std::string_view kTurnedOff = "DEBUGING IS TURNED OF";
constexpr std::string_view kLabelSource = "Label";
constexpr std::string_view kDataSource = "Data";
constexpr std::array<std::string_view, 4> kBorders{ "left", "top", "right", "bottom" };

class DebugFieldClass final : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override {
		start_w_ = float(as2_round(r_host.self().width()));
		start_h_ = float(as2_round(r_host.self().height()));
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		setup_size(r_host);
		set_color(r_host, kDebugFieldRgb);
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		const std::string *label = p_init.property(kLabelSource);
		update_label(r_host, label != nullptr ? std::string_view(*label) : std::string_view("undefined"));
		update_label(r_host, kTurnedOff);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == kLabelSource) {
			update_label(r_host, p_value.to_string());
		}
	}

private:
	static void update_label(HudWidgetHost &r_host, std::string_view p_label) {
		r_host.clip("txtLabel").set_text(std::string(p_label) + ":");
	}

	void setup_size(HudWidgetHost &r_host) const {
		const As2Clip self = r_host.self();
		self.set_x(float(as2_round(self.x())));
		self.set_y(float(as2_round(self.y())));
		self.set_yscale(100.0f);
		self.set_xscale(100.0f);
		const As2Clip label = r_host.clip("txtLabel");
		const As2Clip value = r_host.clip("txtValue");
		for (const As2Clip &field : { label, value }) {
			field.set_x(float(as2_round(field.x())));
			field.set_y(float(as2_round(field.y())));
			field.set_width(float(as2_round(start_w_ - 4.0f)));
		}
		value.set_height(float(as2_round(start_h_ - (label.height() + 4.0f))));
		setup_bg(r_host, start_w_, start_h_);
	}

	static void setup_bg(HudWidgetHost &r_host, float p_w, float p_h) {
		const As2Clip bg = r_host.clip("bg");
		const As2Clip left = bg.child("left");
		left.set_x(0.0f);
		left.set_y(0.0f);
		left.set_width(1.0f);
		left.set_height(p_h);
		const As2Clip top = bg.child("top");
		top.set_x(0.0f);
		top.set_y(0.0f);
		top.set_width(p_w);
		top.set_height(1.0f);
		const As2Clip right = bg.child("right");
		right.set_x(p_w - 1.0f);
		right.set_y(0.0f);
		right.set_width(1.0f);
		right.set_height(p_h);
		const As2Clip bottom = bg.child("bottom");
		bottom.set_x(0.0f);
		bottom.set_y(p_h - 1.0f);
		bottom.set_width(p_w);
		bottom.set_height(1.0f);
		const As2Clip black = bg.child("black");
		black.set_width(p_w);
		black.set_height(p_h);
	}

	static void set_color(HudWidgetHost &r_host, uint32_t p_rgb) {
		r_host.clip("txtLabel").set_text_color(p_rgb);
		r_host.clip("txtValue").set_text_color(p_rgb);
		const As2Clip bg = r_host.clip("bg");
		for (const std::string_view border : kBorders) {
			bg.child(border).set_rgb(p_rgb);
		}
	}

	float start_w_ = 0.0f;
	float start_h_ = 0.0f;
};

}

std::string debug_field_path(size_t p_index) {
	return core::fmt("%s.DebugField_%02u", std::string(kHudScreenInstance).c_str(), unsigned(p_index + 1));
}

HudScreenAsset debug_screen_asset(std::span<const DebugFieldText> p_fields) {
	HudScreenAsset asset = hud_movie_screen_asset(kDebugScreenMovie);
	for (size_t i = 0; i < p_fields.size() && i < kDebugScreenFields; ++i) {
		HudWidgetNode n;
		n.instance = std::string(clip_leaf(debug_field_path(i)));
		n.name = p_fields[i].label;
		n.widget_asset = std::string(kDebugFieldWidget);
		n.properties.push_back({ std::string(kLabelSource), p_fields[i].label });
		HudDataSource label;
		label.name = std::string(kLabelSource);
		label.on_initialize = false;
		n.sources.push_back(label);
		HudDataSource data;
		data.name = std::string(kDataSource);
		data.on_initialize = false;
		n.sources.push_back(data);
		asset.widgets.push_back(std::move(n));
	}
	return asset;
}

size_t debug_screen_show(HudScreen &r_screen, std::span<const DebugFieldText> p_fields) {
	size_t shown = 0;
	for (size_t i = 0; i < p_fields.size() && i < kDebugScreenFields; ++i) {
		const std::string path = debug_field_path(i);
		HudWidgetController *controller = r_screen.controller(path);
		HudWidgetHost *host = r_screen.host(path);
		const HudWidgetState *state = r_screen.widget(path);
		if (controller == nullptr || host == nullptr || state == nullptr || !state->initialized) {
			continue;
		}
		controller->update(*host, kLabelSource, As2Value::of(p_fields[i].label));
		controller->update(*host, kDataSource, As2Value::of(p_fields[i].value));
		host->clip("txtValue").set_text(p_fields[i].value);
		++shown;
	}
	return shown;
}

void register_hud_widgets_debug_field(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Text.DebugField", [] { return std::make_unique<DebugFieldClass>(); });
}

}
