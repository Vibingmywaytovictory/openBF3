#include "weapon_sim_commands.h"

#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/level/spawn_points.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/command.h"
#include "bf3/sim/spawn.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/weapons/ballistics.h"
#include "bf3/sim/weapons/damage.h"
#include "bf3/sim/weapons/weapon_install.h"
#include "bf3/sim_harness/shooting_range.h"
#include "bf3/sim_harness/weapon_install_checks.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::sim;
using namespace bf3::sim::weapons;

namespace bf3dump {

namespace {

using bf3::core::seconds_since;

double ms(SimTime p_t) {
	return units_to_seconds(p_t) * 1000.0;
}

bool install_unchanged(const EbxContext &p_ctx, const InstallSnapshot &p_before) {
	if (p_ctx.reader == nullptr) {
		std::printf("install.unchanged unknown (no reader)\n");
		return true;
	}
	const SnapshotDiff diff = diff_snapshots(p_before, p_ctx.reader->snapshot());
	const bool clean = p_before.valid && diff.clean();
	std::printf("install.unchanged %s\n", clean ? ("yes (" + std::to_string(p_before.files.size()) + " files: sizes and mtimes equal)").c_str()
												: ("NO: " + diff.summary()).c_str());
	return clean;
}

bool parse_u64(const std::string &p_s, uint64_t &r_v) {
	const auto r = std::from_chars(p_s.data(), p_s.data() + p_s.size(), r_v);
	return r.ec == std::errc() && r.ptr == p_s.data() + p_s.size();
}

bool parse_float(const std::string &p_s, float &r_v) {
	const auto r = std::from_chars(p_s.data(), p_s.data() + p_s.size(), r_v);
	return r.ec == std::errc() && r.ptr == p_s.data() + p_s.size() && std::isfinite(r_v);
}

const char *logic_name(FireLogic p_l) {
	return bf3::core::gameplay::fire_logic_name(p_l);
}

FireScript script_for(const WeaponSimDef &p_d) {
	switch (p_d.modes[0].logic) {
		case FireLogic::Automatic:
			return FireScript::Hold;
		case FireLogic::HoldAndRelease:
			return FireScript::Throw;
		default:
			return FireScript::Tap;
	}
}

std::optional<FireTimingResult> timing(const WeaponTable &p_table, FireScript p_script, uint32_t p_ticks, uint64_t p_seed, float p_eye, uint32_t p_reload_tick = 0) {
	RangeScenario s;
	s.weapon = 0;
	s.script = p_script;
	s.ticks = p_ticks;
	s.settle_ticks = 0;
	s.match_seed = p_seed;
	s.reload_tick = p_reload_tick;
	s.hold_ticks = 40;
	Result<FireTimingResult> r = run_fire_timing(p_table, s, p_eye);
	if (!r) {
		std::fprintf(stderr, "fire timing: %s\n", r.error().message.c_str());
		return std::nullopt;
	}
	return std::move(*r);
}

void print_reload(const char *p_name, const FireTimingResult &p_t) {
	SimTime started = -1, committed = -1, finished = -1;
	std::vector<SimTime> shells;
	for (const TimedWeaponEvent &e : p_t.timed) {
		if (e.event == weapon_events::kReloadStarted && started < 0) {
			started = e.time;
		} else if (started >= 0 && e.event == weapon_events::kReloadCommitted && committed < 0) {
			committed = e.time;
		} else if (started >= 0 && e.event == weapon_events::kShellLoaded && finished < 0) {
			shells.push_back(e.time - started);
		} else if (started >= 0 && e.event == weapon_events::kReloadFinished && finished < 0) {
			finished = e.time;
		}
	}
	if (started < 0) {
		std::printf("reload\t%s\tnone in the run\n", p_name);
		return;
	}
	std::printf("reload\t%s\tstarted %" PRId64 "\tcommitted +%" PRId64 "\tfinished +%" PRId64 " (%.4f s)", p_name, started, committed >= 0 ? committed - started : -1,
			finished >= 0 ? finished - started : -1, finished >= 0 ? units_to_seconds(finished - started) : -1.0);
	if (!shells.empty()) {
		std::printf("\tshells");
		for (const SimTime s : shells) {
			std::printf(" +%" PRId64, s);
		}
	}
	std::printf("\n");
}

int weapon_sim_one(const EbxContext &p_ctx, const InstallWeapons &p_w, const std::string &p_partition, const std::vector<std::string> &p_unlocks, uint64_t p_seed,
		float p_seconds, const InstallSnapshot &p_before) {
	Result<bf3::core::gameplay::WeaponDef> def = p_w.catalog->resolve(p_partition, p_unlocks);
	if (!def) {
		std::fprintf(stderr, "weapon-sim %s: %s\n", p_partition.c_str(), def.error().message.c_str());
		return 1;
	}
	const std::vector<bf3::core::gameplay::WeaponDef> defs{ *def };
	Result<std::shared_ptr<const WeaponTable>> table = make_weapon_table(defs, p_w.materials, install_table_options(p_w));
	Result<std::shared_ptr<const WeaponTable>> oracle = make_weapon_table(defs, p_w.materials, install_table_options(p_w, {}, true));
	if (!table || !oracle) {
		std::fprintf(stderr, "weapon-sim %s: %s\n", p_partition.c_str(), (!table ? table.error() : oracle.error()).message.c_str());
		return 1;
	}
	const WeaponTable &t = **table;
	const WeaponSimDef &d = t.defs[0];
	const ProjectileSimDef &p = d.projectile;
	const Mechanics &m = t.mechanics;
	const char *unm = m.measured ? "" : "  unmeasured";
	std::printf("weapon-sim\t%s\tclass %s\ttable hash %016" PRIx64 "\n", d.variant.c_str(), d.weapon_class.c_str(), t.hash);
	for (uint8_t i = 0; i < d.mode_count; ++i) {
		const FireModeDef &f = d.modes[i];
		std::printf("mode\t%u\t%s\tinterval %" PRId64 " (%.3f ms, %.2f RPM)", unsigned(i), logic_name(f.logic), f.interval, ms(f.interval), sim_effective_rpm(d, i));
		if (f.logic == FireLogic::Burst) {
			std::printf("\tburst %d x %" PRId64 " (%.3f ms)", f.bullets_per_burst, f.burst_interval, ms(f.burst_interval));
		}
		std::printf("\n");
	}
	bool bolt_mode = false;
	for (uint8_t i = 0; i < d.mode_count; ++i) {
		bolt_mode = bolt_mode || d.modes[i].logic == FireLogic::SingleWithBoltAction;
	}
	if (bolt_mode) {
		std::printf("bolt\tcycle %" PRId64 " (%.4f s)\thold until fire release %d\n", d.bolt_cycle, units_to_seconds(d.bolt_cycle),
				d.bolt_hold_until_fire_release ? 1 : 0);
	}
	std::printf("ammo\tmagazine %d\tmagazines %d\tunlimited %d\tpellets %d\ttrace every %d\n", d.magazine_capacity, d.magazines, d.unlimited_ammo ? 1 : 0, d.pellets,
			d.trace_frequency);
	std::printf("reload-def\t%s\ttactical %" PRId64 "\tempty %" PRId64 "\tdelay %" PRId64 "\tthreshold %g\tcommit %" PRId64 " / %" PRId64 "\tswitch cancels %d%s\n",
			bf3::core::gameplay::reload_type_name(d.reload_type), d.reload_tactical, d.reload_empty, d.reload_delay, double(d.reload_threshold), d.reload_commit_tactical,
			d.reload_commit_empty, d.switch_cancels_reload ? 1 : 0, unm);
	std::printf("deploy\t%" PRId64 " (%.3f s)\tspawn delay %" PRId64 "\n", d.deploy_time, units_to_seconds(d.deploy_time), d.spawn_delay);
	std::printf("projectile\t%s\tv0 %g\tlaunch (%g, %g, %g) from (%g, %g, %g)\tg %g\tttl %" PRId64 "\tdamage %g -> %g over %g..%g m\tproperty %u\tzones %g %g %g %g\n",
			bf3::core::gameplay::projectile_kind_name(p.kind), double(p.muzzle_speed), double(p.launch_velocity[0]), double(p.launch_velocity[1]),
			double(p.launch_velocity[2]), double(p.launch_offset[0]), double(p.launch_offset[1]), double(p.launch_offset[2]), double(p.gravity), p.time_to_live,
			double(p.start_damage), double(p.end_damage), double(p.falloff_start), double(p.falloff_end), unsigned(p.property),
			double(p.zone[0].protection_multiplier), double(p.zone[1].protection_multiplier), double(p.zone[2].protection_multiplier),
			double(p.zone[3].protection_multiplier));
	for (const auto &[name, x] : { std::pair<const char *, const ExplosionSimDef &>{ "explosion", p.explosion }, { "dud", p.dud } }) {
		if (x.present) {
			std::printf("%s\t%g / inner %g / radius %g\tzones %g %g %g %g (%s)%s\n", name, double(x.blast_damage), double(x.inner_radius), double(x.radius),
					double(x.zone_multiplier[0]), double(x.zone_multiplier[1]), double(x.zone_multiplier[2]), double(x.zone_multiplier[3]),
					x.zone_multiplier_from_data ? "data" : "default", unm);
		}
	}
	if (p.time_to_arm > 0 || p.direct_damage > 0) {
		std::printf("explosive\tarm %" PRId64 "\tdirect %g\tengine %g m/s2 to %g m/s\n", p.time_to_arm, double(p.direct_damage), double(p.engine_acceleration),
				double(p.max_speed));
	}

	const float eye = p_w.stand_eye_height;
	const uint32_t ticks = uint32_t(std::clamp(p_seconds, 0.1f, 600.0f) * float(kSimHz));
	const FireScript script = script_for(d);
	if (const std::optional<FireTimingResult> f = timing(t, script, ticks, p_seed, eye)) {
		std::printf("fire\tscript %s\t%zu shots in %.2f s (real magazine)", script == FireScript::Hold ? "hold" : (script == FireScript::Throw ? "throw" : "tap"),
				f->shots.size(), double(ticks) / double(kSimHz));
		if (const std::optional<FireTimingResult> o = timing(**oracle, script, ticks, p_seed, eye)) {
			std::printf("\t%zu with infinite_magazine", o->shots.size());
		}
		std::printf("\n");
		SimTime lo = INT64_MAX, hi = 0;
		for (size_t i = 0; i < f->shots.size(); ++i) {
			const ShotEvent &s = f->shots[i];
			const SimTime gap = i > 0 ? s.time - f->shots[i - 1].time : 0;
			if (i > 0) {
				lo = std::min(lo, gap);
				hi = std::max(hi, gap);
			}
			if (i < 20) {
				std::printf("shot\t%zu\t%" PRId64 "\t%.3f ms\tgap %" PRId64 "\tspread %.3f\tpellets %u\n", i, s.time, ms(s.time), gap, double(s.spread), unsigned(s.pellets));
			}
		}
		if (f->shots.size() > 1) {
			std::printf("gaps\tmin %" PRId64 " (%.3f ms)\tmax %" PRId64 " (%.3f ms)\n", lo, ms(lo), hi, ms(hi));
		}
		size_t bolts = 0;
		SimTime bolt_start = -1;
		for (const TimedWeaponEvent &e : f->timed) {
			if (e.event == weapon_events::kBoltStarted) {
				bolt_start = e.time;
			} else if (e.event == weapon_events::kBoltFinished && bolt_start >= 0 && bolts++ == 0) {
				std::printf("bolt-run\tkBoltStarted -> kBoltFinished %" PRId64 "\n", e.time - bolt_start);
			}
		}
	}

	if (d.magazine_capacity > 1) {
		if (const std::optional<FireTimingResult> r = timing(t, FireScript::ReloadAt, 600, p_seed, eye, 10)) {
			print_reload("tactical", *r);
		}
	}
	if (const std::optional<FireTimingResult> r = timing(t, script, 1200, p_seed, eye)) {
		print_reload("empty", *r);
	}

	Projectile start;
	start.base_position = { 0, double(eye), 0 };
	start.position = start.base_position;
	const double v = std::sqrt(double(p.launch_velocity[0]) * p.launch_velocity[0] + double(p.launch_velocity[1]) * p.launch_velocity[1] +
			double(p.launch_velocity[2]) * p.launch_velocity[2]);
	start.base_velocity = { double(p.launch_velocity[0]), double(p.launch_velocity[1]), double(p.launch_velocity[2]) };
	start.velocity = start.base_velocity;
	std::printf("# drop\tdistance_m\ttime_s\tdrop_m\tspeed_mps\tdamage\n");
	const SimTime ttl = p.time_to_live;
	for (const double dist : { 10.0, 25.0, 50.0, 100.0, 200.0, 300.0, 500.0, 1000.0 }) {
		if (!(v > 0)) {
			break;
		}
		const Projectile end = fly_free(d, start, ttl);
		if (end.position[2] < dist) {
			break;
		}
		SimTime lo = 0, hi = ttl;
		for (int b = 0; b < 64 && hi - lo > 1; ++b) {
			const SimTime mid = lo + (hi - lo) / 2;
			(fly_free(d, start, mid).position[2] < dist ? lo : hi) = mid;
		}
		const Projectile at = fly_free(d, start, hi);
		const double speed = std::sqrt(at.velocity[0] * at.velocity[0] + at.velocity[1] * at.velocity[1] + at.velocity[2] * at.velocity[2]);
		std::printf("drop\t%g\t%.5f\t%.4f\t%.2f\t%.3f\n", dist, units_to_seconds(hi), double(eye) - at.position[1], speed, falloff_damage(p, dist));
	}
	std::printf("max-range\t%.3f m (v0 x TTL)\n", double(p.muzzle_speed) * units_to_seconds(ttl));

	static const char *const kZones[kHitZoneCount] = { "head", "body", "chest", "legs" };
	std::printf("# stk\tdistance_m\thead\tbody\tchest\tlegs%s\n", unm);
	for (const double dist : kW4Distances) {
		const std::array<int32_t, kHitZoneCount> s = shots_to_kill(d, dist, p_w.max_health, m);
		std::printf("stk\t%g\t%d\t%d\t%d\t%d\n", dist, s[0], s[1], s[2], s[3]);
	}
	for (size_t z = 0; z < kHitZoneCount; ++z) {
		const double r = one_shot_range(d, HitZone(z), p_w.max_health, m);
		std::printf("ohk\t%s\t%s%s\n", kZones[z], std::isinf(r) ? "every range" : (r < 0 ? "never" : (std::to_string(r) + " m").c_str()), unm);
	}
	return install_unchanged(p_ctx, p_before) ? 0 : 1;
}

struct SimArgs {
	std::string partition;
	std::vector<std::string> unlocks;
	std::string level = bf3::core::gameplay::kDefaultWeaponLevel;
	uint64_t seed = 1;
	float seconds = 10.0f;
	unsigned threads = 0;
	bool all = false;
	bool check = false;
};

bool parse_sim_args(const std::vector<std::string> &p_args, SimArgs &r_a) {
	for (size_t i = 0; i < p_args.size(); ++i) {
		const std::string &a = p_args[i];
		const bool has = i + 1 < p_args.size();
		if (a == "--unlock" && has) {
			r_a.unlocks.push_back(p_args[++i]);
		} else if (a == "--level" && has) {
			r_a.level = p_args[++i];
		} else if (a == "--seed" && has) {
			if (!parse_u64(p_args[++i], r_a.seed)) {
				std::fprintf(stderr, "--seed: not a number\n");
				return false;
			}
		} else if (a == "--seconds" && has) {
			if (!parse_float(p_args[++i], r_a.seconds) || r_a.seconds <= 0 || r_a.seconds > 600) {
				std::fprintf(stderr, "--seconds: a number in (0, 600]\n");
				return false;
			}
		} else if (a == "--threads" && has) {
			uint64_t n = 0;
			if (!parse_u64(p_args[++i], n) || n > 256) {
				std::fprintf(stderr, "--threads: 0..256\n");
				return false;
			}
			r_a.threads = unsigned(n);
		} else if (a == "--all") {
			r_a.all = true;
		} else if (a == "--check") {
			r_a.check = true;
		} else if (a.rfind("--", 0) != 0 && r_a.partition.empty()) {
			r_a.partition = a;
		} else {
			std::fprintf(stderr, "weapon-sim: unexpected argument %s\n", a.c_str());
			return false;
		}
	}
	if (int(r_a.all) + int(r_a.check) + int(!r_a.partition.empty()) != 1) {
		std::fprintf(stderr, "weapon-sim: give one of <partition>, --all, --check\n");
		return false;
	}
	return true;
}

std::optional<HitZone> zone_from(const std::string &p_s) {
	if (p_s == "head") {
		return HitZone::Head;
	}
	if (p_s == "body") {
		return HitZone::Body;
	}
	if (p_s == "chest") {
		return HitZone::Chest;
	}
	if (p_s == "legs") {
		return HitZone::Legs;
	}
	return std::nullopt;
}

}

const char *weapon_sim_usage() {
	return "  weapon-sim <partition> [--unlock <u>]... [--level <sb>] [--seed N] [--seconds S]\n"
		   "  weapon-sim --all [--level <sb>] [--threads N]\n"
		   "  weapon-sim --check [--level <sb>]\n"
		   "  range <partition> [--level MP_Subway] [--mode TeamDeathMatch0] [--distances 10,25,50] [--zone head|chest|body|legs]\n"
		   "        [--script hold|tap|throw] [--seconds S] [--seed N] [--exact] [--perf]\n";
}

int cmd_weapon_sim(const EbxContext &p_ctx, const std::vector<std::string> &p_args) {
	SimArgs a;
	if (!parse_sim_args(p_args, a)) {
		return 2;
	}
	const InstallSnapshot before = p_ctx.reader != nullptr ? p_ctx.reader->snapshot() : InstallSnapshot();
	const auto start = std::chrono::steady_clock::now();
	Result<InstallWeapons> w = open_install_weapons(p_ctx.install, a.level);
	if (!w) {
		std::fprintf(stderr, "weapon-sim: %s\n", w.error().message.c_str());
		return 1;
	}
	std::fprintf(stderr, "weapon-sim: %s: %zu variants, %zu grid pairs, opened in %.2f s\n", a.level.c_str(), w->catalog->variants().size(), w->grid_pairs.size(),
			seconds_since(start));
	if (a.check) {
		Result<WeaponCheckInputs> in = install_weapon_check_inputs(*w);
		if (!in) {
			std::fprintf(stderr, "weapon-sim --check: %s\n", in.error().message.c_str());
			return 1;
		}
		in->match_seed = a.seed;
		const std::vector<WeaponCheckRow> rows = run_weapon_checks(*in);
		std::printf("%s", format_weapon_checks(rows).c_str());
		bool pass = true;
		for (const WeaponCheckRow &r : rows) {
			pass = pass && r.pass;
		}
		const bool clean = install_unchanged(p_ctx, before);
		return pass && clean ? 0 : 1;
	}
	if (a.all) {
		const W4Table t = build_w4_table(*w, {}, a.threads);
		std::printf("%s", format_w4_tsv(t).c_str());
		std::fprintf(stderr, "weapon-sim --all: %zu variants, %zu qualifying, %zu rows, %zu errors in %.2f s\n", t.variants, t.qualifying, t.rows.size(), t.errors,
				t.seconds);
		const bool clean = install_unchanged(p_ctx, before);
		return t.errors == 0 && clean ? 0 : 1;
	}
	return weapon_sim_one(p_ctx, *w, a.partition, a.unlocks, a.seed, a.seconds, before);
}

int cmd_range(const EbxContext &p_ctx, const std::vector<std::string> &p_args) {
	std::string partition, level_id = "MP_Subway", mode = "TeamDeathMatch0", zone_name = "chest", script_name;
	std::vector<float> distances{ 10.0f, 25.0f, 50.0f };
	float seconds = 10.0f;
	uint64_t seed = 1;
	bool exact = false, perf = false;
	for (size_t i = 0; i < p_args.size(); ++i) {
		const std::string &a = p_args[i];
		const bool has = i + 1 < p_args.size();
		if (a == "--level" && has) {
			level_id = p_args[++i];
		} else if (a == "--mode" && has) {
			mode = p_args[++i];
		} else if (a == "--zone" && has) {
			zone_name = p_args[++i];
		} else if (a == "--script" && has) {
			script_name = p_args[++i];
		} else if (a == "--seconds" && has) {
			if (!parse_float(p_args[++i], seconds) || seconds <= 0 || seconds > 600) {
				std::fprintf(stderr, "--seconds: a number in (0, 600]\n");
				return 2;
			}
		} else if (a == "--seed" && has) {
			if (!parse_u64(p_args[++i], seed)) {
				std::fprintf(stderr, "--seed: not a number\n");
				return 2;
			}
		} else if (a == "--distances" && has) {
			distances.clear();
			const std::string list = p_args[++i];
			size_t pos = 0;
			while (pos <= list.size()) {
				const size_t comma = std::min(list.find(',', pos), list.size());
				float v = 0;
				if (!parse_float(list.substr(pos, comma - pos), v) || v <= 0 || v > 10'000 || distances.size() >= 32) {
					std::fprintf(stderr, "--distances: up to 32 comma-separated distances in (0, 10000] m\n");
					return 2;
				}
				distances.push_back(v);
				pos = comma + 1;
			}
		} else if (a == "--exact") {
			exact = true;
		} else if (a == "--perf") {
			perf = true;
		} else if (a.rfind("--", 0) != 0 && partition.empty()) {
			partition = a;
		} else {
			std::fprintf(stderr, "range: unexpected argument %s\n", a.c_str());
			return 2;
		}
	}
	if (partition.empty()) {
		std::fprintf(stderr, "range: partition missing\n");
		return 2;
	}
	const std::optional<HitZone> zone = zone_from(zone_name);
	if (!zone) {
		std::fprintf(stderr, "range: --zone head|chest|body|legs\n");
		return 2;
	}
	const InstallSnapshot before = p_ctx.reader != nullptr ? p_ctx.reader->snapshot() : InstallSnapshot();
	const auto start = std::chrono::steady_clock::now();
	Result<InstallWeapons> w = open_install_weapons(p_ctx.install, "Win32/Levels/" + level_id + "/" + level_id);
	if (!w) {
		std::fprintf(stderr, "range: %s\n", w.error().message.c_str());
		return 1;
	}
	Result<level::LevelCollision> c = level::load_level_collision(p_ctx.install, level_id, mode);
	if (!c) {
		std::fprintf(stderr, "range: %s %s: %s\n", level_id.c_str(), mode.c_str(), c.error().message.c_str());
		return 1;
	}
	Result<std::shared_ptr<const StaticWorldShapes>> shapes = build_static_world(c->soup);
	if (!shapes) {
		std::fprintf(stderr, "range: shapes: %s\n", shapes.error().message.c_str());
		return 1;
	}
	Result<std::unique_ptr<CollisionWorld>> world = CollisionWorld::create(*shapes);
	if (!world) {
		std::fprintf(stderr, "range: world: %s\n", world.error().message.c_str());
		return 1;
	}
	Result<level::LevelSpawns> spawns = level::read_level_spawns(*c->world);
	if (!spawns) {
		std::fprintf(stderr, "range: spawns: %s\n", spawns.error().message.c_str());
		return 1;
	}
	const std::vector<SpawnPoint> points = to_spawn_points(*spawns);
	std::fprintf(stderr, "range: %s %s: %zu soup triangles, %zu spawn points, ready in %.2f s\n", level_id.c_str(), mode.c_str(), c->soup.triangles, points.size(),
			seconds_since(start));
	const std::string_view variant[1] = { partition };
	Result<std::shared_ptr<const WeaponTable>> table = install_weapon_table(*w, variant, install_table_options(*w, {}, perf));
	if (!table) {
		std::fprintf(stderr, "range: %s\n", table.error().message.c_str());
		return 1;
	}
	const float eye = w->stand_eye_height;
	if (perf) {
		const uint32_t ticks = uint32_t(std::min(seconds, 60.0f) * float(kSimHz));
		Result<RangePerfResult> r = run_range_perf(**table, **world, points, eye, 0, 64, ticks, seed);
		if (!r) {
			std::fprintf(stderr, "range --perf: %s\n", r.error().message.c_str());
			return 1;
		}
		std::printf("perf\t%u shooters x %zu ticks\tshots %zu\tpeak live %zu\tmean lane %.1f m\n", r->shooters, r->tick_ms.size(), r->shots, r->peak_live_projectiles,
				double(r->mean_lane_length));
		std::printf("perf\tper tick ms\tmean %.4f\tp50 %.4f\tp99 %.4f\tmax %.4f\t(threshold p99 <= 2.0 ms; wall clock, noisy)\n", r->mean_ms, r->p50_ms, r->p99_ms,
				r->max_ms);
		return install_unchanged(p_ctx, before) ? 0 : 1;
	}
	const float furthest = *std::max_element(distances.begin(), distances.end());
	const std::optional<RangeLane> lane = find_range_lane(**world, points, eye, furthest + 1.0f);
	if (!lane) {
		const std::optional<RangeLane> any = find_range_lane(**world, points, eye, 0.0f);
		std::fprintf(stderr, "range: no lane of %.1f m among the spawns (longest %.1f m)\n", double(furthest) + 1.0, any ? double(any->length) : 0.0);
		return 1;
	}
	std::printf("lane\tspawn %u\tfeet (%.3f, %.3f, %.3f)\tyaw %u (%.2f deg)\t%.1f m clear\n", lane->spawn_index, double(lane->feet[0]), double(lane->feet[1]),
			double(lane->feet[2]), unsigned(lane->yaw), double(yaw_radians(lane->yaw)) * 180.0 / 3.14159265358979323846, double(lane->length));
	RangeScenario s;
	s.weapon = 0;
	for (const float d : distances) {
		s.targets.push_back({ d, Pose::Stand, *zone });
	}
	const WeaponSimDef &def = (*table)->defs[0];
	s.script = script_for(def);
	if (script_name == "hold") {
		s.script = FireScript::Hold;
	} else if (script_name == "tap") {
		s.script = FireScript::Tap;
	} else if (script_name == "throw") {
		s.script = FireScript::Throw;
	} else if (!script_name.empty()) {
		std::fprintf(stderr, "range: --script hold|tap|throw\n");
		return 2;
	}
	s.ticks = uint32_t(seconds * float(kSimHz));
	s.hold_ticks = 40;
	s.match_seed = seed;
	s.exact_aim = exact;
	Result<RangeResult> r = run_range(**table, **world, w->soldier, w->capsules, *lane, s);
	if (!r) {
		std::fprintf(stderr, "range: %s\n", r.error().message.c_str());
		return 1;
	}
	std::printf("run\t%s\tscript %s\t%.2f s\tseed %" PRIu64 "\texact %d\tshots %zu\timpacts %zu\tdamage events %zu\tdetonations %zu\n", def.variant.c_str(),
			s.script == FireScript::Hold ? "hold" : (s.script == FireScript::Throw ? "throw" : "tap"), double(seconds), seed, exact ? 1 : 0, r->shots.size(),
			r->impacts.size(), r->damage.size(), r->detonations.size());
	std::printf("# target\tdistance_m\thits_head\thits_body\thits_chest\thits_legs\tdamage\thealth\tkilled\tkilling_shot\tshots_at_kill\tkill_time\n");
	for (const RangeTargetResult &t : r->targets) {
		std::printf("target\t%g\t%u\t%u\t%u\t%u\t%.3f\t%.3f\t%d\t%u\t%u\t%" PRId64 "\n", double(t.distance), t.hits[0], t.hits[1], t.hits[2], t.hits[3], double(t.damage),
				double(t.health), t.killed ? 1 : 0, t.killing_shot, t.shots_at_kill, t.kill_time);
	}
	std::map<std::pair<unsigned, unsigned>, size_t> materials;
	size_t unresolved = 0;
	for (const ImpactEvent &e : r->impacts) {
		if (e.kind == ImpactKind::Static) {
			++materials[{ unsigned(e.physics_material), unsigned(e.physics_property) }];
			unresolved += w->materials->static_pair(e.material_value).resolved ? 0 : 1;
		}
	}
	for (const auto &[k, n] : materials) {
		std::printf("impact-material\tphysics material %u\tproperty %u (%s)\t%zu\n", k.first, k.second, std::string(w->grid->material_name(uint8_t(k.second))).c_str(), n);
	}
	std::printf("hashes\tshots %016" PRIx64 "\timpacts %016" PRIx64 "\n", r->shot_list_hash, r->impact_list_hash);
	std::printf("diagnostics\tstatic hits %zu\tunresolved pairs %zu\tinvisible passes %zu\tpenetrations %zu\tcollision-before-raycast %zu\trefused %zu\tpeak live %zu"
				"\twall %.3f s\n",
			r->static_hits, unresolved, r->invisible_passes, r->penetrations, r->collision_before_raycast, r->refused_projectiles, r->peak_live_projectiles,
			r->seconds_wall);
	return install_unchanged(p_ctx, before) ? 0 : 1;
}

}
