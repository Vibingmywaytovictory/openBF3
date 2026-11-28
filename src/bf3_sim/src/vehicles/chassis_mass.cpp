#include "bf3/sim/vehicles/chassis_mass.h"

#include "bf3/core/res/havok.h"

#include <array>
#include <cmath>

namespace bf3::sim::vehicles {

namespace {

using Tensor = std::array<std::array<double, 3>, 3>;

struct Moments {
	double volume = 0;
	std::array<double, 3> first{};
	Tensor second{};
};

std::array<double, 3> wide(const Vec3 &p_v) {
	return { double(p_v[0]), double(p_v[1]), double(p_v[2]) };
}

void add_outer(Tensor &r_t, const std::array<double, 3> &p_a, double p_scale) {
	for (size_t i = 0; i < 3; ++i) {
		for (size_t j = 0; j < 3; ++j) {
			r_t[i][j] += p_scale * p_a[i] * p_a[j];
		}
	}
}

void add_point_mass_shift(Tensor &r_t, const std::array<double, 3> &p_c, double p_mass, double p_sign) {
	const double c2 = p_c[0] * p_c[0] + p_c[1] * p_c[1] + p_c[2] * p_c[2];
	for (size_t i = 0; i < 3; ++i) {
		r_t[i][i] += p_sign * p_mass * c2;
	}
	add_outer(r_t, p_c, -p_sign * p_mass);
}

void add_solid(Moments &r_m, double p_volume, const std::array<double, 3> &p_centre, const Tensor &p_central) {
	r_m.volume += p_volume;
	for (size_t i = 0; i < 3; ++i) {
		r_m.first[i] += p_volume * p_centre[i];
		for (size_t j = 0; j < 3; ++j) {
			r_m.second[i][j] += p_central[i][j];
		}
	}
	add_point_mass_shift(r_m.second, p_centre, p_volume, 1.0);
}

std::array<double, 3> unit(const std::array<double, 3> &p_v) {
	const double l = std::sqrt(p_v[0] * p_v[0] + p_v[1] * p_v[1] + p_v[2] * p_v[2]);
	return l > 0.0 ? std::array<double, 3>{ p_v[0] / l, p_v[1] / l, p_v[2] / l } : std::array<double, 3>{ 0, 1, 0 };
}

bool add_box(Moments &r_m, const core::res::Shape &p_shape, const core::Transform &p_t) {
	std::array<double, 3> h{};
	for (size_t i = 0; i < 3; ++i) {
		h[i] = double(p_shape.half_extents[i]) + double(p_shape.radius);
	}
	const double volume = 8.0 * h[0] * h[1] * h[2];
	const std::array<double, 3> local{ (h[1] * h[1] + h[2] * h[2]) / 3.0, (h[0] * h[0] + h[2] * h[2]) / 3.0,
		(h[0] * h[0] + h[1] * h[1]) / 3.0 };
	Tensor central{};
	for (size_t i = 0; i < 3; ++i) {
		add_outer(central, unit(wide(p_t.axis(i))), volume * local[i]);
	}
	add_solid(r_m, volume, wide(p_t.trans), central);
	return volume > 0.0;
}

bool add_cylinder(Moments &r_m, const core::res::Shape &p_shape, const core::Transform &p_t) {
	const std::array<double, 3> a = wide(p_t.transform_point(p_shape.a));
	const std::array<double, 3> b = wide(p_t.transform_point(p_shape.b));
	const std::array<double, 3> u = unit({ b[0] - a[0], b[1] - a[1], b[2] - a[2] });
	const double grow = double(p_shape.radius);
	const double length =
			std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2])) +
			2.0 * grow;
	const double radius = double(p_shape.cylinder_radius) + grow;
	constexpr double kPi = 3.14159265358979323846;
	const double volume = kPi * radius * radius * length;
	const double across = volume * (3.0 * radius * radius + length * length) / 12.0;
	const double along = volume * radius * radius / 2.0;
	Tensor central{};
	for (size_t i = 0; i < 3; ++i) {
		central[i][i] = across;
	}
	add_outer(central, u, along - across);
	add_solid(r_m, volume, { 0.5 * (a[0] + b[0]), 0.5 * (a[1] + b[1]), 0.5 * (a[2] + b[2]) }, central);
	return volume > 0.0;
}

bool add_shape(Moments &r_m, const core::res::Shape &p_shape, const core::Transform &p_t) {
	switch (p_shape.type) {
		case core::res::ShapeType::List:
			for (const core::res::Shape &child : p_shape.children) {
				if (!add_shape(r_m, child, p_t)) {
					return false;
				}
			}
			return !p_shape.children.empty();
		case core::res::ShapeType::ConvexTranslate:
			return p_shape.children.size() == 1 &&
					add_shape(r_m, p_shape.children[0],
							p_t.then_inner(core::Transform::from_translation(p_shape.translation)));
		case core::res::ShapeType::ConvexTransform:
			return p_shape.children.size() == 1 &&
					add_shape(r_m, p_shape.children[0], p_t.then_inner(p_shape.transform));
		case core::res::ShapeType::Box:
			return add_box(r_m, p_shape, p_t);
		case core::res::ShapeType::Cylinder:
			return add_cylinder(r_m, p_shape, p_t);
		default:
			return false;
	}
}

}

std::optional<ChassisVolume> havok_chassis_volume(const core::res::Shape &p_shape) {
	Moments m;
	if (!add_shape(m, p_shape, core::Transform::identity()) || !(m.volume > 0.0) || !std::isfinite(m.volume)) {
		return std::nullopt;
	}
	const std::array<double, 3> c{ m.first[0] / m.volume, m.first[1] / m.volume, m.first[2] / m.volume };
	add_point_mass_shift(m.second, c, m.volume, -1.0);
	ChassisVolume out;
	out.volume = float(m.volume);
	for (size_t i = 0; i < 3; ++i) {
		out.centroid[i] = float(c[i]);
		out.unit_inertia[i] = float(m.second[i][i] / m.volume);
	}
	return out;
}

Vec3 chassis_inertia(
		const ChassisVolume &p_volume, float p_mass, const Vec3 &p_body_modifier, const Vec3 &p_vehicle_modifier) {
	Vec3 out{};
	for (size_t i = 0; i < 3; ++i) {
		out[i] = p_volume.unit_inertia[i] * p_mass * p_body_modifier[i] * p_vehicle_modifier[i];
	}
	return out;
}

}
