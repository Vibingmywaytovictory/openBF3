#include "bf3/host/vehicle_hud_flow.h"

#include "bf3/host/ui_flow_session.h"

#include <algorithm>

namespace bf3::host {

namespace gp = core::gameplay;

std::vector<const gp::VehicleCameraDef *> vehicle_seat_cameras(const gp::VehicleSeatDef &p_seat) {
	std::vector<const gp::VehicleCameraDef *> out;
	for (const bool first : { true, false }) {
		for (const gp::VehicleCameraDef &c : p_seat.cameras) {
			if (c.first_person == first) {
				out.push_back(&c);
			}
		}
	}
	return out;
}

const gp::VehicleCameraDef *vehicle_seat_camera(const gp::VehicleSeatDef &p_seat, uint32_t p_counter) {
	const std::vector<const gp::VehicleCameraDef *> cameras = vehicle_seat_cameras(p_seat);
	return cameras.empty() ? nullptr : cameras[p_counter % uint32_t(cameras.size())];
}

namespace {

bool switch_on(const gp::VehicleHudSwitch &p_switch, const VehicleHudView &p_view) {
	if (p_view.camera < 0) {
		return false;
	}
	return std::any_of(p_switch.terms.begin(), p_switch.terms.end(), [&](const gp::VehicleHudTerm &t) {
		return t.camera == uint32_t(p_view.camera) && (!t.alternate_view || p_view.alternate_view);
	});
}

void append(std::vector<gp::VehicleHudGraphEvent> &r_out, const std::vector<gp::VehicleHudGraphEvent> &p_events) {
	r_out.insert(r_out.end(), p_events.begin(), p_events.end());
}

}

void VehicleHudSwitches::settle(const VehicleHudView &p_view, std::vector<gp::VehicleHudGraphEvent> &r_out) {
	if (hud_ == nullptr) {
		return;
	}
	for (size_t i = 0; i < hud_->switches.size(); ++i) {
		const gp::VehicleHudSwitch &s = hud_->switches[i];
		const bool on = switch_on(s, p_view);
		if (on != values_[i]) {
			values_[i] = on;
			append(r_out, on ? s.on_true : s.on_false);
		}
	}
}

std::vector<gp::VehicleHudGraphEvent> VehicleHudSwitches::step(const VehicleHudView &p_view) {
	std::vector<gp::VehicleHudGraphEvent> out;
	const std::optional<uint64_t> vehicle =
			p_view.hud != nullptr ? std::optional<uint64_t>(p_view.vehicle) : std::nullopt;
	if (p_view.hud != hud_ || vehicle != vehicle_) {
		settle(VehicleHudView{}, out);
		hud_ = p_view.hud;
		vehicle_ = vehicle;
		values_.clear();
		if (hud_ != nullptr) {
			for (const gp::VehicleHudSwitch &s : hud_->switches) {
				values_.push_back(s.initial);
				if (s.initial_event) {
					append(out, s.initial ? s.on_true : s.on_false);
				}
			}
		}
	}
	settle(p_view, out);
	return out;
}

size_t apply_vehicle_hud_events(UiFlowSession &r_session, const std::vector<gp::VehicleHudGraphEvent> &p_events) {
	size_t applied = 0;
	for (const gp::VehicleHudGraphEvent &e : p_events) {
		applied += r_session.graph_entity_event(e.graph, e.priority, e.pop_previous, e.event) ? 1 : 0;
	}
	return applied;
}

}
