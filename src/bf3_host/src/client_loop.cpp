#include "client_loop.h"

#include "client_ticks.h"

#include "bf3/host/gunplay.h"
#include "bf3/net/other_sample.h"
#include "bf3/sim/weapons/weapon_rng.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>

namespace bf3::host {

namespace {

float length3(const sim::Vec3 &p_v) {
	return std::sqrt(p_v[0] * p_v[0] + p_v[1] * p_v[1] + p_v[2] * p_v[2]);
}

sim::Vec3 sub3(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	return { p_a[0] - p_b[0], p_a[1] - p_b[1], p_a[2] - p_b[2] };
}

bool states_match(const sim::SoldierState &p_a, const sim::SoldierState &p_b, float p_pos_eps, float p_vel_eps) {
	const uint16_t events_a = uint16_t(p_a.events & ~sim::soldier_events::kDamaged);
	const uint16_t events_b = uint16_t(p_b.events & ~sim::soldier_events::kDamaged);
	if (p_a.tick != p_b.tick || p_a.life != p_b.life || p_a.yaw != p_b.yaw || p_a.pitch != p_b.pitch || p_a.pose != p_b.pose ||
			p_a.target_pose != p_b.target_pose || p_a.transition_ticks != p_b.transition_ticks || p_a.transition_total != p_b.transition_total ||
			p_a.mode != p_b.mode || p_a.flags != p_b.flags || events_a != events_b || p_a.jump_penalty_ticks != p_b.jump_penalty_ticks ||
			p_a.air_ticks != p_b.air_ticks || p_a.protect_ticks != p_b.protect_ticks) {
		return false;
	}
	if (!(length3(sub3(p_a.position, p_b.position)) <= p_pos_eps) || !(length3(sub3(p_a.velocity, p_b.velocity)) <= p_vel_eps)) {
		return false;
	}
	return std::fabs(p_a.impact_speed - p_b.impact_speed) <= 1e-3f;
}

}

void ClientLoop::handle_datagram(const net::NetSim::Datagram &p_d, uint64_t p_now) {
	const std::optional<net::MessageType> type = net::peek_type(p_d.bytes);
	if (!type) {
		return;
	}
	switch (*type) {
		case net::MessageType::MatchInfo: {
			core::Result<net::MatchInfo> m = net::decode_match_info(p_d.bytes);
			if (!m || have_match) {
				return;
			}
			if (!accept_match(*m, p_now)) {
				return;
			}
			have_match = true;
			slot = m->slot;
			stats_.slot = m->slot;
			tracks.set_divisor(std::max<uint8_t>(m->snapshot_divisor, 1));
			if (m->motor_hash != motor_hash) {
				++stats_.motor_hash_mismatch;
			}
			clock.start(m->server_tick, p_now, input.next_sequence());
			last_tick_ns = p_now;
			return;
		}
		case net::MessageType::Spawned: {
			core::Result<net::Spawned> s = net::decode_spawned(p_d.bytes);
			if (s && pending_spawned.size() < 16) {
				gunplay.spawned(*s, have_own, own_life);
				feed.spawned(*s);
				clock.anchor_spawned(s->state.tick, p_now);
				pending_spawned.push_back(s->state);
			}
			return;
		}
		case net::MessageType::Snapshot: {
			core::Result<net::Snapshot> s = net::decode_snapshot(p_d.bytes);
			if (!s) {
				return;
			}
			const bool newer = !link.have_snapshot() || net::seq_newer(s->seq, link.newest_seq());
			gunplay.note_arrival(s->seq, p_now);
			if (newer || s->seq != link.newest_seq()) {
				tracks.reorder(s->server_tick, s->others, slot);
			}
			gunplay.snapshot(*s, newer);
			server_events_acc = uint16_t(server_events_acc | s->own_events);
			clock.rtt_sample(*s, p_now, input.next_sequence());
			const SnapshotOrder order = link.arrive(s->seq);
			if (order == SnapshotOrder::Late) {
				tracks.add(s->server_tick, s->others, slot, p_now);
			}
			if (order != SnapshotOrder::Newest) {
				return;
			}
			clock.anchor_snapshot(s->server_tick, p_now);
			tracks.arrival(s->server_tick, p_now);
			tracks.add(s->server_tick, s->others, slot, p_now);
			pending_snapshot = std::move(*s);
			pending_snapshot_valid = true;
			return;
		}
		case net::MessageType::DebugReport: {
			core::Result<net::DebugReport> r = net::decode_debug_report(p_d.bytes);
			if (r) {
				link.report(std::move(*r));
			}
			return;
		}
		case net::MessageType::Kick: {
			core::Result<net::Kick> k = net::decode_kick(p_d.bytes);
			link.drop("kicked: " + (k ? k->reason : std::string("(bad kick)")));
			return;
		}
		case net::MessageType::Bye:
			link.drop("the server said bye");
			return;
		default:
			if (match.datagram(*type, p_d)) {
				return;
			}
			gunplay_datagram(*type, p_d, p_now);
			return;
	}
}

void ClientLoop::run_catch_up(const sim::SoldierState &p_base) {
	const sim::CatchUpStats c = sim::catch_up(motor, p_base, *ring, clock.next_tick());
	stats_.replayed_ticks += c.steps;
}

void ClientLoop::authoritative(const sim::SoldierState &p_s, uint64_t p_now) {
	if (have_own && p_s.life < own_life) {
		return;
	}
	const bool spawn = !have_own || p_s.life > own_life;
	if (!spawn && have_auth && p_s.tick <= last_auth.tick) {
		return;
	}

	if (p_s.tick >= clock.next_tick()) {
		clock.force_past(p_s.tick, p_now, input.next_sequence());
	}
	last_auth = p_s;
	have_auth = true;
	stats_.last_server_tick = std::max(stats_.last_server_tick, p_s.tick);
	if (spawn) {
		own_life = p_s.life;
		have_own = true;
		spawn_state = p_s;
		input.keep_pose(p_s.target_pose);

		ring->put({ sim::hold_command(p_s), p_s, false });
		gunplay.weapons_spawn(p_s);
		run_catch_up(p_s);
		++stats_.spawns;
		stats_.spawned = true;
		visual_offset = {};
		return;
	}
	const sim::PredictionEntry *e = ring->find(p_s.tick);
	if (e == nullptr) {
		++stats_.ring_misses;
		gunplay.weapons_ring_miss(p_s);
		run_catch_up(p_s);
		return;
	}
	if (states_match(e->state, p_s, config.position_epsilon_m, config.velocity_epsilon)) {
		if (config.debug_hash_compare && sim::soldier_state_hash(e->state) != sim::soldier_state_hash(p_s)) {
			++stats_.hash_mismatches;
		}
		after_match(p_s);
		return;
	}
	++stats_.reconciliations;
	stats_.last_reconcile_tick = p_s.tick;
	stats_.last_reconcile_ns = p_now;
	const sim::Tick next = clock.next_tick();
	const sim::PredictionEntry *before = next > 0 ? ring->find(next - 1u) : nullptr;
	const sim::Vec3 old_pos = before != nullptr ? before->state.position : p_s.position;
	gunplay.weapons_reconcile(p_s);
	run_catch_up(p_s);
	const sim::PredictionEntry *after = next > 0 ? ring->find(next - 1u) : nullptr;
	const sim::Vec3 new_pos = after != nullptr ? after->state.position : p_s.position;
	const sim::Vec3 corr = sub3(old_pos, new_pos);
	const float m = length3(corr);
	if (m > 1e-3f) {
		++stats_.corrections_over_1mm;
	}
	stats_.max_correction_m = std::max(stats_.max_correction_m, m);
	if (m > config.teleport_above_m) {
		++stats_.teleports;
		visual_offset = {};
	} else {
		for (size_t k = 0; k < 3; ++k) {
			visual_offset[k] += corr[k];
		}
	}
}

void ClientLoop::client_tick(uint64_t p_now) {

	const double dt_ms = last_tick_ns != 0 && p_now > last_tick_ns ? double(p_now - last_tick_ns) / 1e6 : kMsPerTick;
	last_tick_ns = p_now;
	const float decay = float(std::exp(-std::min(dt_ms, 1000.0) / std::max(1e-3, double(config.error_decay_ms))));
	for (float &v : visual_offset) {
		v *= decay;
	}
	if (length3(visual_offset) < config.snap_below_m) {
		visual_offset = {};
	}

	if (pending_snapshot_valid && pending_snapshot.own.has_value() && have_own) {
		if (const std::optional<sim::Tick> discard = clock.buffer_report(pending_snapshot, p_now, input.next_sequence())) {
			ring->discard_after(*discard);
		}
	}
	for (const sim::SoldierState &s : pending_spawned) {
		authoritative(s, p_now);
	}
	pending_spawned.clear();
	if (pending_snapshot_valid) {
		if (pending_snapshot.own.has_value()) {
			authoritative(*pending_snapshot.own, p_now);
		}
		pending_snapshot_valid = false;
	}
	const sim::Tick c = clock.next_tick();
	const sim::PredictionEntry *made = nullptr;
	sim::Command cmd;
	if (have_own && c > 0 && !match.unspawned(c, have_own, spawn_state.tick)) {

		if (ring->find(c - 1u) == nullptr) {
			const sim::PredictionEntry *b = ring->newest_at_or_below(c - 1u);
			const sim::SoldierState *base = (b != nullptr && b->state.life == own_life) ? &b->state : nullptr;
			if (have_auth && last_auth.life == own_life && last_auth.tick < c && (base == nullptr || last_auth.tick > base->tick)) {
				base = &last_auth;
			}
			if (base != nullptr) {
				const sim::SoldierState copy = *base;
				run_catch_up(copy);
				++stats_.base_fills;
			}
		}

		const sim::PredictionEntry *base = ring->find(c - 1u);
		cmd = input.sample(c, own_life, spawn_state);
		gunplay.command_made(cmd, input.latest(), own_life, p_now, link.have_snapshot(), link.newest_seq(), *build);
		match.restrict(cmd);
		if (base == nullptr || cmd.tick != base->state.tick + 1u) {
			++stats_.step_precondition_failures;
			assert(false && "ClientSession: prediction base missing");
		} else {
			sim::SoldierState state = base->state;
			motor.step(state, cmd);
			gunplay.predict(cmd, base->state, state, *world);
			events_acc |= state.events;
			ring->put({ cmd, state, true });
			made = ring->find(c);
			input.stepped(cmd);

			clock.command_sent(cmd.sequence, p_now);
			link.send_commands(cmd, gunplay.others_ack_bits(link.have_snapshot(), link.newest_seq()), p_now);
		}
	}
	clock.set_next_tick(c + 1u);
	publish(p_now, made, cmd);
}

void ClientLoop::publish(uint64_t p_now, const sim::PredictionEntry *p_made, const sim::Command &p_cmd) {
	tracks.count_timeouts(p_now);
	PresentationFrame &f = *build;
	f.serial = ++serial;
	f.publish_ns = p_now;
	f.own_ticks_per_ns = clock.own_ticks_per_ns();
	const sim::PredictionEntry *prev = p_made != nullptr && p_made->state.tick > 0 ? ring->find(p_made->state.tick - 1u) : nullptr;
	f.valid = p_made != nullptr && prev != nullptr;
	if (f.valid) {
		f.tick = p_made->state.tick;
		f.tick_fraction = std::clamp(clock.ticks(p_now) - double(f.tick), 0.0, 1.0);
		f.own_prev = prev->state;
		f.own_curr = p_made->state;
		f.own_command = p_cmd;
		f.eye_height_prev = motor.eye_height(prev->state);
		f.eye_height_curr = motor.eye_height(p_made->state);
	}
	f.visual_offset = visual_offset;
	f.server_own_valid = have_auth;
	f.server_own = last_auth;
	f.server_events = server_events_acc;
	server_events_acc = 0;
	f.events = events_acc;
	events_acc = 0;

	f.extrapolate_max_ms = config.extrapolate_max_ms;
	for (size_t k = 0; k < sim::kPoseCount; ++k) {
		f.pose_eye_heights[k] = level->motor.poses[k].eye_height;
	}
	tracks.publish(f, p_now);
	f.newest_seq = link.newest_seq();
	gunplay.publish(f, tracks.finish(f, link.have_snapshot(), gunplay.newest_tick(), gunplay.wants_poses()), *world);
	match.publish(f, have_own, own_life);
	stats_.own_shot_id_resets = feed.shot_id_resets();
	stats_.connected = link.connected();
	clock.publish_stats(stats_);
	tracks.publish_stats(stats_);
	gunplay.publish_stats(stats_);
	stats_.enet_rtt_ms = link.transport_rtt_ms();
	stats_.netsim_out = link.out_stats();
	stats_.netsim_in = link.in_stats();
	collect_stats();
	f.stats = stats_;
	std::lock_guard lock(frame_mutex);
	copy_frame(*shared_frame, f);
}

void ClientLoop::collect_stats() {
	link.stats(stats_);
	clock.stats(stats_);
	tracks.stats(stats_);
	feed.stats(stats_);
	match.stats(stats_);
	gunplay.stats(stats_);
	stats_.bad_gunplay_events = gunplay.bad_events() + match.bad_events();
}

void ClientLoop::copy_frame(PresentationFrame &r_to, const PresentationFrame &p_from) {
	constexpr size_t kOthersAt = offsetof(PresentationFrame, others);
	constexpr size_t kOthersEnd = kOthersAt + sizeof(PresentationFrame::others);
	static_assert(kOthersEnd <= sizeof(PresentationFrame));
	const uint32_t n = std::min<uint32_t>(p_from.other_count, uint32_t(p_from.others.size()));
	auto *to = reinterpret_cast<unsigned char *>(&r_to);
	const auto *from = reinterpret_cast<const unsigned char *>(&p_from);
	std::memcpy(to, from, kOthersAt);
	std::memcpy(to + kOthersAt, from + kOthersAt, size_t(n) * sizeof(OtherTrack));
	std::memcpy(to + kOthersEnd, from + kOthersEnd, sizeof(PresentationFrame) - kOthersEnd);
}

void ClientLoop::iterate() {
	if (const int32_t shift = shift_request.exchange(0); shift != 0 && clock.running()) {
		clock.shift(shift, now_ns());
	}
	uint64_t now = now_ns();
	uint64_t deadline = now + 50'000'000ull;
	if (clock.running() && link.connected()) {
		deadline = std::min(deadline, clock.tick_deadline_ns(clock.next_tick()));
	}
	deadline = link.wait_deadline(deadline);
	const uint32_t wait_ms = deadline > now ? uint32_t(std::min<uint64_t>((deadline - now) / 1'000'000ull, 50)) : 0u;

	link.receive(wait_ms);
	now = now_ns();
	net::NetSim::Datagram d;
	while (link.poll(now, d)) {
		handle_datagram(d, now);
	}
	link.send_requests(now);
	link.release(now);
	if (clock.running() && link.connected() && clock.due(now)) {
		client_tick(now);
	}
}

void ClientLoop::shutdown() {
	link.shutdown();
}

core::Status ClientLoop::init(const ClientSessionConfig &p_config, std::shared_ptr<const SharedLevel> p_level) {
	config = p_config;
	level = p_level;
	config.prediction_ring = std::clamp<uint32_t>(p_config.prediction_ring, 16u, 4096u);
	config.max_buffer_ticks = std::max(p_config.max_buffer_ticks, p_config.target_buffer_ticks);
	core::Result<std::unique_ptr<sim::CollisionWorld>> created = sim::CollisionWorld::create(p_level->static_world);
	if (!created) {
		return core::error_at("ClientSession", created.error());
	}
	world = std::move(*created);
	BF3_TRY(motor.init(*world, p_level->motor));
	motor_hash = sim::motor_params_hash(p_level->motor);
	ring = std::make_unique<sim::PredictionRing>(config.prediction_ring);
	link.configure(p_config.netsim);
	return core::Status::success();
}

void ClientLoop::begin(std::unique_ptr<ClientTransport> p_transport, MonotonicClock p_clock) {
	now_ns = std::move(p_clock);
	link.attach(std::move(p_transport));
	stats_ = ClientStats();
	stats_.connected = true;
	const uint64_t now = now_ns();
	net::Hello hello;
	hello.name = config.name;
	clock.hello_sent(now);
	link.submit(net::kChannelReliable, net::encode_hello(hello), now);
	link.release(now);
}

void ClientLoop::close() {
	link.close();
}

void ClientLoop::set_input(const InputSample &p_input) {
	input.set(p_input);
}

bool ClientLoop::read_presentation(PresentationFrame &r_frame) const {
	std::lock_guard lock(frame_mutex);
	if (shared_frame->serial == r_frame.serial) {
		return false;
	}
	copy_frame(r_frame, *shared_frame);
	return true;
}

ClientStats ClientLoop::stats() const {
	std::lock_guard lock(frame_mutex);
	return shared_frame->stats;
}

void ClientLoop::send_debug(const net::DebugRequest &p_request) {
	link.queue_debug(p_request);
}

bool ClientLoop::take_debug_report(net::DebugReport &r_report) {
	return link.take_report(r_report);
}

void ClientLoop::shift_clock(int32_t p_ticks) {
	shift_request.fetch_add(p_ticks, std::memory_order_acq_rel);
}

void ClientLoop::refuse(const char *p_reason) {
	have_match = true;
	link.refuse(p_reason);
	stats_.connected = false;
	collect_stats();
	std::lock_guard lock(frame_mutex);
	shared_frame->stats = stats_;
}

bool ClientLoop::accept_match(const net::MatchInfo &p_match, uint64_t p_now) {
	stats_.slot = p_match.slot;
	if (const char *refusal = match.check_rules(p_match, *level)) {
		refuse(refusal);
		return false;
	}
	const std::optional<std::string> refusal = gunplay.match_info(p_match, *level);
	if (gunplay.lane() != nullptr) {
		ring->attach_weapons(gunplay.lane());
	}
	if (refusal) {
		refuse(refusal->c_str());
		return false;
	}
	if (config.send_kit_select) {
		link.submit(net::kChannelReliable, net::encode_kit_select(config.kit_select), p_now);
	}
	net::Ready ready;
	ready.source_digest = level->source_digest;
	link.submit(net::kChannelReliable, net::encode_ready(ready), p_now);
	return true;
}

void ClientLoop::gunplay_datagram(net::MessageType p_type, const net::NetSim::Datagram &p_d, uint64_t p_now) {
	switch (p_type) {
		case net::MessageType::SnapshotOthers: {
			core::Result<net::SnapshotOthers> o = net::decode_snapshot_others(p_d.bytes);
			if (!o) {
				gunplay.bad_datagram();
				return;
			}
			gunplay.others(*o, p_now);
			tracks.reorder(o->server_tick, o->others, slot);
			tracks.add(o->server_tick, o->others, slot, p_now);
			return;
		}
		case net::MessageType::ShotEvents:
			gunplay.shot_events(p_d);
			return;
		case net::MessageType::CombatEvents: {
			net::CombatEvents e;
			if (gunplay.combat(p_d, e)) {
				match.combat(e, have_own, own_life);
			}
			return;
		}
		default:
			return;
	}
}

void ClientLoop::after_match(const sim::SoldierState &p_s) {
	if (gunplay.reconcile_after_match(p_s)) {
		run_catch_up(p_s);
		return;
	}
	const sim::PredictionEntry *at = ring->find(p_s.tick);
	if (at == nullptr || std::fabs(at->state.health - p_s.health) <= 1e-3f) {
		return;
	}
	const float delta = p_s.health - at->state.health;
	for (sim::Tick t = p_s.tick; t < clock.next_tick(); ++t) {
		const sim::PredictionEntry *e = ring->find(t);
		if (e == nullptr || e->state.life != p_s.life) {
			continue;
		}
		sim::PredictionEntry moved = *e;
		moved.state.health = t == p_s.tick ? p_s.health : moved.state.health + delta;
		if (t == p_s.tick) {
			moved.state.damage = p_s.damage;
			moved.state.events = uint16_t((moved.state.events & ~sim::soldier_events::kDamaged) | (p_s.events & sim::soldier_events::kDamaged));
		}
		ring->put(moved);
	}
	++stats_.health_rebases;
}

PresentationEvents &ClientLoop::events() {
	return feed.ring();
}

void ClientLoop::take_predicted(std::vector<PredictedShot> &r_shots, std::vector<PredictedTimed> &r_timed) {
	gunplay.take_predicted(r_shots, r_timed);
}

void ClientLoop::take_own_test_samples(std::vector<OwnTestSample> &r_samples) {
	gunplay.take_own_test_samples(r_samples);
}

void ClientLoop::send_kit_select(const net::KitSelect &p_select) {
	link.queue_kit_select(p_select);
}

void ClientLoop::send_spawn_request(uint8_t p_kit) {
	link.queue_spawn_request(p_kit);
}

}
