#include "bf3/host/own_hits.h"

namespace bf3::host {

OwnHitLedger::Shot &OwnHitLedger::at(uint32_t p_shot) {
	Shot &s = shots_[p_shot % kShots];
	if (!s.used || s.shot != p_shot) {
		s = Shot{};
		s.used = true;
		s.shot = p_shot;
	}
	return s;
}

void OwnHitLedger::predicted(uint32_t p_shot, uint8_t p_target, uint8_t p_zone) {
	Shot &s = at(p_shot);
	if (s.predicted || s.confirmed || s.unpredicted) {
		return;
	}
	s.predicted = true;
	s.target = p_target;
	s.zone = p_zone;
	++counts_.predicted;
}

void OwnHitLedger::confirmed(uint32_t p_shot, uint8_t p_victim, uint8_t p_zone) {
	Shot &s = at(p_shot);
	if (s.predicted && s.target == p_victim) {
		if (!s.confirmed) {
			s.confirmed = true;
			++counts_.confirmed;
			counts_.same_zone += s.zone == p_zone ? 1u : 0u;
		}
		return;
	}
	if (!s.unpredicted) {
		s.unpredicted = true;
		++counts_.unpredicted;
	}
}

}
