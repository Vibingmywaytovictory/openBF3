#pragma once

#include "bf3/host/gunplay.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/soldier_state.h"
#include "server_combat.h"
#include "server_match.h"
#include "server_outbox.h"
#include "server_spawner.h"

#include <cstdint>

namespace bf3::host {

class CombatLog final : public CombatEventSink {
public:
	CombatLog(Spawner &r_spawner, MatchControl &r_match, const Combat &p_combat) :
			spawner_(r_spawner), match_(r_match), combat_(p_combat) {}

	void open(sim::Tick p_tick) { tick_ = p_tick; }

	void hit(const net::HitEvent &p_event) override;
	void death(const net::DeathEvent &p_event) override;
	void revive(const net::ReviveEvent &p_event) override;
	void detonation(const net::DetonationWire &p_event) override;

	void send(uint32_t p_peer, sim::Tick p_server_tick, Gunplay &r_gunplay, Outbox &r_outbox) const;
	void clear() { events_ = net::CombatEvents(); }
	const net::CombatEvents &events() const { return events_; }

private:
	Spawner &spawner_;
	MatchControl &match_;
	const Combat &combat_;
	sim::Tick tick_ = 0;
	net::CombatEvents events_;
};

}
