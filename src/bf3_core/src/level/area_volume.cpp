#include "bf3/core/level/area_volume.h"

#include "bf3/core/util/finite.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>

namespace bf3::core::level {

namespace {

constexpr float kHalfPi = 1.57079632679489661923f;
constexpr float kTiltTolerance = 1e-3f;
constexpr float kInfinity = std::numeric_limits<float>::infinity();

bool inside_outline(std::span<const std::array<float, 2>> p_outline, float p_u, float p_v) {
	bool in = false;
	for (size_t i = 0, j = p_outline.size() - 1; i < p_outline.size(); j = i++) {
		const std::array<float, 2> &a = p_outline[i], &b = p_outline[j];
		if ((a[1] > p_v) != (b[1] > p_v) && p_u < (b[0] - a[0]) * (p_v - a[1]) / (b[1] - a[1]) + a[0]) {
			in = !in;
		}
	}
	return in;
}

float segment_distance(const std::array<float, 2> &p_a, const std::array<float, 2> &p_b, float p_u, float p_v) {
	const float du = p_b[0] - p_a[0], dv = p_b[1] - p_a[1];
	const float len2 = du * du + dv * dv;
	const float t = len2 > 0.0f ? std::clamp(((p_u - p_a[0]) * du + (p_v - p_a[1]) * dv) / len2, 0.0f, 1.0f) : 0.0f;
	const float eu = p_a[0] + du * t - p_u, ev = p_a[1] + dv * t - p_v;
	return std::sqrt(eu * eu + ev * ev);
}

Error volume_error(const std::string &p_what) {
	return make_error("area volume: " + p_what);
}

}

float area_fade(AreaFadeCurve p_curve, float p_distance, float p_width) {
	if (std::isnan(p_distance)) {
		return 0.0f;
	}
	if (p_distance <= 0.0f) {
		return 1.0f;
	}
	if (!(p_width > 0.0f) || p_distance >= p_width) {
		return 0.0f;
	}
	const float t = p_distance / p_width;
	switch (p_curve) {
		case AreaFadeCurve::SineAmplitude:
			return std::cos(t * kHalfPi);
		case AreaFadeCurve::LinearAmplitude:
			return 1.0f - t;
		case AreaFadeCurve::LinearDecibel:
			return std::pow(10.0f, kAreaFadeDecibelFloor * t / 20.0f);
	}
	return 0.0f;
}

AreaVolume AreaVolume::prism(std::span<const Vec3> p_points, const Vec3 &p_up, float p_height) {
	AreaVolume out;
	out.kind = Kind::Prism;
	out.up = vec::normalize(p_up);
	const Vec3 helper = std::fabs(out.up.z) < 0.9f ? Vec3{ 0.0f, 0.0f, 1.0f } : Vec3{ 1.0f, 0.0f, 0.0f };
	out.across = vec::normalize(vec::cross(out.up, helper));
	out.along = vec::cross(out.across, out.up);
	out.height = p_height;
	out.floor = kInfinity;
	out.outline.reserve(p_points.size());
	for (const Vec3 &p : p_points) {
		out.outline.push_back({ vec::dot(p, out.across), vec::dot(p, out.along) });
		out.floor = std::min(out.floor, vec::dot(p, out.up));
	}
	if (p_points.empty()) {
		out.floor = 0.0f;
	}
	return out;
}

Result<AreaVolume> AreaVolume::box(const LevelTransform &p_transform, const Vec3 &p_half_extents) {
	if (!finite(p_half_extents)) {
		return volume_error("an OBBData has non-finite HalfExtents");
	}
	AreaVolume out;
	out.kind = Kind::Box;
	out.center = p_transform.trans;
	const Vec3 rows[3] = { p_transform.right, p_transform.up, p_transform.forward };
	for (size_t a = 0; a < 3; ++a) {
		const float len = vec::length(rows[a]);
		if (!(len > 1e-6f) || !std::isfinite(len)) {
			return volume_error("an OBBData transform has a degenerate axis");
		}
		out.axes[a] = vec::scale(rows[a], 1.0f / len);
		out.half[a] = p_half_extents[a] * len * p_transform.scale;
	}
	return out;
}

float AreaVolume::distance(const Vec3 &p_point) const {
	if (!finite(p_point)) {
		return kInfinity;
	}
	if (kind == Kind::Box) {
		const Vec3 d = vec::sub(p_point, center);
		float sum = 0.0f;
		for (size_t a = 0; a < 3; ++a) {
			const float excess = std::max(0.0f, std::fabs(vec::dot(d, axes[a])) - half[a]);
			sum += excess * excess;
		}
		return std::sqrt(sum);
	}
	if (outline.size() < 3) {
		return kInfinity;
	}
	const float u = vec::dot(p_point, across), v = vec::dot(p_point, along);
	float horizontal = 0.0f;
	if (!inside_outline(outline, u, v)) {
		horizontal = kInfinity;
		for (size_t i = 0, j = outline.size() - 1; i < outline.size(); j = i++) {
			horizontal = std::min(horizontal, segment_distance(outline[j], outline[i], u, v));
		}
	}
	const float h = vec::dot(p_point, up);
	const float vertical = std::max({ 0.0f, floor - h, h - (floor + height) });
	return std::sqrt(horizontal * horizontal + vertical * vertical);
}

Vec3 AreaVolume::centre() const {
	if (kind == Kind::Box) {
		return center;
	}
	float u = 0.0f, v = 0.0f;
	for (const std::array<float, 2> &p : outline) {
		u += p[0];
		v += p[1];
	}
	const float n = float(std::max<size_t>(1, outline.size()));
	return vec::add(vec::add(vec::scale(across, u / n), vec::scale(along, v / n)), vec::scale(up, floor));
}

Result<AreaVolume> read_area_volume(const EbxInstance &p_shape, const LevelTransform &p_transform) {
	if (p_shape.is_a("OBBData")) {
		const std::optional<EbxValue> half = p_shape.get("HalfExtents");
		const std::optional<Vec3> extents = half ? half->as_vec3() : std::nullopt;
		if (!extents) {
			return volume_error("an OBBData has no HalfExtents");
		}
		return AreaVolume::box(p_transform, *extents);
	}
	if (!p_shape.is_a("VolumeVectorShapeData")) {
		return volume_error("a " + std::string(p_shape.type_name()) + " is not a VolumeVectorShapeData or OBBData");
	}
	const std::optional<EbxValue> points = p_shape.get("Points");
	const OptionalEbxArray array = points ? points->as_array() : OptionalEbxArray();
	if (!array || array->size() < 3 || array->size() > kMaxAreaVolumePoints) {
		return volume_error("a VolumeVectorShapeData has " + std::to_string(array ? array->size() : 0) + " points");
	}
	std::vector<Vec3> world;
	world.reserve(array->size());
	for (const EbxValue p : *array) {
		const std::optional<Vec3> v = p.as_vec3();
		if (!v || !finite(*v)) {
			return volume_error("a VolumeVectorShapeData point is not a finite Vec3");
		}
		world.push_back(p_transform.transform_point(*v));
	}
	Vec3 up{ 0.0f, 1.0f, 0.0f };
	bool tilted = false;
	const std::optional<EbxValue> normals = p_shape.get("Normals");
	const OptionalEbxArray narray = normals ? normals->as_array() : OptionalEbxArray();
	bool first = true;
	if (narray) {
		for (const EbxValue n : *narray) {
			const std::optional<Vec3> v = n.as_vec3();
			if (!v || !finite(*v) || !(vec::length(*v) > 0.0f)) {
				continue;
			}
			const Vec3 unit = vec::normalize(*v);
			if (first) {
				up = unit;
				first = false;
			} else if (vec::length(vec::sub(unit, up)) > kTiltTolerance) {
				tilted = true;
				break;
			}
		}
	}
	const Vec3 world_up = p_transform.rotate(up);
	if (!(vec::length(world_up) > 1e-6f) || !finite(world_up)) {
		return volume_error("a VolumeVectorShapeData transform has a degenerate up axis");
	}
	const std::optional<EbxValue> height = p_shape.get("Height");
	const float h = float(height ? height->as_f64().value_or(0.0) : 0.0) * p_transform.scale;
	if (!std::isfinite(h)) {
		return volume_error("a VolumeVectorShapeData has a non-finite Height");
	}
	AreaVolume out = AreaVolume::prism(world, world_up, h);
	out.tilted = tilted;
	const std::optional<EbxValue> closed = p_shape.get("IsClosed");
	out.open = closed && !closed->as_bool().value_or(true);
	const std::optional<EbxValue> tension = p_shape.get("Tension");
	out.curved = tension && tension->as_f64().value_or(0.0) != 0.0;
	return out;
}

}
