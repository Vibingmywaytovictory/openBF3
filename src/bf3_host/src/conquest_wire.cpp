#include "bf3/host/conquest_wire.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bf3::host {

namespace lg = bf3::sim::logic;

namespace {

uint16_t wire_tickets(int64_t p_tickets) {
	return uint16_t(std::clamp<int64_t>(p_tickets, 0, std::numeric_limits<uint16_t>::max()));
}

uint16_t wire_progress(float p_progress) {
	if (!std::isfinite(p_progress)) {
		return 0;
	}
	const long q = std::lround(double(p_progress) * double(net::kConquestProgressScale));
	return uint16_t(std::clamp<long>(q, 0, long(net::kConquestProgressMax)));
}

int16_t wire_rate(float p_rate) {
	if (!std::isfinite(p_rate)) {
		return 0;
	}
	const long q = std::lround(double(p_rate) * double(net::kConquestProgressScale));
	return int16_t(std::clamp<long>(q, std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max()));
}

bool same_but_progress(const net::ConquestState &p_a, const net::ConquestState &p_b) {
	if (p_a.tickets != p_b.tickets || p_a.initial_tickets != p_b.initial_tickets || p_a.bleeding != p_b.bleeding ||
			p_a.point_count != p_b.point_count || p_a.struck != p_b.struck) {
		return false;
	}
	const int64_t back = int64_t(p_a.return_by_tick) - int64_t(p_b.return_by_tick);
	if ((p_a.return_by_tick == 0) != (p_b.return_by_tick == 0) || back > 1 || back < -1) {
		return false;
	}
	for (size_t i = 0; i < p_a.point_count && i < net::kMaxConquestPoints; ++i) {
		const net::ConquestPointWire &a = p_a.points[i];
		const net::ConquestPointWire &b = p_b.points[i];
		if (a.team != b.team || a.attacker != b.attacker || a.controlled != b.controlled ||
				a.under_attack != b.under_attack || (a.rate == 0) != (b.rate == 0) || (a.rate < 0) != (b.rate < 0)) {
			return false;
		}
	}
	return true;
}

}

net::ConquestState conquest_wire_state(const lg::ConquestNatives &p_conquest, sim::Tick p_tick) {
	net::ConquestState s;
	s.server_tick = p_tick;
	for (uint8_t t = 1; t <= net::kMaxTeam; ++t) {
		s.tickets[t - 1u] = wire_tickets(p_conquest.tickets(t));
		s.initial_tickets[t - 1u] = wire_tickets(p_conquest.initial_tickets(t));
		s.bleeding[t - 1u] = p_conquest.bleeding(t);
	}
	const std::span<const lg::ConquestPointView> points = p_conquest.points();
	s.point_count = uint8_t(std::min(points.size(), net::kMaxConquestPoints));
	for (size_t i = 0; i < s.point_count; ++i) {
		const lg::ConquestPointView &v = points[i];
		net::ConquestPointWire &w = s.points[i];
		w.team = v.team <= net::kMaxTeam ? v.team : uint8_t(0);
		w.attacker = v.attacker <= net::kMaxTeam ? v.attacker : uint8_t(0);
		w.progress = wire_progress(v.progress);
		w.rate = wire_rate(v.rate);
		w.controlled = v.controlled;
		w.under_attack = v.under_attack;
	}
	for (uint8_t t = 1; t <= net::kMaxTeam; ++t) {
		for (const lg::Deserter &d : p_conquest.deserters(t)) {
			if (d.struck) {
				s.strike(d.slot);
			}
		}
	}
	return s;
}

sim::Tick conquest_return_by(const lg::ConquestNatives &p_conquest, sim::Tick p_tick, uint8_t p_slot) {
	for (uint8_t t = 1; t <= net::kMaxTeam; ++t) {
		for (const lg::Deserter &d : p_conquest.deserters(t)) {
			if (d.slot == p_slot && std::isfinite(d.timer)) {
				const long ticks = std::lround(std::max(0.0, double(d.timer)) * double(sim::kSimHz));
				return std::max<sim::Tick>(p_tick + sim::Tick(ticks), 1u);
			}
		}
	}
	return 0;
}

bool conquest_resend(
		const net::ConquestState &p_sent, const net::ConquestState &p_now, sim::Tick p_sent_tick, sim::Tick p_tick) {
	if (!same_but_progress(p_sent, p_now)) {
		return true;
	}
	for (size_t i = 0; i < p_now.point_count && i < net::kMaxConquestPoints; ++i) {
		if (p_sent.points[i].progress != p_now.points[i].progress || p_sent.points[i].rate != p_now.points[i].rate) {
			return p_tick - p_sent_tick >= kConquestProgressIntervalTicks;
		}
	}
	return false;
}

}
