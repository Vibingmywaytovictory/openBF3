#include "bf3/host/round_stats.h"

#include <algorithm>
#include <cmath>

namespace bf3::host {

namespace {

int8_t team_of(const RoundStatsInput &p_input, uint8_t p_slot) {
	return p_slot < p_input.teams.size() ? p_input.teams[p_slot] : int8_t(0);
}

void bump(uint16_t &r_count) {
	if (r_count < UINT16_MAX) {
		++r_count;
	}
}

void own_kill(RoundStats &r_stats, const RoundStatsInput &p_input, const PresentEvent &p_event, uint8_t p_victim) {
	++r_stats.own_kills;
	++r_stats.streak;
	r_stats.longest_streak = std::max(r_stats.longest_streak, r_stats.streak);
	r_stats.longest_nemesis_streak =
			std::max<uint32_t>(r_stats.longest_nemesis_streak, r_stats.nemesis[r_stats.own_slot][p_victim]);
	if (p_event.def < kRoundStatsDefs) {
		bump(r_stats.own_weapon_kills[p_event.def]);
	}
	if ((p_event.flags & present_flags::kHeadshot) == 0) {
		return;
	}
	++r_stats.own_headshots;
	for (const present::PresentedSoldier &s : p_input.others) {
		if (s.slot != p_victim) {
			continue;
		}
		const float dx = s.position[0] - p_input.own_position[0];
		const float dy = s.position[1] - p_input.own_position[1];
		const float dz = s.position[2] - p_input.own_position[2];
		const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (std::isfinite(d)) {
			r_stats.longest_headshot_m = std::max(r_stats.longest_headshot_m, d);
		}
		return;
	}
}

}

void round_stats_reset(RoundStats &r_stats, uint8_t p_own_slot) {
	r_stats = RoundStats{};
	r_stats.own_slot = p_own_slot < kRoundStatsSlots ? p_own_slot : kRoundStatsNoSlot;
}

void round_stats_add(RoundStats &r_stats, const RoundStatsInput &p_input, std::span<const PresentEvent> p_events) {
	if (p_input.own_slot != r_stats.own_slot) {
		round_stats_reset(r_stats, p_input.own_slot);
	}
	const uint8_t own = r_stats.own_slot;
	for (const PresentEvent &e : p_events) {
		if (e.kind != PresentKind::Death) {
			continue;
		}
		const uint8_t victim = e.aux;
		if (victim >= kRoundStatsSlots) {
			++r_stats.bad_events;
			continue;
		}
		++r_stats.deaths_seen;
		const bool suicide = e.slot == victim || e.slot >= kRoundStatsSlots;
		const uint8_t killer = suicide ? victim : e.slot;
		const int8_t killer_team = team_of(p_input, killer);
		const bool team_kill = !suicide && killer_team != 0 && killer_team == team_of(p_input, victim);
		if (!suicide && !team_kill) {
			bump(r_stats.kills[killer][victim]);
			bump(r_stats.nemesis[killer][victim]);
			r_stats.nemesis[victim][killer] = 0;
		}
		if (own == kRoundStatsNoSlot) {
			continue;
		}
		if (victim == own) {
			++r_stats.own_deaths;
			r_stats.own_suicides += suicide ? 1u : 0u;
			r_stats.streak = 0;
		} else if (killer == own && !suicide && !team_kill) {
			own_kill(r_stats, p_input, e, victim);
		}
	}
}

uint16_t round_stats_kills(const RoundStats &p_stats, uint8_t p_killer, uint8_t p_victim) {
	return p_killer < kRoundStatsSlots && p_victim < kRoundStatsSlots ? p_stats.kills[p_killer][p_victim] : uint16_t(0);
}

uint16_t round_stats_nemesis(const RoundStats &p_stats, uint8_t p_killer, uint8_t p_victim) {
	return p_killer < kRoundStatsSlots && p_victim < kRoundStatsSlots ? p_stats.nemesis[p_killer][p_victim]
																	  : uint16_t(0);
}

RoundBestWeapon round_stats_best_weapon(const RoundStats &p_stats) {
	RoundBestWeapon best;
	for (size_t d = 0; d < p_stats.own_weapon_kills.size(); ++d) {
		if (p_stats.own_weapon_kills[d] > best.kills) {
			best.def = uint16_t(d);
			best.kills = p_stats.own_weapon_kills[d];
		}
	}
	return best;
}

float round_stats_per_minute(double p_count, double p_seconds) {
	if (!(p_seconds > 0.0) || !std::isfinite(p_seconds) || !std::isfinite(p_count)) {
		return 0.0f;
	}
	return float(p_count * 60.0 / p_seconds);
}

}
