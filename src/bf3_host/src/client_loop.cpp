#include "client_loop.h"

#include "bf3/host/game_server.h"
#include "client_handshake.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
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
			if (!m) {
				return;
			}
			if (m->phase == net::MatchPhase::LevelChange) {
				note_lobby_match(*m);
				following = true;
				return;
			}
			if (have_match) {
				return;
			}
			if (!accept_match(*m, p_now)) {
				return;
			}
			have_match = true;
			match_identity = *m;
			slot = m->slot;
			stats_.slot = m->slot;
			tracks.set_divisor(std::max<uint8_t>(m->snapshot_divisor, 1));
			prediction.match_started(p_now);
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
			core::Result<net::Snapshot> s = net::decode_snapshot(p_d.bytes, &baselines);
			if (!s) {
				return;
			}
			baselines.keep(s->seq, s->server_tick, s->own, s->own_weapons, s->others);
			for (const uint8_t r : s->removed) {
				tracks.remove(r, s->server_tick);
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
			if (first_snapshot_tick == 0) {
				first_snapshot_tick = s->server_tick;
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
			if (k) {
				note_kick(*k);
			}
			link.drop("kicked: " + (k ? k->reason : std::string("(bad kick)")));
			return;
		}
		case net::MessageType::Bye:
			link.drop("the server said bye");
			return;
		case net::MessageType::LevelChange:
			if (core::Result<net::LevelChange> c = net::decode_level_change(p_d.bytes)) {
				note_level_change(*c);
				following = true;
			}
			return;
		case net::MessageType::RoundOverInfo:
			note_round_over(p_d);
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
	f.first_snapshot_tick = first_snapshot_tick;
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
	net_meter.sample({ &stats_.traffic_in, &stats_.traffic_out, stats_.snapshots_received, stats_.snapshots_lost,
							 stats_.rtt_ms, f.other_count, stats_.last_server_tick, stats_.own_hits },
			p_now, net_stats);
	f.net = net_stats;
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
		if (level != nullptr && !following) {
			handle_datagram(d, now);
		} else {
			hold(std::move(d), now);
		}
	}
	link.send_requests(now);
	send_load_report(now);
	send_prepare_report(now);
	link.release(now);
	if (mismatch_deadline && now >= *mismatch_deadline) {
		mismatch_deadline.reset();
		if (link.connected()) {
			refuse((install_difference_text(stats_.mismatch_components) + " (the server sent no kick)").c_str());
		}
	}
	if (level == nullptr || following) {
		publish_link_stats();
		return;
	}
	if (clock.running() && link.connected()) {
		if (clock.due(now)) {
			client_tick(now);
		}
		return;
	}
	publish_link_stats();
}

void ClientLoop::shutdown() {
	link.shutdown();
}

core::Status ClientLoop::init(const ClientSessionConfig &p_config, std::shared_ptr<const SharedLevel> p_level) {
	configure(p_config);
	return attach_level(std::move(p_level));
}

void ClientLoop::configure(const ClientSessionConfig &p_config) {
	config = p_config;
	config.prediction_ring = std::clamp<uint32_t>(p_config.prediction_ring, 16u, 4096u);
	config.max_buffer_ticks = std::max(p_config.max_buffer_ticks, p_config.target_buffer_ticks);
	link.configure(p_config.netsim);
}

core::Status ClientLoop::attach_level(std::shared_ptr<const SharedLevel> p_level) {
	if (level != nullptr) {
		return core::make_error("ClientSession: a level is already attached");
	}
	if (p_level == nullptr || p_level->static_world == nullptr) {
		return core::make_error("ClientSession: no level");
	}
	{
		std::lock_guard lock(lobby_mutex);
		if (lobby_change &&
				!level_refusal(net::MatchInfo{ .level = lobby_change->level, .mode = lobby_change->mode },
						p_level->level, p_level->mode)
						.empty()) {
			return core::make_error("ClientSession: " + std::string(kAttachStale) + " to " + lobby_change->level + " " +
					lobby_change->mode + ", " + p_level->level + " " + p_level->mode + " is stale");
		}
	}
	core::Result<std::unique_ptr<sim::CollisionWorld>> created = sim::CollisionWorld::create(p_level->static_world);
	if (!created) {
		return core::error_at("ClientSession", created.error());
	}
	world = std::move(*created);
	BF3_TRY(prediction.init(*world, p_level->motor));
	level = std::move(p_level);
	following = false;
	std::vector<HeldDatagram> replay;
	replay.swap(held);
	for (const HeldDatagram &h : replay) {
		handle_datagram(h.datagram, h.at);
	}
	return core::Status::success();
}

void ClientLoop::configure_level(const ClientLevelConfig &p_config) {
	config.gunplay = p_config.gunplay;
	config.send_kit_select = p_config.send_kit_select;
	config.kit_select = p_config.kit_select;
	config.record_predicted = p_config.record_predicted;
}

core::Status ClientLoop::detach_level() {
	reset_level_state();
	following = false;
	return core::Status::success();
}

void ClientLoop::reset_level_state() {
	std::destroy_at(&prediction);
	std::destroy_at(&gunplay);
	std::destroy_at(&match);
	std::destroy_at(&feed);
	std::destroy_at(&tracks);
	std::destroy_at(&input);
	std::destroy_at(&clock);
	std::construct_at(&clock, config);
	std::construct_at(&input);
	std::construct_at(&tracks, config);
	std::construct_at(&feed, slot);
	std::construct_at(&match, feed, slot);
	std::construct_at(&gunplay, config, feed, slot, now_ns);
	std::construct_at(&prediction, config, clock, input, gunplay);
	world.reset();
	level.reset();
	have_match = false;
	pending_snapshot_valid = false;
	pending_snapshot = net::Snapshot();
	first_snapshot_tick = 0;
	pending_spawned.clear();
	baselines = net::SnapshotBaselines();
	mismatch_deadline.reset();
	link.new_level();
	*build = PresentationFrame();
	ClientStats fresh;
	fresh.connected = stats_.connected;
	fresh.slot = stats_.slot;
	fresh.level_changes = stats_.level_changes;
	fresh.ready_sent = stats_.ready_sent;
	fresh.loading_sent = stats_.loading_sent;
	fresh.round_overs = stats_.round_overs;
	fresh.prepare_sent = stats_.prepare_sent;
	fresh.kicked = stats_.kicked;
	fresh.kick_reason = stats_.kick_reason;
	fresh.kick_detail = stats_.kick_detail;
	fresh.mismatch_components = stats_.mismatch_components;
	stats_ = fresh;
	link.stats(stats_);
	std::lock_guard lock(frame_mutex);
	*shared_frame = PresentationFrame();
	shared_frame->serial = ++serial;
	shared_frame->stats = stats_;
}

void ClientLoop::note_level_change(const net::LevelChange &p_change) {
	bool counted = false;
	{
		std::lock_guard lock(lobby_mutex);
		counted = !lobby_change || lobby_change->round_id != p_change.round_id;
		lobby_change = p_change;
		lobby_info.reset();
		if (counted) {
			load_report.reset();
		}
	}
	stats_.level_changes += counted ? 1u : 0u;
	load_report_sent_ns = 0;
	held.clear();
}

void ClientLoop::note_lobby_match(const net::MatchInfo &p_match) {
	std::lock_guard lock(lobby_mutex);
	lobby_info = p_match;
}

void ClientLoop::hold(net::NetSim::Datagram &&p_d, uint64_t p_now) {
	const std::optional<net::MessageType> type = net::peek_type(p_d.bytes);
	if (type == net::MessageType::Kick || type == net::MessageType::Bye) {
		handle_datagram(p_d, p_now);
		return;
	}
	if (!p_d.reliable || type == net::MessageType::CombatEvents || type == net::MessageType::ShotEvents) {
		return;
	}
	if (type == net::MessageType::LevelChange) {
		if (core::Result<net::LevelChange> c = net::decode_level_change(p_d.bytes)) {
			note_level_change(*c);
			following = following || level != nullptr;
		}
		return;
	}
	if (type == net::MessageType::MatchInfo) {
		core::Result<net::MatchInfo> m = net::decode_match_info(p_d.bytes);
		if (!m) {
			return;
		}
		note_lobby_match(*m);
		held.clear();
		if (m->phase == net::MatchPhase::LevelChange) {
			following = following || level != nullptr;
			return;
		}
	} else {
		std::lock_guard lock(lobby_mutex);
		if (!lobby_info || lobby_info->phase == net::MatchPhase::LevelChange) {
			return;
		}
	}
	if (held.size() >= kMaxHeldDatagrams) {
		link.drop("more than " + std::to_string(kMaxHeldDatagrams) + " reliable datagrams before a level was attached");
		return;
	}
	held.push_back({ std::move(p_d), p_now });
}

void ClientLoop::send_load_report(uint64_t p_now) {
	if (!link.connected() || (have_match && level != nullptr && !following) ||
			p_now < load_report_sent_ns + kLoadReportNs) {
		return;
	}
	net::Loading report;
	{
		std::lock_guard lock(lobby_mutex);
		if (!load_report || (!lobby_info && !lobby_change)) {
			return;
		}
		report = *load_report;
		report.round_id = lobby_info ? lobby_info->round_id : lobby_change->round_id;
	}
	link.submit(net::kChannelReliable, net::encode_loading(report), p_now);
	load_report_sent_ns = p_now;
	++stats_.loading_sent;
}

void ClientLoop::set_load_progress(const net::Loading &p_progress) {
	std::lock_guard lock(lobby_mutex);
	if (lobby_change && lobby_change->round_id != p_progress.round_id) {
		return;
	}
	load_report = p_progress;
}

void ClientLoop::note_round_over(const net::NetSim::Datagram &p_d) {
	core::Result<net::RoundOverInfo> r = net::decode_round_over_info(p_d.bytes);
	if (!r) {
		++stats_.bad_tdm_datagrams;
		return;
	}
	++stats_.round_overs;
	std::lock_guard lock(lobby_mutex);
	ClientRoundOver o;
	o.info = std::move(*r);
	o.round_id = match_identity.round_id;
	o.level = match_identity.level;
	o.mode = match_identity.mode;
	o.serial = stats_.round_overs;
	round_over_info = std::move(o);
}

bool ClientLoop::round_over(ClientRoundOver &r_round_over) const {
	std::lock_guard lock(lobby_mutex);
	if (!round_over_info) {
		return false;
	}
	r_round_over = *round_over_info;
	return true;
}

void ClientLoop::set_prepare_progress(const net::Prepare &p_progress) {
	std::lock_guard lock(lobby_mutex);
	prepare_report = p_progress;
}

void ClientLoop::send_prepare_report(uint64_t p_now) {
	if (!link.connected()) {
		return;
	}
	net::Prepare report;
	{
		std::lock_guard lock(lobby_mutex);
		if (!prepare_report) {
			return;
		}
		report = *prepare_report;
	}
	const bool same_state = prepare_last && prepare_last->round_id == report.round_id &&
			prepare_last->state == report.state && prepare_last->next_source_digest == report.next_source_digest;
	if (same_state && (prepare_last->permille == report.permille || p_now < prepare_sent_ns + kLoadReportNs)) {
		return;
	}
	link.submit(net::kChannelReliable, net::encode_prepare(report), p_now);
	prepare_last = report;
	prepare_sent_ns = p_now;
	++stats_.prepare_sent;
}

bool ClientLoop::level_change(net::LevelChange &r_change) const {
	std::lock_guard lock(lobby_mutex);
	if (!lobby_change) {
		return false;
	}
	r_change = *lobby_change;
	return true;
}

void ClientLoop::publish_link_stats() {
	stats_.connected = link.connected();
	link.stats(stats_);
	std::lock_guard lock(frame_mutex);
	shared_frame->stats = stats_;
}

bool ClientLoop::lobby_match(net::MatchInfo &r_match) const {
	std::lock_guard lock(lobby_mutex);
	if (!lobby_info) {
		return false;
	}
	r_match = *lobby_info;
	return true;
}

void ClientLoop::begin(std::unique_ptr<ClientTransport> p_transport, MonotonicClock p_clock) {
	now_ns = std::move(p_clock);
	link.attach(std::move(p_transport));
	stats_ = ClientStats();
	stats_.connected = true;
	const uint64_t now = now_ns();
	net::Hello hello;
	hello.game_build = config.game_build.value_or(net::game_build());
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

void ClientLoop::send_mismatch(uint8_t p_components, uint64_t p_now) {
	have_match = true;
	stats_.mismatch_components = p_components;
	link.submit(net::kChannelReliable, net::encode_mismatch({ p_components }), p_now);
	mismatch_deadline = p_now + uint64_t(config.connect_timeout_ms) * 1'000'000ull;
	collect_stats();
}

void ClientLoop::note_kick(const net::Kick &p_kick) {
	if (stats_.kicked || !link.connected()) {
		return;
	}
	stats_.kicked = true;
	stats_.kick_reason = p_kick.code;
	const size_t n = std::min(p_kick.reason.size(), stats_.kick_detail.size() - 1);
	std::memcpy(stats_.kick_detail.data(), p_kick.reason.data(), n);
	stats_.kick_detail[n] = '\0';
}

bool ClientLoop::accept_match(const net::MatchInfo &p_match, uint64_t p_now) {
	stats_.slot = p_match.slot;
	if (const std::string refusal = match_refusal(p_match, *level); !refusal.empty()) {
		refuse(refusal.c_str());
		return false;
	}
	uint8_t refused = prediction.motor_matches(p_match.motor_hash) ? 0 : net::mismatch_bits::kMotor;
	if (match.check_rules(p_match, *level) != nullptr) {
		refused |= net::mismatch_bits::kLogic;
	}
	const std::optional<GunplayRefusal> gunplay_refusal = gunplay.match_info(p_match, *level);
	if (gunplay.lane() != nullptr) {
		prediction.attach_weapons(gunplay.lane());
	}
	if (gunplay_refusal) {
		refused |= gunplay_refusal->component;
	}
	const net::ComponentDigests own = client_digests(p_match, *level, prediction.motor_hash());
	if (const uint8_t mismatch = client_mismatch(p_match, *level, own, refused); mismatch != 0) {
		send_mismatch(mismatch, p_now);
		return false;
	}
	if (config.send_kit_select) {
		link.submit(net::kChannelReliable, net::encode_kit_select(config.kit_select), p_now);
	}
	net::Ready ready;
	ready.source_digest = level->source_digest;
	ready.digests = own;
	link.submit(net::kChannelReliable, net::encode_ready(ready), p_now);
	++stats_.ready_sent;
	{
		std::lock_guard lock(lobby_mutex);
		load_report.reset();
	}
	return true;
}

void ClientLoop::gunplay_datagram(net::MessageType p_type, const net::NetSim::Datagram &p_d, uint64_t p_now) {
	switch (p_type) {
		case net::MessageType::SnapshotOthers: {
			core::Result<net::SnapshotOthers> o = net::decode_snapshot_others(p_d.bytes, &baselines);
			if (!o) {
				gunplay.bad_datagram();
				return;
			}
			baselines.keep(o->seq, o->server_tick, std::nullopt, std::nullopt, o->others);
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

void ClientLoop::send_spawn_request(uint8_t p_kit, uint8_t p_spawn_choice) {
	link.queue_spawn_request(p_kit, p_spawn_choice);
}

void ClientLoop::send_squad_request(const net::SquadRequest &p_request) {
	link.queue_squad_request(p_request);
}

void ClientLoop::send_suicide_request() {
	link.queue_suicide_request();
}

void ClientLoop::send_loadout_request(const net::LoadoutRequest &p_request) {
	link.queue_loadout_request(p_request);
}
}
