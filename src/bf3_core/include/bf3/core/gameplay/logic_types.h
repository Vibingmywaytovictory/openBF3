#pragma once
// S4 level logic (docs/design/s4.md §4, §6): the closed vocabulary of logic node types, the realm and event-routing
// rules, and the port ids the interpreter binds by. Godot-free, data-free.
//
// Every EBX entity type that takes part in a connection of an instantiated level x mode is one row of the type
// table (logic_types.cpp). A row says how the server treats the type:
//   Primitive   a pure logic node the interpreter runs (Delay, EventGate, SyncedBool, ...);
//   Native      a gameplay entity that stays C++ (spawners, capture points, counters, round flow, triggers,
//               destructible objects); the interpreter talks to it through a port adapter (bf3/sim/logic/
//               logic_native.h);
//   Passive     presentation or tooling (sound, lights, cameras, effects, UI, telemetry): exists in the graph, swallows
//               every input on the server and never emits;
//   Data        a link target that carries data only (volumes, OBBs, spheres, path points, UI combat areas);
//   Structural  references and interface descriptors, which instantiation flattens away (a reference that is left
//               as an object because its blueprint is excluded or missing is Structural too).
// A type that has no row is Unknown: it is logged, counted and treated as Passive (plan D12, gap logic §7). Objects
// that take part in no connection (RoadData, LightProbeVolumeData, ... 16 types over the 230 rows) have no row on
// purpose: the graph counts unknown types over connected nodes only (logic_graph.h, kLogicNodeConnected).
// `s4` marks the rows the S4 slice implements: every Primitive and Native type that takes part in a server-side
// connection on any mode of MP_Subway or MP_007 (docs/design/s4.md §6.1, measured 2026-10-15). Rows with s4 false are
// known types the census reports as "unimplemented" until a later milestone implements them.
//
// Port ids are ebx_name_hash of the port name as i32 (djb2-xor: h = 5381; h = h * 33 ^ byte), the same id the
// connection records store (SourceEvent.Id, TargetFieldId, ...). Designer-named prefab and interface ports have no
// recoverable name; the interpreter never needs one.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace bf3::core::gameplay {

// ebx_name_hash (ebx.h) as a constant expression, signed like the connection records store it.
constexpr int32_t logic_port_id(std::string_view p_name) {
	uint32_t h = 5381;
	for (const char c : p_name) {
		h = (h * 33u) ^ static_cast<uint8_t>(c);
	}
	return static_cast<int32_t>(h);
}

// Port ids the runtime and the port adapters name (verified on retail data, docs/design/s4.md §5.3).
namespace logic_port {
inline constexpr int32_t kIn = logic_port_id("In");
inline constexpr int32_t kOut = logic_port_id("Out"); // 193453899
inline constexpr int32_t kIn1 = logic_port_id("In1");
inline constexpr int32_t kIn2 = logic_port_id("In2");
inline constexpr int32_t kIn3 = logic_port_id("In3");
inline constexpr int32_t kIn4 = logic_port_id("In4");
inline constexpr int32_t kValue = logic_port_id("Value");
inline constexpr int32_t kSetTrue = logic_port_id("SetTrue");
inline constexpr int32_t kSetFalse = logic_port_id("SetFalse");
inline constexpr int32_t kOnTrue = logic_port_id("OnTrue");
inline constexpr int32_t kOnFalse = logic_port_id("OnFalse");
inline constexpr int32_t kOpen = logic_port_id("Open");
inline constexpr int32_t kClose = logic_port_id("Close");
inline constexpr int32_t kEnable = logic_port_id("Enable");
inline constexpr int32_t kDisable = logic_port_id("Disable");
inline constexpr int32_t kEnabled = logic_port_id("Enabled"); // 2662400
inline constexpr int32_t kTrigger = logic_port_id("Trigger");
inline constexpr int32_t kOnTrigger = logic_port_id("OnTrigger"); // 1975570652
inline constexpr int32_t kReset = logic_port_id("Reset");
inline constexpr int32_t kStart = logic_port_id("Start");
inline constexpr int32_t kStop = logic_port_id("Stop");
inline constexpr int32_t kOnFinished = logic_port_id("OnFinished");
inline constexpr int32_t kOnStopped = logic_port_id("OnStopped");
inline constexpr int32_t kDestroy = logic_port_id("Destroy");
inline constexpr int32_t kOnDestroyed = logic_port_id("OnDestroyed"); // -1452333337
inline constexpr int32_t kSpawn = logic_port_id("Spawn");
inline constexpr int32_t kOnSpawned = logic_port_id("OnSpawned");
inline constexpr int32_t kOnCaptured = logic_port_id("OnCaptured");
inline constexpr int32_t kOnLost = logic_port_id("OnLost");
inline constexpr int32_t kOnPlayerEnter = logic_port_id("OnPlayerEnter"); // 1577401599
inline constexpr int32_t kOnRoundOver = logic_port_id("OnRoundOver");
inline constexpr int32_t kRoundStarted = logic_port_id("RoundStarted");
inline constexpr int32_t kOnTriggeredByMatchingTeam = logic_port_id("OnTriggeredByMatchingTeam");
inline constexpr int32_t kOnTriggerOnlyForSpecifiedTeam = logic_port_id("OnTriggerOnlyForSpecifiedTeam");
inline constexpr int32_t kDelay = logic_port_id("Delay");
// EventSplitter's input and output (unnamed in the data; the only ports the type uses on retail).
inline constexpr int32_t kSplitterIn = 1723395486;
inline constexpr int32_t kSplitterOut = -137490305;
} // namespace logic_port

enum class LogicTypeClass : uint8_t { Unknown = 0, Primitive, Native, Passive, Data, Structural };

// Every row of the type table. The numeric values are not stable across versions (kLogicTypesVersion); nothing
// stores them outside a process except through LogicGraph::digest, which hashes the type names.
enum class LogicTypeId : uint16_t {
	Unknown = 0,
	// Primitive, S4 (34): the server-side primitive types of every MP_Subway and MP_007 mode.
	Delay,
	EventGate,
	Sequence,
	TeamFilter,
	SyncedBool,
	EventSplitter,
	Bool,
	MultipleTrigger,
	PlayerFilter,
	Float,
	DelayTrigger,
	BoolToEvent,
	Compare,
	And,
	Or,
	RandomDelay,
	CompareInt,
	SwitchPropertyString,
	SyncedSequence,
	Not,
	MathOp,
	CompareFloat,
	PropertyCast,
	PrintDebugText,
	CompareBool,
	EventSwitch,
	Int,
	RandomMultiEvent,
	Or4,
	IntHub,
	ConsoleCommandTrigger,
	PropertyGate,
	Setting,
	Locator,
	// Primitive, later: 14 server-capable types found only on other rows, then 6 types that are Realm_Client on every
	// row (the server never instantiates them; listed so the client census knows them).
	BoolHub,
	EventAndGate,
	PlatformSplitter,
	PropertyDebug,
	SyncedTransform,
	ToWorldSpaceTransform,
	Transform,
	TransformHub,
	TransformMultiplier,
	TransformSelector,
	TransformSplitter,
	Vec3,
	Vec3MathOp,
	VecSplitter,
	RandomEvent,
	FloatHub,
	EventMemory,
	EulerTransform,
	EulerTransformSplitter,
	CountDown,
	// Native, S4 (25).
	CharacterSpawn,
	VehicleSpawn,
	AlternateSpawn,
	CapturePoint,
	GameInteraction,
	CombatAreaTrigger,
	SurroundingGeometry,
	TicketCounter,
	LifeCounter,
	KillCounter,
	RoundOver,
	PreRound,
	AutoTeam,
	HumanPlayer,
	Objective,
	DamageAreaTrigger,
	ClearAreaTrigger,
	GeometryTrigger,
	UnderFireTrigger,
	FriendZone,
	StaticModel,
	BreakableModel,
	Predestruction,
	LevelControl,
	InputRestriction,
	// Native, later.
	WeaponUnlockPickup,
	VehicleSpawnState,
	GunMasterKillCounter,
	FlagCounter,
	TrackPlayer,
	CharacterStateTrigger,
	DeathAreaTrigger,
	DynamicModel,
	Explosion,
	VehicleEntity,
	AmmoCrate,
	// Passive (server no-op).
	Mixer,
	SoundArea,
	IrReverb,
	Sound,
	SoundState,
	VegetationTree,
	PointLight,
	SpotLight,
	LensFlare,
	ObjectVariationSwitch,
	Camera,
	TargetCamera,
	PlayerCamera,
	Effect,
	Decal,
	VoiceOverEvent,
	Fade,
	StatEventTrigger,
	TelemetryTrigger,
	MapMarker,
	VisualEnvironment,
	LogicVisualEnvironment,
	UIMinimapVolume,
	UIGraph,
	UIEndOfRound,
	UIMessageTrigger,
	Message,
	SpectatorReplay,
	ModelAnimation,
	AreaProximity,
	PlayerInputTrigger,
	Terrain,
	MeshProxy,
	UISetValue,
	EmitterExclusionVolume,
	// Data (link targets).
	VolumeVectorShape,
	Obb,
	Sphere,
	TestPoint,
	UICombatArea,
	// Structural (flattened by instantiation; left over only when the referenced blueprint is not instantiated).
	InterfaceDescriptor,
	ReferenceObject,
	SubWorldReference,
	WorldPartReference,
	Count,
};

struct LogicTypeInfo {
	LogicTypeId id = LogicTypeId::Unknown;
	std::string_view ebx_type; // exact EBX type name, e.g. "DelayEntityData"
	LogicTypeClass cls = LogicTypeClass::Unknown;
	bool s4 = false; // implemented by the S4 slice (Primitive and Native rows); always false for the other classes
};

// The table in LogicTypeId order (index = id, row 0 = Unknown). Built at compile time; no allocation.
std::span<const LogicTypeInfo> logic_type_table();
const LogicTypeInfo &logic_type_info(LogicTypeId p_id);
// Exact, case-sensitive match on the EBX type name (binary search over a name-sorted index). An instance whose type
// derives from a listed type but is not itself listed is Unknown: subtypes of gameplay entities carry their own
// behaviour, so the interpreter never guesses by base type. References (ReferenceObjectData and every type derived
// from it) are resolved by the caller with is_a before this lookup.
LogicTypeId logic_type_of(std::string_view p_ebx_type);

// Bumped whenever a row is added, removed or reclassified; part of LogicGraph::digest and of any cache key.
inline constexpr uint32_t kLogicTypesVersion = 1;

// `Realm` of an entity (Realm.yaml enumerators Realm_Client 0, Realm_Server 1, Realm_ClientAndServer 2, Realm_None 3,
// Realm_Pipeline 4). Unset: the type has no Realm field (synced, gameplay and spatial entities: they exist on both
// sides as a server/client pair, gap logic §5).
enum class LogicRealm : uint8_t { Client = 0, Server = 1, ClientAndServer = 2, None = 3, Pipeline = 4, Unset = 5 };
// From the enumerator name ("Realm_Server"); nullopt for any other text.
std::optional<LogicRealm> logic_realm_from_name(std::string_view p_enumerator);
// Whether a node of this realm is instantiated on the server: Server, ClientAndServer and Unset.
constexpr bool realm_on_server(LogicRealm p_realm) {
	return p_realm == LogicRealm::Server || p_realm == LogicRealm::ClientAndServer || p_realm == LogicRealm::Unset;
}
constexpr bool realm_on_client(LogicRealm p_realm) {
	return p_realm == LogicRealm::Client || p_realm == LogicRealm::ClientAndServer || p_realm == LogicRealm::Unset;
}

// EventConnection.TargetType (EventConnectionTargetType_Invalid 0, _ClientAndServer 1, _Client 2, _Server 3,
// _NetworkedClient 4, _NetworkedClientAndServer 5).
enum class LogicEventTarget : uint8_t { Invalid = 0, ClientAndServer = 1, Client = 2, Server = 3, NetworkedClient = 4, NetworkedClientAndServer = 5 };
std::optional<LogicEventTarget> logic_event_target_from_name(std::string_view p_enumerator);
// The server delivers an event edge to its target when the target type is Server, ClientAndServer or
// NetworkedClientAndServer (and the target node exists on the server). [inferred, gap logic §3.4]
constexpr bool delivered_on_server(LogicEventTarget p_target) {
	return p_target == LogicEventTarget::Server || p_target == LogicEventTarget::ClientAndServer || p_target == LogicEventTarget::NetworkedClientAndServer;
}
// Fired on the server and sent to clients (the networked-event message of plan §3.5, not replayed to late joiners).
constexpr bool networked_to_clients(LogicEventTarget p_target) {
	return p_target == LogicEventTarget::NetworkedClient || p_target == LogicEventTarget::NetworkedClientAndServer;
}

} // namespace bf3::core::gameplay
