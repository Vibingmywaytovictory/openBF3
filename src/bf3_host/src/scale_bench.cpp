// run_scale_bench and format_scale_report (scale_bench.h; spike S1, docs/design/s1.md §1, §6): 64 walker bots on the
// real GameServer, ticked back to back for 120 s of sim time with every tick's parts timed, then a paced row and the
// direct per-query costs replayed on the bench's own CollisionWorld.
#include "bf3/host/scale_bench.h"

#include "tick_timer.h"

#include "bf3/core/install.h"
#include "bf3/core/level/mode_areas.h"
#include "bf3/host/game_server.h"
#include "bf3/host/level_load.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"

#include <windows.h>
// windows.h first
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <optional>

namespace bf3::host {

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point p_start) {
	return std::chrono::duration<double>(Clock::now() - p_start).count();
}

double ms_between(Clock::time_point p_a, Clock::time_point p_b) {
	return std::chrono::duration<double, std::milli>(p_b - p_a).count();
}

uint64_t working_set() {
	PROCESS_MEMORY_COUNTERS pmc{};
	if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
		return uint64_t(pmc.WorkingSetSize);
	}
	return 0;
}

uint64_t peak_working_set() {
	PROCESS_MEMORY_COUNTERS pmc{};
	if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
		return uint64_t(pmc.PeakWorkingSetSize);
	}
	return 0;
}

double thread_cpu_seconds() {
	FILETIME create, exit, kernel, user;
	if (!GetThreadTimes(GetCurrentThread(), &create, &exit, &kernel, &user)) {
		return 0.0;
	}
	const auto to100ns = [](const FILETIME &p_t) { return (uint64_t(p_t.dwHighDateTime) << 32) | p_t.dwLowDateTime; };
	return double(to100ns(kernel) + to100ns(user)) * 1e-7;
}

Percentiles percentiles_impl(std::vector<double> p_values) {
	Percentiles p;
	if (p_values.empty()) {
		return p;
	}
	std::sort(p_values.begin(), p_values.end());
	const auto rank = [&](double p_q) {
		// Nearest rank: the smallest value with at least q of the samples at or below it.
		const size_t n = p_values.size();
		size_t k = size_t(std::ceil(p_q * double(n)));
		k = std::clamp<size_t>(k, 1, n);
		return p_values[k - 1];
	};
	p.p50 = rank(0.50);
	p.p95 = rank(0.95);
	p.p99 = rank(0.99);
	p.max = p_values.back();
	double sum = 0.0;
	for (double v : p_values) {
		sum += v;
	}
	p.mean = sum / double(p_values.size());
	return p;
}

// Raises this thread's priority for the measured windows and restores it.
class PriorityScope {
public:
	explicit PriorityScope(bool p_raise) {
		if (p_raise) {
			previous_ = GetThreadPriority(GetCurrentThread());
			raised_ = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL) != 0;
		}
	}
	~PriorityScope() {
		if (raised_) {
			SetThreadPriority(GetCurrentThread(), previous_);
		}
	}

private:
	int previous_ = 0;
	bool raised_ = false;
};

struct SweepSample {
	sim::Vec3 feet{};
	sim::Vec3 velocity{};
};

std::string fmt(const char *p_format, ...) {
	char buf[512];
	va_list args;
	va_start(args, p_format);
	std::vsnprintf(buf, sizeof(buf), p_format, args);
	va_end(args);
	return buf;
}

std::string percentile_text(const Percentiles &p_p) {
	return fmt("p50 %.3f p95 %.3f p99 %.3f max %.3f mean %.3f", p_p.p50, p_p.p95, p_p.p99, p_p.max, p_p.mean);
}

} // namespace

Percentiles percentiles_of(std::vector<double> p_values) {
	return percentiles_impl(std::move(p_values));
}

TailShares tail_shares(std::span<const GameServerTickTimes> p_ticks, double p_p99) {
	TailShares tail;
	for (const GameServerTickTimes &t : p_ticks) {
		if (t.tick_ms >= p_p99) {
			++tail.ticks;
			tail.tick_ms += t.tick_ms;
			tail.commands_ms += t.commands_ms;
			tail.sources_ms += t.sources_ms;
			tail.step_ms += t.step_ms;
			tail.rules_ms += t.rules_ms;
			tail.encode_ms += t.encode_ms;
		}
	}
	if (tail.ticks == 0) {
		return tail;
	}
	const double n = double(tail.ticks);
	tail.tick_ms /= n;
	tail.commands_ms /= n;
	tail.sources_ms /= n;
	tail.step_ms /= n;
	tail.rules_ms /= n;
	tail.encode_ms /= n;
	if (tail.tick_ms > 0.0) {
		tail.commands = tail.commands_ms / tail.tick_ms;
		tail.sources = tail.sources_ms / tail.tick_ms;
		tail.step = tail.step_ms / tail.tick_ms;
		tail.rules = tail.rules_ms / tail.tick_ms;
		tail.encode = tail.encode_ms / tail.tick_ms;
		tail.other = std::max(0.0, 1.0 - tail.commands - tail.sources - tail.step - tail.rules - tail.encode);
	}
	return tail;
}

core::Result<ScaleReport> run_scale_bench(std::shared_ptr<const core::Install> p_install, const ScaleBenchOptions &p_options) {
	if (p_install == nullptr) {
		return core::make_error("scale bench: no install");
	}
	if (p_options.walkers > 120 || p_options.observers > 7 || p_options.walkers + p_options.observers > 128) {
		return core::make_error("scale bench: at most 120 walkers and 7 observers in 128 slots");
	}
	if (!(p_options.seconds > 0.0) || p_options.seconds > 3600.0 || !(p_options.warmup_seconds >= 0.0) || p_options.warmup_seconds > 600.0 ||
			!(p_options.paced_seconds >= 0.0) || p_options.paced_seconds > 600.0) {
		return core::make_error("scale bench: seconds out of range");
	}
	ScaleReport report;
	report.options = p_options;
	const uint64_t ws_start = working_set();

	// ---- load -----------------------------------------------------------------------------------------------------
	sim::MotorTuning tuning;
	if (!p_options.tuning_text.empty()) {
		core::Result<sim::MotorTuning> t = sim::parse_motor_tuning(p_options.tuning_text);
		if (!t) {
			return core::error_at("scale bench: tuning", t.error());
		}
		tuning = *t;
	}
	LoadProgress progress;
	Clock::time_point clock = Clock::now();
	std::optional<LevelDatabase> db;
	{
		core::Result<LevelDatabase> opened = open_level_database(p_install, p_options.level, p_options.mode, p_options.load_threads, &progress);
		if (!opened) {
			return core::error_at("scale bench: " + p_options.level + " " + p_options.mode, opened.error());
		}
		db.emplace(std::move(*opened));
	}
	report.seconds_database = seconds_since(clock);
	const sim::NavParams defaults;
	clock = Clock::now();
	core::Result<core::level::LevelCombatTiles> combat = core::level::level_combat_tiles(p_install, p_options.level, defaults.region_margin);
	if (!combat) {
		return core::error_at("scale bench: combat areas", combat.error());
	}
	report.seconds_areas = seconds_since(clock);
	clock = Clock::now();
	SharedLoadOptions shared_options;
	shared_options.cache = p_options.cache;
	shared_options.threads = p_options.load_threads;
	shared_options.tuning = tuning;
	shared_options.terrain_cover = combat->tiles;
	core::Result<std::shared_ptr<const SharedLevel>> shared = load_shared_level(*db, shared_options, &progress);
	if (!shared) {
		return core::error_at("scale bench: shared level", shared.error());
	}
	report.seconds_shared = seconds_since(clock);
	clock = Clock::now();
	BotLoadOptions bot_options;
	bot_options.cache = p_options.cache;
	bot_options.threads = p_options.load_threads;
	bot_options.combat = &*combat;
	bot_options.seed = p_options.seed;
	bot_options.synthetic_spawns = std::max<uint32_t>(p_options.walkers, 1);
	core::Result<std::shared_ptr<const BotLevel>> bots = load_bot_level(*db, **shared, bot_options, &progress);
	if (!bots) {
		return core::error_at("scale bench: bot level", bots.error());
	}
	report.seconds_bots = seconds_since(clock);
	report.nav = (*bots)->nav->stats();
	report.spawn_points_level = (*bots)->spawns.level_points;
	report.spawn_points_in_main = (*bots)->spawns.in_main;
	report.spawn_points_excluded = (*bots)->spawns.excluded;
	report.spawn_points_synthetic = (*bots)->spawns.synthetic;
	report.spawn_teams_merged = (*bots)->spawns.teams_merged;
	if ((*bots)->spawns.points.empty()) {
		return core::make_error("scale bench: no bot spawn point (no main-component spawn and no synthetic point)");
	}
	db.reset(); // the World and the mounted partitions: not the server's

	// ---- server ---------------------------------------------------------------------------------------------------
	const uint64_t ws_before_server = working_set();
	(void)ws_before_server;
	GameServer server;
	GameServerConfig config;
	config.level = *shared;
	config.max_slots = 128;
	config.seed = p_options.seed;
	auto fill = std::make_shared<BotFillConfig>();
	fill->level = *bots;
	fill->count = p_options.walkers;
	fill->walker = p_options.walker;
	fill->spawn_in_main = true;
	config.bots = fill;
	clock = Clock::now();
	BF3_TRY(server.init(config));
	report.seconds_init = seconds_since(clock);
	std::vector<OutDatagram> out;
	for (uint32_t i = 0; i < p_options.observers; ++i) {
		const uint32_t peer = 1000 + i;
		server.on_connect(peer);
		net::Hello hello;
		hello.name = "observer" + std::to_string(i);
		const std::vector<uint8_t> h = net::encode_hello(hello);
		server.on_receive(peer, net::kChannelReliable, h);
		net::Ready ready;
		ready.source_digest = (*shared)->source_digest;
		const std::vector<uint8_t> r = net::encode_ready(ready);
		server.on_receive(peer, net::kChannelReliable, r);
	}
	const uint64_t ws_after_init = working_set();
	report.server_working_set = ws_after_init > ws_start ? ws_after_init - ws_start : 0;

	// Bot slots: the server-side soldiers (init adds them before any peer; no scripted walkers here).
	std::vector<uint32_t> bot_slots;
	{
		const GameServerStats s = server.stats();
		for (uint32_t i = 0; i < s.slots.size(); ++i) {
			if (s.slots[i].used && !s.slots[i].remote) {
				bot_slots.push_back(i);
			}
		}
	}

	// ---- warm-up ----------------------------------------------------------------------------------------------------
	const uint64_t warmup_ticks = uint64_t(std::llround(p_options.warmup_seconds * double(sim::kSimHz)));
	std::vector<double> spawn_ticks;
	for (uint64_t t = 0; t < warmup_ticks; ++t) {
		server.tick(out);
		out.clear();
		const GameServerTickTimes &tt = server.last_tick_times();
		if (tt.spawns > 0) {
			spawn_ticks.push_back(tt.tick_ms);
		}
	}
	report.spawn_tick_ms = percentiles_impl(spawn_ticks);

	// ---- the gate window ---------------------------------------------------------------------------------------------
	const GameServerStats before = server.stats();
	const uint64_t window_ticks = uint64_t(std::llround(p_options.seconds * double(sim::kSimHz)));
	std::vector<double> v_tick, v_commands, v_sources, v_step, v_rules, v_encode;
	std::vector<double> v_paths;
	std::vector<GameServerTickTimes> window;
	window.reserve(window_ticks);
	v_tick.reserve(window_ticks);
	v_commands.reserve(window_ticks);
	v_sources.reserve(window_ticks);
	v_step.reserve(window_ticks);
	v_rules.reserve(window_ticks);
	v_encode.reserve(window_ticks);
	v_paths.reserve(window_ticks);
	std::vector<double> metres(bot_slots.size(), 0.0);
	std::vector<sim::Vec3> last(bot_slots.size());
	std::vector<uint32_t> last_life(bot_slots.size(), 0);
	for (size_t b = 0; b < bot_slots.size(); ++b) {
		if (const sim::SoldierState *st = server.soldier(bot_slots[b])) {
			last[b] = st->position;
			last_life[b] = st->life;
		}
	}
	constexpr uint32_t kSweepSamples = 4096;
	std::vector<SweepSample> samples;
	samples.reserve(kSweepSamples);
	const uint64_t sample_every = std::max<uint64_t>(1, (window_ticks * bot_slots.size()) / kSweepSamples);
	uint64_t sample_counter = 0;
	uint64_t motor_steps = 0, queries = 0;
	uint32_t respawns = 0;
	double step_ms_sum = 0.0;
	{
		PriorityScope priority(p_options.raise_priority);
		const double cpu_start = thread_cpu_seconds();
		const Clock::time_point wall_start = Clock::now();
		double bookkeeping = 0.0;
		for (uint64_t t = 0; t < window_ticks; ++t) {
			server.tick(out);
			const Clock::time_point after = Clock::now();
			const GameServerTickTimes &tt = server.last_tick_times();
			window.push_back(tt);
			v_tick.push_back(tt.tick_ms);
			v_commands.push_back(tt.commands_ms);
			v_sources.push_back(tt.sources_ms);
			v_step.push_back(tt.step_ms);
			v_rules.push_back(tt.rules_ms);
			v_encode.push_back(tt.encode_ms);
			v_paths.push_back(tt.paths_ms);
			motor_steps += tt.motor_steps;
			queries += tt.collision_queries;
			step_ms_sum += tt.step_ms;
			respawns += tt.spawns > 0 ? 1u : 0u;
			for (const OutDatagram &d : out) {
				if (d.channel != net::kChannelUnreliable) {
					continue;
				}
				size_t live = 0;
				for (uint32_t s : bot_slots) {
					const sim::SoldierState *st = server.soldier(s);
					live += (st != nullptr && (st->flags & sim::soldier_flags::kAlive) != 0) ? 1 : 0;
				}
				core::Result<net::Snapshot> snap = net::decode_snapshot(d.bytes);
				if (snap && live > snap->others.size()) {
					report.others_dropped_max = std::max<uint32_t>(report.others_dropped_max, uint32_t(live - snap->others.size()));
				}
			}
			out.clear();
			for (size_t b = 0; b < bot_slots.size(); ++b) {
				const sim::SoldierState *st = server.soldier(bot_slots[b]);
				if (st == nullptr) {
					continue;
				}
				if (st->life == last_life[b]) {
					const float dx = st->position[0] - last[b][0], dz = st->position[2] - last[b][2];
					const float d = std::sqrt(dx * dx + dz * dz);
					if (d < 5.0f) {
						metres[b] += double(d);
					}
				}
				last[b] = st->position;
				last_life[b] = st->life;
				if (samples.size() < kSweepSamples && (++sample_counter % sample_every) == 0 && (st->flags & sim::soldier_flags::kAlive) != 0) {
					samples.push_back({ st->position, st->velocity });
				}
			}
			bookkeeping += seconds_since(after);
		}
		report.window_wall_seconds = seconds_since(wall_start);
		report.window_cpu_seconds = thread_cpu_seconds() - cpu_start;
		// The bench's own bookkeeping between ticks runs on the same thread: both clocks include it, so the share
		// compares like with like.
		(void)bookkeeping;
	}
	report.cpu_share = report.window_wall_seconds > 0.0 ? report.window_cpu_seconds / report.window_wall_seconds : 0.0;
	const GameServerStats after = server.stats();
	report.ticks = window_ticks;
	report.tick_ms = percentiles_impl(v_tick);
	report.commands_ms = percentiles_impl(v_commands);
	report.sources_ms = percentiles_impl(v_sources);
	report.step_ms = percentiles_impl(v_step);
	report.rules_ms = percentiles_impl(v_rules);
	report.encode_ms = percentiles_impl(v_encode);
	report.bot_budget_ok = report.sources_ms.p99 <= kBotBudgetP99Ms;
	report.respawns_in_window = respawns;

	// Tail shares (docs §14): the ticks at or above the p99.
	report.tail = tail_shares(window, report.tick_ms.p99);

	// Motor (G3).
	report.motor_steps = motor_steps;
	report.collision_queries = queries;
	report.queries_per_step = motor_steps > 0 ? double(queries) / double(motor_steps) : 0.0;
	report.queries_per_second = p_options.seconds > 0.0 ? double(queries) / p_options.seconds : 0.0;
	report.us_per_step = motor_steps > 0 ? step_ms_sum * 1000.0 / double(motor_steps) : 0.0;
	report.us_per_query_upper = report.queries_per_step > 0.0 ? report.us_per_step / report.queries_per_step : 0.0;

	// Bots: the window's deltas.
	report.walkers = after.bots.walkers;
	{
		const sim::bots::WalkerStats &a = after.bots.walkers, &b = before.bots.walkers;
		sim::bots::WalkerStats &w = report.walkers;
		w.ticks = a.ticks - b.ticks;
		w.thinks = a.thinks - b.thinks;
		w.goals_started = a.goals_started - b.goals_started;
		w.goals_reached = a.goals_reached - b.goals_reached;
		w.goals_abandoned = a.goals_abandoned - b.goals_abandoned;
		w.path_requests = a.path_requests - b.path_requests;
		w.path_partial = a.path_partial - b.path_partial;
		w.path_failed = a.path_failed - b.path_failed;
		w.repaths = a.repaths - b.repaths;
		w.stuck_events = a.stuck_events - b.stuck_events;
		w.off_mesh_ticks = a.off_mesh_ticks - b.off_mesh_ticks;
		w.sprint_ticks = a.sprint_ticks - b.sprint_ticks;
		w.crouch_ticks = a.crouch_ticks - b.crouch_ticks;
		w.metres = a.metres - b.metres;
		w.starts_outside_main = a.starts_outside_main - b.starts_outside_main;
	}
	{
		const sim::NavQueryStats &a = after.bots.query, &b = before.bots.query;
		sim::NavQueryStats &q = report.query;
		q.nearest = a.nearest - b.nearest;
		q.paths_started = a.paths_started - b.paths_started;
		q.paths_done = a.paths_done - b.paths_done;
		q.paths_partial = a.paths_partial - b.paths_partial;
		q.paths_failed = a.paths_failed - b.paths_failed;
		q.path_iterations = a.path_iterations - b.path_iterations;
		q.corridor_moves = a.corridor_moves - b.corridor_moves;
		q.random_points = a.random_points - b.random_points;
		q.ms_nearest = a.ms_nearest - b.ms_nearest;
		q.ms_path = a.ms_path - b.ms_path;
		q.ms_corridor = a.ms_corridor - b.ms_corridor;
		const uint64_t paths = q.paths_done + q.paths_partial + q.paths_failed;
		report.us_per_path = paths > 0 ? q.ms_path * 1000.0 / double(paths) : 0.0;
		report.us_per_corridor_move = q.corridor_moves > 0 ? q.ms_corridor * 1000.0 / double(q.corridor_moves) : 0.0;
		report.us_per_nearest = q.nearest > 0 ? q.ms_nearest * 1000.0 / double(q.nearest) : 0.0;
	}
	{
		const sim::bots::PathServiceStats &a = after.bots.paths, &b = before.bots.paths;
		sim::bots::PathServiceStats &p = report.paths;
		p.runs = a.runs - b.runs;
		p.iterations = a.iterations - b.iterations;
		p.ticks_at_budget = a.ticks_at_budget - b.ticks_at_budget;
		p.requests = a.requests - b.requests;
		p.answers = a.answers - b.answers;
		p.pending_max = a.pending_max;
		p.iterations_last = a.iterations_last;
		p.ms_run = a.ms_run - b.ms_run;
		p.ms_run_max = a.ms_run_max;
		report.us_per_iteration = p.iterations > 0 ? p.ms_run * 1000.0 / double(p.iterations) : 0.0;
		report.ticks_at_budget_share = p.runs > 0 ? double(p.ticks_at_budget) / double(p.runs) : 0.0;
	}
	report.snapshot_bytes = after.snapshot_bytes - before.snapshot_bytes;
	report.max_snapshot_bytes = after.max_snapshot_bytes;

	// Validity (docs §1).
	report.bot_metres = metres;
	for (double m : metres) {
		report.bots_walked += m >= kValidityMetres ? 1u : 0u;
	}
	report.abandoned_share = report.walkers.goals_started > 0 ? double(report.walkers.goals_abandoned) / double(report.walkers.goals_started) : 0.0;
	report.valid = !metres.empty() && double(report.bots_walked) >= kValidityWalkedShare * double(metres.size()) &&
			report.abandoned_share <= kValidityAbandonedShare;

	// ---- paced row (not gated) ----------------------------------------------------------------------------------------
	const uint64_t paced_ticks = uint64_t(std::llround(p_options.paced_seconds * double(sim::kSimHz)));
	if (paced_ticks > 0) {
		PriorityScope priority(p_options.raise_priority);
		detail::TickTimer timer;
		std::vector<double> v;
		v.reserve(paced_ticks);
		const auto period = std::chrono::nanoseconds(1000000000ll / sim::kSimHz);
		Clock::time_point next = Clock::now() + period;
		for (uint64_t t = 0; t < paced_ticks; ++t) {
			timer.sleep_until(next);
			server.tick(out);
			out.clear();
			v.push_back(server.last_tick_times().tick_ms);
			next += period;
			const Clock::time_point now = Clock::now();
			if (now > next + period * 4) {
				next = now + period; // fell behind (a preempted thread): restart the cadence
			}
		}
		report.paced_ticks = paced_ticks;
		report.paced_tick_ms = percentiles_impl(std::move(v));
	}

	// ---- direct query costs (G3, docs §6.3) ---------------------------------------------------------------------------
	{
		core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create((*shared)->static_world);
		if (!world) {
			return core::error_at("scale bench: replay world", world.error());
		}
		const sim::MotorParams &motor = (*shared)->motor;
		const float radius = motor.radius;
		const float height = motor.poses[size_t(sim::Pose::Stand)].height;
		// The timer's own cost: empty pairs of clock reads.
		double overhead = 0.0;
		{
			constexpr int kEmpty = 4096;
			std::vector<double> e;
			e.reserve(kEmpty);
			for (int i = 0; i < kEmpty; ++i) {
				const Clock::time_point a = Clock::now();
				const Clock::time_point b = Clock::now();
				e.push_back(ms_between(a, b) * 1000.0);
			}
			overhead = percentiles_impl(e).mean;
		}
		std::vector<double> us_sweep, us_overlap;
		us_sweep.reserve(samples.size());
		us_overlap.reserve(samples.size());
		const float sprint = 7.0f; // m/s: about the soldier's sprint speed; 1/60 s of it along the velocity
		for (const SweepSample &s : samples) {
			sim::Vec3 dir{ s.velocity[0], 0.0f, s.velocity[2] };
			const float len = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
			if (len > 1e-3f) {
				dir = { dir[0] / len, 0.0f, dir[2] / len };
			} else {
				dir = { 0.0f, 0.0f, 1.0f };
			}
			const sim::Vec3 delta{ dir[0] * sprint / float(sim::kSimHz), 0.0f, dir[2] * sprint / float(sim::kSimHz) };
			const sim::Vec3 feet{ s.feet[0], s.feet[1] + 0.05f, s.feet[2] };
			Clock::time_point a = Clock::now();
			(void)(*world)->sweep_capsule(feet, radius, height, delta, sim::query_mask::kMovement);
			Clock::time_point b = Clock::now();
			us_sweep.push_back(std::max(0.0, ms_between(a, b) * 1000.0 - overhead));
			a = Clock::now();
			(void)(*world)->overlap_capsule(feet, radius, height, sim::query_mask::kMovement);
			b = Clock::now();
			us_overlap.push_back(std::max(0.0, ms_between(a, b) * 1000.0 - overhead));
		}
		report.sweep_samples = uint32_t(samples.size());
		report.us_sweep = percentiles_impl(std::move(us_sweep));
		report.us_overlap = percentiles_impl(std::move(us_overlap));
	}
	report.peak_working_set = peak_working_set();
	return report;
}

std::string format_scale_report(const ScaleReport &p_report) {
	const ScaleReport &r = p_report;
	std::string s;
	const auto line = [&](const std::string &p_text) { s += p_text + "\n"; };
	line(fmt("level %s %s walkers %u observers %u seed %llu warmup %.0f s window %.0f s (%llu ticks)", r.options.level.c_str(), r.options.mode.c_str(),
			r.options.walkers, r.options.observers, (unsigned long long)r.options.seed, r.options.warmup_seconds, r.options.seconds,
			(unsigned long long)r.ticks));
	line(fmt("load: database %.2f s, combat areas %.2f s, shared level %.2f s, bot level %.2f s (nav %s, %zu tiles, %zu empty, %zu polys, %zu drop links, "
			 "%.1f MiB), init %.3f s (initial spawns)",
			r.seconds_database, r.seconds_areas, r.seconds_shared, r.seconds_bots, r.nav.world_hit ? "hit" : "baked", r.nav.tiles, r.nav.tiles_empty, r.nav.polys,
			r.nav.drop_links, double(r.nav.data_bytes) / (1024.0 * 1024.0), r.seconds_init));
	line(fmt("spawn filter: level points %u, in main %u, excluded %u, teams merged %s, synthetic %u; starts outside main in the window %llu",
			r.spawn_points_level, r.spawn_points_in_main, r.spawn_points_excluded, r.spawn_teams_merged ? "yes" : "no", r.spawn_points_synthetic,
			(unsigned long long)r.walkers.starts_outside_main));
	line(fmt("G1 tick_ms %s  [gate p99 <= %.1f ms: %s]", percentile_text(r.tick_ms).c_str(), kScaleGateP99Ms,
			r.tick_ms.p99 <= kScaleGateP99Ms ? "PASS" : "FAIL"));
	line("   commands_ms " + percentile_text(r.commands_ms));
	line("   sources_ms  " + percentile_text(r.sources_ms) + fmt("  (plan bot budget p99 <= %.1f ms: %s)", kBotBudgetP99Ms, r.bot_budget_ok ? "ok" : "over"));
	line("   step_ms     " + percentile_text(r.step_ms));
	line("   rules_ms    " + percentile_text(r.rules_ms));
	line("   encode_ms   " + percentile_text(r.encode_ms));
	line(fmt("   tail (%llu ticks >= p99, mean %.3f ms): commands %.1f%% sources %.1f%% step %.1f%% rules %.1f%% encode %.1f%% other %.1f%%",
			(unsigned long long)r.tail.ticks, r.tail.tick_ms, 100.0 * r.tail.commands, 100.0 * r.tail.sources, 100.0 * r.tail.step, 100.0 * r.tail.rules,
			100.0 * r.tail.encode, 100.0 * r.tail.other));
	const double mean = r.tick_ms.mean > 0.0 ? r.tick_ms.mean : 1.0;
	line(fmt("   window mean shares: commands %.1f%% sources %.1f%% step %.1f%% rules %.1f%% encode %.1f%%", 100.0 * r.commands_ms.mean / mean,
			100.0 * r.sources_ms.mean / mean, 100.0 * r.step_ms.mean / mean, 100.0 * r.rules_ms.mean / mean, 100.0 * r.encode_ms.mean / mean));
	line(fmt("noise: window wall %.2f s, thread cpu %.2f s, cpu_share %.3f%s", r.window_wall_seconds, r.window_cpu_seconds, r.cpu_share,
			r.cpu_share < kNoisyCpuShare ? " (NOISY: the bench thread was preempted)" : ""));
	line(fmt("paced 60 Hz row (%llu ticks, not gated): tick_ms %s", (unsigned long long)r.paced_ticks, percentile_text(r.paced_tick_ms).c_str()));
	line(fmt("validity: %u / %zu bots walked >= %.0f m, abandoned %llu / %llu goals (%.1f%%): %s", r.bots_walked, r.bot_metres.size(), kValidityMetres,
			(unsigned long long)r.walkers.goals_abandoned, (unsigned long long)r.walkers.goals_started, 100.0 * r.abandoned_share, r.valid ? "VALID" : "INVALID"));
	if (!r.bot_metres.empty()) {
		std::vector<double> m = r.bot_metres;
		std::sort(m.begin(), m.end());
		line(fmt("   metres per bot: min %.0f p10 %.0f median %.0f max %.0f", m.front(), m[m.size() / 10], m[m.size() / 2], m.back()));
	}
	const sim::bots::WalkerStats &w = r.walkers;
	line(fmt("bots: goals started %llu reached %llu abandoned %llu, path requests %llu partial %llu failed %llu, repaths %llu, stuck %llu, off-mesh ticks %llu, "
			 "sprint ticks %llu, crouch ticks %llu, metres %.0f, respawns in window %u",
			(unsigned long long)w.goals_started, (unsigned long long)w.goals_reached, (unsigned long long)w.goals_abandoned, (unsigned long long)w.path_requests,
			(unsigned long long)w.path_partial, (unsigned long long)w.path_failed, (unsigned long long)w.repaths, (unsigned long long)w.stuck_events,
			(unsigned long long)w.off_mesh_ticks, (unsigned long long)w.sprint_ticks, (unsigned long long)w.crouch_ticks, w.metres, r.respawns_in_window));
	line(fmt("G3 motor: steps %llu, queries %llu, queries/step %.2f, queries/s %.0f, us/step %.2f, us/query upper %.3f, direct us_sweep mean %.3f p99 %.3f, "
			 "us_overlap mean %.3f p99 %.3f (%u samples)  [%s]",
			(unsigned long long)r.motor_steps, (unsigned long long)r.collision_queries, r.queries_per_step, r.queries_per_second, r.us_per_step,
			r.us_per_query_upper, r.us_sweep.mean, r.us_sweep.p99, r.us_overlap.mean, r.us_overlap.p99, r.sweep_samples,
			(r.motor_steps > 0 && r.queries_per_step > 0 && r.queries_per_second > 0 && r.us_per_step > 0 && r.us_sweep.mean > 0 && r.us_overlap.mean > 0)
					? "measured"
					: "MISSING"));
	line(fmt("nav queries: paths %llu (done %llu partial %llu failed %llu) %.1f us each, iterations %llu; corridor moves %llu %.2f us each; nearest %llu %.2f us "
			 "each; random points %llu",
			(unsigned long long)r.query.paths_started, (unsigned long long)r.query.paths_done, (unsigned long long)r.query.paths_partial,
			(unsigned long long)r.query.paths_failed, r.us_per_path, (unsigned long long)r.query.path_iterations, (unsigned long long)r.query.corridor_moves,
			r.us_per_corridor_move, (unsigned long long)r.query.nearest, r.us_per_nearest, (unsigned long long)r.query.random_points));
	line(fmt("path service: runs %llu, iterations %llu (%.3f us each, upper bound), ticks at budget %llu (%.2f%%), pending max %u, longest run %.3f ms",
			(unsigned long long)r.paths.runs, (unsigned long long)r.paths.iterations, r.us_per_iteration, (unsigned long long)r.paths.ticks_at_budget,
			100.0 * r.ticks_at_budget_share, r.paths.pending_max, r.paths.ms_run_max));
	line(fmt("snapshots: %llu bytes in the window, max %zu B, others dropped by the cap (max) %u", (unsigned long long)r.snapshot_bytes, r.max_snapshot_bytes,
			r.others_dropped_max));
	line(fmt("memory: server working set %.1f MiB (plan M4 <= %.1f MiB: %s), peak working set %.1f MiB", double(r.server_working_set) / (1024.0 * 1024.0),
			double(kServerMemoryBudgetBytes) / (1024.0 * 1024.0), r.server_working_set <= kServerMemoryBudgetBytes ? "ok" : "over",
			double(r.peak_working_set) / (1024.0 * 1024.0)));
	return s;
}

} // namespace bf3::host
