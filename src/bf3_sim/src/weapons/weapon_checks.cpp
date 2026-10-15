// The oracle rows (docs/design/m2a-weapons.md §1; weapon_checks.h).
#include "bf3/sim/weapons/weapon_checks.h"

#include "bf3/sim/collision_world.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/weapons/ballistics.h"
#include "bf3/sim/weapons/damage.h"
#include "weapons_internal.h"

#include <algorithm>
#include <cinttypes>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <optional>
#include <set>

namespace bf3::sim::weapons {

namespace {

using namespace detail;

constexpr std::string_view kM16A3 = "weapons/m16a4/u_m16a4";
constexpr std::string_view kM16A4 = "weapons/m16a4/u_m16_burst";
constexpr std::string_view kM98B = "weapons/model98b/u_m98b";
constexpr std::string_view kJng90 = "weapons/xp2_jng90/u_jng90";
constexpr std::string_view kL96 = "weapons/xp1_l96/u_l96";
constexpr std::string_view kM40A5 = "weapons/m40a5/u_m40a5";
constexpr std::string_view kSv98 = "weapons/sv98/u_sv98";
constexpr std::string_view kAn94 = "weapons/an94/u_an94";
constexpr std::string_view k870 = "weapons/remington870/u_870";
constexpr std::string_view kM9 = "weapons/m9/u_m9";
constexpr std::string_view kM39 = "weapons/m39ebr/u_m39ebr";
constexpr std::string_view kSks = "weapons/sks/u_sks";
constexpr std::string_view kM320 = "weapons/gadgets/m320/u_m320_he";
constexpr std::string_view kRpg7 = "weapons/rpg7/u_rpg7";
constexpr std::string_view kSmaw = "weapons/smaw/u_smaw";
constexpr std::string_view kM67 = "weapons/m67/u_m67";

constexpr std::string_view kVariants[] = { kM16A3, kM16A4, kM98B, kJng90, kL96, kM40A5, kSv98, kAn94, k870, kM9, kM39, kSks, kM320, kRpg7, kSmaw, kM67 };

std::string fmt(const char *p_format, ...) {
	char buf[512];
	va_list args;
	va_start(args, p_format);
	std::vsnprintf(buf, sizeof(buf), p_format, args);
	va_end(args);
	return buf;
}

std::string units(SimTime p_t) {
	return fmt("%" PRId64, int64_t(p_t));
}

class Rows {
public:
	explicit Rows(const Mechanics &p_mechanics) :
			unmeasured_(!p_mechanics.measured) {}
	void add(const char *p_gate, std::string p_name, bool p_pass, std::string p_measured, std::string p_threshold, bool p_depends_unmeasured = false) {
		WeaponCheckRow r;
		r.gate = p_gate;
		r.name = std::move(p_name);
		r.pass = p_pass;
		r.measured = std::move(p_measured);
		r.threshold = std::move(p_threshold);
		r.unmeasured = p_depends_unmeasured && unmeasured_;
		rows_.push_back(std::move(r));
	}
	void report(const char *p_gate, std::string p_name, std::string p_measured) {
		WeaponCheckRow r;
		r.gate = p_gate;
		r.name = std::move(p_name);
		r.pass = true;
		r.reported = true;
		r.measured = std::move(p_measured);
		r.threshold = "reported";
		rows_.push_back(std::move(r));
	}
	std::vector<WeaponCheckRow> take() { return std::move(rows_); }

private:
	bool unmeasured_ = true;
	std::vector<WeaponCheckRow> rows_;
};

std::optional<uint16_t> find(const WeaponTable &p_table, std::string_view p_variant) {
	for (size_t i = 0; i < p_table.defs.size(); ++i) {
		if (p_table.defs[i].variant == p_variant) {
			return uint16_t(i);
		}
	}
	return std::nullopt;
}

std::optional<FireTimingResult> timing(const WeaponTable &p_table, uint16_t p_id, FireScript p_script, uint32_t p_ticks, uint32_t p_settle, float p_eye,
		uint64_t p_seed, uint32_t p_reload_tick = 0, uint32_t p_hold_ticks = 0) {
	RangeScenario s;
	s.weapon = p_id;
	s.script = p_script;
	s.ticks = p_ticks;
	s.settle_ticks = p_settle;
	s.reload_tick = p_reload_tick;
	s.hold_ticks = p_hold_ticks;
	s.match_seed = p_seed;
	core::Result<FireTimingResult> r = run_fire_timing(p_table, s, p_eye);
	if (!r) {
		return std::nullopt;
	}
	return std::move(*r);
}

std::vector<const TimedWeaponEvent *> events_of(const FireTimingResult &p_r, uint32_t p_event) {
	std::vector<const TimedWeaponEvent *> out;
	for (const TimedWeaponEvent &e : p_r.timed) {
		if (e.event == p_event) {
			out.push_back(&e);
		}
	}
	return out;
}

const SoldierWeaponsState *state_at(const FireTimingResult &p_r, SimTime p_time) {
	const Tick t = tick_containing(p_time);
	for (const SoldierWeaponsState &s : p_r.states) {
		if (s.tick == t) {
			return &s;
		}
	}
	return nullptr;
}

// A synthetic range world with walls at p_walls (none: only the slab, 150 m below the eye).
std::unique_ptr<CollisionWorld> wall_world(std::span<const float> p_walls, float p_eye) {
	const std::vector<StaticTriangle> tris = range_wall_triangles(p_walls, p_eye);
	core::Result<std::shared_ptr<const StaticWorldShapes>> shapes = build_static_world(std::span<const StaticTriangle>(tris));
	if (!shapes) {
		return nullptr;
	}
	core::Result<std::unique_ptr<CollisionWorld>> world = CollisionWorld::create(*shapes);
	return world ? std::move(*world) : nullptr;
}

// A level shot of def p_id from (0, eye, 0) along +Z at time 0, flown until it is gone (at most p_max_ticks).
struct Flight {
	std::vector<ImpactEvent> impacts;
	std::vector<DetonationEvent> detonations;
	std::vector<DamageEvent> damage;
	std::optional<Projectile> first_state; // after spawn
};
Flight fly_shot(const std::shared_ptr<const WeaponTable> &p_table, uint16_t p_id, const CollisionWorld &p_world, float p_eye, uint32_t p_max_ticks,
		const HitTargetProvider *p_targets = nullptr, uint64_t p_seed = 1) {
	Flight f;
	ProjectileWorld pw(p_table);
	ShotEvent shot;
	shot.time = 0;
	shot.spawn_time = p_table->defs[p_id].spawn_delay;
	shot.seed = p_seed;
	shot.def = p_id;
	shot.pellets = 1;
	shot.origin = { 0, p_eye, 0 };
	shot.aim = { 0, 0, 1 };
	shot.direction = { 0, 0, 1 };
	if (!pw.spawn(shot).ok()) {
		return f;
	}
	f.first_state = pw.live()[0];
	const StaticTargets none({});
	const HitTargetProvider &targets = p_targets != nullptr ? *p_targets : none;
	ProjectileStepOutput out;
	for (Tick t = 1; t <= p_max_ticks && !pw.live().empty(); ++t) {
		out.clear();
		pw.step(t, p_world, targets, out);
		f.impacts.insert(f.impacts.end(), out.impacts.begin(), out.impacts.end());
		f.detonations.insert(f.detonations.end(), out.detonations.begin(), out.detonations.end());
		f.damage.insert(f.damage.end(), out.damage.begin(), out.damage.end());
	}
	return f;
}

const ImpactEvent *first_impact(const Flight &p_f, ImpactKind p_kind) {
	for (const ImpactEvent &e : p_f.impacts) {
		if (e.kind == p_kind) {
			return &e;
		}
	}
	return nullptr;
}

double angle_deg(const Vec3 &p_a, const Vec3 &p_b) {
	const double c = std::clamp(dot(normalize(to_d(p_a)), normalize(to_d(p_b))), -1.0, 1.0);
	return std::acos(c) * 180.0 / kPiD;
}

// ---- W1 -------------------------------------------------------------------------------------------------------

void check_m16a3(Rows &r_rows, const WeaponCheckInputs &p_in) {
	const std::optional<uint16_t> id = find(*p_in.table, kM16A3);
	if (!id) {
		r_rows.add("W1", "M16A3 one magazine", false, "u_m16a4 missing", "31 shots, 30 gaps of 4500000");
		return;
	}
	if (const std::optional<FireTimingResult> t = timing(*p_in.table, *id, FireScript::Hold, 360, 0, p_in.eye_height, p_in.match_seed)) {
		bool gaps = t->shots.size() >= 31;
		SimTime lo = INT64_MAX, hi = 0;
		for (size_t i = 1; i < std::min<size_t>(31, t->shots.size()); ++i) {
			const SimTime g = t->shots[i].time - t->shots[i - 1].time;
			lo = std::min(lo, g);
			hi = std::max(hi, g);
			gaps = gaps && g == 4'500'000;
		}
		const bool first = !t->shots.empty() && t->shots[0].time == tick_start(t->first_script_tick);
		const std::vector<const TimedWeaponEvent *> reloads = events_of(*t, weapon_events::kReloadStarted);
		const bool reload = t->shots.size() >= 31 && !reloads.empty() && reloads[0]->time == t->shots[30].time + 4'500'000;
		size_t first_mag = 0;
		for (const ShotEvent &s : t->shots) {
			first_mag += (reloads.empty() || s.time < reloads[0]->time) ? 1 : 0;
		}
		r_rows.add("W1", "M16A3 one magazine: held from the press tick", gaps && first && reload && first_mag == 31,
				fmt("%zu shots before the reload, gaps %s..%s, first at the press tick's start %s, empty reload at last + %s", first_mag, units(lo).c_str(),
						units(hi).c_str(), first ? "yes" : "no", reloads.empty() || t->shots.size() < 31 ? "-" : units(reloads[0]->time - t->shots[30].time).c_str()),
				"31 shots, every gap 4500000, then the empty reload");
	}
	const std::optional<uint16_t> oid = find(*p_in.oracle, kM16A3);
	if (!oid || !p_in.oracle->infinite_magazine) {
		r_rows.add("W1", "M16A3 10 s held (infinite_magazine)", false, "no oracle table", "134 shots");
		return;
	}
	if (const std::optional<FireTimingResult> t = timing(*p_in.oracle, *oid, FireScript::Hold, 600, 0, p_in.eye_height, p_in.match_seed)) {
		bool gaps = true;
		for (size_t i = 1; i < t->shots.size(); ++i) {
			gaps = gaps && t->shots[i].time - t->shots[i - 1].time == 4'500'000;
		}
		r_rows.add("W1", "M16A3 10 s held (infinite_magazine table)", t->shots.size() == 134 && gaps,
				fmt("%zu shots, every gap 4500000: %s", t->shots.size(), gaps ? "yes" : "no"), "134 shots, every gap exactly 4500000 (75 ms)");
	}
}

void check_bolts(Rows &r_rows, const WeaponCheckInputs &p_in) {
	if (const std::optional<uint16_t> id = find(*p_in.table, kM98B)) {
		if (const std::optional<FireTimingResult> t = timing(*p_in.table, *id, FireScript::Tap, 420, 0, p_in.eye_height, p_in.match_seed)) {
			const auto starts = events_of(*t, weapon_events::kBoltStarted);
			const auto ends = events_of(*t, weapon_events::kBoltFinished);
			bool exact = !starts.empty() && starts.size() >= ends.size() && !ends.empty();
			for (size_t i = 0; i < ends.size() && i < starts.size(); ++i) {
				exact = exact && ends[i]->time - starts[i]->time == 78'000'000;
			}
			r_rows.add("W1", "M98B bolt cycle (kBoltStarted to kBoltFinished)", exact,
					fmt("%zu cycles, each %s units", ends.size(), ends.empty() ? "-" : units(ends[0]->time - starts[0]->time).c_str()),
					"exactly 78000000 (1.3 s, 46.15 RPM)");
			if (t->shots.size() >= 2) {
				const SimTime gap = t->shots[1].time - t->shots[0].time;
				r_rows.report("W1", "M98B tap-to-tap gap (HoldBoltActionUntilFireRelease)",
						fmt("%s units = %.4f s, %.2f RPM (cycle + 1 tick)", units(gap).c_str(), units_to_seconds(gap), 60.0 / units_to_seconds(gap)));
			}
		}
	} else {
		r_rows.add("W1", "M98B bolt cycle", false, "u_m98b missing", "78000000");
	}
	const struct {
		std::string_view variant;
		SimTime cycle;
	} bolts[] = { { kJng90, 78'000'000 }, { kL96, 82'800'000 }, { kM40A5, 66'000'000 }, { kSv98, 75'000'000 } };
	std::string measured;
	bool ok = true;
	for (const auto &b : bolts) {
		const std::optional<uint16_t> id = find(*p_in.table, b.variant);
		const SimTime c = id ? p_in.table->defs[*id].bolt_cycle : -1;
		ok = ok && c == b.cycle;
		measured += std::string(b.variant.substr(b.variant.rfind('/') + 1)) + " " + units(c) + "  ";
	}
	r_rows.add("W1", "JNG-90 / L96 / M40A5 / SV98 bolt cycles", ok, measured, "1.3 / 1.38 / 1.1 / 1.25 s = 78000000 / 82800000 / 66000000 / 75000000");
}

// Shots grouped by the press that fired them (a press's burst ends before the next press).
std::vector<std::vector<const ShotEvent *>> by_press(const FireTimingResult &p_t) {
	std::vector<std::vector<const ShotEvent *>> groups;
	for (size_t p = 0; p < p_t.presses.size(); ++p) {
		const SimTime from = tick_start(p_t.presses[p]);
		const SimTime to = p + 1 < p_t.presses.size() ? tick_start(p_t.presses[p + 1]) : INT64_MAX;
		std::vector<const ShotEvent *> g;
		for (const ShotEvent &s : p_t.shots) {
			if (s.time >= from && s.time < to) {
				g.push_back(&s);
			}
		}
		groups.push_back(std::move(g));
	}
	return groups;
}

void check_bursts(Rows &r_rows, const WeaponCheckInputs &p_in) {
	const struct {
		std::string_view variant;
		const char *name;
		size_t count;
		SimTime gap;
		SimTime between;
	} cases[] = { { kAn94, "AN-94 burst: 2 per press at 1200 RPM", 2, 3'000'000, 6'000'000 }, { kM16A4, "M16A4 burst: 3 per press at 800 RPM", 3, 4'500'000, 4'500'000 } };
	for (const auto &c : cases) {
		const std::optional<uint16_t> id = find(*p_in.table, c.variant);
		if (!id) {
			r_rows.add("W1", c.name, false, "variant missing", "");
			continue;
		}
		// 30 settle ticks: a burst pressed in the script's last ticks completes after it (the burst runs without fire).
		const std::optional<FireTimingResult> t = timing(*p_in.table, *id, FireScript::Tap, 120, 30, p_in.eye_height, p_in.match_seed);
		if (!t) {
			continue;
		}
		bool ok = !t->presses.empty();
		size_t full = 0;
		SimTime min_between = INT64_MAX;
		const auto groups = by_press(*t);
		for (size_t g = 0; g < groups.size(); ++g) {
			const auto &shots = groups[g];
			if (shots.empty()) {
				continue; // a press after the magazine ran dry
			}
			ok = ok && shots.size() == c.count;
			for (size_t i = 1; i < shots.size(); ++i) {
				ok = ok && shots[i]->time - shots[i - 1]->time == c.gap;
			}
			if (g > 0 && !groups[g - 1].empty()) {
				min_between = std::min(min_between, shots.front()->time - groups[g - 1].back()->time);
			}
			++full;
		}
		ok = ok && full >= 3 && min_between >= c.between;
		r_rows.add("W1", c.name, ok,
				fmt("%zu presses fired, each %zu shots %s apart: %s; burst to burst >= %s", full, c.count, units(c.gap).c_str(), ok ? "yes" : "no",
						min_between == INT64_MAX ? "-" : units(min_between).c_str()),
				fmt("%zu shots %s apart per press, one burst per press, next burst >= %s after the last shot", c.count, units(c.gap).c_str(),
						units(c.between).c_str()));
	}
}

void check_870(Rows &r_rows, const WeaponCheckInputs &p_in) {
	const std::optional<uint16_t> id = find(*p_in.table, k870);
	if (!id) {
		r_rows.add("W1", "870MCS pump and pellets", false, "u_870 missing", "");
		return;
	}
	const std::optional<FireTimingResult> t = timing(*p_in.table, *id, FireScript::Tap, 240, 0, p_in.eye_height, p_in.match_seed);
	if (!t) {
		return;
	}
	const WeaponSimDef &d = p_in.table->defs[*id];
	// The first magazine (5 shells): consecutive shots.
	const auto reloads = events_of(*t, weapon_events::kReloadStarted);
	std::vector<SimTime> times;
	for (const ShotEvent &s : t->shots) {
		if (reloads.empty() || s.time < reloads[0]->time) {
			times.push_back(s.time);
		}
	}
	bool pump = times.size() == size_t(d.magazine_capacity);
	for (size_t i = 1; i < times.size(); ++i) {
		pump = pump && times[i] - times[i - 1] == 28'800'000;
	}
	r_rows.add("W1", "870MCS pump: Tap pinned to the tick containing the ready time", pump,
			fmt("%zu shots in the first magazine, gaps %s", times.size(), times.size() >= 2 ? units(times[1] - times[0]).c_str() : "-"),
			"shots exactly 28800000 apart (0.48 s, 125 RPM)");
	// Pellets: 12 projectiles per shot, 12 distinct directions inside the pellet cone of the shot direction.
	bool pellets = !t->shots.empty();
	double widest = 0;
	size_t distinct_min = 64;
	for (const ShotEvent &s : t->shots) {
		ProjectileWorld pw(p_in.table);
		pellets = pellets && s.pellets == 12 && pw.spawn(s).ok() && pw.live().size() == 12;
		std::set<std::array<float, 3>> dirs;
		for (const Projectile &p : pw.live()) {
			const Vec3 v = to_f(normalize(p.velocity));
			dirs.insert(v);
			widest = std::max(widest, angle_deg(v, s.direction));
		}
		distinct_min = std::min(distinct_min, dirs.size());
	}
	const float cone = d.pellet_cone[0].min_angle;
	pellets = pellets && distinct_min == 12 && widest <= double(cone) + 1e-3;
	r_rows.add("W1", "870MCS: 12 pellets per shot inside the 3 deg cone", pellets,
			fmt("%zu shots, pellets 12, distinct directions >= %zu, widest %.4f deg from the shot direction", t->shots.size(), distinct_min, widest),
			"12 projectiles with 12 distinct directions, each within 3 deg (+ the shot's spread) of the aim");
}

void check_m9(Rows &r_rows, const WeaponCheckInputs &p_in) {
	const std::optional<uint16_t> id = find(*p_in.table, kM9);
	if (!id) {
		r_rows.add("W1", "M9 single fire", false, "u_m9 missing", ">= 9000000");
		return;
	}
	if (const std::optional<FireTimingResult> t = timing(*p_in.table, *id, FireScript::Tap, 120, 0, p_in.eye_height, p_in.match_seed)) {
		SimTime lo = INT64_MAX;
		const auto reloads = events_of(*t, weapon_events::kReloadStarted);
		size_t n = 0;
		for (size_t i = 1; i < t->shots.size(); ++i) {
			if (!reloads.empty() && t->shots[i].time > reloads[0]->time) {
				break;
			}
			lo = std::min(lo, t->shots[i].time - t->shots[i - 1].time);
			++n;
		}
		r_rows.add("W1", "M9 single fire, tapping", n >= 5 && lo >= 9'000'000, fmt("%zu gaps, smallest %s", n, units(lo).c_str()), "every gap >= 9000000 (400 RPM)");
	}
}

// ---- W2 -------------------------------------------------------------------------------------------------------

void check_drop(Rows &r_rows, const WeaponCheckInputs &p_in) {
	const struct {
		std::string_view variant;
		const char *name;
		float distance;
		double oracle;
	} rows[] = { { kM16A3, "M16A3 (v0 650, g 15) drop at 100 m", 100.0f, 0.1775 }, { kM16A3, "M16A3 drop at 300 m", 300.0f, 1.5976 },
		{ kM98B, "M98B (v0 650, g 9.81) drop at 300 m", 300.0f, 1.0449 }, { kM98B, "M98B drop at 1,000 m", 1000.0f, 11.609 } };
	for (const auto &row : rows) {
		const std::optional<uint16_t> id = find(*p_in.table, row.variant);
		const float walls[1] = { row.distance };
		const std::unique_ptr<CollisionWorld> world = wall_world(walls, p_in.eye_height);
		if (!id || world == nullptr) {
			r_rows.add("W2", row.name, false, "variant or world missing", "");
			continue;
		}
		const WeaponSimDef &d = p_in.table->defs[*id];
		const Flight f = fly_shot(p_in.table, *id, *world, p_in.eye_height, 600);
		const ImpactEvent *hit = first_impact(f, ImpactKind::Static);
		const double t = double(row.distance) / double(d.projectile.muzzle_speed);
		const double arithmetic = 0.5 * std::fabs(double(d.projectile.gravity)) * t * t;
		const double drop = hit != nullptr ? double(p_in.eye_height) - double(hit->position[1]) : -1.0;
		const bool ok = hit != nullptr && std::fabs(drop - row.oracle) <= 0.01 && std::fabs(drop - arithmetic) <= 0.01;
		r_rows.add("W2", row.name, ok,
				fmt("impact %.4f m below the launch line at z %.3f (arithmetic %.4f m, diff %.2f mm)", drop, hit != nullptr ? double(hit->position[2]) : 0.0,
						arithmetic, (drop - arithmetic) * 1000.0),
				fmt("%.4f m within 1 cm", row.oracle));
	}
}

void check_range(Rows &r_rows, const WeaponCheckInputs &p_in) {
	const std::unique_ptr<CollisionWorld> open = wall_world({}, p_in.eye_height);
	for (const std::string_view v : { kM16A3, kM98B }) {
		const std::optional<uint16_t> id = find(*p_in.table, v);
		if (!id || open == nullptr) {
			r_rows.add("W2", std::string(v) + " max range", false, "variant or world missing", "");
			continue;
		}
		const WeaponSimDef &d = p_in.table->defs[*id];
		const double range = double(d.projectile.muzzle_speed) * units_to_seconds(d.projectile.time_to_live);
		const Flight f = fly_shot(p_in.table, *id, *open, p_in.eye_height, 600);
		const ImpactEvent *exp = first_impact(f, ImpactKind::Expired);
		const double horizontal = exp != nullptr ? std::hypot(double(exp->position[0]), double(exp->position[2])) : -1.0;
		const std::string name = std::string(v.substr(v.rfind('/') + 1));
		r_rows.add("W2", name + " max range = v0 x TTL", exp != nullptr && std::fabs(horizontal - range) <= 1e-3,
				fmt("horizontal distance at expiry %.4f m (v0 x TTL %.1f m, diff %.3f mm), expired at %s", horizontal, range, (horizontal - range) * 1000.0,
						exp != nullptr ? units(exp->time).c_str() : "-"),
				"v0 x TTL within 1 mm");
		const float near[1] = { float(0.99 * range) };
		const float far[1] = { float(1.01 * range) };
		const std::unique_ptr<CollisionWorld> wn = wall_world(near, p_in.eye_height);
		const std::unique_ptr<CollisionWorld> wf = wall_world(far, p_in.eye_height);
		const Flight fn = fly_shot(p_in.table, *id, *wn, p_in.eye_height, 600);
		const Flight ff = fly_shot(p_in.table, *id, *wf, p_in.eye_height, 600);
		const bool hit_near = first_impact(fn, ImpactKind::Static) != nullptr;
		const bool hit_far = first_impact(ff, ImpactKind::Static) != nullptr;
		r_rows.add("W2", name + " walls at 0.99 x and 1.01 x the range", hit_near && !hit_far,
				fmt("0.99 x (%.1f m): %s, 1.01 x (%.1f m): %s", double(near[0]), hit_near ? "hit" : "missed", double(far[0]), hit_far ? "hit" : "missed"),
				"0.99 x hit, 1.01 x not");
	}
}

// ---- W3, W5 ---------------------------------------------------------------------------------------------------

void check_damage(Rows &r_rows, const WeaponCheckInputs &p_in) {
	const Mechanics &m = p_in.table->mechanics;
	if (const std::optional<uint16_t> id = find(*p_in.table, kM16A3)) {
		const WeaponSimDef &d = p_in.table->defs[*id];
		bool near = true, far = true;
		std::string measured;
		for (const double dist : { 0.0, 5.0, 8.0 }) {
			const auto s = shots_to_kill(d, dist, p_in.health, m);
			near = near && s[size_t(HitZone::Chest)] == 4 && s[size_t(HitZone::Body)] == 4 && s[size_t(HitZone::Head)] == 2;
		}
		for (const double dist : { 50.0, 75.0, 100.0, 300.0 }) {
			const auto s = shots_to_kill(d, dist, p_in.health, m);
			far = far && s[size_t(HitZone::Chest)] == 6 && s[size_t(HitZone::Body)] == 6 && s[size_t(HitZone::Head)] == 3;
		}
		const auto a = shots_to_kill(d, 8.0, p_in.health, m);
		const auto b = shots_to_kill(d, 50.0, p_in.health, m);
		r_rows.add("W3", "M16A3 shots to kill", near && far,
				fmt("8 m chest %d head %d; 50 m chest %d head %d (0 / 5 / 8 and 50 / 75 / 100 / 300 m checked)", a[2], a[0], b[2], b[0]),
				"chest/body 4 and head 2 at <= 8 m; 6 and 3 at >= 50 m", true);
		const double head = zone_damage(d.projectile, HitZone::Head, 5.0, m);
		const double chest = zone_damage(d.projectile, HitZone::Chest, 5.0, m);
		const double body = zone_damage(d.projectile, HitZone::Body, 5.0, m);
		const double legs = zone_damage(d.projectile, HitZone::Legs, 5.0, m);
		r_rows.add("W5", "M16A3 per-hit damage at 5 m",
				std::fabs(head - 50.0) <= 1e-3 && std::fabs(chest - 25.0) <= 1e-3 && std::fabs(body - 25.0) <= 1e-3 && std::fabs(legs - 22.75) <= 1e-3,
				fmt("head %.4f chest %.4f body %.4f legs %.4f", head, chest, body, legs), "50.0 / 25.0 / 25.0 / 22.75 within 1e-3");
	}
	if (const std::optional<uint16_t> id = find(*p_in.table, kM98B)) {
		const WeaponSimDef &d = p_in.table->defs[*id];
		const double head = zone_damage(d.projectile, HitZone::Head, 5.0, m);
		const double chest = zone_damage(d.projectile, HitZone::Chest, 5.0, m);
		const double legs = zone_damage(d.projectile, HitZone::Legs, 5.0, m);
		r_rows.add("W5", "M98B per-hit damage at 5 m (x2.35 / x1.06 / x0.91)",
				std::fabs(head - 223.25) <= 1e-3 && std::fabs(chest - 100.70) <= 1e-3 && std::fabs(legs - 86.45) <= 1e-3,
				fmt("head %.4f chest %.4f legs %.4f", head, chest, legs), "223.25 / 100.70 / 86.45 within 1e-3");
	}
	if (const std::optional<uint16_t> id = find(*p_in.table, k870)) {
		const double head = zone_damage(p_in.table->defs[*id].projectile, HitZone::Head, 5.0, m);
		r_rows.add("W5", "870 pellet head at 5 m (x1.0)", std::fabs(head - 18.0) <= 1e-3, fmt("%.4f", head), "18.0 within 1e-3");
	}
	const struct {
		std::string_view variant;
		HitZone zone;
		double oracle; // +inf: every range
		const char *name;
	} ohk[] = { { kM98B, HitZone::Head, INFINITY, ".338 (M98B) head" }, { kM98B, HitZone::Chest, 17.476, ".338 (M98B) chest" },
		{ kM40A5, HitZone::Chest, 15.000, "M40A5 chest" }, { kSv98, HitZone::Chest, 20.000, "SV98 chest" }, { kM39, HitZone::Head, 50.745, "M39 EBR head" },
		{ kSks, HitZone::Head, 16.257, "SKS head" } };
	for (const auto &o : ohk) {
		const std::optional<uint16_t> id = find(*p_in.table, o.variant);
		if (!id) {
			r_rows.add("W3", std::string("one-shot range ") + o.name, false, "variant missing", "");
			continue;
		}
		const double r = one_shot_range(p_in.table->defs[*id], o.zone, p_in.health, m);
		const bool ok = std::isinf(o.oracle) ? std::isinf(r) : std::fabs(r - o.oracle) <= 0.01;
		r_rows.add("W3", std::string("one-shot range ") + o.name, ok, std::isinf(r) ? std::string("every range") : fmt("%.4f m", r),
				std::isinf(o.oracle) ? std::string("every range") : fmt("%.3f m within 0.01", o.oracle), true);
	}
}

// ---- W6 -------------------------------------------------------------------------------------------------------

struct ReloadSeen {
	SimTime started = -1, committed = -1, finished = -1;
	int32_t magazine_after = -1;
	int32_t reserve_before = -1, reserve_after = -1;
};
ReloadSeen first_reload(const FireTimingResult &p_t) {
	ReloadSeen r;
	const auto s = events_of(p_t, weapon_events::kReloadStarted);
	if (s.empty()) {
		return r;
	}
	r.started = s[0]->time;
	for (const TimedWeaponEvent &e : p_t.timed) {
		if (e.time < r.started) {
			continue;
		}
		if (e.event == weapon_events::kReloadCommitted && r.committed < 0) {
			r.committed = e.time;
		}
		if (e.event == weapon_events::kReloadFinished && r.finished < 0) {
			r.finished = e.time;
		}
	}
	if (const SoldierWeaponsState *b = state_at(p_t, r.started)) {
		r.reserve_before = b->current().reserve;
	}
	if (r.committed >= 0) {
		if (const SoldierWeaponsState *a = state_at(p_t, r.committed)) {
			r.magazine_after = a->current().magazine;
			r.reserve_after = a->current().reserve;
		}
	}
	return r;
}

void check_reloads(Rows &r_rows, const WeaponCheckInputs &p_in) {
	const struct {
		std::string_view variant;
		const char *name;
		bool tactical;
		SimTime duration;
		SimTime commit; // -1: not checked
		int32_t magazine; // -1: not checked
	} cases[] = {
		{ kM16A3, "M16A3 tactical reload", true, 108'000'000, 91'800'000, 31 },
		{ kM16A3, "M16A3 empty reload", false, 142'200'000, 120'870'000, 30 },
		{ kM9, "M9 tactical reload", true, 81'000'000, -1, 16 },
		{ kM9, "M9 empty reload", false, 96'000'000, -1, 15 },
	};
	for (const auto &c : cases) {
		const std::optional<uint16_t> id = find(*p_in.table, c.variant);
		if (!id) {
			r_rows.add("W6", c.name, false, "variant missing", "");
			continue;
		}
		const FireScript script = c.tactical ? FireScript::ReloadAt : (c.variant == kM9 ? FireScript::Tap : FireScript::Hold);
		const std::optional<FireTimingResult> t = timing(*p_in.table, *id, script, 600, 0, p_in.eye_height, p_in.match_seed, 10);
		if (!t) {
			continue;
		}
		const ReloadSeen r = first_reload(*t);
		const bool dur = r.finished - r.started == c.duration;
		const bool commit = c.commit < 0 || r.committed - r.started == c.commit;
		const bool mag = c.magazine < 0 || r.magazine_after == c.magazine;
		const bool reserve = r.reserve_before >= 0 && r.reserve_after >= 0 && r.reserve_before - r.reserve_after >= 0;
		r_rows.add("W6", c.name, r.started >= 0 && dur && commit && mag && reserve,
				fmt("started %s, committed +%s, finished +%s, magazine %d after, reserve %d -> %d", units(r.started).c_str(), units(r.committed - r.started).c_str(),
						units(r.finished - r.started).c_str(), r.magazine_after, r.reserve_before, r.reserve_after),
				fmt("duration %s%s, magazine %d (the 30 + 1 rule and the commit point are [unmeasured])", units(c.duration).c_str(),
						c.commit >= 0 ? (", commit +" + units(c.commit)).c_str() : "", c.magazine),
				true);
	}
	if (const std::optional<uint16_t> id = find(*p_in.table, k870)) {
		const std::optional<FireTimingResult> t = timing(*p_in.table, *id, FireScript::Tap, 600, 0, p_in.eye_height, p_in.match_seed);
		if (t) {
			const ReloadSeen r = first_reload(*t);
			std::vector<SimTime> shells;
			for (const TimedWeaponEvent &e : t->timed) {
				if (e.event == weapon_events::kShellLoaded && e.time > r.started && (r.finished < 0 || e.time <= r.finished)) {
					shells.push_back(e.time - r.started);
				}
			}
			bool ok = r.started >= 0 && shells.size() == 5 && r.finished - r.started == 270'000'000;
			for (size_t k = 0; k < shells.size(); ++k) {
				ok = ok && shells[k] == 69'000'000 + SimTime(k + 1) * 40'200'000;
			}
			r_rows.add("W6", "870 empty reload: ReloadDelay 1.15 s + 0.67 s per shell", ok,
					fmt("%zu shells at +%s.., finished +%s", shells.size(), shells.empty() ? "-" : units(shells[0]).c_str(), units(r.finished - r.started).c_str()),
					"5 shells at 1.15 + k x 0.67 s, done at 4.5 s (270000000) [composition inferred]");
		}
	}
}

// ---- W9 -------------------------------------------------------------------------------------------------------

void check_explosives(Rows &r_rows, const WeaponCheckInputs &p_in) {
	const Mechanics &m = p_in.table->mechanics;
	const std::unique_ptr<CollisionWorld> open = wall_world({}, p_in.eye_height);
	if (const std::optional<uint16_t> id = find(*p_in.table, kM67); id && open != nullptr) {
		const WeaponSimDef &d = p_in.table->defs[*id];
		// The throw through the state machine: hold 40 ticks (past HoldDelay 0.5 s), release.
		const std::optional<FireTimingResult> t = timing(*p_in.table, *id, FireScript::Throw, 60, 0, p_in.eye_height, p_in.match_seed, 0, 40);
		bool thrown = false;
		if (t && !t->shots.empty()) {
			const ShotEvent &s = t->shots[0];
			thrown = s.time == tick_start(t->first_script_tick + 40) && s.spawn_time - s.time == 10'200'000;
			r_rows.add("W9", "M67 throw: at the release tick's start, spawn 0.17 s later", thrown,
					fmt("thrown at %s (release tick start %s), spawn +%s, power %.2f", units(s.time).c_str(), units(tick_start(t->first_script_tick + 40)).c_str(),
							units(s.spawn_time - s.time).c_str(), double(s.power)),
					"spawn exactly 10200000 after the release tick's start");
		} else {
			r_rows.add("W9", "M67 throw", false, "no throw", "a throw after HoldDelay");
		}
		const Flight f = fly_shot(p_in.table, *id, *open, p_in.eye_height, 600);
		const DetonationEvent *det = f.detonations.empty() ? nullptr : &f.detonations[0];
		const SimTime spawn = d.spawn_delay;
		const DVec3 v0 = f.first_state ? f.first_state->velocity : DVec3{};
		const DVec3 closed = ballistic_position({ 0, double(p_in.eye_height), 0 }, v0, double(d.projectile.gravity), 3.6);
		const double err = det != nullptr ? length(sub(to_d(det->position), closed)) : -1.0;
		const bool launch = std::fabs(v0[0]) < 1e-9 && std::fabs(v0[1] - 3.0) < 1e-6 && std::fabs(v0[2] - 18.0) < 1e-6;
		r_rows.add("W9", "M67 fuse: detonation 216000000 after spawn, free flight = closed form", det != nullptr && det->time - spawn == 216'000'000 && det->timeout &&
						err >= 0 && err <= 0.01 && launch,
				fmt("detonation at spawn + %s (timeout %s), position off the closed form by %.3f mm, launch (%.2f, %.2f, %.2f)",
						det != nullptr ? units(det->time - spawn).c_str() : "-", det != nullptr && det->timeout ? "yes" : "no", err * 1000.0, v0[0], v0[1], v0[2]),
				"exactly 216000000 (3.6 s); within 1 cm; launch (0, 3, 18) in the aim frame");
		const double at[5] = { 1.0, 1.5, 3.0, 5.0, 7.0 };
		const double want[5] = { 112.0, 112.0, 81.4545, 40.7273, 0.0 };
		bool ok = d.projectile.explosion.present;
		std::string measured;
		for (size_t i = 0; i < 5; ++i) {
			const double b = blast_damage(d.projectile.explosion, at[i], m.blast_falloff) * double(d.projectile.explosion.zone_multiplier[size_t(HitZone::Chest)]);
			ok = ok && std::fabs(b - want[i]) <= 0.01;
			measured += fmt("%.2f ", b);
		}
		r_rows.add("W9", "M67 blast at 1 / 1.5 / 3 / 5 / 7 m", ok, measured, "112 / 112 / 81.45 / 40.73 / 0 within 0.01 (linear falloff [unmeasured])", true);
	} else {
		r_rows.add("W9", "M67", false, "u_m67 or the world missing", "");
	}
	if (const std::optional<uint16_t> id = find(*p_in.table, kM320)) {
		const WeaponSimDef &d = p_in.table->defs[*id];
		const float w10[1] = { 10.0f };
		const float w40[1] = { 40.0f };
		const std::unique_ptr<CollisionWorld> a = wall_world(w10, p_in.eye_height);
		const std::unique_ptr<CollisionWorld> b = wall_world(w40, p_in.eye_height);
		const Flight fa = fly_shot(p_in.table, *id, *a, p_in.eye_height, 600);
		const Flight fb = fly_shot(p_in.table, *id, *b, p_in.eye_height, 600);
		const bool dud = !fa.detonations.empty() && fa.detonations[0].dud && fa.detonations[0].time < 30'000'000;
		const bool armed = !fb.detonations.empty() && !fb.detonations[0].dud && fb.detonations[0].time >= 30'000'000;
		const ExplosionSimDef &x = d.projectile.explosion;
		const ExplosionSimDef &u = d.projectile.dud;
		const bool numbers = d.projectile.time_to_arm == 30'000'000 && x.blast_damage == 112.0f && x.inner_radius == 0.35f && x.radius == 5.0f &&
				u.blast_damage == 56.0f && u.inner_radius == 2.1f && u.radius == 5.0f && d.projectile.gravity == -7.2f;
		r_rows.add("W9", "M320 HE: dud before 0.5 s, explosion after", dud && armed && numbers,
				fmt("10 m wall at %s: %s; 40 m wall at %s: %s; explosion %.0f/%.2f/%.0f, dud %.0f/%.2f/%.0f, g %.1f",
						fa.detonations.empty() ? "-" : units(fa.detonations[0].time).c_str(), dud ? "dud" : "NOT a dud",
						fb.detonations.empty() ? "-" : units(fb.detonations[0].time).c_str(), armed ? "explosion" : "NOT an explosion", double(x.blast_damage),
						double(x.inner_radius), double(x.radius), double(u.blast_damage), double(u.inner_radius), double(u.radius), double(d.projectile.gravity)),
				"arm 30000000; explosion 112 / 0.35 / 5, dud 56 / 2.1 / 5; g -7.2");
		r_rows.report("W9", "M320 HE direct hit (StartDamage x zone)",
				fmt("%.1f x %.2f = %.1f", double(d.projectile.start_damage), double(d.projectile.zone[size_t(HitZone::Chest)].protection_multiplier),
						double(d.projectile.start_damage * d.projectile.zone[size_t(HitZone::Chest)].protection_multiplier)));
	} else {
		r_rows.add("W9", "M320 HE", false, "u_m320_he missing", "");
	}
	for (const std::string_view v : { kRpg7, kSmaw }) {
		const std::optional<uint16_t> id = find(*p_in.table, v);
		const std::string name = std::string(v.substr(v.rfind('/') + 1));
		if (!id) {
			r_rows.add("W9", name, false, "variant missing", "");
			continue;
		}
		const WeaponSimDef &d = p_in.table->defs[*id];
		const ExplosionSimDef &x = d.projectile.explosion;
		const bool numbers = d.projectile.time_to_arm == 30'000'000 && x.blast_damage == 112.0f && x.inner_radius == 0.35f && x.radius == 4.0f;
		// The trajectory: thrust from 40 m/s to the 100 m/s cap (6.0 s at 10 m/s^2 [inferred]).
		Projectile start;
		start.spawn_time = 0;
		start.base_velocity = { 0, 0, 40 };
		start.velocity = start.base_velocity;
		const Projectile at6 = fly_free(d, start, 360'000'000);
		const Projectile at3 = fly_free(d, start, 180'000'000);
		const float w10[1] = { 10.0f };
		const float w100[1] = { 100.0f };
		const std::unique_ptr<CollisionWorld> a = wall_world(w10, p_in.eye_height);
		const std::unique_ptr<CollisionWorld> b = wall_world(w100, p_in.eye_height);
		const Flight fa = fly_shot(p_in.table, *id, *a, p_in.eye_height, 900);
		const Flight fb = fly_shot(p_in.table, *id, *b, p_in.eye_height, 900);
		const bool dud = !fa.detonations.empty() && fa.detonations[0].dud;
		const bool armed = !fb.detonations.empty() && !fb.detonations[0].dud;
		r_rows.add("W9", name + ": armed after 0.5 s, blast 112 / 0.35 / 4", numbers && dud && armed,
				fmt("arm %s, blast %.0f/%.2f/%.0f; 10 m wall %s, 100 m wall %s", units(d.projectile.time_to_arm).c_str(), double(x.blast_damage), double(x.inner_radius),
						double(x.radius), dud ? "dud" : "NOT a dud", armed ? "explosion" : "NOT an explosion"),
				"arm 30000000; blast 112 / 0.35 / 4; dud before the arm time, explosion after");
		r_rows.report("W9", name + " trajectory (thrust model [inferred]) and direct hit",
				fmt("speed %.2f m/s at 3 s, %.2f m/s at 6 s; direct hit %.0f x %.2f = %.1f (H:W7: the community says 100)", length(at3.velocity), length(at6.velocity),
						double(d.projectile.direct_damage), double(d.projectile.zone[size_t(HitZone::Chest)].protection_multiplier),
						double(d.projectile.direct_damage * d.projectile.zone[size_t(HitZone::Chest)].protection_multiplier)));
	}
}

} // namespace

std::span<const std::string_view> weapon_check_variants() {
	return kVariants;
}

std::vector<WeaponCheckRow> run_weapon_checks(const WeaponCheckInputs &p_in) {
	Rows rows(p_in.table != nullptr ? p_in.table->mechanics : Mechanics{});
	if (p_in.table == nullptr || p_in.oracle == nullptr) {
		rows.add("W1", "inputs", false, "no table", "a table and an infinite_magazine oracle table");
		return rows.take();
	}
	check_m16a3(rows, p_in);
	check_bolts(rows, p_in);
	check_bursts(rows, p_in);
	check_870(rows, p_in);
	check_m9(rows, p_in);
	check_drop(rows, p_in);
	check_range(rows, p_in);
	check_damage(rows, p_in);
	check_reloads(rows, p_in);
	check_explosives(rows, p_in);
	return rows.take();
}

std::string format_weapon_checks(std::span<const WeaponCheckRow> p_rows) {
	std::string out;
	size_t pass = 0, gated = 0;
	for (const WeaponCheckRow &r : p_rows) {
		const char *verdict = r.reported ? "INFO" : (r.pass ? "PASS" : "FAIL");
		out += fmt("%s %s %s: ", verdict, r.gate.c_str(), r.name.c_str()) + r.measured + "  (" + r.threshold + ")" + (r.unmeasured ? "  unmeasured" : "") + "\n";
		if (!r.reported) {
			++gated;
			pass += r.pass ? 1 : 0;
		}
	}
	out += fmt("WEAPON-SIM CHECK: %s (%zu/%zu)\n", pass == gated ? "PASS" : "FAIL", pass, gated);
	return out;
}

} // namespace bf3::sim::weapons
