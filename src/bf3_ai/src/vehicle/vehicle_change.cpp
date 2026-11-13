#include "bf3/ai/vehicle/vehicle_change.h"

#include "bf3/ai/behaviour/curves.h"
#include "bf3/ai/squad/squad_world.h"

#include <algorithm>

namespace bf3::ai {

namespace {

bool upside_down(const ChangeVehicle &p_vehicle) {
	return kUpsideDownUp > p_vehicle.up;
}

bool manned_by_enemy(const ChangeVehicle &p_vehicle, int p_side) {
	if (p_vehicle.side == p_side) {
		return false;
	}
	for (uint8_t k = 0; k < p_vehicle.seats; ++k) {
		if (p_vehicle.seat[k].occupant >= 0) {
			return true;
		}
	}
	return false;
}

struct Pick {
	ChangeTarget target;
	float urgency = 0.0f;
};

void consider(Pick &r_best, ChangeTarget p_target, float p_urgency) {
	if (p_urgency > r_best.urgency) {
		r_best.target = p_target;
		r_best.urgency = p_urgency;
	}
}

}

float change_driver_modifier(const ChangeVehicle &p_vehicle, bool p_current, double p_now_s) {
	const double since = p_now_s - p_vehicle.seat[0].possessed_s;
	if (!p_current && p_vehicle.seat[0].occupant >= 0) {
		return 1.0f;
	}
	if (kPossessionWindowSeconds > since) {
		return 1.0f;
	}
	switch (p_vehicle.kind) {
		case ChangeUnitKind::Ground:
			return 0.77f;
		case ChangeUnitKind::Air:
			return 0.5f;
		case ChangeUnitKind::Soldier:
		case ChangeUnitKind::Naval:
			return 1.0f;
	}
	return 1.0f;
}

ChangeChoice choose_change(const ChangeBot &p_bot, std::span<const ChangeVehicle> p_vehicles) {
	ChangeChoice out;
	const ChangeTarget equipment{ p_bot.vehicle, p_bot.seat };
	const bool on_foot = equipment.soldier();
	const ChangeVehicle *const carrier = on_foot ? nullptr : &p_vehicles[size_t(p_bot.vehicle)];
	const ChangeTarget primary = on_foot ? equipment : ChangeTarget{ p_bot.vehicle, 0 };
	const Vec3 position = on_foot ? p_bot.position : carrier->position;

	float current = 0.0f;
	float candidate = 0.0f;
	if (equipment == primary) {
		candidate = 2.0f * change_unit_urgency(p_bot, p_vehicles, primary, false);
		current = std::max(candidate, 0.0f);
	} else {
		candidate = 2.0f * change_unit_urgency(p_bot, p_vehicles, primary, true);
	}
	const bool flipped = !on_foot && upside_down(*carrier);
	if (flipped) {
		candidate = 0.0f;
	}
	Pick best{ primary, candidate };
	if (!flipped && carrier != nullptr) {
		const float driver = change_driver_modifier(*carrier, primary == equipment, p_bot.now_s);
		for (uint8_t k = 1; k < carrier->seats; ++k) {
			const ChangeTarget seat{ p_bot.vehicle, k };
			if (seat == equipment) {
				candidate = 2.0f * change_unit_urgency(p_bot, p_vehicles, seat, false) * driver;
				current = std::max(candidate, 0.0f);
			} else {
				candidate = change_unit_urgency(p_bot, p_vehicles, seat, true) * driver;
			}
			consider(best, seat, candidate);
		}
	}

	bool get_out = false;
	if (!on_foot) {
		candidate = change_unit_urgency(p_bot, p_vehicles, ChangeTarget{}, false) * 0.2f;
		if (p_bot.stuck || 0.001f > current) {
			candidate = kGetOutUrgency;
		}
		if (candidate > best.urgency) {
			get_out = true;
			best = Pick{ ChangeTarget{}, candidate };
		}
	}

	if (!get_out) {
		const float radius_sqr = kHardwareRadius * kHardwareRadius;
		for (size_t v = 0; v < p_vehicles.size(); ++v) {
			const ChangeVehicle &vehicle = p_vehicles[v];
			if (int(v) == p_bot.vehicle || vehicle.kind == ChangeUnitKind::Soldier ||
					vehicle.kind == ChangeUnitKind::Naval || manned_by_enemy(vehicle, p_bot.side)) {
				continue;
			}
			const float d_sqr = distance_sq(vehicle.position, position);
			if (d_sqr > radius_sqr) {
				continue;
			}
			if (vehicle.kind != ChangeUnitKind::Air && (!vehicle.on_nav || upside_down(vehicle))) {
				continue;
			}
			const float falloff = std::min((radius_sqr - d_sqr * 0.8f) / radius_sqr, 0.5f);
			const float distance_factor = std::max(falloff + 0.5f, 0.0f);
			const bool reachable = equipment != primary ||
					vehicle.nav_gap * vehicle.nav_gap <
							(vehicle.radius + p_bot.radius) * (vehicle.radius + p_bot.radius);
			if (!reachable) {
				continue;
			}
			const bool hurt = on_foot && 400.0f > d_sqr && 0.25f > p_bot.health;
			candidate = change_unit_urgency(p_bot, p_vehicles, ChangeTarget{ int(v), 0 }, true) * distance_factor;
			if (hurt) {
				candidate *= 50.0f;
			}
			consider(best, ChangeTarget{ int(v), 0 }, candidate);
			const float driver = change_driver_modifier(vehicle, false, p_bot.now_s);
			for (uint8_t k = 1; k < vehicle.seats; ++k) {
				candidate = change_unit_urgency(p_bot, p_vehicles, ChangeTarget{ int(v), k }, true) * distance_factor *
						driver;
				if (hurt) {
					candidate *= 50.0f;
				}
				consider(best, ChangeTarget{ int(v), k }, candidate);
			}
		}
	}

	out.best = best.urgency;
	out.current = current;
	if (best.target == equipment) {
		return out;
	}
	out.target = best.target;
	out.change = true;
	out.get_out = get_out;
	out.same_vehicle = !on_foot && best.target.vehicle == p_bot.vehicle;
	const float ratio = current != 0.0f ? best.urgency * 0.5f / current : best.urgency * 0.5f;
	out.urgency = DecliningSlopeCurve::calculate(ratio) * 4.0f;
	const double since = p_bot.now_s - (on_foot ? kNeverPossessedSeconds : carrier->seat[p_bot.seat].possessed_s);
	if (kPossessionWindowSeconds > since) {
		out.urgency *= 6.0f;
	}
	if (get_out) {
		out.urgency *= 5.0f;
	}
	if (on_foot && 400.0f > distance_sq(p_vehicles[size_t(out.target.vehicle)].position, position) &&
			p_bot.health < 0.25f) {
		out.urgency *= 5.0f;
	}
	if (!out.target.soldier()) {
		const int booked = p_vehicles[size_t(out.target.vehicle)].seat[out.target.seat].booked_by;
		if (booked >= 0 && booked != p_bot.player_id && out.urgency > 0.0f) {
			out.urgency = 0.0f;
		}
	}
	return out;
}

ChangeOrder change_plan_step(
		const ChangeBot &p_bot, std::span<const ChangeVehicle> p_vehicles, const ChangeTarget &p_target) {
	ChangeOrder order;
	const bool on_foot = p_bot.vehicle < 0;
	if (p_target.soldier()) {
		order.step = on_foot ? ChangeStep::None : ChangeStep::GetOut;
		return order;
	}
	const ChangeVehicle &target = p_vehicles[size_t(p_target.vehicle)];
	if (!on_foot) {
		if (p_target.vehicle == p_bot.vehicle) {
			order.step = p_target.seat == p_bot.seat ? ChangeStep::None : ChangeStep::SelectSeat;
			order.seat = p_target.seat;
			return order;
		}
		order.step = p_bot.seat == 0 ? ChangeStep::Hold : ChangeStep::GetOut;
		return order;
	}
	const ChangeSeat &seat = target.seat[p_target.seat];
	if ((seat.occupant >= 0 && seat.occupant != p_bot.player_id) || manned_by_enemy(target, p_bot.side)) {
		return order;
	}
	order.goal = target.position;
	const float reach = target.radius + p_bot.radius + 1.0f;
	order.step =
			distance_sq(p_bot.position, target.position) <= reach * reach ? ChangeStep::Enter : ChangeStep::Approach;
	return order;
}

}
