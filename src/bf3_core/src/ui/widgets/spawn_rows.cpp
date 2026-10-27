#include "list.h"

#include "bf3/core/util/key_values.h"

namespace bf3::core::ui {

namespace {

void goto_icon_frame(const As2Clip &p_clip, const As2Value &p_frame) {
	if (p_frame.kind == As2Value::Kind::String && !std::isfinite(As2Value::of(p_frame.text).to_number())) {
		p_clip.goto_and_stop(std::string_view(p_frame.text));
	} else if (p_frame.defined()) {
		goto_and_stop_number(p_clip, p_frame.to_number());
	}
}

As2Clip attach_label(HudWidgetHost &r_host, const As2Clip &p_dummy, std::string_view p_name) {
	const As2Clip self = r_host.self();
	p_dummy.set_visible(false);
	const As2Clip label = r_host.attach_movie(self, "mc_label", p_name, self.next_highest_depth());
	label.set_x(p_dummy.x());
	label.set_y(p_dummy.y());
	return label;
}

class TeamSpawnClass : public RowBaseClass {
public:
	void construct(HudWidgetHost &r_host) override {
		RowBaseClass::construct(r_host);
		swap_text_fields(r_host);
	}

	void set_data(const As2Value &p_data) override {
		set_label(p_data.field("Label").to_string());
		set_vehicle_icon(p_data.field("VehicleIcon"));
		set_mouse_enabled(true);
		RowBaseClass::set_data(p_data);
	}

	void set_width(double p_width) override {
		if (HudListLabel *l = label_field()) {
			l->set_label_width(float(p_width) - host_->clip(std::string(kLabel)).x());
		}
		RowBaseClass::set_width(p_width);
	}

	As2Value label() const override {
		const HudListLabel *l = label_field();
		return l != nullptr ? As2Value::of(l->text()) : As2Value::undefined();
	}

protected:
	static constexpr std::string_view kLabel = "Label";

	virtual void swap_text_fields(HudWidgetHost &r_host) {
		const As2Clip dummy = r_host.clip("dummyTextField");
		if (!dummy.exists()) {
			return;
		}
		attach_label(r_host, dummy, kLabel);
		if (HudListLabel *l = label_field()) {
			l->set_interactive(true);
			l->set_type("bold");
		}
		r_host.clip("vehicleIcon").set_visible(false);
	}

	virtual int32_t icon_frame() const { return 1; }

	void set_label(std::string p_text) {
		if (HudListLabel *l = label_field()) {
			l->set_text(std::move(p_text));
		}
	}

	void set_vehicle_icon(const As2Value &p_icon) const {
		const As2Clip icon = host_->clip("vehicleIcon");
		icon.set_visible(true);
		goto_icon_frame(icon, p_icon);
		if (const As2Clip inner = icon.child("icon"); inner.exists()) {
			inner.goto_and_stop(icon_frame());
		}
	}

	HudListLabel *label_field() const {
		return host_ != nullptr ? controller_at<HudListLabel>(*host_, host_->clip(std::string(kLabel))) : nullptr;
	}
};

class BaseSpawnClass final : public TeamSpawnClass {
public:
	void set_data(const As2Value &p_data) override {
		const As2Value &text = p_data.field("Text");
		set_label(text.defined() ? text.to_string() : p_data.field("Label").to_string());
		setup_icon(p_data.field("Label"));
		set_vehicle_icon(p_data.field("VehicleIcon"));
		set_mouse_enabled(true);
		RowBaseClass::set_data(p_data);
	}

private:
	static constexpr std::string_view kIconLabel = "IconLabel";
	static constexpr float kIconLabelInset = 8.0f;
	static constexpr uint32_t kIconGlowColor = 0xFFFFFFu;
	static constexpr float kIconGlowBlur = 8.0f;
	static constexpr float kIconGlowStrength = 0.3f;

	void swap_text_fields(HudWidgetHost &r_host) override {
		TeamSpawnClass::swap_text_fields(r_host);
		if (!r_host.clip("dummyTextField").exists()) {
			return;
		}
		const As2Clip dummy = r_host.clip("DummyIconLabel");
		if (!dummy.exists()) {
			return;
		}
		const As2Clip icon_label = attach_label(r_host, dummy, kIconLabel);
		if (HudListLabel *l = controller_at<HudListLabel>(r_host, icon_label)) {
			l->set_label_width(dummy.width() - kIconLabelInset);
			l->set_interactive(false);
			l->set_type("bold");
		}
		r_host.clip("vehicleIcon").set_visible(false);
	}

	int32_t icon_frame() const override { return 4; }

	void setup_icon(const As2Value &p_icon) const {
		const As2Clip icon_label = host_->clip(std::string(kIconLabel));
		HudListLabel *l = controller_at<HudListLabel>(*host_, icon_label);
		if (l == nullptr) {
			return;
		}
		const std::string text = p_icon.defined() ? p_icon.to_string() : std::string();
		l->set_text(text.empty() || text.front() == '_' ? std::string() : text);
		icon_label.child("txtField")
				.set_filters(
						{ as2_glow_filter(kIconGlowColor, 1.0, kIconGlowBlur, kIconGlowBlur, kIconGlowStrength, 1) });
	}
};

class SpawnEmptyClass final : public RowBaseClass {
public:
	void construct(HudWidgetHost &r_host) override {
		RowBaseClass::construct(r_host);
		set_mouse_enabled(false);
	}
};

}

std::optional<As2Value> hud_model_list_rows(
		const HudDataSource &p_query, const HudModel &p_model, const HudClientState &p_client) {
	if (p_query.category != kSpawnPointListCategory || p_query.key != kSpawnPointListKey) {
		return std::nullopt;
	}
	As2Value rows = As2Value::array();
	const HudSpawnPointList &list = p_model.spawn_points.spawn_points;
	for (size_t i = 0; i < std::min<size_t>(list.count, list.points.size()); ++i) {
		const HudSpawnPoint &p = list.points[i];
		if (p.kind != HudSpawnKind::Hq && p.kind != HudSpawnKind::Flag) {
			continue;
		}
		const std::string label = p_client.localise_hash ? p_client.localise_hash(p.label_sid_hash) : std::string();
		As2Value row = As2Value::object();
		row.set("Type", As2Value::of(std::string(kSpawnRowType)));
		if (p.kind == HudSpawnKind::Hq) {
			row.set("Text", As2Value::of(label));
			row.set("Label", As2Value::of(std::string()));
		} else {
			row.set("Label", As2Value::of(label));
		}
		row.set("VehicleIcon", As2Value::of(std::string(kSpawnRowNoVehicleIcon)));
		row.set("Index", As2Value::of(int32_t(i)));
		row.set("id", As2Value::of(int32_t(i)));
		if (!p.available) {
			row.set("Disabled", As2Value::of(true));
		}
		rows.push(std::move(row));
	}
	return rows;
}

std::optional<int32_t> hud_model_list_selection(const HudDataSource &p_query, const HudModel &p_model) {
	if (p_query.category != kSpawnPointListCategory || p_query.key != kDefaultSpawnPointKey) {
		return std::nullopt;
	}
	const HudSpawnPointList &list = p_model.spawn_points.spawn_points;
	const int32_t selected = p_model.spawn_points.selected_point;
	std::optional<int32_t> first;
	int32_t row = 0;
	for (size_t i = 0; i < std::min<size_t>(list.count, list.points.size()); ++i) {
		const HudSpawnPoint &p = list.points[i];
		if (p.kind != HudSpawnKind::Hq && p.kind != HudSpawnKind::Flag) {
			continue;
		}
		if (p.available && int32_t(i) == selected) {
			return row;
		}
		if (p.available && !first) {
			first = row;
		}
		++row;
	}
	return first;
}

std::optional<int32_t> hud_selected_spawn_point(std::span<const std::string> p_params) {
	int32_t point = -1;
	if (p_params.empty() ||
			parse_i32_value(p_params.front(), -1.0, double(kHudMaxSpawnPoints) - 1.0, point).fault != KeyFault::None) {
		return std::nullopt;
	}
	return point;
}

void register_hud_widgets_spawn_rows(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.List.Rows.TeamSpawn", [] { return std::make_unique<TeamSpawnClass>(); });
	r_registry.add("Widget.List.Rows.BaseSpawn", [] { return std::make_unique<BaseSpawnClass>(); });
	r_registry.add("Widget.List.Rows.SpawnEmpty", [] { return std::make_unique<SpawnEmptyClass>(); });
}

}
