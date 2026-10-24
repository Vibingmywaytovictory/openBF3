#include "server_snapshots.h"

#include "bf3/core/util/elapsed.h"

#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <utility>

namespace bf3::host {

namespace {

float distance2(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const float dx = p_a[0] - p_b[0], dy = p_a[1] - p_b[1], dz = p_a[2] - p_b[2];
	return dx * dx + dy * dy + dz * dz;
}

}

void SnapshotWriter::send(sim::Tick p_tick, uint64_t p_now_ns, GameServerTickTimes &r_times) {
	const auto start = std::chrono::steady_clock::now();
	const uint64_t sent_before = sent_;
	combat_.record_tick(p_tick, slots_, soldiers_);
	std::vector<std::pair<float, size_t>> order;
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
		order.clear();
		const sim::Vec3 at = own.spawned ? own.state.position : sim::Vec3{};
		for (size_t j = 0; j < slots_.size(); ++j) {
			if (j != i && slots_[j].used && soldiers_.alive(j)) {
				order.push_back({ distance2(at, soldiers_[j].state.position), j });
			}
		}
		std::sort(order.begin(), order.end());
		snap.others.reserve(order.size());
		for (const auto &[d, j] : order) {
			snap.others.push_back(net::quantize_other(uint8_t(j), soldiers_[j].state));
		}
		combat_.fill_snapshot(i, snap);

		size_t dropped = 0;
		std::vector<uint8_t> bytes = net::encode_snapshot(snap, &dropped);
		split_overflow(snap, dropped);
		if (bytes.empty()) {
			continue;
		}
		++sent_;
		bytes_ += bytes.size();
		max_bytes_ = std::max(max_bytes_, bytes.size());
		outbox_.send(s.peer, net::kChannelUnreliable, std::move(bytes));
		send_extras(i, snap);
	}
	log_.clear();
	if (combat_.on()) {
		combat_.gunplay()->end_snapshot_interval();
	}
	last_encode_ms_ = core::ms_since(start);
	r_times.encode_ms = last_encode_ms_;
	r_times.snapshots = uint32_t(sent_ - sent_before);
}

void SnapshotWriter::split_overflow(net::Snapshot &r_snapshot, size_t p_dropped) {
	if (!combat_.on()) {
		return;
	}
	overflow_.clear();
	if (p_dropped > 0 && p_dropped <= r_snapshot.others.size()) {
		const size_t keep = r_snapshot.others.size() - p_dropped;
		overflow_.assign(r_snapshot.others.begin() + ptrdiff_t(keep), r_snapshot.others.end());
		r_snapshot.others.resize(keep);
	}
	main_mask_ = 0;
	for (const net::OtherSoldier &o : r_snapshot.others) {
		if (o.slot < 64) {
			main_mask_ |= uint64_t(1) << o.slot;
		}
	}
}

void SnapshotWriter::send_extras(size_t p_slot, const net::Snapshot &p_snapshot) {
	if (!combat_.on()) {
		return;
	}
	Gunplay &g = *combat_.gunplay();
	const Seat &s = slots_[p_slot];
	uint64_t others_mask = 0;
	if (!overflow_.empty()) {
		net::SnapshotOthers so;
		so.seq = p_snapshot.seq;
		so.server_tick = p_snapshot.server_tick;
		so.others = overflow_;
		size_t dropped = 0;
		std::vector<uint8_t> bytes = net::encode_snapshot_others(so, &dropped);
		g.count_others_dropped(dropped);
		if (!bytes.empty()) {
			const size_t sent = so.others.size() - std::min(dropped, so.others.size());
			for (size_t k = 0; k < sent; ++k) {
				if (so.others[k].slot < 64) {
					others_mask |= uint64_t(1) << so.others[k].slot;
				}
			}
			outbox_.send(s.peer, net::kChannelUnreliable, std::move(bytes));
		}
	}

	g.history().sent(uint8_t(p_slot), p_snapshot.seq, p_snapshot.server_tick, main_mask_, others_mask);

	remote_.clear();
	for (const sim::weapons::ShotEvent &e : g.interval_shots()) {
		if (e.player_slot != p_slot) {
			remote_.push_back(net::remote_shot_of(e));
		}
	}
	if (!remote_.empty()) {
		send_shots(p_slot, p_snapshot, g);
	}

	log_.send(s.peer, p_snapshot.server_tick, g, outbox_);
}

void SnapshotWriter::send_shots(size_t p_slot, const net::Snapshot &p_snapshot, Gunplay &r_gunplay) {
	const Soldier &own = soldiers_[p_slot];
	const uint32_t peer = slots_[p_slot].peer;
	sim::Vec3 eye = own.state.position;
	eye[1] += own.spawned ? motor_.eye_height(own.state) : 0.0f;
	for (int part = 0; part < 2; ++part) {
		net::ShotEvents ev;
		ev.server_tick = p_snapshot.server_tick;
		ev.shots = remote_;
		size_t dropped = 0;
		std::vector<uint8_t> bytes = net::encode_shot_events(ev, combat_.frame(), eye, &dropped);
		if (dropped == 0 || part == 1 || bytes.empty()) {
			r_gunplay.count_shot_events_dropped(dropped);
			if (!bytes.empty()) {
				outbox_.send(peer, net::kChannelUnreliable, std::move(bytes));
			}
			break;
		}
		core::Result<net::ShotEvents> first = net::decode_shot_events(bytes, combat_.frame());
		outbox_.send(peer, net::kChannelUnreliable, std::move(bytes));
		if (!first) {
			r_gunplay.count_shot_events_dropped(dropped);
			break;
		}
		std::unordered_set<uint64_t> carried;
		for (const net::RemoteShot &r : first->shots) {
			carried.insert((uint64_t(r.slot) << 32) | r.shot_index);
		}
		std::vector<net::RemoteShot> rest;
		for (const net::RemoteShot &r : remote_) {
			if (carried.find((uint64_t(r.slot) << 32) | r.shot_index) == carried.end()) {
				rest.push_back(r);
			}
		}
		if (rest.empty()) {
			break;
		}
		remote_ = std::move(rest);
	}
}

void SnapshotWriter::stats(GameServerStats &r_stats) const {
	r_stats.snapshots_sent = sent_;
	r_stats.snapshot_bytes = bytes_;
	r_stats.max_snapshot_bytes = max_bytes_;
	r_stats.last_encode_ms = last_encode_ms_;
}

}
