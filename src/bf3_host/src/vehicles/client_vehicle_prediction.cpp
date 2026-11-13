#include "client_vehicle_prediction.h"

#include "bf3/core/math.h"
#include "bf3/sim/vehicles/vehicle_input.h"
#include "server_vehicles.h"

#include <algorithm>

namespace bf3::host {

namespace {

constexpr uint32_t kRestoreGroups =
		sim::vehicle_restore::kBody | sim::vehicle_restore::kWheels | sim::vehicle_restore::kDriver;

void merge_wire(sim::VehicleState &r_state, const sim::VehicleState &p_wire) {
	r_state.body.com_position = p_wire.body.com_position;
	r_state.body.rotation = p_wire.body.rotation;
	r_state.body.linear_velocity = p_wire.body.linear_velocity;
	r_state.body.angular_velocity = p_wire.body.angular_velocity;
	r_state.body.active = p_wire.body.active;
	for (size_t i = 0; i < r_state.wheel_count && i < p_wire.wheel_count; ++i) {
		r_state.wheels[i].angular_velocity = p_wire.wheels[i].angular_velocity;
		r_state.wheels[i].angle = p_wire.wheels[i].angle;
		r_state.wheels[i].suspension_length = p_wire.wheels[i].suspension_length;
	}
	r_state.driver = p_wire.driver;
}

bool wire_matches(const sim::VehicleState &p_state, const sim::VehicleState &p_wire) {
	const sim::VehicleBodyState &a = p_state.body, &b = p_wire.body;
	if (a.com_position != b.com_position || a.rotation != b.rotation || a.linear_velocity != b.linear_velocity ||
			a.angular_velocity != b.angular_velocity || a.active != b.active || !(p_state.driver == p_wire.driver) ||
			p_state.wheel_count != p_wire.wheel_count) {
		return false;
	}
	for (size_t i = 0; i < p_state.wheel_count && i < sim::kMaxVehicleWheels; ++i) {
		const sim::VehicleWheelState &x = p_state.wheels[i], &y = p_wire.wheels[i];
		if (x.angular_velocity != y.angular_velocity || x.angle != y.angle ||
				x.suspension_length != y.suspension_length) {
			return false;
		}
	}
	return true;
}

PresentedVehiclePose pose_of(const sim::VehicleState &p_state, const sim::Vec3 &p_offset) {
	PresentedVehiclePose p;
	p.com_position = core::vec::add(p_state.body.com_position, p_offset);
	p.rotation = p_state.body.rotation;
	for (size_t i = 0; i < p_state.wheel_count && i < sim::kMaxVehicleWheels; ++i) {
		p.wheel_angle[i] = p_state.wheels[i].angle;
		p.wheel_steer[i] = p_state.wheels[i].steer_angle;
		p.suspension_length[i] = p_state.wheels[i].suspension_length;
	}
	return p;
}

}

sim::Vec3 VehicleBlend::shown() const {
	return core::vec::scale(offset, total > 1e-6f ? remaining / total : 0.0f);
}

void VehicleBlend::correct(const sim::Vec3 &p_error) {
	const float d = core::vec::length(p_error);
	if (d < kVehicleBlendMinM || d > kVehicleBlendMaxM) {
		*this = VehicleBlend();
		return;
	}
	offset = p_error;
	total = std::min(d, kVehicleBlendLongestS);
	remaining = total;
}

void VehicleBlend::decay(float p_seconds) {
	remaining = std::max(remaining - p_seconds, 0.0f);
}

core::Status ClientVehiclePrediction::init(const SharedLevel &p_level) {
	level_ = p_level.vehicles;
	world_.reset();
	if (level_ == nullptr || level_->spawners.empty() || p_level.static_world == nullptr) {
		return core::Status::success();
	}
	sim::VehicleWorldOptions o;
	o.max_vehicles = uint32_t(std::min<size_t>(level_->spawners.size(), sim::kMaxVehicleSlots));
	o.tuning = level_->tuning;
	core::Result<std::unique_ptr<sim::VehicleWorld>> w = sim::VehicleWorld::create(p_level.static_world, o);
	if (!w) {
		return core::error_at("client vehicle prediction", w.error());
	}
	world_ = std::move(*w);
	ring_.assign(kVehiclePredictionRing, Entry());
	return core::Status::success();
}

const ClientVehiclePrediction::Entry *ClientVehiclePrediction::entry(sim::Tick p_tick) const {
	if (ring_.empty()) {
		return nullptr;
	}
	const Entry &e = ring_[p_tick % ring_.size()];
	return e.used && e.tick == p_tick ? &e : nullptr;
}

void ClientVehiclePrediction::keep(sim::Tick p_tick) {
	Entry &e = ring_[p_tick % ring_.size()];
	e.used = true;
	e.tick = p_tick;
	world_->save(e.snapshot);
	e.state = world_->state(sim::VehicleSlot(spawner_));
}

void ClientVehiclePrediction::stop_driving() {
	if (!driving_) {
		return;
	}
	if (world_->has_vehicle(sim::VehicleSlot(spawner_)) && !world_->remove_vehicle(sim::VehicleSlot(spawner_)).ok()) {
		++failures_;
	}
	for (Entry &e : ring_) {
		e.used = false;
	}
	driving_ = false;
	blend_ = VehicleBlend();
}

bool ClientVehiclePrediction::start(const net::VehicleWire &p_wire, sim::Tick p_tick) {
	const LevelVehicleKind &kind = level_->kinds[level_->spawners[p_wire.spawner].kind];
	sim::VehiclePlacement placement;
	placement.rotation = p_wire.state.body.rotation;
	placement.position = vehicle_body_origin(*kind.model, p_wire.state.body);
	const sim::VehicleSlot slot(p_wire.spawner);
	if (!world_->add_vehicle(slot, kind.model, placement).ok()) {
		return false;
	}
	sim::VehicleState target = world_->state(slot);
	merge_wire(target, p_wire.state);
	if (!world_->restore_vehicle_state(slot, target, kRestoreGroups, sim::ContactCachePolicy::Keep).ok()) {
		(void)world_->remove_vehicle(slot);
		return false;
	}
	driving_ = true;
	spawner_ = p_wire.spawner;
	newest_ = p_tick;
	reconciled_ = p_tick;
	blend_ = VehicleBlend();
	keep(p_tick);
	++starts_;
	return true;
}

void ClientVehiclePrediction::correct(const net::VehicleWire &p_wire, sim::Tick p_tick) {
	const sim::VehicleSlot slot(spawner_);
	if (const Entry *e = entry(p_tick)) {
		if (wire_matches(e->state, p_wire.state)) {
			++matches_;
			return;
		}
		if (!world_->restore(e->snapshot).ok()) {
			++failures_;
		}
	}
	sim::VehicleState target = world_->state(slot);
	merge_wire(target, p_wire.state);
	if (!world_->restore_vehicle_state(slot, target, kRestoreGroups, sim::ContactCachePolicy::Keep).ok()) {
		++failures_;
		return;
	}
	keep(p_tick);
	++corrections_;
}

bool ClientVehiclePrediction::advance(const sim::VehicleCommand &p_command) {
	const sim::VehicleSlot slot(spawner_);
	if (!world_->step(std::span(&slot, 1), std::span(&p_command, 1)).ok()) {
		++failures_;
		return false;
	}
	return true;
}

sim::VehicleCommand ClientVehiclePrediction::command_of(const sim::Command &p_command) const {
	const sim::vehicles::VehicleCondition condition = sim::vehicles::VehicleCondition(condition_);
	if (condition == sim::vehicles::VehicleCondition::Destroyed) {
		return sim::VehicleCommand();
	}
	const sim::VehicleCommand command = sim::vehicles::driver_command(p_command);
	return condition == sim::vehicles::VehicleCondition::Disabled
			? disabled_command(command, level_->kinds[level_->spawners[spawner_].kind].disabled_input)
			: command;
}

void ClientVehiclePrediction::replay(sim::Tick p_from, sim::Tick p_next, const CommandAt &p_commands) {
	const sim::Command *base = p_commands(p_from);
	sim::Command previous = base != nullptr ? *base : sim::Command();
	sim::Tick from = p_from;
	if (p_next > from + 1u + kVehiclePredictionRing) {
		from = p_next - 1u - kVehiclePredictionRing;
	}
	for (sim::Tick t = from + 1u; t < p_next; ++t) {
		const sim::Command *c = p_commands(t);
		const sim::Command use = c != nullptr && c->tick == t ? *c : sim::repeated_command(previous, t);
		if (!advance(command_of(use))) {
			return;
		}
		keep(t);
		newest_ = t;
		previous = use;
		++replayed_ticks_;
	}
}

void ClientVehiclePrediction::reconcile(
		const ClientVehicles &p_seen, uint8_t p_own_slot, const CommandAt &p_commands, sim::Tick p_next) {
	if (world_ == nullptr) {
		return;
	}
	const std::optional<SeenSeat> s = p_seen.seat_of(p_own_slot);
	if (!s || s->wire == nullptr || s->wire->spawner >= level_->spawners.size()) {
		seated_ = false;
		stop_driving();
		return;
	}
	const net::VehicleWire &w = *s->wire;
	const bool same = driving_ && spawner_ == w.spawner && generation_ == w.generation;
	if (!(seated_ && spawner_ == w.spawner && generation_ == w.generation)) {
		seated_from_ = s->tick;
	}
	seated_ = true;
	seat_ = s->seat;
	generation_ = w.generation;
	condition_ = w.condition;
	seen_state_ = w.state;
	seen_state_.body.origin = vehicle_body_origin(*level_->kinds[level_->spawners[w.spawner].kind].model, w.state.body);
	if (s->seat != 0) {
		stop_driving();
		spawner_ = w.spawner;
		return;
	}
	if (!same) {
		stop_driving();
		if (!start(w, s->tick)) {
			++failures_;
			return;
		}
		replay(s->tick, p_next, p_commands);
		return;
	}
	if (s->tick <= reconciled_ || s->tick + 1u >= p_next) {
		return;
	}
	reconciled_ = s->tick;
	const Entry *before = entry(newest_);
	const std::optional<sim::Vec3> predicted =
			before != nullptr ? std::optional<sim::Vec3>(before->state.body.com_position) : std::nullopt;
	const uint64_t corrected = corrections_;
	correct(w, s->tick);
	if (corrections_ == corrected) {
		return;
	}
	replay(s->tick, p_next, p_commands);
	const Entry *after = entry(newest_);
	if (predicted && after != nullptr) {
		max_correction_m_ = std::max(
				max_correction_m_, core::vec::length(core::vec::sub(*predicted, after->state.body.com_position)));
		blend_.correct(core::vec::sub(core::vec::add(*predicted, blend_.shown()), after->state.body.com_position));
	}
}

void ClientVehiclePrediction::step(const sim::Command &p_command) {
	if (!driving_ || p_command.tick != newest_ + 1u) {
		return;
	}
	if (!advance(command_of(p_command))) {
		return;
	}
	keep(p_command.tick);
	newest_ = p_command.tick;
	blend_.decay(sim::kTickSeconds);
	++predicted_ticks_;
}

std::optional<sim::Vec3> ClientVehiclePrediction::seat_of(const sim::VehicleState &p_state) const {
	const LevelVehicleKind &kind = level_->kinds[level_->spawners[spawner_].kind];
	if (seat_ >= kind.seat_defs.size()) {
		return std::nullopt;
	}
	return vehicle_seat_point(kind, seat_, p_state.body.rotation, p_state.body.origin);
}

std::optional<sim::Vec3> ClientVehiclePrediction::seat_at(sim::Tick p_tick) const {
	if (!seated_ || level_ == nullptr || p_tick < seated_from_) {
		return std::nullopt;
	}
	if (driving_) {
		if (const Entry *e = entry(p_tick)) {
			return seat_of(e->state);
		}
	}
	return seat_of(seen_state_);
}

void ClientVehiclePrediction::publish(PresentationFrame &r_frame) const {
	if (!driving_) {
		return;
	}
	const Entry *now = entry(newest_);
	const Entry *previous = newest_ > 0 ? entry(newest_ - 1u) : nullptr;
	if (now == nullptr) {
		return;
	}
	for (size_t i = 0; i < r_frame.vehicle_count && i < r_frame.vehicles.size(); ++i) {
		PresentedVehicle &v = r_frame.vehicles[i];
		if (v.spawner != spawner_) {
			continue;
		}
		const sim::Vec3 offset = blend_.shown();
		const sim::VehicleState &s = now->state;
		v.tick = newest_;
		v.predicted = 1;
		v.com_position = core::vec::add(s.body.com_position, offset);
		v.rotation = s.body.rotation;
		v.linear_velocity = s.body.linear_velocity;
		v.angular_velocity = s.body.angular_velocity;
		v.wheel_count = std::min<uint8_t>(s.wheel_count, uint8_t(sim::kMaxVehicleWheels));
		v.wheel_contact = 0;
		for (size_t w = 0; w < v.wheel_count; ++w) {
			v.wheel_contact |= uint8_t((s.wheels[w].in_contact != 0 ? 1u : 0u) << w);
			v.wheel_spin[w] = s.wheels[w].angular_velocity;
			v.wheel_angle[w] = s.wheels[w].angle;
			v.wheel_steer[w] = s.wheels[w].steer_angle;
			v.suspension_length[w] = s.wheels[w].suspension_length;
		}
		v.throttle = s.driver.throttle_filter;
		v.engine_rpm = s.driver.engine_rpm;
		v.gear = int8_t(s.driver.gearbox.gear);
		v.window_count = 0;
		if (previous != nullptr) {
			v.window[v.window_count++] = PresentedVehicleSample{ newest_ - 1u, pose_of(previous->state, offset) };
		}
		v.window[v.window_count++] = PresentedVehicleSample{ newest_, pose_of(s, offset) };
		return;
	}
}

void ClientVehiclePrediction::stats(ClientStats &r_stats) const {
	r_stats.vehicle_predicted_ticks = predicted_ticks_;
	r_stats.vehicle_replayed_ticks = replayed_ticks_;
	r_stats.vehicle_prediction_matches = matches_;
	r_stats.vehicle_corrections = corrections_;
	r_stats.vehicle_prediction_starts = starts_;
	r_stats.vehicle_prediction_failures = failures_;
	r_stats.vehicle_max_correction_m = max_correction_m_;
}

}
