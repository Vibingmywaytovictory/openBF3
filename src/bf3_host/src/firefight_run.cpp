#include "bf3/host/firefight_run.h"

#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/finite.h"
#include "bf3/core/util/percentile.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/nav.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/weapons/ballistics.h"
#include "bf3/sim/weapons/weapon_sim.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>

namespace bf3::host {

namespace {

namespace sw = sim::weapons;
using Clock = std::chrono::steady_clock;

using bf3::core::seconds_since;

bool is_alive(const sim::SoldierState &p_state) {
	return (p_state.flags & sim::soldier_flags::kAlive) != 0 && p_state.mode != sim::MoveMode::Dead;
}

using bf3::core::finite;
using bf3::core::vec::horizontal_distance;

struct Fnv {
	uint64_t h = 0xcbf29ce484222325ull;
	void bytes(const void *p_data, size_t p_size) {
		const uint8_t *b = static_cast<const uint8_t *>(p_data);
		for (size_t i = 0; i < p_size; ++i) {
			h ^= b[i];
			h *= 0x100000001b3ull;
		}
	}
	template <typename T>
	void add(const T &p_value) {
		static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
		bytes(&p_value, sizeof(T));
	}
	void vec(const sim::Vec3 &p_v) {
		add(p_v[0]), add(p_v[1]), add(p_v[2]);
	}
	void dvec(const sw::DVec3 &p_v) {
		add(p_v[0]), add(p_v[1]), add(p_v[2]);
	}
};

class Recorder final : public GunplayObserver {
public:
	std::vector<uint8_t> teams;
	std::vector<uint8_t> remote;
	uint64_t peer_hits = 0;
	uint32_t peer_deaths = 0;
	std::vector<uint64_t> shots;
	std::vector<uint32_t> kills;
	std::vector<uint64_t> hits;
	uint64_t applied = 0;
	uint32_t team_kills = 0;
	std::array<uint32_t, 3> team_kill_count{};
	uint32_t live_peak = 0;
	bool hashing = false;
	Fnv tick;

	explicit Recorder(size_t p_slots) :
			shots(p_slots, 0), kills(p_slots, 0), hits(p_slots, 0) {}

	void weapon_tick(uint8_t p_slot, const sim::Command &, const sw::SoldierWeaponsState &, const sw::WeaponTickOutput &p_out) override {
		if (p_slot < shots.size()) {
			shots[p_slot] += p_out.shot_count;
		}
	}
	void projectiles(uint32_t p_tick, const sw::ProjectileStepOutput &p_out, std::span<const sw::Projectile> p_live) override {
		live_peak = std::max<uint32_t>(live_peak, uint32_t(p_live.size()));
		if (!hashing) {
			return;
		}
		tick.add(p_tick);
		tick.add(uint64_t(p_live.size()));
		for (const sw::Projectile &p : p_live) {
			tick.add(p.time), tick.add(p.spawn_time), tick.add(p.base_time);
			tick.dvec(p.base_position), tick.dvec(p.base_velocity), tick.dvec(p.position), tick.dvec(p.velocity), tick.dvec(p.origin);
			tick.add(p.path_length), tick.add(p.seed), tick.add(p.player_slot), tick.add(p.shot_index), tick.add(p.def), tick.add(p.pellet);
			tick.add(p.flags), tick.add(p.damage_scale), tick.add(p.bounces), tick.add(p.passes);
		}
		tick.add(uint64_t(p_out.impacts.size()));
		for (const sw::ImpactEvent &i : p_out.impacts) {
			tick.add(i.time), tick.vec(i.position), tick.vec(i.normal), tick.add(i.player_slot), tick.add(i.shot_index), tick.add(i.kind);
			tick.add(i.target), tick.add(i.zone), tick.add(i.grid_pair), tick.add(i.penetrated);
		}
		tick.add(uint64_t(p_out.detonations.size()));
		for (const sw::DetonationEvent &d : p_out.detonations) {
			tick.add(d.time), tick.vec(d.position), tick.add(d.player_slot), tick.add(d.shot_index), tick.add(d.dud), tick.add(d.timeout);
		}
	}
	void damage(uint32_t p_tick, const sw::DamageEvent &p_event, bool p_applied, float p_health_after) override {
		if (hashing) {
			tick.add(p_tick), tick.add(p_event.time), tick.add(p_event.attacker_slot), tick.add(p_event.target), tick.add(p_event.shot_index);
			tick.add(p_event.pellet), tick.add(p_event.kind), tick.add(p_event.zone), tick.add(p_event.amount), tick.add(p_applied), tick.add(p_health_after);
		}
		if (!p_applied) {
			return;
		}
		++applied;
		const uint32_t a = p_event.attacker_slot, v = p_event.target;
		const uint8_t ta = a < teams.size() ? teams[a] : 0;
		const uint8_t tv = v < teams.size() ? teams[v] : 0;
		const bool enemy = a != v && ta != 0 && tv != 0 && ta != tv;
		if (enemy && a < hits.size()) {
			++hits[a];
		}
		const bool peer_victim = v < remote.size() && remote[v] != 0 && !(a < remote.size() && remote[a] != 0);
		if (enemy && peer_victim) {
			++peer_hits;
			if (p_health_after <= 0.0f) {
				++peer_deaths;
			}
		}
		if (p_health_after <= 0.0f && a != v) {
			if (enemy) {
				if (a < kills.size()) {
					++kills[a];
				}
				++team_kill_count[ta <= 2 ? ta : 0];
			} else if (ta != 0 && ta == tv) {
				++team_kills;
			} else {
				++team_kill_count[0];
			}
		}
	}
};

struct BotTrack {
	uint8_t slot = 0;
	uint32_t life = 0;
	bool alive = false;
	sim::Vec3 last{};
	bool window_open = false;
	sim::Tick window_start = 0;
	uint32_t window_life = 0;
	sim::Vec3 window_origin{};
	float window_max = 0.0f;
};

}

FirefightPercentiles firefight_percentiles(std::vector<double> p_samples) {
	FirefightPercentiles p;
	if (p_samples.empty()) {
		return p;
	}
	std::sort(p_samples.begin(), p_samples.end());
	p.p50 = core::nearest_rank(p_samples, 0.50);
	p.p99 = core::nearest_rank(p_samples, 0.99);
	p.max = p_samples.back();
	double sum = 0.0;
	for (double v : p_samples) {
		sum += v;
	}
	p.mean = sum / double(p_samples.size());
	return p;
}

FirefightCounters firefight_counters(const FirefightStats &p_stats) {
	FirefightCounters c;
	const auto r = std::memory_order_relaxed;
	c.bots = p_stats.bots.load(r);
	c.lanes = p_stats.lanes.load(r);
	c.lanes_rejected = p_stats.lanes_rejected.load(r);
	c.lanes_shared = p_stats.lanes_shared.load(r);
	c.walk_unavailable = p_stats.walk_unavailable.load(r);
	c.bursts = p_stats.bursts.load(r);
	c.reloads = p_stats.reloads.load(r);
	c.retargets = p_stats.retargets.load(r);
	c.no_target_fire = p_stats.no_target_fire.load(r);
	c.placements = p_stats.placements.load(r);
	c.los_rays = p_stats.los_rays.load(r);
	c.hunts = p_stats.hunts.load(r);
	c.strafes = p_stats.strafes.load(r);
	c.mode_switches = p_stats.mode_switches.load(r);
	c.sidearm_switches = p_stats.sidearm_switches.load(r);
	return c;
}

core::Result<FirefightLevel> load_firefight_level(std::shared_ptr<const core::Install> p_install, std::string_view p_level, std::string_view p_mode,
		const FirefightLoadOptions &p_options) {
	if (p_install == nullptr) {
		return core::make_error("firefight level: no install");
	}
	FirefightLevel out;
	Clock::time_point clock = Clock::now();
	core::Result<LevelDatabase> db = open_level_database(p_install, p_level, p_mode, p_options.threads, nullptr);
	if (!db) {
		return core::error_at("firefight level: database", db.error());
	}
	out.seconds_database = seconds_since(clock);
	clock = Clock::now();
	SharedLoadOptions so;
	so.cache = p_options.cache;
	so.threads = p_options.threads;
	so.tuning = p_options.tuning;
	so.gunplay = true;
	core::Result<std::shared_ptr<const SharedLevel>> shared = load_shared_level(*db, so, nullptr);
	if (!shared) {
		return core::error_at("firefight level: shared level", shared.error());
	}
	out.shared = *shared;
	out.seconds_shared = seconds_since(clock);
	clock = Clock::now();
	BotLoadOptions bo;
	bo.cache = p_options.cache;
	bo.threads = p_options.threads;
	bo.seed = p_options.seed;
	core::Result<std::shared_ptr<const BotLevel>> bots = load_bot_level(*db, *out.shared, bo, nullptr);
	if (!bots) {
		return core::error_at("firefight level: bot level", bots.error());
	}
	out.bots = *bots;
	out.seconds_bots = seconds_since(clock);
	out.nav_cache_hit = out.bots->nav != nullptr && out.bots->nav->stats().world_hit;
	return out;
}

core::Result<FirefightReport> run_firefight(std::shared_ptr<const SharedLevel> p_level, std::shared_ptr<const BotLevel> p_bots,
		const FirefightRunOptions &p_options) {
	if (p_level == nullptr || p_level->gunplay == nullptr) {
		return core::make_error("firefight run: the level has no gunplay tables (SharedLoadOptions::gunplay)");
	}
	if (p_options.count == 0 || p_options.count + p_options.observers > p_options.max_slots) {
		return core::make_error("firefight run: " + std::to_string(p_options.count) + " bots and " + std::to_string(p_options.observers) +
				" observers for " + std::to_string(p_options.max_slots) + " slots");
	}
	FirefightReport report;
	report.mode = p_options.mode;
	report.count = p_options.count;
	report.observers = p_options.observers;
	report.max_slots = p_options.max_slots;
	report.seed = p_options.seed;
	report.level = p_level->level;
	report.mode_name = p_level->mode;

	auto stats = std::make_shared<FirefightStats>();
	auto ff = std::make_shared<FirefightConfig>();
	ff->mode = p_options.mode;
	ff->count = p_options.count;
	ff->observer = p_options.observer;
	ff->walk = p_options.walk;
	ff->stats = stats;
	if (p_options.configure_firefight) {
		p_options.configure_firefight(*ff);
	}
	auto recorder = std::make_shared<Recorder>(p_options.max_slots);
	GameServerConfig config;
	config.level = p_level;
	config.max_slots = p_options.max_slots;
	config.seed = p_options.seed;
	config.respawn_ticks = p_options.respawn_ticks;
	config.dev_commands = true;
	config.gunplay.observer = recorder;
	if (!p_options.mechanics_cfg.empty()) {
		config.gunplay.mechanics_cfg = p_options.mechanics_cfg;
	}
	if (p_bots != nullptr) {
		auto fill = std::make_shared<BotFillConfig>();
		fill->level = p_bots;
		fill->count = 0;
		config.bots = fill;
	}
	config.firefight = ff;
	if (p_options.configure_server) {
		p_options.configure_server(config);
	}
	GameServer server;
	Clock::time_point clock = Clock::now();
	BF3_TRY(server.init(config));
	report.seconds_init = seconds_since(clock);
	for (uint32_t i = 0; i < p_options.observers; ++i) {
		const uint32_t peer = 1000 + i;
		server.on_connect(peer);
		net::Hello hello;
		hello.name = "observer" + std::to_string(i);
		hello.game_build = net::game_build();
		const std::vector<uint8_t> h = net::encode_hello(hello);
		server.on_receive(peer, net::kChannelReliable, h);
		net::Ready ready;
		ready.source_digest = p_level->source_digest;
		const std::vector<uint8_t> r = net::encode_ready(ready);
		server.on_receive(peer, net::kChannelReliable, r);
	}
	const Gunplay *gunplay = server.gunplay();
	if (gunplay == nullptr) {
		return core::make_error("firefight run: the server has no gunplay");
	}
	report.match_seed = gunplay->match_seed();
	report.weapon_hash = gunplay->weapon_hash();
	const ServerView &view = server.view();
	const sim::CollisionWorld &world = view.world();
	const sim::MotorParams &motor = p_level->motor;

	std::vector<BotTrack> tracks;
	{
		const GameServerStats s = server.stats();
		for (uint32_t i = 0; i < s.slots.size(); ++i) {
			if (s.slots[i].used && !s.slots[i].remote) {
				BotTrack t;
				t.slot = uint8_t(i);
				tracks.push_back(t);
			}
		}
	}
	report.bots.resize(tracks.size());
	for (size_t b = 0; b < tracks.size(); ++b) {
		report.bots[b].slot = tracks[b].slot;
		report.bots[b].team = view.team(tracks[b].slot);
		report.bots[b].kit = gunplay->kit(tracks[b].slot);
	}
	const auto refresh_teams = [&]() {
		for (size_t s = 0; s < recorder->teams.size(); ++s) {
			recorder->teams[s] = view.team(uint8_t(s));
		}
	};
	recorder->teams.assign(p_options.max_slots, 0);
	recorder->remote.assign(p_options.max_slots, 0);
	uint32_t first_peer = UINT32_MAX;
	{
		const GameServerStats s = server.stats();
		for (uint32_t i = 0; i < s.slots.size() && i < p_options.max_slots; ++i) {
			if (s.slots[i].used && s.slots[i].remote) {
				recorder->remote[i] = 1;
				if (report.peer_slots == 0) {
					first_peer = i;
				}
				++report.peer_slots;
			}
		}
	}

	std::vector<OutDatagram> out;

	clock = Clock::now();
	for (uint32_t t = 0; t < p_options.warmup_ticks; ++t) {
		refresh_teams();
		server.tick(out);
		out.clear();
	}
	report.seconds_warmup = seconds_since(clock);
	const std::vector<uint64_t> shots_before = recorder->shots;
	const std::vector<uint32_t> kills_before = recorder->kills;
	const std::vector<uint64_t> hits_before = recorder->hits;
	const std::array<uint32_t, 3> team_kills_before = recorder->team_kill_count;
	const uint32_t team_kills_base = recorder->team_kills;
	const uint64_t applied_before = recorder->applied;
	const uint64_t peer_hits_before = recorder->peer_hits;
	const uint32_t peer_deaths_before = recorder->peer_deaths;
	const GunplayStats gunplay_before = gunplay->stats();
	const uint64_t pre_before = server.stats().step_precondition_failures;
	for (BotTrack &t : tracks) {
		if (const sim::SoldierState *st = server.soldier(t.slot)) {
			t.life = st->life;
			t.alive = is_alive(*st);
			t.last = st->position;
		}
	}

	std::vector<double> v_tick, v_sources, v_step, v_rules, v_encode, v_weapons, v_projectiles, v_rewind;
	v_tick.reserve(p_options.ticks);
	if (p_options.hashes) {
		report.tick_hashes.reserve(p_options.ticks);
	}
	recorder->hashing = p_options.hashes;
	clock = Clock::now();
	for (uint32_t t = 0; t < p_options.ticks; ++t) {
		refresh_teams();
		recorder->tick = Fnv();
		server.tick(out);
		out.clear();
		const GameServerTickTimes &tt = server.last_tick_times();
		const GunplayStats &gs = gunplay->stats();
		v_tick.push_back(tt.tick_ms);
		v_sources.push_back(tt.sources_ms);
		v_step.push_back(tt.step_ms);
		v_rules.push_back(tt.rules_ms);
		v_encode.push_back(tt.encode_ms);
		v_weapons.push_back(gs.weapons_ms);
		v_projectiles.push_back(gs.projectiles_ms);
		v_rewind.push_back(gs.rewind_ms);
		const sim::Tick now = server.current_tick();
		if (p_options.hashes) {
			Fnv h = recorder->tick;
			h.add(now);
			for (uint32_t s = 0; s < p_options.max_slots; ++s) {
				const sim::SoldierState *st = server.soldier(s);
				if (st == nullptr) {
					continue;
				}
				h.add(s);
				h.add(sim::soldier_state_hash(*st));
				h.add(sw::soldier_weapons_hash(gunplay->weapons(uint8_t(s))));
				h.add(view.team(uint8_t(s)));
			}
			report.tick_hashes.push_back(h.h);
		}
		if (first_peer < p_options.max_slots) {
			if (const sim::SoldierState *st = server.soldier(first_peer); st != nullptr && is_alive(*st)) {
				++report.peer_alive_ticks;
				report.peer_protected_ticks += (st->flags & sim::soldier_flags::kSpawnProtected) != 0 ? 1u : 0u;
				if (p_options.check_every > 0 && (t % p_options.check_every) == 0) {
					++report.peer_checks;
					const uint8_t pt = view.team(uint8_t(first_peer));
					const size_t ppose = std::min<size_t>(size_t(st->pose), sim::kPoseCount - 1);
					const sim::Vec3 chest{ st->position[0], st->position[1] + 0.8f * motor.poses[ppose].eye_height, st->position[2] };
					for (const BotTrack &k : tracks) {
						const sim::SoldierState *bs = server.soldier(k.slot);
						if (bs == nullptr || !is_alive(*bs) || view.team(k.slot) == pt) {
							continue;
						}
						const size_t bpose = std::min<size_t>(size_t(bs->pose), sim::kPoseCount - 1);
						const sim::Vec3 eye{ bs->position[0], bs->position[1] + motor.poses[bpose].eye_height, bs->position[2] };
						const sim::Vec3 d{ chest[0] - eye[0], chest[1] - eye[1], chest[2] - eye[2] };
						if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 150.0f * 150.0f) {
							continue;
						}
						if (!world.ray(eye, d, sim::query_mask::kProjectile)) {
							++report.peer_exposed;
							break;
						}
					}
				}
			}
		}
		const bool check = p_options.check_every > 0 && (t % p_options.check_every) == 0;
		for (size_t b = 0; b < tracks.size(); ++b) {
			BotTrack &k = tracks[b];
			FirefightBotRow &row = report.bots[b];
			const sim::SoldierState *st = server.soldier(k.slot);
			if (st == nullptr) {
				continue;
			}
			const bool alive_now = is_alive(*st);
			if (k.alive && !alive_now) {
				++row.deaths;
				++report.deaths[row.team <= 2 ? row.team : 0];
			}
			if (st->life != k.life) {
				++row.lives;
				k.life = st->life;
				k.window_open = false;
				k.last = st->position;
			}
			if (alive_now) {
				const float step = horizontal_distance(k.last, st->position);
				if (step < 5.0f && std::isfinite(step)) {
					row.metres += double(step);
				}
				k.last = st->position;
				if (!k.window_open) {
					k.window_open = true;
					k.window_start = now;
					k.window_life = st->life;
					k.window_origin = st->position;
					k.window_max = 0.0f;
				}
				k.window_max = std::max(k.window_max, horizontal_distance(k.window_origin, st->position));
				if (now - k.window_start >= p_options.window_ticks) {
					++row.windows;
					++report.windows;
					row.min_window_metres = row.windows == 1 ? k.window_max : std::min(row.min_window_metres, k.window_max);
					if (k.window_max < p_options.stuck_metres) {
						++row.stuck_windows;
						++report.stuck_windows;
					}
					k.window_open = false;
				}
			} else {
				k.window_open = false;
			}
			k.alive = alive_now;
			if (check && alive_now) {
				++report.checks;
				if (!finite(st->position) || !finite(st->velocity) || !std::isfinite(st->health)) {
					++report.non_finite;
					continue;
				}
				const size_t pose = std::min<size_t>(size_t(st->pose), sim::kPoseCount - 1);
				const float height = std::max(motor.poses[pose].height, 2.0f * motor.radius + 0.05f);
				const sim::Vec3 feet{ st->position[0], st->position[1] + 0.05f, st->position[2] };
				if (world.overlap_capsule(feet, motor.radius, height, sim::query_mask::kMovement, kInsideToleranceMetres)) {
					++report.inside_geometry;
					if (report.inside_geometry <= 8) {
						std::fprintf(stderr, "firefight: inside geometry: tick %u slot %u at %.2f %.2f %.2f move mode %d\n", now, unsigned(k.slot), st->position[0],
								st->position[1], st->position[2], int(st->mode));
					}
				}

				bool ground = false;
				const float rim = 0.9f * motor.radius;
				const float offsets[5][2] = { { 0.0f, 0.0f }, { rim, 0.0f }, { -rim, 0.0f }, { 0.0f, rim }, { 0.0f, -rim } };
				for (const auto &o : offsets) {
					sim::StaticHit hit;
					const sim::Vec3 probe{ st->position[0] + o[0], st->position[1] + 0.5f, st->position[2] + o[1] };
					if (world.ground_below(probe, kBelowProbeMetres, sim::query_mask::kMovement, &hit)) {
						ground = true;
						break;
					}
				}
				if (!ground) {
					++report.below_ground;
					if (report.below_ground <= 8) {
						std::fprintf(stderr, "firefight: below the ground: tick %u slot %u at %.2f %.2f %.2f move mode %d\n", now, unsigned(k.slot), st->position[0],
								st->position[1], st->position[2], int(st->mode));
					}
				}
			}
		}
	}
	report.seconds_window = seconds_since(clock);
	report.ticks = p_options.ticks;
	for (uint64_t h : report.tick_hashes) {
		Fnv f;
		f.h = report.run_hash == 0 ? Fnv().h : report.run_hash;
		f.add(h);
		report.run_hash = f.h;
	}
	report.tick_ms = firefight_percentiles(std::move(v_tick));
	report.sources_ms = firefight_percentiles(std::move(v_sources));
	report.step_ms = firefight_percentiles(std::move(v_step));
	report.rules_ms = firefight_percentiles(std::move(v_rules));
	report.encode_ms = firefight_percentiles(std::move(v_encode));
	report.weapons_ms = firefight_percentiles(std::move(v_weapons));
	report.projectiles_ms = firefight_percentiles(std::move(v_projectiles));
	report.rewind_ms = firefight_percentiles(std::move(v_rewind));
	report.peak_projectiles = recorder->live_peak;
	for (size_t b = 0; b < tracks.size(); ++b) {
		FirefightBotRow &row = report.bots[b];
		const uint8_t s = row.slot;
		row.shots = recorder->shots[s] - shots_before[s];
		row.kills = recorder->kills[s] - kills_before[s];
		row.hits = recorder->hits[s] - hits_before[s];
		report.shots[row.team <= 2 ? row.team : 0] += row.shots;
		report.bots_fired += row.shots > 0 ? 1u : 0u;
		report.bots_hit_enemy += row.hits > 0 ? 1u : 0u;
		report.bots_killed_enemy += row.kills > 0 ? 1u : 0u;
		report.bots_stuck += row.stuck_windows > 0 ? 1u : 0u;
	}
	for (size_t i = 0; i < 3; ++i) {
		report.kills[i] = recorder->team_kill_count[i] - team_kills_before[i];
	}
	report.team_kills = recorder->team_kills - team_kills_base;
	report.hits = recorder->applied - applied_before;
	report.peer_hits = recorder->peer_hits - peer_hits_before;
	report.peer_deaths = recorder->peer_deaths - peer_deaths_before;
	if (first_peer < p_options.max_slots) {
		report.peer_team = view.team(uint8_t(first_peer));
	}
	const GameServerStats end = server.stats();
	report.step_precondition_failures = end.step_precondition_failures - pre_before;
	report.gunplay = gunplay->stats();
	report.weapon_precondition_failures = report.gunplay.weapon_precondition_failures - gunplay_before.weapon_precondition_failures;
	report.counters = firefight_counters(*stats);
	report.bot_fill = end.bots;
	return report;
}

std::string firefight_report_text(const FirefightReport &p_report) {
	const FirefightReport &r = p_report;
	std::string s;
	char line[768];
	const auto put = [&](const char *p_format, auto... p_args) {
		std::snprintf(line, sizeof(line), p_format, p_args...);
		if (!s.empty()) {
			s += '\n';
		}
		s += line;
	};
	const double seconds = double(r.ticks) / double(sim::kSimHz);
	put("firefight %s %s: %s, %u bots, %u observer peers, seed %" PRIu64 ", match seed 0x%016" PRIx64 ", weapon hash 0x%016" PRIx64, r.level.c_str(),
			r.mode_name.c_str(), r.mode == FirefightMode::Pairs ? "pairs" : "free", r.count, r.observers, r.seed, r.match_seed, r.weapon_hash);
	if (r.max_slots > 64) {
		put("  note: %u slots; soldiers in slots 64 and above are never hit (lag_comp.cpp kMaxLagSlots = 64)", r.max_slots);
	}
	put("  init %.2f s, warm-up %.2f s wall, window %u ticks (%.1f s sim) in %.2f s wall", r.seconds_init, r.seconds_warmup, r.ticks, seconds, r.seconds_window);
	put("  kills team 1 %u, team 2 %u (no team %u, team kills %u); deaths team 1 %u, team 2 %u; hits %" PRIu64 "; shots team 1 %" PRIu64 ", team 2 %" PRIu64,
			r.kills[1], r.kills[2], r.kills[0], r.team_kills, r.deaths[1], r.deaths[2], r.hits, r.shots[1], r.shots[2]);
	if (r.peer_slots > 0) {
		put("  observer peers %u: hit by bots %" PRIu64 " times, killed %u times; the first (team %u) alive %u ticks, spawn protected %u, exposed to an enemy bot in "
			"%u of %u checks",
				r.peer_slots, r.peer_hits, r.peer_deaths, unsigned(r.peer_team), r.peer_alive_ticks, r.peer_protected_ticks, r.peer_exposed, r.peer_checks);
	}
	put("  bots fired %u / %zu, hit an enemy %u, killed an enemy %u; displacement windows %u, stuck %u (bots stuck %u)", r.bots_fired, r.bots.size(),
			r.bots_hit_enemy, r.bots_killed_enemy, r.windows, r.stuck_windows, r.bots_stuck);
	put("  checks %" PRIu64 ": inside geometry %" PRIu64 ", below ground %" PRIu64 ", non-finite %" PRIu64 "; precondition failures: motor %" PRIu64
		", weapons %" PRIu64,
			r.checks, r.inside_geometry, r.below_ground, r.non_finite, r.step_precondition_failures, r.weapon_precondition_failures);
	const FirefightCounters &c = r.counters;
	put("  brains: lanes %u (rejected tries %u, shared %u), placements %" PRIu64 ", bursts %" PRIu64 ", reloads %" PRIu64 ", retargets %" PRIu64
		", blind bursts %" PRIu64 ", hunts %" PRIu64 ", strafes %" PRIu64 ", mode steps %" PRIu64 ", sidearm %" PRIu64 ", rays %" PRIu64 "%s",
			c.lanes, c.lanes_rejected, c.lanes_shared, c.placements, c.bursts, c.reloads, c.retargets, c.no_target_fire, c.hunts, c.strafes, c.mode_switches,
			c.sidearm_switches, c.los_rays, c.walk_unavailable != 0 ? " (walk asked without a bot level)" : "");
	put("  gunplay: shots %" PRIu64 ", hits %" PRIu64 ", deaths %" PRIu64 ", damage dropped %" PRIu64 " (team %" PRIu64 ", protected %" PRIu64
		", dead victim %" PRIu64 "), projectiles refused %" PRIu64 ", budget cut %" PRIu64 ", peak live projectiles %u",
			r.gunplay.shots, r.gunplay.hits, r.gunplay.deaths, r.gunplay.damage_dropped, r.gunplay.team_damage_dropped, r.gunplay.protected_refused,
			r.gunplay.dead_victim_dropped, r.gunplay.projectiles_refused, r.gunplay.budget_exhausted, r.peak_projectiles);
	const auto row = [&](const char *p_name, const FirefightPercentiles &p) {
		put("  %-12s p50 %.3f ms  p99 %.3f ms  max %.3f ms  mean %.3f ms", p_name, p.p50, p.p99, p.max, p.mean);
	};
	row("tick", r.tick_ms);
	row("sources", r.sources_ms);
	row("motor", r.step_ms);
	row("rules", r.rules_ms);
	row("encode", r.encode_ms);
	row("weapons", r.weapons_ms);
	row("projectiles", r.projectiles_ms);
	row("rewind", r.rewind_ms);
	if (!r.tick_hashes.empty()) {
		put("  world hash over %zu ticks 0x%016" PRIx64 " (last tick 0x%016" PRIx64 ")", r.tick_hashes.size(), r.run_hash, r.tick_hashes.back());
	}
	return s;
}

}
