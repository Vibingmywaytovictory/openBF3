#include "server_snapshots.h"

#include "bf3/core/util/elapsed.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/net/enet_host.h"
#include "bf3/sim/weapons/spread_recoil.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace bf3::host {

namespace {

int64_t wire_bytes(size_t p_payload) {
	return p_payload == 0 ? 0 : int64_t(net::enet_command_bytes(p_payload, net::Host::SendMode::Unsequenced));
}

int64_t command_header() {
	return wire_bytes(1) - 1;
}

uint64_t slot_mask(std::span<const net::OtherSoldier> p_others) {
	uint64_t mask = 0;
	for (const net::OtherSoldier &o : p_others) {
		if (o.slot < 64) {
			mask |= uint64_t(1) << o.slot;
		}
	}
	return mask;
}

}

void SnapshotWriter::gather(sim::Tick p_tick) {
	combat_.record_tick(p_tick, slots_, soldiers_);
	for (const net::HitEvent &h : log_.events().hits) {
		ghosts_.attacked(h.victim, h.shooter, p_tick);
	}
	net::Snapshot all;
	for (size_t j = 0; j < slots_.size(); ++j) {
		if (slots_[j].used && soldiers_.alive(j)) {
			all.others.push_back(net::quantize_other(uint8_t(j), soldiers_[j].state));
		}
	}
	combat_.fill_snapshot(0, all);
	candidates_.clear();
	candidate_of_.assign(slots_.size(), -1);
	for (const net::OtherSoldier &o : all.others) {
		GhostCandidate c;
		c.slot = o.slot;
		c.team = uint8_t(std::clamp(slots_[o.slot].team, 0, 255));
		c.position = soldiers_[o.slot].state.position;
		c.record = o;
		candidate_of_[o.slot] = int(candidates_.size());
		candidates_.push_back(c);
	}
}

GhostViewer SnapshotWriter::viewer(size_t p_slot) const {
	GhostViewer v;
	const Soldier &own = soldiers_[p_slot];
	v.slot = uint8_t(p_slot);
	v.team = uint8_t(std::clamp(slots_[p_slot].team, 0, 255));
	v.spawned = own.spawned;
	if (own.spawned) {
		v.position = own.state.position;
		v.eye = own.state.position;
		v.eye[1] += motor_.eye_height(own.state);
		v.aim = sim::weapons::aim_direction(own.state.yaw, own.state.pitch, {});
	}
	if (candidate_of_[p_slot] >= 0) {
		v.zoomed = candidates_[size_t(candidate_of_[p_slot])].record.ads;
	}
	return v;
}

void SnapshotWriter::send(sim::Tick p_tick, uint64_t p_now_ns, GameServerTickTimes &r_times) {
	const auto start = std::chrono::steady_clock::now();
	const uint64_t sent_before = sent_;
	gather(p_tick);
	const TdmRules *rules = match_.rules();
	std::vector<net::OtherSoldier> overflow;
	for (size_t i = 0; i < slots_.size(); ++i) {
		const Seat &s = slots_[i];
		if (!s.used || !s.remote) {
			continue;
		}
		net::Snapshot snap;
		snap.seq = seq_[i]++;
		snap.server_tick = p_tick;
		commands_.stamp(i, p_tick, p_now_ns, snap);
		snap.own_events = soldiers_.take_events(i);
		const Soldier &own = soldiers_[i];
		if (own.spawned) {
			snap.own = own.state;
		}
		combat_.fill_snapshot(i, snap);

		const int64_t budget = ghosts_.open_interval(i, outbox_.sent_bytes(s.peer));
		std::vector<GhostCandidate> &cands = candidates_;
		for (GhostCandidate &c : cands) {
			c.squad_mate = rules != nullptr && c.slot != i && rules->squads().same_squad(uint8_t(i), c.slot);
		}
		ghosts_.plan(i, viewer(i), cands, snap.seq, p_tick, plan_);
		snap.others = plan_.others;
		snap.removed = plan_.removed;

		size_t shot_cut = 0;
		std::vector<uint8_t> wanted = shot_events(i, snap, net::kMaxDatagramBytes, shot_cut);
		const int64_t reserve = std::min(wire_bytes(wanted.size()), budget * 2 / 5);
		const int64_t snapshot_room = std::max<int64_t>(budget - reserve - command_header(), 0);

		net::SnapshotSize size;
		size_t dropped = 0;
		std::vector<uint8_t> bytes = net::encode_snapshot(snap, &dropped, &plan_.bases, size_t(snapshot_room), &size);
		if (bytes.empty()) {
			continue;
		}
		const size_t carried = size.carried;
		overflow.clear();
		const size_t extra = send_overflow(i, snap, carried, budget - reserve - wire_bytes(bytes.size()), overflow);
		const std::span<const net::OtherSoldier> main(snap.others.data(), carried);
		ghosts_.sent(i, snap, main, overflow);
		++sent_;
		bytes_ += bytes.size();
		max_bytes_ = std::max(max_bytes_, bytes.size());
		const int64_t used = wire_bytes(bytes.size()) + wire_bytes(extra);
		outbox_.send(s.peer, net::kChannelUnreliable, std::move(bytes));
		send_extras(i, snap, main, overflow);
		if (combat_.on()) {
			Gunplay &g = *combat_.gunplay();
			send_shots(i, snap, std::move(wanted), shot_cut, budget - used, g);
			log_.send(s.peer, snap.server_tick, g, outbox_);
		}
	}
	log_.clear();
	if (combat_.on()) {
		combat_.gunplay()->end_snapshot_interval();
	}
	last_encode_ms_ = core::ms_since(start);
	r_times.encode_ms = last_encode_ms_;
	r_times.snapshots = uint32_t(sent_ - sent_before);
}

size_t SnapshotWriter::send_overflow(size_t p_slot, const net::Snapshot &p_snapshot, size_t p_carried, int64_t p_room,
		std::vector<net::OtherSoldier> &r_sent) {
	const size_t required = std::min(plan_.bases.required, p_snapshot.others.size());
	const size_t mandatory = required > p_carried ? required - p_carried : 0;
	const int64_t payload = p_room - command_header();
	if (p_carried >= p_snapshot.others.size() || (mandatory == 0 && payload <= 0)) {
		return 0;
	}
	net::SnapshotOthers so;
	so.seq = p_snapshot.seq;
	so.server_tick = p_snapshot.server_tick;
	so.others.assign(p_snapshot.others.begin() + ptrdiff_t(p_carried), p_snapshot.others.end());
	const std::span<const net::OtherBaseline> bases(
			plan_.bases.others.data() + p_carried, p_snapshot.others.size() - p_carried);
	net::SnapshotSize size;
	std::vector<uint8_t> bytes =
			net::encode_snapshot_others(so, nullptr, bases, &size, size_t(std::max<int64_t>(payload, 0)), mandatory);
	if (combat_.on()) {
		combat_.gunplay()->count_others_dropped(mandatory > size.carried ? mandatory - size.carried : 0);
	}
	if (bytes.empty() || size.carried == 0) {
		return 0;
	}
	r_sent.assign(so.others.begin(), so.others.begin() + ptrdiff_t(size.carried));
	const size_t n = bytes.size();
	outbox_.send(slots_[p_slot].peer, net::kChannelUnreliable, std::move(bytes));
	return n;
}

void SnapshotWriter::send_extras(size_t p_slot, const net::Snapshot &p_snapshot,
		std::span<const net::OtherSoldier> p_main, std::span<const net::OtherSoldier> p_others) {
	if (!combat_.on()) {
		return;
	}
	combat_.gunplay()->history().sent(
			uint8_t(p_slot), p_snapshot.seq, p_snapshot.server_tick, slot_mask(p_main), slot_mask(p_others));
}

std::vector<uint8_t> SnapshotWriter::shot_events(
		size_t p_slot, const net::Snapshot &p_snapshot, size_t p_max_bytes, size_t &r_cut) const {
	r_cut = 0;
	if (!combat_.on()) {
		return {};
	}
	const Gunplay &g = *combat_.gunplay();
	std::vector<bool> scoped(slots_.size(), false);
	for (const uint8_t slot : plan_.in_scope) {
		scoped[slot] = true;
	}
	const GhostViewer v = viewer(p_slot);
	net::ShotEvents ev;
	ev.server_tick = p_snapshot.server_tick;
	for (const sim::weapons::ShotEvent &e : g.interval_shots()) {
		if (e.player_slot == p_slot) {
			continue;
		}
		if (e.player_slot < scoped.size() && scoped[e.player_slot]) {
			ev.shots.push_back(net::remote_shot_of(e));
		}
	}
	if (ev.shots.empty()) {
		return {};
	}
	std::vector<uint8_t> bytes = net::encode_shot_events(ev, combat_.frame(), v.eye, &r_cut, p_max_bytes);
	if (r_cut >= ev.shots.size()) {
		return {};
	}
	return bytes;
}

void SnapshotWriter::send_shots(size_t p_slot, const net::Snapshot &p_snapshot, std::vector<uint8_t> p_wanted,
		size_t p_cut, int64_t p_room, Gunplay &r_gunplay) {
	std::vector<uint8_t> bytes = std::move(p_wanted);
	size_t cut = p_cut;
	if (!bytes.empty() && wire_bytes(bytes.size()) > p_room) {
		const int64_t payload = std::max<int64_t>(p_room - command_header(), 0);
		bytes = shot_events(p_slot, p_snapshot, size_t(payload), cut);
	}
	r_gunplay.count_shot_events_dropped(cut);
	if (!bytes.empty()) {
		outbox_.send(slots_[p_slot].peer, net::kChannelUnreliable, std::move(bytes));
	}
}

void SnapshotWriter::stats(GameServerStats &r_stats) const {
	r_stats.snapshots_sent = sent_;
	r_stats.snapshot_bytes = bytes_;
	r_stats.max_snapshot_bytes = max_bytes_;
	r_stats.last_encode_ms = last_encode_ms_;
}

}
