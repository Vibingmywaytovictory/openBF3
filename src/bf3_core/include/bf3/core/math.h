#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <iterator>
#include <numbers>
#include <type_traits>

namespace bf3::core {

inline constexpr double kDegToRad = std::numbers::pi / 180.0;
inline constexpr float kDegToRadF = float(kDegToRad);

template <class V, class F> struct ComponentIterator {
	using iterator_category = std::forward_iterator_tag;
	using value_type = std::remove_const_t<F>;
	using difference_type = std::ptrdiff_t;
	using pointer = F *;
	using reference = F &;

	V *vector = nullptr;
	size_t index = 0;

	constexpr F &operator*() const { return (*vector)[index]; }
	constexpr ComponentIterator &operator++() {
		++index;
		return *this;
	}
	constexpr ComponentIterator operator++(int) {
		ComponentIterator before = *this;
		++index;
		return before;
	}
	constexpr bool operator==(const ComponentIterator &) const = default;
};

struct Vec3 {
	float x = 0.0f, y = 0.0f, z = 0.0f;

	constexpr float &operator[](size_t p_i) { return p_i == 0 ? x : p_i == 1 ? y : z; }
	constexpr const float &operator[](size_t p_i) const { return p_i == 0 ? x : p_i == 1 ? y : z; }
	static constexpr size_t size() { return 3; }
	constexpr ComponentIterator<Vec3, float> begin() { return { this, 0 }; }
	constexpr ComponentIterator<Vec3, float> end() { return { this, 3 }; }
	constexpr ComponentIterator<const Vec3, const float> begin() const { return { this, 0 }; }
	constexpr ComponentIterator<const Vec3, const float> end() const { return { this, 3 }; }
	auto operator<=>(const Vec3 &) const = default;
};
static_assert(
		sizeof(Vec3) == 3 * sizeof(float) && std::is_trivially_copyable_v<Vec3> && std::is_standard_layout_v<Vec3>);

constexpr std::array<float, 3> to_array(const Vec3 &p_v) {
	return { p_v.x, p_v.y, p_v.z };
}

constexpr Vec3 to_vec3(const std::array<float, 3> &p_a) {
	return { p_a[0], p_a[1], p_a[2] };
}

namespace vec {
float dot(const Vec3 &p_a, const Vec3 &p_b);
Vec3 cross(const Vec3 &p_a, const Vec3 &p_b);
Vec3 sub(const Vec3 &p_a, const Vec3 &p_b);
Vec3 add(const Vec3 &p_a, const Vec3 &p_b);
Vec3 scale(const Vec3 &p_a, float p_s);
float length(const Vec3 &p_a);
float horizontal_distance(const Vec3 &p_a, const Vec3 &p_b);
Vec3 normalize(const Vec3 &p_a);
}

struct Transform {
	Vec3 right{ 1.0f, 0.0f, 0.0f };
	Vec3 up{ 0.0f, 1.0f, 0.0f };
	Vec3 forward{ 0.0f, 0.0f, 1.0f };
	Vec3 trans{ 0.0f, 0.0f, 0.0f };

	static Transform identity() { return Transform(); }
	static Transform from_axes(const std::array<Vec3, 3> &p_axes, const Vec3 &p_trans);
	static Transform from_translation(const Vec3 &p_trans);
	static Transform from_rotation_translation(const std::array<float, 4> &p_rotation, const Vec3 &p_trans);
	static Transform from_translation_rotation_scale(
			const Vec3 &p_translation, const std::array<float, 4> &p_rotation, const Vec3 &p_scale);

	constexpr Vec3 &axis(size_t p_i) { return p_i == 0 ? right : p_i == 1 ? up : forward; }
	constexpr const Vec3 &axis(size_t p_i) const { return p_i == 0 ? right : p_i == 1 ? up : forward; }
	std::array<Vec3, 3> axes() const { return { right, up, forward }; }

	Vec3 transform_vector(const Vec3 &p_v) const;
	Vec3 transform_point(const Vec3 &p_p) const;

	Transform then_inner(const Transform &p_inner) const;
	float determinant() const;

	std::array<float, 4> transform_plane(const std::array<float, 4> &p_plane) const;

	bool operator==(const Transform &) const = default;
};
static_assert(sizeof(Transform) == 4 * sizeof(Vec3) && std::is_trivially_copyable_v<Transform>);

}
