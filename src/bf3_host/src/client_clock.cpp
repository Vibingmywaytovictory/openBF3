#include "client_clock.h"

#include "client_ticks.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

namespace {

constexpr uint32_t kMaxBurstTicks = 8;

}

double ClientClock::ticks(uint64_t p_now) const {
	const double dt = p_now >= base_ns_ ? double(p_now - base_ns_) : -double(base_ns_ - p_now);
	return base_ticks_ + dt / kNsPerTick * rate_scale_;
}

double ClientClock::own_ticks_per_ns() const {
	return rate_scale_ / kNsPerTick;
}

void ClientClock::set(double p_ticks, uint64_t p_now) {
	base_ticks_ = p_ticks;
	base_ns_ = p_now;
}

void ClientClock::set_rate(double p_scale, uint64_t p_now) {
	base_ticks_ = ticks(p_now);
	base_ns_ = p_now;
	rate_scale_ = p_scale;
}

uint64_t ClientClock::tick_deadline_ns(sim::Tick p_tick) const {
	const double dt_ticks = double(p_tick) - base_ticks_;
	const double ns = double(base_ns_) + dt_ticks * kNsPerTick / rate_scale_;
	return ns <= 0.0 ? 0u : uint64_t(ns);
}

double ClientClock::rtt_for_estimate_ms() const {
	return rtt_valid_ ? rtt_min_ms_ : hello_rtt_ms_;
}

double ClientClock::estimate(uint64_t p_now) const {
	const double since = p_now >= anchor_ns_ ? double(p_now - anchor_ns_) / kNsPerTick : 0.0;
	return double(anchor_tick_) + since + rtt_for_estimate_ms() / kMsPerTick + double(target_);
}

void ClientClock::begin_holdoff(uint32_t p_next_sequence) {
	holdoff_ = true;
	holdoff_sequence_ = p_next_sequence;
	reports8_.clear();
	reports64_.clear();
}

void ClientClock::start(sim::Tick p_server_tick, uint64_t p_now, uint32_t p_next_sequence) {
	hello_rtt_ms_ = hello_ns_ != 0 && p_now > hello_ns_ ? double(p_now - hello_ns_) / 1e6 : 0.0;
	anchor_tick_ = p_server_tick;
	anchor_ns_ = p_now;
	set(estimate(p_now) + double(config_.initial_clock_offset_ticks), p_now);
	next_tick_ = sim::Tick(std::max(0.0, std::floor(ticks(p_now))));
	running_ = true;
	begin_holdoff(p_next_sequence);
}

void ClientClock::anchor_spawned(sim::Tick p_tick, uint64_t p_now) {
	if (p_tick > anchor_tick_) {
		anchor_tick_ = p_tick;
		anchor_ns_ = p_now;
	}
}

void ClientClock::anchor_snapshot(sim::Tick p_tick, uint64_t p_now) {
	if (p_tick >= anchor_tick_) {
		anchor_tick_ = p_tick;
		anchor_ns_ = p_now;
	}
}

void ClientClock::command_sent(uint32_t p_sequence, uint64_t p_now) {
	sent_times_[p_sequence % kSentTimes] = { p_sequence, p_now };
}

void ClientClock::rtt_sample(const net::Snapshot &p_snap, uint64_t p_now, uint32_t p_next_sequence) {
	if (p_next_sequence <= 1) {
		return;
	}
	const uint32_t newest = p_next_sequence - 1u;
	uint32_t seq = (newest & ~0xFFFFu) | p_snap.sequence_echo;
	if (seq > newest) {
		if (seq < 0x10000u) {
			return;
		}
		seq -= 0x10000u;
	}
	const SentTime &t = sent_times_[seq % kSentTimes];
	if (t.sequence != seq || t.ns == 0 || seq == 0) {
		return;
	}
	const uint64_t hold_ns = uint64_t(p_snap.echo_hold_100us) * 100000u;
	const double ms = p_now > t.ns + hold_ns ? double(p_now - t.ns - hold_ns) / 1e6 : 0.0;
	rtt_ms_ = rtt_valid_ ? rtt_ms_ + (ms - rtt_ms_) * 0.1 : ms;
	rtt_window_.push_back({ p_now, ms });
	while (!rtt_window_.empty() && p_now - rtt_window_.front().ns > 2'000'000'000ull) {
		rtt_window_.pop_front();
	}
	double lo = ms;
	for (const RttSample &s : rtt_window_) {
		lo = std::min(lo, s.ms);
	}
	rtt_min_ms_ = lo;
	rtt_valid_ = true;
}

std::optional<sim::Tick> ClientClock::buffer_report(const net::Snapshot &p_snap, uint64_t p_now, uint32_t p_next_sequence) {
	command_buffer_ticks_ = p_snap.command_buffer_ticks;
	if (holdoff_) {
		if (net::seq_newer(uint16_t(holdoff_sequence_ & 0xFFFFu), p_snap.sequence_echo) || p_next_sequence <= holdoff_sequence_) {
			return std::nullopt;
		}
		holdoff_ = false;
	}
	const int32_t report = p_snap.command_buffer_ticks;
	reports64_.push_back(report);
	while (reports64_.size() > 64) {
		reports64_.pop_front();
	}
	reports8_.push_back(report);
	while (reports8_.size() > 8) {
		reports8_.pop_front();
	}

	double mean = 0;
	for (int32_t r : reports64_) {
		mean += r;
	}
	mean /= double(reports64_.size());
	double mad = 0;
	for (int32_t r : reports64_) {
		mad += std::fabs(double(r) - mean);
	}
	mad /= double(reports64_.size());
	const double extra = std::max(0.0, std::ceil(2.0 * mad - 1.0));
	const uint32_t want = std::min<uint32_t>(config_.target_buffer_ticks + uint32_t(extra), std::max(config_.max_buffer_ticks, config_.target_buffer_ticks));
	if (want != candidate_) {
		candidate_ = want;
		candidate_since_ns_ = p_now;
	} else if (want != target_ && double(p_now - candidate_since_ns_) >= double(config_.target_hold_s) * 1e9) {
		target_ = want;
	}

	if (std::fabs(double(report) - double(target_)) * kMsPerTick > double(config_.clock_reset_ms)) {
		const double e = estimate(p_now);
		const sim::Tick c = sim::Tick(std::max(0.0, std::floor(e)));
		std::optional<sim::Tick> discard;
		if (c < next_tick_) {
			discard = c - 1u;
		}
		rate_scale_ = 1.0;
		set(e, p_now);
		next_tick_ = c;
		++clock_resets_;
		begin_holdoff(p_next_sequence);
		return discard;
	}

	double err = 0;
	for (int32_t r : reports8_) {
		err += r;
	}
	err = err / double(reports8_.size()) - double(target_);
	double adjust = 0;
	if (std::fabs(err) > double(config_.clock_deadband_ticks)) {
		adjust = -err / (double(sim::kSimHz) * std::max(0.01, double(config_.clock_time_constant_s)));
		adjust = std::clamp(adjust, -double(config_.max_clock_adjust), double(config_.max_clock_adjust));
	}
	set_rate(1.0 + adjust, p_now);
	return std::nullopt;
}

void ClientClock::force_past(sim::Tick p_tick, uint64_t p_now, uint32_t p_next_sequence) {
	if (p_tick < next_tick_) {
		return;
	}
	const double e = estimate(p_now);
	const sim::Tick c = std::max(sim::Tick(std::max(0.0, std::floor(e))), p_tick + 1u);
	rate_scale_ = 1.0;
	set(std::max(e, double(c)), p_now);
	next_tick_ = c;
	++forced_forward_;
	++clock_resets_;
	begin_holdoff(p_next_sequence);
}

void ClientClock::shift(int32_t p_ticks, uint64_t p_now) {
	set(ticks(p_now) + double(p_ticks), p_now);
	next_tick_ = sim::Tick(int64_t(next_tick_) + p_ticks);
}

bool ClientClock::due(uint64_t p_now) {
	const double clock = ticks(p_now);
	if (!(clock >= double(next_tick_))) {
		return false;
	}
	if (clock - double(next_tick_) > double(kMaxBurstTicks)) {
		next_tick_ = sim::Tick(std::floor(clock));
	}
	return true;
}

void ClientClock::publish_stats(ClientStats &r_stats) const {
	r_stats.client_tick = next_tick_;
	r_stats.target_buffer_ticks = target_;
	r_stats.clock_adjust = float(rate_scale_ - 1.0);
}

void ClientClock::stats(ClientStats &r_stats) const {
	r_stats.rtt_ms = rtt_ms_;
	r_stats.rtt_min_ms = rtt_min_ms_;
	r_stats.command_buffer_ticks = command_buffer_ticks_;
	r_stats.clock_resets = clock_resets_;
	r_stats.forced_forward = forced_forward_;
}

}
