#include "server_ghosts.h"

#include "bf3/core/math.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

namespace {

float distance2(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	const float dx = p_a[0] - p_b[0], dy = p_a[1] - p_b[1], dz = p_a[2] - p_b[2];
	return dx * dx + dy * dy + dz * dz;
}

template <size_t N> GhostTier tier_at(const std::array<GhostTier, N> &p_tiers, float p_d2) {
	for (const GhostTier &t : p_tiers) {
		if (p_d2 <= t.metres * t.metres) {
			return t;
		}
	}
	return p_tiers.back();
}

GhostTier tighter(const GhostTier &p_a, const GhostTier &p_b) {
	return { p_a.metres, std::min(p_a.target, p_b.target), std::min(p_a.required, p_b.required) };
}

bool in_aim_cone(const GhostViewer &p_viewer, const sim::Vec3 &p_target) {
	const sim::Vec3 to = core::vec::sub(p_target, p_viewer.eye);
	const float length = core::vec::length(to);
	return length > 0.0f && core::vec::dot(to, p_viewer.aim) >= kAimConeCos * length;
}

}

std::optional<GhostTier> ghost_tier(const GhostViewer &p_viewer, const GhostCandidate &p_ghost, bool p_attacker) {
	const float d2 = distance2(p_viewer.position, p_ghost.position);
	const bool enemy = p_viewer.team == 0 || p_ghost.team != p_viewer.team;
	if (!enemy && !p_ghost.squad_mate) {
		return tier_at(kTeammateTiers, d2);
	}
	if (enemy && p_attacker) {
		return kAttackerTier;
	}
	if (enemy && d2 > kEnemyScopeMetres * kEnemyScopeMetres) {
		return std::nullopt;
	}
	GhostTier t = tier_at(kEnemyTiers, d2);
	if (enemy && p_viewer.zoomed && in_aim_cone(p_viewer, p_ghost.position)) {
		t = tighter(t, kAimConeTier);
	}
	return t;
}

bool shot_passes_by(
		const sim::Vec3 &p_origin, const sim::Vec3 &p_direction, const FlyBy &p_fly_by, const sim::Vec3 &p_eye) {
	const float length = core::vec::length(p_direction);
	if (!(p_fly_by.radius > 0.0f) || !(p_fly_by.range > 0.0f) || !(length > 0.0f) || !std::isfinite(length)) {
		return false;
	}
	const sim::Vec3 dir = core::vec::scale(p_direction, 1.0f / length);
	const sim::Vec3 to = core::vec::sub(p_eye, p_origin);
	const float along = std::clamp(core::vec::dot(to, dir), 0.0f, p_fly_by.range);
	const sim::Vec3 closest = core::vec::add(p_origin, core::vec::scale(dir, along));
	return distance2(closest, p_eye) <= p_fly_by.radius * p_fly_by.radius;
}

void GhostScheduler::configure(size_t p_slots, int64_t p_budget_bytes) {
	clients_.assign(p_slots, Client());
	budget_ = p_budget_bytes;
}

void GhostScheduler::reset(size_t p_client) {
	if (p_client < clients_.size()) {
		clients_[p_client] = Client();
	}
}

GhostScheduler::SentSnapshot *GhostScheduler::entry(Client &r_client, uint16_t p_seq) {
	return const_cast<SentSnapshot *>(entry(static_cast<const Client &>(r_client), p_seq));
}

const GhostScheduler::SentSnapshot *GhostScheduler::entry(const Client &p_client, uint16_t p_seq) const {
	if (p_client.snapshots.empty()) {
		return nullptr;
	}
	const SentSnapshot &e = p_client.snapshots[size_t(p_seq) % p_client.snapshots.size()];
	return e.valid && e.seq == p_seq ? &e : nullptr;
}

void GhostScheduler::finish_removals(Client &r_client, const SentSnapshot &p_snapshot) {
	for (const uint8_t slot : p_snapshot.removed) {
		if (slot < r_client.ghosts.size() && r_client.ghosts[slot].removing) {
			Ghost &g = r_client.ghosts[slot];
			g.removing = false;
			g.known = false;
		}
	}
}

void GhostScheduler::ack(size_t p_client, const net::CommandPacket &p_packet) {
	if (p_client >= clients_.size() || !clients_[p_client].open) {
		return;
	}
	Client &c = clients_[p_client];
	const auto main = [&](uint16_t p_seq) {
		if (SentSnapshot *e = entry(c, p_seq); e != nullptr && !e->main_acked) {
			e->main_acked = true;
			finish_removals(c, *e);
		}
	};
	main(p_packet.newest_snapshot);
	for (uint32_t i = 0; i < 32; ++i) {
		if ((p_packet.snapshot_ack_bits >> i) & 1u) {
			main(uint16_t(p_packet.newest_snapshot - 1u - i));
		}
		if ((p_packet.others_ack_bits >> i) & 1u) {
			if (SentSnapshot *e = entry(c, uint16_t(p_packet.newest_snapshot - i))) {
				e->others_acked = true;
			}
		}
	}
}

void GhostScheduler::attacked(size_t p_victim, size_t p_shooter, sim::Tick p_tick) {
	if (p_victim >= clients_.size() || !clients_[p_victim].open || p_shooter >= clients_[p_victim].ghosts.size()) {
		return;
	}
	clients_[p_victim].ghosts[p_shooter].attacked_until = p_tick + kAttackerTicks;
}

int64_t GhostScheduler::open_interval(size_t p_client, uint64_t p_sent_total) {
	Client &c = clients_[p_client];
	if (!c.open) {
		c.open = true;
		c.ghosts.assign(clients_.size(), Ghost());
		c.snapshots.assign(net::kBaselineRing, SentSnapshot());
		c.sent_total = p_sent_total;
		c.credit = budget_;
		return c.credit;
	}
	const int64_t spent = int64_t(p_sent_total - c.sent_total);
	c.sent_total = p_sent_total;
	c.credit = std::min(c.credit - spent + budget_, budget_);
	return std::max<int64_t>(c.credit, 0);
}

void GhostScheduler::forget(Ghost &r_ghost) {
	r_ghost.ring = {};
	r_ghost.ring_next = 0;
	r_ghost.ever = false;
	r_ghost.accumulator = 1.0f;
}

bool GhostScheduler::acked(const Client &p_client, const SentGhost &p_sent) const {
	const SentSnapshot *e = entry(p_client, p_sent.seq);
	return e != nullptr && e->tick == p_sent.tick && (p_sent.in_main ? e->main_acked : e->others_acked);
}

const GhostScheduler::SentGhost *GhostScheduler::baseline(const Client &p_client, const Ghost &p_ghost,
		const net::OtherSoldier &p_record, uint16_t p_seq, sim::Tick p_tick) const {
	const SentGhost *best = nullptr;
	for (const SentGhost &s : p_ghost.ring) {
		const uint16_t back = uint16_t(p_seq - s.seq);
		if (!s.valid || back < 1 || back > net::kMaxBaselineBack || s.tick >= p_tick ||
				s.record.generation != p_record.generation || s.record.slot != p_record.slot || !acked(p_client, s)) {
			continue;
		}
		if (best == nullptr || uint16_t(p_seq - best->seq) > back) {
			best = &s;
		}
	}
	return best;
}

void GhostScheduler::plan(size_t p_client, const GhostViewer &p_viewer, std::span<const GhostCandidate> p_candidates,
		uint16_t p_seq, sim::Tick p_tick, GhostPlan &r_plan) {
	r_plan.others.clear();
	r_plan.bases = net::SnapshotBases();
	r_plan.removed.clear();
	r_plan.in_scope.clear();
	Client &c = clients_[p_client];
	struct Pick {
		bool required = false;
		float accumulator = 0;
		float d2 = 0;
		size_t candidate = 0;
	};
	std::vector<Pick> picks;
	std::vector<bool> present(c.ghosts.size(), false);
	for (size_t k = 0; k < p_candidates.size(); ++k) {
		const GhostCandidate &cand = p_candidates[k];
		if (cand.slot >= c.ghosts.size() || cand.slot == p_viewer.slot) {
			continue;
		}
		Ghost &g = c.ghosts[cand.slot];
		const std::optional<GhostTier> tier = ghost_tier(p_viewer, cand, g.attacked_until > p_tick);
		if (!tier) {
			continue;
		}
		present[cand.slot] = true;
		r_plan.in_scope.push_back(cand.slot);
		if (g.removing || (g.ever && g.generation != cand.record.generation)) {
			g.removing = false;
			forget(g);
			g.scoped = false;
		}
		if (!g.scoped) {
			g.scoped = true;
			g.scope_seq = p_seq;
		}
		g.generation = cand.record.generation;
		g.accumulator += 1.0f / float(tier->target);
		const uint32_t since =
				g.ever ? uint32_t(uint16_t(p_seq - g.last_seq)) : uint32_t(uint16_t(p_seq - g.scope_seq)) + 1u;
		Pick p;
		p.required = since >= tier->required;
		p.accumulator = g.accumulator;
		p.d2 = distance2(p_viewer.position, cand.position);
		p.candidate = k;
		if (p.required || g.accumulator >= 1.0f - 1e-4f) {
			picks.push_back(p);
		}
	}
	for (size_t j = 0; j < c.ghosts.size(); ++j) {
		Ghost &g = c.ghosts[j];
		if (!present[j]) {
			g.scoped = false;
		}
		if (present[j] || !(g.known || g.removing)) {
			continue;
		}
		if (!g.removing) {
			g.removing = true;
			forget(g);
		}
		r_plan.removed.push_back(uint8_t(j));
	}
	std::sort(picks.begin(), picks.end(), [&](const Pick &p_a, const Pick &p_b) {
		if (p_a.required != p_b.required) {
			return p_a.required;
		}
		if (!p_a.required && p_a.accumulator != p_b.accumulator) {
			return p_a.accumulator > p_b.accumulator;
		}
		if (p_a.d2 != p_b.d2) {
			return p_a.d2 < p_b.d2;
		}
		return p_candidates[p_a.candidate].slot < p_candidates[p_b.candidate].slot;
	});
	for (const Pick &p : picks) {
		const GhostCandidate &cand = p_candidates[p.candidate];
		r_plan.others.push_back(cand.record);
		net::OtherBaseline b;
		if (const SentGhost *s = baseline(c, c.ghosts[cand.slot], cand.record, p_seq, p_tick)) {
			b.back = uint8_t(uint16_t(p_seq - s->seq));
			b.tick = s->tick;
			b.record = s->record;
		}
		r_plan.bases.others.push_back(b);
		r_plan.bases.required += p.required ? 1u : 0u;
	}
	own_base(p_client, p_seq, p_tick, r_plan.bases.own);
}

void GhostScheduler::own_base(size_t p_client, uint16_t p_seq, sim::Tick p_tick, net::OwnBaseline &r_base) const {
	r_base = net::OwnBaseline();
	const Client &c = clients_[p_client];
	for (uint8_t back = 1; back <= net::kMaxBaselineBack; ++back) {
		const SentSnapshot *e = entry(c, uint16_t(p_seq - back));
		if (e == nullptr || !e->main_acked || !e->has_own || e->tick >= p_tick) {
			continue;
		}
		r_base.back = back;
		r_base.tick = e->tick;
		r_base.state = e->own;
		r_base.weapons = e->weapons;
		return;
	}
}

void GhostScheduler::sent(size_t p_client, const net::Snapshot &p_snapshot, std::span<const net::OtherSoldier> p_main,
		std::span<const net::OtherSoldier> p_others) {
	Client &c = clients_[p_client];
	SentSnapshot &e = c.snapshots[size_t(p_snapshot.seq) % c.snapshots.size()];
	e = SentSnapshot();
	e.valid = true;
	e.seq = p_snapshot.seq;
	e.tick = p_snapshot.server_tick;
	e.has_own = p_snapshot.own.has_value();
	if (e.has_own) {
		e.own = *p_snapshot.own;
		e.weapons = p_snapshot.own_weapons;
	}
	e.removed = p_snapshot.removed;
	const auto note = [&](const net::OtherSoldier &p_o, bool p_main_list) {
		if (p_o.slot >= c.ghosts.size()) {
			return;
		}
		Ghost &g = c.ghosts[p_o.slot];
		SentGhost &s = g.ring[g.ring_next];
		g.ring_next = (g.ring_next + 1) % g.ring.size();
		s.valid = true;
		s.in_main = p_main_list;
		s.seq = p_snapshot.seq;
		s.tick = p_snapshot.server_tick;
		s.record = p_o;
		g.known = true;
		g.ever = true;
		g.last_seq = p_snapshot.seq;
		g.accumulator = 0.0f;
	};
	for (const net::OtherSoldier &o : p_main) {
		note(o, true);
	}
	for (const net::OtherSoldier &o : p_others) {
		note(o, false);
	}
}

}
