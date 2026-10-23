#include "tdm_bot_shared.h"

#include "bf3/host/gunplay.h"
#include "bf3/host/server_view.h"
#include "bf3/sim/weapons/weapon_sim_def.h"

#include <algorithm>
#include <cmath>
#include <span>

namespace bf3::host {

using namespace tdm_bot;

namespace {

namespace sw = sim::weapons;

constexpr uint32_t kRingTicks = 30;
constexpr size_t kMaxRing = 1024;
constexpr uint32_t kFiringTicks = 30;
constexpr float kHeatShot = 0.05f, kHeatDeath = 1.0f;
constexpr size_t kMaxBrainSlots = 256;

}

int TdmShared::heat_cell(const sim::Vec3 &p_at) const {
	const float cell = std::max(config->heat_cell_m, 1.0f);
	const float fx = (p_at[0] - host.heat_origin[0]) / cell, fz = (p_at[2] - host.heat_origin[2]) / cell;
	if (!std::isfinite(fx) || !std::isfinite(fz) || fx < 0.0f || fz < 0.0f || fx >= float(kHeatCells) || fz >= float(kHeatCells)) {
		return -1;
	}
	return int(fz) * kHeatCells + int(fx);
}

float TdmShared::heat_at(int p_cell, sim::Tick p_tick) {
	const uint32_t decay_ticks = std::max<uint32_t>(sim::seconds_to_ticks(config->heat_decay_s), 1u);
	const uint32_t age = p_tick >= heat_tick[size_t(p_cell)] ? p_tick - heat_tick[size_t(p_cell)] : 0u;
	if (age > 0) {
		heat[size_t(p_cell)] *= std::exp(-float(age) / float(decay_ticks));
		heat_tick[size_t(p_cell)] = p_tick;
	}
	return heat[size_t(p_cell)];
}

void TdmShared::add_heat(const sim::Vec3 &p_at, float p_amount, sim::Tick p_tick) {
	const int c = heat_cell(p_at);
	if (c >= 0) {
		heat[size_t(c)] = std::min(heat_at(c, p_tick) + p_amount, 1000.0f);
	}
}

sim::Vec3 TdmShared::heat_centre(int p_cell) const {
	const float cell = std::max(config->heat_cell_m, 1.0f);
	return { host.heat_origin[0] + (float(p_cell % kHeatCells) + 0.5f) * cell, 0.0f, host.heat_origin[2] + (float(p_cell / kHeatCells) + 0.5f) * cell };
}

void TdmShared::begin_tick(sim::Tick p_tick) {
	if (time_tick != p_tick) {
		time_tick = p_tick;
		perception_ns = brains_ns = 0;
	}
	if (nav != nullptr) {
		nav->run_once(p_tick);
	}
	if (scan_tick == p_tick) {
		return;
	}
	scan_tick = p_tick;
	const ServerView &view = *host.view;
	const std::span<const sim::SoldierState> states = view.states();
	const size_t n = std::min(states.size(), kMaxBrainSlots);
	teams.assign(n, 0);
	alive.assign(n, 0);
	if (was_alive.size() < n) {
		was_alive.resize(n, 0);
		fired_tick.resize(n, 0);
		shot_cursor.resize(n, 0);
	}
	firing.assign(n, 0);
	for (size_t s = 0; s < n; ++s) {
		teams[s] = view.team(uint8_t(s));
		alive[s] = view.alive(uint8_t(s)) ? 1 : 0;
		if (was_alive[s] != 0 && alive[s] == 0 && finite(states[s].position)) {
			add_heat(states[s].position, kHeatDeath, p_tick);
		}
		was_alive[s] = alive[s];
	}

	if (host.gunplay != nullptr) {
		for (const sw::ShotEvent &shot : host.gunplay->interval_shots()) {
			const size_t s = shot.player_slot;
			if (s >= n || shot.shot_index < shot_cursor[s] || !finite(shot.origin) || !finite(shot.direction)) {
				continue;
			}
			shot_cursor[s] = shot.shot_index + 1u;
			if (ring.size() >= kMaxRing) {
				ring.erase(ring.begin());
			}
			ring.push_back({ uint8_t(s), p_tick, shot.origin, shot.direction });
			fired_tick[s] = p_tick;
			add_heat(shot.origin, kHeatShot, p_tick);
		}
	}
	ring.erase(std::remove_if(ring.begin(), ring.end(), [&](const RecentShot &p_s) { return p_s.tick + kRingTicks < p_tick; }), ring.end());
	heard.clear();
	for (const RecentShot &r : ring) {
		heard.push_back({ r.shooter, r.origin, r.tick });
	}
	for (size_t s = 0; s < n; ++s) {
		firing[s] = fired_tick[s] != 0 && p_tick - fired_tick[s] <= kFiringTicks ? 1 : 0;
	}
}

core::Result<std::shared_ptr<TdmShared>> make_tdm_shared(const TdmBrainHost &p_host, std::shared_ptr<const TdmBotConfig> p_config) {
	if (p_host.view == nullptr) {
		return core::make_error("tdm bots: no server view");
	}
	auto sh = std::make_shared<TdmShared>();
	sh->host = p_host;
	sh->config = p_config != nullptr ? std::move(p_config) : std::make_shared<const TdmBotConfig>();
	sh->stats = sh->config->stats.get();
	sh->observer = sh->config->observer.get();
	sh->walker = p_host.walker;
	sh->walker.objective_share = std::clamp(sh->config->hunt_share, 0.0f, 1.0f);
	if (p_host.nav != nullptr && p_host.nav->nav != nullptr) {
		core::Result<std::unique_ptr<BotNav>> nav = make_bot_nav(p_host.nav);
		if (!nav) {
			return core::error_at("tdm bots", nav.error());
		}
		sh->nav = std::move(*nav);
	} else if (sh->stats != nullptr) {
		sh->stats->walk_unavailable.store(1, std::memory_order_relaxed);
	}
	return sh;
}

}
