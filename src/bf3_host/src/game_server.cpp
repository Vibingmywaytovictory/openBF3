#include "bf3/host/game_server.h"

#include "bf3/core/util/elapsed.h"
#include "bf3/host/firefight_brain.h"
#include "bf3/host/tdm_brain.h"
#include "bot_director.h"
#include "server_combat.h"
#include "server_combat_log.h"
#include "server_commands.h"
#include "server_dev.h"
#include "server_match.h"
#include "server_outbox.h"
#include "server_slots.h"
#include "server_snapshots.h"
#include "server_spawner.h"

#include "bf3/net/loopback_ping.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/spawn.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

namespace bf3::host {

namespace {

using bf3::core::ms_since;

class Walker final : public CommandSource {
public:
	explicit Walker(uint32_t p_phase) :
			phase_(p_phase) {}
	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) override {
		if (!started_) {
			started_ = true;
			base_yaw_ = p_state.yaw;
			start_ = p_tick;
		}
		sim::Command c;
		c.tick = p_tick;
		c.move_forward = sim::kAxisMax;
		const uint32_t leg = ((p_tick - start_) / (2u * sim::kSimHz) + phase_) & 3u;
		c.yaw = uint16_t(base_yaw_ + leg * 16384u);
		c.pose = sim::Pose::Stand;
		return c;
	}

private:
	uint32_t phase_ = 0;
	bool started_ = false;
	uint16_t base_yaw_ = 0;
	sim::Tick start_ = 0;
};

}

struct GameServer::Impl {
	GameServerConfig config;
	std::unique_ptr<sim::CollisionWorld> world;
	sim::SoldierMotor motor;
	uint64_t motor_hash = 0;
	SlotTable slots;
	sim::Tick tick = 0;
	Outbox outbox;
	CommandBuffers commands;
	MatchControl match;
	Combat combat;
	CombatLog combat_log{ slots, match, combat };
	SnapshotWriter snapshots{ slots, commands, combat, combat_log, motor, outbox };
	Spawner spawner{ slots, match, combat, combat_log, motor, outbox };
	DevCommands dev{ slots, spawner, combat, motor, outbox, [this](size_t p_slot) { reset_slot(slots[p_slot]); } };
	uint64_t bad_datagrams = 0;
	uint64_t level_mismatch_kicks = 0;
	uint64_t step_precondition_failures = 0;
	double last_step_ms = 0;

	GameServerTickTimes times;
	std::unique_ptr<BotDirector> bots;
	uint64_t motor_steps = 0;

	void reset_slot(ServerSlot &r_slot) {
		const size_t i = slots.index_of(r_slot);
		slots.clear(i);
		commands.reset(i);
		snapshots.reset(i);
		dev.reset(i);
	}

	void receive_commands(size_t p_slot, const net::CommandPacket &p_packet, uint64_t p_now_ns) {
		combat.ack(p_slot, p_packet);
		for (const sim::Command &c : commands.receive(p_slot, p_packet, p_now_ns, tick)) {
			combat.note_command(p_slot, c);
		}
	}

	void handle_hello(uint32_t p_peer, const net::Hello &p_hello) {
		if (slots.slot_of_peer(p_peer) >= 0) {
			return;
		}
		if (p_hello.protocol != net::kProtocolVersion) {
			net::Kick k;
			k.reason = "protocol " + std::to_string(p_hello.protocol) + " (server " + std::to_string(net::kProtocolVersion) + ")";
			outbox.send(p_peer, net::kChannelReliable, net::encode_kick(k));
			return;
		}
		const int at = slots.free_slot();
		if (at < 0) {
			net::Kick k;
			k.reason = "server full";
			outbox.send(p_peer, net::kChannelReliable, net::encode_kick(k));
			return;
		}
		ServerSlot &s = slots[size_t(at)];
		reset_slot(s);
		s.used = true;
		s.remote = true;
		s.peer = p_peer;
		s.team = match.on() ? int(match.connect(size_t(at), false, p_hello.name)) : 1;
		s.hello = true;
		net::MatchInfo m;
		m.level = config.level->level;
		m.mode = config.level->mode;
		m.snapshot_divisor = config.snapshot_divisor;
		m.slot = uint8_t(at);
		m.server_tick = tick;
		m.phase = net::MatchPhase::InRound;
		m.source_digest = config.level->source_digest;
		m.motor_hash = motor_hash;
		if (const std::optional<uint8_t> team = combat.client_joined(size_t(at), config.dev_commands, m)) {
			s.team = *team;
		}
		match.describe(m);
		outbox.send(p_peer, net::kChannelReliable, net::encode_match_info(m));
		const MatchBroadcast join = match.join_messages(tick);
		if (!join.state.empty()) {
			outbox.send(p_peer, net::kChannelReliable, join.state);
		}
		if (!join.players.empty()) {
			outbox.send(p_peer, net::kChannelReliable, join.players);
		}
	}

	void run_tick(std::vector<OutDatagram> &r_out) {
		const sim::Tick n = tick + 1u;
		const auto tick_start = std::chrono::steady_clock::now();
		times = GameServerTickTimes();
		times.tick = n;
		spawner.begin_tick();
		const uint64_t queries_before = world->query_count();
		const double paths_before = bots != nullptr ? bots->paths_ms_total() : 0.0;

		for (size_t i = 0; i < slots.size(); ++i) {
			ServerSlot &s = slots[i];
			if (!s.used || !s.remote) {
				continue;
			}
			const std::optional<sim::Command> taken = commands.take(i, n);
			if (!s.spawned) {
				continue;
			}
			s.last = commands.apply(i, taken, s.last, n);
		}
		times.commands_ms = ms_since(tick_start);

		const auto sources_start = std::chrono::steady_clock::now();
		for (ServerSlot &s : slots) {
			if (!s.used || s.remote || !s.spawned) {
				continue;
			}
			sim::Command c;
			if (s.dummy) {
				c = sim::idle_command(s.state.tick + 1u, s.state.yaw, s.state.pitch, s.state.target_pose);
			} else if (s.source) {
				c = s.source->command(n, s.state);
			}
			c.tick = n;
			c.flags = uint8_t(c.flags | sim::command_flags::kLocalSource);
			s.last = c;
			commands.count_local(slots.index_of(s));
		}
		times.sources_ms = ms_since(sources_start);
		times.paths_ms = bots != nullptr ? bots->paths_ms_total() - paths_before : 0.0;

		const auto step_start = std::chrono::steady_clock::now();
		for (ServerSlot &s : slots) {
			if (!s.used || !s.spawned) {
				continue;
			}
			if (s.last.tick != s.state.tick + 1u) {
				++step_precondition_failures;
			}
			const bool was_alive = soldier_alive(s.state);
			const sim::SoldierState sv_before = s.state;
			if (!s.dummy) {
				s.last = match.restrict(s.last);
			}
			motor.step(s.state, s.last);
			++times.motor_steps;
			if (was_alive && !s.dummy) {
				combat.step_soldier(slots.index_of(s), s.last, sv_before, s.state);
			}
			if (was_alive && !soldier_alive(s.state) && !s.dummy) {
				const std::optional<uint32_t> respawn = match.policy().respawn_tick(uint8_t(slots.index_of(s)), n);
				s.want_spawn = respawn.has_value();
				s.respawn_at = respawn.value_or(0);
			}
		}
		last_step_ms = ms_since(step_start);
		times.step_ms = last_step_ms;
		motor_steps += times.motor_steps;
		const auto rules_start = std::chrono::steady_clock::now();

		sv_rules(n);
		for (size_t i = 0; i < slots.size(); ++i) {
			ServerSlot &s = slots[i];
			if (s.used && !s.dummy && s.want_spawn && (!s.spawned || n >= s.respawn_at)) {
				spawner.spawn(i, SIZE_MAX, n);
			}
		}
		dev.apply_debug(n);
		dev.run_sweep(n);
		times.rules_ms = ms_since(rules_start);
		times.spawns = spawner.tick_spawns();
		tick = n;

		for (size_t i = 0; i < slots.size(); ++i) {
			ServerSlot &s = slots[i];
			if (!s.used || !s.spawned) {
				continue;
			}
			s.own_events = uint16_t(s.own_events | s.state.events);
			dev.trace(i, tick);
		}
		if (config.snapshot_divisor <= 1 || n % config.snapshot_divisor == 0) {
			snapshots.send(n, net::monotonic_ns(), times);
		}
		send_rules(n);
		outbox.drain(r_out);
		times.collision_queries = world->query_count() - queries_before;
		times.tick_ms = ms_since(tick_start);
	}

	int add_local(std::unique_ptr<CommandSource> p_source, int p_team, bool p_bot) {
		const int at = slots.free_slot();
		if (at < 0 || p_source == nullptr) {
			return -1;
		}
		ServerSlot &s = slots[size_t(at)];
		reset_slot(s);
		s.used = true;
		s.team = p_team;
		s.bot = p_bot;
		s.source = std::move(p_source);
		if (match.on()) {
			s.team = int(match.connect(size_t(at), true, ""));
			if (s.team == 0) {
				match.leave(size_t(at));
				reset_slot(s);
				return -1;
			}
		}
		if (!spawner.spawn(size_t(at), SIZE_MAX, tick)) {
			reset_slot(s);
			return -1;
		}
		return at;
	}

	struct Sv;
	std::unique_ptr<Sv> sv;
	core::Status sv_init();
	const ServerView &server_view() const;
	void sv_rules(sim::Tick p_tick);

	void send_rules(sim::Tick p_tick) {
		const MatchBroadcast b = match.take_broadcast(p_tick);
		if (b.state.empty() && b.players.empty()) {
			return;
		}
		for (const ServerSlot &s : slots) {
			if (!s.used || !s.remote || !s.hello) {
				continue;
			}
			if (!b.state.empty()) {
				outbox.send(s.peer, net::kChannelReliable, b.state);
			}
			if (!b.players.empty()) {
				outbox.send(s.peer, net::kChannelReliable, b.players);
			}
		}
	}

	void receive_kit_select(int p_slot, std::span<const uint8_t> p_datagram) {
		core::Result<net::KitSelect> k = net::decode_kit_select(p_datagram);
		if (!k || p_slot < 0 || size_t(p_slot) >= slots.size()) {
			++bad_datagrams;
			combat.refuse_kit_select();
			return;
		}
		combat.select_kit(size_t(p_slot), *k, config.dev_commands);
	}

	void receive_spawn_request(int p_slot, std::span<const uint8_t> p_datagram) {
		core::Result<net::SpawnRequest> r = net::decode_spawn_request(p_datagram);
		if (!r || p_slot < 0 || size_t(p_slot) >= slots.size() || !match.on()) {
			++bad_datagrams;
			return;
		}
		match.request_spawn(size_t(p_slot), r->kit, r->spawn_choice, tick + 1u);
	}
};

core::Status GameServer::init(const GameServerConfig &p_config) {
	Impl &m = *impl_;
	if (p_config.level == nullptr || p_config.level->static_world == nullptr) {
		return core::make_error("GameServer: no level");
	}
	if (p_config.max_slots == 0 || p_config.max_slots > net::kMaxSlots) {
		return core::make_error("GameServer: max_slots must be 1.." + std::to_string(net::kMaxSlots));
	}
	if (p_config.snapshot_divisor == 0) {
		return core::make_error("GameServer: snapshot_divisor 0");
	}
	m.config = p_config;
	core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(p_config.level->static_world);
	if (!world) {
		return core::error_at("GameServer", world.error());
	}
	m.world = std::move(*world);
	BF3_TRY(m.motor.init(*m.world, p_config.level->motor));
	m.motor_hash = sim::motor_params_hash(p_config.level->motor);
	m.slots.resize(p_config.max_slots);
	m.commands.configure(p_config.max_slots, p_config.jitter_buffer_ticks);
	m.snapshots.configure(p_config.max_slots);
	m.dev.configure(m.config, *m.world, p_config.max_slots);
	for (ServerSlot &s : m.slots) {
		m.reset_slot(s);
	}
	BF3_TRY(m.sv_init());
	for (uint32_t i = 0; i < p_config.walkers; ++i) {
		if (add_local_soldier(std::make_unique<Walker>(i), 2) < 0) {
			return core::make_error("GameServer: no slot for walker " + std::to_string(i));
		}
	}

	if (p_config.bots != nullptr && p_config.bots->count > 0) {
		core::Result<std::unique_ptr<BotDirector>> director = BotDirector::create(p_config.bots, p_config.seed);
		if (!director) {
			return core::error_at("GameServer", director.error());
		}
		m.bots = std::move(*director);
		for (uint32_t i = 0; i < m.bots->count(); ++i) {
			if (m.add_local(m.bots->source(i), 1 + int(i % 2u), true) < 0) {
				return core::make_error("GameServer: no slot for bot " + std::to_string(i));
			}
		}
	}
	BF3_TRY(add_firefight_bots(*this, p_config));
	if (p_config.tdm_bots != nullptr) {
		BF3_TRY(add_tdm_bots(*this, p_config));
	}
	return core::Status::success();
}

void GameServer::on_connect(uint32_t) {

}

void GameServer::on_disconnect(uint32_t p_peer) {
	Impl &m = *impl_;
	const int at = m.slots.slot_of_peer(p_peer);
	if (at >= 0) {
		m.dev.client_left(size_t(at));
		m.combat.client_left(size_t(at));
		m.match.leave(size_t(at));
		m.reset_slot(m.slots[size_t(at)]);
	}
}

void GameServer::on_receive(uint32_t p_peer, uint8_t p_channel, std::span<const uint8_t> p_datagram) {
	Impl &m = *impl_;
	const std::optional<net::MessageType> type = net::peek_type(p_datagram);
	if (!type) {
		++m.bad_datagrams;
		return;
	}
	const int at = m.slots.slot_of_peer(p_peer);
	switch (*type) {
		case net::MessageType::Hello: {
			core::Result<net::Hello> h = net::decode_hello(p_datagram);
			if (!h || p_channel != net::kChannelReliable) {
				++m.bad_datagrams;
				return;
			}
			m.handle_hello(p_peer, *h);
			return;
		}
		case net::MessageType::Ready: {
			core::Result<net::Ready> r = net::decode_ready(p_datagram);
			if (!r || at < 0) {
				++m.bad_datagrams;
				return;
			}
			if (r->source_digest && m.config.level->source_digest && *r->source_digest != *m.config.level->source_digest) {

				net::Kick k;
				k.reason = "level data differs from the server's (source digest)";
				m.outbox.send(p_peer, net::kChannelReliable, net::encode_kick(k));
				++m.level_mismatch_kicks;
				on_disconnect(p_peer);
				return;
			}
			ServerSlot &s = m.slots[size_t(at)];
			if (!s.spawned && !m.match.waits_for_deploy(size_t(at))) {
				s.want_spawn = true;
			}
			return;
		}
		case net::MessageType::CommandPacket: {
			core::Result<net::CommandPacket> c = net::decode_command_packet(p_datagram);
			if (!c || at < 0) {
				++m.bad_datagrams;
				return;
			}
			m.receive_commands(size_t(at), *c, net::monotonic_ns());
			return;
		}
		case net::MessageType::Loading: {
			if (!net::decode_loading(p_datagram) || at < 0) {
				++m.bad_datagrams;
			}
			return;
		}
		case net::MessageType::Bye:
			if (at >= 0) {
				on_disconnect(p_peer);
			}
			return;
		case net::MessageType::Debug: {
			core::Result<net::DebugRequest> d = net::decode_debug(p_datagram);
			if (!d || at < 0) {
				++m.bad_datagrams;
				return;
			}
			m.dev.handle_debug(size_t(at), *d);
			return;
		}
		case net::MessageType::KitSelect:
			m.receive_kit_select(at, p_datagram);
			return;
		case net::MessageType::SpawnRequest:
			m.receive_spawn_request(at, p_datagram);
			return;
		default:
			++m.bad_datagrams;
			return;
	}
}

void GameServer::tick(std::vector<OutDatagram> &r_out) {
	impl_->run_tick(r_out);
}

sim::Tick GameServer::current_tick() const {
	return impl_->tick;
}

int GameServer::add_local_soldier(std::unique_ptr<CommandSource> p_source, int p_team) {
	return impl_->add_local(std::move(p_source), p_team, false);
}

GameServerStats GameServer::stats() const {
	const Impl &m = *impl_;
	GameServerStats g;
	g.tick = m.tick;
	g.bad_datagrams = m.bad_datagrams;
	g.spawn_failures = m.spawner.failures();
	g.level_mismatch_kicks = m.level_mismatch_kicks;
	g.rules_step_failures = m.match.step_failures();
	g.step_precondition_failures = m.step_precondition_failures;
	g.last_step_ms = m.last_step_ms;
	m.snapshots.stats(g);

	g.last_tick_ms = m.times.tick_ms;
	g.last_commands_ms = m.times.commands_ms;
	g.last_sources_ms = m.times.sources_ms;
	g.last_rules_ms = m.times.rules_ms;
	g.motor_steps = m.motor_steps;
	g.collision_queries = m.world != nullptr ? m.world->query_count() : 0;
	if (m.bots != nullptr) {
		g.bots = m.bots->stats();
	}
	g.slots.resize(m.slots.size());
	for (size_t i = 0; i < m.slots.size(); ++i) {
		const ServerSlot &s = m.slots[i];
		SlotStats &o = g.slots[i];
		m.commands.stats(i, m.tick, o);
		o.used = s.used;
		o.remote = s.remote;
		o.peer = s.peer;
		o.life = s.state.life;
		o.alive = s.spawned && soldier_alive(s.state);
		o.state = s.state;
		g.clients += (s.used && s.remote) ? 1u : 0u;
		g.soldiers += o.alive ? 1u : 0u;
	}
	m.combat.stats(g);
	return g;
}

const GameServerTickTimes &GameServer::last_tick_times() const {
	return impl_->times;
}

const sim::SoldierState *GameServer::soldier(uint32_t p_slot) const {
	const Impl &m = *impl_;
	if (p_slot >= m.slots.size() || !m.slots[p_slot].used || !m.slots[p_slot].spawned) {
		return nullptr;
	}
	return &m.slots[p_slot].state;
}

}

#include "bf3/host/target_bots.h"

#include <cstdlib>

namespace bf3::host {

struct GameServer::Impl::Sv {
	struct View final : ServerView {
		Impl *m = nullptr;
		mutable std::vector<sim::SoldierState> cache;
		mutable sim::Tick cache_tick = 0;
		mutable bool cached = false;
		std::span<const sim::SoldierState> states() const override {
			if (!cached || cache_tick != m->tick) {
				cache.assign(m->slots.size(), sim::SoldierState());
				for (size_t i = 0; i < m->slots.size(); ++i) {
					if (m->slots[i].used && m->slots[i].spawned) {
						cache[i] = m->slots[i].state;
					}
				}
				cache_tick = m->tick;
				cached = true;
			}
			return cache;
		}
		uint8_t team(uint8_t p_slot) const override {
			return p_slot < m->slots.size() && m->slots[p_slot].used ? uint8_t(std::clamp(m->slots[p_slot].team, 0, 255)) : uint8_t(0);
		}
		bool alive(uint8_t p_slot) const override {
			return p_slot < m->slots.size() && m->slots[p_slot].used && m->slots[p_slot].spawned && soldier_alive(m->slots[p_slot].state);
		}
		const sim::weapons::SoldierWeaponsState &weapons(uint8_t p_slot) const override {
			static const sim::weapons::SoldierWeaponsState s_empty;
			return m->combat.on() ? m->combat.gunplay()->weapons(p_slot) : s_empty;
		}
		const sim::CollisionWorld &world() const override { return *m->world; }
		sim::Tick tick() const override { return m->tick + 1u; }

		std::span<const DownedSoldier> downed() const override {
			return m->match.on() ? m->match.rules()->downed() : ServerView::downed();
		}
		net::MatchPhase phase() const override {
			return m->match.on() ? m->match.rules()->phase() : ServerView::phase();
		}
		bool input_restricted() const override {
			return m->match.on() ? m->match.rules()->input_restricted() : ServerView::input_restricted();
		}
		uint8_t kit(uint8_t p_slot) const override {
			if (!m->match.on()) {
				return ServerView::kit(p_slot);
			}
			const uint8_t k = m->match.rules()->kit_of(p_slot);
			return k != kKeepKit ? k : (m->combat.on() ? m->combat.gunplay()->kit(p_slot) : uint8_t(0));
		}
	};
	View view;
};

GameServer::GameServer() :
		impl_(std::make_unique<Impl>()) {}

GameServer::~GameServer() = default;

const ServerView &GameServer::Impl::server_view() const {
	return sv->view;
}

core::Status GameServer::Impl::sv_init() {
	sv = std::make_unique<Sv>();
	Sv &v = *sv;
	v.view.m = this;
	spawner.configure(config, *world, v.view);
	match.configure(config, slots.size());
	combat.configure(slots.size(), *config.level->static_world);
	const bool tdm = config.rules.mode == MatchRulesConfig::Mode::Tdm;
	if (tdm && (config.level->logic == nullptr || config.level->gunplay == nullptr)) {
		return core::make_error("GameServer: TDM rules need the level's logic graph and gunplay tables (SharedLoadOptions::tdm, ::gunplay)");
	}
	if (config.level->gunplay == nullptr) {
		return core::Status::success();
	}
	if (tdm) {
		config.gunplay.defib_def = config.level->gunplay->kits.defib_def;
	}
	const GunplayConfig &g = config.gunplay;
	if (!config.dev_commands && (g.exact_aim || g.infinite_magazine || g.target_bots > 0 || g.player_team != 0 || g.observer != nullptr)) {
		return core::make_error("GameServer: exact_aim, infinite_magazine, target bots, a player team and the observer are dev options (dev_commands)");
	}
	if (g.target_bots > slots.size()) {
		return core::make_error("GameServer: " + std::to_string(g.target_bots) + " target bots for " + std::to_string(slots.size()) + " slots");
	}
	BF3_TRY(combat.create(*config.level, g, config.seed, *world));
	if (tdm) {
		BF3_TRY(match.create_rules(config, combat.gunplay()->match_seed()));
	}
	for (uint32_t i = 0; i < g.target_bots; ++i) {
		auto source = std::make_unique<TargetBotSource>(TargetBotConfig{});
		TargetBotSource *raw = source.get();
		const int team = g.target_team != 0 ? int(std::min<uint8_t>(g.target_team, net::kMaxTeam)) : (i % 2u == 0 ? 2 : 1);
		const int at = add_local(std::move(source), team, false);
		if (at < 0) {
			return core::make_error("GameServer: no slot for target bot " + std::to_string(i));
		}
		combat.set_target(size_t(at), raw);
	}
	return core::Status::success();
}

void GameServer::Impl::sv_rules(sim::Tick p_tick) {
	if (sv == nullptr) {
		return;
	}
	Sv &v = *sv;
	if (combat.on()) {
		combat_log.open(p_tick);
		combat.resolve(p_tick, slots, combat_log);
		match.step(p_tick, v.view, *combat.gunplay(), combat.tick_states());
		combat.restore(slots);
	}

	spawner.place_queued(p_tick);
	const std::vector<RulesOrder> orders = match.take_orders();
	if (!orders.empty()) {
		spawner.apply_orders(p_tick, orders);
	}
}

core::Status GameServer::place_soldier(uint32_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw, sim::Pose p_pose) {
	return impl_->spawner.queue_place(p_slot, p_position, p_yaw, p_pose);
}

const ServerView &GameServer::view() const {
	if (impl_->sv == nullptr) {

		std::fprintf(stderr, "GameServer::view() called before init\n");
		std::abort();
	}
	return impl_->sv->view;
}

const Gunplay *GameServer::gunplay() const {
	return impl_->combat.gunplay();
}

const TdmRules *GameServer::rules() const {
	return impl_->match.rules();
}

}

namespace bf3::host {

int GameServer::add_local_bot(std::unique_ptr<CommandSource> p_source, int p_team) {
	return impl_->add_local(std::move(p_source), p_team, true);
}

}
