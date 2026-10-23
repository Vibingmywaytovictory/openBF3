#include "server_dev.h"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

namespace bf3::host {

namespace {

constexpr uint32_t kMaxSweepTicks = 600;

constexpr size_t kMaxQueuedDebug = 64;

}

void DevCommands::configure(const GameServerConfig &p_config, const sim::CollisionWorld &p_world, size_t p_slots) {
	config_ = &p_config;
	world_ = &p_world;
	per_slot_.resize(p_slots);
}

void DevCommands::client_left(size_t p_slot) {
	if (sweep_.active && sweep_.requester == uint32_t(p_slot)) {
		sweep_.requester = UINT32_MAX;
	}
}

void DevCommands::report(uint32_t p_slot, uint32_t p_id, bool p_ok, std::string p_text) {
	if (p_slot >= slots_.size()) {
		return;
	}
	const ServerSlot &s = slots_[p_slot];
	if (!s.used || !s.remote || outbox_.replies_full()) {
		return;
	}
	net::DebugReport r;
	r.id = p_id;
	r.ok = p_ok;
	r.text = std::move(p_text);
	outbox_.send(s.peer, net::kChannelReliable, net::encode_debug_report(r));
}

void DevCommands::handle_debug(size_t p_slot, const net::DebugRequest &p_request) {
	if (!config_->dev_commands) {
		report(uint32_t(p_slot), p_request.id, false, "dev commands are disabled on this server");
		return;
	}
	PerSlot &s = per_slot_[p_slot];
	switch (p_request.kind) {
		case net::DebugKind::Trace:
			if (p_request.ticks == 0 || p_request.ticks > net::kMaxTraceTicks) {
				report(uint32_t(p_slot), p_request.id, false, "trace: ticks must be 1.." + std::to_string(net::kMaxTraceTicks));
				return;
			}
			if (s.tracing) {
				report(uint32_t(p_slot), p_request.id, false, "trace: one is already running");
				return;
			}
			s.tracing = true;
			s.trace_id = p_request.id;
			s.trace_ticks = p_request.ticks;
			s.trace.clear();
			s.trace.reserve(p_request.ticks);
			return;
		case net::DebugKind::SetSeed:
			spawner_.seed(uint64_t(p_request.index) * 0x9E3779B97F4A7C15ull + 1u);
			report(uint32_t(p_slot), p_request.id, true, "seed set");
			return;
		case net::DebugKind::SpawnTarget:
		case net::DebugKind::TargetInfo:
		case net::DebugKind::Teleport:
		case net::DebugKind::SpawnAt:
		case net::DebugKind::SpawnSweep:
			if (queue_.size() >= kMaxQueuedDebug) {
				report(uint32_t(p_slot), p_request.id, false, "too many queued requests");
				return;
			}
			queue_.push_back({ uint32_t(p_slot), p_request });
			return;
	}
	report(uint32_t(p_slot), p_request.id, false, "unknown debug request");
}

void DevCommands::apply_debug(sim::Tick p_tick) {
	while (!queue_.empty()) {
		const PendingDebug d = queue_.front();
		queue_.pop_front();
		ServerSlot &s = slots_[d.slot];
		if (!s.used) {
			continue;
		}
		const net::DebugRequest &q = d.request;
		switch (q.kind) {
			case net::DebugKind::Teleport: {
				if (!s.spawned) {
					report(d.slot, q.id, false, "teleport: no soldier");
					break;
				}

				s.state = motor_.spawn_state(q.position, q.yaw, s.state.life, p_tick, false);
				char text[128];
				std::snprintf(text, sizeof(text), "teleported to (%.3f, %.3f, %.3f) at tick %u", double(q.position[0]), double(q.position[1]),
						double(q.position[2]), p_tick);
				report(d.slot, q.id, true, text);
				break;
			}
			case net::DebugKind::SpawnAt:
				if (q.index >= config_->level->spawns.size()) {
					report(d.slot, q.id, false, "spawn at: no point " + std::to_string(q.index));
					break;
				}
				if (!spawner_.spawn(d.slot, q.index, p_tick)) {
					report(d.slot, q.id, false, "spawn at: the slot could not spawn at point " + std::to_string(q.index));
					break;
				}
				report(d.slot, q.id, true, "spawned at point " + std::to_string(q.index) + " at tick " + std::to_string(p_tick));
				break;
			case net::DebugKind::SpawnSweep:
				if (sweep_.active) {
					report(d.slot, q.id, false, "spawn sweep: one is already running");
					break;
				}
				sweep_ = Sweep();
				sweep_.active = true;
				sweep_.requester = d.slot;
				sweep_.id = q.id;
				sweep_.ticks = std::clamp<uint32_t>(q.ticks, 1u, kMaxSweepTicks);
				break;
			case net::DebugKind::SpawnTarget:
			case net::DebugKind::TargetInfo:
				apply_target(d, p_tick);
				break;
			default:
				break;
		}
	}
}

void DevCommands::apply_target(const PendingDebug &p_debug, sim::Tick p_tick) {
	const net::DebugRequest &q = p_debug.request;
	if (q.kind == net::DebugKind::SpawnTarget) {
		const core::Status st = spawner_.place(q.slot, q.position, q.yaw, q.pose, p_tick);
		if (!st.ok()) {
			report(p_debug.slot, q.id, false, "spawn target: " + st.error().message);
			return;
		}
		char text[160];
		std::snprintf(text, sizeof(text), "target %u placed at (%.3f, %.3f, %.3f) yaw %u pose %u at tick %u", unsigned(q.slot), double(q.position[0]),
				double(q.position[1]), double(q.position[2]), unsigned(q.yaw), unsigned(q.pose), p_tick);
		report(p_debug.slot, q.id, true, text);
		return;
	}

	if (!combat_.on()) {
		report(p_debug.slot, q.id, false, "target info: no gunplay on this server");
		return;
	}
	if (q.slot >= slots_.size() || !slots_[q.slot].used || !slots_[q.slot].spawned) {
		report(p_debug.slot, q.id, false, "target info: no soldier in slot " + std::to_string(q.slot));
		return;
	}
	const std::optional<net::TargetInfoRow> row = combat_.gunplay()->target_info(q.slot, slots_[q.slot].state);
	if (!row) {
		report(p_debug.slot, q.id, false, "target info: slot " + std::to_string(q.slot) + " is not a live armed soldier");
		return;
	}
	const ServerSlot &requester = slots_[p_debug.slot];
	if (!requester.used || !requester.remote || outbox_.replies_full()) {
		return;
	}
	net::DebugReport r;
	r.id = q.id;
	r.ok = true;
	r.text = "target " + std::to_string(q.slot) + ": " + std::to_string(row->target.capsule_count) + " capsules at tick " + std::to_string(p_tick);
	r.targets.push_back(*row);
	outbox_.send(requester.peer, net::kChannelReliable, net::encode_debug_report(r));
}

void DevCommands::run_sweep(sim::Tick p_tick) {
	if (!sweep_.active) {
		return;
	}
	const std::vector<sim::SpawnPoint> &points = config_->level->spawns;
	if (sweep_.batch_active && p_tick - sweep_.batch_start >= sweep_.ticks) {
		for (size_t i = 0; i < slots_.size(); ++i) {
			ServerSlot &s = slots_[i];
			if (!s.used || !s.dummy) {
				continue;
			}
			sim::SettleResult &r = sweep_.results[per_slot_[i].sweep_entry];
			r.state = s.state;
			const sim::SpawnPoint &p = points[sweep_.batch_points[per_slot_[i].sweep_entry]];
			sim::judge_settle(motor_, *world_, p, sim::SettleOptions(), r);
			net::SpawnSweepRow row;
			row.index = p.index;
			row.ok = r.ok;
			row.point = p.position;
			row.settled = r.state.position;
			row.lift = r.lift;
			row.shift = r.shift;
			row.gap = r.gap;
			row.grounded = (r.state.flags & sim::soldier_flags::kGrounded) != 0;
			row.overlapping = r.overlapping;
			sweep_.rows.push_back(row);
			reset_slot_(i);
		}
		sweep_.batch_active = false;
	}
	if (!sweep_.batch_active && sweep_.next_point < points.size()) {
		sweep_.results.clear();
		sweep_.batch_points.clear();
		while (sweep_.next_point < points.size()) {
			const int at = slots_.free_slot();
			if (at < 0) {
				break;
			}
			const sim::SpawnPoint &p = points[sweep_.next_point];
			sim::SettleResult r = sim::place_spawn(motor_, *world_, p, 1, p_tick);
			if (!r.failure.empty()) {
				net::SpawnSweepRow row;
				row.index = p.index;
				row.ok = false;
				row.point = p.position;
				row.settled = r.placed;
				row.lift = r.lift;
				row.shift = r.shift;
				sweep_.rows.push_back(row);
				++sweep_.next_point;
				continue;
			}
			ServerSlot &s = slots_[size_t(at)];
			reset_slot_(size_t(at));
			s.used = true;
			s.dummy = true;
			s.team = 0;
			s.spawned = true;
			s.state = r.state;
			per_slot_[size_t(at)].sweep_entry = sweep_.results.size();
			sweep_.results.push_back(std::move(r));
			sweep_.batch_points.push_back(uint32_t(sweep_.next_point));
			++sweep_.next_point;
		}
		if (!sweep_.results.empty()) {
			sweep_.batch_active = true;
			sweep_.batch_start = p_tick;
		} else if (sweep_.next_point < points.size()) {
			report(sweep_.requester, sweep_.id, false, "spawn sweep: no free slot");
			sweep_ = Sweep();
			return;
		}
	}
	if (!sweep_.batch_active && sweep_.next_point >= points.size()) {
		std::sort(sweep_.rows.begin(), sweep_.rows.end(), [](const net::SpawnSweepRow &a, const net::SpawnSweepRow &b) { return a.index < b.index; });
		size_t ok = 0;
		for (const net::SpawnSweepRow &r : sweep_.rows) {
			ok += r.ok ? 1 : 0;
		}
		if (sweep_.requester < slots_.size() && slots_[sweep_.requester].used && slots_[sweep_.requester].remote) {
			const ServerSlot &s = slots_[sweep_.requester];
			net::DebugReport rep;
			rep.id = sweep_.id;
			rep.ok = ok == sweep_.rows.size();
			rep.text = "spawn sweep: " + std::to_string(ok) + " / " + std::to_string(sweep_.rows.size()) + " ok";
			rep.sweep = std::move(sweep_.rows);
			outbox_.send(s.peer, net::kChannelReliable, net::encode_debug_report(rep));
		}
		sweep_ = Sweep();
	}
}

void DevCommands::trace(size_t p_slot, sim::Tick p_tick) {
	PerSlot &t = per_slot_[p_slot];
	if (!t.tracing) {
		return;
	}
	const ServerSlot &s = slots_[p_slot];
	t.trace.push_back(s.state);
	combat_.trace_tick(p_slot, t.trace.size() == 1, p_tick);
	if (t.trace.size() >= t.trace_ticks) {
		if (s.remote) {
			net::DebugReport r;
			r.id = t.trace_id;
			r.ok = true;
			r.text = "trace of " + std::to_string(t.trace.size()) + " ticks from " + std::to_string(t.trace.front().tick);
			r.trace = std::move(t.trace);
			combat_.take_trace(p_slot, r);
			outbox_.send(s.peer, net::kChannelReliable, net::encode_debug_report(r));
		}
		t.trace.clear();
		t.tracing = false;
	}
}

}
