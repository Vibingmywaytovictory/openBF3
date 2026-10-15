// S4 interface contract (docs/design/s4.md §5-§9, §12): the level-logic headers compile together, the port ids are
// ebx_name_hash at compile time, the type table keeps the classification the design counts on (48 server-capable
// primitives, 34 of them in the S4 slice, 25 S4 natives), lookups and enumerator parsing behave, and the plain types
// stay plain. No data, no Godot; nothing here calls a function the S4 implementation has yet to write.
#include <doctest/doctest.h>

#include "bf3/core/ebx.h"
#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/gameplay/logic_types.h"
#include "bf3/sim/logic/logic_harness.h"
#include "bf3/sim/logic/logic_native.h"
#include "bf3/sim/logic/logic_runtime.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <type_traits>

namespace gp = bf3::core::gameplay;
namespace lg = bf3::sim::logic;

// ---- compile-time contract ---------------------------------------------------------------------------------

// Port ids verified on retail data (docs/design/s4.md §5.3, §13).
static_assert(gp::logic_port::kOut == 193453899);
static_assert(gp::logic_port::kOnTrigger == 1975570652);
static_assert(gp::logic_port::kTrigger == -1688613187);
static_assert(gp::logic_port::kOnDestroyed == -1452333337);
static_assert(gp::logic_port::kOnPlayerEnter == 1577401599);
static_assert(gp::logic_port::kEnabled == 2662400);

static_assert(std::is_trivially_copyable_v<gp::LogicValue>);
static_assert(std::is_trivially_copyable_v<gp::LogicParam>);
static_assert(std::is_trivially_copyable_v<gp::LogicNode>);
static_assert(std::is_trivially_copyable_v<gp::LogicEdge>);
static_assert(std::is_trivially_copyable_v<gp::LogicInput>);
static_assert(std::is_trivially_copyable_v<gp::LogicScopeInfo>);
static_assert(std::is_trivially_copyable_v<lg::LogicEffect>);
static_assert(std::is_trivially_copyable_v<lg::LogicTraceRecord>);
static_assert(std::is_trivially_copyable_v<lg::LogicNetworkedEvent>);
static_assert(std::is_trivially_copyable_v<lg::NativePortBinding>);
static_assert(std::is_trivially_copyable_v<lg::NativeTimelineEntry>);
static_assert(std::is_move_constructible_v<gp::LogicGraph>);
static_assert(!std::is_copy_constructible_v<lg::LogicRuntime>);

static_assert(gp::realm_on_server(gp::LogicRealm::Server));
static_assert(gp::realm_on_server(gp::LogicRealm::ClientAndServer));
static_assert(gp::realm_on_server(gp::LogicRealm::Unset));
static_assert(!gp::realm_on_server(gp::LogicRealm::Client));
static_assert(gp::delivered_on_server(gp::LogicEventTarget::NetworkedClientAndServer));
static_assert(!gp::delivered_on_server(gp::LogicEventTarget::NetworkedClient));
static_assert(!gp::delivered_on_server(gp::LogicEventTarget::Client));
static_assert(!gp::delivered_on_server(gp::LogicEventTarget::Invalid));
static_assert(gp::networked_to_clients(gp::LogicEventTarget::NetworkedClient));
static_assert(!gp::networked_to_clients(gp::LogicEventTarget::Server));

static_assert(lg::LogicRuntimeOptions{}.max_deliveries_per_tick == 10000);
static_assert(lg::LogicRuntimeOptions{}.hz == bf3::sim::kSimHz);

TEST_SUITE("s4-contract") {
	TEST_CASE("port ids equal ebx_name_hash") {
		for (const char *name : { "In", "Out", "Value", "SetTrue", "SetFalse", "OnTrue", "OnFalse", "Enabled", "Trigger", "OnTrigger", "OnDestroyed",
					 "OnPlayerEnter", "OnCaptured", "OnLost", "Delay", "" }) {
			CHECK(gp::logic_port_id(name) == static_cast<int32_t>(bf3::core::ebx_name_hash(name)));
		}
	}

	TEST_CASE("type table classification") {
		const auto table = gp::logic_type_table();
		REQUIRE(table.size() == static_cast<size_t>(gp::LogicTypeId::Count));
		size_t by_class[6] = {};
		size_t s4_primitive = 0, s4_native = 0, server_primitive = 0;
		std::set<std::string> names;
		for (size_t i = 0; i < table.size(); ++i) {
			const gp::LogicTypeInfo &row = table[i];
			CHECK(static_cast<size_t>(row.id) == i);
			++by_class[static_cast<size_t>(row.cls)];
			if (row.cls == gp::LogicTypeClass::Primitive) {
				// The 48 server-capable primitives are the rows Delay..VecSplitter; the rest are Realm_Client everywhere.
				if (row.id >= gp::LogicTypeId::Delay && row.id <= gp::LogicTypeId::VecSplitter) {
					++server_primitive;
				}
				s4_primitive += row.s4;
			} else if (row.cls == gp::LogicTypeClass::Native) {
				s4_native += row.s4;
			} else {
				CHECK_FALSE(row.s4);
			}
			if (i > 0) {
				CHECK(names.insert(std::string(row.ebx_type)).second);
				CHECK(gp::logic_type_of(row.ebx_type) == row.id);
				CHECK(&gp::logic_type_info(row.id) == &row);
			}
		}
		CHECK(by_class[static_cast<size_t>(gp::LogicTypeClass::Unknown)] == 1);
		CHECK(by_class[static_cast<size_t>(gp::LogicTypeClass::Primitive)] == 54);
		CHECK(by_class[static_cast<size_t>(gp::LogicTypeClass::Native)] == 36);
		CHECK(by_class[static_cast<size_t>(gp::LogicTypeClass::Passive)] == 35);
		CHECK(by_class[static_cast<size_t>(gp::LogicTypeClass::Data)] == 5);
		CHECK(by_class[static_cast<size_t>(gp::LogicTypeClass::Structural)] == 4);
		CHECK(server_primitive == 48);
		CHECK(s4_primitive == 34);
		CHECK(s4_native == 25);
	}

	TEST_CASE("type lookup is exact") {
		CHECK(gp::logic_type_of("DelayEntityData") == gp::LogicTypeId::Delay);
		CHECK(gp::logic_type_of("CharacterSpawnReferenceObjectData") == gp::LogicTypeId::CharacterSpawn);
		CHECK(gp::logic_type_of("OBBData") == gp::LogicTypeId::Obb);
		CHECK(gp::logic_type_of("UIMessageTriggerData") == gp::LogicTypeId::UIMessageTrigger);
		CHECK(gp::logic_type_of("") == gp::LogicTypeId::Unknown);
		CHECK(gp::logic_type_of("delayentitydata") == gp::LogicTypeId::Unknown);
		CHECK(gp::logic_type_of("DelayEntityDataX") == gp::LogicTypeId::Unknown);
		CHECK(gp::logic_type_of("AAA") == gp::LogicTypeId::Unknown);
		CHECK(gp::logic_type_of("zzz") == gp::LogicTypeId::Unknown);
		CHECK(gp::logic_type_info(gp::LogicTypeId::Count).id == gp::LogicTypeId::Unknown);
		CHECK(gp::logic_type_info(static_cast<gp::LogicTypeId>(0xffff)).cls == gp::LogicTypeClass::Unknown);
	}

	TEST_CASE("realm and target enumerators") {
		CHECK(gp::logic_realm_from_name("Realm_Client") == gp::LogicRealm::Client);
		CHECK(gp::logic_realm_from_name("Realm_Server") == gp::LogicRealm::Server);
		CHECK(gp::logic_realm_from_name("Realm_ClientAndServer") == gp::LogicRealm::ClientAndServer);
		CHECK(gp::logic_realm_from_name("Realm_Pipeline") == gp::LogicRealm::Pipeline);
		CHECK_FALSE(gp::logic_realm_from_name("Server").has_value());
		CHECK(gp::logic_event_target_from_name("EventConnectionTargetType_Server") == gp::LogicEventTarget::Server);
		CHECK(gp::logic_event_target_from_name("EventConnectionTargetType_NetworkedClientAndServer") == gp::LogicEventTarget::NetworkedClientAndServer);
		CHECK(gp::logic_event_target_from_name("EventConnectionTargetType_Invalid") == gp::LogicEventTarget::Invalid);
		CHECK_FALSE(gp::logic_event_target_from_name("").has_value());
	}
}
