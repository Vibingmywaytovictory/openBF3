#include <doctest/doctest.h>

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/math.h"
#include "bf3/host/firefight_run.h"
#include "bf3/host/gunplay.h"
#include "bf3/net/enet_host.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/weapons/spread_recoil.h"
#include "client_tracks.h"
#include "server_ghosts.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define OPENBF3_GETPID _getpid
#else
#include <unistd.h>
#define OPENBF3_GETPID getpid
#endif

namespace {

using namespace bf3;

inline constexpr uint32_t kPeer = 7;
inline constexpr uint32_t kWalkers = 31;
inline constexpr uint32_t kShooters = 32;
inline constexpr sim::Tick kRunTicks = 20 * sim::kSimHz;
inline constexpr size_t kIntervalBytes = 1200;
inline constexpr double kBytesPerSecond = 36000.0;
inline constexpr sim::Tick kAttackerTicks = 5 * sim::kSimHz;
inline constexpr float kConeMarginCos = 0.99026807f;

std::string read_text(const std::filesystem::path &p_path) {
	std::ifstream in(p_path, std::ios::binary);
	std::ostringstream s;
	s << in.rdbuf();
	return s.str();
}

struct Fixture {
	test::Mounted mount;
	std::filesystem::path cache_root;
	host::FirefightLevel level;
};

Fixture *fixture() {
	static std::unique_ptr<Fixture> s_fixture;
	static bool s_tried = false;
	if (s_tried) {
		if (s_fixture == nullptr) {
			test::skip_install_case("no BF3 install or the load failed (see the first case)");
		}
		return s_fixture.get();
	}
	s_tried = true;
	auto f = std::make_unique<Fixture>();
	if (!test::open_or_skip(f->mount)) {
		return nullptr;
	}
	std::error_code ec;
	f->cache_root =
			std::filesystem::temp_directory_path(ec) / ("openbf3_host_ghost_" + std::to_string(OPENBF3_GETPID()));
	std::filesystem::remove_all(f->cache_root, ec);
	core::Cache cache;
	core::CacheOptions co;
	co.root = f->cache_root;
	co.guard = f->mount.reader.write_guard();
	std::string detail;
	REQUIRE_MESSAGE(cache.open(std::move(co), &detail) == core::CacheStatus::Ok, detail);
	core::Result<sim::MotorTuning> tuning = sim::parse_motor_tuning(read_text(OPENBF3_MOTOR_TUNING_CFG));
	REQUIRE_MESSAGE(tuning.ok(), (tuning.ok() ? std::string() : tuning.error().message));
	host::FirefightLoadOptions lo;
	lo.cache = &cache;
	lo.tuning = *tuning;
	const auto start = std::chrono::steady_clock::now();
	core::Result<host::FirefightLevel> level =
			host::load_firefight_level(f->mount.install, "MP_Subway", "ConquestLarge0", lo);
	REQUIRE_MESSAGE(level.ok(), (level.ok() ? std::string() : level.error().message));
	cache.close();
	f->level = std::move(*level);
	std::printf("ghost fixture: Metro ConquestLarge0 with gunplay and the bot level in %.2f s, %zu spawns\n",
			std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
			f->level.shared->spawns.size());
	s_fixture = std::move(f);
	return s_fixture.get();
}

struct Tier {
	float metres = 0;
	uint32_t every = 1;
};

inline constexpr std::array<Tier, 4> kEnemyFloor{ { { 100.0f, 1 }, { 300.0f, 3 }, { 800.0f, 6 }, { 1500.0f, 24 } } };
inline constexpr std::array<Tier, 3> kTeammateFloor{ { { 100.0f, 3 }, { 300.0f, 8 }, { 1.0e9f, 24 } } };

template <size_t N> std::optional<uint32_t> floor_of(const std::array<Tier, N> &p_tiers, float p_metres) {
	for (const Tier &t : p_tiers) {
		if (p_metres <= t.metres) {
			return t.every;
		}
	}
	return std::nullopt;
}

struct Ghost {
	bool in_scope = false;
	uint32_t life = 0;
	uint16_t scope_seq = 0;
	std::optional<uint16_t> last_seq;
	uint64_t sent = 0;
};

struct Interval {
	uint16_t seq = 0;
	sim::Tick tick = 0;
	size_t unreliable_bytes = 0;
	size_t all_bytes = 0;
	std::array<bool, net::kMaxSlots> carried{};
};

class Peer {
public:
	explicit Peer(host::GameServer &r_server) : server_(r_server) {}

	void join(const host::SharedLevel &p_level) {
		net::Hello hello;
		hello.name = "ghost-peer";
		hello.game_build = net::game_build();
		server_.on_receive(kPeer, net::kChannelReliable, net::encode_hello(hello));
		net::Ready ready;
		ready.source_digest = p_level.source_digest;
		server_.on_receive(kPeer, net::kChannelReliable, net::encode_ready(ready));
	}

	void receive(std::span<const host::OutDatagram> p_out, sim::Tick p_tick) {
		for (const host::OutDatagram &d : p_out) {
			if (d.peer != kPeer) {
				continue;
			}
			const size_t wire = net::enet_command_bytes(
					d.bytes.size(), d.reliable ? net::Host::SendMode::Reliable : net::Host::SendMode::Unsequenced);
			total_bytes += wire;
			if (!d.reliable) {
				pending_.unreliable_bytes += wire;
			}
			pending_.all_bytes += wire;
			const std::optional<net::MessageType> type = net::peek_type(d.bytes);
			const bool lost = !d.reliable && ((++unreliable_count_ * 2654435761u) >> 28) == 0;
			if (type == net::MessageType::MatchInfo) {
				if (const core::Result<net::MatchInfo> m = net::decode_match_info(d.bytes)) {
					slot = m->slot;
				}
			} else if (type == net::MessageType::CombatEvents) {
				const core::Result<net::CombatEvents> c = net::decode_combat_events(d.bytes, frame);
				if (!c) {
					++decode_failures;
					continue;
				}
				for (const net::HitEvent &h : c->hits) {
					if (slot >= 0 && h.victim == uint8_t(slot) && h.shooter < net::kMaxSlots) {
						attacked_until[h.shooter] =
								std::max(attacked_until[h.shooter], c->server_tick + kAttackerTicks);
					}
				}
			} else if (type == net::MessageType::Snapshot) {
				const core::Result<net::Snapshot> s = net::decode_snapshot(d.bytes, &baselines_);
				if (!s) {
					++decode_failures;
					continue;
				}
				pending_.seq = s->seq;
				pending_.tick = s->server_tick;
				have_interval_ = true;
				for (const net::OtherSoldier &o : s->others) {
					pending_.carried[o.slot] = true;
				}
				if (!lost) {
					baselines_.keep(s->seq, s->server_tick, s->own, s->own_weapons, s->others);
					arrive(s->seq);
				}
				lost_ += lost ? 1u : 0u;
			} else if (type == net::MessageType::SnapshotOthers) {
				const core::Result<net::SnapshotOthers> s = net::decode_snapshot_others(d.bytes, &baselines_);
				if (!s) {
					++decode_failures;
					continue;
				}
				for (const net::OtherSoldier &o : s->others) {
					pending_.carried[o.slot] = true;
				}
				++overflow;
				if (!lost) {
					baselines_.keep(s->seq, s->server_tick, std::nullopt, std::nullopt, s->others);
					others_[s->seq] = true;
				}
			}
		}
		if (have_interval_) {
			pending_.tick = p_tick;
			intervals.push_back(pending_);
		} else if (!intervals.empty()) {
			intervals.back().all_bytes += pending_.all_bytes;
			intervals.back().unreliable_bytes += pending_.unreliable_bytes;
		}
		pending_ = Interval();
		have_interval_ = false;
	}

	static bool zooming(sim::Tick p_next) { return (p_next / 90) % 4 == 1; }

	void command(sim::Tick p_next, const std::optional<sim::Vec3> &p_look = std::nullopt) {
		sim::Command c;
		c.tick = p_next;
		c.sequence = ++sequence_;
		c.move_forward = int8_t(p_next % 600 < 420 ? sim::kAxisMax : 0);
		c.move_right = int8_t((p_next / 120) % 3 == 0 ? 64 : 0);
		c.yaw = uint16_t(p_next * 97u);
		c.pitch = int16_t(int32_t(p_next % 200) - 100);
		if (p_look && zooming(p_next)) {
			const sim::Vec3 &d = *p_look;
			c.yaw = sim::quantize_yaw(std::atan2(d[0], d[2]));
			c.pitch = sim::quantize_pitch(std::atan2(d[1], std::hypot(d[0], d[2])));
			c.move_forward = 0;
			c.move_right = 0;
		}
		c.buttons = zooming(p_next) ? sim::buttons::kZoom : 0u;
		net::CommandPacket p;
		p.count = 1;
		p.commands[0] = c;
		p.newest_snapshot = have_ ? newest_ : uint16_t(0xFFFF);
		for (uint32_t i = 0; have_ && i < 32; ++i) {
			if (received_[uint16_t(newest_ - 1u - i)]) {
				p.snapshot_ack_bits |= 1u << i;
			}
			if (others_[uint16_t(newest_ - i)]) {
				p.others_ack_bits |= 1u << i;
			}
		}
		server_.on_receive(kPeer, net::kChannelUnreliable, net::encode_command_packet(p));
	}

	int slot = -1;
	net::ShotQuantization frame;
	std::array<sim::Tick, net::kMaxSlots> attacked_until{};
	uint64_t total_bytes = 0;
	uint64_t decode_failures = 0;
	uint64_t overflow = 0;
	std::vector<Interval> intervals;

	uint64_t lost() const { return lost_; }

private:
	void arrive(uint16_t p_seq) {
		if (!have_ || net::seq_newer(p_seq, newest_)) {
			for (uint16_t s = uint16_t(newest_ + 1u); have_ && s != p_seq; ++s) {
				received_[s] = false;
				others_[s] = false;
			}
			newest_ = p_seq;
			have_ = true;
		}
		received_[p_seq] = true;
	}

	host::GameServer &server_;
	net::SnapshotBaselines baselines_;
	std::vector<bool> received_ = std::vector<bool>(65536, false);
	std::vector<bool> others_ = std::vector<bool>(65536, false);
	bool have_ = false;
	uint16_t newest_ = 0;
	uint32_t sequence_ = 0;
	uint32_t unreliable_count_ = 0;
	uint64_t lost_ = 0;
	Interval pending_;
	bool have_interval_ = false;
};

host::GhostViewer test_viewer(bool p_zoomed) {
	host::GhostViewer v;
	v.slot = 0;
	v.team = 1;
	v.spawned = true;
	v.zoomed = p_zoomed;
	v.eye = { 0.0f, 1.6f, 0.0f };
	v.aim = { 0.0f, 0.0f, 1.0f };
	return v;
}

host::GhostCandidate candidate(uint8_t p_slot, uint8_t p_team, float p_metres, float p_degrees, bool p_squad_mate) {
	host::GhostCandidate c;
	c.slot = p_slot;
	c.team = p_team;
	c.squad_mate = p_squad_mate;
	const float a = p_degrees * 3.14159265f / 180.0f;
	c.position = { p_metres * std::sin(a), 1.6f, p_metres * std::cos(a) };
	c.record.slot = p_slot;
	c.record.generation = 1;
	return c;
}

class Schedule {
public:
	Schedule() { scheduler.configure(8, 1200); }

	std::vector<uint8_t> step(
			const host::GhostViewer &p_viewer, std::span<const host::GhostCandidate> p_cands, bool p_ack = true) {
		scheduler.open_interval(0, 0);
		scheduler.plan(0, p_viewer, p_cands, seq, tick, plan);
		net::Snapshot snap;
		snap.seq = seq;
		snap.server_tick = tick;
		snap.others = plan.others;
		snap.removed = plan.removed;
		scheduler.sent(0, snap, plan.others, {});
		if (p_ack) {
			net::CommandPacket p;
			p.newest_snapshot = seq;
			scheduler.ack(0, p);
		}
		++seq;
		tick += 2;
		std::vector<uint8_t> carried;
		for (const net::OtherSoldier &o : plan.others) {
			carried.push_back(o.slot);
		}
		return carried;
	}

	std::vector<uint32_t> gaps(const host::GhostViewer &p_viewer, std::span<const host::GhostCandidate> p_cands,
			uint8_t p_slot, uint32_t p_steps) {
		std::vector<uint32_t> out;
		std::optional<uint32_t> last;
		for (uint32_t i = 0; i < p_steps; ++i) {
			const std::vector<uint8_t> carried = step(p_viewer, p_cands);
			if (std::find(carried.begin(), carried.end(), p_slot) == carried.end()) {
				continue;
			}
			if (last) {
				out.push_back(i - *last);
			}
			last = i;
		}
		return out;
	}

	host::GhostScheduler scheduler;
	host::GhostPlan plan;
	uint16_t seq = 0;
	sim::Tick tick = 1000;
};

bool all_equal(const std::vector<uint32_t> &p_gaps, uint32_t p_gap) {
	return !p_gaps.empty() && std::all_of(p_gaps.begin(), p_gaps.end(), [&](uint32_t g) { return g == p_gap; });
}

}

TEST_SUITE("host-ghost") {
	TEST_CASE("Metro ConquestLarge0, 31 walkers and 32 firefight shooters around one peer, 20 s: the peer's ghosts fit "
			  "the budget and keep their tier rates") {
		Fixture *f = fixture();
		if (f == nullptr) {
			return;
		}
		host::GameServerConfig cfg;
		cfg.level = f->level.shared;
		cfg.max_slots = 64;
		cfg.seed = 0x6057;
		cfg.walkers = kWalkers;
		cfg.respawn_ticks = 60;
		cfg.dev_commands = true;
		cfg.gunplay.mechanics_cfg = OPENBF3_MECHANICS_CFG;
		auto fill = std::make_shared<host::BotFillConfig>();
		fill->level = f->level.bots;
		fill->count = 0;
		cfg.bots = fill;
		auto ff = std::make_shared<host::FirefightConfig>();
		ff->mode = host::FirefightMode::Free;
		ff->count = kShooters;
		ff->walk = true;
		cfg.firefight = ff;
		host::GameServer server;
		const core::Status st = server.init(cfg);
		REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));

		Peer peer(server);
		peer.frame = host::shot_frame(*f->level.shared->static_world);
		std::vector<host::OutDatagram> out;
		const auto step = [&] {
			out.clear();
			server.tick(out);
			peer.receive(out, server.current_tick());
		};
		server.on_connect(kPeer);
		peer.join(*f->level.shared);
		step();
		REQUIRE(peer.slot >= 0);
		const uint8_t own = uint8_t(peer.slot);
		for (sim::Tick i = 0; i < 4 * sim::kSimHz && !server.view().alive(own); ++i) {
			peer.command(server.current_tick() + 1u);
			step();
		}
		REQUIRE(server.view().alive(own));
		const size_t first_interval = peer.intervals.size();
		const uint64_t bytes_before = peer.total_bytes;
		const sim::Tick start_tick = server.current_tick();
		std::array<Ghost, net::kMaxSlots> ghosts{};
		uint64_t enemy_near_checks = 0, enemy_near_missing = 0, rate_checks = 0, rate_misses = 0;
		uint64_t attacker_checks = 0, attacker_misses = 0, cone_checks = 0, cone_misses = 0;
		std::array<uint64_t, 4> enemy_tier_checks{};
		uint64_t worst_rate_slot = 0;
		std::array<uint64_t, 25> misses_by_every{};
		int target = -1;
		const auto look = [&]() -> std::optional<sim::Vec3> {
			const sim::SoldierState *me = server.soldier(own);
			if (me == nullptr) {
				return std::nullopt;
			}
			const auto usable = [&](int p_slot) {
				const sim::SoldierState *s = p_slot >= 0 ? server.soldier(uint32_t(p_slot)) : nullptr;
				if (s == nullptr || !server.view().alive(uint8_t(p_slot)) ||
						server.view().team(uint8_t(p_slot)) == server.view().team(own)) {
					return false;
				}
				const float metres = core::vec::length(core::vec::sub(s->position, me->position));
				return metres > 320.0f && metres < 780.0f;
			};
			for (uint32_t j = 0; !usable(target) && j < cfg.max_slots; ++j) {
				target = j != own && usable(int(j)) ? int(j) : -1;
			}
			if (!usable(target)) {
				return std::nullopt;
			}
			const sim::Vec3 eye = core::vec::add(me->position, { 0.0f, 1.6f, 0.0f });
			return core::vec::sub(
					core::vec::add(server.soldier(uint32_t(target))->position, { 0.0f, 1.0f, 0.0f }), eye);
		};
		std::array<bool, net::kMaxSlots> moved{};
		uint32_t relocated = 0;
		const auto relocate_attacker = [&] {
			const sim::SoldierState *me = server.soldier(own);
			for (uint32_t j = 0; me != nullptr && relocated < 1 && j < cfg.max_slots; ++j) {
				if (moved[j] || j == own || peer.attacked_until[j] <= server.current_tick() ||
						!server.view().alive(uint8_t(j)) || server.view().team(uint8_t(j)) == server.view().team(own)) {
					continue;
				}
				for (const sim::SpawnPoint &sp : f->level.shared->spawns) {
					const float metres = core::vec::length(core::vec::sub(sp.position, me->position));
					if (metres > 150.0f && metres < 280.0f) {
						REQUIRE(server.place_soldier(j, sp.position, sp.yaw, sim::Pose::Stand).ok());
						moved[j] = true;
						++relocated;
						return;
					}
				}
			}
		};
		uint64_t cone_intervals = 0;
		const auto wall = std::chrono::steady_clock::now();
		for (sim::Tick i = 0; i < kRunTicks; ++i) {
			peer.command(server.current_tick() + 1u, look());
			const size_t before = peer.intervals.size();
			step();
			relocate_attacker();
			if (peer.intervals.size() == before) {
				continue;
			}
			const Interval &iv = peer.intervals.back();
			const sim::SoldierState *me = server.soldier(own);
			const bool zoomed = me != nullptr && server.gunplay()->other_record(own, *me).ads;
			sim::Vec3 eye{};
			sim::Vec3 aim{};
			if (me != nullptr) {
				eye = core::vec::add(me->position, { 0.0f, 1.6f, 0.0f });
				aim = sim::weapons::aim_direction(me->yaw, me->pitch, {});
			}
			for (uint32_t j = 0; j < cfg.max_slots; ++j) {
				Ghost &g = ghosts[j];
				const sim::SoldierState *s = server.soldier(j);
				const bool alive = j != own && s != nullptr && server.view().alive(uint8_t(j));
				if (!alive || me == nullptr) {
					g = Ghost();
					continue;
				}
				const float metres = core::vec::length(core::vec::sub(s->position, me->position));
				const bool enemy = server.view().team(uint8_t(j)) != server.view().team(own);
				const bool attacker = enemy && peer.attacked_until[j] > iv.tick;
				const sim::Vec3 to = core::vec::sub(s->position, eye);
				const bool in_cone = enemy && zoomed && metres > 100.0f &&
						core::vec::dot(to, aim) >= kConeMarginCos * core::vec::length(to);
				std::optional<uint32_t> every =
						enemy ? floor_of(kEnemyFloor, metres) : floor_of(kTeammateFloor, metres);
				if (attacker) {
					every = 1u;
				} else if (in_cone && every) {
					every = std::min<uint32_t>(*every, 2u);
				}
				if (!every) {
					g = Ghost();
					continue;
				}
				if (!g.in_scope || g.life != s->life) {
					g = Ghost();
					g.in_scope = true;
					g.life = s->life;
					g.scope_seq = iv.seq;
				}
				const uint16_t since =
						g.last_seq ? uint16_t(iv.seq - *g.last_seq) : uint16_t(iv.seq - g.scope_seq + 1u);
				const bool due = since >= *every;
				if (enemy) {
					for (size_t t = 0; t < kEnemyFloor.size(); ++t) {
						if (metres <= kEnemyFloor[t].metres) {
							++enemy_tier_checks[t];
							break;
						}
					}
				}
				if (enemy && metres <= 100.0f) {
					++enemy_near_checks;
					enemy_near_missing += iv.carried[j] ? 0u : 1u;
				}
				if (due && attacker && metres > 100.0f) {
					++attacker_checks;
					attacker_misses += iv.carried[j] ? 0u : 1u;
				}
				cone_intervals += in_cone ? 1u : 0u;
				if (due && in_cone && !attacker) {
					++cone_checks;
					cone_misses += iv.carried[j] ? 0u : 1u;
				}
				if (due) {
					++rate_checks;
					if (!iv.carried[j]) {
						++rate_misses;
						worst_rate_slot = j;
						++misses_by_every[*every];
					}
				}
				if (iv.carried[j]) {
					g.last_seq = iv.seq;
					++g.sent;
				}
			}
		}
		const double run_ms =
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall).count();

		const std::span<const Interval> run(
				peer.intervals.data() + first_interval, peer.intervals.size() - first_interval);
		REQUIRE(run.size() > 500);
		size_t worst_unreliable = 0, worst_all = 0, over = 0;
		uint64_t carried = 0;
		for (const Interval &iv : run) {
			worst_unreliable = std::max(worst_unreliable, iv.unreliable_bytes);
			worst_all = std::max(worst_all, iv.all_bytes);
			over += iv.unreliable_bytes > kIntervalBytes ? 1u : 0u;
			carried += size_t(std::count(iv.carried.begin(), iv.carried.end(), true));
		}
		const double seconds = double(server.current_tick() - start_tick) / double(sim::kSimHz);
		const double per_second = double(peer.total_bytes - bytes_before) / seconds;
		const host::GameServerStats stats = server.stats();
		std::printf("host.ghost: %zu intervals in %.1f s of sim (%.0f ms wall); worst unreliable interval %zu B, worst "
					"all-channel interval %zu B, %zu over "
					"%zu B; mean %.0f B/s all channels; %.1f ghosts per interval; %llu overflow datagrams, %llu "
					"snapshots lost\n",
				run.size(), seconds, run_ms, worst_unreliable, worst_all, over, kIntervalBytes, per_second,
				double(carried) / double(run.size()), (unsigned long long)peer.overflow,
				(unsigned long long)peer.lost());
		std::printf("host.ghost: enemies within 100 m: %llu checks, %llu missing; tier floors: %llu due, %llu missed "
					"(last slot %llu); enemy checks per tier "
					"%llu / %llu / %llu / %llu; %u soldiers, %llu shots, %llu hits, shot events dropped %llu\n",
				(unsigned long long)enemy_near_checks, (unsigned long long)enemy_near_missing,
				(unsigned long long)rate_checks, (unsigned long long)rate_misses, (unsigned long long)worst_rate_slot,
				(unsigned long long)enemy_tier_checks[0], (unsigned long long)enemy_tier_checks[1],
				(unsigned long long)enemy_tier_checks[2], (unsigned long long)enemy_tier_checks[3], stats.soldiers,
				(unsigned long long)stats.gunplay.shots, (unsigned long long)stats.gunplay.hits,
				(unsigned long long)stats.gunplay.shot_events_dropped);
		std::printf("host.ghost: recent attackers beyond 100 m: %llu due, %llu missed; enemies in the zoomed aim cone "
					"beyond 100 m: %llu intervals, %llu due, %llu missed\n",
				(unsigned long long)attacker_checks, (unsigned long long)attacker_misses,
				(unsigned long long)cone_intervals, (unsigned long long)cone_checks, (unsigned long long)cone_misses);
		for (size_t e = 0; e < misses_by_every.size(); ++e) {
			if (misses_by_every[e] != 0) {
				std::printf("host.ghost: %llu misses of a ghost due every %zu intervals\n",
						(unsigned long long)misses_by_every[e], e);
			}
		}
		std::fflush(stdout);
		CHECK(peer.decode_failures == 0);
		CHECK(std::count_if(stats.slots.begin(), stats.slots.end(),
					  [](const host::SlotStats &p_s) { return p_s.used; }) == 64);
		CHECK(stats.gunplay.shots > 0);
		CHECK(enemy_near_checks > 800);
		CHECK(enemy_tier_checks[1] + enemy_tier_checks[2] > 1000);
		CHECK(worst_unreliable <= kIntervalBytes);
		CHECK(per_second <= kBytesPerSecond);
		CHECK(enemy_near_missing == 0);
		CHECK(rate_misses == 0);
		CHECK(attacker_checks > 0);
		CHECK(attacker_misses == 0);
		CHECK(cone_checks > 0);
		CHECK(cone_misses == 0);
	}

	TEST_CASE("a shot is relevant to a receiver its path passes within the fly-by radius of, up to the round's range; "
			  "Metro's 5.56 rounds carry radius 15") {
		const host::FlyBy r{ 15.0f, 1000.0f };
		const sim::Vec3 o{ 0.0f, 0.0f, 0.0f };
		const sim::Vec3 x{ 1.0f, 0.0f, 0.0f };
		CHECK(host::shot_passes_by(o, x, r, { 50.0f, 10.0f, 0.0f }));
		CHECK(host::shot_passes_by(o, { 2.0f, 0.0f, 0.0f }, r, { 50.0f, 0.0f, 14.9f }));
		CHECK_FALSE(host::shot_passes_by(o, x, r, { 50.0f, 20.0f, 0.0f }));
		CHECK(host::shot_passes_by(o, x, r, { -10.0f, 5.0f, 0.0f }));
		CHECK_FALSE(host::shot_passes_by(o, x, r, { -14.0f, 6.0f, 0.0f }));
		CHECK(host::shot_passes_by(o, x, r, { 1010.0f, 5.0f, 0.0f }));
		CHECK_FALSE(host::shot_passes_by(o, x, r, { 1100.0f, 5.0f, 0.0f }));
		CHECK_FALSE(host::shot_passes_by(o, x, { 0.0f, 1000.0f }, { 50.0f, 1.0f, 0.0f }));
		CHECK_FALSE(host::shot_passes_by(o, x, { 15.0f, 0.0f }, { 5.0f, 1.0f, 0.0f }));
		CHECK_FALSE(host::shot_passes_by(o, { 0.0f, 0.0f, 0.0f }, r, { 1.0f, 0.0f, 0.0f }));
		CHECK_FALSE(host::shot_passes_by(o, { std::nanf(""), 0.0f, 0.0f }, r, { 1.0f, 0.0f, 0.0f }));

		Fixture *f = fixture();
		if (f == nullptr) {
			return;
		}
		REQUIRE(f->level.shared->gunplay != nullptr);
		size_t with = 0, rifle = 0;
		for (const core::gameplay::WeaponDef &d : f->level.shared->gunplay->defs) {
			with += d.projectile.fly_by_radius > 0.0f ? 1u : 0u;
			if (d.projectile.fly_by_sound == "sound/bullet_craft/bullet_flybys/bullet_flyby_556") {
				++rifle;
				CHECK(d.projectile.fly_by_radius == 15.0f);
				CHECK(d.projectile.fly_by_speed == 20.0f);
			}
		}
		std::printf("host.ghost: %zu of %zu weapon defs carry a fly-by radius, %zu fire 5.56 rounds\n", with,
				f->level.shared->gunplay->defs.size(), rifle);
		CHECK(rifle > 0);
		CHECK(with > rifle);
	}

	TEST_CASE("ghost tiers: distance tables, enemy scope to 1.5 km, attackers at any range, the zoomed 10 degree aim "
			  "cone, squad mates on the enemy table") {
		const auto tier = [](const host::GhostCandidate &p_c, bool p_attacker, bool p_zoomed) {
			return host::ghost_tier(test_viewer(p_zoomed), p_c, p_attacker);
		};
		const auto is = [](const std::optional<host::GhostTier> &p_t, uint32_t p_target, uint32_t p_required) {
			return p_t && p_t->target == p_target && p_t->required == p_required;
		};
		CHECK(is(tier(candidate(1, 2, 50.0f, 90.0f, false), false, false), 1, 1));
		CHECK(is(tier(candidate(1, 2, 250.0f, 90.0f, false), false, false), 2, 3));
		CHECK(is(tier(candidate(1, 2, 500.0f, 90.0f, false), false, false), 5, 6));
		CHECK(is(tier(candidate(1, 2, 1200.0f, 90.0f, false), false, false), 15, 24));
		CHECK_FALSE(tier(candidate(1, 2, 1600.0f, 90.0f, false), false, false));

		CHECK(is(tier(candidate(1, 2, 250.0f, 90.0f, false), true, false), 1, 1));
		CHECK(is(tier(candidate(1, 2, 1600.0f, 90.0f, false), true, false), 1, 1));
		CHECK(is(tier(candidate(1, 2, 20000.0f, 90.0f, false), true, false), 1, 1));

		CHECK(is(tier(candidate(1, 2, 500.0f, 0.0f, false), false, true), 1, 2));
		CHECK(is(tier(candidate(1, 2, 500.0f, 9.0f, false), false, true), 1, 2));
		CHECK(is(tier(candidate(1, 2, 1200.0f, -9.0f, false), false, true), 1, 2));
		CHECK(is(tier(candidate(1, 2, 500.0f, 11.0f, false), false, true), 5, 6));
		CHECK(is(tier(candidate(1, 2, 500.0f, 0.0f, false), false, false), 5, 6));
		CHECK(is(tier(candidate(1, 2, 50.0f, 0.0f, false), false, true), 1, 1));
		CHECK_FALSE(tier(candidate(1, 2, 1600.0f, 0.0f, false), false, true));

		CHECK(is(tier(candidate(1, 1, 50.0f, 90.0f, false), false, false), 2, 3));
		CHECK(is(tier(candidate(1, 1, 250.0f, 90.0f, false), false, false), 6, 8));
		CHECK(is(tier(candidate(1, 1, 5000.0f, 90.0f, false), false, false), 15, 24));
		CHECK(is(tier(candidate(1, 1, 250.0f, 90.0f, false), true, false), 6, 8));
		CHECK(is(tier(candidate(1, 1, 500.0f, 0.0f, false), false, true), 15, 24));

		CHECK(is(tier(candidate(1, 1, 50.0f, 90.0f, true), false, false), 1, 1));
		CHECK(is(tier(candidate(1, 1, 250.0f, 90.0f, true), false, false), 2, 3));
		CHECK(is(tier(candidate(1, 1, 500.0f, 90.0f, true), false, false), 5, 6));
		CHECK(is(tier(candidate(1, 1, 5000.0f, 90.0f, true), false, false), 15, 24));
	}

	TEST_CASE(
			"the scheduler sends each ghost at its tier's rate, an attacker every snapshot for 5 s, a zoomed-on enemy "
			"every snapshot, a squad mate at the enemy rate, and repeats a scope-out until it is acked") {
		Schedule s;
		const host::GhostViewer plain = test_viewer(false);
		const std::array<host::GhostCandidate, 4> cands{ candidate(1, 2, 250.0f, 90.0f, false),
			candidate(2, 2, 1200.0f, 90.0f, false), candidate(3, 2, 500.0f, 5.0f, false),
			candidate(4, 1, 250.0f, -90.0f, false) };
		s.step(plain, cands);
		CHECK(all_equal(s.gaps(plain, cands, 1, 60), 2));
		CHECK(all_equal(s.gaps(plain, cands, 2, 90), 15));
		CHECK(all_equal(s.gaps(plain, cands, 3, 60), 5));
		CHECK(all_equal(s.gaps(plain, cands, 4, 60), 6));

		s.scheduler.attacked(0, 1, s.tick);
		s.scheduler.attacked(0, 2, s.tick);
		const sim::Tick attack_end = s.tick + 5 * sim::kSimHz;
		uint32_t boosted = 0, carried_1 = 0, carried_2 = 0;
		while (s.tick < attack_end) {
			const std::vector<uint8_t> c = s.step(plain, cands);
			++boosted;
			carried_1 += std::count(c.begin(), c.end(), uint8_t(1)) != 0 ? 1u : 0u;
			carried_2 += std::count(c.begin(), c.end(), uint8_t(2)) != 0 ? 1u : 0u;
		}
		CHECK(boosted == 5 * sim::kSimHz / 2);
		CHECK(carried_1 == boosted);
		CHECK(carried_2 == boosted);
		CHECK(all_equal(s.gaps(plain, cands, 1, 60), 2));

		const host::GhostViewer zoomed = test_viewer(true);
		CHECK(all_equal(s.gaps(zoomed, cands, 3, 60), 1));
		CHECK(all_equal(s.gaps(zoomed, cands, 2, 90), 15));
		CHECK(all_equal(s.gaps(plain, cands, 3, 60), 5));

		std::array<host::GhostCandidate, 4> squad = cands;
		squad[3].squad_mate = true;
		CHECK(all_equal(s.gaps(plain, squad, 4, 60), 2));
		CHECK(all_equal(s.gaps(plain, cands, 4, 60), 6));

		std::array<host::GhostCandidate, 4> gone = cands;
		gone[1] = candidate(2, 2, 1600.0f, 90.0f, false);
		s.step(plain, gone, false);
		CHECK(std::count(s.plan.removed.begin(), s.plan.removed.end(), uint8_t(2)) == 1);
		CHECK(std::count(s.plan.in_scope.begin(), s.plan.in_scope.end(), uint8_t(2)) == 0);
		s.step(plain, gone, true);
		CHECK(std::count(s.plan.removed.begin(), s.plan.removed.end(), uint8_t(2)) == 1);
		s.step(plain, gone);
		CHECK(s.plan.removed.empty());
		s.scheduler.attacked(0, 2, s.tick);
		const std::vector<uint8_t> back = s.step(plain, gone);
		CHECK(std::count(back.begin(), back.end(), uint8_t(2)) == 1);
	}

	TEST_CASE("a remote track lasts three of its own sample gaps, at least 1 s, and a removal ends it") {
		host::ClientSessionConfig config;
		host::RemoteTracks tracks(config);
		const uint64_t second = 1'000'000'000ull;
		net::OtherSoldier o;
		o.slot = 3;
		o.generation = 1;
		net::OtherSoldier near = o;
		near.slot = 4;
		const auto add = [&](const net::OtherSoldier &p_o, sim::Tick p_tick, uint64_t p_now) {
			tracks.add(p_tick, std::span<const net::OtherSoldier>(&p_o, 1), 0, p_now);
		};
		add(o, 1000, 10 * second);
		add(o, 1000 + sim::kSimHz, 11 * second);
		add(near, 1000 + sim::kSimHz - 2, 11 * second);
		add(near, 1000 + sim::kSimHz, 11 * second);
		const auto timeouts = [&](uint64_t p_now) {
			tracks.count_timeouts(p_now);
			host::ClientStats s;
			tracks.stats(s);
			return s.track_timeout;
		};
		CHECK(timeouts(11 * second + second / 2) == 0);
		CHECK(timeouts(11 * second + 3 * second / 2) == 1);
		CHECK(timeouts(11 * second + 5 * second / 2) == 2);
		CHECK(timeouts(14 * second + second / 10) == 4);

		auto frame = std::make_unique<host::PresentationFrame>();
		tracks.remove(3, 1000 + sim::kSimHz);
		add(o, 1000 + sim::kSimHz - 1, 11 * second);
		tracks.publish(*frame, 11 * second + second / 10);
		REQUIRE(frame->other_count == 1);
		CHECK(frame->others[0].slot == 4);
		o.generation = 2;
		add(o, 1000 + sim::kSimHz + 2, 11 * second + second / 10);
		tracks.publish(*frame, 11 * second + second / 5);
		CHECK(frame->other_count == 2);
	}

	TEST_CASE("install unchanged") {
		Fixture *f = fixture();
		if (f == nullptr) {
			return;
		}
		const core::SnapshotDiff diff = core::diff_snapshots(f->mount.before, f->mount.reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s (%zu files)\n", diff.clean() ? "clean" : diff.summary().c_str(),
				f->mount.before.files.size());
		std::error_code ec;
		std::filesystem::remove_all(f->cache_root, ec);
	}
}
