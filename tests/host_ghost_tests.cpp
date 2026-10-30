#include <doctest/doctest.h>

#include "bf3/core/cache/disk_cache.h"
#include "bf3/core/math.h"
#include "bf3/host/firefight_run.h"
#include "bf3/net/enet_host.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/soldier_motor.h"
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

	void command(sim::Tick p_next) {
		sim::Command c;
		c.tick = p_next;
		c.sequence = ++sequence_;
		c.move_forward = int8_t(p_next % 600 < 420 ? sim::kAxisMax : 0);
		c.move_right = int8_t((p_next / 120) % 3 == 0 ? 64 : 0);
		c.yaw = uint16_t(p_next * 97u);
		c.pitch = int16_t(int32_t(p_next % 200) - 100);
		c.buttons = (p_next / 90) % 4 == 1 ? sim::buttons::kZoom : 0u;
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
		std::array<uint64_t, 4> enemy_tier_checks{};
		uint64_t worst_rate_slot = 0;
		std::array<uint64_t, 25> misses_by_every{};
		const auto wall = std::chrono::steady_clock::now();
		for (sim::Tick i = 0; i < kRunTicks; ++i) {
			peer.command(server.current_tick() + 1u);
			const size_t before = peer.intervals.size();
			step();
			if (peer.intervals.size() == before) {
				continue;
			}
			const Interval &iv = peer.intervals.back();
			const sim::SoldierState *me = server.soldier(own);
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
				const std::optional<uint32_t> every =
						enemy ? floor_of(kEnemyFloor, metres) : floor_of(kTeammateFloor, metres);
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
		CHECK(enemy_near_checks > 1000);
		CHECK(enemy_tier_checks[1] + enemy_tier_checks[2] > 1000);
		CHECK(worst_unreliable <= kIntervalBytes);
		CHECK(per_second <= kBytesPerSecond);
		CHECK(enemy_near_missing == 0);
		CHECK(rate_misses == 0);
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
