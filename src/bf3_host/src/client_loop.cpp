#include "client_loop.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

namespace bf3::host {

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
			prediction.match_started(m->motor_hash, p_now);
			clock.start(m->server_tick, p_now, input.next_sequence());
			return;
		}
		case net::MessageType::Spawned: {
			core::Result<net::Spawned> s = net::decode_spawned(p_d.bytes);
			if (s && pending_spawned.size() < 16) {
				gunplay.spawned(*s, prediction.have_own(), prediction.own_life());
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
			prediction.note_server_events(s->own_events);
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

void ClientLoop::client_tick(uint64_t p_now) {
	prediction.decay(p_now);
	if (pending_snapshot_valid && pending_snapshot.own.has_value() && prediction.have_own()) {
		if (const std::optional<sim::Tick> discard = clock.buffer_report(pending_snapshot, p_now, input.next_sequence())) {
			prediction.discard_after(*discard);
		}
	}
	for (const sim::SoldierState &s : pending_spawned) {
		prediction.authoritative(s, p_now);
	}
	pending_spawned.clear();
	if (pending_snapshot_valid) {
		if (pending_snapshot.own.has_value()) {
			prediction.authoritative(*pending_snapshot.own, p_now);
		}
		pending_snapshot_valid = false;
	}
	const sim::Tick c = clock.next_tick();
	const sim::PredictionEntry *made = nullptr;
	sim::Command cmd;
	if (prediction.have_own() && c > 0 && !match.unspawned(c, prediction.have_own(), prediction.spawn_state().tick)) {
		prediction.fill_base(c);
		const sim::PredictionEntry *base = prediction.entry(c - 1u);
		cmd = input.sample(c, prediction.own_life(), prediction.spawn_state());
		gunplay.command_made(cmd, input.latest(), prediction.own_life(), p_now, link.have_snapshot(), link.newest_seq(), *build);
		match.restrict(cmd);
		if (prediction.step(base, cmd)) {
			made = prediction.entry(c);
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
	prediction.publish(f, p_made, p_cmd, p_now);
	f.extrapolate_max_ms = config.extrapolate_max_ms;
	for (size_t k = 0; k < sim::kPoseCount; ++k) {
		f.pose_eye_heights[k] = level->motor.poses[k].eye_height;
	}
	tracks.publish(f, p_now);
	f.newest_seq = link.newest_seq();
	gunplay.publish(f, tracks.finish(f, link.have_snapshot(), gunplay.newest_tick(), gunplay.wants_poses()), *world);
	match.publish(f, prediction.have_own(), prediction.own_life());
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
	prediction.stats(stats_);
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
	BF3_TRY(prediction.init(*world, p_level->motor));
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
		prediction.attach_weapons(gunplay.lane());
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
				match.combat(e, prediction.have_own(), prediction.own_life());
			}
			return;
		}
		default:
			return;
	}
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

void ClientLoop::send_suicide_request() {
	link.queue_suicide_request();
}
}
