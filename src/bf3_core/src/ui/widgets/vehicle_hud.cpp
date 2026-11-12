#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

bool strict_number(const As2Value &p_value, double p_number) {
	return p_value.kind == As2Value::Kind::Number && p_value.number == p_number;
}

class VehicleHealthClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		show_health_ = true;
		r_host.renderer(r_host.self(), "t2");
		r_host.tilt(r_host.self(), -15.0f, 15.0f, kHudPerspectiveFov);
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		const std::string *prop = p_init.property("HealthData");
		As2Value health = prop != nullptr && !prop->empty() ? As2Value::of(*prop) : As2Value::undefined();
		if (!health.defined()) {
			if (const As2Value *v = p_init.value("Health")) {
				health = *v;
			}
		}
		const As2Clip holder = r_host.clip("container.vhHolder");
		holder.goto_and_stop("us");
		if (HudBackgroundWidgetApi *bg = controller_at<HudBackgroundWidgetApi>(r_host, holder.child("bg"))) {
			bg->initialize_without_data();
		}
		set_value(r_host, health);
		const As2Value *visible = p_init.value("Visible");
		hardcore_ = visible != nullptr && visible->truthy();
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "IsInVehicle") {
			const bool in_vehicle = p_value.defined() && !hardcore_ && p_value.truthy();
			r_host.self().set_visible(in_vehicle);
			if (in_vehicle) {
				show_health_ = true;
				update_visibility(r_host);
			}
		} else if (p_key == "Health") {
			set_value(r_host, p_value);
		} else if (p_key == "Disabled") {
			const bool disabled = p_value.defined() && p_value.truthy();
			const As2Clip meter = r_host.clip("container.vhHolder.meter");
			for (const char *part : { "meter", "leftEdge", "rightEdge" }) {
				if (disabled) {
					meter.child(part).goto_and_play("loop");
				} else {
					meter.child(part).goto_and_stop(1);
				}
			}
		}
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		r_out.push_back({ "IsInVehicle", As2Value::of(p_model.vehicle.in_vehicle) });
		r_out.push_back({ "Health", As2Value::of(double(p_model.vehicle.health)) });
		r_out.push_back({ "Disabled", As2Value::of(p_model.vehicle.damage_state_disabled) });
		r_out.push_back({ "Visible", As2Value::of(p_client.hardcore) });
	}

private:
	static void set_value(HudWidgetHost &r_host, const As2Value &p_value) {
		const double health = as2_round(p_value.to_number());
		const As2Clip holder = r_host.clip("container.vhHolder");
		holder.child("healthtxt").child("txtDisplay").set_text(as2_number_string(health));
		const As2Clip meter = holder.child("meter");
		const As2Clip bar = meter.child("meter");
		bar.set_xscale(float(health));
		meter.child("rightEdge").set_x(bar.x() + bar.width());
		if (health == 0.0) {
			meter.child("leftEdge").set_visible(false);
			meter.child("rightEdge").set_visible(false);
		}
	}

	void update_visibility(HudWidgetHost &r_host) const {
		r_host.clip("container").goto_and_play(show_health_ ? "show" : "hide");
	}

	bool show_health_ = true;
	bool hardcore_ = false;
};

class PassangerListClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override {
		r_host.renderer(r_host.self(), "t2");
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "IsInVehicle") {
			r_host.self().set_visible(p_value.truthy());
		} else if (p_key == "PassangerList") {
			update_list(r_host, p_value);
		}
	}

	void bind(const HudModel &p_model, const HudClientState &, HudData &r_out) const override {
		r_out.push_back({ "IsInVehicle", As2Value::of(p_model.vehicle.in_vehicle) });
		r_out.push_back({ "PassangerList", hud_passenger_list(p_model.vehicle.passenger_list) });
	}

private:
	static void update_list(HudWidgetHost &r_host, const As2Value &p_list) {
		for (size_t i = 0; i < kHudMaxPassengers; ++i) {
			const As2Value &entry = p_list.at(i);
			const As2Clip row = r_host.self().child("passanger_" + std::to_string(i));
			if (!entry.defined()) {
				row.set_visible(false);
				continue;
			}
			row.set_visible(true);
			const As2Value state = entry.field("teamState");
			const As2Clip label = row.child("label");
			if (strict_number(state, 0.0)) {
				label.goto_and_stop(entry.field("isPlayer").truthy() ? "bold" : "standard");
			} else if (strict_number(state, 1.0)) {
				label.goto_and_stop("friend");
			}
			const std::string name = entry.field("Name").to_string();
			label.child("txt").set_text(name);
			label.child("txtBg").set_text(name);
			row.child("seatIcon").goto_and_stop(icon_string(state, entry.field("seat")));
		}
	}

	static std::string icon_string(const As2Value &p_state, const As2Value &p_seat) {
		if (std::isnan(p_state.to_number()) || std::isnan(p_seat.to_number())) {
			return "neutralIdle";
		}
		std::string team = "neutral";
		if (strict_number(p_state, 0.0)) {
			team = "squad";
		} else if (strict_number(p_state, 1.0)) {
			team = "friend";
		}
		std::string seat = "Idle";
		if (strict_number(p_seat, 0.0)) {
			seat = "Driver";
		} else if (strict_number(p_seat, 1.0)) {
			seat = "Turret";
		}
		return team + seat;
	}
};

}

As2Value hud_passenger_list(const HudPassengerList &p_list) {
	As2Value list = As2Value::array();
	for (size_t i = 0; i < p_list.count && i < p_list.passengers.size(); ++i) {
		const HudPassenger &p = p_list.passengers[i];
		As2Value entry = As2Value::object();
		entry.set("Name", As2Value::of(hud_string(p.name)));
		entry.set("teamState", As2Value::of(double(p.team_state)));
		entry.set("isPlayer", As2Value::of(p.is_player));
		entry.set("seat", As2Value::of(double(int32_t(p.seat))));
		list.push(std::move(entry));
	}
	return list;
}

void register_hud_widgets_vehicle_hud(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Hud.VehicleHealth", [] { return std::make_unique<VehicleHealthClass>(); });
	r_registry.add("Widget.Hud.PassangerList", [] { return std::make_unique<PassangerListClass>(); });
}

}
