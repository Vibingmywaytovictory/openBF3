#include "bf3/host/lag_comp.h"

#include "bf3/sim/weapons/posed_target.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace bf3::host {

namespace {

constexpr uint32_t kMaxLagSlots = 64;
constexpr uint32_t kNoRecord = 0xFFFFFFFFu;
constexpr double kMsPerTick = 1000.0 / double(sim::kSimHz);
constexpr size_t kTickTimes = 4096;
constexpr size_t kCacheProbes = 16;

bool bit(uint64_t p_mask, uint32_t p_i) {
	return p_i < 64 && ((p_mask >> p_i) & 1u) != 0;
}

bool finite3(const sim::Vec3 &p_v) {
	return std::isfinite(p_v[0]) && std::isfinite(p_v[1]) && std::isfinite(p_v[2]);
}

double p99_of(std::vector<double> p_v) {
	if (p_v.empty()) {
		return 0;
	}
	const size_t k = std::min(p_v.size() - 1, size_t(std::ceil(0.99 * double(p_v.size()))) - 1);
	std::nth_element(p_v.begin(), p_v.begin() + ptrdiff_t(k), p_v.end());
	return p_v[k];
}

}

struct LagHistory::Impl {
	struct TickRecords {
		bool valid = false;
		uint32_t tick = 0;
		uint64_t live = 0;
		std::array<net::OtherSoldier, kMaxLagSlots> records{};
	};
	struct Sent {
		bool valid = false;
		bool main_received = false;
		bool others_received = false;
		uint16_t seq = 0;
		uint32_t tick = 0;
		uint32_t record_index = kNoRecord;
		uint64_t main_mask = 0;
		uint64_t others_mask = 0;
	};

	LagCompConfig config;
	uint32_t slots = 0;
	std::vector<TickRecords> ticks;
	size_t newest = 0;
	bool any_tick = false;
	std::vector<std::array<Sent, kSentRingEntries>> sent;
	std::array<SweptBounds, kMaxLagSlots> bounds{};
	mutable LagCompStats stats;

	const Sent *entry(uint8_t p_client, uint16_t p_seq) const {
		if (p_client >= slots) {
			return nullptr;
		}
		const Sent &e = sent[p_client][p_seq % kSentRingEntries];
		return e.valid && e.seq == p_seq ? &e : nullptr;
	}
	Sent *entry(uint8_t p_client, uint16_t p_seq) { return const_cast<Sent *>(static_cast<const Impl *>(this)->entry(p_client, p_seq)); }

	void rebuild_bounds() {
		for (uint32_t s = 0; s < slots; ++s) {
			SweptBounds b;
			for (const TickRecords &t : ticks) {
				if (!t.valid || !bit(t.live, s)) {
					continue;
				}
				const net::OtherSoldier &o = t.records[s];
				if (!b.any) {
					b.any = true;
					for (size_t k = 0; k < 3; ++k) {
						b.lo[k] = b.hi[k] = o.position[k];
					}
					b.oldest_tick = b.newest_tick = t.tick;
				}
				for (size_t k = 0; k < 3; ++k) {
					b.lo[k] = std::min(b.lo[k], o.position[k]);
					b.hi[k] = std::max(b.hi[k], o.position[k]);
				}
				const float v = std::sqrt(o.velocity[0] * o.velocity[0] + o.velocity[1] * o.velocity[1] + o.velocity[2] * o.velocity[2]);
				b.max_speed = std::max(b.max_speed, v);
				b.oldest_tick = std::min(b.oldest_tick, t.tick);
				b.newest_tick = std::max(b.newest_tick, t.tick);
			}
			bounds[s] = b;
		}
	}
};

LagHistory::LagHistory(const LagCompConfig &p_config, uint32_t p_max_slots) :
		impl_(std::make_unique<Impl>()) {
	impl_->config = p_config;
	impl_->config.history_snapshots = std::clamp<uint32_t>(p_config.history_snapshots, 2u, 256u);
	impl_->slots = std::min(p_max_slots, kMaxLagSlots);
	impl_->ticks.resize(impl_->config.history_snapshots);
	impl_->sent.resize(impl_->slots);
}

LagHistory::~LagHistory() = default;

void LagHistory::record_tick(uint32_t p_snapshot_tick, std::span<const net::OtherSoldier> p_records) {
	Impl &m = *impl_;
	size_t at = m.newest;
	if (!m.any_tick || !m.ticks[m.newest].valid || m.ticks[m.newest].tick != p_snapshot_tick) {
		at = m.any_tick ? (m.newest + 1) % m.ticks.size() : 0;
	}
	Impl::TickRecords &t = m.ticks[at];
	t.valid = true;
	t.tick = p_snapshot_tick;
	t.live = 0;
	for (const net::OtherSoldier &o : p_records) {
		if (o.slot >= m.slots || !finite3(o.position) || !finite3(o.velocity)) {
			continue;
		}
		t.records[o.slot] = o;
		t.live |= uint64_t(1) << o.slot;
	}
	m.newest = at;
	m.any_tick = true;
	m.rebuild_bounds();
}

void LagHistory::sent(uint8_t p_client_slot, uint16_t p_seq, uint32_t p_snapshot_tick, uint64_t p_main_mask, uint64_t p_others_mask) {
	Impl &m = *impl_;
	if (p_client_slot >= m.slots) {
		return;
	}
	Impl::Sent e;
	e.valid = true;
	e.seq = p_seq;
	e.tick = p_snapshot_tick;
	e.main_mask = p_main_mask;
	e.others_mask = p_others_mask;

	for (size_t k = 0; k < m.ticks.size() && m.any_tick; ++k) {
		const size_t i = (m.newest + m.ticks.size() - k) % m.ticks.size();
		if (m.ticks[i].valid && m.ticks[i].tick == p_snapshot_tick) {
			e.record_index = uint32_t(i);
			break;
		}
	}
	m.sent[p_client_slot][p_seq % kSentRingEntries] = e;
}

void LagHistory::ack(uint8_t p_client_slot, uint16_t p_newest, uint32_t p_snapshot_ack_bits, uint32_t p_others_ack_bits) {
	Impl &m = *impl_;
	if (p_client_slot >= m.slots) {
		++m.stats.acks_ignored;
		return;
	}

	if (Impl::Sent *e = m.entry(p_client_slot, p_newest)) {
		e->main_received = true;
	} else {
		++m.stats.acks_ignored;
	}
	for (uint32_t i = 0; i < 32; ++i) {
		if ((p_snapshot_ack_bits >> i) & 1u) {
			if (Impl::Sent *e = m.entry(p_client_slot, uint16_t(p_newest - 1 - i))) {
				e->main_received = true;
			} else {
				++m.stats.acks_ignored;
			}
		}
		if ((p_others_ack_bits >> i) & 1u) {
			Impl::Sent *e = m.entry(p_client_slot, uint16_t(p_newest - i));
			if (e != nullptr && e->others_mask != 0) {
				e->others_received = true;
			} else {
				++m.stats.acks_ignored;
			}
		}
	}
}

AckState LagHistory::ack_state(uint8_t p_client_slot) const {
	const Impl &m = *impl_;
	AckState a;
	if (p_client_slot >= m.slots) {
		return a;
	}
	bool any = false;
	for (const Impl::Sent &e : m.sent[p_client_slot]) {
		if (e.valid && e.main_received && (!any || net::seq_newer(e.seq, a.newest))) {
			a.newest = e.seq;
			any = true;
		}
	}
	if (!any) {
		return a;
	}
	for (uint32_t i = 0; i < 64; ++i) {
		const Impl::Sent *e = m.entry(p_client_slot, uint16_t(a.newest - i));
		if (e == nullptr) {
			continue;
		}
		if (e->main_received) {
			a.main_bits |= uint64_t(1) << i;
		}
		if (e->others_received) {
			a.others_bits |= uint64_t(1) << i;
		}
	}
	return a;
}

bool LagHistory::shot_view(uint8_t p_client_slot, const AckState &p_at_arrival, uint8_t p_view_seq8, int32_t p_view_back_q8, uint8_t p_quality,
		ShotView &r_view) const {
	r_view = ShotView();
	r_view.view_back_q8 = p_view_back_q8;
	r_view.quality = p_quality;
	if (p_quality == uint8_t(sim::ViewQuality::None)) {
		return true;
	}

	if (p_quality > uint8_t(sim::ViewQuality::Extrapolated) || net::view_lag_q8(p_view_back_q8) < 0 || p_at_arrival.main_bits == 0) {
		return false;
	}
	const uint8_t d = uint8_t(uint8_t(p_at_arrival.newest & 0xFF) - p_view_seq8);
	if (d >= kViewSeqWindow) {
		return false;
	}
	const uint16_t view_seq = uint16_t(p_at_arrival.newest - d);
	if (impl_->entry(p_client_slot, view_seq) == nullptr) {
		return false;
	}
	uint32_t main = 0, others = 0;
	for (uint32_t i = 0; i < 32; ++i) {
		if (bit(p_at_arrival.main_bits, d + i)) {
			main |= 1u << i;
		}
		if (bit(p_at_arrival.others_bits, d + i)) {
			others |= 1u << i;
		}
	}
	if ((main & 1u) == 0) {
		return false;
	}
	r_view.view_seq = view_seq;
	r_view.main_bits = main;
	r_view.others_bits = others;
	return true;
}

size_t LagHistory::track(uint8_t p_client_slot, const ShotView &p_view, uint8_t p_target, std::span<net::TimedOther, net::kTrackSamples> r_list) const {
	const Impl &m = *impl_;
	if (p_target >= m.slots) {
		return 0;
	}
	const Impl::Sent *view = m.entry(p_client_slot, p_view.view_seq);
	if (view == nullptr) {
		return 0;
	}
	std::array<net::TimedOther, 32> candidates;
	size_t n = 0;
	for (uint32_t i = 0; i < 32; ++i) {
		const bool main = ((p_view.main_bits >> i) & 1u) != 0;
		const bool others = ((p_view.others_bits >> i) & 1u) != 0;
		if (!main && !others) {
			continue;
		}
		const Impl::Sent *e = m.entry(p_client_slot, uint16_t(p_view.view_seq - i));
		if (e == nullptr) {
			continue;
		}
		const bool carried = (main && bit(e->main_mask, p_target)) || (others && bit(e->others_mask, p_target));
		if (!carried) {
			continue;
		}
		if (e->record_index == kNoRecord || e->record_index >= m.ticks.size()) {
			++m.stats.history_misses;
			continue;
		}
		const Impl::TickRecords &t = m.ticks[e->record_index];
		if (!t.valid || t.tick != e->tick || !bit(t.live, p_target)) {
			++m.stats.history_misses;
			continue;
		}
		candidates[n].server_tick = t.tick;
		candidates[n].soldier = t.records[p_target];
		++n;
	}
	return net::rebuild_track(std::span<const net::TimedOther>(candidates.data(), n), view->tick, r_list);
}

net::OtherPose LagHistory::sample(std::span<const net::TimedOther> p_list, int64_t p_time_q8) const {
	const LagCompConfig &c = impl_->config;
	return net::sample_other(p_list, double(p_time_q8) / 256.0, c.pose_eye_heights, double(std::max(0.0f, c.extrapolate_max_ms)) / kMsPerTick);
}

void LagHistory::client_reset(uint8_t p_client_slot) {
	if (p_client_slot < impl_->slots) {
		impl_->sent[p_client_slot].fill(Impl::Sent());
	}
}

const LagCompStats &LagHistory::stats() const {
	return impl_->stats;
}

const LagCompConfig &LagHistory::config() const {
	return impl_->config;
}

uint32_t LagHistory::max_slots() const {
	return impl_->slots;
}

SweptBounds LagHistory::bounds(uint8_t p_slot) const {
	return p_slot < impl_->slots ? impl_->bounds[p_slot] : SweptBounds();
}

bool LagHistory::sent_tick(uint8_t p_client_slot, uint16_t p_seq, uint32_t &r_tick) const {
	const Impl::Sent *e = impl_->entry(p_client_slot, p_seq);
	if (e == nullptr) {
		return false;
	}
	r_tick = e->tick;
	return true;
}

struct RewindProvider::Impl {
	struct Present {
		bool alive = false;
		uint8_t generation = 0;
		uint8_t team = 0;
		bool spawn_protected = false;
		sim::weapons::HitTarget target;
		float lo[3]{}, hi[3]{};
		float tlo[3]{}, thi[3]{};
	};
	struct View {
		bool valid = false;
		bool refused = false;
		uint32_t shot_index = 0;
		ShotView view;
	};
	struct CacheEntry {
		uint32_t epoch = 0;
		uint8_t slot = 0, generation = 0, kind = 0;
		int64_t time_q8 = 0;
		uint32_t tick_a = 0, tick_b = 0;
		sim::weapons::HitTarget target;
	};

	const LagHistory *history = nullptr;
	const core::gameplay::CapsulePoseTable *table = nullptr;
	LagCompConfig config;
	uint32_t tick = 0;
	std::array<Present, kMaxLagSlots> present{};
	std::vector<std::array<View, kShotViewRing>> views;
	mutable std::vector<CacheEntry> cache;
	uint32_t epoch = 1;
	mutable std::vector<sim::weapons::HitTarget> scratch;
	mutable LagCompStats stats;
	std::vector<RewindTraceRow> *trace = nullptr;

	mutable double tick_seconds = 0;
	bool tick_open = false;
	double last_ms = 0;
	double sum_ms = 0;
	std::vector<double> tick_ms;
	size_t tick_ms_next = 0;
	mutable bool p99_dirty = true;

	void close_tick() {
		if (!tick_open) {
			return;
		}
		tick_open = false;
		last_ms = tick_seconds * 1000.0;
		tick_seconds = 0;
		if (tick_ms.size() < kTickTimes) {
			tick_ms.push_back(last_ms);
		} else {
			tick_ms[tick_ms_next] = last_ms;
			tick_ms_next = (tick_ms_next + 1) % kTickTimes;
		}
		sum_ms += last_ms;
		stats.tick_ms_max = std::max(stats.tick_ms_max, last_ms);
		p99_dirty = true;
	}

	static size_t hash_key(uint8_t p_slot, uint8_t p_gen, int64_t p_time, uint32_t p_a, uint32_t p_b, uint8_t p_kind) {
		uint64_t h = 0xCBF29CE484222325ull;
		auto mix = [&](uint64_t p_v) {
			h ^= p_v;
			h *= 0x100000001B3ull;
			h ^= h >> 29;
		};
		mix(uint64_t(p_slot) | uint64_t(p_gen) << 8 | uint64_t(p_kind) << 16);
		mix(uint64_t(p_time));
		mix(uint64_t(p_a) | uint64_t(p_b) << 32);
		return size_t(h);
	}
};

namespace {

class ScopedTimer {
public:
	ScopedTimer(bool p_on, double &r_acc) :
			on_(p_on), acc_(r_acc) {
		if (on_) {
			t0_ = std::chrono::steady_clock::now();
		}
	}
	~ScopedTimer() {
		if (on_) {
			acc_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
		}
	}
	ScopedTimer(const ScopedTimer &) = delete;
	ScopedTimer &operator=(const ScopedTimer &) = delete;

private:
	bool on_;
	double &acc_;
	std::chrono::steady_clock::time_point t0_{};
};

bool overlap(const float *p_alo, const float *p_ahi, const float *p_blo, const float *p_bhi) {
	return p_alo[0] <= p_bhi[0] && p_blo[0] <= p_ahi[0] && p_alo[1] <= p_bhi[1] && p_blo[1] <= p_ahi[1] && p_alo[2] <= p_bhi[2] && p_blo[2] <= p_ahi[2];
}

sim::weapons::TargetPose target_pose_of(const net::OtherPose &p_pose, uint32_t p_id, uint32_t p_team, bool p_protected) {
	sim::weapons::TargetPose t;
	t.feet = p_pose.position;
	t.yaw = p_pose.yaw;
	t.pitch = p_pose.pitch;
	t.pose = sim::Pose(std::min<uint8_t>(p_pose.pose, uint8_t(sim::kPoseCount - 1)));
	t.target_pose = sim::Pose(std::min<uint8_t>(p_pose.target_pose, uint8_t(sim::kPoseCount - 1)));
	t.transition = float(p_pose.transition) / 255.0f;
	t.id = p_id;
	t.team = p_team;
	t.spawn_protected = p_protected;
	return t;
}

}

RewindProvider::RewindProvider(const LagHistory &p_history, const core::gameplay::CapsulePoseTable &p_capsules, const LagCompConfig &p_config) :
		impl_(std::make_unique<Impl>()) {
	impl_->history = &p_history;
	impl_->table = &p_capsules;
	impl_->config = p_config;
	impl_->views.resize(kMaxLagSlots);
	impl_->cache.resize(kPoseCacheEntries);
	impl_->scratch.reserve(kMaxLagSlots);
}

RewindProvider::~RewindProvider() = default;

void RewindProvider::begin_tick(uint32_t p_tick, std::span<const sim::SoldierState> p_present) {
	begin_tick(p_tick, p_present, {});
}

void RewindProvider::begin_tick(uint32_t p_tick, std::span<const sim::SoldierState> p_present, std::span<const uint8_t> p_teams) {
	Impl &m = *impl_;
	m.close_tick();
	m.tick_open = true;
	ScopedTimer timer(m.config.time_provider, m.tick_seconds);
	m.tick = p_tick;
	++m.stats.ticks;
	if (++m.epoch == 0) {
		for (Impl::CacheEntry &e : m.cache) {
			e.epoch = 0;
		}
		m.epoch = 1;
	}
	const double extra_s = double(std::max(0.0f, m.config.extrapolate_max_ms)) / 1000.0;
	const float table_bound = m.table->bound_radius;
	for (uint32_t s = 0; s < kMaxLagSlots; ++s) {
		Impl::Present &p = m.present[s];
		p = Impl::Present();
		if (s >= p_present.size()) {
			continue;
		}
		const sim::SoldierState &st = p_present[s];
		if (st.life == 0 || st.mode == sim::MoveMode::Dead || !finite3(st.position)) {
			continue;
		}
		p.alive = true;
		p.generation = uint8_t(st.life & 0xFF);
		p.team = s < p_teams.size() ? p_teams[s] : uint8_t(0);
		p.spawn_protected = st.protect_ticks > 0;

		const net::OtherSoldier rec = net::quantize_other(uint8_t(s), st);
		const net::TimedOther one{ st.tick, rec };
		const net::OtherPose op = net::sample_other(std::span<const net::TimedOther>(&one, 1), double(st.tick), m.config.pose_eye_heights, 0.0);
		p.target = sim::weapons::posed_target(*m.table, target_pose_of(op, s, p.team, p.spawn_protected));
		for (size_t k = 0; k < 3; ++k) {
			p.tlo[k] = p.target.bound_center[k] - p.target.bound_radius;
			p.thi[k] = p.target.bound_center[k] + p.target.bound_radius;
		}

		const SweptBounds hb = m.history->bounds(uint8_t(s));
		float lo[3] = { rec.position[0], rec.position[1], rec.position[2] };
		float hi[3] = { lo[0], lo[1], lo[2] };
		double grow = double(table_bound);
		if (hb.any) {
			for (size_t k = 0; k < 3; ++k) {
				lo[k] = std::min(lo[k], hb.lo[k]);
				hi[k] = std::max(hi[k], hb.hi[k]);
			}
			const double span_s = double(hb.newest_tick - hb.oldest_tick) / double(sim::kSimHz);
			grow += double(hb.max_speed) * (extra_s + span_s * 8.0 / 27.0);
		}
		grow += 0.01;
		for (size_t k = 0; k < 3; ++k) {
			p.lo[k] = float(double(lo[k]) - grow);
			p.hi[k] = float(double(hi[k]) + grow);
		}
	}
}

void RewindProvider::end_tick() {
	impl_->close_tick();
}

void RewindProvider::note_shot(uint8_t p_slot, uint32_t p_shot_index, const ShotView &p_view) {
	Impl &m = *impl_;
	if (p_slot >= kMaxLagSlots) {
		return;
	}
	Impl::View &v = m.views[p_slot][p_shot_index % kShotViewRing];
	v.valid = true;
	v.refused = false;
	v.shot_index = p_shot_index;
	v.view = p_view;
	if (!m.config.enabled || p_view.quality == uint8_t(sim::ViewQuality::None)) {
		++m.stats.present;
	} else if (net::view_lag_q8(p_view.view_back_q8) > int64_t(m.config.max_rewind_ticks) * 256) {
		++m.stats.clamped;
	} else {
		++m.stats.rewound;
	}
}

void RewindProvider::note_refused(uint8_t p_slot, uint32_t p_shot_index) {
	Impl &m = *impl_;
	if (p_slot >= kMaxLagSlots) {
		return;
	}
	Impl::View &v = m.views[p_slot][p_shot_index % kShotViewRing];
	v.valid = true;
	v.refused = true;
	v.shot_index = p_shot_index;
	v.view = ShotView();
	++m.stats.refused;
}

bool RewindProvider::shot_view_of(uint8_t p_slot, uint32_t p_shot_index, ShotView &r_view) const {
	if (p_slot >= kMaxLagSlots) {
		return false;
	}
	const Impl::View &v = impl_->views[p_slot][p_shot_index % kShotViewRing];
	if (!v.valid || v.refused || v.shot_index != p_shot_index) {
		return false;
	}
	r_view = v.view;
	return true;
}

void RewindProvider::set_trace(std::vector<RewindTraceRow> *r_rows) {
	impl_->trace = r_rows;
}

const sim::weapons::HitTarget *RewindProvider::present_target(uint8_t p_slot) const {
	return p_slot < kMaxLagSlots && impl_->present[p_slot].alive ? &impl_->present[p_slot].target : nullptr;
}

double RewindProvider::last_tick_ms() const {
	return impl_->last_ms;
}

std::span<const sim::weapons::HitTarget> RewindProvider::targets_for(const sim::weapons::TargetQuery &p_query) const {
	const Impl &m = *impl_;
	ScopedTimer timer(m.config.time_provider, m.tick_seconds);
	++m.stats.queries;
	m.scratch.clear();

	float slo[3] = { 0, 0, 0 }, shi[3] = { 0, 0, 0 };
	const bool cull = p_query.has_segment && finite3(p_query.from) && finite3(p_query.to) && std::isfinite(p_query.radius);
	if (cull) {
		const float r = std::max(0.0f, p_query.radius);
		for (size_t k = 0; k < 3; ++k) {
			slo[k] = std::min(p_query.from[k], p_query.to[k]) - r;
			shi[k] = std::max(p_query.from[k], p_query.to[k]) + r;
		}
	}
	const uint32_t shooter = p_query.player_slot;
	const Impl::View *view = nullptr;
	if (shooter < kMaxLagSlots) {
		const Impl::View &v = m.views[shooter][p_query.shot_index % kShotViewRing];
		if (v.valid && v.shot_index == p_query.shot_index) {
			view = &v;
		}
	}
	const bool rewind = m.config.enabled && view != nullptr && !view->refused && view->view.quality != uint8_t(sim::ViewQuality::None);
	if (!rewind) {
		++m.stats.present_queries;
		if (view == nullptr) {
			++m.stats.no_view;
		}
		for (uint32_t s = 0; s < kMaxLagSlots; ++s) {
			const Impl::Present &p = m.present[s];
			if (!p.alive || s == shooter) {
				continue;
			}
			if (cull && !overlap(slo, shi, p.tlo, p.thi)) {
				++m.stats.culled;
				continue;
			}
			m.scratch.push_back(p.target);
		}
		m.stats.answered += m.scratch.size();
		return m.scratch;
	}
	++m.stats.rewound_queries;
	const ShotView &sv = view->view;
	const int64_t time_q8 = net::rewind_time_q8(p_query.time_start, sv.view_back_q8, m.config.max_rewind_ticks);
	const double render = double(time_q8) / 256.0;
	const double max_extra = double(std::max(0.0f, m.config.extrapolate_max_ms)) / kMsPerTick;
	std::array<net::TimedOther, net::kTrackSamples> list;
	for (uint32_t s = 0; s < kMaxLagSlots; ++s) {
		const Impl::Present &p = m.present[s];
		if (!p.alive || s == shooter) {
			continue;
		}
		if (cull && !overlap(slo, shi, p.lo, p.hi)) {
			++m.stats.culled;
			continue;
		}
		++m.stats.candidates;
		const size_t n = m.history->track(uint8_t(shooter), sv, uint8_t(s), list);
		if (n == 0) {
			continue;
		}
		const std::span<const net::TimedOther> l(list.data(), n);
		if (l.back().soldier.generation != p.generation) {
			++m.stats.generation_skips;
			continue;
		}
		const net::SampleSelection sel = net::select_sample(l, render, max_extra);
		const uint8_t kind = uint8_t(sel.kind);
		Impl::CacheEntry *slot = nullptr;
		bool hit = false;
		if (m.config.pose_cache) {
			const size_t h = Impl::hash_key(uint8_t(s), p.generation, time_q8, sel.tick_a, sel.tick_b, kind);
			for (size_t probe = 0; probe < kCacheProbes; ++probe) {
				Impl::CacheEntry &e = m.cache[(h + probe) % m.cache.size()];
				if (e.epoch != m.epoch) {
					slot = &e;
					break;
				}
				if (e.slot == s && e.generation == p.generation && e.time_q8 == time_q8 && e.tick_a == sel.tick_a && e.tick_b == sel.tick_b && e.kind == kind) {
					slot = &e;
					hit = true;
					break;
				}
			}
		}
		if (hit) {
			++m.stats.pose_cache_hits;
			m.scratch.push_back(slot->target);
			if (m.trace != nullptr) {
				RewindTraceRow row;
				row.shooter = shooter;
				row.shot_index = p_query.shot_index;
				row.pellet = p_query.pellet;
				row.target = uint8_t(s);
				row.time_q8 = time_q8;
				row.pose = m.history->sample(l, time_q8);
				row.cached = true;
				m.trace->push_back(row);
			}
			continue;
		}
		const net::OtherPose pose = m.history->sample(l, time_q8);
		const sim::weapons::HitTarget target = sim::weapons::posed_target(*m.table, target_pose_of(pose, s, p.team, p.spawn_protected));
		if (m.config.pose_cache) {
			if (slot != nullptr) {
				slot->epoch = m.epoch;
				slot->slot = uint8_t(s);
				slot->generation = p.generation;
				slot->kind = kind;
				slot->time_q8 = time_q8;
				slot->tick_a = sel.tick_a;
				slot->tick_b = sel.tick_b;
				slot->target = target;
			} else {
				++m.stats.pose_cache_full;
			}
		}
		m.scratch.push_back(target);
		if (m.trace != nullptr) {
			RewindTraceRow row;
			row.shooter = shooter;
			row.shot_index = p_query.shot_index;
			row.pellet = p_query.pellet;
			row.target = uint8_t(s);
			row.time_q8 = time_q8;
			row.pose = pose;
			m.trace->push_back(row);
		}
	}
	m.stats.answered += m.scratch.size();
	return m.scratch;
}

const LagCompStats &RewindProvider::stats() const {
	Impl &m = *impl_;
	if (m.p99_dirty) {
		m.stats.tick_ms_p99 = p99_of(m.tick_ms);
		m.stats.tick_ms_mean = m.stats.ticks > 0 && !m.tick_ms.empty() ? m.sum_ms / double(std::max<uint64_t>(1, m.stats.ticks)) : 0.0;
		m.p99_dirty = false;
	}

	m.stats.acks_ignored = m.history->stats().acks_ignored;
	m.stats.history_misses = m.history->stats().history_misses;
	return m.stats;
}

}
