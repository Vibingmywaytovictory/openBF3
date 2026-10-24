#pragma once

#include "bf3/host/client_session.h"
#include "bf3/net/protocol.h"

#include <array>
#include <cstdint>

namespace bf3::host {

constexpr bool valid_slot(uint32_t p_slot) {
	return p_slot < net::kMaxSlots;
}

class ClientEvents {
public:
	explicit ClientEvents(const uint8_t &p_own_slot) :
			own_slot_(p_own_slot) {}

	PresentationEvents &ring() { return ring_; }
	PresentationEvents &own_staging() { return own_; }
	void push(const PresentEvent &p_event);
	void drain_own();
	void spawned(const net::Spawned &p_spawned);

	uint64_t shot_id_resets() const { return shot_ids_.resets(); }
	void stats(ClientStats &r_stats) const { r_stats.events_overflow = overflow_; }

private:
	const uint8_t &own_slot_;
	PresentationEvents ring_;
	PresentationEvents own_;
	OwnShotIds shot_ids_;
	std::array<uint32_t, 8> revived_lives_{};
	uint64_t overflow_ = 0;
};

}
