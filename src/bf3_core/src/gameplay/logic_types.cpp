#include "bf3/core/gameplay/logic_types.h"

#include <array>
#include <cstddef>

namespace bf3::core::gameplay {

namespace {

using C = LogicTypeClass;
using T = LogicTypeId;

constexpr LogicTypeInfo kRows[] = {
	{ T::Unknown, "", C::Unknown, false },

	{ T::Delay, "DelayEntityData", C::Primitive, true },
	{ T::EventGate, "EventGateEntityData", C::Primitive, true },
	{ T::Sequence, "SequenceEntityData", C::Primitive, true },
	{ T::TeamFilter, "TeamFilterEntityData", C::Primitive, true },
	{ T::SyncedBool, "SyncedBoolEntityData", C::Primitive, true },
	{ T::EventSplitter, "EventSplitterEntityData", C::Primitive, true },
	{ T::Bool, "BoolEntityData", C::Primitive, true },
	{ T::MultipleTrigger, "MultipleTriggerEntityData", C::Primitive, true },
	{ T::PlayerFilter, "PlayerFilterEntityData", C::Primitive, true },
	{ T::Float, "FloatEntityData", C::Primitive, true },
	{ T::DelayTrigger, "DelayTriggerEntityData", C::Primitive, true },
	{ T::BoolToEvent, "BoolToEventEntityData", C::Primitive, true },
	{ T::Compare, "CompareEntityData", C::Primitive, true },
	{ T::And, "AndEntityData", C::Primitive, true },
	{ T::Or, "OrEntityData", C::Primitive, true },
	{ T::RandomDelay, "RandomDelayEntityData", C::Primitive, true },
	{ T::CompareInt, "CompareIntEntityData", C::Primitive, true },
	{ T::SwitchPropertyString, "SwitchPropertyStringEntityData", C::Primitive, true },
	{ T::SyncedSequence, "SyncedSequenceEntityData", C::Primitive, true },
	{ T::Not, "NotEntityData", C::Primitive, true },
	{ T::MathOp, "MathOpEntityData", C::Primitive, true },
	{ T::CompareFloat, "CompareFloatEntityData", C::Primitive, true },
	{ T::PropertyCast, "PropertyCastEntityData", C::Primitive, true },
	{ T::PrintDebugText, "PrintDebugTextEntityData", C::Primitive, true },
	{ T::CompareBool, "CompareBoolEntityData", C::Primitive, true },
	{ T::EventSwitch, "EventSwitchEntityData", C::Primitive, true },
	{ T::Int, "IntEntityData", C::Primitive, true },
	{ T::RandomMultiEvent, "RandomMultiEventEntityData", C::Primitive, true },
	{ T::Or4, "Or4EntityData", C::Primitive, true },
	{ T::IntHub, "IntHubEntityData", C::Primitive, true },
	{ T::ConsoleCommandTrigger, "ConsoleCommandTriggerEntityData", C::Primitive, true },
	{ T::PropertyGate, "PropertyGateEntityData", C::Primitive, true },
	{ T::Setting, "SettingEntityData", C::Primitive, true },
	{ T::Locator, "LocatorEntityData", C::Primitive, true },

	{ T::BoolHub, "BoolHubEntityData", C::Primitive, false },
	{ T::EventAndGate, "EventAndGateEntityData", C::Primitive, false },
	{ T::PlatformSplitter, "PlatformSplitterEntityData", C::Primitive, false },
	{ T::PropertyDebug, "PropertyDebugEntityData", C::Primitive, false },
	{ T::SyncedTransform, "SyncedTransformEntityData", C::Primitive, false },
	{ T::ToWorldSpaceTransform, "ToWorldSpaceTransformEntityData", C::Primitive, false },
	{ T::Transform, "TransformEntityData", C::Primitive, false },
	{ T::TransformHub, "TransformHubEntityData", C::Primitive, false },
	{ T::TransformMultiplier, "TransformMultiplierEntityData", C::Primitive, false },
	{ T::TransformSelector, "TransformSelectorEntityData", C::Primitive, false },
	{ T::TransformSplitter, "TransformSplitterEntityData", C::Primitive, false },
	{ T::Vec3, "Vec3EntityData", C::Primitive, false },
	{ T::Vec3MathOp, "Vec3MathOpEntityData", C::Primitive, false },
	{ T::VecSplitter, "VecSplitterEntityData", C::Primitive, false },
	{ T::RandomEvent, "RandomEventEntityData", C::Primitive, false },
	{ T::FloatHub, "FloatHubEntityData", C::Primitive, false },
	{ T::EventMemory, "EventMemoryEntityData", C::Primitive, false },
	{ T::EulerTransform, "EulerTransformEntityData", C::Primitive, false },
	{ T::EulerTransformSplitter, "EulerTransformSplitterEntityData", C::Primitive, false },
	{ T::CountDown, "CountDownEntityData", C::Primitive, false },

	{ T::CharacterSpawn, "CharacterSpawnReferenceObjectData", C::Native, true },
	{ T::VehicleSpawn, "VehicleSpawnReferenceObjectData", C::Native, true },
	{ T::AlternateSpawn, "AlternateSpawnEntityData", C::Native, true },
	{ T::CapturePoint, "CapturePointEntityData", C::Native, true },
	{ T::GameInteraction, "GameInteractionEntityData", C::Native, true },
	{ T::CombatAreaTrigger, "CombatAreaTriggerEntityData", C::Native, true },
	{ T::SurroundingGeometry, "SurroundingGeometryEntityData", C::Native, true },
	{ T::TicketCounter, "TicketCounterEntityData", C::Native, true },
	{ T::LifeCounter, "LifeCounterEntityData", C::Native, true },
	{ T::KillCounter, "KillCounterEntityData", C::Native, true },
	{ T::RoundOver, "RoundOverEntityData", C::Native, true },
	{ T::PreRound, "PreRoundEntityData", C::Native, true },
	{ T::AutoTeam, "AutoTeamEntityData", C::Native, true },
	{ T::HumanPlayer, "HumanPlayerEntityData", C::Native, true },
	{ T::Objective, "ObjectiveEntityData", C::Native, true },
	{ T::DamageAreaTrigger, "DamageAreaTriggerEntityData", C::Native, true },
	{ T::ClearAreaTrigger, "ClearAreaTriggerEntityData", C::Native, true },
	{ T::GeometryTrigger, "GeometryTriggerEntityData", C::Native, true },
	{ T::UnderFireTrigger, "UnderFireTriggerEntityData", C::Native, true },
	{ T::FriendZone, "FriendZoneEntityData", C::Native, true },
	{ T::StaticModel, "StaticModelEntityData", C::Native, true },
	{ T::BreakableModel, "BreakableModelEntityData", C::Native, true },
	{ T::Predestruction, "PredestructionEntityData", C::Native, true },
	{ T::LevelControl, "LevelControlEntityData", C::Native, true },
	{ T::InputRestriction, "InputRestrictionEntityData", C::Native, true },

	{ T::WeaponUnlockPickup, "WeaponUnlockPickupEntityData", C::Native, false },
	{ T::VehicleSpawnState, "VehicleSpawnStateEntityData", C::Native, false },
	{ T::GunMasterKillCounter, "GunMasterKillCounterEntityData", C::Native, false },
	{ T::FlagCounter, "FlagCounterEntityData", C::Native, false },
	{ T::TrackPlayer, "TrackPlayerEntityData", C::Native, false },
	{ T::CharacterStateTrigger, "CharacterStateTriggerEntityData", C::Native, false },
	{ T::DeathAreaTrigger, "DeathAreaTriggerEntityData", C::Native, false },
	{ T::DynamicModel, "DynamicModelEntityData", C::Native, false },
	{ T::Explosion, "ExplosionEntityData", C::Native, false },
	{ T::VehicleEntity, "VehicleEntityData", C::Native, false },
	{ T::AmmoCrate, "AmmoCrateEntityData", C::Native, false },

	{ T::Mixer, "MixerEntityData", C::Passive, false },
	{ T::SoundArea, "SoundAreaEntityData", C::Passive, false },
	{ T::IrReverb, "IrReverbEntityData", C::Passive, false },
	{ T::Sound, "SoundEntityData", C::Passive, false },
	{ T::SoundState, "SoundStateEntityData", C::Passive, false },
	{ T::VegetationTree, "VegetationTreeEntityData", C::Passive, false },
	{ T::PointLight, "PointLightEntityData", C::Passive, false },
	{ T::SpotLight, "SpotLightEntityData", C::Passive, false },
	{ T::LensFlare, "LensFlareEntityData", C::Passive, false },
	{ T::ObjectVariationSwitch, "ObjectVariationSwitchEntityData", C::Passive, false },
	{ T::Camera, "CameraEntityData", C::Passive, false },
	{ T::TargetCamera, "TargetCameraEntityData", C::Passive, false },
	{ T::PlayerCamera, "PlayerCameraEntityData", C::Passive, false },
	{ T::Effect, "EffectEntityData", C::Passive, false },
	{ T::Decal, "DecalEntityData", C::Passive, false },
	{ T::VoiceOverEvent, "VoiceOverEventEntityData", C::Passive, false },
	{ T::Fade, "FadeEntityData", C::Passive, false },
	{ T::StatEventTrigger, "StatEventTriggerEntityData", C::Passive, false },
	{ T::TelemetryTrigger, "TelemetryTriggerEntityData", C::Passive, false },
	{ T::MapMarker, "MapMarkerEntityData", C::Passive, false },
	{ T::VisualEnvironment, "VisualEnvironmentEntityData", C::Passive, false },
	{ T::LogicVisualEnvironment, "LogicVisualEnvironmentEntityData", C::Passive, false },
	{ T::UIMinimapVolume, "UIMinimapVolumeEntityData", C::Passive, false },
	{ T::UIGraph, "UIGraphEntityData", C::Passive, false },
	{ T::UIEndOfRound, "UIEndOfRoundEntityData", C::Passive, false },
	{ T::UIMessageTrigger, "UIMessageTriggerData", C::Passive, false },
	{ T::Message, "MessageEntityData", C::Passive, false },
	{ T::SpectatorReplay, "SpectatorReplayEntityData", C::Passive, false },
	{ T::ModelAnimation, "ModelAnimationEntityData", C::Passive, false },
	{ T::AreaProximity, "AreaProximityEntityData", C::Passive, false },
	{ T::PlayerInputTrigger, "PlayerInputTriggerEntityData", C::Passive, false },
	{ T::Terrain, "TerrainEntityData", C::Passive, false },
	{ T::MeshProxy, "MeshProxyEntityData", C::Passive, false },
	{ T::UISetValue, "UISetValueData", C::Passive, false },
	{ T::EmitterExclusionVolume, "EmitterExclusionVolumeData", C::Passive, false },

	{ T::VolumeVectorShape, "VolumeVectorShapeData", C::Data, false },
	{ T::Obb, "OBBData", C::Data, false },
	{ T::Sphere, "SphereData", C::Data, false },
	{ T::TestPoint, "TestPointEntityData", C::Data, false },
	{ T::UICombatArea, "UICombatAreaEntityData", C::Data, false },

	{ T::InterfaceDescriptor, "InterfaceDescriptorData", C::Structural, false },
	{ T::ReferenceObject, "ReferenceObjectData", C::Structural, false },
	{ T::SubWorldReference, "SubWorldReferenceObjectData", C::Structural, false },
	{ T::WorldPartReference, "WorldPartReferenceObjectData", C::Structural, false },
};

constexpr size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);
static_assert(kRowCount == static_cast<size_t>(LogicTypeId::Count), "one row per LogicTypeId");

constexpr bool rows_in_id_order() {
	for (size_t i = 0; i < kRowCount; ++i) {
		if (static_cast<size_t>(kRows[i].id) != i) {
			return false;
		}
	}
	return true;
}
static_assert(rows_in_id_order(), "rows must be in LogicTypeId order");

constexpr std::array<uint16_t, kRowCount> make_name_index() {
	std::array<uint16_t, kRowCount> index{};
	for (size_t i = 0; i < kRowCount; ++i) {
		index[i] = static_cast<uint16_t>(i);
	}
	for (size_t i = 1; i < kRowCount; ++i) {
		const uint16_t v = index[i];
		size_t j = i;
		while (j > 0 && kRows[v].ebx_type < kRows[index[j - 1]].ebx_type) {
			index[j] = index[j - 1];
			--j;
		}
		index[j] = v;
	}
	return index;
}
constexpr std::array<uint16_t, kRowCount> kNameIndex = make_name_index();

constexpr bool names_unique() {
	for (size_t i = 1; i < kRowCount; ++i) {
		if (kRows[kNameIndex[i]].ebx_type == kRows[kNameIndex[i - 1]].ebx_type) {
			return false;
		}
	}
	return true;
}
static_assert(names_unique(), "EBX type names must be unique");

struct EnumName {
	std::string_view name;
	uint8_t value;
};

constexpr EnumName kRealmNames[] = {
	{ "Realm_Client", 0 },
	{ "Realm_Server", 1 },
	{ "Realm_ClientAndServer", 2 },
	{ "Realm_None", 3 },
	{ "Realm_Pipeline", 4 },
};

constexpr EnumName kTargetNames[] = {
	{ "EventConnectionTargetType_Invalid", 0 },
	{ "EventConnectionTargetType_ClientAndServer", 1 },
	{ "EventConnectionTargetType_Client", 2 },
	{ "EventConnectionTargetType_Server", 3 },
	{ "EventConnectionTargetType_NetworkedClient", 4 },
	{ "EventConnectionTargetType_NetworkedClientAndServer", 5 },
};

}

std::span<const LogicTypeInfo> logic_type_table() {
	return std::span<const LogicTypeInfo>(kRows, kRowCount);
}

const LogicTypeInfo &logic_type_info(LogicTypeId p_id) {
	const size_t i = static_cast<size_t>(p_id);
	return i < kRowCount ? kRows[i] : kRows[0];
}

LogicTypeId logic_type_of(std::string_view p_ebx_type) {
	if (p_ebx_type.empty()) {
		return LogicTypeId::Unknown;
	}
	size_t lo = 0;
	size_t hi = kRowCount;
	while (lo < hi) {
		const size_t mid = lo + (hi - lo) / 2;
		const LogicTypeInfo &row = kRows[kNameIndex[mid]];
		if (row.ebx_type < p_ebx_type) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	if (lo < kRowCount && kRows[kNameIndex[lo]].ebx_type == p_ebx_type) {
		return kRows[kNameIndex[lo]].id;
	}
	return LogicTypeId::Unknown;
}

std::optional<LogicRealm> logic_realm_from_name(std::string_view p_enumerator) {
	for (const EnumName &e : kRealmNames) {
		if (e.name == p_enumerator) {
			return static_cast<LogicRealm>(e.value);
		}
	}
	return std::nullopt;
}

std::optional<LogicEventTarget> logic_event_target_from_name(std::string_view p_enumerator) {
	for (const EnumName &e : kTargetNames) {
		if (e.name == p_enumerator) {
			return static_cast<LogicEventTarget>(e.value);
		}
	}
	return std::nullopt;
}

}
