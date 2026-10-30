#include "server_seats.h"

#include "bf3/core/util/format.h"

#include <algorithm>

namespace bf3::host {

void SeatLoads::configure(size_t p_slots, const LoadDeadlines &p_deadlines, uint32_t p_round_id) {
	seats_.assign(p_slots, SeatLoad());
	stall_ticks_ = std::max<sim::Tick>(sim::seconds_to_ticks(p_deadlines.stall_s), 1u);
	warm_ticks_ = std::max<sim::Tick>(sim::seconds_to_ticks(p_deadlines.warm_s), 1u);
	cold_ticks_ = std::max<sim::Tick>(sim::seconds_to_ticks(p_deadlines.cold_s), 1u);
	round_id_ = p_round_id;
}

void SeatLoads::begin(size_t p_slot, sim::Tick p_now) {
	SeatLoad &s = seats_[p_slot];
	s = SeatLoad();
	s.loading = true;
	s.since = p_now;
	s.last_progress = p_now;
}

bool SeatLoads::progress(size_t p_slot, const net::Loading &p_message, sim::Tick p_now) {
	SeatLoad &s = seats_[p_slot];
	if (!s.loading || p_message.round_id != round_id_) {
		++stale_;
		return false;
	}
	s.cold = s.cold || p_message.warmth == net::LoadWarmth::Cold;
	const bool further =
			!s.reported || p_message.stage > s.stage || (p_message.stage == s.stage && p_message.permille > s.permille);
	if (further) {
		s.stage = p_message.stage;
		s.permille = p_message.permille;
		s.last_progress = p_now;
	}
	s.reported = true;
	++s.reports;
	return true;
}

void SeatLoads::done(size_t p_slot) {
	seats_[p_slot].loading = false;
}

void SeatLoads::reset(size_t p_slot) {
	seats_[p_slot] = SeatLoad();
}

std::vector<SeatLoads::Due> SeatLoads::due(sim::Tick p_now) const {
	std::vector<Due> out;
	for (size_t i = 0; i < seats_.size(); ++i) {
		const SeatLoad &s = seats_[i];
		if (!s.loading) {
			continue;
		}
		const sim::Tick limit = s.cold ? cold_ticks_ : warm_ticks_;
		if (p_now - s.since >= limit) {
			out.push_back(Due{ i, net::KickReason::LoadTimeout,
					core::fmt("not ready %.1f s after the %s load began", double(p_now - s.since) / double(sim::kSimHz),
							s.cold ? "cold" : "warm") });
		} else if (s.reported && p_now - s.last_progress >= stall_ticks_) {
			out.push_back(Due{ i, net::KickReason::LoadStalled,
					core::fmt("no load progress for %.1f s (stage %u, %u per mille)",
							double(p_now - s.last_progress) / double(sim::kSimHz), unsigned(s.stage),
							unsigned(s.permille)) });
		}
	}
	return out;
}

}
