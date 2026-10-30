#pragma once

#include "bf3/core/ebx.h"
#include "bf3/core/level/level_transform.h"
#include "bf3/core/math.h"
#include "bf3/core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bf3::core::level {

inline constexpr size_t kMaxAreaVolumePoints = 4096;
inline constexpr float kAreaFadeDecibelFloor = -60.0f;

enum class AreaFadeCurve : uint8_t { SineAmplitude, LinearAmplitude, LinearDecibel };

float area_fade(AreaFadeCurve p_curve, float p_distance, float p_width);

struct AreaVolume {
	enum class Kind : uint8_t { Prism, Box };
	Kind kind = Kind::Prism;
	std::vector<std::array<float, 2>> outline;
	Vec3 across{ 1.0f, 0.0f, 0.0f };
	Vec3 along{ 0.0f, 0.0f, 1.0f };
	Vec3 up{ 0.0f, 1.0f, 0.0f };
	float floor = 0.0f;
	float height = 0.0f;
	Vec3 center{};
	std::array<Vec3, 3> axes{ Vec3{ 1.0f, 0.0f, 0.0f }, Vec3{ 0.0f, 1.0f, 0.0f }, Vec3{ 0.0f, 0.0f, 1.0f } };
	Vec3 half{};
	bool open = false;
	bool curved = false;
	bool tilted = false;

	static AreaVolume prism(std::span<const Vec3> p_points, const Vec3 &p_up, float p_height);
	static Result<AreaVolume> box(const LevelTransform &p_transform, const Vec3 &p_half_extents);

	float distance(const Vec3 &p_point) const;
	Vec3 centre() const;
};

Result<AreaVolume> read_area_volume(const EbxInstance &p_shape, const LevelTransform &p_transform);

}
