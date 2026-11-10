#include "bf3/audio/flyby_audio.h"

#include "bf3/core/math.h"
#include "bf3/core/util/finite.h"
#include "bf3/present/effect_seed.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace bf3::audio {

namespace ca = core::audio;
namespace vec = core::vec;
namespace sw = sim::weapons;

using bf3::core::finite;

float closest_fraction(const Vec3 &p_from, const Vec3 &p_to, const Vec3 &p_point) {
	const Vec3 d = vec::sub(p_to, p_from);
	const float len2 = vec::dot(d, d);
	if (!(len2 > 0.0f) || !std::isfinite(len2)) {
		return 0.0f;
	}
	return std::clamp(vec::dot(vec::sub(p_point, p_from), d) / len2, 0.0f, 1.0f);
}

FlyByCheck check_fly_by_step(const Vec3 &p_from, const Vec3 &p_to, double p_seconds, const Vec3 &p_listener,
		float p_radius_m, float p_min_speed) {
	FlyByCheck c;
	const Vec3 d = vec::sub(p_to, p_from);
	const float len2 = vec::dot(d, d);
	if (!(p_seconds > 0.0) || !(p_radius_m != 0.0f) || !(len2 > 0.0f) || !std::isfinite(len2)) {
		return c;
	}
	c.fraction = vec::dot(vec::sub(p_listener, p_from), d) / len2;
	if (!(c.fraction > 0.0f)) {
		c.step = FlyByStep::Behind;
		return c;
	}
	if (!(c.fraction < 1.0f)) {
		c.step = FlyByStep::Ahead;
		return c;
	}
	c.point = vec::add(p_from, vec::scale(d, c.fraction));
	const float miss2 = vec::dot(vec::sub(p_listener, c.point), vec::sub(p_listener, c.point));
	c.distance_m = std::sqrt(miss2);
	const Vec3 to_from = vec::sub(p_listener, p_from), to_to = vec::sub(p_listener, p_to);
	if (!(miss2 < p_radius_m * p_radius_m) || !(miss2 < vec::dot(to_from, to_from)) ||
			!(miss2 < vec::dot(to_to, to_to))) {
		c.step = FlyByStep::Outside;
		return c;
	}
	if (!(double(std::sqrt(len2)) / p_seconds >= double(p_min_speed))) {
		c.step = FlyByStep::Slow;
		return c;
	}
	c.step = FlyByStep::Plays;
	return c;
}

namespace {

uint64_t shot_key(uint8_t p_slot, uint8_t p_pellet, uint32_t p_shot) {
	return (uint64_t(p_slot) << 40) | (uint64_t(p_pellet) << 32) | uint64_t(p_shot);
}

uint64_t bullet_key(const present::PresentedBullet &p_b) {
	return shot_key(p_b.slot, p_b.pellet, p_b.shot_index);
}

}

struct FlyByAudio::Impl {
	struct Track {
		uint64_t key = 0;
		Vec3 last;
		sw::SimTime last_time = 0;
		uint64_t seen = 0;
		bool done = false;
		bool approaching = false;
	};

	MixerCore &mixer;
	std::vector<FlyBySound> sounds;
	std::shared_ptr<const ca::WaveBank> bank;
	AudioTuning tuning;
	OneShotPool &pool;
	ListenerState listener;
	FlyByAudioStats stats;
	std::vector<Track> tracks;
	std::vector<Track> fresh;
	std::array<uint64_t, kFlyByOwnHits> own_hits{};
	size_t own_hit_count = 0, own_hit_next = 0;
	uint64_t frame = 0;

	bool hit_own(uint64_t p_key) const {
		return std::find(own_hits.begin(), own_hits.begin() + std::ptrdiff_t(own_hit_count), p_key) !=
				own_hits.begin() + std::ptrdiff_t(own_hit_count);
	}

	Impl(MixerCore &r_mixer, std::vector<FlyBySound> p_sounds, std::shared_ptr<const ca::WaveBank> p_bank,
			const AudioTuning &p_tuning, OneShotPool &r_pool) :
			mixer(r_mixer), sounds(std::move(p_sounds)), bank(std::move(p_bank)), tuning(p_tuning), pool(r_pool) {
		tracks.reserve(kMaxFlyByTracks);
		fresh.reserve(present::kMaxPresentedBullets);
	}

	const FlyBySound *sound_of(uint16_t p_def) const {
		if (p_def >= sounds.size()) {
			return nullptr;
		}
		const FlyBySound &s = sounds[p_def];
		return s.program && s.radius_m > 0.0f ? &s : nullptr;
	}

	Track *find(uint64_t p_key) {
		const auto it = std::lower_bound(
				tracks.begin(), tracks.end(), p_key, [](const Track &t, uint64_t k) { return t.key < k; });
		return it != tracks.end() && it->key == p_key ? &*it : nullptr;
	}

	void pass(Track &r_track, const present::PresentedBullet &p_b, const FlyBySound &p_sound, Vec3 p_from,
			sw::SimTime p_from_time, uint64_t p_match_seed, SimMixerClock &r_clock) {
		const Vec3 to = core::to_vec3(p_b.position);
		r_track.last = to;
		r_track.last_time = p_b.time;
		if (r_track.done || !finite(listener.position)) {
			return;
		}
		const double seconds = double(p_b.time - p_from_time) / double(sw::kTimeUnitsPerSecond);
		const FlyByCheck c =
				check_fly_by_step(p_from, to, seconds, listener.position, p_sound.radius_m, p_sound.min_speed);
		switch (c.step) {
			case FlyByStep::Still:
			case FlyByStep::Ahead:
				r_track.approaching = r_track.approaching || c.step == FlyByStep::Ahead;
				if (p_b.ended) {
					r_track.done = true;
					++stats.stopped;
				}
				return;
			case FlyByStep::Behind:
				r_track.done = true;
				stats.receding += r_track.approaching ? 0 : 1;
				return;
			case FlyByStep::Outside:
				r_track.done = true;
				return;
			case FlyByStep::Slow:
				r_track.done = true;
				++stats.slow;
				return;
			case FlyByStep::Plays:
				r_track.done = true;
				break;
		}
		if (hit_own(r_track.key)) {
			++stats.own_hits;
			return;
		}
		const float u = c.fraction;
		const float distance = c.distance_m;
		EmitterState em;
		em.position = c.point;
		const Vec3 step = vec::sub(to, p_from);
		const float step_len = vec::length(step);
		if (step_len > 0.0f && std::isfinite(step_len)) {
			em.forward = vec::scale(step, 1.0f / step_len);
			if (seconds > 0.0) {
				em.velocity = vec::scale(step, float(1.0 / seconds));
			}
		}
		const sw::SimTime when = p_from_time + sw::SimTime(std::llround(double(u) * double(p_b.time - p_from_time)));
		const uint64_t seed = present::impact_seed(
				present::effect_seed(p_match_seed, p_b.slot, p_b.shot_index), p_b.pellet, kFlyByOrdinal);
		OneShotOptions o;
		o.shakes_camera = true;
		if (!pool.start(mixer, p_sound.program, bank, tuning, seed, em, listener,
					r_clock.frame_of(when, present::PresentTimeline::Render), o)) {
			++stats.refused;
			return;
		}
		++stats.starts;
		stats.farthest_start_m = std::max(stats.farthest_start_m, distance);
	}
};

FlyByAudio::FlyByAudio(MixerCore &r_mixer, std::vector<FlyBySound> p_sounds, std::shared_ptr<const ca::WaveBank> p_bank,
		const AudioTuning &p_tuning, OneShotPool &r_pool) :
		impl_(std::make_unique<Impl>(r_mixer, std::move(p_sounds), std::move(p_bank), p_tuning, r_pool)) {}

FlyByAudio::~FlyByAudio() = default;

void FlyByAudio::set_listener(const ListenerState &p_listener) {
	impl_->listener = p_listener;
}

void FlyByAudio::on_events(std::span<const present::PresentEvent> p_events) {
	Impl &m = *impl_;
	for (const present::PresentEvent &e : p_events) {
		if (e.kind != present::PresentKind::Hit || (e.flags & present::flags::kOwnVictim) == 0) {
			continue;
		}
		const uint64_t key = shot_key(e.slot, e.pellet, e.id);
		if (m.hit_own(key)) {
			continue;
		}
		m.own_hits[m.own_hit_next] = key;
		m.own_hit_next = (m.own_hit_next + 1) % m.own_hits.size();
		m.own_hit_count = std::min(m.own_hit_count + 1, m.own_hits.size());
	}
}

void FlyByAudio::on_bullets(
		std::span<const present::PresentedBullet> p_bullets, uint64_t p_match_seed, SimMixerClock &r_clock) {
	Impl &m = *impl_;
	++m.frame;
	m.fresh.clear();
	for (const present::PresentedBullet &b : p_bullets) {
		const FlyBySound *sound = m.sound_of(b.def);
		if (!finite(std::span<const float>(b.origin)) || !finite(std::span<const float>(b.position))) {
			continue;
		}
		const uint64_t key = bullet_key(b);
		if (Impl::Track *t = m.find(key)) {
			t->seen = m.frame;
			if (sound != nullptr) {
				m.pass(*t, b, *sound, t->last, t->last_time, p_match_seed, r_clock);
			}
			continue;
		}
		if (std::any_of(m.fresh.begin(), m.fresh.end(), [&](const Impl::Track &t) { return t.key == key; })) {
			continue;
		}
		if (m.tracks.size() + m.fresh.size() >= kMaxFlyByTracks) {
			++m.stats.untracked;
			continue;
		}
		Impl::Track &t = m.fresh.emplace_back();
		t.key = key;
		t.seen = m.frame;
		t.last = core::to_vec3(b.origin);
		t.last_time = b.spawn_time;
		if (sound == nullptr) {
			++m.stats.silent;
			t.done = true;
			continue;
		}
		++m.stats.bullets;
		m.pass(t, b, *sound, t.last, t.last_time, p_match_seed, r_clock);
	}
	const uint64_t frame = m.frame;
	m.tracks.erase(std::remove_if(m.tracks.begin(), m.tracks.end(),
						   [frame](const Impl::Track &t) { return frame - t.seen > kFlyByTrackFrames; }),
			m.tracks.end());
	m.tracks.insert(m.tracks.end(), m.fresh.begin(), m.fresh.end());
	std::sort(
			m.tracks.begin(), m.tracks.end(), [](const Impl::Track &a, const Impl::Track &b) { return a.key < b.key; });
}

const FlyByAudioStats &FlyByAudio::stats() const {
	return impl_->stats;
}

size_t FlyByAudio::tracked() const {
	return impl_->tracks.size();
}

}
