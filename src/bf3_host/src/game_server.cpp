#include "bf3/host/game_server.h"

#include "bf3/core/util/elapsed.h"
#include "bf3/host/conquest_wire.h"
#include "bf3/host/firefight_brain.h"
#include "bf3/host/load_bots.h"
#include "bf3/host/target_bots.h"
#include "bf3/host/tdm_brain.h"
#include "bot_director.h"
#include "server_combat.h"
#include "server_combat_log.h"
#include "server_commands.h"
#include "server_conquest.h"
#include "server_dev.h"
#include "server_gadgets.h"
#include "server_handshake.h"
#include "server_logic_sync.h"
#include "server_match.h"
#include "server_outbox.h"
#include "server_seats.h"
#include "server_slot_view.h"
#include "server_slots.h"
#include "server_snapshots.h"
#include "server_soldiers.h"
#include "server_spawner.h"
#include "server_spotting.h"
#include "vehicles/server_vehicles.h"

#include "bf3/net/loopback_ping.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/spawn.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace bf3::host {

namespace {

using bf3::core::ms_since;

}

struct GameServer::Impl {
	GameServerConfig config;
	std::unique_ptr<sim::CollisionWorld> world;
	sim::SoldierMotor motor;
	uint64_t motor_hash = 0;
	SlotTable slots;
	Soldiers soldiers;
	sim::Tick tick = 0;
	Outbox outbox;
	CommandBuffers commands;
	MatchControl match;
	Combat combat;
	SlotView view{ slots, soldiers, match, combat, tick };
	ServerLogicSync logic{ slots, match, outbox };
	Spawner spawner{ slots, soldiers, match, combat, motor, outbox, logic };
	CombatLog combat_log{ spawner, match, combat };
	SnapshotWriter snapshots{ slots, soldiers, commands, combat, combat_log, motor, outbox, match };
	DeployableSync deployables;
	ServerSpotting spotting;
	ConquestSync conquest_sync;
	ServerVehicles vehicles;
	std::vector<uint8_t> alive_mask;
	DevCommands dev{ slots, soldiers, spawner, combat, match, motor, outbox,
		[this](size_t p_slot) { reset_slot(p_slot); } };
	ServerHandshake handshake;
	SeatLoads loads;
	std::unordered_map<uint32_t, NetTraffic> received;
	uint64_t bad_datagrams = 0;
	uint64_t step_precondition_failures = 0;
	float fall_out_y = -INFINITY;
	double last_step_ms = 0;

	GameServerTickTimes times;
	std::unique_ptr<BotDirector> bots;
	uint64_t motor_steps = 0;
	bool changing = false;
	net::LevelChange level_change;
	net::NextLevel next_level;
	uint32_t carried = 0;
	uint64_t held_joins = 0;
	uint64_t held_readies = 0;

	void reset_slot(size_t i) {
		slots.clear(i);
		soldiers.clear(i);
		spawner.reset(i);
		logic.reset(i);
		commands.reset(i);
		snapshots.reset(i);
		deployables.reset(i);
		dev.reset(i);
		loads.reset(i);
		spotting.reset(i);
		vehicles.leave(i);
		vehicles.forget_viewer(i);
	}

	void drop_client(size_t p_slot) {
		dev.client_left(p_slot);
		combat.client_left(p_slot);
		match.leave(p_slot);
		reset_slot(p_slot);
	}

	void kick_stalled_loads(sim::Tick p_tick) {
		if (changing) {
			return;
		}
		for (const SeatLoads::Due &d : loads.due(p_tick)) {
			const Seat &s = slots[d.slot];
			if (!s.used || !s.remote) {
				continue;
			}
			outbox.send(s.peer, net::kChannelReliable, net::encode_kick(handshake.kick(d.reason, d.detail)));
			drop_client(d.slot);
		}
	}

	void receive_commands(size_t p_slot, const net::CommandPacket &p_packet, uint64_t p_now_ns) {
		combat.ack(p_slot, p_packet);
		snapshots.ack(p_slot, p_packet);
		for (const sim::Command &c : commands.receive(p_slot, p_packet, p_now_ns, tick)) {
			combat.note_command(p_slot, c);
		}
	}

	void handle_hello(uint32_t p_peer, const net::Hello &p_hello) {
		if (slots.slot_of_peer(p_peer) >= 0) {
			return;
		}
		if (const std::optional<net::Kick> refused = handshake.hello(p_hello)) {
			outbox.send(p_peer, net::kChannelReliable, net::encode_kick(*refused));
			return;
		}
		const int at = slots.free_slot();
		if (at < 0) {
			outbox.send(p_peer, net::kChannelReliable, net::encode_kick(handshake.full()));
			return;
		}
		if (changing) {
			hold_join(size_t(at), p_peer, p_hello.name);
			return;
		}
		join(size_t(at), p_peer, p_hello.name, nullptr, -1);
	}

	void join(size_t p_slot, uint32_t p_peer, std::string_view p_name, const CarriedSeat *p_carry, int p_squad_mate) {
		reset_slot(p_slot);
		int team = 1;
		if (p_carry != nullptr) {
			team = match.on() ? int(match.carry(p_slot, p_name, p_carry->team, p_squad_mate)) : int(p_carry->team);
		} else if (match.on()) {
			team = int(match.connect(p_slot, false, p_name));
		}
		net::MatchInfo m;
		m.level = config.level->level;
		m.mode = config.level->mode;
		m.snapshot_divisor = config.snapshot_divisor;
		m.slot = uint8_t(p_slot);
		m.server_tick = tick;
		m.phase = net::MatchPhase::InRound;
		m.source_digest = config.level->source_digest;
		m.motor_hash = motor_hash;
		m.round_id = config.round_id;
		if (const std::optional<uint8_t> chosen = combat.client_joined(p_slot, config.dev_commands, m);
				chosen && p_carry == nullptr) {
			team = *chosen;
		}
		if (p_carry != nullptr) {
			carry_gear(p_slot, *p_carry);
		}
		slots.admit_client(p_slot, p_peer, team);
		slots.name(p_slot, p_name);
		loads.begin(p_slot, tick);
		match.describe(m);
		handshake.describe(p_slot, m);
		outbox.send(p_peer, net::kChannelReliable, net::encode_match_info(m));
		const MatchBroadcast joined = match.join_messages(tick, uint8_t(std::clamp(team, 0, int(net::kMaxTeam))));
		if (!joined.state.empty()) {
			outbox.send(p_peer, net::kChannelReliable, joined.state);
		}
		if (!joined.players.empty()) {
			outbox.send(p_peer, net::kChannelReliable, joined.players);
		}
		for (const SquadOrderMessage &o : joined.orders) {
			outbox.send(p_peer, net::kChannelReliable, o.bytes);
		}
		if (combat.on()) {
			outbox.send(p_peer, net::kChannelReliable, net::encode_spot_states(spotting.full(tick)));
		}
	}

	void carry_gear(size_t p_slot, const CarriedSeat &p_carry) {
		combat.set_kit(p_slot, p_carry.kit < kKitCount ? p_carry.kit : uint8_t(0));
		for (uint8_t t = 0; t < kKitTeams; ++t) {
			for (uint8_t k = 0; k < kKitCount; ++k) {
				if (const std::optional<LoadoutChoice> &c = p_carry.loadouts[t][k]) {
					net::LoadoutRequest r;
					r.kit = k;
					r.items = c->items;
					r.accessories = c->accessories;
					r.specialization = c->specialization;
					combat.select_loadout(p_slot, uint8_t(t + 1u), r);
				}
			}
		}
	}

	void hold_join(size_t p_slot, uint32_t p_peer, std::string_view p_name) {
		reset_slot(p_slot);
		net::MatchInfo m;
		m.level = next_level.level;
		m.mode = next_level.mode;
		m.snapshot_divisor = config.snapshot_divisor;
		m.slot = uint8_t(p_slot);
		m.server_tick = tick;
		m.phase = net::MatchPhase::LevelChange;
		m.source_digest = next_level.source_digest;
		m.logic_digest = next_level.logic_digest;
		m.round_id = level_change.round_id;
		slots.admit_client(p_slot, p_peer, 0);
		slots.name(p_slot, p_name);
		outbox.send(p_peer, net::kChannelReliable, net::encode_match_info(m));
		outbox.send(p_peer, net::kChannelReliable, net::encode_level_change(level_change));
		++held_joins;
	}

	void broadcast(const std::vector<uint8_t> &p_bytes) {
		for (size_t i = 0; i < slots.size(); ++i) {
			if (slots[i].used && slots[i].remote) {
				outbox.send(slots[i].peer, net::kChannelReliable, p_bytes);
			}
		}
	}

	void run_tick(std::vector<OutDatagram> &r_out) {
		const sim::Tick n = tick + 1u;
		const auto tick_start = std::chrono::steady_clock::now();
		times = GameServerTickTimes();
		times.tick = n;
		spawner.begin_tick();
		kick_stalled_loads(n);
		const uint64_t queries_before = world->query_count();
		const double paths_before = bots != nullptr ? bots->paths_ms_total() : 0.0;

		for (size_t i = 0; i < slots.size(); ++i) {
			if (!slots[i].used || !slots[i].remote) {
				continue;
			}
			const std::optional<sim::Command> taken = commands.take(i, n);
			if (!soldiers[i].spawned) {
				continue;
			}
			soldiers.command(i, commands.apply(i, taken, soldiers[i].last, n));
		}
		times.commands_ms = ms_since(tick_start);

		const auto sources_start = std::chrono::steady_clock::now();
		for (size_t i = 0; i < slots.size(); ++i) {
			const Seat &s = slots[i];
			const Soldier &body = soldiers[i];
			if (!s.used || s.remote || !body.spawned) {
				continue;
			}
			sim::Command c = s.dummy ? sim::idle_command(body.state.tick + 1u, body.state.yaw, body.state.pitch, body.state.target_pose)
									 : slots.source_command(i, n, body.state);
			c.tick = n;
			c.flags = uint8_t(c.flags | sim::command_flags::kLocalSource);
			soldiers.command(i, c);
			commands.count_local(i);
		}
		times.sources_ms = ms_since(sources_start);
		times.paths_ms = bots != nullptr ? bots->paths_ms_total() - paths_before : 0.0;

		const auto step_start = std::chrono::steady_clock::now();
		for (size_t i = 0; i < slots.size(); ++i) {
			const Seat &s = slots[i];
			const Soldier &body = soldiers[i];
			if (!s.used || !body.spawned) {
				continue;
			}
			if (body.last.tick != body.state.tick + 1u) {
				++step_precondition_failures;
			}
			const bool was_alive = soldier_alive(body.state);
			const sim::SoldierState before = body.state;
			if (!s.dummy) {
				soldiers.command(i, combat.clamp_pitch(i, match.restrict(body.last), body.state.pose));
			}
			if (was_alive && !s.dummy && (match.take_suicide(i) || body.state.position[1] < fall_out_y)) {
				soldiers.kill(i);
			}
			if (was_alive && !s.dummy) {
				if (const std::optional<sim::Vec3> out = vehicles.use(i, body.last, body.state)) {
					sim::SoldierState moved = body.state;
					moved.position = *out;
					moved.velocity = {};
					soldiers.teleport(i, moved);
				}
			}
			const bool seated = was_alive && vehicles.seated(i);
			if (seated) {
				vehicles.drive(i, body.last);
				soldiers.seat(i, body.last, body.state.position);
			} else {
				soldiers.step(i, motor, combat.motor_modifiers(i, motor.params()));
			}
			++times.motor_steps;
			if (was_alive && !s.dummy && !seated) {
				combat.step_soldier(i, body.last, before, body.state);
			}
			if (was_alive && !soldier_alive(body.state) && !s.dummy) {
				spawner.died(i, n);
			}
		}
		step_vehicles(n);
		last_step_ms = ms_since(step_start);
		times.step_ms = last_step_ms;
		motor_steps += times.motor_steps;
		const auto rules_start = std::chrono::steady_clock::now();

		step_rules(n);
		vehicles.spawn_due(n, conquest());
		spawner.spawn_due(n);
		dev.apply_debug(n);
		dev.run_sweep(n);
		times.rules_ms = ms_since(rules_start);
		times.spawns = spawner.tick_spawns();
		tick = n;

		for (size_t i = 0; i < slots.size(); ++i) {
			if (!slots[i].used || !soldiers[i].spawned) {
				continue;
			}
			soldiers.collect_events(i);
			dev.trace(i, tick);
		}
		if (config.snapshot_divisor <= 1 || n % config.snapshot_divisor == 0) {
			snapshots.send(n, net::monotonic_ns(), times);
			send_vehicles(n);
		}
		send_rules(n);
		send_deployables(n);
		send_spotting(n);
		logic.send(n);
		outbox.drain(r_out);
		times.collision_queries = world->query_count() - queries_before;
		times.tick_ms = ms_since(tick_start);
	}

	const sim::logic::ConquestNatives *conquest() const {
		const TdmRules *rules = match.rules();
		return rules != nullptr ? rules->natives().conquest() : nullptr;
	}

	void step_vehicles(sim::Tick p_tick) {
		if (!vehicles.on()) {
			return;
		}
		(void)vehicles.step(p_tick, combat.on() ? combat.gunplay()->match_seed() : 0u);
		std::vector<sim::Vec3> alive_at;
		for (size_t i = 0; i < slots.size(); ++i) {
			if (slots[i].used && soldiers.alive(i)) {
				alive_at.push_back(soldiers[i].state.position);
			}
		}
		vehicles.step_health(p_tick, alive_at);
		for (size_t i = 0; i < slots.size(); ++i) {
			if (!slots[i].used || !soldiers.alive(i)) {
				vehicles.leave(i);
				continue;
			}
			if (const std::optional<sim::Vec3> at = vehicles.seat_position(i)) {
				soldiers.seat(i, soldiers[i].last, *at);
			}
		}
	}

	void send_vehicles(sim::Tick p_tick) {
		if (!vehicles.on()) {
			return;
		}
		for (size_t i = 0; i < slots.size(); ++i) {
			if (!slots[i].used || !slots[i].remote) {
				continue;
			}
			GhostViewer viewer;
			viewer.slot = uint8_t(i);
			viewer.team = uint8_t(std::clamp(slots[i].team, 0, 255));
			viewer.spawned = soldiers[i].spawned;
			viewer.position = soldiers[i].state.position;
			viewer.eye = soldiers[i].state.position;
			for (const net::VehicleStates &m : vehicles.wire_for(i, viewer, p_tick)) {
				outbox.send(slots[i].peer, net::kChannelUnreliable, net::encode_vehicle_states(m));
			}
		}
	}

	int add_local(std::unique_ptr<CommandSource> p_source, int p_team, bool p_bot) {
		const int at = slots.free_slot();
		if (at < 0 || p_source == nullptr) {
			return -1;
		}
		reset_slot(size_t(at));
		int team = p_team;
		if (match.on()) {
			team = int(match.connect(size_t(at), true, ""));
			if (team == 0) {
				match.leave(size_t(at));
				return -1;
			}
		}
		CommandSource &source = *p_source;
		slots.admit_local(size_t(at), std::move(p_source), team, p_bot);
		source.admitted(uint8_t(at));
		if (!spawner.spawn(size_t(at), SIZE_MAX, tick)) {
			reset_slot(size_t(at));
			return -1;
		}
		return at;
	}

	core::Status init_gameplay();

	core::Status admit_carried() {
		std::vector<CarriedSeat> seats = config.carried_seats;
		std::sort(
				seats.begin(), seats.end(), [](const CarriedSeat &a, const CarriedSeat &b) { return a.slot < b.slot; });
		for (size_t i = 0; i < seats.size(); ++i) {
			const CarriedSeat &c = seats[i];
			if (c.slot >= slots.size() || slots[c.slot].used || slots.slot_of_peer(c.peer) >= 0) {
				return core::make_error(
						"GameServer: carried seat " + std::to_string(c.slot) + " is taken or out of range");
			}
			int mate = -1;
			for (size_t j = 0; j < i && c.squad != 0; ++j) {
				if (seats[j].team == c.team && seats[j].squad == c.squad) {
					mate = int(seats[j].slot);
					break;
				}
			}
			join(c.slot, c.peer, c.name, &c, mate);
			++carried;
		}
		return core::Status::success();
	}

	GameServerConfig bot_fill_config() const {
		if (carried == 0 || config.tdm_bots == nullptr || !match.on()) {
			return config;
		}
		GameServerConfig c = config;
		auto bots = std::make_shared<TdmBotConfig>(*config.tdm_bots);
		const uint32_t room = config.rules.max_players > carried ? uint32_t(config.rules.max_players) - carried : 0u;
		bots->count = std::min(bots->count, room);
		c.tdm_bots = bots;
		return c;
	}
	void step_rules(sim::Tick p_tick);

	void send_rules(sim::Tick p_tick) {
		if (match.on()) {
			conquest_sync.send(p_tick, *match.rules(), slots, outbox);
		}
		const MatchBroadcast b = match.take_broadcast(p_tick);
		if (b.state.empty() && b.players.empty() && b.orders.empty()) {
			return;
		}
		for (size_t i = 0; i < slots.size(); ++i) {
			const Seat &s = slots[i];
			if (!s.used || !s.remote) {
				continue;
			}
			if (!b.state.empty()) {
				outbox.send(s.peer, net::kChannelReliable, b.state);
			}
			if (!b.players.empty()) {
				outbox.send(s.peer, net::kChannelReliable, b.players);
			}
			const uint8_t team = match.rules()->team_of(uint8_t(i));
			for (const SquadOrderMessage &o : b.orders) {
				if (o.team == team) {
					outbox.send(s.peer, net::kChannelReliable, o.bytes);
				}
			}
		}
	}

	void send_deployables(sim::Tick p_tick) {
		const Gunplay *g = combat.gunplay();
		if (g == nullptr) {
			return;
		}
		for (size_t i = 0; i < slots.size(); ++i) {
			const Seat &s = slots[i];
			if (!s.used || !s.remote) {
				continue;
			}
			const uint8_t team = match.on() ? match.rules()->team_of(uint8_t(i))
											: uint8_t(std::clamp(s.team, 0, int(net::kMaxTeam)));
			if (std::optional<std::vector<uint8_t>> bytes = deployables.update(
						i, team, soldiers[i].state.position, g->deployables(), g->deployables_revision(), p_tick)) {
				outbox.send(s.peer, net::kChannelReliable, *bytes);
			}
		}
	}

	const std::vector<uint8_t> &alive() {
		alive_mask.assign(slots.size(), 0);
		for (size_t i = 0; i < slots.size(); ++i) {
			alive_mask[i] = slots[i].used && soldiers[i].spawned && soldiers.alive(i) ? 1 : 0;
		}
		return alive_mask;
	}

	void step_spotting() {
		const Gunplay *g = combat.gunplay();
		if (g == nullptr) {
			return;
		}
		spotting.step(sim::kTickSeconds, alive());
		step_bot_spotters(g->level().soldier);
		for (const auto &[slot, request] : spotting.take_queued_orders()) {
			order(slot, request);
		}
		const float heal_below = spotting.def().spotter.order_heal_percent;
		const float max_health = g->level().max_health;
		for (size_t i = 0; i < slots.size(); ++i) {
			spotting.set_low_health(uint8_t(i),
					alive_mask[i] != 0 && max_health > 0.0f &&
							100.0f * soldiers[i].state.health / max_health < heal_below);
		}
		if (spotting.low_ammo_due(sim::kTickSeconds)) {
			const float need = spotting.def().spotter.order_need_ammo_percent;
			for (size_t i = 0; i < slots.size(); ++i) {
				spotting.set_low_ammo(uint8_t(i), alive_mask[i] != 0 && soldier_low_ammo(*g, uint8_t(i), need));
			}
		}
	}

	void step_bot_spotters(const core::gameplay::SoldierDef &p_soldier) {
		std::vector<SpotSubject> subjects;
		std::vector<BotEye> eyes;
		for (size_t i = 0; i < slots.size(); ++i) {
			if (alive_mask[i] == 0) {
				continue;
			}
			const sim::SoldierState &st = soldiers[i].state;
			const uint8_t team = uint8_t(std::clamp(
					match.on() ? int(match.rules()->team_of(uint8_t(i))) : slots[i].team, 0, int(net::kMaxTeam)));
			const float eye = motor.eye_height(st);
			SpotSubject s;
			s.slot = uint8_t(i);
			s.team = team;
			if (place_spot_body(s, p_soldier, st.pose, st.position)) {
				subjects.push_back(s);
			}
			if (slots[i].bot) {
				eyes.push_back(BotEye{ uint8_t(i), team, { st.position[0], st.position[1] + eye, st.position[2] },
						sim::weapons::aim_direction(st.yaw, st.pitch, {}) });
			}
		}
		spotting.step_bots(
				double(tick) * double(sim::kTickSeconds), sim::kTickSeconds, eyes, subjects, world.get(), alive_mask);
	}

	void send_spotting(sim::Tick p_tick) {
		if (std::optional<net::SpotStates> changes = spotting.take_changes(p_tick)) {
			broadcast(net::encode_spot_states(*changes));
		}
	}

	void spot(size_t p_slot, const net::SpotRequest &p_request) {
		if (combat.on()) {
			spotting.spot(uint8_t(p_slot), p_request, alive());
		}
	}

	void order(size_t p_slot, const net::OrderRequest &p_request) {
		if (!combat.on()) {
			return;
		}
		if (const std::optional<OrderDelivery> d = spotting.order(uint8_t(p_slot), p_request, alive())) {
			const Seat &target = slots[d->target];
			if (target.used && target.remote) {
				outbox.send(target.peer, net::kChannelReliable, net::encode_order_notice(d->notice));
			}
		}
	}

	void receive_spot_request(int p_slot, std::span<const uint8_t> p_datagram) {
		core::Result<net::SpotRequest> r = net::decode_spot_request(p_datagram);
		if (!r || p_slot < 0 || size_t(p_slot) >= slots.size()) {
			++bad_datagrams;
			return;
		}
		spot(size_t(p_slot), *r);
	}

	void receive_order_request(int p_slot, std::span<const uint8_t> p_datagram) {
		core::Result<net::OrderRequest> r = net::decode_order_request(p_datagram);
		if (!r || p_slot < 0 || size_t(p_slot) >= slots.size()) {
			++bad_datagrams;
			return;
		}
		order(size_t(p_slot), *r);
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
		const SpawnRefusal why = match.request_spawn(size_t(p_slot), r->kit, r->spawn_choice, tick + 1u);
		if (const std::optional<net::SpawnAnswerKind> kind = spawn_answer_of(why);
				kind && slots[size_t(p_slot)].remote) {
			outbox.send(slots[size_t(p_slot)].peer, net::kChannelReliable,
					net::encode_spawn_answer({ *kind, r->spawn_choice }));
		}
	}

	void receive_loadout_request(int p_slot, std::span<const uint8_t> p_datagram) {
		core::Result<net::LoadoutRequest> r = net::decode_loadout_request(p_datagram);
		if (!r || p_slot < 0 || size_t(p_slot) >= slots.size()) {
			++bad_datagrams;
			combat.refuse_loadout();
			return;
		}
		const uint8_t team = match.on() ? match.rules()->team_of(uint8_t(p_slot))
										: uint8_t(std::clamp(slots[size_t(p_slot)].team, 0, int(net::kMaxTeam)));
		combat.select_loadout(size_t(p_slot), team, *r);
	}

	void receive_squad_request(int p_slot, std::span<const uint8_t> p_datagram) {
		core::Result<net::SquadRequest> r = net::decode_squad_request(p_datagram);
		if (!r || p_slot < 0 || size_t(p_slot) >= slots.size() || !match.on()) {
			++bad_datagrams;
			return;
		}
		match.request_squad(size_t(p_slot), *r);
	}

	void receive_suicide_request(int p_slot, std::span<const uint8_t> p_datagram) {
		if (!net::decode_suicide_request(p_datagram) || p_slot < 0 || size_t(p_slot) >= slots.size() || !match.on()) {
			++bad_datagrams;
			return;
		}
		match.request_suicide(size_t(p_slot));
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
	if (p_config.snapshot_budget_bytes < 256) {
		return core::make_error("GameServer: snapshot_budget_bytes " + std::to_string(p_config.snapshot_budget_bytes) +
				" is under 256");
	}
	m.config = p_config;
	core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(p_config.level->static_world);
	if (!world) {
		return core::error_at("GameServer", world.error());
	}
	m.world = std::move(*world);
	BF3_TRY(m.motor.init(*m.world, p_config.level->motor));
	m.fall_out_y = sim::fall_out_y(m.motor, *m.world);
	m.motor_hash = sim::motor_params_hash(p_config.level->motor);
	m.slots.resize(p_config.max_slots);
	m.soldiers.resize(p_config.max_slots);
	m.spawner.resize(p_config.max_slots);
	m.logic.resize(p_config.max_slots);
	m.commands.configure(p_config.max_slots, p_config.jitter_buffer_ticks);
	m.snapshots.configure(p_config.max_slots, p_config.snapshot_budget_bytes, p_config.ghost_scope);
	m.deployables.configure(p_config.max_slots, p_config.ghost_scope);
	m.dev.configure(m.config, *m.world, p_config.max_slots);
	BF3_TRY(m.vehicles.configure(
			p_config.level->vehicles, p_config.level->static_world, p_config.vehicle_spawn_delay_modifier));
	m.vehicles.configure_scope(p_config.max_slots, p_config.ghost_scope);
	m.handshake.configure(p_config.game_build.value_or(net::game_build()), *p_config.level, p_config.max_slots);
	m.loads.configure(p_config.max_slots, p_config.load, p_config.round_id);
	for (size_t i = 0; i < m.slots.size(); ++i) {
		m.reset_slot(i);
	}
	BF3_TRY(m.init_gameplay());
	BF3_TRY(m.admit_carried());
	for (uint32_t i = 0; i < p_config.walkers; ++i) {
		if (add_local_soldier(std::make_unique<LegWalker>(i), 2) < 0) {
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
		BF3_TRY(add_tdm_bots(*this, m.bot_fill_config()));
	}
	return core::Status::success();
}

void GameServer::on_connect(uint32_t p_peer) {
	impl_->outbox.forget_traffic(p_peer);
	impl_->received.erase(p_peer);
}

void GameServer::on_disconnect(uint32_t p_peer) {
	Impl &m = *impl_;
	const int at = m.slots.slot_of_peer(p_peer);
	if (at >= 0) {
		m.drop_client(size_t(at));
	}
}

void GameServer::on_receive(uint32_t p_peer, uint8_t p_channel, std::span<const uint8_t> p_datagram) {
	Impl &m = *impl_;
	m.received[p_peer].count(p_channel, p_datagram);
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
			if (m.changing) {
				++m.held_readies;
				return;
			}
			if (const std::optional<net::Kick> refused = m.handshake.ready(size_t(at), *r)) {
				m.outbox.send(p_peer, net::kChannelReliable, net::encode_kick(*refused));
				on_disconnect(p_peer);
				return;
			}
			m.loads.done(size_t(at));
			m.spawner.ready(size_t(at));
			return;
		}
		case net::MessageType::Mismatch: {
			core::Result<net::Mismatch> x = net::decode_mismatch(p_datagram);
			if (!x || at < 0) {
				++m.bad_datagrams;
				return;
			}
			m.outbox.send(p_peer, net::kChannelReliable, net::encode_kick(m.handshake.mismatch(x->components)));
			on_disconnect(p_peer);
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
			core::Result<net::Loading> l = net::decode_loading(p_datagram);
			if (!l || at < 0) {
				++m.bad_datagrams;
				return;
			}
			m.loads.progress(size_t(at), *l, m.tick);
			return;
		}
		case net::MessageType::Prepare: {
			core::Result<net::Prepare> p = net::decode_prepare(p_datagram);
			if (!p || at < 0) {
				++m.bad_datagrams;
				return;
			}
			m.loads.prepared(size_t(at), *p);
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
		case net::MessageType::SuicideRequest:
			m.receive_suicide_request(at, p_datagram);
			return;
		case net::MessageType::LoadoutRequest:
			m.receive_loadout_request(at, p_datagram);
			return;
		case net::MessageType::SquadRequest:
			m.receive_squad_request(at, p_datagram);
			return;
		case net::MessageType::SpotRequest:
			m.receive_spot_request(at, p_datagram);
			return;
		case net::MessageType::OrderRequest:
			m.receive_order_request(at, p_datagram);
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
	g.kicks = m.handshake.kicks();
	g.round_id = m.config.round_id;
	g.carried_seats = m.carried;
	g.level_changing = m.changing;
	g.held_joins = m.held_joins;
	g.held_readies = m.held_readies;
	g.stale_loading = m.loads.stale_reports();
	if (const TdmRules *rules = m.match.rules(); rules != nullptr) {
		g.rules_active = true;
		g.match = rules->match_state();
		g.rules = rules->stats();
		if (const sim::logic::ConquestNatives *conquest = rules->natives().conquest()) {
			g.conquest_active = true;
			g.conquest = conquest_wire_state(*conquest, m.tick);
			g.conquest_captures = conquest->stats().captures;
			g.conquest_messages = m.conquest_sync.messages();
		}
	}
	g.rules_step_failures = m.match.step_failures();
	g.spotting = m.spotting.stats();
	g.vehicle_shots = m.vehicles.gunnery().stats().shots;
	g.vehicle_hits = m.vehicles.stats().hits;
	g.vehicle_repairs = m.vehicles.stats().repairs;
	g.vehicles_destroyed = m.vehicles.stats().destroyed;
	g.vehicle_crew_kills = m.vehicles.stats().occupant_kills;
	g.vehicle_wreck_blasts = m.vehicles.stats().wreck_blasts;
	g.vehicle_wreck_blast_soldier_hits = m.vehicles.stats().wreck_blast_soldier_hits;
	g.vehicle_wreck_blast_vehicle_hits = m.vehicles.stats().wreck_blast_vehicle_hits;
	g.vehicle_unlocated_hits = m.vehicles.stats().unlocated;
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
		const Seat &s = m.slots[i];
		const Soldier &body = m.soldiers[i];
		SlotStats &o = g.slots[i];
		m.commands.stats(i, m.tick, o);
		o.used = s.used;
		o.remote = s.remote;
		o.peer = s.peer;
		o.life = body.state.life;
		o.alive = m.soldiers.alive(i);
		o.state = body.state;
		o.load = m.loads[i];
		g.clients += (s.used && s.remote) ? 1u : 0u;
		g.soldiers += o.alive ? 1u : 0u;
		if (s.used && s.remote) {
			const auto in = m.received.find(s.peer);
			g.traffic.push_back({ uint8_t(i), s.peer, m.outbox.traffic(s.peer),
					in == m.received.end() ? NetTraffic() : in->second, m.snapshots.intervals(i) });
		}
	}
	m.combat.stats(g);
	return g;
}

const GameServerTickTimes &GameServer::last_tick_times() const {
	return impl_->times;
}

const sim::SoldierState *GameServer::soldier(uint32_t p_slot) const {
	const Impl &m = *impl_;
	if (p_slot >= m.slots.size() || !m.slots[p_slot].used || !m.soldiers[p_slot].spawned) {
		return nullptr;
	}
	return &m.soldiers[p_slot].state;
}

GameServer::GameServer() :
		impl_(std::make_unique<Impl>()) {}

GameServer::~GameServer() = default;

core::Status GameServer::Impl::init_gameplay() {
	view.configure(*world);
	view.set_vehicles(&vehicles);
	spawner.configure(config, *world, view);
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
	BF3_TRY(vehicles.configure_weapons(combat.gunplay()->level().materials, combat.gunplay()->table().mechanics));
	vehicles.configure_damage(combat.gunplay()->table(), combat.gunplay()->level().defs);
	core::Result<sim::spotting::SpottingDef> spotting_def =
			sim::spotting::make_spotting_def(config.level->gunplay->soldier);
	if (!spotting_def) {
		return core::error_at("GameServer", spotting_def.error());
	}
	spotting.configure(slots.size(), *spotting_def);
	view.set_spotting(&spotting);
	std::vector<FlyBy> fly_by;
	for (const core::gameplay::WeaponDef &d : config.level->gunplay->defs) {
		fly_by.push_back({ d.projectile.fly_by_radius, float(core::gameplay::max_range(d)) });
	}
	snapshots.set_fly_by(std::move(fly_by));
	if (tdm) {
		BF3_TRY(match.create_rules(config, combat.gunplay()->match_seed()));
		combat.gunplay()->set_squads(&match.rules()->squads());
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

void GameServer::Impl::step_rules(sim::Tick p_tick) {
	if (combat.on()) {
		combat_log.open(p_tick);
		combat.gunplay()->set_vehicle_targets(vehicles.targets());
		combat.gunplay()->set_pack_vehicles(vehicles.pack_vehicles());
		combat.resolve(p_tick, slots, soldiers, combat_log,
				[&](const sim::weapons::HitTargetProvider &p_targets,
						std::vector<sim::weapons::DamageEvent> &r_damage) {
					vehicles.resolve_weapons(p_tick, *world, p_targets, r_damage);
					vehicles.take_hits(*combat.gunplay(), r_damage);
				});
		match.step(p_tick, view, *combat.gunplay(), combat.tick_states());
		soldiers.take_rules(combat.tick_states(), slots);
	}

	step_spotting();
	spawner.place_queued(p_tick);
	const std::vector<RulesOrder> orders = match.take_orders();
	if (!orders.empty()) {
		spawner.apply_orders(p_tick, orders, combat_log);
	}
}

core::Status GameServer::place_soldier(uint32_t p_slot, const sim::Vec3 &p_position, uint16_t p_yaw, sim::Pose p_pose) {
	return impl_->spawner.queue_place(p_slot, p_position, p_yaw, p_pose);
}

void GameServer::request_spot(uint8_t p_slot, const net::SpotRequest &p_request) {
	if (p_slot < impl_->slots.size()) {
		impl_->spot(p_slot, p_request);
	}
}

void GameServer::request_order(uint8_t p_slot, const net::OrderRequest &p_request) {
	if (p_slot < impl_->slots.size()) {
		impl_->order(p_slot, p_request);
	}
}

net::SpotRow GameServer::spot_row(uint8_t p_slot) const {
	const Impl &m = *impl_;
	if (p_slot >= m.slots.size() || !m.combat.on()) {
		return net::SpotRow{ p_slot };
	}
	return m.spotting.row(p_slot);
}

const ServerView &GameServer::view() const {
	if (!impl_->view.ready()) {

		std::fprintf(stderr, "GameServer::view() called before init\n");
		std::abort();
	}
	return impl_->view;
}

const Gunplay *GameServer::gunplay() const {
	return impl_->combat.gunplay();
}

const TdmRules *GameServer::rules() const {
	return impl_->match.rules();
}

TdmRules *GameServer::rules() {
	return impl_->match.rules();
}

void GameServer::set_peer_rtts(std::span<const uint32_t> p_rtt_ms) {
	Impl &m = *impl_;
	TdmRules *rules = m.match.rules();
	if (rules == nullptr) {
		return;
	}
	for (size_t i = 0; i < m.slots.size(); ++i) {
		const Seat &s = m.slots[i];
		if (s.used && s.remote && s.peer < p_rtt_ms.size()) {
			rules->set_ping(uint8_t(i), uint16_t(std::min<uint32_t>(p_rtt_ms[s.peer], UINT16_MAX)));
		}
	}
}

bool GameServer::set_squad_order(uint8_t p_team, uint8_t p_squad, const SquadOrderState &p_order) {
	TdmRules *rules = impl_->match.rules();
	return rules != nullptr && rules->set_squad_order(p_team, p_squad, p_order);
}

bool GameServer::level_ended() const {
	const TdmRules *rules = impl_->match.rules();
	return rules != nullptr && rules->level_ended();
}

void GameServer::restart_round() {
	Impl &m = *impl_;
	TdmRules *rules = m.match.rules();
	if (rules == nullptr) {
		return;
	}
	std::vector<RulesOrder> orders;
	rules->restart_in_place(m.tick, orders);
	m.vehicles.restart();
	if (Gunplay *g = m.combat.gunplay()) {
		g->clear_deployables();
	}
	if (!orders.empty()) {
		m.spawner.apply_orders(m.tick, orders, m.combat_log);
	}
}

void GameServer::announce_round_over(const net::RoundOverInfo &p_info) {
	impl_->broadcast(net::encode_round_over_info(p_info));
}

void GameServer::begin_level_change(const net::LevelChange &p_change, const net::NextLevel &p_next) {
	Impl &m = *impl_;
	m.changing = true;
	m.level_change = p_change;
	m.next_level = p_next;
	m.broadcast(net::encode_level_change(p_change));
}

bool GameServer::level_changing() const {
	return impl_->changing;
}

std::vector<CarriedSeat> GameServer::seats_to_carry() const {
	const Impl &m = *impl_;
	std::vector<CarriedSeat> out;
	const TdmRules *rules = m.match.rules();
	const Gunplay *gunplay = m.combat.gunplay();
	for (size_t i = 0; i < m.slots.size(); ++i) {
		const Seat &s = m.slots[i];
		if (!s.used || !s.remote) {
			continue;
		}
		CarriedSeat c;
		c.slot = uint8_t(i);
		c.peer = s.peer;
		c.name = s.name;
		c.team = rules != nullptr ? rules->team_of(uint8_t(i)) : uint8_t(std::clamp(s.team, 0, int(net::kMaxTeam)));
		c.squad = rules != nullptr ? rules->squads().squad_of(uint8_t(i)) : uint8_t(0);
		c.kit = m.combat.kit(i);
		if (gunplay != nullptr) {
			c.loadouts = gunplay->loadout_choices(uint8_t(i));
		}
		out.push_back(std::move(c));
	}
	return out;
}

std::vector<CarriedSeat> GameServer::release_seats() {
	std::vector<CarriedSeat> out = seats_to_carry();
	for (const CarriedSeat &c : out) {
		impl_->reset_slot(c.slot);
	}
	return out;
}

int GameServer::add_local_bot(std::unique_ptr<CommandSource> p_source, int p_team) {
	return impl_->add_local(std::move(p_source), p_team, true);
}

}
