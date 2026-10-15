// The S2 replay harness and its judges (docs/design/s2.md §1, §7, §8; vehicle_replay.h). One engine records a
// server run of n slots (n = 1 for a course, 3 for G1m), then checks G1 (a prediction world restored from the tick-0
// snapshot), G1r (restores at chosen ticks, into the prediction world and in place) and, for one slot, G2 (the
// quantised-ghost client loop per restore mode).
#include "bf3/sim/vehicle_replay.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

namespace bf3::sim {

namespace {

constexpr float kPi = 3.14159265358979323846f;

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

Vec3 sub3(const Vec3 &p_a, const Vec3 &p_b) {
	return { p_a[0] - p_b[0], p_a[1] - p_b[1], p_a[2] - p_b[2] };
}

float len3(const Vec3 &p_a) {
	return std::sqrt(p_a[0] * p_a[0] + p_a[1] * p_a[1] + p_a[2] * p_a[2]);
}

Vec3 forward_of(const std::array<float, 4> &p_q) {
	const float x = p_q[0], y = p_q[1], z = p_q[2], w = p_q[3];
	return { 2.0f * (x * z + w * y), 2.0f * (y * z - w * x), 1.0f - 2.0f * (x * x + y * y) };
}

uint64_t splitmix(uint64_t &r_state) {
	uint64_t z = (r_state += 0x9E3779B97F4A7C15ull);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

// The angle (degrees) between two rotations, 2 acos |q . q'| through acos(d) = sqrt(2 (1 - d)) (1 + (1 - d) / 12)
// (the series is exact to 1e-7 rad below 1 degree; reported only).
float rotation_error_degrees(const std::array<float, 4> &p_a, const std::array<float, 4> &p_b) {
	double d = std::fabs(double(p_a[0]) * p_b[0] + double(p_a[1]) * p_b[1] + double(p_a[2]) * p_b[2] + double(p_a[3]) * p_b[3]);
	d = std::min(d, 1.0);
	const double e = 1.0 - d;
	return float(2.0 * std::sqrt(2.0 * e) * (1.0 + e / 12.0) * 180.0 / 3.14159265358979323846);
}

VehicleErrorStats stats_of(std::vector<std::pair<float, Tick>> p_samples) {
	VehicleErrorStats s;
	s.samples = p_samples.size();
	if (p_samples.empty()) {
		return s;
	}
	double sum = 0.0;
	bool first = true;
	for (const auto &[e, tick] : p_samples) {
		sum += e;
		if (first || e > s.max) {
			s.max = e;
			s.worst_tick = tick;
			first = false;
		}
	}
	s.mean = float(sum / double(p_samples.size()));
	std::sort(p_samples.begin(), p_samples.end(), [](const auto &p_a, const auto &p_b) { return p_a.first < p_b.first; });
	const size_t rank = size_t(std::ceil(0.99 * double(p_samples.size())));
	s.p99 = p_samples[std::max<size_t>(rank, 1) - 1].first;
	return s;
}

// ---- the engine -----------------------------------------------------------------------------------------------------

struct Recording {
	size_t slots = 0;
	uint32_t ticks = 0;
	std::vector<VehicleWorldSnapshot> snapshots; // ticks 0..ticks
	std::vector<std::vector<VehicleState>> states; // [tick][slot]
	std::vector<std::vector<VehicleContacts>> contacts; // [tick][slot]; tick 0 = the settle's last step
	std::vector<std::vector<uint64_t>> state_hashes; // [tick][slot]
	std::vector<std::vector<VehicleCommand>> commands; // [tick - 1][slot]
};

struct Harness {
	std::shared_ptr<const StaticWorldShapes> shapes;
	std::shared_ptr<const VehicleModel> model;
	std::vector<VehicleCourse> courses; // slot i drives courses[i]
	std::vector<VehicleSlot> slot_ids;
	uint64_t steps = 0;

	core::Result<std::unique_ptr<VehicleWorld>> make_world() const {
		VehicleWorldOptions o;
		o.max_vehicles = uint32_t(courses.size());
		o.tuning = model->tuning;
		core::Result<std::unique_ptr<VehicleWorld>> w = VehicleWorld::create(shapes, o);
		BF3_TRY(w);
		for (size_t i = 0; i < courses.size(); ++i) {
			BF3_TRY((*w)->add_vehicle(VehicleSlot(i), model, courses[i].start));
		}
		return w;
	}

	core::Status step(VehicleWorld &r_world, const std::vector<VehicleCommand> &p_commands) {
		++steps;
		return r_world.step(slot_ids, p_commands);
	}

	// Settle, then record ticks 1..p_ticks with each slot's scripted driver (closed loop on the server's state).
	core::Status record(VehicleWorld &r_server, uint32_t p_ticks, Recording &r_rec) {
		const size_t n = courses.size();
		r_rec.slots = n;
		r_rec.ticks = p_ticks;
		const std::vector<VehicleCommand> settle(n, kSettleCommand);
		for (uint32_t t = 0; t < kSettleTicks; ++t) {
			BF3_TRY(step(r_server, settle));
		}
		r_rec.snapshots.resize(size_t(p_ticks) + 1);
		r_rec.states.assign(size_t(p_ticks) + 1, std::vector<VehicleState>(n));
		r_rec.contacts.assign(size_t(p_ticks) + 1, std::vector<VehicleContacts>(n));
		r_rec.state_hashes.assign(size_t(p_ticks) + 1, std::vector<uint64_t>(n));
		r_rec.commands.assign(p_ticks, std::vector<VehicleCommand>(n));
		const auto capture = [&](uint32_t p_tick) {
			r_server.save(r_rec.snapshots[p_tick]);
			for (size_t s = 0; s < n; ++s) {
				r_rec.states[p_tick][s] = r_server.state(VehicleSlot(s));
				r_rec.contacts[p_tick][s] = r_server.contacts(VehicleSlot(s));
				r_rec.state_hashes[p_tick][s] = vehicle_state_hash(r_rec.states[p_tick][s]);
			}
		};
		capture(0);
		std::vector<VehicleScriptMemory> memory(n);
		for (uint32_t t = 1; t <= p_ticks; ++t) {
			std::vector<VehicleCommand> &c = r_rec.commands[t - 1];
			for (size_t s = 0; s < n; ++s) {
				c[s] = drive_vehicle_course(courses[s], *model, r_rec.states[t - 1][s], t, memory[s]);
			}
			BF3_TRY(step(r_server, c));
			capture(t);
		}
		return core::Status::success();
	}

	// "slot<i>.<field>" or "jolt:<section>+<offset>" for the first difference of p_world against the recording at
	// p_tick (the server's sections come from a scratch world restored from its snapshot).
	std::string divergence(const VehicleWorld &p_world, const Recording &p_rec, uint32_t p_tick) const {
		for (size_t s = 0; s < p_rec.slots; ++s) {
			const std::string field = vehicle_state_first_difference(p_rec.states[p_tick][s], p_world.state(VehicleSlot(s)));
			if (!field.empty()) {
				return p_rec.slots == 1 ? field : "slot" + std::to_string(s) + "." + field;
			}
		}
		core::Result<std::unique_ptr<VehicleWorld>> scratch = make_world();
		if (!scratch || !(*scratch)->restore(p_rec.snapshots[p_tick]).ok()) {
			return "jolt:?";
		}
		for (size_t k = 0; k < kVehicleSnapshotSectionCount; ++k) {
			std::vector<uint8_t> a, b;
			(*scratch)->save_section(VehicleSnapshotSection(k), a);
			p_world.save_section(VehicleSnapshotSection(k), b);
			if (a != b) {
				size_t at = 0;
				while (at < a.size() && at < b.size() && a[at] == b[at]) {
					++at;
				}
				return std::string("jolt:") + vehicle_snapshot_section_name(VehicleSnapshotSection(k)) + "+" + std::to_string(at);
			}
		}
		return "drivers";
	}

	bool matches(const VehicleWorld &p_world, const Recording &p_rec, uint32_t p_tick) const {
		if (p_world.state_hash() != p_rec.snapshots[p_tick].hash) {
			return false;
		}
		for (size_t s = 0; s < p_rec.slots; ++s) {
			if (vehicle_state_hash(p_world.state(VehicleSlot(s))) != p_rec.state_hashes[p_tick][s]) {
				return false;
			}
		}
		return true;
	}

	// Steps p_world through ticks p_from + 1 .. p_to with the logged commands, comparing every tick.
	core::Status replay(VehicleWorld &r_world, const Recording &p_rec, uint32_t p_from, uint32_t p_to, uint32_t &r_ticks, uint32_t &r_mismatches,
			VehicleDivergence &r_first) {
		for (uint32_t t = p_from + 1; t <= p_to; ++t) {
			BF3_TRY(step(r_world, p_rec.commands[t - 1]));
			++r_ticks;
			if (!matches(r_world, p_rec, t)) {
				if (r_mismatches == 0) {
					r_first.tick = t;
					r_first.field = divergence(r_world, p_rec, t);
				}
				++r_mismatches;
			}
		}
		return core::Status::success();
	}

	// G1 then G1r (prediction world and in-place rollback of p_server).
	core::Status check_g1(VehicleWorld &r_server, const Recording &p_rec, const VehicleReplayOptions &p_options, uint64_t p_salt, uint32_t &r_g1_ticks,
			uint32_t &r_g1_mismatches, VehicleDivergence &r_g1_first, uint32_t &r_runs, uint32_t &r_replay_ticks, uint32_t &r_replay_mismatches,
			VehicleDivergence &r_replay_first, std::unique_ptr<VehicleWorld> &r_prediction) {
		core::Result<std::unique_ptr<VehicleWorld>> made = make_world();
		BF3_TRY(made);
		r_prediction = std::move(*made);
		BF3_TRY(r_prediction->restore(p_rec.snapshots[0]));
		BF3_TRY(replay(*r_prediction, p_rec, 0, p_rec.ticks, r_g1_ticks, r_g1_mismatches, r_g1_first));
		if (p_rec.ticks < 2) {
			return core::Status::success();
		}
		std::vector<uint32_t> restore_ticks;
		for (const uint32_t t : p_options.restore_ticks) {
			restore_ticks.push_back(std::min(t, p_rec.ticks - 1));
		}
		uint64_t seed = p_options.seed ^ (p_salt * 0x9E3779B97F4A7C15ull);
		for (uint32_t i = 0; i < p_options.random_restores; ++i) {
			restore_ticks.push_back(1u + uint32_t(splitmix(seed) % uint64_t(p_rec.ticks - 1)));
		}
		for (const uint32_t t : restore_ticks) {
			const uint32_t end = std::min<uint32_t>(p_rec.ticks, t + p_options.replay_length);
			for (VehicleWorld *w : { r_prediction.get(), &r_server }) {
				BF3_TRY(w->restore(p_rec.snapshots[t]));
				++r_runs;
				BF3_TRY(replay(*w, p_rec, t, end, r_replay_ticks, r_replay_mismatches, r_replay_first));
			}
		}
		return core::Status::success();
	}
};

// The server run's facts (gate D) for slot 0.
void course_facts(const Recording &p_rec, const VehicleModel &p_model, VehicleCourseReport &r_report) {
	float last_sign = 0.0f;
	double heading_sum = 0.0;
	float last_heading = 0.0f;
	for (uint32_t t = 0; t <= p_rec.ticks; ++t) {
		const VehicleState &s = p_rec.states[t][0];
		const VehicleContacts &c = p_rec.contacts[t][0];
		const Vec3 f = forward_of(s.body.rotation);
		const float heading = std::atan2(f[0], f[2]);
		if (t == 0) {
			last_heading = heading;
			continue;
		}
		const VehicleState &prev = p_rec.states[t - 1][0];
		const float speed = len3(s.body.linear_velocity);
		const float prev_speed = len3(prev.body.linear_velocity);
		r_report.distance += len3(sub3(s.body.com_position, prev.body.com_position));
		r_report.max_speed_kmh = std::max(r_report.max_speed_kmh, speed * 3.6f);
		r_report.ticks_all_wheels_off += c.wheels_in_contact == 0 ? 1u : 0u;
		r_report.ticks_any_wheel_off += c.wheels_in_contact < p_model.wheel_count ? 1u : 0u;
		r_report.ticks_static_contact += c.chassis_static_contacts > 0 ? 1u : 0u;
		if (c.chassis_static_contacts > 0 && p_rec.contacts[t - 1][0].chassis_static_contacts == 0 && prev_speed * 3.6f >= 10.0f) {
			++r_report.static_impacts;
			r_report.max_impact_speed_kmh = std::max(r_report.max_impact_speed_kmh, prev_speed * 3.6f);
		}
		r_report.ticks_asleep += s.body.active == 0 ? 1u : 0u;
		const float yaw_rate = s.body.angular_velocity[1];
		if (speed * 3.6f >= 15.0f && std::fabs(yaw_rate) > 0.05f) {
			const float sign = yaw_rate > 0.0f ? 1.0f : -1.0f;
			if (last_sign != 0.0f && sign != last_sign) {
				++r_report.yaw_rate_sign_changes;
			}
			last_sign = sign;
		}
		float delta = heading - last_heading;
		if (delta > kPi) {
			delta -= 2.0f * kPi;
		} else if (delta < -kPi) {
			delta += 2.0f * kPi;
		}
		heading_sum += delta;
		last_heading = heading;
		if (t >= 600 && p_rec.ticks >= 600) {
			r_report.creep = std::max(r_report.creep, len3(sub3(s.body.com_position, p_rec.states[600][0].body.com_position)));
		}
	}
	r_report.yaw_turns = float(std::fabs(heading_sum) / (2.0 * 3.14159265358979323846));
}

const char *kModeNames[kVehicleRestoreModeCount] = { "own", "invalidate", "lambdas", "pod", "pod-own-lambdas", "full" };

// The G2 client loop for one mode and replay length (s2.md §8.4 step 5).
core::Status client_loop(Harness &r_h, VehicleWorld &r_client, const Recording &p_rec, VehicleRestoreMode p_mode, uint32_t p_replay,
		const VehicleReplayOptions &p_options, VehicleCorrectionReport &r_report) {
	r_report.mode = p_mode;
	r_report.replay_ticks = p_replay;
	const uint32_t interval = p_options.snapshot_interval;
	if (interval == 0 || p_replay < interval) {
		return core::make_error("vehicle G2: replay length " + std::to_string(p_replay) + " is below the snapshot interval " + std::to_string(interval));
	}
	const VehicleGhostScale scale = vehicle_ghost_scale(*r_h.model);
	std::vector<VehicleWorldSnapshot> history(size_t(p_rec.ticks) + 1);
	std::vector<uint8_t> have(size_t(p_rec.ticks) + 1, 0);
	history[0] = p_rec.snapshots[0];
	have[0] = 1;
	std::vector<std::pair<float, Tick>> all, wheel_contact, static_collision;
	for (uint32_t n = 0; n + p_replay <= p_rec.ticks; n += interval) {
		if (p_mode == VehicleRestoreMode::Full) {
			BF3_TRY(r_client.restore(p_rec.snapshots[n]));
		} else {
			if (have[n] == 0) {
				return core::make_error("vehicle G2: no client history at tick " + std::to_string(n));
			}
			BF3_TRY(r_client.restore(history[n]));
			const VehicleState &server = p_rec.states[n][0];
			VehicleState target = r_client.state(0);
			uint32_t groups = vehicle_restore::kGhost;
			switch (p_mode) {
				case VehicleRestoreMode::OwnHistory:
				case VehicleRestoreMode::Invalidate:
				case VehicleRestoreMode::GhostLambdas: {
					const VehicleGhostCodes codes = encode_vehicle_ghost(server, p_options.quantisation, scale);
					BF3_TRY(decode_vehicle_ghost(codes, p_options.quantisation, scale, target));
					if (p_mode == VehicleRestoreMode::GhostLambdas) {
						for (size_t w = 0; w < target.wheel_count; ++w) {
							target.wheels[w].lambda_suspension = server.wheels[w].lambda_suspension;
							target.wheels[w].lambda_suspension_max_up = server.wheels[w].lambda_suspension_max_up;
							target.wheels[w].lambda_longitudinal = server.wheels[w].lambda_longitudinal;
							target.wheels[w].lambda_lateral = server.wheels[w].lambda_lateral;
						}
						groups |= vehicle_restore::kLambdas;
					}
					break;
				}
				case VehicleRestoreMode::ExactPod:
					target = server;
					groups = vehicle_restore::kAll;
					break;
				case VehicleRestoreMode::PodOwnLambdas:
					target = server;
					groups = vehicle_restore::kAll & ~vehicle_restore::kLambdas;
					break;
				case VehicleRestoreMode::Full:
					break;
			}
			BF3_TRY(r_client.restore_vehicle_state(0, target, groups,
					p_mode == VehicleRestoreMode::Invalidate ? ContactCachePolicy::Invalidate : ContactCachePolicy::Keep));
		}
		for (uint32_t t = n + 1; t <= n + p_replay; ++t) {
			BF3_TRY(r_h.step(r_client, p_rec.commands[t - 1]));
			if (t == n + interval) {
				r_client.save(history[t]);
				have[t] = 1;
			}
		}
		const VehicleState client = r_client.state(0);
		const VehicleState &server = p_rec.states[n + p_replay][0];
		const float error = len3(sub3(client.body.origin, server.body.origin));
		r_report.max_orientation_degrees = std::max(r_report.max_orientation_degrees, rotation_error_degrees(client.body.rotation, server.body.rotation));
		r_report.max_velocity_error = std::max(r_report.max_velocity_error, len3(sub3(client.body.linear_velocity, server.body.linear_velocity)));
		all.push_back({ error, n });
		if (p_rec.contacts[n + p_replay][0].wheels_in_contact > 0) {
			wheel_contact.push_back({ error, n });
		}
		bool touched = false;
		for (uint32_t t = n; t <= n + p_replay; ++t) {
			touched = touched || p_rec.contacts[t][0].chassis_static_contacts > 0;
		}
		if (touched) {
			static_collision.push_back({ error, n });
		}
	}
	r_report.all = stats_of(std::move(all));
	r_report.wheel_contact = stats_of(std::move(wheel_contact));
	r_report.static_collision = stats_of(std::move(static_collision));
	return core::Status::success();
}

} // namespace

const char *vehicle_restore_mode_name(VehicleRestoreMode p_mode) {
	return size_t(p_mode) < kVehicleRestoreModeCount ? kModeNames[size_t(p_mode)] : "?";
}

core::Result<VehicleCourseReport> run_vehicle_course(std::shared_ptr<const StaticWorldShapes> p_shapes, std::shared_ptr<const VehicleModel> p_model,
		const VehicleCourse &p_course, const VehicleReplayOptions &p_options) {
	const auto start = std::chrono::steady_clock::now();
	VehicleCourseReport report;
	report.course = p_course;
	report.ticks = p_options.ticks;
	if (p_shapes == nullptr || p_model == nullptr) {
		return core::make_error("vehicle course: no shapes or model");
	}
	if (p_options.ticks == 0 || p_options.ticks > kMaxVehicleLogCommands) {
		return core::make_error("vehicle course: ticks outside 1.." + std::to_string(kMaxVehicleLogCommands));
	}
	if (!p_course.found) {
		return report;
	}
	Harness h;
	h.shapes = p_shapes;
	h.model = p_model;
	h.courses = { p_course };
	h.slot_ids = { 0 };
	core::Result<std::unique_ptr<VehicleWorld>> server = h.make_world();
	BF3_TRY(server);
	Recording rec;
	BF3_TRY(h.record(**server, p_options.ticks, rec));
	course_facts(rec, *p_model, report);
	report.snapshot_bytes = rec.snapshots[0].jolt.size() + rec.snapshots[0].drivers.size() * sizeof(VehicleDriverState);
	if (p_options.keep_hashes) {
		for (const VehicleWorldSnapshot &s : rec.snapshots) {
			report.server_hashes.push_back(s.hash);
		}
	}
	report.drove = vehicle_course_drove(report, &report.drove_why);

	std::unique_ptr<VehicleWorld> prediction;
	if (p_options.g1) {
		BF3_TRY(h.check_g1(**server, rec, p_options, uint64_t(p_course.kind) + 1u, report.g1_ticks, report.g1_mismatches, report.g1_first, report.replay_runs,
				report.replay_ticks, report.replay_mismatches, report.replay_first, prediction));
	}
	if (p_options.g2) {
		if (prediction == nullptr) {
			core::Result<std::unique_ptr<VehicleWorld>> made = h.make_world();
			BF3_TRY(made);
			prediction = std::move(*made);
		}
		const uint32_t modes = p_options.modes | 1u; // the primary mode always runs
		for (size_t m = 0; m < kVehicleRestoreModeCount; ++m) {
			if ((modes & (1u << m)) == 0) {
				continue;
			}
			for (const uint32_t r : p_options.replay_ticks) {
				VehicleCorrectionReport c;
				BF3_TRY(client_loop(h, *prediction, rec, VehicleRestoreMode(m), r, p_options, c));
				report.corrections.push_back(c);
			}
		}
	}
	report.steps = h.steps;
	report.seconds = seconds_since(start);
	return report;
}

bool vehicle_course_drove(const VehicleCourseReport &p_report, std::string *r_why) {
	const auto fail = [&](const std::string &p_why) {
		if (r_why != nullptr) {
			*r_why = std::string(vehicle_course_name(p_report.course.kind)) + ": " + p_why;
		}
		return false;
	};
	if (!p_report.course.found) {
		if (r_why != nullptr) {
			r_why->clear();
		}
		return true; // gate C judges it
	}
	const size_t kind = size_t(p_report.course.kind);
	if (kind >= kVehicleCourseKindCount) {
		return fail("unknown kind");
	}
	const VehicleActivityThresholds &a = kVehicleActivity[kind];
	char buffer[160];
	if (a.must_stay_awake && p_report.ticks_asleep != 0) {
		std::snprintf(buffer, sizeof(buffer), "asleep for %u ticks (must stay awake)", p_report.ticks_asleep);
		return fail(buffer);
	}
	if (p_report.distance < a.min_distance) {
		std::snprintf(buffer, sizeof(buffer), "drove %.1f m (< %.0f m)", double(p_report.distance), double(a.min_distance));
		return fail(buffer);
	}
	if (p_report.max_speed_kmh < a.min_max_speed_kmh) {
		std::snprintf(buffer, sizeof(buffer), "top speed %.1f km/h (< %.0f km/h)", double(p_report.max_speed_kmh), double(a.min_max_speed_kmh));
		return fail(buffer);
	}
	if (p_report.yaw_rate_sign_changes < a.min_yaw_rate_sign_changes) {
		std::snprintf(buffer, sizeof(buffer), "%u yaw-rate sign changes (< %u)", p_report.yaw_rate_sign_changes, a.min_yaw_rate_sign_changes);
		return fail(buffer);
	}
	if (p_report.yaw_turns < a.min_yaw_turns) {
		std::snprintf(buffer, sizeof(buffer), "%.2f turns (< %.0f)", double(p_report.yaw_turns), double(a.min_yaw_turns));
		return fail(buffer);
	}
	if (p_report.ticks_any_wheel_off < a.min_ticks_any_wheel_off) {
		std::snprintf(buffer, sizeof(buffer), "%u ticks with a wheel off the ground (< %u)", p_report.ticks_any_wheel_off, a.min_ticks_any_wheel_off);
		return fail(buffer);
	}
	if (p_report.static_impacts < a.min_static_impacts) {
		std::snprintf(buffer, sizeof(buffer), "%u impacts (< %u)", p_report.static_impacts, a.min_static_impacts);
		return fail(buffer);
	}
	if (p_report.ticks_static_contact < a.min_ticks_static_contact) {
		std::snprintf(buffer, sizeof(buffer), "%u static-contact ticks (< %u)", p_report.ticks_static_contact, a.min_ticks_static_contact);
		return fail(buffer);
	}
	if (a.max_creep > 0.0f && !(p_report.creep <= a.max_creep)) {
		std::snprintf(buffer, sizeof(buffer), "crept %.3f m (> %.2f m)", double(p_report.creep), double(a.max_creep));
		return fail(buffer);
	}
	if (r_why != nullptr) {
		r_why->clear();
	}
	return true;
}

bool vehicle_gate_courses(const std::vector<VehicleCourseReport> &p_reports, std::string *r_why) {
	std::string why;
	for (size_t k = 0; k < kVehicleCourseKindCount; ++k) {
		bool found = false;
		for (const VehicleCourseReport &r : p_reports) {
			found = found || (r.course.kind == VehicleCourseKind(k) && r.course.found);
		}
		if (!found) {
			why += std::string(why.empty() ? "" : ", ") + vehicle_course_name(VehicleCourseKind(k)) + " not found";
		}
	}
	if (r_why != nullptr) {
		*r_why = why;
	}
	return why.empty();
}

bool vehicle_gate_drove(const std::vector<VehicleCourseReport> &p_reports, std::string *r_why) {
	std::string why;
	for (const VehicleCourseReport &r : p_reports) {
		std::string w;
		if (!vehicle_course_drove(r, &w)) {
			why += (why.empty() ? "" : "; ") + w;
		}
	}
	if (r_why != nullptr) {
		*r_why = why;
	}
	return why.empty();
}

bool vehicle_gate_g1(const std::vector<VehicleCourseReport> &p_reports, std::string *r_why) {
	std::string why;
	size_t found = 0;
	for (const VehicleCourseReport &r : p_reports) {
		if (!r.course.found) {
			continue;
		}
		++found;
		const char *name = vehicle_course_name(r.course.kind);
		if (r.ticks == 0 || r.g1_ticks != r.ticks) {
			why += std::string(why.empty() ? "" : "; ") + name + ": " + std::to_string(r.g1_ticks) + " of " + std::to_string(r.ticks) + " ticks compared";
		} else if (r.g1_mismatches != 0) {
			why += std::string(why.empty() ? "" : "; ") + name + ": " + std::to_string(r.g1_mismatches) + " mismatching ticks, first " +
					std::to_string(r.g1_first.tick) + " " + r.g1_first.field;
		} else if (r.replay_runs == 0 || r.replay_mismatches != 0) {
			why += std::string(why.empty() ? "" : "; ") + name + ": G1r " + std::to_string(r.replay_mismatches) + " mismatching ticks over " +
					std::to_string(r.replay_runs) + " runs" + (r.replay_mismatches != 0 ? ", first " + std::to_string(r.replay_first.tick) + " " + r.replay_first.field : "");
		}
	}
	std::string drove;
	if (found == 0) {
		why = "no course found";
	} else if (!vehicle_gate_drove(p_reports, &drove)) {
		why += std::string(why.empty() ? "" : "; ") + "gate D failed (" + drove + ")";
	}
	if (r_why != nullptr) {
		*r_why = why;
	}
	return why.empty();
}

bool vehicle_gate_g2(const std::vector<VehicleCourseReport> &p_reports, std::string *r_why) {
	std::string why;
	size_t found = 0;
	char buffer[200];
	for (const VehicleCourseReport &r : p_reports) {
		if (!r.course.found) {
			continue;
		}
		++found;
		const char *name = vehicle_course_name(r.course.kind);
		size_t primary = 0;
		for (const VehicleCorrectionReport &c : r.corrections) {
			if (c.mode != VehicleRestoreMode::OwnHistory) {
				continue;
			}
			++primary;
			if (c.all.samples == 0 || !(c.all.max < kG2MaxError) || !(c.all.p99 < kG2MaxError)) {
				std::snprintf(buffer, sizeof(buffer), "%s R=%u: max %.4f m p99 %.4f m over %zu samples", name, c.replay_ticks, double(c.all.max), double(c.all.p99),
						c.all.samples);
				why += (why.empty() ? "" : "; ") + std::string(buffer);
			}
			if (r.course.kind == VehicleCourseKind::Collision && c.static_collision.samples < 30) {
				std::snprintf(buffer, sizeof(buffer), "%s R=%u: %zu static-collision samples (< 30)", name, c.replay_ticks, c.static_collision.samples);
				why += (why.empty() ? "" : "; ") + std::string(buffer);
			}
		}
		if (primary == 0) {
			why += std::string(why.empty() ? "" : "; ") + name + ": no OwnHistory result";
		}
	}
	std::string drove;
	if (found == 0) {
		why = "no course found";
	} else if (!vehicle_gate_drove(p_reports, &drove)) {
		why += std::string(why.empty() ? "" : "; ") + "gate D failed (" + drove + ")";
	}
	if (r_why != nullptr) {
		*r_why = why;
	}
	return why.empty();
}

core::Result<VehicleGroupReport> run_vehicle_group(std::shared_ptr<const StaticWorldShapes> p_shapes, std::shared_ptr<const VehicleModel> p_model,
		std::span<const VehicleCourse> p_courses, const VehicleReplayOptions &p_options) {
	const auto start = std::chrono::steady_clock::now();
	if (p_shapes == nullptr || p_model == nullptr) {
		return core::make_error("vehicle group: no shapes or model");
	}
	if (p_courses.size() < 2 || p_courses.size() > kMaxVehicleSlots) {
		return core::make_error("vehicle group: " + std::to_string(p_courses.size()) + " vehicles (2.." + std::to_string(kMaxVehicleSlots) + ")");
	}
	if (p_options.ticks == 0 || p_options.ticks > kMaxVehicleLogCommands) {
		return core::make_error("vehicle group: ticks outside 1.." + std::to_string(kMaxVehicleLogCommands));
	}
	Harness h;
	h.shapes = p_shapes;
	h.model = p_model;
	h.courses.assign(p_courses.begin(), p_courses.end());
	for (size_t i = 0; i < p_courses.size(); ++i) {
		h.slot_ids.push_back(VehicleSlot(i));
	}
	core::Result<std::unique_ptr<VehicleWorld>> server = h.make_world();
	BF3_TRY(server);
	Recording rec;
	BF3_TRY(h.record(**server, p_options.ticks, rec));
	VehicleGroupReport report;
	report.vehicles = uint32_t(p_courses.size());
	report.ticks = p_options.ticks;
	report.slot_distance.assign(p_courses.size(), 0.0f);
	for (uint32_t t = 1; t <= rec.ticks; ++t) {
		bool contact = false;
		for (size_t s = 0; s < rec.slots; ++s) {
			contact = contact || rec.contacts[t][s].chassis_vehicle_contacts > 0;
			report.slot_distance[s] += len3(sub3(rec.states[t][s].body.com_position, rec.states[t - 1][s].body.com_position));
		}
		report.ticks_vehicle_contact += contact ? 1u : 0u;
		report.slot0_ticks_asleep += rec.states[t][0].body.active == 0 ? 1u : 0u;
	}
	std::unique_ptr<VehicleWorld> prediction;
	BF3_TRY(h.check_g1(**server, rec, p_options, 0x51u, report.g1_ticks, report.g1_mismatches, report.g1_first, report.replay_runs, report.replay_ticks,
			report.replay_mismatches, report.replay_first, prediction));
	report.steps = h.steps;
	report.seconds = seconds_since(start);
	return report;
}

bool vehicle_gate_g1m(const VehicleGroupReport &p_report, std::string *r_why) {
	std::string why;
	if (p_report.ticks == 0 || p_report.g1_ticks != p_report.ticks) {
		why = std::to_string(p_report.g1_ticks) + " of " + std::to_string(p_report.ticks) + " ticks compared";
	} else if (p_report.g1_mismatches != 0) {
		why = std::to_string(p_report.g1_mismatches) + " mismatching ticks, first " + std::to_string(p_report.g1_first.tick) + " " + p_report.g1_first.field;
	} else if (p_report.replay_runs == 0 || p_report.replay_mismatches != 0) {
		why = "G1r " + std::to_string(p_report.replay_mismatches) + " mismatching ticks over " + std::to_string(p_report.replay_runs) + " runs" +
				(p_report.replay_mismatches != 0 ? ", first " + std::to_string(p_report.replay_first.tick) + " " + p_report.replay_first.field : "");
	} else if (p_report.ticks_vehicle_contact < 30) {
		why = std::to_string(p_report.ticks_vehicle_contact) + " ticks of chassis-chassis contact (< 30)";
	} else {
		for (size_t s = 1; s < p_report.slot_distance.size(); ++s) {
			if (!(p_report.slot_distance[s] >= 100.0f)) {
				why = "slot " + std::to_string(s) + " drove " + std::to_string(p_report.slot_distance[s]) + " m (< 100 m)";
				break;
			}
		}
	}
	if (r_why != nullptr) {
		*r_why = why;
	}
	return why.empty();
}

std::string format_vehicle_course_report(const VehicleCourseReport &p_report) {
	char line[512];
	std::string out;
	const VehicleCourse &c = p_report.course;
	std::snprintf(line, sizeof(line), "course %s: %s, seed %u, %zu queries%s\n", vehicle_course_name(c.kind), c.found ? "found" : "NOT FOUND", c.seed,
			c.queries, c.budget_exhausted ? " (budget exhausted)" : "");
	out += line;
	if (!c.found) {
		return out;
	}
	std::snprintf(line, sizeof(line), "  start (%.2f, %.2f, %.2f), a (%.1f, %.1f, %.1f), b (%.1f, %.1f, %.1f), slope %.1f deg, feature %.3f\n",
			double(c.start.position[0]), double(c.start.position[1]), double(c.start.position[2]), double(c.a[0]), double(c.a[1]), double(c.a[2]),
			double(c.b[0]), double(c.b[1]), double(c.b[2]), double(c.slope_degrees), double(c.feature));
	out += line;
	std::snprintf(line, sizeof(line),
			"  D %s%s%s: %.1f m, top %.1f km/h, wheel off %u ticks (all off %u), static contact %u ticks, %u impacts (max %.1f km/h), asleep %u ticks, "
			"%u yaw-rate sign changes, %.2f turns, creep %.4f m\n",
			p_report.drove ? "pass" : "FAIL", p_report.drove ? "" : " - ", p_report.drove_why.c_str(), double(p_report.distance), double(p_report.max_speed_kmh),
			p_report.ticks_any_wheel_off, p_report.ticks_all_wheels_off, p_report.ticks_static_contact, p_report.static_impacts,
			double(p_report.max_impact_speed_kmh), p_report.ticks_asleep, p_report.yaw_rate_sign_changes, double(p_report.yaw_turns), double(p_report.creep));
	out += line;
	std::snprintf(line, sizeof(line), "  G1 %u / %u ticks, %u mismatching%s%s; G1r %u runs, %u ticks, %u mismatching%s%s\n", p_report.g1_ticks, p_report.ticks,
			p_report.g1_mismatches, p_report.g1_mismatches != 0 ? " first at " : "",
			p_report.g1_mismatches != 0 ? (std::to_string(p_report.g1_first.tick) + " " + p_report.g1_first.field).c_str() : "", p_report.replay_runs,
			p_report.replay_ticks, p_report.replay_mismatches, p_report.replay_mismatches != 0 ? " first at " : "",
			p_report.replay_mismatches != 0 ? (std::to_string(p_report.replay_first.tick) + " " + p_report.replay_first.field).c_str() : "");
	out += line;
	for (const VehicleCorrectionReport &r : p_report.corrections) {
		std::snprintf(line, sizeof(line),
				"  G2 %-15s R=%u: all %zu max %.5f p99 %.5f mean %.5f (worst N %u) | wheel contact %zu max %.5f p99 %.5f | static collision %zu max %.5f "
				"p99 %.5f | max rot %.4f deg, max vel %.4f m/s\n",
				vehicle_restore_mode_name(r.mode), r.replay_ticks, r.all.samples, double(r.all.max), double(r.all.p99), double(r.all.mean), r.all.worst_tick,
				r.wheel_contact.samples, double(r.wheel_contact.max), double(r.wheel_contact.p99), r.static_collision.samples, double(r.static_collision.max),
				double(r.static_collision.p99), double(r.max_orientation_degrees), double(r.max_velocity_error));
		out += line;
	}
	std::snprintf(line, sizeof(line), "  snapshot %zu bytes, %llu world steps, %.2f s (shared machine: noisy)\n", p_report.snapshot_bytes,
			static_cast<unsigned long long>(p_report.steps), p_report.seconds);
	out += line;
	return out;
}

std::string format_vehicle_group_report(const VehicleGroupReport &p_report) {
	char line[512];
	std::string out;
	std::snprintf(line, sizeof(line), "group of %u: G1 %u / %u ticks, %u mismatching%s%s; G1r %u runs, %u ticks, %u mismatching%s%s\n", p_report.vehicles,
			p_report.g1_ticks, p_report.ticks, p_report.g1_mismatches, p_report.g1_mismatches != 0 ? " first at " : "",
			p_report.g1_mismatches != 0 ? (std::to_string(p_report.g1_first.tick) + " " + p_report.g1_first.field).c_str() : "", p_report.replay_runs,
			p_report.replay_ticks, p_report.replay_mismatches, p_report.replay_mismatches != 0 ? " first at " : "",
			p_report.replay_mismatches != 0 ? (std::to_string(p_report.replay_first.tick) + " " + p_report.replay_first.field).c_str() : "");
	out += line;
	std::snprintf(line, sizeof(line), "  chassis-chassis contact %u ticks, slot 0 asleep %u ticks, distances", p_report.ticks_vehicle_contact,
			p_report.slot0_ticks_asleep);
	out += line;
	for (const float d : p_report.slot_distance) {
		std::snprintf(line, sizeof(line), " %.1f", double(d));
		out += line;
	}
	std::snprintf(line, sizeof(line), " m; %llu world steps, %.2f s\n", static_cast<unsigned long long>(p_report.steps), p_report.seconds);
	out += line;
	return out;
}

// ---- handling numbers and the spawn settle ----------------------------------------------------------------------------

namespace {

struct Bench {
	std::unique_ptr<VehicleWorld> world;
	VehicleState state;
};

core::Result<Bench> bench(std::shared_ptr<const StaticWorldShapes> p_shapes, std::shared_ptr<const VehicleModel> p_model, const VehiclePlacement &p_at) {
	VehicleWorldOptions o;
	o.max_vehicles = 1;
	o.tuning = p_model->tuning;
	core::Result<std::unique_ptr<VehicleWorld>> w = VehicleWorld::create(std::move(p_shapes), o);
	BF3_TRY(w);
	BF3_TRY((*w)->add_vehicle(0, std::move(p_model), p_at));
	const VehicleSlot slot = 0;
	for (uint32_t t = 0; t < kSettleTicks; ++t) {
		BF3_TRY((*w)->step(std::span<const VehicleSlot>(&slot, 1), std::span<const VehicleCommand>(&kSettleCommand, 1)));
	}
	Bench b;
	b.world = std::move(*w);
	b.state = b.world->state(0);
	return b;
}

// Throttle p_throttle (or a held speed when p_hold > 0), steering towards +Z far ahead (or p_steer when set).
VehicleCommand bench_command(const VehicleState &p_state, int8_t p_throttle, float p_hold, int8_t p_steer, bool p_free_steer) {
	VehicleCommand c;
	const Vec3 f = forward_of(p_state.body.rotation);
	const float fs = p_state.body.linear_velocity[0] * f[0] + p_state.body.linear_velocity[1] * f[1] + p_state.body.linear_velocity[2] * f[2];
	if (p_hold > 0.0f) {
		const float err = p_hold - fs;
		if (err > 0.0f) {
			c.throttle = int8_t(std::lround(std::clamp(0.2f + err * 0.4f, 0.0f, 1.0f) * 127.0f));
		} else if (err < -0.5f) {
			c.brake = uint8_t(std::lround(std::clamp(-err * 0.3f, 0.0f, 1.0f) * 255.0f));
		} else {
			c.throttle = 1;
		}
	} else {
		c.throttle = p_throttle;
	}
	if (p_free_steer) {
		c.steer = p_steer;
	} else {
		// along x = 0 towards +Z: steer at a point 30 m ahead on that line (the project's right = (-f.z, 0, f.x))
		const float dx = 0.0f - p_state.body.origin[0];
		const float dz = 30.0f;
		const float lateral = dx * -f[2] + dz * f[0];
		const float along = dx * f[0] + dz * f[2];
		c.steer = int8_t(std::lround(std::clamp(2.5f * std::atan2(lateral, along), -1.0f, 1.0f) * 127.0f));
	}
	return c;
}

} // namespace

core::Result<VehicleHandling> measure_vehicle_handling(std::shared_ptr<const VehicleModel> p_model) {
	if (p_model == nullptr) {
		return core::make_error("vehicle handling: no model");
	}
	SyntheticVehicleGround g;
	g.half_size = 2048.0f;
	g.features = false;
	core::Result<std::shared_ptr<const StaticWorldShapes>> shapes = build_synthetic_vehicle_ground(g);
	BF3_TRY(shapes);
	VehicleHandling out;
	const VehicleSlot slot = 0;
	VehiclePlacement start;
	start.position = { 0.0f, 0.05f, -1950.0f };
	// Full throttle for 60 s: 0-60 km/h, the speed at 6 s, the top speed.
	{
		core::Result<Bench> b = bench(*shapes, p_model, start);
		BF3_TRY(b);
		VehicleState s = b->state;
		for (uint32_t t = 1; t <= 3600; ++t) {
			const VehicleCommand c = bench_command(s, 127, 0.0f, 0, false);
			BF3_TRY(b->world->step(std::span<const VehicleSlot>(&slot, 1), std::span<const VehicleCommand>(&c, 1)));
			s = b->world->state(0);
			const float kmh = len3(s.body.linear_velocity) * 3.6f;
			if (out.time_0_60_kmh == 0.0f && kmh >= 60.0f) {
				out.time_0_60_kmh = float(t) * kTickSeconds;
			}
			if (t == 360) {
				out.speed_at_6s_kmh = kmh;
			}
			out.top_speed_kmh = std::max(out.top_speed_kmh, kmh);
			if (s.body.origin[2] > 1950.0f) {
				break;
			}
		}
	}
	// Brake from a held 60 km/h to rest.
	{
		core::Result<Bench> b = bench(*shapes, p_model, start);
		BF3_TRY(b);
		VehicleState s = b->state;
		uint32_t held = 0;
		bool braking = false;
		Vec3 from{};
		float v0 = 0.0f;
		for (uint32_t t = 1; t <= 3600 * 2; ++t) {
			VehicleCommand c = bench_command(s, 0, 60.0f / 3.6f, 0, false);
			const float speed = len3(s.body.linear_velocity);
			if (!braking) {
				held = speed * 3.6f >= 59.0f ? held + 1 : 0;
				if (held >= 60) {
					braking = true;
					from = s.body.com_position;
					v0 = speed;
				}
			}
			if (braking) {
				c.throttle = 0;
				c.brake = 255;
				if (speed < 0.05f) {
					out.brake_distance_60_m = len3(sub3(s.body.com_position, from));
					out.brake_deceleration = out.brake_distance_60_m > 0.0f ? v0 * v0 / (2.0f * out.brake_distance_60_m) : 0.0f;
					break;
				}
			}
			BF3_TRY(b->world->step(std::span<const VehicleSlot>(&slot, 1), std::span<const VehicleCommand>(&c, 1)));
			s = b->world->state(0);
		}
	}
	// Full lock left at a held 15 km/h: the paths of the centre of mass and of the outer front wheel's contact.
	{
		core::Result<Bench> b = bench(*shapes, p_model, start);
		BF3_TRY(b);
		VehicleState s = b->state;
		size_t outer = 0; // the right front wheel (side 1, axle 0) is outside a left turn
		for (size_t i = 0; i < p_model->wheel_count; ++i) {
			if (p_model->wheels[i].axle == 0 && p_model->wheels[i].side == 1) {
				outer = i;
				break;
			}
		}
		Vec3 com_lo{ 1e9f, 0, 1e9f }, com_hi{ -1e9f, 0, -1e9f }, wheel_lo{ 1e9f, 0, 1e9f }, wheel_hi{ -1e9f, 0, -1e9f };
		for (uint32_t t = 1; t <= 1800; ++t) {
			const VehicleCommand c = bench_command(s, 0, 15.0f / 3.6f, -127, true);
			BF3_TRY(b->world->step(std::span<const VehicleSlot>(&slot, 1), std::span<const VehicleCommand>(&c, 1)));
			s = b->world->state(0);
			if (t > 900) {
				for (const size_t a : { size_t(0), size_t(2) }) {
					com_lo[a] = std::min(com_lo[a], s.body.com_position[a]);
					com_hi[a] = std::max(com_hi[a], s.body.com_position[a]);
					if (s.wheels[outer].in_contact != 0) {
						wheel_lo[a] = std::min(wheel_lo[a], s.wheels[outer].contact_position[a]);
						wheel_hi[a] = std::max(wheel_hi[a], s.wheels[outer].contact_position[a]);
					}
				}
			}
		}
		out.turning_circle_com_m = 0.5f * ((com_hi[0] - com_lo[0]) + (com_hi[2] - com_lo[2]));
		out.turning_circle_m = wheel_hi[0] > wheel_lo[0] ? 0.5f * ((wheel_hi[0] - wheel_lo[0]) + (wheel_hi[2] - wheel_lo[2])) : 0.0f;
	}
	return out;
}

core::Result<std::vector<VehicleSettleReport>> measure_vehicle_settle(std::shared_ptr<const StaticWorldShapes> p_shapes,
		std::shared_ptr<const VehicleModel> p_model, std::span<const VehiclePlacement> p_placements) {
	if (p_shapes == nullptr || p_model == nullptr) {
		return core::make_error("vehicle settle: no shapes or model");
	}
	std::vector<VehicleSettleReport> out;
	const VehicleSlot slot = 0;
	for (const VehiclePlacement &p : p_placements) {
		VehicleWorldOptions o;
		o.max_vehicles = 1;
		o.tuning = p_model->tuning;
		core::Result<std::unique_ptr<VehicleWorld>> w = VehicleWorld::create(p_shapes, o);
		BF3_TRY(w);
		BF3_TRY((*w)->add_vehicle(0, p_model, p));
		std::vector<Vec3> com;
		for (uint32_t t = 1; t <= kSettleTicks; ++t) {
			BF3_TRY((*w)->step(std::span<const VehicleSlot>(&slot, 1), std::span<const VehicleCommand>(&kSettleCommand, 1)));
			com.push_back((*w)->state(0).body.com_position);
		}
		const VehicleState end = (*w)->state(0);
		VehicleSettleReport r;
		r.spawn = p.position;
		r.drop = p.position[1] - end.body.origin[1];
		for (size_t t = kSettleTicks - kSettleDriftTicks; t < kSettleTicks; ++t) {
			const Vec3 d = sub3(com[t], com.back());
			r.drift_y = std::max(r.drift_y, std::fabs(d[1]));
			r.drift = std::max(r.drift, len3(d));
		}
		r.wheels_in_contact = (*w)->contacts(0).wheels_in_contact;
		r.settled = r.drift < kSettleDriftLimit && r.wheels_in_contact == p_model->wheel_count;
		out.push_back(r);
	}
	return out;
}

} // namespace bf3::sim
