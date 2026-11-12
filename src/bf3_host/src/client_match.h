#pragma once

#include "bf3/host/client_session.h"
#include "bf3/net/netsim.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"
#include "client_events.h"
#include "client_logic_sync.h"
#include "vehicles/client_vehicles.h"

#include <cstdint>
#include <vector>

namespace bf3::host {

class ClientMatch {
public:
	ClientMatch(ClientEvents &r_events, const uint8_t &p_own_slot) :
			events_(r_events), own_slot_(p_own_slot) {}

	bool datagram(net::MessageType p_type, const net::NetSim::Datagram &p_d);
	const char *check_rules(const net::MatchInfo &p_match, const SharedLevel &p_level);
	void combat(const net::CombatEvents &p_events, bool p_have_own, uint32_t p_own_life);
	void deployables(const net::Deployables &p_message);
	void restrict(sim::Command &r_command);
	bool unspawned(sim::Tick p_tick, bool p_have_own, sim::Tick p_life_spawn_tick);
	void publish(PresentationFrame &r_frame, bool p_have_own, uint32_t p_own_life) const;
	void take_logic(ClientLogicFeed &r_feed) { logic_.take(r_feed); }
	const std::array<net::SoldierGear, net::kMaxSlots> &gear() const { return gear_; }
	const ClientVehicles &vehicles() const { return vehicles_; }

	uint64_t bad_events() const { return bad_events_; }
	void stats(ClientStats &r_stats) const;

private:
	ClientEvents &events_;
	const uint8_t &own_slot_;
	bool valid_ = false;
	net::MatchState match_;
	PlayerTable players_;
	std::array<net::SoldierGear, net::kMaxSlots> gear_ = kNoGear;
	SquadOrders orders_{};
	std::vector<net::DeployableWire> deployables_;
	std::array<net::SpotRow, net::kMaxSlots> spots_ = kNoSpots;
	ReceivedOrder own_order_;
	bool conquest_valid_ = false;
	net::ConquestState conquest_;
	uint64_t conquest_states_ = 0;
	uint64_t spawn_answers_ = 0;
	net::SpawnAnswer spawn_answer_;
	uint64_t spot_state_messages_ = 0;
	uint64_t deployable_messages_ = 0;
	uint64_t bad_deployables_ = 0;
	bool death_valid_ = false;
	uint32_t death_life_ = 0;
	sim::Tick respawn_tick_ = 0;
	sim::Tick revive_until_ = 0;
	bool unspawned_now_ = false;

	uint64_t match_states_ = 0;
	uint64_t player_lists_ = 0;
	uint64_t soldier_gears_ = 0;
	uint64_t revives_ = 0;
	uint64_t bad_tdm_datagrams_ = 0;
	uint64_t bad_events_ = 0;
	uint64_t restricted_commands_ = 0;
	uint64_t unspawned_ticks_ = 0;
	bool rules_mismatch_ = false;
	ClientLogicSync logic_;
	ClientVehicles vehicles_;
};

}
