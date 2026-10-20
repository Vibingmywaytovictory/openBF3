#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace bf3::core::gameplay {

constexpr int32_t logic_port_id(std::string_view p_name) {
	uint32_t h = 5381;
	for (const char c : p_name) {
		h = (h * 33u) ^ static_cast<uint8_t>(c);
	}
	return static_cast<int32_t>(h);
}

namespace logic_port {
inline constexpr int32_t kIn = logic_port_id("In");
inline constexpr int32_t kOut = logic_port_id("Out");
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
inline constexpr int32_t kEnabled = logic_port_id("Enabled");
inline constexpr int32_t kTrigger = logic_port_id("Trigger");
inline constexpr int32_t kOnTrigger = logic_port_id("OnTrigger");
inline constexpr int32_t kReset = logic_port_id("Reset");
inline constexpr int32_t kStart = logic_port_id("Start");
inline constexpr int32_t kStop = logic_port_id("Stop");
inline constexpr int32_t kOnFinished = logic_port_id("OnFinished");
inline constexpr int32_t kOnStopped = logic_port_id("OnStopped");
inline constexpr int32_t kDestroy = logic_port_id("Destroy");
inline constexpr int32_t kOnDestroyed = logic_port_id("OnDestroyed");
inline constexpr int32_t kSpawn = logic_port_id("Spawn");
inline constexpr int32_t kOnSpawned = logic_port_id("OnSpawned");
inline constexpr int32_t kOnCaptured = logic_port_id("OnCaptured");
inline constexpr int32_t kOnLost = logic_port_id("OnLost");
inline constexpr int32_t kOnPlayerEnter = logic_port_id("OnPlayerEnter");
inline constexpr int32_t kOnRoundOver = logic_port_id("OnRoundOver");
inline constexpr int32_t kRoundStarted = logic_port_id("RoundStarted");
inline constexpr int32_t kOnTriggeredByMatchingTeam = logic_port_id("OnTriggeredByMatchingTeam");
inline constexpr int32_t kOnTriggerOnlyForSpecifiedTeam = logic_port_id("OnTriggerOnlyForSpecifiedTeam");
inline constexpr int32_t kDelay = logic_port_id("Delay");

inline constexpr int32_t kSplitterIn = 1723395486;
inline constexpr int32_t kSplitterOut = -137490305;
}

enum class LogicTypeClass : uint8_t { Unknown = 0, Primitive, Native, Passive, Data, Structural };

enum class LogicTypeId : uint16_t {
	Unknown = 0,

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

	VolumeVectorShape,
	Obb,
	Sphere,
	TestPoint,
	UICombatArea,

	InterfaceDescriptor,
	ReferenceObject,
	SubWorldReference,
	WorldPartReference,
	Count,
};

struct LogicTypeInfo {
	LogicTypeId id = LogicTypeId::Unknown;
	std::string_view ebx_type;
	LogicTypeClass cls = LogicTypeClass::Unknown;
	bool s4 = false;
};

std::span<const LogicTypeInfo> logic_type_table();
const LogicTypeInfo &logic_type_info(LogicTypeId p_id);

LogicTypeId logic_type_of(std::string_view p_ebx_type);

inline constexpr uint32_t kLogicTypesVersion = 1;

enum class LogicRealm : uint8_t { Client = 0, Server = 1, ClientAndServer = 2, None = 3, Pipeline = 4, Unset = 5 };

std::optional<LogicRealm> logic_realm_from_name(std::string_view p_enumerator);

constexpr bool realm_on_server(LogicRealm p_realm) {
	return p_realm == LogicRealm::Server || p_realm == LogicRealm::ClientAndServer || p_realm == LogicRealm::Unset;
}
constexpr bool realm_on_client(LogicRealm p_realm) {
	return p_realm == LogicRealm::Client || p_realm == LogicRealm::ClientAndServer || p_realm == LogicRealm::Unset;
}

enum class LogicEventTarget : uint8_t { Invalid = 0, ClientAndServer = 1, Client = 2, Server = 3, NetworkedClient = 4, NetworkedClientAndServer = 5 };
std::optional<LogicEventTarget> logic_event_target_from_name(std::string_view p_enumerator);

constexpr bool delivered_on_server(LogicEventTarget p_target) {
	return p_target == LogicEventTarget::Server || p_target == LogicEventTarget::ClientAndServer || p_target == LogicEventTarget::NetworkedClientAndServer;
}

constexpr bool networked_to_clients(LogicEventTarget p_target) {
	return p_target == LogicEventTarget::NetworkedClient || p_target == LogicEventTarget::NetworkedClientAndServer;
}

}
