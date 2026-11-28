#include <doctest/doctest.h>

#include "bf3/host/ai_bot_source.h"
#include "bf3/host/game_server.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/host/server_view.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/vehicles/vehicle_armament.h"
#include "bf3/sim/vehicles/vehicle_input.h"

#include "ai_shipped_config.h"
#include "tdm_bot_shared.h"
#include "tdm_fixture.h"
#include "vehicle_weapon_hits.h"
#include "vehicles/server_vehicles.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <string>

using namespace bf3;
namespace tt = bf3::test::tdm;

namespace {

constexpr float kTargetMetres = 60.0f;
constexpr uint32_t kRunTicks = 90u * sim::kSimHz;
constexpr float kEyeMetres = 1.6f;
constexpr float kChestMetres = 1.3f;
constexpr float kSearchRadians = 0.2f;

class SeatedBot final : public host::CommandSource {
public:
	SeatedBot(std::unique_ptr<host::AiBotSource> p_ai, const host::GameServer &p_server, size_t p_vehicle,
			uint8_t p_seat) : ai_(std::move(p_ai)), server_(p_server), vehicle_(p_vehicle), seat_(p_seat) {}

	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) override {
		const sim::Command c = ai_->command(p_tick, p_state);
		const host::ServerVehicles *v = server_.view().server_vehicles();
		const std::optional<host::VehicleSeat> at = v != nullptr ? v->seat_of(slot_) : std::nullopt;
		if (at && at->vehicle == vehicle_ && at->seat == seat_) {
			++seated_ticks;
			return c;
		}
		sim::Command force = sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
		if (!at) {
			use_down_ = !use_down_;
			force.buttons |= use_down_ ? sim::buttons::kUse : 0u;
		} else {
			force.buttons = sim::vehicles::with_seat_request(force.buttons, seat_);
		}
		return force;
	}
	void admitted(uint8_t p_slot) override {
		slot_ = p_slot;
		ai_->admitted(p_slot);
	}
	uint8_t slot() const { return slot_; }
	uint64_t seated_ticks = 0;

private:
	std::unique_ptr<host::AiBotSource> ai_;
	const host::GameServer &server_;
	size_t vehicle_;
	uint8_t seat_;
	uint8_t slot_ = 0xFF;
	bool use_down_ = false;
};

class StandingSoldier final : public host::CommandSource {
public:
	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) override {
		return sim::idle_command(p_tick, p_state.yaw, p_state.pitch, p_state.target_pose);
	}
};

struct GunneryRun {
	std::string error;
	std::string waiting;
	uint64_t seated_ticks = 0;
	uint64_t shots = 0;
	uint64_t hits = 0;
	uint64_t fire_ticks = 0;
	float target_metres = 0.0f;
};

std::optional<sim::Vec3> ground_at(const sim::CollisionWorld &p_world, const sim::Vec3 &p_at) {
	sim::StaticHit hit;
	const sim::Vec3 from{ p_at[0], p_at[1] + 30.0f, p_at[2] };
	if (!p_world.ray(from, { 0.0f, -80.0f, 0.0f }, sim::query_mask::kMovement, &hit)) {
		return std::nullopt;
	}
	return sim::Vec3{ hit.position[0], hit.position[1] + 0.05f, hit.position[2] };
}

GunneryRun run_gunnery(const tt::TdmLevel &p_level, const std::function<bool(const host::LevelVehicleKind &)> &p_kind,
		uint8_t p_seat, uint64_t p_seed) {
	GunneryRun out;
	const host::LevelVehicles &lv = *p_level.shared->vehicles;
	std::optional<size_t> vehicle;
	auto stats = std::make_shared<host::TdmBotStats>();
	SeatedBot *gunner = nullptr;
	int target_slot = -1;
	bool placed = false;
	bool boarding = false;
	uint64_t shots0 = 0, hits0 = 0;
	auto seat_hits = std::make_shared<bf3::test::VehicleWeaponHits>();
	tt::TdmRunConfig c;
	c.seed = p_seed;
	c.bots = 0;
	c.peer = false;
	c.rules.mode = host::MatchRulesConfig::Mode::Tdm;
	c.rules.deploy_screen = false;
	c.rules.preround_override_s = 0.0f;
	c.rules.max_players = 32;
	c.rules.min_players_override = 2;
	c.max_ticks = sim::seconds_to_ticks(tt::latest_initial_delay(lv)) + kRunTicks;
	c.after_round_ticks = 0;
	c.hashes = false;
	c.configure_server = [&](host::GameServerConfig &r_cfg) {
		auto vehicles = std::make_shared<host::LevelVehicles>(lv);
		for (host::LevelVehicleSpawner &s : vehicles->spawners) {
			s.initial_spawn_delay = 0.0f;
		}
		auto shared = std::make_shared<host::SharedLevel>(*r_cfg.level);
		shared->vehicles = vehicles;
		r_cfg.level = shared;
		r_cfg.gunplay.observer = seat_hits;
	};
	c.until = [&](sim::Tick) { return out.shots > 0 && out.hits > 0; };
	c.on_tick = [&](sim::Tick p_tick, host::GameServer &r_server, tt::ScriptedPeer *) {
		const host::ServerView &view = r_server.view();
		const host::ServerVehicles *live = view.server_vehicles();
		for (size_t i = 0; !vehicle && live != nullptr && i < lv.spawners.size(); ++i) {
			if (p_kind(lv.kinds[lv.spawners[i].kind]) && live->vehicles()[i].live && live->vehicles()[i].team != 0) {
				vehicle = i;
			}
		}
		if (!vehicle) {
			out.waiting = "no live team vehicle of the kind";
			return;
		}
		const host::LevelVehicleKind &kind = lv.kinds[lv.spawners[*vehicle].kind];
		const int team = int(live->vehicles()[*vehicle].team);
		if (gunner == nullptr) {
			auto cfg = std::make_shared<host::TdmBotConfig>();
			bf3::test::use_shipped_bot_ai(*cfg);
			cfg->stats = stats;
			cfg->squads = false;
			host::TdmBrainHost h;
			h.view = &view;
			h.gunplay = r_server.gunplay();
			h.vehicles = p_level.shared->vehicles;
			h.nav = p_level.bots;
			h.soldier_top_speed = sim::sprint_speed(p_level.shared->motor);
			h.match_seed = p_seed;
			h.rules = true;
			core::Result<std::shared_ptr<host::TdmShared>> shared = host::make_tdm_shared(h, cfg);
			if (!shared) {
				out.error = shared.error().message;
				return;
			}
			auto seated = std::make_unique<SeatedBot>(
					std::make_unique<host::AiBotSource>(*shared, 0), r_server, *vehicle, p_seat);
			gunner = seated.get();
			if (r_server.add_local_bot(std::move(seated), team) < 0 ||
					(target_slot = r_server.add_local_bot(std::make_unique<StandingSoldier>(), 3 - team)) < 0) {
				out.error = "no slot for the two soldiers";
			}
			return;
		}
		if (!live->vehicles()[*vehicle].live || !view.alive(gunner->slot()) || !view.alive(uint8_t(target_slot))) {
			out.waiting = "the vehicle or a soldier is not alive";
			return;
		}
		out.waiting.clear();
		const core::Transform hull = live->hull(*vehicle);
		const sim::Vec3 com = live->world()->state(sim::VehicleSlot(*vehicle)).body.com_position;
		const uint16_t yaw = sim::quantize_yaw(std::atan2(hull.forward[0], hull.forward[2]));
		if (!boarding) {
			const std::optional<sim::Vec3> beside = ground_at(
					view.world(), core::vec::add(com, core::vec::scale(hull.right, 0.6f * kind.entry_radius)));
			if (!beside) {
				out.error = "no ground beside the vehicle";
				return;
			}
			CHECK(r_server.place_soldier(gunner->slot(), *beside, yaw, sim::Pose::Stand).ok());
			boarding = true;
			return;
		}
		const std::optional<sim::Vec3> seat = live->seat_position(gunner->slot());
		if (!placed && gunner->seated_ticks > 0 && seat) {
			const sim::Vec3 eye = core::vec::add(*seat, { 0.0f, kEyeMetres, 0.0f });
			for (int k = 0; k < 16 && !placed; ++k) {
				const float a = float((k + 1) / 2) * (k % 2 == 0 ? 1.0f : -1.0f) * kSearchRadians;
				const sim::Vec3 dir = core::vec::add(
						core::vec::scale(hull.forward, std::cos(a)), core::vec::scale(hull.right, std::sin(a)));
				const std::optional<sim::Vec3> feet =
						ground_at(view.world(), core::vec::add(com, core::vec::scale(dir, kTargetMetres)));
				if (!feet) {
					continue;
				}
				const sim::Vec3 chest = core::vec::add(*feet, { 0.0f, kChestMetres, 0.0f });
				if (!view.world().ray(eye, core::vec::sub(chest, eye), sim::query_mask::kProjectile)) {
					CHECK(r_server.place_soldier(uint32_t(target_slot), *feet, yaw, sim::Pose::Stand).ok());
					out.target_metres = tt::distance(com, *feet);
					placed = true;
				}
			}
			if (!placed) {
				out.error = "no clear line from the seat to a point 60 m out";
			}
			return;
		}
		const host::VehicleGunneryStats &g = live->gunnery().stats();
		if (gunner->seated_ticks == 1) {
			shots0 = g.shots;
			hits0 = seat_hits->hits();
		}
		if (gunner->seated_ticks > 0) {
			out.shots = g.shots - shots0;
			out.hits = seat_hits->hits() - hits0;
		}

		out.seated_ticks = gunner->seated_ticks;
		(void)p_tick;
	};
	core::Result<tt::TdmRunResult> r = tt::run_tdm(p_level, c);
	if (!r && out.error.empty()) {
		out.error = r.error().message;
	}
	out.fire_ticks = stats->vehicle_fire_ticks.load();
	return out;
}

void check_gunnery(const char *p_name, const GunneryRun &p_run) {
	std::printf("host.bots.gunnery: %s: target %.1f m ahead; %llu ticks seated, %llu ticks holding fire, %llu "
				"vehicle shots, %llu soldier hits\n",
			p_name, double(p_run.target_metres), (unsigned long long)p_run.seated_ticks,
			(unsigned long long)p_run.fire_ticks, (unsigned long long)p_run.shots, (unsigned long long)p_run.hits);
	CHECK_MESSAGE(p_run.error.empty(), p_run.error);
	CHECK_MESSAGE(p_run.waiting.empty(), p_run.waiting);
	CHECK(p_run.seated_ticks > 0u);
	CHECK(p_run.fire_ticks > 0u);
	CHECK(p_run.shots > 0u);
	CHECK(p_run.hits > 0u);
}

uint16_t seat_weapon_def(const host::LevelVehicleKind &p_kind, uint8_t p_seat) {
	for (size_t w = 0; p_kind.armament != nullptr && w < p_kind.armament->weapons.size(); ++w) {
		if (p_kind.armament->weapons[w].seat == int8_t(p_seat) &&
				sim::vehicles::weapon_in_stance(p_kind.armament->weapons[w], 0) && w < p_kind.weapon_defs.size()) {
			return p_kind.weapon_defs[w];
		}
	}
	return host::kNoVehicleWeapon;
}

bool m1a2(const host::LevelVehicleKind &p_kind) {
	return p_kind.blueprint.find("m1a2") != std::string::npos;
}

bool growler(const host::LevelVehicleKind &p_kind) {
	return p_kind.blueprint.find("growler") != std::string::npos;
}

}

TEST_SUITE("host-bots-gunnery") {
	TEST_CASE("bots fire seat weapons: on MP_007 ConquestSmall0 with the shipped AI a bot in the M1A2's gunner seat "
			  "fires its remote HMG at an enemy soldier 60 m ahead and hits him") {
		tt::TdmLevel *level = tt::load_conquest_bots_level_once();
		if (level == nullptr) {
			return;
		}
		check_gunnery("M1A2 gunner seat", run_gunnery(*level, m1a2, 1, 11));
	}

	TEST_CASE("bots fire seat weapons: a bot in the Growler's gun seat fires its mounted M2HB at an enemy soldier 60 m "
			  "ahead and hits him") {
		tt::TdmLevel *level = tt::load_conquest_bots_level_once();
		if (level == nullptr) {
			return;
		}
		check_gunnery("Growler gun seat", run_gunnery(*level, growler, 1, 12));
	}

	TEST_CASE("bots fire seat weapons: a bot driving the M1A2 fires its main gun, the seat-0 cannon, at an enemy "
			  "soldier 60 m ahead and hits him") {
		tt::TdmLevel *level = tt::load_conquest_bots_level_once();
		if (level == nullptr) {
			return;
		}
		const GunneryRun run = run_gunnery(*level, m1a2, 0, 13);
		check_gunnery("M1A2 driver seat", run);
		const host::LevelVehicles &lv = *level->shared->vehicles;
		for (const host::LevelVehicleKind &k : lv.kinds) {
			if (!m1a2(k)) {
				continue;
			}
			uint32_t seat_weapons = 0;
			for (size_t w = 0; w < k.armament->weapons.size() && w < k.weapon_defs.size(); ++w) {
				seat_weapons +=
						k.armament->weapons[w].seat == 0 && sim::vehicles::weapon_in_stance(k.armament->weapons[w], 0)
						? 1u
						: 0u;
			}
			const uint16_t def = seat_weapon_def(k, 0);
			REQUIRE(def < lv.weapons.size());
			std::printf("host.bots.gunnery: the M1A2 driver seat's one stance-0 weapon is %s\n",
					lv.weapons[def].variant.c_str());
			CHECK(seat_weapons == 1u);
			CHECK(lv.weapons[def].variant.find("cannon") != std::string::npos);
		}
	}
}
