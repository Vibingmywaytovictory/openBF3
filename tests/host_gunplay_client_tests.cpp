// The M2a client: prediction of the own weapons, reconciliation by wire view, the events, the presented weapon, the
// cosmetic projectiles and the refusal on MatchInfo (docs/design/m2a.md §6.1-§6.3, §7.2-§7.4, §8, §13.3; owner cl;
// as built: docs/design/m2a-cl.md).
//
// Until sv lands its server gunplay, the end-to-end cases run against m2a_fake::GunServer below: M1's GameServer (the
// motor, the jitter buffer, spawns, snapshots, the clock reports) on its own ENet loop at 60 Hz, wrapped with the
// gunplay a v2 server adds: the applied command per slot mirrored from GameServer's rules, the weapons stepped through
// step_weapons_m2a after every motor step, one ProjectileWorld against lc's real LagHistory / RewindProvider (with the
// trace rows E12 (b) compares), MatchInfo's match fields, Spawned's kit and team, the Snapshot's own weapons block,
// SnapshotOthers (an optional split), ShotEvents and CombatEvents, and a virtual health loss for R2. sv's merge
// replaces it with the GameServer itself.
//
// Suite host-gunplay-client-unit (row host.gunplay.client.unit; no install, short): the events ring, present_weapon,
// the weapon lane under catch_up, ClientProjectiles, the view sample and window_extrapolated, E6's refusal, R2's
// health rebase, the SnapshotOthers merge with its acks.
// Suite host-gunplay-client (row host.gunplay.client; wall clock, minutes; runs only with OPENBF3_CL_TIMING=1, else
// the row reports skipped): E1 + E5 (fire and reload), E12 lagcomp (b) + control + stress, E15 loopback (10 min by
// default, OPENBF3_CL_LOOPBACK_S) and two clients. Timings are wall-clock and noisy on a shared machine.
#define OPENBF3_GUNPLAY_CLIENT_TESTS 1
#include "gunplay_fixture.h"
#include "weapons_support.h"

#include "bf3/core/gameplay/capsule_pose_table.h"
#include "bf3/host/client_projectiles.h"
#include "bf3/host/client_session.h"
#include "bf3/host/game_server.h"
#include "bf3/host/kits.h"
#include "bf3/host/lag_comp.h"
#include "bf3/host/presentation_events.h"
#include "bf3/host/presentation_weapon.h"
#include "bf3/net/enet_host.h"
#include "bf3/net/other_sample.h"
#include "bf3/net/protocol.h"
#include "bf3/net/weapons_wire.h"
#include "bf3/sim/prediction.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/spawn.h"
#include "bf3/sim/weapons/ballistics.h"
#include "bf3/sim/weapons/posed_target.h"
#include "bf3/sim/weapons/weapon_rng.h"
#include "bf3/sim/weapons/weapon_rules.h"
#include "test_support.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace bf3;
namespace gp = bf3::core::gameplay;
namespace sw = bf3::sim::weapons;
namespace tw = bf3::test::weapons;
namespace tg = bf3::test::gunplay;

// ---- m2a_fake: test-local stand-ins until sv lands ----------------------------------------------------------------

namespace m2a_fake {

// lc's synthetic capsule pose table (sim_lagcomp_tests.cpp synth_table), copied: per stance a head, a chest and a leg
// capsule, the head leaning with the pitch bucket, the transitions blended with a bump.
gp::CapsuleLocal cap(float p_ax, float p_ay, float p_az, float p_bx, float p_by, float p_bz, float p_r, sw::HitZone p_zone, uint8_t p_index) {
	gp::CapsuleLocal c{};
	c.a[0] = p_ax;
	c.a[1] = p_ay;
	c.a[2] = p_az;
	c.b[0] = p_bx;
	c.b[1] = p_by;
	c.b[2] = p_bz;
	c.radius = p_r;
	c.zone = uint8_t(p_zone);
	c.capsule = p_index;
	c.property = uint8_t(10 + p_index);
	c.flags = p_zone == sw::HitZone::Legs ? sw::zone_capsule_flags::kDeactivateIfBehindWall : uint8_t(0);
	return c;
}

void stance_entry(size_t p_stance, size_t p_pitch, double p_extra, gp::CapsuleLocal *r_out) {
	const float s = float(p_stance);
	const float lean = 0.004f * float(p_pitch);
	r_out[0] = cap(0.0f, 1.65f - 0.6f * s, 0.05f + lean, 0.0f, 1.65f - 0.6f * s, 0.05f + lean, 0.15f, sw::HitZone::Head, 0);
	r_out[1] = cap(0.0f, 1.3f - 0.5f * s, 0.0f, 0.0f, 1.3f - 0.5f * s, 0.0f, 0.25f, sw::HitZone::Chest, 1);
	r_out[2] = cap(0.1f, 0.1f, 0.0f, 0.1f, 0.8f - 0.3f * s, 0.01f * float(p_pitch), 0.1f, sw::HitZone::Legs, 2);
	for (int c = 0; c < 3; ++c) {
		r_out[c].a[1] += float(p_extra);
		r_out[c].b[1] += float(p_extra);
	}
}

gp::CapsulePoseTable capsule_table() {
	gp::CapsulePoseTable t;
	t.version = gp::kCapsuleTableVersion;
	t.capsule_count = 3;
	for (size_t i = 0; i < gp::kCapsulePitchBuckets; ++i) {
		t.pitch_deg[i] = i <= 14 ? float(-60.7 + 60.7 * double(i) / 14.0) : float(78.1 * double(i - 14) / 18.0);
	}
	t.pitch_deg[14] = 0.0f;
	t.steady.resize(gp::kCapsuleStances * gp::kCapsulePitchBuckets * 3);
	t.transition.resize(gp::kCapsuleTransitions * gp::kCapsuleTransitionFractions * gp::kCapsulePitchBuckets * 3);
	for (size_t s = 0; s < gp::kCapsuleStances; ++s) {
		for (size_t p = 0; p < gp::kCapsulePitchBuckets; ++p) {
			stance_entry(s, p, 0, &t.steady[t.steady_index(s, p)]);
		}
	}
	const size_t from[6] = { 0, 0, 1, 1, 2, 2 }, to[6] = { 1, 2, 0, 2, 0, 1 };
	for (size_t r = 0; r < gp::kCapsuleTransitions; ++r) {
		for (size_t f = 0; f < gp::kCapsuleTransitionFractions; ++f) {
			const double w = double(f) / double(gp::kCapsuleTransitionFractions - 1);
			for (size_t p = 0; p < gp::kCapsulePitchBuckets; ++p) {
				gp::CapsuleLocal a[3], b[3];
				stance_entry(from[r], p, 0.05 * std::sin(3.14159265358979 * w), a);
				stance_entry(to[r], p, 0, b);
				gp::CapsuleLocal *o = &t.transition[t.transition_index(r, f, p)];
				for (int c = 0; c < 3; ++c) {
					o[c] = a[c];
					for (int k = 0; k < 3; ++k) {
						o[c].a[k] = float(a[c].a[k] + (b[c].a[k] - a[c].a[k]) * w);
						o[c].b[k] = float(a[c].b[k] + (b[c].b[k] - a[c].b[k]) * w);
					}
				}
			}
		}
	}
	double bound = 0;
	for (const auto *v : { &t.steady, &t.transition }) {
		for (const gp::CapsuleLocal &e : *v) {
			for (const float *p : { e.a, e.b }) {
				bound = std::max(bound, std::sqrt(double(p[0]) * p[0] + double(p[1]) * p[1] + double(p[2]) * p[2]) + double(e.radius));
			}
		}
	}
	t.bound_radius = float(bound) + 1e-3f;
	t.hash = gp::capsule_pose_table_hash(t);
	return t;
}

// The cfg files of E6 without sv's parser: the fake loader maps a path to its Mechanics ("variant.cfg" differs in
// the disc distribution; "missing.cfg" fails like an unreadable file; anything else is the shipped default).
core::Result<sw::Mechanics> load_mechanics(const std::string &p_path) {
	sw::Mechanics m;
	if (p_path == "missing.cfg") {
		return core::make_error("cannot read " + p_path);
	}
	if (p_path == "variant.cfg") {
		m.disc = sw::DiscDistribution::UniformRadius;
	}
	return m;
}

struct Tables {
	std::shared_ptr<const sw::WeaponTable> table;
	std::shared_ptr<const gp::CapsulePoseTable> capsules;
	host::KitTable kits;
	uint16_t rifle = 0;
	uint16_t pistol = 0;
};

const Tables &tables() {
	static Tables s;
	if (s.table != nullptr) {
		return s;
	}
	s.table = tw::synthetic_table(false);
	s.capsules = std::make_shared<const gp::CapsulePoseTable>(capsule_table());
	s.rifle = tw::id_of(*s.table, "weapons/m16a4/u_m16a4");
	s.pistol = tw::id_of(*s.table, "weapons/m9/u_m9");
	for (size_t team = 0; team < host::kKitTeams; ++team) {
		for (size_t kit = 0; kit < host::kKitCount; ++kit) {
			host::KitLoadout &l = s.kits.loadouts[team][kit];
			l.kit = uint8_t(kit);
			l.team = uint8_t(team + 1);
			l.defs.fill(sw::kNoWeapon);
			l.defs[0] = s.rifle;
			l.defs[1] = s.pistol;
			l.active = 0;
		}
	}
	s.kits.hash = 0xC1A55E5D0001ull; // kit_table_hash is sv's; any agreed value
	return s;
}

std::shared_ptr<const host::ClientGunplayInputs> inputs(uint64_t p_kit_hash = 0) {
	const Tables &t = tables();
	auto in = std::make_shared<host::ClientGunplayInputs>();
	in->table = t.table;
	in->capsules = t.capsules;
	in->kits = t.kits;
	if (p_kit_hash != 0) {
		in->kits.hash = p_kit_hash;
	}
	in->shot_frame.origin_min = { -1024.0f, -512.0f, -1024.0f };
	in->load_mechanics = load_mechanics;
	return in;
}

using Brain = std::function<sim::Command(sim::Tick, const sim::SoldierState &)>;

struct BotSpec {
	int team = 2;
	bool armed = false;
	Brain brain;
};

struct GunServerConfig {
	std::shared_ptr<const host::SharedLevel> level;
	std::shared_ptr<const host::ClientGunplayInputs> inputs;
	bool gunplay = true; // false: MatchInfo without a weapon_hash (an M1 server)
	std::string mechanics_cfg = "server.cfg";
	uint8_t weapon_flags = 0;
	uint32_t max_rewind_ticks = host::kDefaultMaxRewindTicks;
	uint64_t match_seed = 0x5EED0000C1u;
	uint32_t split_keep = 0xFFFFFFFFu; // others kept in the Snapshot; the rest travel in its SnapshotOthers
	uint32_t fake_damage_every_ticks = 0; // R2: a virtual health loss of the remote soldiers in their own Snapshots
	float fake_damage = 7.0f;
	uint64_t seed = 1;
	std::vector<BotSpec> bots;
	size_t trace_limit = size_t(1) << 20;
	// Hostile: two mutated copies (bit flips, truncation, trailing bytes) of every SnapshotOthers, ShotEvents and
	// CombatEvents, and every 20 ticks decodable hostile values (an own weapons block with a def outside the table,
	// a remote shot of an unknown def, a hit with huge damage naming slot 250, which the 7-bit wire slot carries as 122).
	bool hostile = false;
};

// A remote shot that decodes but names a def outside the table (the client refuses it and counts it).
net::RemoteShot hostile_shot(sim::Tick p_tick, const sim::Vec3 &p_eye) {
	net::RemoteShot r;
	r.slot = 63;
	r.shot_index = 1000u + p_tick;
	r.def = uint16_t(net::kMaxWireDefs - 1); // the largest def the wire carries; the synthetic table is far smaller
	r.time = sw::tick_start(p_tick);
	r.origin = p_eye;
	r.direction = { 0.0f, 0.0f, 1.0f };
	return r;
}

struct ServerShot {
	sim::Tick tick = 0;
	sw::ShotEvent shot;
};
struct ServerTimed {
	sim::Tick tick = 0;
	uint8_t slot = 0;
	sw::TimedWeaponEvent event;
};

struct ServerLog {
	std::vector<ServerShot> shots;
	std::vector<ServerTimed> timed;
	std::vector<sw::ImpactEvent> target_impacts;
	std::vector<host::RewindTraceRow> trace;
	std::vector<std::pair<sim::Tick, net::WeaponsWire>> wires; // the first remote slot's own weapons block per snapshot
	uint64_t readys = 0, kit_selects = 0, weapon_precondition_failures = 0, refused_views = 0, fake_hits = 0;
	uint64_t snapshots = 0, snapshot_others = 0, shot_datagrams = 0, combat_datagrams = 0, hostile_sent = 0;
	host::LagCompStats lag;
};

class GunServer {
public:
	~GunServer() { stop(); }

	bool start(const GunServerConfig &p_config, std::string &r_error) {
		config_ = p_config;
		host::GameServerConfig gc;
		gc.level = p_config.level;
		gc.max_slots = 64; // LagHistory's 64-bit masks
		gc.dev_commands = true;
		gc.seed = p_config.seed;
		const core::Status s = game_.init(gc);
		if (!s.ok()) {
			r_error = s.error().message;
			return false;
		}
		core::Result<std::unique_ptr<sim::CollisionWorld>> w = sim::CollisionWorld::create(p_config.level->static_world);
		if (!w) {
			r_error = w.error().message;
			return false;
		}
		world_ = std::move(*w);
		if (!motor_.init(*world_, p_config.level->motor).ok()) {
			r_error = "motor";
			return false;
		}
		if (p_config.inputs != nullptr) {
			core::Result<std::shared_ptr<const sw::WeaponTable>> t = host::build_client_weapon_table(*p_config.inputs, p_config.mechanics_cfg, p_config.weapon_flags);
			if (!t) {
				r_error = t.error().message;
				return false;
			}
			table_ = *t;
			weapon_hash_ = host::client_weapon_hash(*table_, *p_config.inputs->capsules);
			frame_ = p_config.inputs->shot_frame;
			host::LagCompConfig lc;
			lc.max_rewind_ticks = p_config.max_rewind_ticks;
			for (size_t k = 0; k < sim::kPoseCount; ++k) {
				lc.pose_eye_heights[k] = p_config.level->motor.poses[k].eye_height;
			}
			lag_config_ = lc;
			lag_ = std::make_unique<host::LagHistory>(lc, 64);
			rewind_ = std::make_unique<host::RewindProvider>(*lag_, *p_config.inputs->capsules, lc);
			rewind_->set_trace(&trace_rows_);
			projectiles_ = std::make_unique<sw::ProjectileWorld>(table_);
		}
		for (const BotSpec &b : p_config.bots) {
			auto source = std::make_unique<Recording>();
			source->server = this;
			source->brain = b.brain;
			Recording *raw = source.get();
			const int slot = game_.add_local_soldier(std::move(source), b.team);
			if (slot < 0) {
				r_error = "no slot for a bot";
				return false;
			}
			raw->slot = uint32_t(slot);
			Mirror &m = mirror_[size_t(slot)];
			m.used = true;
			m.remote = false;
			m.armed = b.armed;
			m.team = uint8_t(b.team);
			bot_slots_.push_back(slot);
		}
		std::string err;
		host_ = net::Host::create_server("127.0.0.1", 0, 16, net::kChannelCount, &err);
		if (host_ == nullptr) {
			r_error = err;
			return false;
		}
		port_ = host_->bound_port();
		stop_ = false;
		thread_ = std::thread([this] { run(); });
		return true;
	}

	void stop() {
		if (!thread_.joinable()) {
			return;
		}
		stop_ = true;
		thread_.join();
		if (host_ != nullptr) {
			host_->disconnect_all();
			host_->flush();
		}
		host_.reset();
	}

	uint16_t port() const { return port_; }
	uint64_t weapon_hash() const { return weapon_hash_; }
	const std::vector<int> &bot_slots() const { return bot_slots_; }

	ServerLog take_log() {
		std::lock_guard lock(mutex_);
		ServerLog l = std::move(log_);
		log_ = ServerLog();
		return l;
	}
	uint64_t readys() const { return readys_.load(); }
	// The authoritative state of a slot after the last tick (life 0 when none).
	sim::SoldierState state_of(uint32_t p_slot) const {
		std::lock_guard lock(mutex_);
		return p_slot < published_.size() ? published_[p_slot] : sim::SoldierState();
	}
	sim::Tick tick() const { return tick_.load(); }

private:
	struct Recording final : host::CommandSource {
		GunServer *server = nullptr;
		uint32_t slot = 0;
		Brain brain;
		sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) override {
			sim::Command c = brain ? brain(p_tick, p_state) : sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
			c.tick = p_tick;
			server->mirror_[slot].bot_last = c;
			return c;
		}
	};
	struct Buffered {
		sim::Command command;
		host::AckState ack;
	};
	// GameServer's per-slot rules mirrored (game_server.cpp receive_commands / run_tick): the command it applies.
	struct Mirror {
		bool used = false;
		bool remote = false;
		bool armed = true;
		uint32_t peer = 0;
		uint8_t team = 1;
		std::map<sim::Tick, Buffered> buffer;
		bool spawned = false;
		uint32_t life = 0;
		sim::Command last;
		host::AckState last_ack;
		sim::Command bot_last;
		bool have_weapons = false;
		sw::SoldierWeaponsState weapons;
		sim::Tick spawn_tick = 0;
		sim::Tick last_snapshot_tick = 0;
		float fake_total = 0;
	};
	struct HitRecord {
		sim::Tick tick = 0;
		net::HitEvent hit;
	};

	int slot_of_peer(uint32_t p_peer) const {
		for (size_t i = 0; i < mirror_.size(); ++i) {
			if (mirror_[i].used && mirror_[i].remote && mirror_[i].peer == p_peer) {
				return int(i);
			}
		}
		return -1;
	}

	void receive(uint32_t p_peer, uint8_t p_channel, const std::vector<uint8_t> &p_data) {
		const std::optional<net::MessageType> type = net::peek_type(p_data);
		if (type == net::MessageType::KitSelect) {
			std::lock_guard lock(mutex_);
			++log_.kit_selects;
			return; // M1's GameServer does not know it
		}
		if (type == net::MessageType::Ready) {
			++readys_;
			std::lock_guard lock(mutex_);
			++log_.readys;
		}
		const int at = slot_of_peer(p_peer);
		if (type == net::MessageType::CommandPacket && at >= 0) {
			core::Result<net::CommandPacket> c = net::decode_command_packet(p_data);
			if (c) {
				AckState a;
				if (lag_ != nullptr) {
					lag_->ack(uint8_t(at), c->newest_snapshot, c->snapshot_ack_bits, c->others_ack_bits);
					a = lag_->ack_state(uint8_t(at));
				}
				Mirror &m = mirror_[size_t(at)];
				const sim::Tick tick = game_.current_tick();
				for (size_t k = c->count; k-- > 0;) {
					sim::Command cmd = c->commands[k];
					cmd.flags = 0;
					if (cmd.tick <= tick || cmd.tick - tick > 32u) {
						continue;
					}
					auto it = m.buffer.find(cmd.tick);
					if (it == m.buffer.end()) {
						m.buffer[cmd.tick] = { cmd, a };
					} else if (cmd.sequence > it->second.command.sequence) {
						it->second = { cmd, a };
					}
				}
			}
		}
		game_.on_receive(p_peer, p_channel, p_data);
	}
	using AckState = host::AckState;

	static bool alive_for_weapons(const sim::SoldierState &p_s) {
		return p_s.life != 0 && p_s.mode != sim::MoveMode::Dead; // the client lane's rule (prediction.cpp)
	}

	void step_gunplay(sim::Tick p_n, const std::vector<sim::SoldierState> &p_before, const std::vector<sim::Command> &p_applied) {
		if (table_ == nullptr) {
			return;
		}
		for (size_t i = 0; i < mirror_.size(); ++i) {
			Mirror &m = mirror_[i];
			const sim::SoldierState *after = game_.soldier(uint32_t(i));
			if (!m.used || !m.spawned || !m.have_weapons || after == nullptr) {
				continue;
			}
			const sim::SoldierState &before = p_before[i];
			if (!alive_for_weapons(before)) {
				m.weapons.tick = p_n;
				m.weapons.events = 0;
				continue;
			}
			const sim::Command &cmd = p_applied[i];
			if (cmd.tick != m.weapons.tick + 1u) {
				std::lock_guard lock(mutex_);
				++log_.weapon_precondition_failures;
			}
			const sw::WeaponContext ctx = sw::make_weapon_context(config_.level->motor, before, *after, cmd);
			sw::WeaponTickOutput out;
			sw::step_weapons_m2a(*table_, m.weapons, cmd, ctx, config_.match_seed, m.spawn_tick, out);
			std::lock_guard lock(mutex_);
			for (size_t k = 0; k < out.shot_count; ++k) {
				const sw::ShotEvent &shot = out.shots[k];
				log_.shots.push_back({ p_n, shot });
				host::ShotView view;
				if (lag_->shot_view(uint8_t(i), m.remote ? m.last_ack : AckState(), m.remote ? cmd.view_seq8 : 0, m.remote ? cmd.view_back_q8 : 0,
							m.remote ? cmd.view_quality : 0, view)) {
					rewind_->note_shot(uint8_t(i), shot.shot_index, view);
				} else {
					rewind_->note_refused(uint8_t(i), shot.shot_index);
					++log_.refused_views;
				}
				(void)projectiles_->spawn(shot);
				recent_shots_.push_back({ p_n, net::remote_shot_of(shot) });
			}
			for (size_t k = 0; k < out.timed_count; ++k) {
				log_.timed.push_back({ p_n, uint8_t(i), out.timed[k] });
			}
		}
		// The projectiles against the rewound targets.
		std::vector<sim::SoldierState> present(64);
		std::vector<uint8_t> teams(64, 0);
		for (size_t i = 0; i < 64; ++i) {
			if (const sim::SoldierState *s = game_.soldier(uint32_t(i))) {
				present[i] = *s;
			}
			teams[i] = mirror_[i].team;
		}
		rewind_->begin_tick(p_n, present, teams);
		sw::ProjectileStepOutput out;
		projectiles_->step(p_n, *world_, *rewind_, out);
		rewind_->end_tick();
		std::lock_guard lock(mutex_);
		for (const sw::ImpactEvent &e : out.impacts) {
			if (e.kind != sw::ImpactKind::Target) {
				continue;
			}
			log_.target_impacts.push_back(e);
			HitRecord h;
			h.tick = p_n;
			h.hit.victim = uint8_t(e.target);
			h.hit.shooter = uint8_t(e.player_slot);
			h.hit.shot_index = e.shot_index;
			h.hit.pellet = e.pellet;
			h.hit.zone = e.zone;
			h.hit.damage = 10.0f;
			h.hit.health_after = 100.0f;
			h.hit.position = e.position;
			h.hit.normal = e.normal;
			h.hit.time = e.time;
			recent_hits_.push_back(h);
		}
		if (trace_rows_.size() > 0) {
			for (const host::RewindTraceRow &r : trace_rows_) {
				if (log_.trace.size() < config_.trace_limit) {
					log_.trace.push_back(r);
				}
			}
			trace_rows_.clear();
		}
		log_.lag = rewind_->stats();
	}

	void detect_spawns() {
		for (size_t i = 0; i < mirror_.size(); ++i) {
			Mirror &m = mirror_[i];
			const sim::SoldierState *s = game_.soldier(uint32_t(i));
			if (!m.used || s == nullptr || s->life == m.life) {
				continue;
			}
			m.life = s->life;
			m.spawned = true;
			m.last = sim::hold_command(*s);
			m.last_ack = AckState();
			m.fake_total = 0;
			if (table_ != nullptr && m.armed) {
				const host::KitLoadout &k = config_.inputs->kits.loadouts[size_t(std::clamp<int>(m.team, 1, 2) - 1)][0];
				m.weapons.player_slot = uint32_t(i);
				m.have_weapons = sw::spawn_soldier_weapons(m.weapons, *table_, k.defs, k.active, s->tick).ok();
				m.spawn_tick = s->tick;
			}
		}
	}

	void send(uint32_t p_peer, uint8_t p_channel, bool p_reliable, const std::vector<uint8_t> &p_bytes) {
		if (p_bytes.empty()) {
			return;
		}
		const net::Host::SendMode mode =
				p_reliable ? net::Host::SendMode::Reliable : (p_channel == net::kChannelUnreliable ? net::Host::SendMode::Unsequenced : net::Host::SendMode::Sequenced);
		host_->send(p_peer, p_channel, p_bytes.data(), p_bytes.size(), mode);
	}

	void hostile_copies(uint32_t p_peer, const std::vector<uint8_t> &p_bytes) {
		if (!config_.hostile || p_bytes.size() < 2) {
			return;
		}
		for (int k = 0; k < 2; ++k) {
			std::vector<uint8_t> b = p_bytes;
			switch (rng_() % 3) {
				case 0: {
					const int flips = 1 + int(rng_() % 8);
					for (int f = 0; f < flips; ++f) {
						const size_t at = 1 + size_t(rng_() % (b.size() - 1)); // byte 0 stays the type
						b[at] ^= uint8_t(1u << (rng_() % 8));
					}
					break;
				}
				case 1:
					b.resize(1 + size_t(rng_() % (b.size() - 1)));
					break;
				default:
					for (int t = 0, n = 1 + int(rng_() % 16); t < n; ++t) {
						b.push_back(uint8_t(rng_()));
					}
					break;
			}
			send(p_peer, net::kChannelUnreliable, false, b);
			std::lock_guard lock(mutex_);
			++log_.hostile_sent;
		}
	}

	void rewrite_and_send(std::vector<host::OutDatagram> &r_out, sim::Tick p_n) {
		struct Snap {
			uint32_t peer = 0;
			net::Snapshot snapshot;
		};
		std::vector<Snap> snaps;
		for (host::OutDatagram &d : r_out) {
			const std::optional<net::MessageType> type = net::peek_type(d.bytes);
			if (type == net::MessageType::MatchInfo) {
				core::Result<net::MatchInfo> m = net::decode_match_info(d.bytes);
				if (m) {
					Mirror &mm = mirror_[m->slot];
					mm = Mirror();
					mm.used = true;
					mm.remote = true;
					mm.peer = d.peer;
					mm.team = 1;
					if (lag_ != nullptr) {
						lag_->client_reset(m->slot);
					}
					if (config_.gunplay && table_ != nullptr) {
						m->match_seed = config_.match_seed;
						m->weapon_hash = weapon_hash_;
						m->weapon_flags = config_.weapon_flags;
						m->kit_hash = config_.inputs->kits.hash;
						m->max_rewind_ticks = uint8_t(config_.max_rewind_ticks);
					}
					d.bytes = net::encode_match_info(*m);
				}
			} else if (type == net::MessageType::Spawned) {
				core::Result<net::Spawned> s = net::decode_spawned(d.bytes);
				if (s) {
					s->kit = 0;
					s->team = mirror_[s->slot].team;
					d.bytes = net::encode_spawned(*s);
				}
			} else if (type == net::MessageType::Snapshot) {
				core::Result<net::Snapshot> s = net::decode_snapshot(d.bytes);
				if (s) {
					snaps.push_back({ d.peer, std::move(*s) });
					continue;
				}
			}
			send(d.peer, d.channel, d.reliable, d.bytes);
		}
		if (snaps.empty()) {
			return;
		}
		// The shared records of this snapshot tick: every record any client is sent (receiver-independent).
		std::map<uint8_t, net::OtherSoldier> records;
		for (const Snap &s : snaps) {
			for (const net::OtherSoldier &o : s.snapshot.others) {
				records[o.slot] = o;
			}
		}
		if (lag_ != nullptr) {
			std::vector<net::OtherSoldier> list;
			for (const auto &[slot, o] : records) {
				list.push_back(o);
			}
			lag_->record_tick(p_n, list);
		}
		const bool damage_tick = config_.fake_damage_every_ticks > 0 && p_n % config_.fake_damage_every_ticks == 0;
		for (Snap &s : snaps) {
			const int at = slot_of_peer(s.peer);
			if (at < 0) {
				continue;
			}
			Mirror &m = mirror_[size_t(at)];
			net::Snapshot &snap = s.snapshot;
			std::vector<HitRecord> hits;
			if (snap.own.has_value()) {
				if (damage_tick && alive_for_weapons(*snap.own)) {
					m.fake_total += config_.fake_damage;
					HitRecord h;
					h.tick = p_n;
					h.hit.victim = uint8_t(at);
					h.hit.shooter = 63;
					h.hit.damage = config_.fake_damage;
					h.hit.health_after = std::max(1.0f, snap.own->health - m.fake_total);
					h.hit.position = snap.own->position;
					h.hit.time = sw::tick_end(p_n);
					hits.push_back(h);
					std::lock_guard lock(mutex_);
					++log_.fake_hits;
				}
				if (m.fake_total > 0) {
					snap.own->health = std::max(1.0f, snap.own->health - m.fake_total);
				}
				if (table_ != nullptr && m.have_weapons && config_.gunplay) {
					snap.own_weapons = net::weapons_wire_view(m.weapons, p_n);
					std::lock_guard lock(mutex_);
					if (log_.wires.size() < 100000 && at == first_remote_slot()) {
						log_.wires.push_back({ p_n, *snap.own_weapons });
					}
				}
			}
			net::SnapshotOthers others;
			others.seq = snap.seq;
			others.server_tick = snap.server_tick;
			if (snap.others.size() > config_.split_keep) {
				others.others.assign(snap.others.begin() + std::ptrdiff_t(config_.split_keep), snap.others.end());
				snap.others.resize(config_.split_keep);
			}
			uint64_t main_mask = 0, others_mask = 0;
			for (const net::OtherSoldier &o : snap.others) {
				main_mask |= o.slot < 64 ? uint64_t(1) << o.slot : 0;
			}
			for (const net::OtherSoldier &o : others.others) {
				others_mask |= o.slot < 64 ? uint64_t(1) << o.slot : 0;
			}
			const bool hostile_tick = config_.hostile && p_n % 20u == 0;
			if (hostile_tick && snap.own_weapons.has_value()) {
				snap.own_weapons->slots[snap.own_weapons->active % snap.own_weapons->slots.size()].def = 0xFFF0;
			}
			send(s.peer, net::kChannelUnreliable, false, net::encode_snapshot(snap));
			if (!others.others.empty()) {
				const std::vector<uint8_t> bytes = net::encode_snapshot_others(others);
				send(s.peer, net::kChannelUnreliable, false, bytes);
				hostile_copies(s.peer, bytes);
			}
			if (lag_ != nullptr) {
				lag_->sent(uint8_t(at), snap.seq, p_n, main_mask, others_mask);
			}
			if (table_ != nullptr && config_.gunplay) {
				net::ShotEvents shots;
				shots.server_tick = p_n;
				for (const auto &[tick, r] : recent_shots_) {
					if (tick > m.last_snapshot_tick && tick <= p_n && r.slot != at) {
						shots.shots.push_back(r);
					}
				}
				sim::Vec3 eye = snap.own.has_value() ? snap.own->position : sim::Vec3{};
				eye[1] += 1.6f;
				if (hostile_tick) {
					net::ShotEvents bad;
					bad.server_tick = p_n;
					bad.shots.push_back(hostile_shot(p_n, eye));
					send(s.peer, net::kChannelUnreliable, false, net::encode_shot_events(bad, frame_, eye));
				}
				if (!shots.shots.empty()) {
					const std::vector<uint8_t> bytes = net::encode_shot_events(shots, frame_, eye);
					send(s.peer, net::kChannelUnreliable, false, bytes);
					hostile_copies(s.peer, bytes);
					std::lock_guard lock(mutex_);
					++log_.shot_datagrams;
				}
				net::CombatEvents combat;
				combat.server_tick = p_n;
				for (const HitRecord &h : recent_hits_) {
					if (h.tick > m.last_snapshot_tick && h.tick <= p_n) {
						combat.hits.push_back(h.hit);
					}
				}
				for (const HitRecord &h : hits) {
					combat.hits.push_back(h.hit);
				}
				if (hostile_tick) {
					net::HitEvent bad;
					bad.victim = 250;
					bad.shooter = 251;
					bad.damage = 1e30f;
					bad.health_after = -1e30f;
					combat.hits.push_back(bad);
				}
				if (!combat.hits.empty()) {
					const std::vector<uint8_t> bytes = net::encode_combat_events(combat, frame_);
					send(s.peer, net::kChannelReliable, true, bytes);
					hostile_copies(s.peer, bytes);
					std::lock_guard lock(mutex_);
					++log_.combat_datagrams;
				}
			}
			m.last_snapshot_tick = p_n;
			std::lock_guard lock(mutex_);
			++log_.snapshots;
			log_.snapshot_others += others.others.empty() ? 0u : 1u;
		}
		// Keep 16 ticks of shots and hits for the snapshots to come.
		while (!recent_shots_.empty() && recent_shots_.front().first + 16u < p_n) {
			recent_shots_.erase(recent_shots_.begin());
		}
		while (!recent_hits_.empty() && recent_hits_.front().tick + 16u < p_n) {
			recent_hits_.erase(recent_hits_.begin());
		}
	}

	int first_remote_slot() const {
		for (size_t i = 0; i < mirror_.size(); ++i) {
			if (mirror_[i].used && mirror_[i].remote) {
				return int(i);
			}
		}
		return -1;
	}

	void run() {
		using clock = std::chrono::steady_clock;
		const auto period = std::chrono::nanoseconds(1'000'000'000LL / int64_t(sim::kSimHz));
		auto next = clock::now();
		std::vector<sim::SoldierState> before(64);
		std::vector<sim::Command> applied(64);
		while (!stop_.load()) {
			net::Host::Event ev;
			while (host_->service(0, ev) > 0) {
				if (ev.type == net::Host::EventType::Connect) {
					game_.on_connect(ev.peer);
				} else if (ev.type == net::Host::EventType::Disconnect) {
					const int at = slot_of_peer(ev.peer);
					if (at >= 0) {
						mirror_[size_t(at)] = Mirror();
						if (lag_ != nullptr) {
							lag_->client_reset(uint8_t(at));
						}
					}
					game_.on_disconnect(ev.peer);
				} else if (ev.type == net::Host::EventType::Receive) {
					receive(ev.peer, ev.channel, ev.data);
				}
			}
			const sim::Tick n = game_.current_tick() + 1u;
			for (size_t i = 0; i < 64; ++i) {
				const sim::SoldierState *s = game_.soldier(uint32_t(i));
				before[i] = s != nullptr ? *s : sim::SoldierState();
				Mirror &m = mirror_[i];
				if (!m.used || !m.remote) {
					continue;
				}
				// GameServer's step 2 for this slot (run_tick): the buffered command of tick n, else the repeat.
				auto it = m.buffer.find(n);
				const bool have = it != m.buffer.end();
				Buffered b;
				if (have) {
					b = it->second;
				}
				m.buffer.erase(m.buffer.begin(), m.buffer.upper_bound(n));
				if (!m.spawned) {
					continue;
				}
				if (have) {
					m.last = b.command;
					m.last_ack = b.ack;
				} else {
					m.last = sim::repeated_command(m.last, n);
				}
				applied[i] = m.last;
			}
			std::vector<host::OutDatagram> out;
			game_.tick(out);
			for (size_t i = 0; i < 64; ++i) {
				if (mirror_[i].used && !mirror_[i].remote) {
					applied[i] = mirror_[i].bot_last;
				}
			}
			step_gunplay(n, before, applied);
			detect_spawns();
			rewrite_and_send(out, n);
			host_->flush();
			{
				std::lock_guard lock(mutex_);
				published_.assign(64, sim::SoldierState());
				for (size_t i = 0; i < 64; ++i) {
					if (const sim::SoldierState *s = game_.soldier(uint32_t(i))) {
						published_[i] = *s;
					}
				}
			}
			tick_ = n;
			next += period;
			auto now = clock::now();
			if (now > next + 5 * period) {
				next = now;
			}
			while ((now = clock::now()) < next) {
				const auto left = next - now;
				if (left > std::chrono::microseconds(2500)) {
					std::this_thread::sleep_for(left - std::chrono::microseconds(1500));
				} else {
					std::this_thread::yield();
				}
			}
		}
	}

	GunServerConfig config_;
	host::GameServer game_;
	std::unique_ptr<sim::CollisionWorld> world_;
	sim::SoldierMotor motor_;
	std::shared_ptr<const sw::WeaponTable> table_;
	uint64_t weapon_hash_ = 0;
	net::ShotQuantization frame_;
	host::LagCompConfig lag_config_;
	std::unique_ptr<host::LagHistory> lag_;
	std::unique_ptr<host::RewindProvider> rewind_;
	std::unique_ptr<sw::ProjectileWorld> projectiles_;
	std::vector<host::RewindTraceRow> trace_rows_;
	std::array<Mirror, 64> mirror_{};
	std::vector<int> bot_slots_;
	std::vector<std::pair<sim::Tick, net::RemoteShot>> recent_shots_;
	std::vector<HitRecord> recent_hits_;
	std::mt19937_64 rng_{ 0xB0B5EED };
	std::unique_ptr<net::Host> host_;
	uint16_t port_ = 0;
	std::thread thread_;
	std::atomic<bool> stop_{ false };
	std::atomic<uint64_t> readys_{ 0 };
	std::atomic<sim::Tick> tick_{ 0 };
	mutable std::mutex mutex_;
	ServerLog log_;
	std::vector<sim::SoldierState> published_;
};

// A bot that stands and fires bursts (10 ticks of fire every 60) turning slowly, so it shoots the floor around it.
Brain firing_bot(uint32_t p_phase) {
	return [p_phase](sim::Tick p_tick, const sim::SoldierState &p_state) {
		sim::Command c = sim::idle_command(p_tick, uint16_t(p_state.yaw + 300u), sim::quantize_pitch(-0.3f), sim::Pose::Stand);
		c.flags = 0;
		if ((p_tick + p_phase) % 60u < 10u) {
			c.buttons |= sim::buttons::kFire;
		}
		return c;
	};
}

// A target that strafes across its spawn heading: it faces left or right of the spawn yaw and runs forward at the
// stand speed (4 m/s), turning back at +-4 m from where it started (an 8 m leg).
Brain strafing_bot() {
	struct State {
		bool init = false;
		sim::Vec3 origin{};
		uint16_t yaw0 = 0;
		int dir = 1;
	};
	auto st = std::make_shared<State>();
	return [st](sim::Tick p_tick, const sim::SoldierState &p_state) {
		if (!st->init) {
			st->init = true;
			st->origin = p_state.position;
			st->yaw0 = p_state.yaw;
		}
		const float y = sim::yaw_radians(st->yaw0);
		const float rx = std::cos(y), rz = -std::sin(y);
		const float off = (p_state.position[0] - st->origin[0]) * rx + (p_state.position[2] - st->origin[2]) * rz;
		if (off > 4.0f) {
			st->dir = -1;
		} else if (off < -4.0f) {
			st->dir = 1;
		}
		sim::Command c = sim::idle_command(p_tick, uint16_t(st->yaw0 + (st->dir > 0 ? 16384u : 49152u)), 0, sim::Pose::Stand);
		c.flags = 0;
		c.move_forward = sim::kAxisMax;
		return c;
	};
}

} // namespace m2a_fake

namespace {

using m2a_fake::GunServer;
using m2a_fake::GunServerConfig;

bool same_bits(const void *p_a, const void *p_b, size_t p_n) {
	return std::memcmp(p_a, p_b, p_n) == 0;
}

template <typename T>
bool same_value(const T &p_a, const T &p_b) {
	return same_bits(&p_a, &p_b, sizeof(T));
}

// Field for field (floats by their bits; ShotEvent has padding, so never a memcmp of the struct).
bool same_shot(const sw::ShotEvent &p_a, const sw::ShotEvent &p_b, std::string *r_why = nullptr) {
	auto fail = [&](const char *p_field) {
		if (r_why != nullptr) {
			*r_why = p_field;
		}
		return false;
	};
	if (p_a.time != p_b.time) {
		return fail("time");
	}
	if (p_a.spawn_time != p_b.spawn_time) {
		return fail("spawn_time");
	}
	if (p_a.seed != p_b.seed) {
		return fail("seed");
	}
	if (p_a.player_slot != p_b.player_slot || p_a.shot_index != p_b.shot_index || p_a.def != p_b.def || p_a.weapon_slot != p_b.weapon_slot ||
			p_a.pellets != p_b.pellets) {
		return fail("ids");
	}
	if (!same_value(p_a.origin, p_b.origin)) {
		return fail("origin");
	}
	if (!same_value(p_a.aim, p_b.aim)) {
		return fail("aim");
	}
	if (!same_value(p_a.direction, p_b.direction)) {
		return fail("direction");
	}
	if (!same_value(p_a.spread, p_b.spread) || !same_value(p_a.pellet_cone, p_b.pellet_cone)) {
		return fail("spread");
	}
	if (!same_value(p_a.recoil.pitch, p_b.recoil.pitch) || !same_value(p_a.recoil.yaw, p_b.recoil.yaw)) {
		return fail("recoil");
	}
	if (p_a.tracer != p_b.tracer || !same_value(p_a.power, p_b.power)) {
		return fail("tracer/power");
	}
	return true;
}

std::optional<std::string> env_value(const char *p_name) {
	char *v = nullptr;
	size_t n = 0;
	if (_dupenv_s(&v, &n, p_name) != 0 || v == nullptr) {
		return std::nullopt;
	}
	std::string out(v);
	std::free(v);
	return out;
}

bool timing_enabled(const char *p_case) {
	const std::optional<std::string> e = env_value("OPENBF3_CL_TIMING");
	if (!e.has_value() || *e != "1") {
		const std::string why = std::string(p_case) + ": a timing case (set OPENBF3_CL_TIMING=1 and run under the quiet lock)";
		bf3::test::skip_install_case(why.c_str());
		return false;
	}
	return true;
}

double env_seconds(const char *p_name, double p_default) {
	const std::optional<std::string> e = env_value(p_name);
	return e.has_value() ? std::max(1.0, std::atof(e->c_str())) : p_default;
}

// One client and its main-thread side: the newest frame, the spawn view adopted, the drawn frame's view sample, the
// events drained.
struct Player {
	host::ClientSession client;
	host::InputSample input;
	std::unique_ptr<host::PresentationFrame> frame = std::make_unique<host::PresentationFrame>();
	std::vector<host::PresentEvent> events;
	std::vector<host::PresentEvent> scratch = std::vector<host::PresentEvent>(host::kPresentationEventCapacity);
	float adopt_yaw = NAN;

	~Player() { client.stop(); }

	bool start(uint16_t p_port, std::shared_ptr<const host::SharedLevel> p_level, const std::string &p_netsim, uint64_t p_seed,
			const std::function<void(host::ClientSessionConfig &)> &p_configure = {}) {
		host::ClientSessionConfig cc;
		cc.port = p_port;
		core::Result<net::NetSimConfig> ns = net::parse_netsim(p_netsim);
		REQUIRE(ns.ok());
		if (!ns.ok()) {
			return false;
		}
		cc.netsim = *ns;
		cc.netsim.seed = p_seed * 7919u + 1u;
		cc.request_dev_commands = true;
		cc.gunplay = m2a_fake::inputs();
		cc.mechanics_cfg = "server.cfg";
		cc.record_predicted = true;
		if (p_configure) {
			p_configure(cc);
		}
		const core::Status st = client.start(cc, p_level);
		CHECK_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
		return st.ok();
	}

	void poll() {
		client.read_presentation(*frame);
		const host::PresentationFrame &f = *frame;
		if (f.valid && f.own_curr.life > input.life) {
			input.life = f.own_curr.life;
			input.yaw = std::isnan(adopt_yaw) ? sim::yaw_radians(f.own_curr.yaw) : adopt_yaw;
			input.pitch = sim::pitch_radians(f.own_curr.pitch);
		}
		host::set_view_sample(input, f, net::monotonic_ns());
		client.set_input(input);
		const size_t n = client.events().drain(scratch);
		events.insert(events.end(), scratch.begin(), scratch.begin() + std::ptrdiff_t(n));
	}

	void run_for(double p_seconds, const std::function<void(double)> &p_each = {}) {
		const auto start = std::chrono::steady_clock::now();
		for (;;) {
			const double t = tg::seconds_since(start);
			if (t >= p_seconds) {
				break;
			}
			if (p_each) {
				p_each(t);
			}
			poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
	}

	bool wait_spawned(double p_timeout_s = 8.0) {
		const auto start = std::chrono::steady_clock::now();
		while (tg::seconds_since(start) < p_timeout_s) {
			poll();
			if (frame->valid && frame->own_curr.life > 0 && input.life == frame->own_curr.life) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
		return false;
	}

	std::optional<net::DebugReport> teleport(const sim::Vec3 &p_position, uint16_t p_yaw) {
		net::DebugRequest q;
		q.kind = net::DebugKind::Teleport;
		q.position = p_position;
		q.yaw = p_yaw;
		q.id = 77;
		client.send_debug(q);
		const auto start = std::chrono::steady_clock::now();
		while (tg::seconds_since(start) < 10.0) {
			poll();
			net::DebugReport r;
			while (client.take_debug_report(r)) {
				if (r.id == q.id) {
					return r;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
		return std::nullopt;
	}

	size_t count(host::PresentKind p_kind) const {
		return size_t(std::count_if(events.begin(), events.end(), [&](const host::PresentEvent &e) { return e.kind == p_kind; }));
	}
};

std::shared_ptr<const host::SharedLevel> floor_level() {
	return tg::synthetic_floor();
}

GunServerConfig server_config() {
	GunServerConfig c;
	c.level = floor_level();
	c.inputs = m2a_fake::inputs();
	return c;
}

void print_client(const char *p_label, const host::ClientStats &p_s) {
	std::printf("%s: reconciliations %" PRIu64 ", weapon_reconciliations %" PRIu64 ", weapon_ring_misses %" PRIu64 ", health_rebases %" PRIu64
				", ring_misses %" PRIu64 ", predicted_shots %" PRIu64 ", predicted_timed %" PRIu64 ", no_view_shots %" PRIu64 ", remote_shots %" PRIu64
				", hits %" PRIu64 ", snapshot_others %" PRIu64 ", late_sample %" PRIu64 ", generation_reorder %" PRIu64 ", others_ahead %" PRIu64
				", track_timeout %" PRIu64 ", events_overflow %" PRIu64 ", step_precondition_failures %" PRIu64 ", lane gaps %" PRIu64 ", rtt %.1f ms\n",
			p_label, p_s.reconciliations, p_s.weapon_reconciliations, p_s.weapon_ring_misses, p_s.health_rebases, p_s.ring_misses, p_s.predicted_shots,
			p_s.predicted_timed, p_s.no_view_shots, p_s.remote_shots, p_s.combat_hits, p_s.snapshot_others_received, p_s.late_sample,
			p_s.generation_reorder, p_s.others_ahead, p_s.track_timeout, p_s.events_overflow, p_s.step_precondition_failures, p_s.lane.gaps, p_s.rtt_ms);
}

// A synthetic own-world record source: per seq a tick and the records of the main Snapshot.
struct FakeRecords final : host::ReceivedRecordSource {
	std::map<uint16_t, std::pair<uint32_t, std::vector<net::OtherSoldier>>> seqs;
	size_t candidates(const host::ShotView &p_view, uint8_t p_target, std::span<net::TimedOther> r_out) const override {
		size_t n = 0;
		for (uint32_t i = 0; i < 32 && n < r_out.size(); ++i) {
			if (((p_view.main_bits >> i) & 1u) == 0) {
				continue;
			}
			auto it = seqs.find(uint16_t(p_view.view_seq - i));
			if (it == seqs.end()) {
				continue;
			}
			for (const net::OtherSoldier &o : it->second.second) {
				if (o.slot == p_target && n < r_out.size()) {
					r_out[n++] = { it->second.first, o };
				}
			}
		}
		return n;
	}
	bool view_tick(uint16_t p_seq, uint32_t &r_tick) const override {
		auto it = seqs.find(p_seq);
		if (it == seqs.end()) {
			return false;
		}
		r_tick = it->second.first;
		return true;
	}
	void slots_in(const host::ShotView &p_view, std::array<uint64_t, 2> &r_slots) const override {
		r_slots = {};
		for (uint32_t i = 0; i < 32; ++i) {
			if (((p_view.main_bits >> i) & 1u) == 0) {
				continue;
			}
			auto it = seqs.find(uint16_t(p_view.view_seq - i));
			if (it == seqs.end()) {
				continue;
			}
			for (const net::OtherSoldier &o : it->second.second) {
				r_slots[o.slot >> 6] |= uint64_t(1) << (o.slot & 63);
			}
		}
	}
};

net::OtherSoldier record_at(uint8_t p_slot, const sim::Vec3 &p_feet, const sim::Vec3 &p_velocity) {
	sim::SoldierState s;
	s.life = 1;
	s.position = p_feet;
	s.velocity = p_velocity;
	s.flags = sim::soldier_flags::kAlive;
	return net::quantize_other(p_slot, s);
}

sw::ShotEvent own_shot(const sw::WeaponTable &p_table, uint16_t p_def, uint8_t p_slot, uint32_t p_index, sim::Tick p_tick, const sim::Vec3 &p_origin,
		const sim::Vec3 &p_dir) {
	sw::ShotEvent e;
	e.time = sw::tick_start(p_tick);
	e.spawn_time = e.time + p_table.defs[p_def].spawn_delay;
	e.seed = sw::shot_seed(1, p_slot, p_index);
	e.player_slot = p_slot;
	e.shot_index = p_index;
	e.def = p_def;
	e.origin = p_origin;
	e.aim = p_dir;
	e.direction = p_dir;
	return e;
}

} // namespace

// ---- unit ---------------------------------------------------------------------------------------------------------

TEST_SUITE("host-gunplay-client-unit") {
	TEST_CASE("host.gunplay.client: PresentationEvents keeps order across threads and counts a full ring") {
		auto ring = std::make_unique<host::PresentationEvents>();
		std::vector<host::PresentEvent> out(host::kPresentationEventCapacity + 10);
		for (uint32_t i = 0; i < host::kPresentationEventCapacity + 5; ++i) {
			host::PresentEvent e;
			e.id = i;
			const bool ok = ring->push(e);
			CHECK(ok == (i < host::kPresentationEventCapacity));
		}
		CHECK(ring->overflows() == 5);
		CHECK(ring->drain(out) == host::kPresentationEventCapacity);
		CHECK(out[0].id == 0);
		CHECK(out[host::kPresentationEventCapacity - 1].id == host::kPresentationEventCapacity - 1);
		// One producer, one consumer, 1,000,000 events: every id once, in order.
		constexpr uint32_t kCount = 1'000'000;
		std::atomic<bool> done{ false };
		std::thread producer([&] {
			for (uint32_t i = 0; i < kCount;) {
				host::PresentEvent e;
				e.id = i;
				e.seed = uint64_t(i) * 0x9E3779B97F4A7C15ull;
				if (ring->push(e)) {
					++i;
				} else {
					std::this_thread::yield();
				}
			}
			done = true;
		});
		uint32_t expect = 0;
		bool order = true;
		std::vector<host::PresentEvent> batch(512);
		while (expect < kCount) {
			const size_t n = ring->drain(batch);
			for (size_t k = 0; k < n; ++k) {
				order = order && batch[k].id == expect && batch[k].seed == uint64_t(expect) * 0x9E3779B97F4A7C15ull;
				++expect;
			}
			if (n == 0) {
				std::this_thread::yield();
			}
		}
		producer.join();
		CHECK(order);
		std::printf("events ring: %u events in order across threads, overflows %" PRIu64 " (the producer retried on full)\n", kCount, ring->overflows());
	}

	TEST_CASE("host.gunplay.client: present_weapon from two predicted ticks (phase, ammo, spread and recoil lerp, zoom, no lerp across items)") {
		const m2a_fake::Tables &t = m2a_fake::tables();
		sw::SoldierWeaponsState w;
		w.player_slot = 3;
		const std::array<uint16_t, 2> loadout{ t.rifle, t.pistol };
		REQUIRE(sw::spawn_soldier_weapons(w, *t.table, loadout, 0, 100).ok());
		sw::WeaponContext ctx;
		ctx.eye_start = { 0, 1.6f, 0 };
		ctx.eye_end = { 0, 1.6f, 0 };
		std::vector<sw::SoldierWeaponsState> states{ w };
		for (sim::Tick tick = 101; tick <= 200; ++tick) {
			sim::Command c;
			c.tick = tick;
			if (tick >= 160 && tick < 170) {
				c.buttons = sim::buttons::kFire;
			}
			if (tick >= 180) {
				c.buttons = sim::buttons::kZoom;
			}
			ctx.yaw = c.yaw;
			ctx.pitch = c.pitch;
			sw::WeaponTickOutput out;
			sw::step_weapons_m2a(*t.table, w, c, ctx, 1, 100, out);
			states.push_back(w);
		}
		auto view = [&](sim::Tick p_tick) { return net::weapons_wire_view(states[p_tick - 100], p_tick); };
		// Deploying right after the spawn: phase progress runs 0..1 over the deploy.
		const host::PresentedWeapon d = host::present_weapon(view(110), view(111), 111, 0.5, *t.table);
		REQUIRE(d.valid);
		CHECK(d.def == t.rifle);
		CHECK(d.magazine == 31);
		CHECK(d.magazine_capacity == 31);
		CHECK(d.phase_progress > 0.0f);
		CHECK(d.phase_progress < 1.0f);
		CHECK(std::fabs(d.time_s - (sw::units_to_seconds(sw::tick_end(110)) + 0.5 / 60.0)) < 1e-9);
		// Firing: the magazine goes down, the spread lerps between the two ticks.
		const host::PresentedWeapon f0 = host::present_weapon(view(164), view(165), 165, 0.0, *t.table);
		const host::PresentedWeapon f1 = host::present_weapon(view(164), view(165), 165, 1.0, *t.table);
		const host::PresentedWeapon fh = host::present_weapon(view(164), view(165), 165, 0.5, *t.table);
		CHECK(f0.magazine < 31);
		CHECK(f0.spread_deg == doctest::Approx(view(164).timers[0].spread));
		CHECK(f1.spread_deg == doctest::Approx(view(165).timers[0].spread));
		CHECK(fh.spread_deg == doctest::Approx(0.5f * (f0.spread_deg + f1.spread_deg)));
		CHECK(fh.recoil.pitch == doctest::Approx(0.5f * (f0.recoil.pitch + f1.recoil.pitch)));
		// Zoom held: kZoomWasDown.
		const host::PresentedWeapon z = host::present_weapon(view(190), view(191), 191, 0.3, *t.table);
		CHECK(z.zoomed);
		// A different item in the previous tick: the newer item's values, no lerp.
		net::WeaponsWire other = view(164);
		other.slots[other.active].def = t.pistol;
		const host::PresentedWeapon s = host::present_weapon(other, view(165), 165, 0.5, *t.table);
		CHECK(s.spread_deg == doctest::Approx(view(165).timers[0].spread));
		// Hostile: a def outside the table, an active slot out of range.
		net::WeaponsWire bad = view(165);
		bad.slots[bad.active].def = uint16_t(t.table->defs.size() + 3);
		CHECK(!host::present_weapon(view(164), bad, 165, 0.5, *t.table).valid);
		bad = view(165);
		bad.active = 200;
		CHECK(!host::present_weapon(view(164), bad, 165, 0.5, *t.table).valid);
	}

	TEST_CASE("host.gunplay.client: the weapon lane replays under catch_up without emitting twice; a dead soldier steps no weapon") {
		const m2a_fake::Tables &t = m2a_fake::tables();
		std::shared_ptr<const host::SharedLevel> level = floor_level();
		REQUIRE(level != nullptr);
		core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(level->static_world);
		REQUIRE(world.ok());
		sim::SoldierMotor motor;
		REQUIRE(motor.init(**world, level->motor).ok());
		const sim::SettleResult spawn = sim::settle_spawn(motor, **world, level->spawns[0], 1, 50);
		REQUIRE(spawn.ok);
		struct Sink final : sim::WeaponEmitSink {
			std::vector<std::pair<sim::Tick, uint32_t>> shots;
			uint64_t ticks = 0;
			void emitted(const sim::Command &p_c, const sw::WeaponTickOutput &p_o, const sim::WeaponPredictionEntry &) override {
				++ticks;
				for (size_t i = 0; i < p_o.shot_count; ++i) {
					shots.push_back({ p_c.tick, p_o.shots[i].shot_index });
				}
			}
		} sink;
		sim::PredictionRing ring(128);
		sim::WeaponLane lane(128, t.table, level->motor, 0x1234);
		lane.set_sink(&sink);
		ring.attach_weapons(&lane);
		sim::WeaponPredictionEntry w0;
		w0.tick = 50;
		w0.spawn_tick = 50;
		w0.state.player_slot = 2;
		const std::array<uint16_t, 2> loadout{ t.rifle, t.pistol };
		REQUIRE(sw::spawn_soldier_weapons(w0.state, *t.table, loadout, 0, 50).ok());
		lane.put(w0);
		lane.set_emitted_through(50);
		ring.put({ sim::hold_command(spawn.state), spawn.state, false });
		std::map<sim::Tick, uint64_t> first;
		sim::SoldierState state = spawn.state;
		for (sim::Tick tick = 51; tick <= 170; ++tick) {
			sim::Command c;
			c.tick = tick;
			c.sequence = tick;
			c.yaw = uint16_t(tick * 37u);
			c.move_forward = tick % 40u < 20u ? int8_t(127) : int8_t(0);
			if (tick >= 100 && tick < 130) {
				c.buttons |= sim::buttons::kFire;
			}
			const sim::SoldierState before = state;
			motor.step(state, c);
			const sim::WeaponPredictionEntry e = lane.step(*lane.find(tick - 1u), c, before, state);
			lane.put(e);
			ring.put({ c, state, true });
			first[tick] = sw::soldier_weapons_hash(e.state);
		}
		const size_t shots = sink.shots.size();
		CHECK(shots >= 6);
		CHECK(sink.ticks == 120);
		// Replay from tick 90 (a reconciliation): every weapon state equal, nothing emitted again.
		const sim::SoldierState base = ring.find(90)->state;
		const sim::CatchUpStats c = sim::catch_up(motor, base, ring, 171);
		CHECK(c.steps == 80);
		bool equal = true;
		for (sim::Tick tick = 90; tick <= 170; ++tick) {
			const sim::WeaponPredictionEntry *e = lane.find(tick);
			equal = equal && e != nullptr && sw::soldier_weapons_hash(e->state) == first[tick];
		}
		CHECK(equal);
		CHECK(sink.shots.size() == shots);
		CHECK(lane.stats().replayed_ticks == 80);
		CHECK(lane.stats().gaps == 0);
		// A changed base weapon state at 95 (a wire rebase): the replay recomputes from it.
		sim::WeaponPredictionEntry rebased = *lane.find(95);
		rebased.state.slots[0].magazine = 3;
		lane.put(rebased);
		(void)sim::catch_up(motor, ring.find(95)->state, ring, 171);
		CHECK(lane.find(170)->state.slots[0].magazine <= 3);
		// Dead before the tick: carried, no events.
		sim::SoldierState dead = state;
		dead.mode = sim::MoveMode::Dead;
		sim::Command c2;
		c2.tick = 171;
		c2.buttons = sim::buttons::kFire;
		const sim::WeaponPredictionEntry carried = lane.step(*lane.find(170), c2, dead, dead);
		CHECK(carried.state.tick == 171);
		CHECK(carried.state.events == 0);
		CHECK(carried.state.next_shot_index == lane.find(170)->state.next_shot_index);
		CHECK(lane.stats().dead_ticks == 1);
		std::printf("weapon lane: %zu shots emitted once over 120 ticks, 80-tick replay equal by soldier_weapons_hash, dead tick carried\n", shots);
	}

	TEST_CASE("host.gunplay.client: ClientProjectiles: own world on the rewound records (E12 log), remote world, impacts, seeds, refusals") {
		const m2a_fake::Tables &t = m2a_fake::tables();
		std::shared_ptr<const host::SharedLevel> level = floor_level();
		core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(level->static_world);
		REQUIRE(world.ok());
		host::ClientProjectilesConfig pc;
		pc.own_slot = 2;
		core::Result<std::unique_ptr<host::ClientProjectiles>> made = host::ClientProjectiles::create(t.table, t.capsules, 1, pc);
		REQUIRE(made.ok());
		host::ClientProjectiles &p = **made;
		CHECK(!host::ClientProjectiles::create(nullptr, t.capsules, 1).ok());
		auto ring = std::make_unique<host::PresentationEvents>();
		// The target (slot 5) stood at x = 0 at seq 9 / 10 (ticks 98, 100) and moved to x = 3 afterwards: the
		// rewound test must hit it where those records put it.
		FakeRecords records;
		records.seqs[9] = { 98, { record_at(5, { 0.0f, 0.0f, 20.0f }, { 0, 0, 0 }) } };
		records.seqs[10] = { 100, { record_at(5, { 0.0f, 0.0f, 20.0f }, { 0, 0, 0 }) } };
		host::ShotView view;
		view.quality = uint8_t(sim::ViewQuality::Interpolated);
		view.view_seq = 10;
		view.main_bits = 0b11;
		const sim::Tick tick = 110;
		view.view_back_q8 = int32_t((tick << 8) - (100u << 8)); // drawn at render time 100
		const sim::Vec3 origin{ 0.0f, 1.3f, 0.0f };
		const sw::ShotEvent shot = own_shot(*t.table, t.rifle, 2, 7, tick, origin, { 0.0f, 0.0f, 1.0f });
		p.spawn_own(shot, view);
		for (sim::Tick k = tick; k < tick + 10; ++k) {
			p.step_own(k, **world, records, *ring);
		}
		std::vector<host::PresentEvent> ev(256);
		size_t n = ring->drain(ev);
		size_t contacts = 0;
		for (size_t i = 0; i < n; ++i) {
			if (ev[i].kind == host::PresentKind::SoldierContact) {
				++contacts;
				CHECK(ev[i].aux == 5);
				CHECK(ev[i].id == 7);
				CHECK(ev[i].timeline == host::PresentTimeline::Predicted);
				CHECK(uint8_t(ev[i].value) == uint8_t(sw::HitZone::Chest));
			}
		}
		CHECK(contacts == 1);
		std::vector<host::OwnTestSample> samples(64);
		const size_t got = p.take_samples(samples);
		CHECK(got >= 1);
		if (got >= 1) {
			CHECK(samples[0].target == 5);
			CHECK(samples[0].shot_index == 7);
			CHECK(samples[0].time_q8 == net::rewind_time_q8(sw::tick_start(tick), view.view_back_q8, host::kDefaultMaxRewindTicks));
		}
		// Without a stored view (a repeat): tested against nothing on the own world (counted), no contact.
		const sw::ShotEvent stray = own_shot(*t.table, t.rifle, 2, 8, tick + 20, origin, { 0.0f, 0.0f, 1.0f });
		host::ShotView none;
		none.view_seq = 99; // not held
		none.quality = uint8_t(sim::ViewQuality::Interpolated);
		p.spawn_own(stray, none);
		for (sim::Tick k = tick + 20; k < tick + 30; ++k) {
			p.step_own(k, **world, records, *ring);
		}
		n = ring->drain(ev);
		CHECK(std::none_of(ev.begin(), ev.begin() + std::ptrdiff_t(n), [](const host::PresentEvent &e) { return e.kind == host::PresentKind::SoldierContact; }));
		CHECK(p.stats().own_no_view > 0);
		// Remote: a shot of slot 9 down into the floor, spawned when the render time reaches it.
		net::RemoteShot r;
		r.slot = 9;
		r.shot_index = 41;
		r.def = t.rifle;
		r.time = sw::tick_start(300);
		r.origin = { 5.0f, 1.6f, 5.0f };
		r.direction = { 0.0f, -0.7071068f, 0.7071068f };
		p.queue_remote(r);
		net::RemoteShot bad = r;
		bad.def = uint16_t(t.table->defs.size() + 1);
		p.queue_remote(bad);
		CHECK(p.stats().refused == 1);
		const std::vector<net::OtherPose> others;
		p.step_remote(295.0, **world, others, *ring);
		CHECK(p.stats().remote_spawned == 0);
		for (double rt = 296.0; rt <= 320.0; rt += 1.0) {
			p.step_remote(rt, **world, others, *ring);
		}
		CHECK(p.stats().remote_spawned == 1);
		n = ring->drain(ev);
		size_t impacts = 0;
		for (size_t i = 0; i < n; ++i) {
			if (ev[i].kind == host::PresentKind::Impact && ev[i].slot == 9) {
				++impacts;
				CHECK(ev[i].timeline == host::PresentTimeline::Render);
				CHECK(ev[i].seed == host::impact_seed(host::effect_seed(1, 9, 41), 0, ev[i].extra));
				CHECK(std::fabs(ev[i].pos[1]) < 0.01f); // the floor at y = 0
			}
		}
		CHECK(impacts == 1);
		// The seed rules of §4.4.
		CHECK(host::effect_seed(1, 9, 41) == sw::splitmix64(1 ^ (uint64_t(9) << 32 | 41u)));
		sw::ShotEvent derived;
		REQUIRE(host::shot_event_of(r, *t.table, 1, derived));
		CHECK(derived.seed == sw::shot_seed(1, 9, 41));
		CHECK(derived.spawn_time == r.time + t.table->defs[t.rifle].spawn_delay);
		CHECK(!host::shot_event_of(bad, *t.table, 1, derived));
		std::printf("client projectiles: own contact %zu (chest of slot 5 at its rewound record), %" PRIu64 " E12 samples, remote impact %zu, refused %" PRIu64 "\n",
				contacts, p.stats().samples_logged, impacts, p.stats().refused);
	}

	TEST_CASE("host.gunplay.client: the view sample, present_view_quality and window_extrapolated") {
		auto f = std::make_unique<host::PresentationFrame>();
		CHECK(host::present_view_quality(*f, 100.0) == sim::ViewQuality::None);
		f->serial = 5;
		f->publish_ns = 1'000'000'000ull;
		f->render_ticks_at_publish = 100.0;
		f->ticks_per_ns = 60e-9;
		f->newest_seq = 321;
		f->other_count = 1;
		host::OtherTrack &t = f->others[0];
		t.slot = 4;
		t.count = 2;
		t.samples[0] = { 98, record_at(4, { 0, 0, 0 }, { 1, 0, 0 }) };
		t.samples[1] = { 100, record_at(4, { 0.0333f, 0, 0 }, { 1, 0, 0 }) };
		t.track_newest_tick = 100;
		CHECK(host::present_view_quality(*f, 99.0) == sim::ViewQuality::Interpolated);
		CHECK(host::present_view_quality(*f, 101.0) == sim::ViewQuality::Extrapolated);
		CHECK(host::present_view_quality(*f, 100.0 + 15.5) == sim::ViewQuality::Held);
		host::InputSample in;
		host::set_view_sample(in, *f, f->publish_ns + 8'333'333ull); // half a tick later
		CHECK(in.view_valid);
		CHECK(in.view_render_ticks == doctest::Approx(100.5));
		CHECK(in.view_quality == uint8_t(sim::ViewQuality::Extrapolated));
		CHECK(in.view_seq == 321);
		std::array<host::PresentedSoldier, 4> out;
		host::PresentOthersCounts counts;
		CHECK(host::present_others(*f, f->publish_ns + 16'666'667ull, out, counts) == 1);
		CHECK(counts.extrapolated == 1);
		CHECK(counts.window_extrapolated == 0);
		t.track_newest_tick = 102; // the client's track held a newer sample the window did not
		host::PresentOthersCounts counts2;
		host::present_others(*f, f->publish_ns + 16'666'667ull, out, counts2);
		CHECK(counts2.window_extrapolated == 1);
		// The M1 signature adds the extrapolated ones and draws the same.
		uint64_t extra = 0;
		std::array<host::PresentedSoldier, 4> out1;
		CHECK(host::present_others(*f, f->publish_ns + 16'666'667ull, out1, &extra) == 1);
		CHECK(extra == 1);
		CHECK(same_value(out1[0].position, out[0].position));
		CHECK(out1[0].target_pose == out[0].target_pose);
	}

	TEST_CASE("host.gunplay.client: E6: a client whose weapon table differs is refused on MatchInfo, never sends Ready, never predicts") {
		struct Row {
			const char *label;
			std::string cfg;
			uint64_t kit_hash;
			bool no_inputs;
			const char *reason;
		};
		const Row rows[] = {
			{ "mechanics differ (variant.cfg)", "variant.cfg", 0, false, "weapon table mismatch" },
			{ "cfg unreadable (missing.cfg)", "missing.cfg", 0, false, "weapon table mismatch" },
			{ "no weapon table", "server.cfg", 0, true, "weapon table mismatch" },
			{ "kit table differs", "server.cfg", 0xBADull, false, "kit table mismatch" },
		};
		for (const Row &row : rows) {
			GunServer server;
			std::string err;
			REQUIRE_MESSAGE(server.start(server_config(), err), err);
			Player p;
			REQUIRE(p.start(server.port(), floor_level(), "off", 3, [&](host::ClientSessionConfig &r_c) {
				r_c.mechanics_cfg = row.cfg;
				r_c.gunplay = row.no_inputs ? nullptr : m2a_fake::inputs(row.kit_hash);
			}));
			p.run_for(1.5);
			const host::ClientStats s = p.client.stats();
			const std::string error(s.last_error.data());
			CHECK_MESSAGE(s.refused, row.label);
			CHECK_MESSAGE(error.find(row.reason) != std::string::npos, row.label << ": " << error);
			CHECK_MESSAGE((row.kit_hash != 0 ? s.kit_hash_mismatch : s.weapon_hash_mismatch) == 1, row.label);
			CHECK_MESSAGE(server.readys() == 0, row.label);
			CHECK_MESSAGE(s.commands_sent == 0, row.label);
			CHECK_MESSAGE(!p.frame->valid, row.label);
			CHECK_MESSAGE(!s.connected, row.label);
			std::printf("E6 %s: refused \"%s\", weapon_hash_mismatch %" PRIu64 ", kit_hash_mismatch %" PRIu64 ", Ready %" PRIu64 ", commands %" PRIu64 "\n", row.label,
					error.c_str(), s.weapon_hash_mismatch, s.kit_hash_mismatch, server.readys(), s.commands_sent);
		}
		// The matched client: Ready once, spawned, predicting; an M1 server (no weapon_hash): Ready, no gunplay.
		for (const bool gunplay : { true, false }) {
			GunServer server;
			std::string err;
			GunServerConfig sc = server_config();
			sc.gunplay = gunplay;
			REQUIRE_MESSAGE(server.start(sc, err), err);
			Player p;
			REQUIRE(p.start(server.port(), floor_level(), "off", 4));
			CHECK(p.wait_spawned());
			p.run_for(0.5);
			const host::ClientStats s = p.client.stats();
			CHECK(!s.refused);
			CHECK(server.readys() == 1);
			CHECK(s.gunplay == gunplay);
			CHECK(s.weapon_hash_mismatch == 0);
			CHECK(p.frame->own_weapons_valid == gunplay);
			std::printf("E6 matched (%s): Ready %" PRIu64 ", gunplay %d, weapon_hash 0x%016" PRIx64 "\n", gunplay ? "gunplay server" : "M1 server", server.readys(),
					int(s.gunplay), server.weapon_hash());
		}
	}

	TEST_CASE("host.gunplay.client: R2: a server hit (health only) is a health rebase, not a reconciliation; SnapshotOthers merge and acks") {
		GunServer server;
		std::string err;
		GunServerConfig sc = server_config();
		sc.fake_damage_every_ticks = 30;
		sc.split_keep = 0; // every other travels in the SnapshotOthers
		sc.bots.push_back({ 2, false, m2a_fake::strafing_bot() });
		sc.bots.push_back({ 2, true, m2a_fake::firing_bot(7) });
		REQUIRE_MESSAGE(server.start(sc, err), err);
		Player p;
		REQUIRE(p.start(server.port(), floor_level(), "off", 5));
		REQUIRE(p.wait_spawned());
		// At least 4 s, then until 3 virtual hits and 50 SnapshotOthers have arrived (a loaded machine ticks slower), at most 20 s.
		p.run_for(4.0);
		for (const auto t0 = std::chrono::steady_clock::now(); tg::seconds_since(t0) < 16.0;) {
			const host::ClientStats now = p.client.stats();
			if (now.health_rebases >= 3 && now.combat_hits >= 3 && now.snapshot_others_received > 50) {
				break;
			}
			p.run_for(0.25);
		}
		const host::ClientStats s = p.client.stats();
		const m2a_fake::ServerLog log = server.take_log();
		print_client("R2 / SnapshotOthers", s);
		CHECK(s.health_rebases >= 3);
		CHECK(s.reconciliations == 0);
		CHECK(s.weapon_reconciliations == 0);
		CHECK(s.combat_hits >= 3);
		CHECK(p.frame->own_curr.health < 100.0f);
		CHECK(p.count(host::PresentKind::Hit) >= 3);
		CHECK(std::all_of(p.events.begin(), p.events.end(), [](const host::PresentEvent &e) {
			return e.kind != host::PresentKind::Hit || (e.timeline == host::PresentTimeline::Immediate && (e.flags & host::present_flags::kOwnVictim) != 0);
		}));
		CHECK(s.snapshot_others_received > 50);
		CHECK(p.frame->other_count == 2);
		CHECK(log.lag.acks_ignored == 0);
		CHECK(s.remote_shots > 0);
		CHECK(p.count(host::PresentKind::RemoteShot) == s.remote_shots);
		CHECK(s.projectiles.remote_spawned > 0);
		CHECK(p.count(host::PresentKind::Impact) > 0);
		std::printf("R2: health_rebases %" PRIu64 " for %" PRIu64 " virtual hits, health %.1f, reconciliations %" PRIu64 "; SnapshotOthers %" PRIu64
					" received of %" PRIu64 " sent, acks_ignored %" PRIu64 ", remote shots %" PRIu64 ", remote impacts %" PRIu64 "\n",
				s.health_rebases, log.fake_hits, double(p.frame->own_curr.health), s.reconciliations, s.snapshot_others_received, log.snapshot_others,
				log.lag.acks_ignored, s.remote_shots, s.projectiles.remote_impacts);
	}
}

TEST_SUITE("host-gunplay-client-unit") {
	TEST_CASE("host.gunplay.client: hostile gunplay datagrams are refused and counted; the session keeps predicting") {
		GunServer server;
		std::string err;
		GunServerConfig sc = server_config();
		sc.hostile = true;
		sc.split_keep = 1;
		sc.fake_damage_every_ticks = 45;
		sc.bots.push_back({ 2, true, m2a_fake::firing_bot(5) });
		sc.bots.push_back({ 2, true, m2a_fake::firing_bot(35) });
		sc.bots.push_back({ 2, false, m2a_fake::strafing_bot() });
		REQUIRE_MESSAGE(server.start(sc, err), err);
		Player p;
		REQUIRE(p.start(server.port(), floor_level(), "off", 9));
		REQUIRE(p.wait_spawned());
		p.run_for(5.0, [&](double p_t) {
			p.input.held_buttons = std::fmod(p_t, 1.0) < 0.3 ? sim::buttons::kFire : 0u;
			p.input.yaw = float(p_t * 0.5);
		});
		const host::ClientStats s = p.client.stats();
		const m2a_fake::ServerLog log = server.take_log();
		print_client("hostile", s);
		CHECK(s.connected);
		CHECK(!s.refused);
		CHECK(p.frame->valid);
		CHECK(log.hostile_sent > 100);
		CHECK(s.bad_gunplay_datagrams > 0);
		// The unknown-def shot and the hit on slot 250 decode (the wire carries any def and slot byte) and are dropped
		// by the client's range checks before any event.
		net::ShotEvents probe;
		probe.server_tick = 600;
		probe.shots.push_back(m2a_fake::hostile_shot(600, { 0.0f, 1.6f, 0.0f }));
		const sim::Vec3 eye0{ 0.0f, 1.6f, 0.0f };
		const core::Result<net::ShotEvents> decoded =
				net::decode_shot_events(net::encode_shot_events(probe, m2a_fake::inputs()->shot_frame, eye0), m2a_fake::inputs()->shot_frame);
		REQUIRE(decoded.ok());
		REQUIRE(decoded->shots.size() == 1);
		CHECK(decoded->shots[0].def >= m2a_fake::tables().table->defs.size());
		std::printf("hostile probe: def %u decodes (table %zu defs), slot %u\n", unsigned(decoded->shots[0].def), m2a_fake::tables().table->defs.size(),
				unsigned(decoded->shots[0].slot));
		CHECK(s.bad_gunplay_events > 0);
		CHECK(std::none_of(p.events.begin(), p.events.end(), [&](const host::PresentEvent &e) {
			return e.slot >= net::kMaxSlots || e.aux >= net::kMaxSlots || (e.kind == host::PresentKind::RemoteShot && e.def >= m2a_fake::tables().table->defs.size());
		}));
		CHECK(s.bad_weapon_wires > 0);
		CHECK(s.step_precondition_failures == 0);
		CHECK(s.lane.gaps == 0);
		CHECK(s.predicted_shots > 0);
		std::printf("hostile: %" PRIu64 " mutated datagrams sent; refused to decode %" PRIu64 ", out-of-range events dropped %" PRIu64 ", own weapon wires refused %" PRIu64
					", still predicting (%" PRIu64 " own shots, %" PRIu64 " weapon reconciliations)\n",
				log.hostile_sent, s.bad_gunplay_datagrams, s.bad_gunplay_events, s.bad_weapon_wires, s.predicted_shots, s.weapon_reconciliations);
	}
}

// ---- timing (OPENBF3_CL_TIMING=1, under the quiet lock) ------------------------------------------------------------

namespace {

struct LagcompResult {
	size_t shots = 0;
	size_t client_hits = 0;
	size_t both = 0;
	size_t same_zone = 0;
	size_t compared = 0, equal = 0, mismatched = 0, client_only = 0;
	host::ClientStats stats;
	m2a_fake::ServerLog log;
	double strafe_speed = 0;
	uint64_t draws = 0, window_extrapolated = 0, drawn_extrapolated = 0, drawn_held = 0;
};

LagcompResult run_lagcomp(const char *p_label, const std::string &p_netsim, uint32_t p_server_rewind, size_t p_shots) {
	LagcompResult r;
	GunServer server;
	std::string err;
	GunServerConfig sc = server_config();
	sc.weapon_flags = net::weapon_flags::kExactAim | net::weapon_flags::kInfiniteMagazine;
	sc.max_rewind_ticks = p_server_rewind;
	sc.bots.push_back({ 2, false, m2a_fake::strafing_bot() });
	REQUIRE_MESSAGE(server.start(sc, err), err);
	const uint32_t bot = uint32_t(server.bot_slots().front());
	Player p;
	REQUIRE(p.start(server.port(), floor_level(), p_netsim, 11, [](host::ClientSessionConfig &r_c) { r_c.own_test_max_rewind_ticks = int32_t(host::kDefaultMaxRewindTicks); }));
	REQUIRE(p.wait_spawned());
	// Stand 20 m to the side of the target's strafe line (across its velocity), facing it: the target crosses the
	// view at 4 m/s, so the ~250 ms the client draws it behind is about 1 m sideways (the control run's miss).
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	const sim::SoldierState b0 = server.state_of(bot);
	const float vl = std::sqrt(b0.velocity[0] * b0.velocity[0] + b0.velocity[2] * b0.velocity[2]);
	REQUIRE(vl > 1.0f);
	const sim::Vec3 across{ -b0.velocity[2] / vl, 0.0f, b0.velocity[0] / vl };
	const sim::Vec3 at{ b0.position[0] + across[0] * 20.0f, 0.05f, b0.position[2] + across[2] * 20.0f };
	const uint16_t face = sim::quantize_yaw(std::atan2(-across[0], -across[2]));
	p.adopt_yaw = sim::yaw_radians(face);
	p.input.yaw = sim::yaw_radians(face);
	REQUIRE(p.teleport(at, face).has_value());
	p.run_for(1.5);
	std::set<uint32_t> fired;
	std::map<uint32_t, uint8_t> client_hit; // shot index -> zone
	sim::Tick last_press = 0;
	double speed_sum = 0;
	size_t speed_n = 0;
	p.input.held_buttons = sim::buttons::kZoom; // aimed down the sights: the smaller spread
	const auto start = std::chrono::steady_clock::now();
	while (fired.size() < p_shots && tg::seconds_since(start) < double(p_shots) / 4.0 + 60.0) {
		p.poll();
		const host::PresentationFrame &f = *p.frame;
		std::array<host::PresentedSoldier, 8> drawn;
		host::PresentOthersCounts counts;
		const size_t n = host::present_others(f, net::monotonic_ns(), drawn, counts);
		r.window_extrapolated += counts.window_extrapolated;
		r.drawn_extrapolated += counts.extrapolated;
		r.drawn_held += counts.held;
		++r.draws;
		for (size_t i = 0; i < n; ++i) {
			if (drawn[i].slot != bot || !f.valid) {
				continue;
			}
			const sim::Vec3 eye = tg::own_eye(f);
			const float dx = drawn[i].position[0] - eye[0], dy = drawn[i].position[1] + 1.3f - eye[1], dz = drawn[i].position[2] - eye[2];
			p.input.yaw = std::atan2(dx, dz);
			p.input.pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
			speed_sum += std::sqrt(drawn[i].velocity[0] * drawn[i].velocity[0] + drawn[i].velocity[2] * drawn[i].velocity[2]);
			++speed_n;
			if (f.tick >= last_press + 12u) {
				++p.input.fire_presses;
				last_press = f.tick;
			}
		}
		p.client.set_input(p.input);
		for (const host::PresentEvent &e : p.events) {
			if (e.kind == host::PresentKind::OwnShot) {
				fired.insert(e.id);
			} else if (e.kind == host::PresentKind::SoldierContact && e.timeline == host::PresentTimeline::Predicted && e.aux == bot) {
				client_hit.emplace(e.id, uint8_t(e.value));
			}
		}
		p.events.clear();
		std::this_thread::sleep_for(std::chrono::milliseconds(3));
	}
	p.run_for(2.0);
	for (const host::PresentEvent &e : p.events) {
		if (e.kind == host::PresentKind::SoldierContact && e.timeline == host::PresentTimeline::Predicted && e.aux == bot) {
			client_hit.emplace(e.id, uint8_t(e.value));
		}
	}
	r.stats = p.client.stats();
	std::vector<host::OwnTestSample> samples;
	p.client.take_own_test_samples(samples);
	p.client.stop();
	server.stop();
	r.log = server.take_log();
	const uint8_t own = r.stats.slot;
	std::map<uint32_t, uint8_t> server_hit;
	for (const sw::ImpactEvent &e : r.log.target_impacts) {
		if (e.player_slot == own && e.target == bot) {
			server_hit.emplace(e.shot_index, uint8_t(e.zone));
		}
	}
	r.shots = fired.size();
	r.client_hits = client_hit.size();
	for (const auto &[shot, zone] : client_hit) {
		auto it = server_hit.find(shot);
		if (it != server_hit.end()) {
			++r.both;
			r.same_zone += it->second == zone ? 1u : 0u;
		}
	}
	// (b): the server's posed OtherPose (trace) against the client's cosmetic-test sample, keyed by shot, target, time.
	std::map<std::tuple<uint32_t, uint8_t, int64_t>, net::OtherPose> server_rows;
	for (const host::RewindTraceRow &row : r.log.trace) {
		if (row.shooter == own) {
			server_rows.emplace(std::make_tuple(row.shot_index, row.target, row.time_q8), row.pose);
		}
	}
	std::set<std::tuple<uint32_t, uint8_t, int64_t>> seen;
	for (const host::OwnTestSample &s : samples) {
		const auto key = std::make_tuple(s.shot_index, s.target, s.time_q8);
		if (!seen.insert(key).second) {
			continue;
		}
		auto it = server_rows.find(key);
		if (it == server_rows.end()) {
			++r.client_only;
			continue;
		}
		++r.compared;
		if (net::same_bits(it->second, s.pose)) {
			++r.equal;
		} else {
			++r.mismatched;
		}
	}
	r.strafe_speed = speed_n > 0 ? speed_sum / double(speed_n) : 0;
	std::printf("E12 %s (%s, server max_rewind %u): %zu shots, client test hit %zu, server hit %zu of them (%.2f %%), same zone %zu (%.2f %%); "
				"(b) samples compared %zu, equal %zu, mismatched %zu (late_sample %" PRIu64 "), client-only %zu (the server's broad phase culled or no row); "
				"server verdicts present %" PRIu64 " rewound %" PRIu64 " clamped %" PRIu64 " refused %" PRIu64 "; drawn strafe speed %.2f m/s\n",
			p_label, p_netsim.c_str(), p_server_rewind, r.shots, r.client_hits, r.both, r.client_hits ? 100.0 * double(r.both) / double(r.client_hits) : 0.0,
			r.same_zone, r.both ? 100.0 * double(r.same_zone) / double(r.both) : 0.0, r.compared, r.equal, r.mismatched, r.stats.late_sample, r.client_only,
			r.log.lag.present, r.log.lag.rewound, r.log.lag.clamped, r.log.lag.refused, r.strafe_speed);
	print_client(p_label, r.stats);
	std::printf("E12 %s residuals: generation_reorder %" PRIu64 ", others_ahead %" PRIu64 ", track_timeout %" PRIu64 ", window_extrapolated %" PRIu64
				" of %" PRIu64 " draws (extrapolated %" PRIu64 ", held %" PRIu64 "), refused views %" PRIu64 "\n",
			p_label, r.stats.generation_reorder, r.stats.others_ahead, r.stats.track_timeout, r.window_extrapolated, r.draws, r.drawn_extrapolated,
			r.drawn_held, r.log.refused_views);
	return r;
}

} // namespace

TEST_SUITE("host-gunplay-client") {
	TEST_CASE("host.gunplay.client: E1 + E5: 31 predicted shots equal the server's field for field; reload phase events server = predicted") {
		if (!timing_enabled("E1/E5")) {
			return;
		}
		GunServer server;
		std::string err;
		REQUIRE_MESSAGE(server.start(server_config(), err), err);
		Player p;
		REQUIRE(p.start(server.port(), floor_level(), "off", 21));
		REQUIRE(p.wait_spawned());
		p.run_for(1.5); // the deploy (0.67 s) is over
		// Fire held from a full magazine until it is empty, then released.
		p.input.held_buttons = sim::buttons::kFire;
		p.run_for(3.0);
		p.input.held_buttons = 0;
		p.run_for(0.5);
		// Empty reload (2.37 s), then a tactical one after 5 shots (1.8 s, commit at 0.85).
		p.input.held_buttons = sim::buttons::kReload;
		p.run_for(0.1);
		p.input.held_buttons = 0;
		p.run_for(3.0);
		p.input.held_buttons = sim::buttons::kFire;
		p.run_for(0.36);
		p.input.held_buttons = 0;
		p.run_for(0.3);
		p.input.held_buttons = sim::buttons::kReload;
		p.run_for(0.1);
		p.input.held_buttons = 0;
		p.run_for(2.5);
		const host::ClientStats s = p.client.stats();
		std::vector<host::PredictedShot> shots;
		std::vector<host::PredictedTimed> timed;
		p.client.take_predicted(shots, timed);
		p.client.stop();
		server.stop();
		const m2a_fake::ServerLog log = server.take_log();
		print_client("E1/E5", s);
		std::vector<m2a_fake::ServerShot> server_shots;
		for (const m2a_fake::ServerShot &x : log.shots) {
			if (x.shot.player_slot == s.slot) {
				server_shots.push_back(x);
			}
		}
		// E1: the first 31 shots (the full magazine).
		REQUIRE(shots.size() >= 31);
		REQUIRE(server_shots.size() >= 31);
		size_t equal = 0;
		std::string why;
		for (size_t i = 0; i < 31; ++i) {
			std::string w;
			if (same_shot(shots[i].shot, server_shots[i].shot, &w) && shots[i].tick == server_shots[i].tick) {
				++equal;
			} else if (why.empty()) {
				why = "shot " + std::to_string(i) + ": " + w;
			}
		}
		CHECK_MESSAGE(equal == 31, why);
		size_t gaps = 0;
		for (size_t i = 1; i < 31; ++i) {
			gaps += shots[i].shot.time - shots[i - 1].shot.time == 4'500'000 ? 1u : 0u;
		}
		CHECK(gaps == 30);
		CHECK(shots[0].shot.time == sw::tick_start(shots[0].tick)); // the press tick's start
		const size_t muzzle = p.count(host::PresentKind::OwnShot);
		CHECK(muzzle == shots.size());
		std::printf("E1: predicted %zu / server %zu shots; the first 31 equal field for field: %zu / 31; gaps of 4,500,000: %zu / 30; first at the press tick's start: %d; OwnShot events %zu\n",
				shots.size(), server_shots.size(), equal, gaps, int(shots[0].shot.time == sw::tick_start(shots[0].tick)), muzzle);
		// E5: server = predicted timed events (all of them), and the reload durations.
		std::vector<m2a_fake::ServerTimed> server_timed;
		for (const m2a_fake::ServerTimed &x : log.timed) {
			if (x.slot == s.slot) {
				server_timed.push_back(x);
			}
		}
		size_t same_timed = 0;
		for (size_t i = 0; i < std::min(timed.size(), server_timed.size()); ++i) {
			const sw::TimedWeaponEvent &a = timed[i].event, &b = server_timed[i].event;
			same_timed += a.time == b.time && a.event == b.event && a.weapon_slot == b.weapon_slot && timed[i].tick == server_timed[i].tick ? 1u : 0u;
		}
		CHECK(timed.size() == server_timed.size());
		CHECK(same_timed == timed.size());
		std::vector<sw::SimTime> starts, commits, finishes;
		for (const host::PredictedTimed &x : timed) {
			if (x.event.event == sw::weapon_events::kReloadStarted) {
				starts.push_back(x.event.time);
			} else if (x.event.event == sw::weapon_events::kReloadCommitted) {
				commits.push_back(x.event.time);
			} else if (x.event.event == sw::weapon_events::kReloadFinished) {
				finishes.push_back(x.event.time);
			}
		}
		REQUIRE(starts.size() >= 2);
		REQUIRE(finishes.size() >= 2);
		REQUIRE(commits.size() >= 2);
		CHECK(finishes[0] - starts[0] == 142'200'000); // empty: 2.37 s
		CHECK(finishes[1] - starts[1] == 108'000'000); // tactical: 1.8 s
		CHECK(commits[1] - starts[1] == 91'800'000);
		// The ammo after the two reloads, from the server's wire views: 30 after the empty one, 31 after the tactical.
		int32_t after_empty = -1, after_tactical = -1;
		for (const auto &[tick, wire] : log.wires) {
			const sw::SimTime end = sw::tick_end(tick);
			if (after_empty < 0 && end > finishes[0]) {
				after_empty = wire.slots[wire.active].magazine;
			}
			if (after_tactical < 0 && end > finishes[1]) {
				after_tactical = wire.slots[wire.active].magazine;
			}
		}
		CHECK(after_empty == 30);
		CHECK(after_tactical == 31);
		CHECK(s.weapon_reconciliations == 0);
		CHECK(s.reconciliations == 0);
		std::printf("E5: timed events predicted %zu / server %zu, equal %zu; empty reload %" PRId64 " -> %d rounds, tactical %" PRId64 " (commit +%" PRId64 ") -> %d rounds\n",
				timed.size(), server_timed.size(), same_timed, int64_t(finishes[0] - starts[0]), after_empty, int64_t(finishes[1] - starts[1]),
				int64_t(commits[1] - starts[1]), after_tactical);
	}

	TEST_CASE("host.gunplay.client: E12 lagcomp: 150/2 against a strafing target, control (server max_rewind 0) and stress (lat=150-165,loss=2)") {
		if (!timing_enabled("E12 lagcomp")) {
			return;
		}
		const LagcompResult gate = run_lagcomp("gate", "150/2", host::kDefaultMaxRewindTicks, 500);
		CHECK(gate.shots >= 500);
		CHECK(gate.client_hits >= 100);
		CHECK(double(gate.both) >= 0.99 * double(gate.client_hits));
		CHECK(double(gate.same_zone) >= 0.99 * double(gate.both));
		CHECK(gate.compared > 0);
		CHECK(gate.mismatched <= gate.stats.late_sample);
		const LagcompResult control = run_lagcomp("control", "150/2", 0, 500);
		CHECK(double(control.both) <= 0.5 * double(control.client_hits));
		const LagcompResult stress = run_lagcomp("stress", "lat=150-165,loss=2", host::kDefaultMaxRewindTicks, 500);
		std::printf("E12 summary: gate %.2f %% (zone %.2f %%), control %.2f %%, stress %.2f %% (reported); (b) gate %zu / %zu equal, %zu mismatched, late_sample %" PRIu64 "\n",
				gate.client_hits ? 100.0 * double(gate.both) / double(gate.client_hits) : 0.0, gate.both ? 100.0 * double(gate.same_zone) / double(gate.both) : 0.0,
				control.client_hits ? 100.0 * double(control.both) / double(control.client_hits) : 0.0,
				stress.client_hits ? 100.0 * double(stress.both) / double(stress.client_hits) : 0.0, gate.equal, gate.compared, gate.mismatched,
				gate.stats.late_sample);
	}

	TEST_CASE("host.gunplay.client: E15 loopback: scripted play without netsim, 0 weapon and 0 motor reconciliations") {
		if (!timing_enabled("E15 loopback")) {
			return;
		}
		const double seconds = env_seconds("OPENBF3_CL_LOOPBACK_S", 600.0);
		GunServer server;
		std::string err;
		GunServerConfig sc = server_config();
		sc.fake_damage_every_ticks = 600; // a virtual hit every 10 s: health rebases, counted apart
		sc.bots.push_back({ 2, false, m2a_fake::strafing_bot() });
		sc.bots.push_back({ 2, true, m2a_fake::firing_bot(13) });
		REQUIRE_MESSAGE(server.start(sc, err), err);
		Player p;
		REQUIRE(p.start(server.port(), floor_level(), "off", 31));
		REQUIRE(p.wait_spawned());
		// A 12 s script, repeated: walk and turn, fire bursts and taps, zoom, reload, switch to the pistol and back,
		// crouch, jump, sprint.
		p.run_for(seconds, [&](double p_t) {
			const double t = std::fmod(p_t, 12.0);
			uint32_t b = 0;
			p.input.move_forward = t < 6.0 ? 1.0f : (t < 8.0 ? -0.5f : 0.0f);
			p.input.move_right = std::sin(p_t * 0.7) > 0.3 ? 1.0f : 0.0f;
			p.input.yaw = float(std::fmod(p_t * 0.4, 6.283185307179586));
			p.input.pitch = float(0.2 * std::sin(p_t));
			if (t > 1.0 && t < 2.0) {
				b |= sim::buttons::kFire;
			}
			if (t > 2.5 && t < 3.5) {
				b |= sim::buttons::kZoom;
				if (std::fmod(t, 0.25) < 0.02) {
					++p.input.fire_presses;
				}
			}
			if (t > 4.0 && t < 4.1) {
				b |= sim::buttons::kReload;
			}
			if (t > 7.0 && t < 7.05) {
				b |= uint32_t(2) << sw::weapon_buttons::kSlotShift; // the pistol (slot 1)
			}
			if (t > 8.0 && t < 8.6) {
				b |= sim::buttons::kFire;
			}
			if (t > 9.5 && t < 9.55) {
				b |= uint32_t(1) << sw::weapon_buttons::kSlotShift; // back to the rifle
			}
			if (t > 10.0 && t < 10.05) {
				++p.input.jump_presses;
			}
			if (t > 10.5 && t < 11.0) {
				b |= sim::buttons::kSprint;
			}
			p.input.pose_request = host::PoseRequest::Set;
			p.input.pose = t > 5.0 && t < 6.0 ? sim::Pose::Crouch : sim::Pose::Stand;
			p.input.held_buttons = b;
		});
		const host::ClientStats s = p.client.stats();
		p.client.stop();
		server.stop();
		const m2a_fake::ServerLog log = server.take_log();
		print_client("E15 loopback", s);
		CHECK(s.weapon_reconciliations == 0);
		CHECK(s.reconciliations == 0);
		CHECK(s.weapon_ring_misses == 0);
		CHECK(s.step_precondition_failures == 0);
		CHECK(s.lane.gaps == 0);
		CHECK(s.events_overflow == 0);
		CHECK(s.predicted_shots > 100);
		CHECK(log.weapon_precondition_failures == 0);
		std::printf("E15 loopback: %.0f s, predicted shots %" PRIu64 ", server shots %zu, motor reconciliations %" PRIu64 ", weapon reconciliations %" PRIu64
					", health rebases %" PRIu64 " (virtual hits %" PRIu64 "), lane steps %" PRIu64 " replayed %" PRIu64 "\n",
				seconds, s.predicted_shots, log.shots.size(), s.reconciliations, s.weapon_reconciliations, s.health_rebases, log.fake_hits, s.lane.steps,
				s.lane.replayed_ticks);
	}

	TEST_CASE("host.gunplay.client: E15 two clients: the second joins after 30 s; both derive the same effect seed for every remote shot") {
		if (!timing_enabled("E15 two clients")) {
			return;
		}
		GunServer server;
		std::string err;
		GunServerConfig sc = server_config();
		sc.bots.push_back({ 2, true, m2a_fake::firing_bot(3) });
		sc.bots.push_back({ 2, true, m2a_fake::firing_bot(29) });
		REQUIRE_MESSAGE(server.start(sc, err), err);
		Player a, b;
		REQUIRE(a.start(server.port(), floor_level(), "off", 41));
		REQUIRE(a.wait_spawned());
		const auto t0 = std::chrono::steady_clock::now();
		auto script = [](Player &r_p, double p_t) {
			r_p.input.yaw = float(std::fmod(p_t * 0.3, 6.283185307179586));
			r_p.input.pitch = -0.2f;
			r_p.input.held_buttons = std::fmod(p_t, 3.0) < 0.5 ? sim::buttons::kFire : 0u;
		};
		a.run_for(30.0, [&](double p_t) { script(a, p_t); });
		std::set<uint8_t> fired_before;
		for (const host::PresentEvent &e : a.events) {
			if (e.kind == host::PresentKind::RemoteShot) {
				fired_before.insert(e.slot);
			}
		}
		for (int slot : server.bot_slots()) {
			CHECK_MESSAGE(fired_before.count(uint8_t(slot)) == 1, "bot slot " << slot << " fired before the join");
		}
		REQUIRE(b.start(server.port(), floor_level(), "off", 42));
		const auto join = std::chrono::steady_clock::now();
		for (;;) {
			const double t = tg::seconds_since(join);
			if (t > 30.0) {
				break;
			}
			script(a, tg::seconds_since(t0));
			script(b, t + 1.5);
			a.poll();
			b.poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
		// Both stop firing and keep draining together until the last shots have arrived.
		a.input.held_buttons = 0;
		b.input.held_buttons = 0;
		for (const auto tail = std::chrono::steady_clock::now(); tg::seconds_since(tail) < 1.5;) {
			a.poll();
			b.poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
		const host::ClientStats sa = a.client.stats(), sb = b.client.stats();
		a.client.stop();
		b.client.stop();
		server.stop();
		print_client("E15 client A", sa);
		print_client("E15 client B", sb);
		// Every shot B saw as remote: A saw it (as remote, or fired it itself) with the same effect seed. A's whole
		// stream: B's first ShotEvents also carry shots of the ticks just before its join.
		std::map<std::pair<uint8_t, uint32_t>, const host::PresentEvent *> at_a;
		for (const host::PresentEvent &e : a.events) {
			if (e.kind == host::PresentKind::RemoteShot || e.kind == host::PresentKind::OwnShot) {
				at_a[{ e.slot, e.id }] = &e;
			}
		}
		size_t remote_b = 0, matched = 0, same_seed = 0, near = 0;
		for (const host::PresentEvent &e : b.events) {
			if (e.kind != host::PresentKind::RemoteShot) {
				continue;
			}
			++remote_b;
			auto it = at_a.find({ e.slot, e.id });
			if (it == at_a.end()) {
				continue;
			}
			++matched;
			same_seed += it->second->seed == e.seed ? 1u : 0u;
			const float dx = it->second->pos[0] - e.pos[0], dy = it->second->pos[1] - e.pos[1], dz = it->second->pos[2] - e.pos[2];
			near += std::sqrt(dx * dx + dy * dy + dz * dz) < 0.01f ? 1u : 0u;
		}
		// The first shots after the join may predate B's first snapshot; count from B's first remote shot on.
		CHECK(remote_b > 100);
		CHECK(matched == remote_b);
		CHECK(same_seed == matched);
		std::printf("E15 two clients: B saw %zu remote shots after its join, A saw %zu of them, same effect seed %zu, origin within 1 cm %zu; bots fired before the join: %zu / %zu\n",
				remote_b, matched, same_seed, near, fired_before.size(), server.bot_slots().size());
	}
}
