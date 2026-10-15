// bf3dump vehicledrive / vehicle-replay (S2; docs/design/s2.md §1, §7.3, §8). Read-only on the install: the level
// is loaded through bf3_core, the static world built in memory (no cache), logs and hash files go where the caller
// says (scratch), never into the install.
#include "vehicle_commands.h"

#include "bf3/core/gameplay/vehicle_drive.h"
#include "bf3/core/gameplay/work_budget.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/vehicle_spawns.h"
#include "bf3/core/level/world.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/vehicle_course.h"
#include "bf3/sim/vehicle_ghost.h"
#include "bf3/sim/vehicle_model.h"
#include "bf3/sim/vehicle_replay.h"
#include "bf3/sim/vehicle_world.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace bf3::core;
using namespace bf3::sim;

namespace bf3dump {

namespace {

constexpr const char *kDefaultBlueprint = "vehicles/humveearmored/humveearmored";

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

bool option(const std::string &p_arg, const char *p_name, std::string &r_value) {
	const std::string prefix = std::string(p_name) + "=";
	if (p_arg.rfind(prefix, 0) != 0) {
		return false;
	}
	r_value = p_arg.substr(prefix.size());
	return true;
}

bool read_file(const std::string &p_path, std::string &r_text) {
	std::ifstream in(std::filesystem::path(p_path), std::ios::binary);
	if (!in) {
		return false;
	}
	std::ostringstream s;
	s << in.rdbuf();
	r_text = s.str();
	return true;
}

bool load_tuning(const std::string &p_path, VehicleTuning &r_tuning) {
	if (p_path.empty()) {
		r_tuning = VehicleTuning{};
		return true;
	}
	std::string text;
	if (!read_file(p_path, text)) {
		std::fprintf(stderr, "tuning: cannot read %s\n", p_path.c_str());
		return false;
	}
	Result<VehicleTuning> t = parse_vehicle_tuning(text);
	if (!t) {
		std::fprintf(stderr, "tuning %s: %s\n", p_path.c_str(), t.error().message.c_str());
		return false;
	}
	r_tuning = *t;
	return true;
}

bool parse_kinds(const std::string &p_list, std::array<bool, kVehicleCourseKindCount> &r_kinds) {
	r_kinds.fill(false);
	size_t at = 0;
	while (at <= p_list.size()) {
		const size_t comma = std::min(p_list.find(',', at), p_list.size());
		const std::string name = p_list.substr(at, comma - at);
		bool known = false;
		for (size_t k = 0; k < kVehicleCourseKindCount; ++k) {
			if (name == vehicle_course_name(VehicleCourseKind(k))) {
				r_kinds[k] = true;
				known = true;
			}
		}
		if (!known) {
			std::fprintf(stderr, "--courses: unknown kind `%s`\n", name.c_str());
			return false;
		}
		at = comma + 1;
	}
	return true;
}

void print_handling(const VehicleHandling &p_h) {
	std::printf("H (unverified placeholders for S5 Session 2): 0-60 km/h %.2f s, speed at 6 s %.1f km/h (IW4L target 68), braking from 60 km/h %.2f m "
				"at %.2f m/s^2 (IW4L ~10), turning circle %.2f m kerb to kerb (%.2f m CoM), top speed %.1f km/h\n",
			double(p_h.time_0_60_kmh), double(p_h.speed_at_6s_kmh), double(p_h.brake_distance_60_m), double(p_h.brake_deceleration),
			double(p_h.turning_circle_m), double(p_h.turning_circle_com_m), double(p_h.top_speed_kmh));
}

struct SweepRow {
	std::string name;
	VehicleGhostQuantisation q;
};

// s2.md §7.3: position step 1/1024, 1/256, 1/64 m; orientation 9, 11, 13 bits; velocity steps x1/4, x1, x4; no
// suspension. Beyond the design's rows (report only, for the G2 diagnosis): the gearbox timers and driver filters
// finer, and the controller inputs sent.
std::vector<SweepRow> sweep_rows() {
	std::vector<SweepRow> rows;
	rows.push_back({ "default", kDefaultVehicleGhost });
	VehicleGhostQuantisation q = kDefaultVehicleGhost;
	q.position_xz = { -4096.0f, 1.0f / 1024.0f, 23 };
	q.position_y = { -256.0f, 1.0f / 512.0f, 20 };
	rows.push_back({ "position 1/1024", q });
	q = kDefaultVehicleGhost;
	q.position_xz = { -4096.0f, 1.0f / 64.0f, 19 };
	q.position_y = { -256.0f, 1.0f / 32.0f, 16 };
	rows.push_back({ "position 1/64", q });
	q = kDefaultVehicleGhost;
	q.orientation_bits = 9;
	rows.push_back({ "orientation 9", q });
	q.orientation_bits = 13;
	rows.push_back({ "orientation 13", q });
	q = kDefaultVehicleGhost;
	q.linear_velocity = { -512.0f, 1.0f / 256.0f, 18 };
	q.angular_velocity = { -16.0f, 1.0f / 1024.0f, 15 };
	rows.push_back({ "velocity x1/4", q });
	q = kDefaultVehicleGhost;
	q.linear_velocity = { -512.0f, 1.0f / 16.0f, 14 };
	q.angular_velocity = { -16.0f, 1.0f / 64.0f, 11 };
	rows.push_back({ "velocity x4", q });
	q = kDefaultVehicleGhost;
	q.send_suspension = false;
	rows.push_back({ "no suspension", q });
	q = kDefaultVehicleGhost;
	q.wheel_spin = { -256.0f, 1.0f / 256.0f, 17 };
	q.engine_rpm = { 0.0f, 1.0f / 16.0f, 17 };
	q.clutch = { 0.0f, 1.0f / 4095.0f, 12 };
	q.gearbox_timer = { 0.0f, 1.0f / 4096.0f, 13 };
	q.driver_filter = { -1.0f, 1.0f / 32767.0f, 16 };
	rows.push_back({ "driver block x1/16", q });
	q = kDefaultVehicleGhost;
	q.send_controller_inputs = true;
	rows.push_back({ "controller inputs", q });
	q = kDefaultVehicleGhost;
	q.position_xz = { -4096.0f, 1.0f / 1024.0f, 23 };
	q.position_y = { -256.0f, 1.0f / 512.0f, 20 };
	q.orientation_bits = 13;
	q.linear_velocity = { -512.0f, 1.0f / 256.0f, 18 };
	q.angular_velocity = { -16.0f, 1.0f / 1024.0f, 15 };
	rows.push_back({ "all finer (pos 1/1024, ori 13, vel x1/4)", q });
	return rows;
}

// Replays p_log open loop from the course start (settled as the harness does); the server hashes per tick 0..n.
Result<std::vector<uint64_t>> replay_log(std::shared_ptr<const StaticWorldShapes> p_shapes, std::shared_ptr<const VehicleModel> p_model,
		const VehicleCourse &p_course, const VehicleCommandLog &p_log) {
	VehicleWorldOptions o;
	o.max_vehicles = 1;
	o.tuning = p_model->tuning;
	Result<std::unique_ptr<VehicleWorld>> w = VehicleWorld::create(std::move(p_shapes), o);
	BF3_TRY(w);
	BF3_TRY((*w)->add_vehicle(0, p_model, p_course.start));
	const VehicleSlot slot = 0;
	for (uint32_t t = 0; t < kSettleTicks; ++t) {
		BF3_TRY((*w)->step(std::span<const VehicleSlot>(&slot, 1), std::span<const VehicleCommand>(&kSettleCommand, 1)));
	}
	std::vector<uint64_t> hashes{ (*w)->state_hash() };
	for (const VehicleCommand &c : p_log.commands) {
		BF3_TRY((*w)->step(std::span<const VehicleSlot>(&slot, 1), std::span<const VehicleCommand>(&c, 1)));
		hashes.push_back((*w)->state_hash());
	}
	return hashes;
}

} // namespace

const char *vehicle_usage() {
	return "  vehicledrive <LEVEL> <MODE> [<blueprint>] [--tuning=<cfg>]\n"
		   "  vehicle-replay <LEVEL> <MODE> [--synthetic] [--courses=a,b] [--modes=<hex>] [--ticks=N] [--no-g1] [--no-g2] [--handling] [--sweep]\n"
		   "                 [--hash-out=<file>] [--log-out=<dir>] [--log-in=<file>] [--tuning=<cfg>]\n"
		   "                 [--search-radius=m] [--turnaround=m] [--flat=deg]\n";
}

int cmd_vehicledrive(const std::shared_ptr<Install> &p_install, const std::vector<std::string> &p_args) {
	std::vector<std::string> positional;
	std::string tuning_path;
	for (const std::string &a : p_args) {
		if (!option(a, "--tuning", tuning_path)) {
			positional.push_back(a);
		}
	}
	if (positional.size() < 2 || positional.size() > 3) {
		std::fprintf(stderr, "%s", vehicle_usage());
		return 2;
	}
	VehicleTuning tuning;
	if (!load_tuning(tuning_path, tuning)) {
		return 1;
	}
	const std::string blueprint = positional.size() == 3 ? positional[2] : kDefaultBlueprint;
	const auto start = std::chrono::steady_clock::now();
	Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(p_install, positional[0], positional[1]);
	if (!loaded) {
		std::fprintf(stderr, "vehicledrive: %s\n", loaded.error().message.c_str());
		return 1;
	}
	gameplay::WorkBudget budget;
	Result<gameplay::VehicleDriveDef> def = gameplay::read_vehicle_drive_def((*loaded)->db(), blueprint, &budget);
	if (!def) {
		std::fprintf(stderr, "vehicledrive %s: %s\n", blueprint.c_str(), def.error().message.c_str());
		return 1;
	}
	const gameplay::VehicleDriveDef &d = *def;
	std::printf("blueprint\t%s\ncomponents\t%zu\nphysics_asset\t%s x %g\n", d.blueprint.c_str(), d.components, d.physics_asset.c_str(), double(d.physics_asset_scale));
	const gameplay::ChassisConfigDef &c = d.chassis;
	std::printf("chassis\tmass %g gravity %g com (%g %g %g) inertia_mod (%g %g %g) drag %g x %g x %g anti_roll %g / %g stand_still %d %g s gearbox %d "
				"neg_throttle %d\n",
			double(c.body_mass), double(c.gravity_modifier), double(c.center_of_mass[0]), double(c.center_of_mass[1]), double(c.center_of_mass[2]),
			double(c.inertia_modifier[0]), double(c.inertia_modifier[1]), double(c.inertia_modifier[2]), double(c.air_density), double(c.air_drag_area),
			double(c.coefficient_of_air_friction), double(c.anti_roll_front), double(c.anti_roll_rear), int(c.use_stand_still_brake),
			double(c.stand_still_low_speed_time_limit), int(c.use_gearbox), int(c.allow_negative_throttle));
	for (const gameplay::VehicleWheelDef &w : d.wheels) {
		std::printf("wheel\t%s\tconfig %u\tat (%g %g %g)\n", w.physics_type.c_str(), w.config, double(w.transform.trans[0]), double(w.transform.trans[1]),
				double(w.transform.trans[2]));
	}
	for (const gameplay::WheelConfigDef &w : d.wheel_configs) {
		std::printf("wheel_config\tradius %g width %g mass %g spring %g / %g / %g brake %g diff %g drive %d steer %d sensitivity %zu points\n", double(w.radius),
				double(w.width), double(w.mass), double(w.spring_length), double(w.spring_stiffness), double(w.spring_damping), double(w.brake_force),
				double(w.diff_gear_ratio), w.driving_type, w.steering_type, w.steering_sensitivity.size());
	}
	for (const gameplay::EngineDef &e : d.engines) {
		std::printf("engine\t%s\t%zu points\trpm %g..%g\n", e.config_type.c_str(), e.rpm_points.size(), double(e.rpm_min), double(e.rpm_max));
	}
	std::printf("gearbox\t%zu forward, %zu reverse, present %d\n", d.gearbox.forward_ratios.size(), d.gearbox.reverse_ratios.size(), int(d.gearbox.present));
	Result<std::shared_ptr<const VehicleModel>> model = load_vehicle_model((*loaded)->db(), (*loaded)->mount(), blueprint, tuning);
	if (!model) {
		std::fprintf(stderr, "vehicledrive model: %s\n", model.error().message.c_str());
		return 1;
	}
	const VehicleModel &m = **model;
	std::printf("model\tdigest %016llx\tpieces %zu\twheels %u\tmass %g\n", (unsigned long long)m.model_digest, m.chassis.size(), unsigned(m.wheel_count),
			double(m.mass));
	for (size_t i = 0; i < m.wheel_count; ++i) {
		const VehicleWheelModel &w = m.wheels[i];
		std::printf("model_wheel\t%zu\tattach (%.4f %.4f %.4f) r %.3f susp %.3f..%.3f k %.1f c %.1f steer %.4f brake %.1f hand %.1f axle %u side %u\n", i,
				double(w.attachment[0]), double(w.attachment[1]), double(w.attachment[2]), double(w.radius), double(w.suspension_min), double(w.suspension_max),
				double(w.spring_stiffness), double(w.spring_damping), double(w.max_steer_angle), double(w.max_brake_torque), double(w.max_hand_brake_torque),
				unsigned(w.axle), unsigned(w.side));
	}
	std::fprintf(stderr, "vehicledrive: %zu budget units, %.2f s\n", size_t(budget.used()), seconds_since(start));
	return 0;
}

int cmd_vehicle_replay(const std::shared_ptr<Install> &p_install, const std::vector<std::string> &p_args) {
	std::vector<std::string> positional;
	std::string tuning_path, hash_out, log_out, log_in, courses_arg, value;
	bool synthetic = false, handling = false, sweep = false;
	uint32_t trace = 0;
	VehicleReplayOptions o;
	VehicleCourseSearch search;
	for (const std::string &a : p_args) {
		if (option(a, "--trace", value)) {
			const auto r = std::from_chars(value.data(), value.data() + value.size(), trace);
			if (r.ec != std::errc() || r.ptr != value.data() + value.size() || trace == 0) {
				std::fprintf(stderr, "--trace: ticks between rows, >= 1\n");
				return 2;
			}
		} else if (a == "--synthetic") {
			synthetic = true;
		} else if (a == "--handling") {
			handling = true;
		} else if (a == "--sweep") {
			sweep = true;
		} else if (a == "--no-g1") {
			o.g1 = false;
		} else if (a == "--no-g2") {
			o.g2 = false;
		} else if (option(a, "--tuning", tuning_path) || option(a, "--hash-out", hash_out) || option(a, "--log-out", log_out) ||
				option(a, "--log-in", log_in) || option(a, "--courses", courses_arg)) {
		} else if (option(a, "--modes", value)) {
			unsigned m = 0;
			const auto r = std::from_chars(value.data(), value.data() + value.size(), m, 16);
			if (r.ec != std::errc() || r.ptr != value.data() + value.size() || m == 0 || m > 0x3Fu) {
				std::fprintf(stderr, "--modes: a hex mask 1..3f\n");
				return 2;
			}
			o.modes = m;
		} else if (option(a, "--search-radius", value) || option(a, "--turnaround", value) || option(a, "--flat", value)) {
			float f = 0.0f;
			const auto r = std::from_chars(value.data(), value.data() + value.size(), f);
			if (r.ec != std::errc() || r.ptr != value.data() + value.size() || !(f > 0.0f && f <= 5000.0f)) {
				std::fprintf(stderr, "%s: a number in (0, 5000]\n", a.c_str());
				return 2;
			}
			(a.rfind("--search-radius", 0) == 0 ? search.radius : (a.rfind("--turnaround", 0) == 0 ? search.turnaround_radius : search.max_flat_degrees)) = f;
		} else if (option(a, "--ticks", value)) {
			const auto r = std::from_chars(value.data(), value.data() + value.size(), o.ticks);
			if (r.ec != std::errc() || r.ptr != value.data() + value.size() || o.ticks < 10 || o.ticks > 360000) {
				std::fprintf(stderr, "--ticks: 10..360000\n");
				return 2;
			}
		} else if (a.rfind("--", 0) == 0) {
			std::fprintf(stderr, "vehicle-replay: unknown option %s\n%s", a.c_str(), vehicle_usage());
			return 2;
		} else {
			positional.push_back(a);
		}
	}
	if (positional.size() != 2) {
		std::fprintf(stderr, "%s", vehicle_usage());
		return 2;
	}
	std::array<bool, kVehicleCourseKindCount> kinds;
	kinds.fill(true);
	if (!courses_arg.empty() && !parse_kinds(courses_arg, kinds)) {
		return 2;
	}
	VehicleTuning tuning;
	if (!load_tuning(tuning_path, tuning)) {
		return 1;
	}
	const std::string &lv = positional[0], &mode = positional[1];
	auto start = std::chrono::steady_clock::now();
	Result<level::LevelCollision> collision = level::load_level_collision(p_install, lv, mode);
	if (!collision) {
		std::fprintf(stderr, "vehicle-replay %s %s: %s\n", lv.c_str(), mode.c_str(), collision.error().message.c_str());
		return 1;
	}
	Result<std::shared_ptr<const VehicleModel>> model = load_vehicle_model(collision->level->db(), collision->level->mount(), kDefaultBlueprint, tuning);
	if (!model) {
		std::fprintf(stderr, "vehicle-replay model: %s\n", model.error().message.c_str());
		return 1;
	}
	std::printf("model %s digest %016llx, tuning hash %016llx; level loaded in %.2f s (noisy)\n", kDefaultBlueprint, (unsigned long long)(*model)->model_digest,
			(unsigned long long)vehicle_tuning_hash(tuning), seconds_since(start));
	if (handling) {
		Result<VehicleHandling> h = measure_vehicle_handling(*model);
		if (!h) {
			std::fprintf(stderr, "handling: %s\n", h.error().message.c_str());
			return 1;
		}
		print_handling(*h);
		return 0;
	}
	std::shared_ptr<const StaticWorldShapes> shapes;
	std::array<VehicleCourse, kVehicleCourseKindCount> courses;
	if (synthetic) {
		Result<std::shared_ptr<const StaticWorldShapes>> s = build_synthetic_vehicle_ground();
		if (!s) {
			std::fprintf(stderr, "synthetic ground: %s\n", s.error().message.c_str());
			return 1;
		}
		shapes = *s;
		courses = synthetic_vehicle_courses();
	} else {
		start = std::chrono::steady_clock::now();
		Result<std::shared_ptr<const StaticWorldShapes>> s = build_static_world(collision->soup);
		if (!s) {
			std::fprintf(stderr, "static world: %s\n", s.error().message.c_str());
			return 1;
		}
		shapes = *s;
		Result<std::unique_ptr<CollisionWorld>> cw = CollisionWorld::create(shapes);
		if (!cw) {
			std::fprintf(stderr, "collision world: %s\n", cw.error().message.c_str());
			return 1;
		}
		Result<std::vector<level::VehicleSpawnerDef>> spawners = level::read_vehicle_spawners(*collision->world);
		if (!spawners) {
			std::fprintf(stderr, "spawners: %s\n", spawners.error().message.c_str());
			return 1;
		}
		std::vector<Vec3> seeds;
		for (const level::VehicleSpawnerDef &sp : *spawners) {
			if (sp.blueprint == kDefaultBlueprint) {
				seeds.push_back(sp.transform.trans);
			}
		}
		Result<std::array<VehicleCourse, kVehicleCourseKindCount>> found = find_vehicle_courses(**cw, seeds, **model, search);
		if (!found) {
			std::fprintf(stderr, "course search: %s\n", found.error().message.c_str());
			return 1;
		}
		courses = *found;
		std::printf("static world and course search: %.1f s (noisy), %zu seeds\n", seconds_since(start), seeds.size());
		for (const VehicleCourse &c : courses) {
			std::printf("C %-10s %s: %zu queries%s\n", vehicle_course_name(c.kind), c.found ? "found" : "NOT FOUND", c.queries,
					c.budget_exhausted ? " (budget exhausted)" : "");
		}
	}
	std::ofstream hashes;
	if (!hash_out.empty()) {
		hashes.open(std::filesystem::path(hash_out), std::ios::binary);
		if (!hashes) {
			std::fprintf(stderr, "--hash-out: cannot write %s\n", hash_out.c_str());
			return 1;
		}
	}
	const auto write_hashes = [&](const char *p_name, const std::vector<uint64_t> &p_hashes) {
		if (hashes.is_open()) {
			hashes << "# course " << p_name << "\n";
			char line[24];
			for (const uint64_t h : p_hashes) {
				std::snprintf(line, sizeof(line), "%016llx\n", (unsigned long long)h);
				hashes << line;
			}
		}
	};
	int rc = 0;
	if (!log_in.empty()) {
		size_t chosen = kVehicleCourseKindCount, count = 0;
		for (size_t k = 0; k < kVehicleCourseKindCount; ++k) {
			if (kinds[k] && !courses_arg.empty()) {
				chosen = k;
				++count;
			}
		}
		if (count != 1 || !courses[chosen].found) {
			std::fprintf(stderr, "--log-in needs --courses=<one found kind>\n");
			rc = 2;
		} else {
			std::string text;
			Result<VehicleCommandLog> log = read_file(log_in, text) ? parse_vehicle_command_log(text) : Result<VehicleCommandLog>(make_error("cannot read " + log_in));
			Result<std::vector<uint64_t>> h = log ? replay_log(shapes, *model, courses[chosen], *log) : Result<std::vector<uint64_t>>(log.error());
			if (!h) {
				std::fprintf(stderr, "--log-in: %s\n", h.error().message.c_str());
				rc = 1;
			} else {
				std::printf("log %s on %s: %zu commands, final state_hash %016llx\n", log_in.c_str(), vehicle_course_name(courses[chosen].kind),
						log->commands.size(), (unsigned long long)h->back());
				write_hashes(vehicle_course_name(courses[chosen].kind), *h);
			}
		}
	} else if (trace > 0) {
		// The server run of each selected course, closed loop as the harness records it, one row every --trace ticks.
		for (const VehicleCourse &c : courses) {
			if (!c.found || !kinds[size_t(c.kind)]) {
				continue;
			}
			VehicleWorldOptions wo;
			wo.max_vehicles = 1;
			wo.tuning = (*model)->tuning;
			Result<std::unique_ptr<VehicleWorld>> w = VehicleWorld::create(shapes, wo);
			if (!w || !(*w)->add_vehicle(0, *model, c.start).ok()) {
				std::fprintf(stderr, "trace: cannot build the world\n");
				return 1;
			}
			const VehicleSlot slot = 0;
			for (uint32_t t = 0; t < kSettleTicks; ++t) {
				(void)(*w)->step(std::span<const VehicleSlot>(&slot, 1), std::span<const VehicleCommand>(&kSettleCommand, 1));
			}
			VehicleScriptMemory memory;
			std::printf("# trace %s: tick phase throttle brake steer x y z kmh wheels chassis_static active gear rpm\n", vehicle_course_name(c.kind));
			for (Tick t = 1; t <= o.ticks; ++t) {
				const VehicleCommand cmd = drive_vehicle_course(c, **model, (*w)->state(0), t, memory);
				if (!(*w)->step(std::span<const VehicleSlot>(&slot, 1), std::span<const VehicleCommand>(&cmd, 1)).ok()) {
					std::fprintf(stderr, "trace: step failed\n");
					return 1;
				}
				const VehicleState s = (*w)->state(0);
				const VehicleContacts k = (*w)->contacts(0);
				if (t % trace == 0 || k.chassis_static_contacts > 0) {
					const float kmh = std::sqrt(s.body.linear_velocity[0] * s.body.linear_velocity[0] + s.body.linear_velocity[1] * s.body.linear_velocity[1] +
												s.body.linear_velocity[2] * s.body.linear_velocity[2]) *
							3.6f;
					std::printf("%u %u %d %u %d %.2f %.2f %.2f %.1f %u %u %u %d %.0f\n", t, unsigned(memory.phase), int(cmd.throttle), unsigned(cmd.brake), int(cmd.steer),
							double(s.body.origin[0]), double(s.body.origin[1]), double(s.body.origin[2]), double(kmh), unsigned(k.wheels_in_contact),
							unsigned(k.chassis_static_contacts), unsigned(s.body.active), s.controller.gear, double(s.controller.engine_rpm));
				}
			}
		}
	} else if (sweep) {
		VehicleReplayOptions so = o;
		so.g1 = false;
		so.g2 = true;
		so.modes = 1u << uint32_t(VehicleRestoreMode::OwnHistory);
		so.keep_hashes = false;
		for (const SweepRow &row : sweep_rows()) {
			so.quantisation = row.q;
			std::printf("sweep %-42s %4zu bits:", row.name.c_str(), vehicle_ghost_bits(row.q, (*model)->wheel_count));
			for (const VehicleCourse &c : courses) {
				if (!c.found || !kinds[size_t(c.kind)]) {
					continue;
				}
				Result<VehicleCourseReport> r = run_vehicle_course(shapes, *model, c, so);
				if (!r) {
					std::fprintf(stderr, "\nsweep %s: %s\n", vehicle_course_name(c.kind), r.error().message.c_str());
					return 1;
				}
				std::printf(" | %s", vehicle_course_name(c.kind));
				for (const VehicleCorrectionReport &x : r->corrections) {
					std::printf(" R%u %.4f/%.4f", x.replay_ticks, double(x.all.max), double(x.all.p99));
				}
				std::fflush(stdout);
			}
			std::printf("\n");
		}
	} else {
		std::vector<VehicleCourseReport> reports;
		uint64_t steps = 0;
		start = std::chrono::steady_clock::now();
		for (const VehicleCourse &c : courses) {
			if (!kinds[size_t(c.kind)]) {
				continue;
			}
			VehicleCourseReport r;
			r.course = c;
			if (c.found) {
				Result<VehicleCourseReport> run = run_vehicle_course(shapes, *model, c, o);
				if (!run) {
					std::fprintf(stderr, "course %s: %s\n", vehicle_course_name(c.kind), run.error().message.c_str());
					return 1;
				}
				r = std::move(*run);
				steps += r.steps;
				write_hashes(vehicle_course_name(c.kind), r.server_hashes);
				if (!log_out.empty()) {
					const std::filesystem::path dir(log_out);
					std::error_code ec;
					std::filesystem::create_directories(dir, ec);
					std::ofstream out(dir / (std::string(vehicle_course_name(c.kind)) + ".vlog"), std::ios::binary);
					out << format_vehicle_command_log(r.log);
				}
			}
			std::printf("%s", format_vehicle_course_report(r).c_str());
			std::fflush(stdout);
			reports.push_back(std::move(r));
		}
		std::printf("%llu steps in %.1f s (noisy)\n", (unsigned long long)steps, seconds_since(start));
		const auto gate = [&](const char *p_name, bool p_pass, const std::string &p_why) {
			std::printf("gate %-3s %s%s%s\n", p_name, p_pass ? "PASS" : "FAIL", p_why.empty() ? "" : ": ", p_why.c_str());
			if (!p_pass) {
				rc = 1;
			}
		};
		std::string why;
		if (courses_arg.empty()) {
			bool pass = vehicle_gate_courses(reports, &why);
			gate("C", pass, why);
		}
		why.clear();
		bool pass = vehicle_gate_drove(reports, &why);
		gate("D", pass, why);
		if (o.g1) {
			why.clear();
			pass = vehicle_gate_g1(reports, &why);
			gate("G1", pass, why);
		}
		if (o.g2) {
			why.clear();
			pass = vehicle_gate_g2(reports, &why);
			gate("G2", pass, why);
		}
	}
	return rc;
}

} // namespace bf3dump
