#include "bf3/host/client_projectiles.h"

#include "bf3/sim/weapons/posed_target.h"
#include "bf3/sim/weapons/weapon_rng.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

namespace bf3::host {

namespace sw = sim::weapons;

namespace {

constexpr double kMsPerTick = 1000.0 / double(sim::kSimHz);
constexpr uint32_t kMaxRemoteTicksPerStep = 8;
constexpr size_t kImpactOrdinals = 256;

sw::TargetPose target_pose_of(const net::OtherPose &p_pose, uint32_t p_id) {
	sw::TargetPose t;
	t.feet = p_pose.position;
	t.yaw = p_pose.yaw;
	t.pitch = p_pose.pitch;
	t.pose = sim::Pose(std::min<uint8_t>(p_pose.pose, uint8_t(sim::kPoseCount - 1)));
	t.target_pose = sim::Pose(std::min<uint8_t>(p_pose.target_pose, uint8_t(sim::kPoseCount - 1)));
	t.transition = float(p_pose.transition) / 255.0f;
	t.id = p_id;
	return t;
}

bool bit(const std::array<uint64_t, 2> &p_mask, uint32_t p_slot) {
	return p_slot < 128 && ((p_mask[p_slot >> 6] >> (p_slot & 63)) & 1u) != 0;
}

}

bool shot_event_of(const net::RemoteShot &p_shot, const sw::WeaponTable &p_table, uint64_t p_match_seed, sw::ShotEvent &r_event) {
	if (p_shot.def >= p_table.defs.size()) {
		return false;
	}
	sw::ShotEvent e;
	e.time = p_shot.time;
	e.spawn_time = p_shot.time + p_table.defs[p_shot.def].spawn_delay;
	e.seed = sw::shot_seed(p_match_seed, p_shot.slot, p_shot.shot_index);
	e.player_slot = p_shot.slot;
	e.shot_index = p_shot.shot_index;
	e.def = p_shot.def;
	e.pellets = std::max<uint8_t>(p_shot.pellets, 1);
	e.origin = p_shot.origin;
	e.aim = p_shot.tracer ? p_shot.aim : p_shot.direction;
	e.direction = p_shot.direction;
	e.pellet_cone = p_shot.pellet_cone;
	e.tracer = p_shot.tracer;
	e.power = p_shot.power;
	r_event = e;
	return true;
}

struct ClientProjectiles::Impl {
	struct View {
		bool valid = false;
		uint32_t shot_index = 0;
		ShotView view;
	};
	struct Ordinal {
		uint32_t shot_index = UINT32_MAX;
		uint8_t slot = 0;
		uint32_t next = 0;
	};

	class OwnProvider final : public sw::HitTargetProvider {
	public:
		Impl *m = nullptr;
		const ReceivedRecordSource *records = nullptr;
		std::span<const sw::HitTarget> targets_for(const sw::TargetQuery &p_query) const override { return m->own_targets(p_query, *records); }
	};

	class RemoteProvider final : public sw::HitTargetProvider {
	public:
		Impl *m = nullptr;
		std::span<const sw::HitTarget> targets_for(const sw::TargetQuery &p_query) const override {
			++m->stats.remote_queries;
			m->scratch.clear();
			for (const sw::HitTarget &t : m->remote_targets) {
				if (t.id != p_query.player_slot) {
					m->scratch.push_back(t);
				}
			}
			return m->scratch;
		}
	};

	std::shared_ptr<const sw::WeaponTable> table;
	std::shared_ptr<const core::gameplay::CapsulePoseTable> capsules;
	uint64_t match_seed = 0;
	ClientProjectilesConfig config;
	std::unique_ptr<sw::ProjectileWorld> own;
	std::unique_ptr<sw::ProjectileWorld> remote;
	std::array<View, kShotViewRing> views{};
	std::deque<sw::ShotEvent> pending;
	sim::Tick remote_tick = 0;
	std::vector<sw::HitTarget> remote_targets;
	mutable std::vector<sw::HitTarget> scratch;
	mutable std::vector<OwnTestSample> samples;
	mutable ClientProjectilesStats stats;
	std::array<Ordinal, kImpactOrdinals> ordinals{};
	sw::ProjectileStepOutput out;

	uint32_t next_ordinal(uint8_t p_slot, uint32_t p_shot) {
		Ordinal &o = ordinals[(size_t(p_shot) * 131u + p_slot) % kImpactOrdinals];
		if (o.shot_index != p_shot || o.slot != p_slot) {
			o.shot_index = p_shot;
			o.slot = p_slot;
			o.next = 0;
		}
		return o.next++;
	}

	std::span<const sw::HitTarget> own_targets(const sw::TargetQuery &p_query, const ReceivedRecordSource &p_records) const {
		++stats.own_queries;
		scratch.clear();
		const View &v = views[p_query.shot_index % kShotViewRing];
		if (!v.valid || v.shot_index != p_query.shot_index || p_query.player_slot != config.own_slot) {
			++stats.own_no_view;
			return scratch;
		}
		const ShotView &sv = v.view;
		uint32_t view_tick = 0;
		if (!p_records.view_tick(sv.view_seq, view_tick)) {
			++stats.own_no_view;
			return scratch;
		}

		const bool present = sv.quality == uint8_t(sim::ViewQuality::None) || sv.quality > uint8_t(sim::ViewQuality::Extrapolated) ||
				net::view_lag_q8(sv.view_back_q8) < 0;
		const int32_t back = present ? 256 : sv.view_back_q8;
		const int64_t time_q8 = net::rewind_time_q8(p_query.time_start, back, config.max_rewind_ticks);
		const double render = double(time_q8) / 256.0;
		const double max_extra = double(std::max(0.0f, config.extrapolate_max_ms)) / kMsPerTick;
		std::array<uint64_t, 2> slots{};
		p_records.slots_in(sv, slots);
		std::array<net::TimedOther, 64> candidates;
		std::array<net::TimedOther, net::kTrackSamples> list;
		for (uint32_t s = 0; s < 128; ++s) {
			if (s == config.own_slot || !bit(slots, s)) {
				continue;
			}
			const size_t c = p_records.candidates(sv, uint8_t(s), candidates);
			const size_t n = net::rebuild_track(std::span<const net::TimedOther>(candidates.data(), std::min(c, candidates.size())), view_tick, list);
			if (n == 0) {
				continue;
			}
			const net::OtherPose pose = net::sample_other(std::span<const net::TimedOther>(list.data(), n), render, config.pose_eye_heights, max_extra);
			scratch.push_back(sw::posed_target(*capsules, target_pose_of(pose, s)));
			if (samples.size() < config.max_samples) {
				OwnTestSample row;
				row.shooter = config.own_slot;
				row.shot_index = p_query.shot_index;
				row.target = uint8_t(s);
				row.time_q8 = time_q8;
				row.pose = pose;
				samples.push_back(row);
				++stats.samples_logged;
			} else {
				++stats.samples_dropped;
			}
		}
		return scratch;
	}

	void push(PresentationEvents &r_events, const PresentEvent &p_event) {
		if (!r_events.push(p_event)) {
			++stats.events_refused;
		}
	}

	void emit_impacts(PresentationEvents &r_events, PresentTimeline p_timeline, bool p_own) {
		for (const sw::ImpactEvent &i : out.impacts) {
			PresentEvent e;
			e.timeline = p_timeline;
			e.slot = uint8_t(i.player_slot);
			e.def = i.def;
			e.id = i.shot_index;
			e.pellet = i.pellet;
			e.time = i.time;
			for (size_t k = 0; k < 3; ++k) {
				e.pos[k] = i.position[k];
				e.dir[k] = i.normal[k];
			}
			if (i.kind == sw::ImpactKind::Static) {
				e.kind = PresentKind::Impact;
				e.material = i.physics_material;
				e.extra = next_ordinal(e.slot, i.shot_index);
				e.seed = impact_seed(effect_seed(match_seed, e.slot, i.shot_index), i.pellet, e.extra);
				++(p_own ? stats.own_impacts : stats.remote_impacts);
			} else if (i.kind == sw::ImpactKind::Target) {
				e.kind = PresentKind::SoldierContact;
				e.aux = uint8_t(i.target);
				e.value = float(uint8_t(i.zone));
				++(p_own ? stats.own_soldier_contacts : stats.remote_soldier_contacts);
			} else {
				continue;
			}
			push(r_events, e);
		}
		stats.detonations_suppressed += out.detonations.size();
	}
};

ClientProjectiles::ClientProjectiles() :
		impl_(std::make_unique<Impl>()) {}

ClientProjectiles::~ClientProjectiles() = default;

core::Result<std::unique_ptr<ClientProjectiles>> ClientProjectiles::create(std::shared_ptr<const sw::WeaponTable> p_table,
		std::shared_ptr<const core::gameplay::CapsulePoseTable> p_capsules, uint64_t p_match_seed) {
	return create(std::move(p_table), std::move(p_capsules), p_match_seed, ClientProjectilesConfig{});
}

core::Result<std::unique_ptr<ClientProjectiles>> ClientProjectiles::create(std::shared_ptr<const sw::WeaponTable> p_table,
		std::shared_ptr<const core::gameplay::CapsulePoseTable> p_capsules, uint64_t p_match_seed, const ClientProjectilesConfig &p_config) {
	if (p_table == nullptr) {
		return core::make_error("ClientProjectiles: no weapon table");
	}
	if (p_capsules == nullptr || !core::gameplay::capsule_pose_table_valid(*p_capsules)) {
		return core::make_error("ClientProjectiles: no valid capsule pose table");
	}
	std::unique_ptr<ClientProjectiles> c(new ClientProjectiles());
	Impl &m = *c->impl_;
	m.table = p_table;
	m.capsules = std::move(p_capsules);
	m.match_seed = p_match_seed;
	m.config = p_config;
	m.config.max_samples = std::min<size_t>(p_config.max_samples, size_t(1) << 22);
	m.config.max_remote_pending = std::clamp<size_t>(p_config.max_remote_pending, 1, 1u << 16);
	m.own = std::make_unique<sw::ProjectileWorld>(p_table, p_config.limits);
	m.remote = std::make_unique<sw::ProjectileWorld>(p_table, p_config.limits);
	m.scratch.reserve(128);
	return c;
}

void ClientProjectiles::spawn_own(const sw::ShotEvent &p_shot, const ShotView &p_view) {
	Impl &m = *impl_;
	Impl::View &v = m.views[p_shot.shot_index % kShotViewRing];
	v.valid = true;
	v.shot_index = p_shot.shot_index;
	v.view = p_view;
	if (m.own->spawn(p_shot).ok()) {
		++m.stats.own_spawned;
	} else {
		++m.stats.refused;
	}
}

void ClientProjectiles::queue_remote(const net::RemoteShot &p_shot) {
	Impl &m = *impl_;
	sw::ShotEvent e;
	if (!shot_event_of(p_shot, *m.table, m.match_seed, e) || m.pending.size() >= m.config.max_remote_pending) {
		++m.stats.refused;
		return;
	}
	m.pending.push_back(e);
}

void ClientProjectiles::step_own(sim::Tick p_tick, const sim::CollisionWorld &p_world, const ReceivedRecordSource &p_records, PresentationEvents &r_events) {
	Impl &m = *impl_;
	Impl::OwnProvider provider;
	provider.m = &m;
	provider.records = &p_records;
	m.out.clear();
	m.own->step(p_tick, p_world, provider, m.out);
	m.emit_impacts(r_events, PresentTimeline::Predicted, true);
	m.stats.live_own = uint32_t(m.own->live().size());
}

void ClientProjectiles::step_remote(double p_render_ticks, const sim::CollisionWorld &p_world, std::span<const net::OtherPose> p_others,
		PresentationEvents &r_events) {
	Impl &m = *impl_;
	if (!std::isfinite(p_render_ticks) || p_render_ticks < 1.0) {
		return;
	}
	const sim::Tick target = sim::Tick(std::min(std::floor(p_render_ticks), 4.0e9));
	if (m.remote_tick == 0 || target > m.remote_tick + kMaxRemoteTicksPerStep || target + 600u < m.remote_tick) {
		m.remote_tick = target > 0 ? target - 1u : 0u;
	}
	if (target <= m.remote_tick) {
		return;
	}
	m.remote_targets.clear();
	for (const net::OtherPose &o : p_others) {
		m.remote_targets.push_back(sw::posed_target(*m.capsules, target_pose_of(o, o.slot)));
	}
	Impl::RemoteProvider provider;
	provider.m = &m;
	for (sim::Tick t = m.remote_tick + 1u; t <= target; ++t) {
		const sw::SimTime end = sw::tick_end(t);

		while (!m.pending.empty() && m.pending.front().spawn_time < end) {
			const sw::ShotEvent &e = m.pending.front();
			if (e.spawn_time < sw::tick_start(t)) {
				++m.stats.remote_late;
			}
			if (m.remote->spawn(e).ok()) {
				++m.stats.remote_spawned;
			} else {
				++m.stats.refused;
			}
			m.pending.pop_front();
		}
		m.out.clear();
		m.remote->step(t, p_world, provider, m.out);
		m.emit_impacts(r_events, PresentTimeline::Render, false);
	}
	m.remote_tick = target;
	m.stats.live_remote = uint32_t(m.remote->live().size());
}

size_t ClientProjectiles::take_samples(std::span<OwnTestSample> r_out) {
	Impl &m = *impl_;
	const size_t n = std::min(r_out.size(), m.samples.size());
	std::copy(m.samples.begin(), m.samples.begin() + std::ptrdiff_t(n), r_out.begin());
	m.samples.erase(m.samples.begin(), m.samples.begin() + std::ptrdiff_t(n));
	return n;
}

const ClientProjectilesStats &ClientProjectiles::stats() const {
	return impl_->stats;
}

}
