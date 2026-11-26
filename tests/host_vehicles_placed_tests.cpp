#include <doctest/doctest.h>

#include "bf3/core/level/level.h"
#include "bf3/core/level/vehicle_spawns.h"
#include "bf3/core/thread_pool.h"
#include "bf3/host/client_session.h"
#include "bf3/host/level_load.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/vehicle_world.h"
#include "bf3/sim/vehicles/vehicle_armament.h"
#include "vehicles/client_vehicles.h"
#include "vehicles/server_vehicles.h"

#include "test_support.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3;

namespace {

constexpr std::array<std::string_view, 22> kNotLandVehicles{
	"vehicles/a-10_thunderbolt/a10_thunderbolt",
	"vehicles/ah1z/ah1z",
	"vehicles/ah6/ah6_littlebird",
	"vehicles/f18-f/f18",
	"vehicles/f18-f/f18_spawninair",
	"vehicles/ka-60_kasatka/ka-60_kasatka",
	"vehicles/mi28/mi28",
	"vehicles/su-25tm/su-25tm",
	"vehicles/su-35bm-e/su-35bm-e",
	"vehicles/su-35bm-e/su-35bm-e_spawninair",
	"vehicles/venom/venom",
	"vehicles/xp3/ac130/ac130",
	"vehicles/xp5/c130/c130",
	"vehicles/xpack01/f35/f35b",
	"vehicles/z11w/z-11w",
	"vehicles/rhib/rhib",
	"vehicles/rhib/rhib_xp3",
	"vehicles/centurion_c-ram/centurion_c-ram",
	"vehicles/centurion_c-ram/centurion_c-ram_carrier",
	"vehicles/kornet/kornet",
	"vehicles/pantsir/pantsir-s1",
	"vehicles/tow2/tow2",
};

bool land_vehicle(std::string_view p_blueprint) {
	return std::find(kNotLandVehicles.begin(), kNotLandVehicles.end(), p_blueprint) == kNotLandVehicles.end();
}

struct PairCensus {
	std::string level;
	std::string mode;
	std::string error;
	size_t placed = 0;
	size_t placed_land = 0;
	size_t built = 0;
	std::map<std::string, size_t> placed_by_blueprint;
	std::map<std::string, size_t> built_by_blueprint;
	std::vector<std::string> notes;
};

PairCensus census(
		const std::shared_ptr<core::Install> &p_install, const std::string &p_level, const std::string &p_mode) {
	PairCensus c;
	c.level = p_level;
	c.mode = p_mode;
	core::Result<host::LevelDatabase> db = host::open_level_database(p_install, p_level, p_mode, 1, nullptr);
	if (!db) {
		c.error = db.error().message;
		return c;
	}
	core::Result<std::vector<core::level::VehicleSpawnerDef>> placed = core::level::read_vehicle_spawners(*db->world);
	if (!placed) {
		c.error = placed.error().message;
		return c;
	}
	for (const core::level::VehicleSpawnerDef &s : *placed) {
		++c.placed;
		++c.placed_by_blueprint[s.blueprint];
		c.placed_land += land_vehicle(s.blueprint) ? 1u : 0u;
	}
	core::Result<std::shared_ptr<const host::LevelVehicles>> built =
			host::load_level_vehicles(*db, sim::VehicleTuning());
	if (!built) {
		c.error = built.error().message;
		return c;
	}
	const host::LevelVehicles &lv = **built;
	c.built = lv.spawners.size();
	for (const host::LevelVehicleSpawner &s : lv.spawners) {
		++c.built_by_blueprint[lv.kinds[s.kind].blueprint];
	}
	c.notes = lv.notes;
	return c;
}

size_t missing_land(const PairCensus &p_c) {
	size_t missing = 0;
	for (const auto &[blueprint, placed] : p_c.placed_by_blueprint) {
		if (!land_vehicle(blueprint)) {
			continue;
		}
		const auto built = p_c.built_by_blueprint.find(blueprint);
		const size_t have = built == p_c.built_by_blueprint.end() ? size_t(0) : built->second;
		missing += placed > have ? placed - have : size_t(0);
	}
	return missing;
}

void print(const char *p_row, const PairCensus &p_c) {
	std::printf("%s %s %s: %zu spawners placed, %zu of them land vehicles, %zu built\n", p_row, p_c.level.c_str(),
			p_c.mode.c_str(), p_c.placed, p_c.placed_land, p_c.built);
	for (const auto &[blueprint, placed] : p_c.placed_by_blueprint) {
		const auto built = p_c.built_by_blueprint.find(blueprint);
		std::printf("%s   %-48s placed %2zu built %2zu%s\n", p_row, blueprint.c_str(), placed,
				built == p_c.built_by_blueprint.end() ? size_t(0) : built->second,
				land_vehicle(blueprint) ? "" : " (not a land vehicle)");
	}
}

}

TEST_SUITE("host-vehicles-census") {
	TEST_CASE("vehicles: on MP_007 and MP_001 ConquestLarge0, XP3_Desert ConquestLarge0, XP5_001 ConquestLarge0 and "
			  "MP_007 RushLarge0 the loader builds a spawner for every land vehicle the data places") {
		bf3::test::Mounted m;
		if (!bf3::test::open_or_skip(m)) {
			return;
		}
		const std::vector<std::pair<std::string, std::string>> pairs{ { "MP_007", "ConquestLarge0" },
			{ "MP_001", "ConquestLarge0" }, { "XP3_Desert", "ConquestLarge0" }, { "XP5_001", "ConquestLarge0" },
			{ "MP_007", "RushLarge0" } };
		const std::vector<PairCensus> rows = core::parallel_map<PairCensus>(
				pairs.size(), [&](size_t i) { return census(m.install, pairs[i].first, pairs[i].second); }, 5);
		for (const PairCensus &c : rows) {
			CHECK_MESSAGE(c.error.empty(), (c.level + " " + c.mode + ": " + c.error));
			if (!c.error.empty()) {
				continue;
			}
			print("host.vehicles.census", c);
			for (const std::string &n : c.notes) {
				std::printf("host.vehicles.census   note: %s\n", n.c_str());
			}
			CHECK(c.placed > 0);
			CHECK_MESSAGE(missing_land(c) == 0, (c.level + " " + c.mode));
			CHECK_MESSAGE(c.built == c.placed_land, (c.level + " " + c.mode));
			CHECK(c.built <= sim::kMaxVehicleSlots);
		}
		const core::SnapshotDiff diff = core::diff_snapshots(m.before, m.reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s (%zu files)\n", diff.clean() ? "clean" : diff.summary().c_str(),
				m.before.files.size());
	}
}

TEST_SUITE("host-vehicles-census-all") {
	TEST_CASE("vehicles: over every multiplayer level and mode the loader builds a spawner for every land vehicle "
			  "the data places") {
		bf3::test::Mounted m;
		if (!bf3::test::open_or_skip(m)) {
			return;
		}
		std::vector<std::pair<std::string, std::string>> pairs;
		for (const std::string_view id : core::level::shipped_level_ids()) {
			core::Result<core::level::LevelInfo> info = core::level::level_info(*m.install, id);
			if (!info || !info->multiplayer) {
				continue;
			}
			for (const std::string &mode : info->modes) {
				pairs.emplace_back(std::string(id), mode);
			}
		}
		REQUIRE(!pairs.empty());
		const std::vector<PairCensus> rows = core::parallel_map<PairCensus>(
				pairs.size(), [&](size_t i) { return census(m.install, pairs[i].first, pairs[i].second); }, 8);
		size_t placed = 0, land = 0, built = 0, failed = 0, missing = 0, over = 0;
		std::map<std::string, std::pair<size_t, size_t>> by_blueprint;
		for (const PairCensus &c : rows) {
			if (!c.error.empty()) {
				++failed;
				std::printf("host.vehicles.census.all %s %s: %s\n", c.level.c_str(), c.mode.c_str(), c.error.c_str());
				continue;
			}
			placed += c.placed;
			land += c.placed_land;
			built += c.built;
			missing += missing_land(c);
			over += c.built > sim::kMaxVehicleSlots ? 1u : 0u;
			for (const auto &[blueprint, n] : c.placed_by_blueprint) {
				by_blueprint[blueprint].first += n;
			}
			for (const auto &[blueprint, n] : c.built_by_blueprint) {
				by_blueprint[blueprint].second += n;
			}
			if (missing_land(c) != 0 || c.built != c.placed_land) {
				print("host.vehicles.census.all", c);
			}
		}
		for (const auto &[blueprint, n] : by_blueprint) {
			std::printf("host.vehicles.census.all   %-48s placed %4zu built %4zu%s\n", blueprint.c_str(), n.first,
					n.second, land_vehicle(blueprint) ? "" : " (not a land vehicle)");
		}
		std::printf(
				"host.vehicles.census.all: %zu level/mode pairs (%zu not loaded), %zu spawners placed, %zu of them "
				"land vehicles, %zu built, %zu land placements without a spawner, %zu pairs over %u vehicle slots\n",
				pairs.size(), failed, placed, land, built, missing, over, unsigned(sim::kMaxVehicleSlots));
		CHECK(missing == 0);
		CHECK(built == land);
		CHECK(over == 0);
		const core::SnapshotDiff diff = core::diff_snapshots(m.before, m.reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s (%zu files)\n", diff.clean() ? "clean" : diff.summary().c_str(),
				m.before.files.size());
	}
}

TEST_SUITE("host-vehicles-placed-unit") {
	TEST_CASE("vehicles: a client presents every one of the 64 vehicles a level can hold") {
		host::ClientVehicles seen;
		for (uint16_t i = 0; i < sim::kMaxVehicleSlots; ++i) {
			net::VehicleStates m;
			m.server_tick = 10;
			net::VehicleWire w;
			w.spawner = i;
			w.generation = 1;
			w.seat_count = 1;
			w.state.wheel_count = 4;
			m.vehicles.push_back(w);
			const std::vector<uint8_t> bytes = net::encode_vehicle_states(m);
			REQUIRE(!bytes.empty());
			seen.receive(bytes);
		}
		auto frame = std::make_unique<host::PresentationFrame>();
		frame->render_ticks_at_publish = 10.0;
		seen.publish(*frame);
		host::ClientStats stats;
		seen.stats(stats);
		CHECK(frame->vehicle_count == sim::kMaxVehicleSlots);
		CHECK(stats.bad_vehicle_messages == 0);
	}

	TEST_CASE("vehicles: the server packs vehicle states by their encoded size, so twelve 8-wheel vehicles with every "
			  "seat and turret go out in datagrams that all encode and decode") {
		std::vector<net::VehicleStates> out;
		for (uint16_t i = 0; i < 12; ++i) {
			net::VehicleWire w;
			w.spawner = i;
			w.generation = 1;
			w.seat_count = uint8_t(net::kMaxVehicleSeats);
			w.state.wheel_count = uint8_t(sim::kMaxVehicleWheels);
			w.turret_count = uint8_t(net::kMaxVehicleTurrets);
			w.state.driver.tyres.fill({ 1.0f, -1.0f, 1.0f, 5, 3, 2 });
			host::pack_vehicle_wire(out, 5, std::move(w));
		}
		size_t carried = 0, largest = 0;
		for (const net::VehicleStates &m : out) {
			const std::vector<uint8_t> bytes = net::encode_vehicle_states(m);
			CHECK(!bytes.empty());
			CHECK(bytes.size() <= net::kMaxDatagramBytes);
			CHECK(bytes.size() == (net::vehicle_states_bits(m) + 7u) / 8u);
			CHECK(net::decode_vehicle_states(bytes).ok());
			CHECK(m.vehicles.size() <= net::kMaxVehiclesPerDatagram);
			carried += m.vehicles.size();
			largest = std::max(largest, bytes.size());
		}
		std::printf("host.vehicles.placed.unit: 12 vehicles of 8 wheels, 8 seats and 16 turrets in %zu datagrams, the "
					"largest %zu bytes\n",
				out.size(), largest);
		CHECK(carried == 12);
		CHECK(out.size() > 2);
		net::VehicleStates six;
		six.server_tick = 5;
		for (size_t i = 0; i < 6; ++i) {
			six.vehicles.push_back(out.front().vehicles.front());
		}
		CHECK(net::vehicle_states_bits(six) > net::kMaxDatagramBytes * 8);
	}
}

TEST_SUITE("host-vehicles-settle") {
	TEST_CASE("vehicles: every kind the loader builds on MP_007, MP_001, XP3_Desert and XP5_001 ConquestLarge0 comes "
			  "to rest upright on flat ground within 10 s of its spawn drop") {
		bf3::test::Mounted m;
		if (!bf3::test::open_or_skip(m)) {
			return;
		}
		core::Result<std::shared_ptr<const sim::StaticWorldShapes>> ground =
				sim::build_synthetic_vehicle_ground(sim::SyntheticVehicleGround{ 512.0f, 0.0f, false });
		REQUIRE_MESSAGE(ground.ok(), (ground.ok() ? std::string() : ground.error().message));
		std::vector<std::string> done;
		size_t settled = 0, kinds = 0;
		for (const char *level : { "MP_007", "MP_001", "XP3_Desert", "XP5_001" }) {
			core::Result<host::LevelDatabase> db =
					host::open_level_database(m.install, level, "ConquestLarge0", 1, nullptr);
			REQUIRE_MESSAGE(db.ok(), (db.ok() ? std::string() : db.error().message));
			core::Result<std::shared_ptr<const host::LevelVehicles>> built =
					host::load_level_vehicles(*db, sim::VehicleTuning());
			REQUIRE_MESSAGE(built.ok(), (built.ok() ? std::string() : built.error().message));
			for (const host::LevelVehicleKind &kind : (*built)->kinds) {
				if (std::find(done.begin(), done.end(), kind.blueprint) != done.end()) {
					continue;
				}
				done.push_back(kind.blueprint);
				++kinds;
				sim::VehicleWorldOptions options;
				options.max_vehicles = 1;
				options.tuning = (*built)->tuning;
				core::Result<std::unique_ptr<sim::VehicleWorld>> world = sim::VehicleWorld::create(*ground, options);
				REQUIRE_MESSAGE(world.ok(), (world.ok() ? std::string() : world.error().message));
				sim::VehiclePlacement at;
				at.position = { 0.0f, host::kVehicleSpawnLift, 0.0f };
				const core::Status added = (*world)->add_vehicle(0, kind.model, at);
				REQUIRE_MESSAGE(added.ok(), (added.ok() ? std::string() : added.error().message));
				const sim::VehicleSlot slot = 0;
				const sim::VehicleCommand idle;
				float lowest_up = 1.0f;
				for (uint32_t t = 0; t < 600; ++t) {
					const core::Status stepped = (*world)->step(std::span<const sim::VehicleSlot>(&slot, 1),
							std::span<const sim::VehicleCommand>(&idle, 1));
					REQUIRE_MESSAGE(stepped.ok(), (stepped.ok() ? std::string() : stepped.error().message));
					lowest_up = std::min(lowest_up, sim::vehicles::hull_up_y((*world)->state(0).body.rotation));
				}
				const sim::VehicleState s = (*world)->state(0);
				const float speed = std::sqrt(s.body.linear_velocity[0] * s.body.linear_velocity[0] +
						s.body.linear_velocity[1] * s.body.linear_velocity[1] +
						s.body.linear_velocity[2] * s.body.linear_velocity[2]);
				const float up = sim::vehicles::hull_up_y(s.body.rotation);
				std::printf("host.vehicles.settle %-48s %u wheels, up y %.3f (lowest %.3f), %.2f m/s, y %.2f m\n",
						kind.blueprint.c_str(), unsigned(kind.model->wheel_count), double(up), double(lowest_up),
						double(speed), double(s.body.com_position[1]));
				const bool rests = up > 0.95f && speed < 0.2f;
				settled += rests ? 1u : 0u;
				CHECK_MESSAGE(rests, kind.blueprint);
			}
		}
		std::printf("host.vehicles.settle: %zu of %zu kinds rest upright\n", settled, kinds);
		CHECK(kinds >= 20);
		const core::SnapshotDiff diff = core::diff_snapshots(m.before, m.reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s (%zu files)\n", diff.clean() ? "clean" : diff.summary().c_str(),
				m.before.files.size());
	}
}
