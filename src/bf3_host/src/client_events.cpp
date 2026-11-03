#include "client_events.h"

namespace bf3::host {

void ClientEvents::push(const PresentEvent &p_event) {
	PresentEvent e = p_event;
	present_own_shot_id(shot_ids_, own_slot_, e);
	count_own_hit(e);
	if (e.kind == PresentKind::Spawn && e.slot == own_slot_ && e.id != 0 && revived_lives_[e.id % revived_lives_.size()] == e.id) {
		e.flags |= present_flags::kRevived;
	}
	if (!ring_.push(e)) {
		++overflow_;
	}
}

void ClientEvents::count_own_hit(const PresentEvent &p_event) {
	if (p_event.slot != own_slot_) {
		return;
	}
	if (p_event.kind == PresentKind::SoldierContact && p_event.timeline == PresentTimeline::Predicted) {
		own_hits_.predicted(p_event.id, p_event.aux, uint8_t(p_event.value));
	} else if (p_event.kind == PresentKind::Hit && (p_event.flags & present_flags::kOwnShooter) != 0) {
		own_hits_.confirmed(p_event.id, p_event.aux, uint8_t(p_event.material));
	}
}

void ClientEvents::drain_own() {
	std::array<PresentEvent, 64> batch;
	size_t got = 0;
	while ((got = own_.drain(batch)) > 0) {
		for (size_t k = 0; k < got; ++k) {
			push(batch[k]);
		}
	}
}

void ClientEvents::spawned(const net::Spawned &p_spawned) {
	uint32_t &life = revived_lives_[p_spawned.state.life % revived_lives_.size()];
	if (p_spawned.revived) {
		life = p_spawned.state.life;
	} else if (life == p_spawned.state.life) {
		life = 0;
	}
}

}
