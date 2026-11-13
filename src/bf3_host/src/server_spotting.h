#pragma once

#include "bf3/host/game_server.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/server_view.h"
#include "bf3/host/spotter.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/commo.h"
#include "bf3/sim/spotting.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bf3::host {

struct BotEye {
	uint8_t slot = 0;
	uint8_t team = 0;
	sim::Vec3 eye{};
	sim::Vec3 view{};
};

struct OrderDelivery {
	uint8_t target = 0;
	net::OrderNotice notice;
};

sim::commo::Supplies soldier_supplies(const Gunplay &p_gunplay, uint8_t p_slot);
bool soldier_low_ammo(const Gunplay &p_gunplay, uint8_t p_slot, float p_need_percent);

class ServerSpotting {
public:
	void configure(size_t p_slots, const sim::spotting::SpottingDef &p_def);
	const sim::spotting::SpottingDef &def() const { return def_; }
	void reset(size_t p_slot);

	void spot(uint8_t p_spotter, const net::SpotRequest &p_request, std::span<const uint8_t> p_alive);
	void radar(uint8_t p_target, uint8_t p_spotter, std::span<const uint8_t> p_alive);
	std::optional<OrderDelivery> order(
			uint8_t p_issuer, const net::OrderRequest &p_request, std::span<const uint8_t> p_alive);
	void step(float p_dt, std::span<const uint8_t> p_alive);
	void step_bots(double p_clock, float p_dt, std::span<const BotEye> p_bots, std::span<const SpotSubject> p_subjects,
			const sim::CollisionWorld *p_world, std::span<const uint8_t> p_alive);
	void queue_order(uint8_t p_slot, const net::OrderRequest &p_request);
	std::vector<std::pair<uint8_t, net::OrderRequest>> take_queued_orders();
	void set_low_ammo(uint8_t p_slot, bool p_low);
	void set_low_health(uint8_t p_slot, bool p_low);
	bool low_ammo_due(float p_dt);

	std::optional<net::SpotStates> take_changes(sim::Tick p_tick);
	net::SpotStates full(sim::Tick p_tick) const;

	const sim::spotting::SpotTarget &target(uint8_t p_slot) const { return targets_[p_slot]; }
	bool low_ammo(uint8_t p_slot) const { return low_ammo_[p_slot] != 0; }
	const StoredOrder &stored_order(uint8_t p_slot) const { return orders_[p_slot]; }
	const SpottingStats &stats() const { return stats_; }

	net::SpotRow row(uint8_t p_slot) const;

private:
	sim::spotting::SpottingDef def_;
	std::vector<sim::spotting::SpotTarget> targets_;
	std::vector<uint8_t> low_ammo_;
	std::vector<uint8_t> low_health_;
	std::vector<net::SpotRow> sent_;
	std::vector<StoredOrder> orders_;
	std::vector<std::optional<Spotter>> bots_;
	std::vector<std::pair<uint8_t, net::OrderRequest>> queued_orders_;
	std::vector<net::SpotRow> rows_;
	uint64_t order_serial_ = 0;
	float low_ammo_timer_ = 0;
	SpottingStats stats_;
};

}
