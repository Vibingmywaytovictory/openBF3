#pragma once

#include "bf3/core/audio/mixer_asset.h"
#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/gameplay/logic_world.h"
#include "bf3/core/level/level_transform.h"
#include "bf3/core/math.h"
#include "bf3/core/result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bf3::core::gameplay {

inline constexpr int32_t kAreaShapeLink = 838548383;
inline constexpr int32_t kSoundAreaProximity = logic_port_id("Proximity");
inline constexpr int32_t kSoundAreaProximityMultiplier = logic_port_id("ProximityMultiplier");
inline constexpr int32_t kAreaProximityOut = logic_port_id("Out");
inline constexpr float kAreaFadeDecibelFloor = -60.0f;
inline constexpr size_t kMaxAreaShapePoints = 4096;

enum class AreaFadeCurve : uint8_t { SineAmplitude, LinearAmplitude, LinearDecibel };

float area_fade(AreaFadeCurve p_curve, float p_distance, float p_width);

struct AreaShape {
	enum class Kind : uint8_t { Prism, Box };
	Kind kind = Kind::Prism;
	std::vector<Vec3> polygon;
	Vec3 up{ 0.0f, 1.0f, 0.0f };
	float height = 0.0f;
	level::LevelTransform box;
	Vec3 half_extents{};

	float distance(const Vec3 &p_point) const;
};

struct AreaDef {
	uint32_t node = kNoLogicIndex;
	uint32_t shape_node = kNoLogicIndex;
	LogicTypeId type = LogicTypeId::SoundArea;
	int32_t out_port = kSoundAreaProximity;
	AreaShape shape;
	float fade_width = 0.0f;
	AreaFadeCurve curve = AreaFadeCurve::SineAmplitude;
	float proximity_multiplier = 1.0f;

	float proximity(const Vec3 &p_listener, float p_multiplier) const;
};

struct MixerEntityDef {
	uint32_t node = kNoLogicIndex;
	uint32_t mixer = 0;
};

struct SoundAreaData {
	std::vector<AreaDef> areas;
	std::vector<audio::MixerAsset> mixers;
	std::vector<MixerEntityDef> mixer_entities;
	size_t unshaped = 0;
	size_t open_paths = 0;
	size_t curved_paths = 0;
	size_t tilted_normals = 0;
	size_t mixers_without_asset = 0;
};

Result<SoundAreaData> read_sound_areas(const LogicWorld &p_world, const LogicGraph &p_graph);

}
