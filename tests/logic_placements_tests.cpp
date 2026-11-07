#include <doctest/doctest.h>

#include "bf3/core/level/census.h"
#include "bf3/core/level/level.h"
#include "bf3/core/level/world.h"
#include "bf3/host/ui_flow_session.h"
#include "bf3/sim/logic/placement_drivers.h"
#include "test_support.h"

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3;
namespace lvl = bf3::core::level;

namespace {

constexpr core::Vec3 kOfficeGarage{ -20.797226f, 3.2299173f, -26.904398f };
constexpr core::Vec3 kOfficeUpperFloors{ -6.8930206f, 13.547946f, -33.583633f };
constexpr core::Vec3 kSubwayStreet{ -196.14f, 61.47f, -568.25f };
constexpr core::Vec3 kSubwayParkArea{ -89.38f, -22.28f, 204.88f };

struct Opened {
	std::unique_ptr<lvl::LoadedLevel> level;
	std::unique_ptr<lvl::World> world;
	std::unique_ptr<host::UiFlowSession> session;
	host::UiGameState state;
};

std::optional<Opened> open(const char *p_level, const char *p_mode) {
	bf3::test::Mounted *mount = bf3::test::shared_mount_or_skip();
	if (mount == nullptr) {
		return std::nullopt;
	}
	Opened o;
	core::Result<std::unique_ptr<lvl::LoadedLevel>> loaded = lvl::load_level(mount->install, p_level, p_mode);
	REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
	o.level = std::move(*loaded);
	core::Result<std::unique_ptr<lvl::World>> world = lvl::World::traverse(*o.level);
	REQUIRE(world.ok());
	o.world = std::move(*world);
	host::UiFlowSessionOptions options;
	options.initialize_screens = false;
	core::Result<std::unique_ptr<host::UiFlowSession>> session = host::UiFlowSession::create(*o.level, options);
	REQUIRE_MESSAGE(session.ok(), (session.ok() ? std::string() : session.error().message));
	o.session = std::move(*session);
	o.state.joined = true;
	o.state.phase = net::MatchPhase::InRound;
	return o;
}

void run(Opened &r_o, std::optional<core::Vec3> p_listener, int p_ticks) {
	if (p_listener) {
		r_o.session->set_listener(*p_listener);
	}
	for (int t = 0; t < p_ticks; ++t) {
		REQUIRE(r_o.session->update(r_o.state).ok());
	}
}

bool in_partition(const Opened &p_o, const lvl::PlacedObject &p_object, std::string_view p_partition) {
	return p_object.reference && p_o.level->db().name_of(p_object.reference->partition().guid()) == p_partition;
}

struct Count {
	size_t placements = 0;
	size_t driven = 0;
	size_t unbound = 0;
	size_t drawn = 0;
};

Count drawn_in(const Opened &p_o, std::string_view p_partition) {
	Count c;
	for (const lvl::PlacedObject &object : p_o.world->objects) {
		if (object.kind != lvl::ObjectKind::Mesh || !in_partition(p_o, object, p_partition)) {
			continue;
		}
		++c.placements;
		const core::Result<std::vector<lvl::VisibilityDriver>> drivers =
				lvl::visibility_drivers(*p_o.world, object, core::gameplay::LogicSide::Client);
		REQUIRE(drivers.ok());
		std::vector<lvl::LogicDriverRef> refs;
		for (const lvl::VisibilityDriver &d : *drivers) {
			refs.push_back(
					lvl::LogicDriverRef{ lvl::scope_path(*p_o.world, object.scope), d.source.guid(), d.port, d.start });
		}
		const sim::logic::LogicRuntime &runtime = p_o.session->runtime();
		const std::vector<uint32_t> nodes = sim::logic::bind_placement_drivers(runtime.graph(), refs);
		bool drawn = true;
		for (size_t k = 0; k < refs.size(); ++k) {
			c.unbound += nodes[k] == core::gameplay::kNoLogicIndex ? 1 : 0;
			drawn = drawn && sim::logic::placement_driver_value(runtime, nodes[k], refs[k]);
		}
		c.driven += refs.empty() ? 0 : 1;
		c.drawn += drawn ? 1 : 0;
	}
	return c;
}

}

TEST_SUITE("logic-placements") {
	TEST_CASE("logic placements: the Office garage draws as the client logic's AreaProximity says") {
		std::optional<Opened> o = open("XP2_Office", "TeamDeathMatchC0");
		if (!o) {
			return;
		}
		constexpr std::string_view kGarage = "xp2/objects/officegarage/garageprefab_nongroupable_autogen";
		run(*o, std::nullopt, 30);
		const Count unheard = drawn_in(*o, kGarage);
		run(*o, kOfficeGarage, 30);
		const Count garage = drawn_in(*o, kGarage);
		run(*o, kOfficeUpperFloors, 30);
		const Count upper = drawn_in(*o, kGarage);
		run(*o, kOfficeGarage, 30);
		const Count back = drawn_in(*o, kGarage);
		std::printf("logic.placements XP2_Office garage: %zu mesh placements, %zu driven by the client logic, %zu "
					"unbound; drawn %zu with "
					"no listener, %zu in the garage, %zu on the upper floors, %zu back in the garage\n",
				garage.placements, garage.driven, garage.unbound, unheard.drawn, garage.drawn, upper.drawn, back.drawn);
		CHECK(garage.placements == 343);
		CHECK(garage.driven == 282);
		CHECK(garage.unbound == 0);
		CHECK(unheard.drawn == garage.placements);
		CHECK(garage.drawn == garage.placements);
		CHECK(upper.drawn == garage.placements - 282);
		CHECK(back.drawn == garage.placements);
		CHECK(o->session->errors().empty());
	}

	TEST_CASE(
			"logic placements: Subway's city and park backdrops draw at their own ends, as their AreaProximity says") {
		std::optional<Opened> o = open("MP_Subway", "TeamDeathMatch0");
		if (!o) {
			return;
		}
		constexpr std::string_view kPark = "levels/mp_subway/objects/prefab/backdrop_park";
		constexpr std::string_view kCity = "levels/mp_subway/objects/prefab/backdrop_city";
		run(*o, kSubwayStreet, 30);
		const Count street_park = drawn_in(*o, kPark), street_city = drawn_in(*o, kCity);
		run(*o, kSubwayParkArea, 30);
		const Count park_park = drawn_in(*o, kPark), park_city = drawn_in(*o, kCity);
		std::printf("logic.placements MP_Subway backdrops: park %zu of %zu (%zu driven, %zu unbound), city %zu of %zu "
					"(%zu driven, %zu unbound) "
					"drawn on the street; park %zu, city %zu drawn in the park\n",
				street_park.drawn, street_park.placements, street_park.driven, street_park.unbound, street_city.drawn,
				street_city.placements, street_city.driven, street_city.unbound, park_park.drawn, park_city.drawn);
		CHECK(street_park.placements == 42);
		CHECK(street_city.placements == 22);
		CHECK(street_park.driven == 42);
		CHECK(street_city.driven == 22);
		CHECK(street_park.unbound + street_city.unbound == 0);
		CHECK(street_park.drawn == 0);
		CHECK(street_city.drawn == 22);
		CHECK(park_park.drawn == 42);
		CHECK(park_city.drawn == 0);
		CHECK(o->session->errors().empty());
	}

	TEST_CASE("logic placements: MP_011's storefront ruins stay hidden as their SyncedBool says") {
		std::optional<Opened> o = open("MP_011", "TeamDeathMatch0");
		if (!o) {
			return;
		}
		constexpr std::string_view kStorefront = "architecture/parisstorefront_01/parisstorefront_01_c90_closedprefab";
		const Count before = drawn_in(*o, kStorefront);
		run(*o, std::nullopt, 120);
		const Count after = drawn_in(*o, kStorefront);
		std::printf("logic.placements MP_011 storefront: %zu mesh placements, %zu driven, %zu unbound, drawn %zu at "
					"start and %zu after 2 s "
					"of client logic\n",
				before.placements, before.driven, before.unbound, before.drawn, after.drawn);
		CHECK(before.placements - before.drawn == 30);
		CHECK(before.unbound == 0);
		CHECK(after.drawn == before.drawn);
		CHECK(o->session->errors().empty());
	}
}
