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

void set_bit(std::array<uint64_t, 2> &r_mask, uint32_t p_slot) {
	if (p_slot < 128) {
		r_mask[p_slot >> 6] |= uint64_t(1) << (p_slot & 63);
	}
}

void copy3(float (&r_to)[3], const sim::Vec3 &p_from) {
	for (size_t k = 0; k < 3; ++k) {
		r_to[k] = p_from[k];
	}
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
			if (!cl_match_info(*m, p_now)) {
				return;
			}
			have_match = true;
			slot = m->slot;
			stats_.slot = m->slot;
			tracks.set_divisor(std::max<uint8_t>(m->snapshot_divisor, 1));
			if (m->motor_hash != motor_hash) {
				++stats_.motor_hash_mismatch;
			}
			clock.start(m->server_tick, p_now, next_sequence);
			last_tick_ns = p_now;
			return;
		}
		case net::MessageType::Spawned: {
			core::Result<net::Spawned> s = net::decode_spawned(p_d.bytes);
			if (s && pending_spawned.size() < 16) {
				cl_spawned(*s);
				tdm_spawned(*s);
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
			cl_snapshot(*s, p_now);
			server_events_acc = uint16_t(server_events_acc | s->own_events);
			clock.rtt_sample(*s, p_now, next_sequence);
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
			if (tdm_datagram(*type, p_d)) {
				return;
			}
			cl_datagram(*type, p_d, p_now);
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
		clock.force_past(p_s.tick, p_now, next_sequence);
	}
	last_auth = p_s;
	have_auth = true;
	stats_.last_server_tick = std::max(stats_.last_server_tick, p_s.tick);
	if (spawn) {
		own_life = p_s.life;
		have_own = true;
		spawn_state = p_s;
		keep_pose = p_s.target_pose;

		ring->put({ sim::hold_command(p_s), p_s, false });
		cl_weapons_spawn(p_s);
		run_catch_up(p_s);
		++stats_.spawns;
		stats_.spawned = true;
		visual_offset = {};
		return;
	}
	const sim::PredictionEntry *e = ring->find(p_s.tick);
	if (e == nullptr) {
		++stats_.ring_misses;
		cl_weapons_ring_miss(p_s);
		run_catch_up(p_s);
		return;
	}
	if (states_match(e->state, p_s, config.position_epsilon_m, config.velocity_epsilon)) {
		if (config.debug_hash_compare && sim::soldier_state_hash(e->state) != sim::soldier_state_hash(p_s)) {
			++stats_.hash_mismatches;
		}
		cl_after_match(p_s);
		return;
	}
	++stats_.reconciliations;
	stats_.last_reconcile_tick = p_s.tick;
	stats_.last_reconcile_ns = p_now;
	const sim::Tick next = clock.next_tick();
	const sim::PredictionEntry *before = next > 0 ? ring->find(next - 1u) : nullptr;
	const sim::Vec3 old_pos = before != nullptr ? before->state.position : p_s.position;
	cl_weapons_reconcile(p_s);
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

sim::Command ClientLoop::sample_command(sim::Tick p_tick) {
	InputSample s;
	{
		std::lock_guard lock(input_mutex);
		s = input;
	}
	sim::Command c;
	c.tick = p_tick;
	c.sequence = next_sequence++;
	const bool jumped = jump_seen && s.jump_presses != last_jump_presses;
	jump_seen = true;
	last_jump_presses = s.jump_presses;
	if (s.life < own_life) {

		c.yaw = spawn_state.yaw;
		c.pitch = spawn_state.pitch;
		c.pose = spawn_state.target_pose;
		keep_pose = spawn_state.target_pose;
		return c;
	}
	if (s.free_camera) {
		c.yaw = last_made.yaw;
		c.pitch = last_made.pitch;
		c.pose = keep_pose;
		return c;
	}
	c.move_forward = sim::quantize_axis(s.move_forward);
	c.move_right = sim::quantize_axis(s.move_right);
	c.yaw = sim::quantize_yaw(s.yaw);
	c.pitch = sim::quantize_pitch(s.pitch);
	c.buttons = s.held_buttons & sim::buttons::kWireMask & ~sim::buttons::kEdgeMask;
	if (jumped) {
		c.buttons |= sim::buttons::kJump;
	}
	if (s.pose_request == PoseRequest::Set && uint8_t(s.pose) < sim::kPoseCount) {
		keep_pose = s.pose;
	}
	c.pose = keep_pose;
	return c;
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
		if (const std::optional<sim::Tick> discard = clock.buffer_report(pending_snapshot, p_now, next_sequence)) {
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
	if (have_own && c > 0 && !tdm_unspawned(c)) {

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
		cmd = sample_command(c);
		cl_command_made(cmd, p_now);
		tdm_command(cmd);
		if (base == nullptr || cmd.tick != base->state.tick + 1u) {
			++stats_.step_precondition_failures;
			assert(false && "ClientSession: prediction base missing");
		} else {
			sim::SoldierState state = base->state;
			motor.step(state, cmd);
			cl_predict_weapons(cmd, base->state, state);
			events_acc |= state.events;
			ring->put({ cmd, state, true });
			made = ring->find(c);
			last_made = cmd;

			clock.command_sent(cmd.sequence, p_now);
			link.send_commands(cmd, cl_others_ack_bits(), p_now);
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
	cl_publish_end(f, p_now);
	tdm_publish(f);
	stats_.connected = link.connected();
	clock.publish_stats(stats_);
	tracks.publish_stats(stats_);
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
	std::lock_guard lock(input_mutex);
	input = p_input;
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

void ClientLoop::ClRecords::add(uint16_t p_seq, uint32_t p_tick, bool p_main, std::span<const net::OtherSoldier> p_records) {
	ClSeq &e = ring[p_seq % kSeqs];
	if (e.valid && e.seq != p_seq) {
		if (net::seq_newer(e.seq, p_seq)) {
			return;
		}
		e.valid = false;
	}
	if (!e.valid) {
		e.valid = true;
		e.seq = p_seq;
		e.tick = p_tick;
		e.main = e.others = false;
		e.main_records.clear();
		e.others_records.clear();
		e.main_at.fill(0xFF);
		e.others_at.fill(0xFF);
		e.main_slots = {};
		e.others_slots = {};
	}
	bool &arrived = p_main ? e.main : e.others;
	if (arrived) {
		return;
	}
	arrived = true;
	std::vector<net::OtherSoldier> &records = p_main ? e.main_records : e.others_records;
	std::array<uint8_t, net::kMaxSlots> &at = p_main ? e.main_at : e.others_at;
	std::array<uint64_t, 2> &slots = p_main ? e.main_slots : e.others_slots;
	for (const net::OtherSoldier &o : p_records) {
		if (o.slot >= net::kMaxSlots || at[o.slot] != 0xFF || records.size() >= 0xFF) {
			continue;
		}
		at[o.slot] = uint8_t(records.size());
		records.push_back(o);
		set_bit(slots, o.slot);
	}
}

uint32_t ClientLoop::ClRecords::mask(uint16_t p_newest, bool p_main) const {
	uint32_t bits = 0;
	for (uint32_t i = 0; i < 32; ++i) {
		if (arrived(uint16_t(p_newest - i), p_main)) {
			bits |= 1u << i;
		}
	}
	return bits;
}

size_t ClientLoop::ClRecords::candidates(const ShotView &p_view, uint8_t p_target, std::span<net::TimedOther> r_out) const {
	size_t n = 0;
	if (p_target >= net::kMaxSlots) {
		return 0;
	}
	for (uint32_t i = 0; i < 32 && n < r_out.size(); ++i) {
		const ClSeq *e = find(uint16_t(p_view.view_seq - i));
		if (e == nullptr) {
			continue;
		}
		if (((p_view.main_bits >> i) & 1u) != 0 && e->main && e->main_at[p_target] != 0xFF) {
			r_out[n++] = { e->tick, e->main_records[e->main_at[p_target]] };
		}
		if (n < r_out.size() && ((p_view.others_bits >> i) & 1u) != 0 && e->others && e->others_at[p_target] != 0xFF) {
			r_out[n++] = { e->tick, e->others_records[e->others_at[p_target]] };
		}
	}
	return n;
}

bool ClientLoop::ClRecords::view_tick(uint16_t p_seq, uint32_t &r_tick) const {
	const ClSeq *e = find(p_seq);
	if (e == nullptr) {
		return false;
	}
	r_tick = e->tick;
	return true;
}

void ClientLoop::ClRecords::slots_in(const ShotView &p_view, std::array<uint64_t, 2> &r_slots) const {
	r_slots = {};
	for (uint32_t i = 0; i < 32; ++i) {
		const ClSeq *e = find(uint16_t(p_view.view_seq - i));
		if (e == nullptr) {
			continue;
		}
		for (size_t w = 0; w < 2; ++w) {
			if (((p_view.main_bits >> i) & 1u) != 0 && e->main) {
				r_slots[w] |= e->main_slots[w];
			}
			if (((p_view.others_bits >> i) & 1u) != 0 && e->others) {
				r_slots[w] |= e->others_slots[w];
			}
		}
	}
}

void ClientLoop::ClEmit::emitted(const sim::Command &p_command, const sim::weapons::WeaponTickOutput &p_output,
		const sim::WeaponPredictionEntry &p_after) {
	(void)p_after;
	ClientLoop &s = *m;
	const ClCommandView &v = s.cl_views[p_command.tick % kClCommandViews];
	const bool have_view = v.valid && v.tick == p_command.tick && v.sequence == p_command.sequence;
	const uint8_t own = s.slot;
	for (size_t i = 0; i < p_output.shot_count && i < p_output.shots.size(); ++i) {
		const sim::weapons::ShotEvent &shot = p_output.shots[i];
		PresentEvent e;
		e.kind = PresentKind::OwnShot;
		e.timeline = PresentTimeline::Predicted;
		e.slot = own;
		e.id = shot.shot_index;
		e.def = shot.def;
		copy3(e.pos, shot.origin);
		copy3(e.dir, shot.direction);
		e.aux = shot.weapon_slot;
		e.flags = shot.tracer ? present_flags::kTracer : uint8_t(0);
		e.seed = effect_seed(s.cl_match_seed, own, shot.shot_index);
		e.value = shot.power;
		e.extra = shot.pellets;
		e.time = shot.time;
		s.cl_push(e);
		++s.stats_.predicted_shots;
		if (!have_view) {
			++s.stats_.no_view_shots;
		} else {
			s.cl_fired = true;
			s.cl_fired_view_seq = v.view.view_seq;
			s.cl_fired_ns = s.now_ns();
		}
		if (s.cl_projectiles != nullptr) {
			s.cl_projectiles->spawn_own(shot, have_view ? v.view : ShotView{});
		}
	}
	for (size_t i = 0; i < p_output.timed_count && i < p_output.timed.size(); ++i) {
		const sim::weapons::TimedWeaponEvent &t = p_output.timed[i];
		PresentEvent e;
		e.kind = PresentKind::OwnTimed;
		e.timeline = PresentTimeline::Predicted;
		e.slot = own;
		e.aux = t.weapon_slot;
		const sim::weapons::WeaponSlotState &ws = p_after.state.slots[t.weapon_slot < sim::weapons::kMaxWeaponSlots ? t.weapon_slot : 0];
		e.def = ws.def;
		e.id = t.event;
		e.time = t.time;
		s.cl_push(e);
		++s.stats_.predicted_timed;
	}
	if (s.config.record_predicted && (p_output.shot_count > 0 || p_output.timed_count > 0)) {
		std::lock_guard lock(s.cl_log_mutex);
		for (size_t i = 0; i < p_output.shot_count && s.cl_shot_log.size() < kClLogLimit; ++i) {
			s.cl_shot_log.push_back({ p_command.tick, p_output.shots[i] });
		}
		for (size_t i = 0; i < p_output.timed_count && s.cl_timed_log.size() < kClLogLimit; ++i) {
			s.cl_timed_log.push_back({ p_command.tick, p_output.timed[i] });
		}
	}
}

void ClientLoop::cl_push(const PresentEvent &p_event) {
	PresentEvent e = p_event;
	tdm_push(e);
	if (!cl_events.push(e)) {
		++stats_.events_overflow;
	}
}

void ClientLoop::cl_refuse(const char *p_reason) {
	have_match = true;
	link.refuse(p_reason);
	stats_.connected = false;
	collect_stats();
	std::lock_guard lock(frame_mutex);
	shared_frame->stats = stats_;
}

bool ClientLoop::cl_match_info(const net::MatchInfo &p_match, uint64_t p_now) {
	stats_.slot = p_match.slot;
	if (!tdm_rules(p_match)) {
		return false;
	}
	if (p_match.weapon_hash != 0) {
		if (config.gunplay == nullptr && config.gunplay_from_level && level->gunplay != nullptr) {

			core::Result<std::shared_ptr<const ClientGunplayInputs>> own = client_gunplay_inputs(*level);
			if (!own) {
				++stats_.weapon_hash_mismatch;
				cl_refuse(("weapon table mismatch (" + own.error().message + ")").c_str());
				return false;
			}
			config.gunplay = *own;
		}
		const ClientGunplayInputs *gp = config.gunplay.get();
		if (gp == nullptr || gp->table == nullptr || gp->capsules == nullptr || !core::gameplay::capsule_pose_table_valid(*gp->capsules)) {
			++stats_.weapon_hash_mismatch;
			cl_refuse("weapon table mismatch (this client has no weapon table)");
			return false;
		}
		core::Result<std::shared_ptr<const sim::weapons::WeaponTable>> table = build_client_weapon_table(*gp, config.mechanics_cfg, p_match.weapon_flags);
		if (!table) {
			++stats_.weapon_hash_mismatch;
			cl_refuse(("weapon table mismatch (" + table.error().message + ")").c_str());
			return false;
		}
		if (client_weapon_hash(**table, *gp->capsules) != p_match.weapon_hash) {
			++stats_.weapon_hash_mismatch;
			cl_refuse("weapon table mismatch");
			return false;
		}
		if (p_match.kit_hash != 0 && p_match.kit_hash != gp->kits.hash) {
			++stats_.kit_hash_mismatch;
			cl_refuse("kit table mismatch");
			return false;
		}
		cl_table = *table;
		cl_match_seed = p_match.match_seed;
		cl_frame = gp->shot_frame;
		cl_lane = std::make_unique<sim::WeaponLane>(config.prediction_ring, cl_table, level->motor, cl_match_seed);
		cl_emit.m = this;
		cl_lane->set_sink(&cl_emit);
		ring->attach_weapons(cl_lane.get());
		ClientProjectilesConfig pc;
		pc.own_slot = p_match.slot;
		pc.max_rewind_ticks = config.own_test_max_rewind_ticks >= 0 ? uint32_t(config.own_test_max_rewind_ticks) : uint32_t(p_match.max_rewind_ticks);
		for (size_t k = 0; k < sim::kPoseCount; ++k) {
			pc.pose_eye_heights[k] = level->motor.poses[k].eye_height;
		}
		pc.extrapolate_max_ms = config.extrapolate_max_ms;
		core::Result<std::unique_ptr<ClientProjectiles>> p = ClientProjectiles::create(cl_table, gp->capsules, cl_match_seed, pc);
		if (!p) {
			cl_refuse(("weapon table mismatch (" + p.error().message + ")").c_str());
			return false;
		}
		cl_projectiles = std::move(*p);
		cl_gunplay = true;
		stats_.gunplay = true;
	}
	if (config.send_kit_select) {
		link.submit(net::kChannelReliable, net::encode_kit_select(config.kit_select), p_now);
	}
	net::Ready ready;
	ready.source_digest = level->source_digest;
	link.submit(net::kChannelReliable, net::encode_ready(ready), p_now);
	return true;
}

void ClientLoop::cl_spawned(const net::Spawned &p_spawned) {
	ClLife &l = cl_lives[p_spawned.state.life % cl_lives.size()];
	l.valid = true;
	l.life = p_spawned.state.life;
	l.kit = p_spawned.kit;
	l.team = p_spawned.team;
	if (have_own && p_spawned.state.life == own_life) {
		cl_kit = p_spawned.kit;
		cl_team = p_spawned.team;
	}
}

void ClientLoop::cl_note_arrival(uint16_t p_seq, uint64_t p_now) {

	if (cl_fired && p_now - cl_fired_ns < 1'000'000'000ull && !net::seq_newer(p_seq, cl_fired_view_seq)) {
		++stats_.late_sample;
	}
}

void ClientLoop::cl_snapshot(const net::Snapshot &p_snapshot, uint64_t p_now) {
	const bool newer = !link.have_snapshot() || net::seq_newer(p_snapshot.seq, link.newest_seq());
	cl_note_arrival(p_snapshot.seq, p_now);
	if (newer || p_snapshot.seq != link.newest_seq()) {
		tracks.reorder(p_snapshot.server_tick, p_snapshot.others, slot);
	}
	cl_records.add(p_snapshot.seq, p_snapshot.server_tick, true, p_snapshot.others);
	if (newer) {
		cl_newest_tick = p_snapshot.server_tick;
		cl_wire_valid = p_snapshot.own_weapons.has_value();
		if (cl_wire_valid) {
			cl_wire = *p_snapshot.own_weapons;
		}
		cl_wire_tick = p_snapshot.server_tick;
	}
}

bool ClientLoop::cl_slot_ok(uint32_t p_slot) {
	return p_slot < net::kMaxSlots;
}

bool ClientLoop::cl_def_ok(uint32_t p_def) const {
	return cl_table != nullptr && p_def < cl_table->defs.size();
}

void ClientLoop::cl_datagram(net::MessageType p_type, const net::NetSim::Datagram &p_d, uint64_t p_now) {
	switch (p_type) {
		case net::MessageType::SnapshotOthers: {
			core::Result<net::SnapshotOthers> o = net::decode_snapshot_others(p_d.bytes);
			if (!o) {
				++stats_.bad_gunplay_datagrams;
				return;
			}
			++stats_.snapshot_others_received;
			cl_note_arrival(o->seq, p_now);
			tracks.reorder(o->server_tick, o->others, slot);
			cl_records.add(o->seq, o->server_tick, false, o->others);
			tracks.add(o->server_tick, o->others, slot, p_now);
			return;
		}
		case net::MessageType::ShotEvents: {
			core::Result<net::ShotEvents> e = net::decode_shot_events(p_d.bytes, cl_frame);
			if (!e) {
				++stats_.bad_gunplay_datagrams;
				return;
			}
			for (const net::RemoteShot &r : e->shots) {
				if (r.slot == slot) {
					continue;
				}
				if (!cl_slot_ok(r.slot) || !cl_def_ok(r.def)) {
					++stats_.bad_gunplay_events;
					continue;
				}
				PresentEvent ev;
				ev.kind = PresentKind::RemoteShot;
				ev.timeline = PresentTimeline::Render;
				ev.slot = r.slot;
				ev.id = r.shot_index;
				ev.def = r.def;
				copy3(ev.pos, r.origin);
				copy3(ev.dir, r.direction);
				ev.flags = r.tracer ? present_flags::kTracer : uint8_t(0);
				ev.seed = effect_seed(cl_match_seed, r.slot, r.shot_index);
				ev.value = r.power;
				ev.extra = r.pellets;
				ev.time = r.time;
				cl_push(ev);
				++stats_.remote_shots;
				if (cl_projectiles != nullptr) {
					cl_projectiles->queue_remote(r);
				}
			}
			return;
		}
		case net::MessageType::CombatEvents: {
			core::Result<net::CombatEvents> e = net::decode_combat_events(p_d.bytes, cl_frame);
			if (!e) {
				++stats_.bad_gunplay_datagrams;
				return;
			}
			for (const net::HitEvent &h : e->hits) {
				if (!cl_slot_ok(h.victim) || !cl_slot_ok(h.shooter)) {
					++stats_.bad_gunplay_events;
					continue;
				}
				PresentEvent ev;
				ev.kind = PresentKind::Hit;
				const bool own_victim = h.victim == slot, own_shooter = h.shooter == slot;
				ev.timeline = own_victim || own_shooter ? PresentTimeline::Immediate : PresentTimeline::Render;
				ev.flags = uint8_t((own_victim ? present_flags::kOwnVictim : 0u) | (own_shooter ? present_flags::kOwnShooter : 0u));
				ev.slot = h.shooter;
				ev.aux = h.victim;
				ev.id = h.shot_index;
				ev.pellet = h.pellet;
				copy3(ev.pos, h.position);
				copy3(ev.dir, h.normal);
				ev.value = h.damage;
				ev.extra = std::isfinite(h.health_after) ? uint32_t(std::clamp(h.health_after * 100.0f, 0.0f, 1.0e7f)) : 0u;
				ev.material = uint32_t(h.zone);
				ev.time = h.time;
				cl_push(ev);
				++stats_.combat_hits;
			}
			for (const net::DeathEvent &d : e->deaths) {
				if (!cl_slot_ok(d.victim) || !cl_slot_ok(d.killer) || (d.def != sim::weapons::kNoWeapon && !cl_def_ok(d.def))) {
					++stats_.bad_gunplay_events;
					continue;
				}
				PresentEvent ev;
				ev.kind = PresentKind::Death;
				ev.timeline = PresentTimeline::Render;
				ev.flags = uint8_t((d.headshot ? present_flags::kHeadshot : 0u) | (d.victim == slot ? present_flags::kOwnVictim : 0u) |
						(d.killer == slot ? present_flags::kOwnShooter : 0u));
				ev.slot = d.killer;
				ev.aux = d.victim;
				ev.def = d.def;
				ev.id = d.shot_index;
				ev.material = uint32_t(d.zone);
				ev.time = d.time;
				ev.value = d.killer_health;
				ev.extra = d.respawn_tick;
				ev.seed = d.revive_until;
				cl_push(ev);
				++stats_.combat_deaths;
			}
			for (const net::DetonationWire &d : e->detonations) {
				if (!cl_slot_ok(d.shooter) || !cl_def_ok(d.def)) {
					++stats_.bad_gunplay_events;
					continue;
				}
				PresentEvent ev;
				ev.kind = PresentKind::Detonation;
				ev.timeline = PresentTimeline::Render;
				ev.flags = d.dud ? present_flags::kDud : uint8_t(0);
				ev.slot = d.shooter;
				ev.id = d.shot_index;
				ev.def = d.def;
				copy3(ev.pos, d.position);
				copy3(ev.dir, d.normal);
				ev.time = d.time;
				cl_push(ev);
				++stats_.combat_detonations;
			}
			tdm_combat(*e);
			return;
		}
		default:
			return;
	}
}

bool ClientLoop::cl_put_wire(const sim::SoldierState &p_s, sim::Tick p_spawn_tick) {
	core::Result<sim::weapons::SoldierWeaponsState> w = net::weapons_from_wire(cl_wire, p_s.tick, slot, cl_table->defs.size());
	if (!w) {
		++stats_.bad_weapon_wires;
		return false;
	}
	sim::WeaponPredictionEntry e;
	e.tick = p_s.tick;
	e.spawn_tick = p_spawn_tick;
	e.state = *w;
	cl_lane->put(e);
	return true;
}

void ClientLoop::cl_weapons_spawn(const sim::SoldierState &p_s) {
	if (!cl_gunplay) {
		return;
	}
	const ClLife &l = cl_lives[p_s.life % cl_lives.size()];
	const sim::Tick t = p_s.tick;
	cl_lane->set_emitted_through(t);
	PresentEvent spawn;
	spawn.kind = PresentKind::Spawn;
	spawn.timeline = PresentTimeline::Predicted;
	spawn.slot = slot;
	spawn.id = p_s.life;
	spawn.time = sim::weapons::tick_end(t);
	if (l.valid && l.life == p_s.life && l.team >= 1 && l.team <= kKitTeams && l.kit < kKitCount) {

		const KitLoadout &k = config.gunplay->kits.loadouts[size_t(l.team - 1)][l.kit];
		sim::WeaponPredictionEntry e;
		const sim::WeaponPredictionEntry *prev = cl_lane->newest_at_or_below(t);
		if (prev != nullptr) {
			e.state = prev->state;
		}
		e.state.player_slot = slot;
		if (sim::weapons::spawn_soldier_weapons(e.state, *cl_table, k.defs, k.active, t).ok()) {
			e.tick = t;
			e.spawn_tick = t;
			cl_lane->put(e);
		}
		cl_kit = l.kit;
		cl_team = l.team;
	} else if (cl_wire_at(t)) {
		cl_put_wire(p_s, t);
	}
	spawn.aux = cl_kit;
	spawn.extra = cl_team;
	cl_push(spawn);
}

void ClientLoop::cl_weapons_ring_miss(const sim::SoldierState &p_s) {
	if (cl_wire_at(p_s.tick)) {
		const sim::WeaponPredictionEntry *old = cl_lane->newest_at_or_below(p_s.tick);
		if (cl_put_wire(p_s, old != nullptr ? old->spawn_tick : p_s.tick)) {
			++stats_.weapon_ring_misses;
		}
	}
}

void ClientLoop::cl_weapons_reconcile(const sim::SoldierState &p_s) {
	if (!cl_wire_at(p_s.tick)) {
		return;
	}
	const sim::WeaponPredictionEntry *w = cl_lane->find(p_s.tick);
	if (w == nullptr) {
		const sim::WeaponPredictionEntry *old = cl_lane->newest_at_or_below(p_s.tick);
		if (cl_put_wire(p_s, old != nullptr ? old->spawn_tick : p_s.tick)) {
			++stats_.weapon_ring_misses;
		}
	} else if (!(net::weapons_wire_view(w->state, p_s.tick) == cl_wire)) {
		if (cl_put_wire(p_s, w->spawn_tick)) {
			++stats_.weapon_reconciliations;
		}
	}
}

void ClientLoop::cl_after_match(const sim::SoldierState &p_s) {

	bool replay = false;
	if (cl_wire_at(p_s.tick) && p_s.life != 0 && p_s.mode != sim::MoveMode::Dead) {
		const uint64_t before = stats_.weapon_reconciliations + stats_.weapon_ring_misses;
		cl_weapons_reconcile(p_s);
		replay = stats_.weapon_reconciliations + stats_.weapon_ring_misses != before;
	}
	if (replay) {
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

void ClientLoop::cl_command_made(sim::Command &r_command, uint64_t p_now) {
	InputSample s;
	{
		std::lock_guard lock(input_mutex);
		s = input;
	}
	const bool pressed = cl_fire_seen && s.fire_presses != cl_last_fire_presses;
	cl_fire_seen = true;
	cl_last_fire_presses = s.fire_presses;
	if (pressed && s.life >= own_life && !s.free_camera) {
		r_command.buttons |= sim::buttons::kFire;
	}

	double render = 0;
	sim::ViewQuality quality = sim::ViewQuality::None;
	uint16_t seq = 0;
	if (s.view_valid) {
		render = s.view_render_ticks;
		quality = sim::ViewQuality(std::min<uint8_t>(s.view_quality, uint8_t(sim::ViewQuality::Held)));
		seq = s.view_seq;
	} else if (build->serial != 0) {
		render = present_render_ticks(*build, p_now);
		quality = present_view_quality(*build, render);
		seq = build->newest_seq;
	}
	if (!link.have_snapshot() || !std::isfinite(render)) {
		quality = sim::ViewQuality::None;
	}
	ClCommandView &v = cl_views[r_command.tick % kClCommandViews];
	v = ClCommandView();
	v.valid = true;
	v.tick = r_command.tick;
	v.sequence = r_command.sequence;
	if (quality == sim::ViewQuality::None) {
		r_command.view_back_q8 = 0;
		r_command.view_quality = 0;
		r_command.view_seq8 = 0;
		return;
	}
	const double back = double(int64_t(r_command.tick) << 8) - std::round(render * 256.0);
	r_command.view_back_q8 = uint16_t(std::clamp(back, 0.0, 65535.0));
	r_command.view_quality = uint8_t(quality);
	r_command.view_seq8 = uint8_t(seq & 0xFFu);
	v.view.view_back_q8 = r_command.view_back_q8;
	v.view.quality = r_command.view_quality;

	const uint16_t back_seqs = uint16_t(link.newest_seq() - seq);
	if (back_seqs < kViewSeqWindow) {
		v.view.view_seq = seq;
		v.view.main_bits = cl_records.mask(seq, true);
		v.view.others_bits = cl_records.mask(seq, false);
	} else {
		v.valid = false;
	}
}

void ClientLoop::cl_predict_weapons(const sim::Command &p_command, const sim::SoldierState &p_before, const sim::SoldierState &p_after) {
	if (!cl_gunplay || p_command.tick == 0) {
		return;
	}
	const sim::WeaponPredictionEntry *b = cl_lane->find(p_command.tick - 1u);
	if (b == nullptr) {
		b = cl_lane->newest_at_or_below(p_command.tick - 1u);
	}
	if (b != nullptr) {
		const sim::WeaponPredictionEntry from = *b;
		cl_lane->put(cl_lane->step(from, p_command, p_before, p_after));
	}
	if (cl_projectiles != nullptr) {
		cl_projectiles->step_own(p_command.tick, *world, cl_records, tdm_own_events);
		tdm_drain_own();
	}
}

uint32_t ClientLoop::cl_others_ack_bits() const {
	return link.have_snapshot() ? cl_records.mask(link.newest_seq(), false) : 0u;
}

void ClientLoop::cl_publish_end(PresentationFrame &r_frame, uint64_t p_now) {
	(void)p_now;
	r_frame.newest_seq = link.newest_seq();
	r_frame.newest_tick = cl_newest_tick;
	r_frame.match_seed = cl_match_seed;
	r_frame.own_kit = cl_kit;
	r_frame.own_team = cl_team;
	const std::span<const net::OtherPose> poses = tracks.finish(r_frame, link.have_snapshot(), cl_newest_tick, cl_projectiles != nullptr);
	r_frame.own_weapons_valid = false;
	if (cl_gunplay && r_frame.valid && r_frame.tick > 0) {
		const sim::WeaponPredictionEntry *a = cl_lane->find(r_frame.tick - 1u);
		const sim::WeaponPredictionEntry *b = cl_lane->find(r_frame.tick);
		if (a != nullptr && b != nullptr) {
			r_frame.own_weapons_prev = net::weapons_wire_view(a->state, r_frame.tick - 1u);
			r_frame.own_weapons_curr = net::weapons_wire_view(b->state, r_frame.tick);
			r_frame.own_weapons_valid = true;
		}
	}
	if (cl_projectiles != nullptr) {
		cl_projectiles->step_remote(r_frame.render_ticks_at_publish, *world, poses, cl_events);
		cl_sample_scratch.resize(4096);
		size_t got = 0;
		while ((got = cl_projectiles->take_samples(cl_sample_scratch)) > 0) {
			std::lock_guard lock(cl_log_mutex);
			for (size_t k = 0; k < got && cl_sample_log.size() < kClLogLimit; ++k) {
				cl_sample_log.push_back(cl_sample_scratch[k]);
			}
		}
		stats_.projectiles = cl_projectiles->stats();
	}
	if (cl_lane != nullptr) {
		stats_.lane = cl_lane->stats();
	}
}

PresentationEvents &ClientLoop::events() {
	return cl_events;
}

void ClientLoop::take_predicted(std::vector<PredictedShot> &r_shots, std::vector<PredictedTimed> &r_timed) {
	std::lock_guard lock(cl_log_mutex);
	r_shots.insert(r_shots.end(), cl_shot_log.begin(), cl_shot_log.end());
	r_timed.insert(r_timed.end(), cl_timed_log.begin(), cl_timed_log.end());
	cl_shot_log.clear();
	cl_timed_log.clear();
}

void ClientLoop::take_own_test_samples(std::vector<OwnTestSample> &r_samples) {
	std::lock_guard lock(cl_log_mutex);
	r_samples.insert(r_samples.end(), cl_sample_log.begin(), cl_sample_log.end());
	cl_sample_log.clear();
}

void ClientLoop::send_kit_select(const net::KitSelect &p_select) {
	link.queue_kit_select(p_select);
}

bool ClientLoop::tdm_datagram(net::MessageType p_type, const net::NetSim::Datagram &p_d) {
	switch (p_type) {
		case net::MessageType::MatchState: {
			core::Result<net::MatchState> m = net::decode_match_state(p_d.bytes);
			if (!m) {
				++stats_.bad_tdm_datagrams;
				return true;
			}
			tdm_match = *m;
			tdm_match_valid = true;
			++stats_.match_states;
			return true;
		}
		case net::MessageType::PlayerList: {
			core::Result<net::PlayerList> l = net::decode_player_list(p_d.bytes);
			if (!l) {
				++stats_.bad_tdm_datagrams;
				return true;
			}
			apply_player_list(tdm_players, *l);
			++stats_.player_lists;
			return true;
		}
		default:
			return false;
	}
}

void ClientLoop::tdm_spawned(const net::Spawned &p_spawned) {
	uint32_t &life = tdm_revived_lives[p_spawned.state.life % tdm_revived_lives.size()];
	if (p_spawned.revived) {
		life = p_spawned.state.life;
	} else if (life == p_spawned.state.life) {
		life = 0;
	}
}

void ClientLoop::tdm_combat(const net::CombatEvents &p_events) {
	for (const net::DeathEvent &d : p_events.deaths) {
		if (d.victim == slot && have_own) {

			tdm_death_valid = true;
			tdm_death_life = own_life;
			tdm_respawn_tick = d.respawn_tick;
			tdm_revive_until = d.revive_until;
		}
	}
	for (const net::ReviveEvent &v : p_events.revives) {
		if (!cl_slot_ok(v.reviver) || !cl_slot_ok(v.revived)) {
			++stats_.bad_gunplay_events;
			continue;
		}
		PresentEvent ev;
		ev.kind = PresentKind::Revive;
		ev.timeline = PresentTimeline::Render;
		ev.slot = v.reviver;
		ev.aux = v.revived;
		ev.id = v.tick;
		ev.time = sim::weapons::tick_end(v.tick);
		ev.flags = uint8_t((v.reviver == slot ? present_flags::kOwnShooter : 0u) | (v.revived == slot ? present_flags::kOwnVictim : 0u));
		cl_push(ev);
		++stats_.revives;
	}
}

bool ClientLoop::tdm_rules(const net::MatchInfo &p_match) {
	const char *refusal = rules_refusal(p_match, level->logic != nullptr, level->logic_digest);
	if (refusal == nullptr) {
		return true;
	}
	stats_.rules_mismatch = true;
	cl_refuse(refusal);
	return false;
}

void ClientLoop::tdm_command(sim::Command &r_command) {
	if (!tdm_match_valid) {
		return;
	}
	const sim::Command r = net::restrict_command(r_command, tdm_match.restrict_until_tick, tdm_match.restrict_mask);
	if (r.move_forward != r_command.move_forward || r.move_right != r_command.move_right || r.buttons != r_command.buttons) {
		++stats_.restricted_commands;
	}
	r_command = r;
}

bool ClientLoop::tdm_unspawned(sim::Tick p_tick) {
	tdm_unspawned_now = have_own && own_unspawned_at(tdm_match_valid, tdm_match, spawn_state.tick, p_tick);
	if (tdm_unspawned_now) {
		++stats_.unspawned_ticks;
	}
	return tdm_unspawned_now;
}

void ClientLoop::tdm_push(PresentEvent &r_event) {
	present_own_shot_id(tdm_shot_ids, slot, r_event);
	if (r_event.kind == PresentKind::Spawn && r_event.slot == slot && r_event.id != 0 &&
			tdm_revived_lives[r_event.id % tdm_revived_lives.size()] == r_event.id) {
		r_event.flags |= present_flags::kRevived;
	}
}

void ClientLoop::tdm_drain_own() {
	std::array<PresentEvent, 64> batch;
	size_t got = 0;
	while ((got = tdm_own_events.drain(batch)) > 0) {
		for (size_t k = 0; k < got; ++k) {
			cl_push(batch[k]);
		}
	}
}

void ClientLoop::tdm_publish(PresentationFrame &r_frame) {
	r_frame.match_valid = tdm_match_valid;
	r_frame.match = tdm_match;
	r_frame.player_count = player_rows(tdm_players, r_frame.players);

	const bool dead_life = tdm_death_valid && have_own && own_life == tdm_death_life;
	r_frame.own_respawn_tick = dead_life ? tdm_respawn_tick : 0u;
	r_frame.own_revive_until = dead_life ? tdm_revive_until : 0u;
	r_frame.own_unspawned = tdm_unspawned_now;
	stats_.own_shot_id_resets = tdm_shot_ids.resets();
}

void ClientLoop::send_spawn_request(uint8_t p_kit) {
	link.queue_spawn_request(p_kit);
}

}
