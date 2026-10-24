#include "client_tracks.h"

#include "client_ticks.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

namespace {

constexpr size_t kTrackSamples = 8;
constexpr uint64_t kTrackTimeoutNs = 1'000'000'000ull;

double local_ticks(uint64_t p_ns) {
	return double(p_ns) / kNsPerTick;
}

}

void RemoteTracks::arrival(sim::Tick p_server_tick, uint64_t p_now) {
	const double o = double(p_server_tick) - local_ticks(p_now);
	offsets_.push_back({ p_now, o });
	while (!offsets_.empty() && p_now - offsets_.front().first > 1'000'000'000ull) {
		offsets_.pop_front();
	}
	if (!recv_valid_) {
		recv_offset_ = o;
		recv_valid_ = true;
	}
	if (last_arrival_ns_ != 0) {
		const double interval = double(p_now - last_arrival_ns_) / 1e6;
		if (interval_mean_ms_ == 0.0) {
			interval_mean_ms_ = interval;
		}
		interval_mean_ms_ += (interval - interval_mean_ms_) / 16.0;
		interval_mad_ms_ += (std::fabs(interval - interval_mean_ms_) - interval_mad_ms_) / 16.0;
	}
	last_arrival_ns_ = p_now;
}

void RemoteTracks::add(sim::Tick p_server_tick, std::span<const net::OtherSoldier> p_others, uint8_t p_own_slot, uint64_t p_now) {
	for (const net::OtherSoldier &o : p_others) {
		if (o.slot >= tracks_.size() || o.slot == p_own_slot) {
			continue;
		}
		Track &t = tracks_[o.slot];
		if (!t.used || t.generation != o.generation) {
			t = Track();
			t.used = true;
			t.generation = o.generation;
		}
		t.last_seen_ns = p_now;
		OtherSample s;
		s.server_tick = p_server_tick;
		s.soldier = o;
		auto at = std::lower_bound(t.samples.begin(), t.samples.end(), s.server_tick,
				[](const OtherSample &a, sim::Tick b) { return a.server_tick < b; });
		if (at != t.samples.end() && at->server_tick == s.server_tick) {
			continue;
		}
		t.samples.insert(at, s);
		while (t.samples.size() > kTrackSamples) {
			t.samples.pop_front();
		}
	}
}

void RemoteTracks::reorder(sim::Tick p_tick, std::span<const net::OtherSoldier> p_records, uint8_t p_own_slot) {
	for (const net::OtherSoldier &o : p_records) {
		if (o.slot >= tracks_.size() || o.slot == p_own_slot) {
			continue;
		}
		const Track &t = tracks_[o.slot];
		if (t.used && t.generation != o.generation && !t.samples.empty() && p_tick < t.samples.back().server_tick) {
			++generation_reorder_;
		}
	}
}

void RemoteTracks::count_timeouts(uint64_t p_now) {
	for (const Track &t : tracks_) {
		if (t.used && !t.samples.empty() && p_now > t.last_seen_ns + kTrackTimeoutNs) {
			++track_timeout_;
		}
	}
}

void RemoteTracks::publish(PresentationFrame &r_frame, uint64_t p_now) {
	double recv_rate = 0;
	if (recv_valid_ && !offsets_.empty()) {
		double lo = offsets_.front().second;
		for (const auto &o : offsets_) {
			lo = std::min(lo, o.second);
		}
		const double step = 0.05;
		const double diff = lo - recv_offset_;
		const double moved = std::clamp(diff, -step, step);
		recv_offset_ += moved;
		recv_rate = moved / 1.0;
	}
	const double snapshot_interval_ms = double(divisor_) * kMsPerTick;
	delay_ms_ = std::clamp(2.0 * snapshot_interval_ms + interval_mad_ms_, double(config_.interp_min_ms),
			double(std::max(config_.interp_min_ms, config_.interp_max_ms)));
	r_frame.render_ticks_at_publish = local_ticks(p_now) + recv_offset_ - delay_ms_ / kMsPerTick;
	r_frame.ticks_per_ns = (1.0 + std::clamp(recv_rate, -0.05, 0.05)) / kNsPerTick;
	uint32_t n = 0;
	for (size_t i = 0; i < tracks_.size(); ++i) {
		Track &t = tracks_[i];
		if (!t.used) {
			continue;
		}
		if (p_now > t.last_seen_ns + kTrackTimeoutNs || t.samples.empty()) {
			t = Track();
			continue;
		}
		size_t first = 0;
		while (first + 1 < t.samples.size() && double(t.samples[first + 1].server_tick) <= r_frame.render_ticks_at_publish) {
			++first;
		}
		OtherTrack &o = r_frame.others[n++];
		o.slot = uint8_t(i);
		o.count = 0;
		for (size_t k = first; k < t.samples.size() && o.count < kOtherSamples; ++k) {
			o.samples[o.count++] = t.samples[k];
		}
	}
	r_frame.other_count = n;
}

std::span<const net::OtherPose> RemoteTracks::finish(PresentationFrame &r_frame, bool p_have_snapshot, sim::Tick p_newest_tick, bool p_poses) {
	bool ahead = false;
	poses_.clear();
	const uint32_t n = std::min<uint32_t>(r_frame.other_count, uint32_t(r_frame.others.size()));
	const double max_extra_ticks = std::max(0.0, double(config_.extrapolate_max_ms)) / kMsPerTick;
	for (uint32_t i = 0; i < n; ++i) {
		OtherTrack &o = r_frame.others[i];
		const Track &t = tracks_[o.slot];
		o.track_newest_tick = t.samples.empty() ? 0u : t.samples.back().server_tick;
		if (p_have_snapshot && o.track_newest_tick > p_newest_tick) {
			ahead = true;
		}
		if (p_poses && !t.samples.empty()) {
			scratch_.clear();
			for (const OtherSample &x : t.samples) {
				scratch_.push_back({ x.server_tick, x.soldier });
			}
			poses_.push_back(net::sample_other(scratch_, r_frame.render_ticks_at_publish, r_frame.pose_eye_heights, max_extra_ticks));
		}
	}
	if (ahead) {
		++others_ahead_;
	}
	return poses_;
}

void RemoteTracks::stats(ClientStats &r_stats) const {
	r_stats.generation_reorder = generation_reorder_;
	r_stats.others_ahead = others_ahead_;
	r_stats.track_timeout = track_timeout_;
}

}
