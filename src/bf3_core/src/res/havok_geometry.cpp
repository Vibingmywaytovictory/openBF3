#include "bf3/core/res/havok.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <unordered_map>

namespace bf3::core::res {

namespace {

constexpr size_t kRoundSegments = 12;

using V = std::array<double, 3>;

V dsub(const V &p_a, const V &p_b) {
	return { p_a[0] - p_b[0], p_a[1] - p_b[1], p_a[2] - p_b[2] };
}
V dcross(const V &p_a, const V &p_b) {
	return { p_a[1] * p_b[2] - p_a[2] * p_b[1], p_a[2] * p_b[0] - p_a[0] * p_b[2], p_a[0] * p_b[1] - p_a[1] * p_b[0] };
}
double ddot(const V &p_a, const V &p_b) {
	return p_a[0] * p_b[0] + p_a[1] * p_b[1] + p_a[2] * p_b[2];
}
double dlength(const V &p_a) {
	return std::sqrt(ddot(p_a, p_a));
}
V dnormalize(const V &p_a) {
	const double len = dlength(p_a);
	return len > 0.0 ? V{ p_a[0] / len, p_a[1] / len, p_a[2] / len } : p_a;
}

int64_t total_key(double p_value) {
	int64_t bits;
	std::memcpy(&bits, &p_value, 8);
	return bits ^ int64_t(uint64_t(bits >> 63) >> 1);
}
bool total_less(double p_a, double p_b) {
	return total_key(p_a) < total_key(p_b);
}

double rmin(double p_a, double p_b) {
	if (std::isnan(p_a)) {
		return p_b;
	}
	if (std::isnan(p_b)) {
		return p_a;
	}
	return p_b < p_a ? p_b : p_a;
}
double rmax(double p_a, double p_b) {
	if (std::isnan(p_a)) {
		return p_b;
	}
	if (std::isnan(p_b)) {
		return p_a;
	}
	return p_b > p_a ? p_b : p_a;
}

struct Hull {
	std::vector<Vec3> points;
	std::vector<std::array<size_t, 3>> faces;
	std::vector<Vec4> planes;
};

struct Face {
	std::array<size_t, 3> v{};
	V normal{};
	double offset = 0.0;
	std::vector<size_t> outside;
	bool alive = true;

	Face(const std::vector<V> &p_points, const std::array<size_t, 3> &p_v) :
			v(p_v) {
		normal = dnormalize(dcross(dsub(p_points[v[1]], p_points[v[0]]), dsub(p_points[v[2]], p_points[v[0]])));
		offset = ddot(normal, p_points[v[0]]);
	}
	double distance(const V &p_p) const { return ddot(normal, p_p) - offset; }
};

uint64_t edge_key(size_t p_a, size_t p_b) {
	return (uint64_t(p_a) << 32) | uint64_t(p_b);
}

struct Builder {
	const std::vector<V> &points;
	double tolerance;
	std::vector<Face> faces;
	std::unordered_map<uint64_t, size_t> edges;

	size_t add_face(const std::array<size_t, 3> &p_v) {
		const size_t index = faces.size();
		for (int k = 0; k < 3; k++) {
			edges[edge_key(p_v[k], p_v[(k + 1) % 3])] = index;
		}
		faces.emplace_back(points, p_v);
		return index;
	}

	bool sees(const Face &p_face, const V &p_p) const { return p_face.alive && p_face.distance(p_p) > tolerance; }

	void assign(const std::vector<size_t> &p_candidates, size_t p_point) {
		const V &p = points[p_point];
		for (size_t face : p_candidates) {
			if (sees(faces[face], p)) {
				faces[face].outside.push_back(p_point);
				return;
			}
		}
		for (Face &face : faces) {
			if (sees(face, p)) {
				face.outside.push_back(p_point);
				return;
			}
		}
	}

	void add_point(size_t p_start) {
		size_t apex = 0;
		{
			const Face &face = faces[p_start];
			if (face.outside.empty()) {
				return;
			}
			apex = face.outside[0];
			double best = face.distance(points[apex]);
			for (size_t k = 1; k < face.outside.size(); k++) {
				const double d = face.distance(points[face.outside[k]]);
				if (!total_less(d, best)) {
					best = d;
					apex = face.outside[k];
				}
			}
		}
		const V p = points[apex];
		std::vector<size_t> visible{ p_start };
		std::vector<size_t> seen{ p_start };
		std::vector<std::pair<size_t, size_t>> horizon;
		for (size_t at = 0; at < visible.size(); at++) {
			const std::array<size_t, 3> v = faces[visible[at]].v;
			for (int k = 0; k < 3; k++) {
				const size_t a = v[k], b = v[(k + 1) % 3];
				const auto it = edges.find(edge_key(b, a));
				if (it != edges.end()) {
					const size_t next = it->second;
					if (std::find(seen.begin(), seen.end(), next) != seen.end()) {
						continue;
					}
					if (faces[next].distance(p) > -tolerance) {
						seen.push_back(next);
						visible.push_back(next);
						continue;
					}
				}
				horizon.emplace_back(a, b);
			}
		}
		std::vector<size_t> orphans;
		for (size_t face : visible) {
			Face &removed = faces[face];
			removed.alive = false;
			orphans.insert(orphans.end(), removed.outside.begin(), removed.outside.end());
			removed.outside.clear();
			const std::array<size_t, 3> v = removed.v;
			for (int k = 0; k < 3; k++) {
				edges.erase(edge_key(v[k], v[(k + 1) % 3]));
			}
		}
		std::vector<size_t> created;
		for (const auto &[a, b] : horizon) {
			created.push_back(add_face({ a, b, apex }));
		}
		for (size_t point : orphans) {
			if (point != apex) {
				assign(created, point);
			}
		}
	}
};

Hull finish(const std::vector<V> &p_points, const std::vector<std::array<size_t, 3>> &p_faces, double p_tolerance) {
	std::vector<size_t> remap(p_points.size(), SIZE_MAX);
	std::vector<V> out_points;
	Hull hull;
	for (const auto &face : p_faces) {
		std::array<size_t, 3> mapped{};
		for (int k = 0; k < 3; k++) {
			const size_t v = face[k];
			if (remap[v] == SIZE_MAX) {
				remap[v] = out_points.size();
				out_points.push_back(p_points[v]);
			}
			mapped[k] = remap[v];
		}
		hull.faces.push_back(mapped);
	}
	std::vector<std::pair<V, double>> planes;
	for (const auto &face : hull.faces) {
		const Face f(out_points, face);
		if (dlength(f.normal) == 0.0) {
			continue;
		}
		bool duplicate = false;
		for (const auto &[n, d] : planes) {
			if (ddot(n, f.normal) > 1.0 - 1e-6 && std::abs(d - f.offset) <= p_tolerance * 10.0) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate) {
			planes.emplace_back(f.normal, f.offset);
		}
	}
	for (const V &p : out_points) {
		hull.points.push_back({ float(p[0]), float(p[1]), float(p[2]) });
	}
	for (const auto &[n, d] : planes) {
		hull.planes.push_back({ float(n[0]), float(n[1]), float(n[2]), float(-d) });
	}
	return hull;
}

Hull flat_hull(const std::vector<V> &p_points, const V &p_normal, double p_tolerance) {
	const V helper = std::abs(p_normal[0]) < 0.9 ? V{ 1.0, 0.0, 0.0 } : V{ 0.0, 1.0, 0.0 };
	const V u = dnormalize(dcross(p_normal, helper));
	const V v = dcross(p_normal, u);
	auto project = [&](size_t i) { return std::pair<double, double>(ddot(p_points[i], u), ddot(p_points[i], v)); };
	std::vector<size_t> order(p_points.size());
	for (size_t i = 0; i < order.size(); i++) {
		order[i] = i;
	}
	std::stable_sort(order.begin(), order.end(), [&](size_t p_a, size_t p_b) {
		const auto pa = project(p_a), pb = project(p_b);
		if (total_key(pa.first) != total_key(pb.first)) {
			return total_less(pa.first, pb.first);
		}
		return total_less(pa.second, pb.second);
	});
	auto turn = [&](size_t o, size_t a, size_t b) {
		const auto po = project(o), pa = project(a), pb = project(b);
		const double ax = pa.first - po.first, ay = pa.second - po.second, bx = pb.first - po.first, by = pb.second - po.second;
		return (ax * by - ay * bx) / rmax(std::hypot(ax, ay), p_tolerance);
	};
	std::vector<size_t> ring;
	for (int pass = 0; pass < 2; pass++) {
		const size_t start = ring.size();
		for (size_t k = 0; k < order.size(); k++) {
			const size_t i = pass == 0 ? order[k] : order[order.size() - 1 - k];
			while (ring.size() >= start + 2 && turn(ring[ring.size() - 2], ring[ring.size() - 1], i) <= p_tolerance) {
				ring.pop_back();
			}
			ring.push_back(i);
		}
		ring.pop_back();
	}
	if (ring.size() < 3) {
		return Hull();
	}
	std::vector<std::array<size_t, 3>> faces;
	for (size_t k = 1; k + 1 < ring.size(); k++) {
		faces.push_back({ ring[0], ring[k], ring[k + 1] });
		faces.push_back({ ring[0], ring[k + 1], ring[k] });
	}
	Hull hull = finish(p_points, faces, p_tolerance);
	const double offset = ddot(p_normal, p_points[ring[0]]);
	hull.planes = { { float(p_normal[0]), float(p_normal[1]), float(p_normal[2]), float(-offset) },
		{ float(-p_normal[0]), float(-p_normal[1]), float(-p_normal[2]), float(offset) } };
	for (size_t k = 0; k < ring.size(); k++) {
		const V &a = p_points[ring[k]];
		const V &b = p_points[ring[(k + 1) % ring.size()]];
		const V side = dnormalize(dcross(dsub(b, a), p_normal));
		hull.planes.push_back({ float(side[0]), float(side[1]), float(side[2]), float(-ddot(side, a)) });
	}
	return hull;
}

Hull convex_hull(const std::vector<Vec3> &p_input) {
	std::vector<V> raw;
	raw.reserve(p_input.size());
	for (const Vec3 &p : p_input) {
		raw.push_back({ double(p[0]), double(p[1]), double(p[2]) });
	}
	if (raw.empty()) {
		return Hull();
	}
	V lo = raw[0], hi = raw[0];
	double magnitude = 0.0;
	for (const V &p : raw) {
		for (int a = 0; a < 3; a++) {
			lo[a] = rmin(lo[a], p[a]);
			hi[a] = rmax(hi[a], p[a]);
			magnitude = rmax(magnitude, std::abs(p[a]));
		}
	}
	const double span = dlength(dsub(hi, lo));
	if (span <= 0.0) {
		return Hull();
	}
	const double tolerance = rmax(span, magnitude) * 1e-9;
	std::vector<V> points;
	points.reserve(raw.size());
	for (const V &p : raw) {
		bool near = false;
		for (const V &q : points) {
			if (dlength(dsub(p, q)) <= tolerance) {
				near = true;
				break;
			}
		}
		if (!near) {
			points.push_back(p);
		}
	}
	if (points.size() < 3) {
		return Hull();
	}

	auto farthest = [&](const std::function<double(const V &)> &p_from) {
		size_t best = 0;
		double best_value = p_from(points[0]);
		for (size_t i = 1; i < points.size(); i++) {
			const double value = p_from(points[i]);
			if (!total_less(value, best_value)) {
				best = i;
				best_value = value;
			}
		}
		return best;
	};
	const size_t i0 = farthest([](const V &p) { return -p[0]; });
	const size_t i1 = farthest([&](const V &p) { return dlength(dsub(p, points[i0])); });
	const V axis = dnormalize(dsub(points[i1], points[i0]));
	auto line_distance = [&](const V &p) { return dlength(dcross(dsub(p, points[i0]), axis)); };
	const size_t i2 = farthest(line_distance);
	if (line_distance(points[i2]) <= tolerance) {
		return Hull();
	}
	const Face base(points, { i0, i1, i2 });
	const size_t i3 = farthest([&](const V &p) { return std::abs(base.distance(p)); });
	if (std::abs(base.distance(points[i3])) <= tolerance) {
		return flat_hull(points, base.normal, tolerance);
	}

	Builder builder{ points, tolerance, {}, {} };
	const bool flip = base.distance(points[i3]) > 0.0;
	const std::array<std::array<size_t, 3>, 4> simplex_faces{ { { i0, i1, i2 }, { i0, i3, i1 }, { i1, i3, i2 }, { i2, i3, i0 } } };
	for (const auto &f : simplex_faces) {
		builder.add_face(flip ? std::array<size_t, 3>{ f[0], f[2], f[1] } : f);
	}
	const std::vector<size_t> initial{ 0, 1, 2, 3 };
	for (size_t point = 0; point < points.size(); point++) {
		if (point != i0 && point != i1 && point != i2 && point != i3) {
			builder.assign(initial, point);
		}
	}
	size_t next = 0;
	while (next < builder.faces.size()) {
		if (builder.faces[next].alive && !builder.faces[next].outside.empty()) {
			builder.add_point(next);
		} else {
			next++;
		}
	}
	std::vector<std::array<size_t, 3>> faces;
	for (const Face &face : builder.faces) {
		if (face.alive) {
			faces.push_back(face.v);
		}
	}
	return finish(points, faces, tolerance);
}

struct Convex {
	std::vector<Vec3> points;
	bool has_planes = false;
	std::vector<Vec4> planes;
	float radius = 0.0f;
	uint32_t user_data = 0;
};

void basis(const Vec3 &p_axis, Vec3 &r_u, Vec3 &r_v) {
	const Vec3 axis = vec::length(p_axis) > 0.0f ? vec::normalize(p_axis) : Vec3{ 0.0f, 1.0f, 0.0f };
	const Vec3 helper = std::abs(axis[0]) < 0.9f ? Vec3{ 1.0f, 0.0f, 0.0f } : Vec3{ 0.0f, 1.0f, 0.0f };
	r_u = vec::normalize(vec::cross(axis, helper));
	r_v = vec::cross(axis, r_u);
}

float scalar_cos(float p_x) {
	volatile float x = p_x;
	return std::cos(float(x));
}
float scalar_sin(float p_x) {
	volatile float x = p_x;
	return std::sin(float(x));
}

void ring(const Vec3 &p_center, const Vec3 &p_u, const Vec3 &p_v, float p_radius, std::vector<Vec3> &r_points) {
	constexpr float kTau = 6.28318530717958647692f;
	for (size_t k = 0; k < kRoundSegments; k++) {
		const float angle = float(k) * kTau / float(kRoundSegments);
		r_points.push_back(vec::add(p_center, vec::add(vec::scale(p_u, p_radius * scalar_cos(angle)), vec::scale(p_v, p_radius * scalar_sin(angle)))));
	}
}

std::vector<Vec3> capsule_points(const Vec3 &p_a, const Vec3 &p_b, float p_radius) {
	const Vec3 span = vec::sub(p_b, p_a);
	const Vec3 axis = vec::length(span) > 0.0f ? vec::normalize(span) : Vec3{ 0.0f, 1.0f, 0.0f };
	Vec3 u, v;
	basis(axis, u, v);
	std::vector<Vec3> points;
	constexpr float kRadsPerDeg = 3.14159265358979323846f / 180.0f;
	const std::pair<Vec3, float> ends[2] = { { p_a, -1.0f }, { p_b, 1.0f } };
	for (const auto &[end, outward] : ends) {
		for (float latitude : { 0.0f, 30.0f, 60.0f }) {
			const float rad = latitude * kRadsPerDeg;
			const float s = scalar_sin(rad);
			const float c = scalar_cos(rad);
			const Vec3 center = vec::add(end, vec::scale(axis, outward * p_radius * s));
			ring(center, u, v, p_radius * c, points);
		}
		points.push_back(vec::add(end, vec::scale(axis, outward * p_radius)));
	}
	return points;
}

bool primitive_convex(const Shape &p_shape, Convex &r_convex) {
	switch (p_shape.type) {
		case ShapeType::Box: {
			const Vec3 &h = p_shape.half_extents;
			for (int corner = 0; corner < 8; corner++) {
				r_convex.points.push_back({ (corner & 1) == 0 ? -h[0] : h[0], (corner & 2) == 0 ? -h[1] : h[1], (corner & 4) == 0 ? -h[2] : h[2] });
			}
			r_convex.has_planes = true;
			r_convex.planes = { { 1.0f, 0.0f, 0.0f, -h[0] }, { -1.0f, 0.0f, 0.0f, -h[0] }, { 0.0f, 1.0f, 0.0f, -h[1] },
				{ 0.0f, -1.0f, 0.0f, -h[1] }, { 0.0f, 0.0f, 1.0f, -h[2] }, { 0.0f, 0.0f, -1.0f, -h[2] } };
			r_convex.radius = p_shape.radius;
			r_convex.user_data = p_shape.user_data;
			return true;
		}
		case ShapeType::Sphere:
			r_convex.points = capsule_points({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, p_shape.radius);
			r_convex.radius = 0.0f;
			r_convex.user_data = p_shape.user_data;
			return true;
		case ShapeType::Capsule:
			r_convex.points = capsule_points(p_shape.a, p_shape.b, p_shape.radius);
			r_convex.radius = 0.0f;
			r_convex.user_data = p_shape.user_data;
			return true;
		case ShapeType::Cylinder: {
			Vec3 u, v;
			basis(vec::sub(p_shape.b, p_shape.a), u, v);
			ring(p_shape.a, u, v, p_shape.cylinder_radius, r_convex.points);
			ring(p_shape.b, u, v, p_shape.cylinder_radius, r_convex.points);
			r_convex.radius = p_shape.radius;
			r_convex.user_data = p_shape.user_data;
			return true;
		}
		case ShapeType::ConvexVertices:
			r_convex.points = p_shape.points;
			r_convex.radius = p_shape.radius;
			r_convex.user_data = p_shape.user_data;
			return true;
		default:
			return false;
	}
}

struct Visitor {
	std::function<void(const Convex &, const Transform &, uint32_t)> convex;
	std::function<void(const CompressedMesh &, const Transform &, uint32_t)> compressed;
	std::function<void(const TrianglesSubpart &, const Transform &, uint32_t)> triangles;
};

void visit(const Shape &p_shape, const Transform &p_transform, uint32_t p_part, const Visitor &p_visitor) {
	switch (p_shape.type) {
		case ShapeType::MoppBvTree:
		case ShapeType::List:
			for (const Shape &child : p_shape.children) {
				visit(child, p_transform, p_part, p_visitor);
			}
			break;
		case ShapeType::ConvexTranslate:
			for (const Shape &child : p_shape.children) {
				visit(child, p_transform.then_inner(Transform::from_translation(p_shape.translation)), p_part, p_visitor);
			}
			break;
		case ShapeType::ConvexTransform:
			for (const Shape &child : p_shape.children) {
				visit(child, p_transform.then_inner(p_shape.transform), p_part, p_visitor);
			}
			break;
		case ShapeType::ExtendedMesh:
			if (p_shape.extended) {
				for (const ShapesSubpart &subpart : p_shape.extended->shape_subparts) {
					const Transform placed = p_transform.then_inner(subpart.transform());
					for (const Shape &child : subpart.children) {
						visit(child, placed, subpart.user_data, p_visitor);
					}
				}
				if (p_visitor.triangles) {
					for (const TrianglesSubpart &subpart : p_shape.extended->triangle_subparts) {
						p_visitor.triangles(subpart, p_transform, p_part);
					}
				}
			}
			break;
		case ShapeType::CompressedMesh:
			if (p_shape.compressed && p_visitor.compressed) {
				p_visitor.compressed(*p_shape.compressed, p_transform, p_part);
			}
			break;
		case ShapeType::Unsupported:
			break;
		default: {
			Convex convex;
			if (primitive_convex(p_shape, convex) && p_visitor.convex) {
				p_visitor.convex(convex, p_transform, p_part);
			}
			break;
		}
	}
}

const Transform &placed_by(const CompressedMesh &p_mesh, int32_t p_index, const Transform &p_transform, Transform &r_scratch) {
	if (p_index >= 0 && size_t(p_index) < p_mesh.transforms.size()) {
		r_scratch = p_transform.then_inner(p_mesh.transforms[size_t(p_index)]);
		return r_scratch;
	}
	return p_transform;
}

void compressed_triangles(const CompressedMesh &p_mesh, const Transform &p_transform, std::vector<Tri> &r_out) {
	for (const CompressedChunk &chunk : p_mesh.chunks) {
		const CompressedChunk &geometry =
				chunk.reference >= 0 && size_t(chunk.reference) < p_mesh.chunks.size() ? p_mesh.chunks[size_t(chunk.reference)] : chunk;
		Transform scratch;
		const Transform &placed = placed_by(p_mesh, chunk.transform_index, p_transform, scratch);
		auto vertex = [&](uint16_t p_index, Vec3 &r_v) {
			if (p_index >= geometry.vertices.size()) {
				return false;
			}
			const auto &q = geometry.vertices[p_index];
			r_v = placed.transform_point({ geometry.offset[0] + float(q[0]) * p_mesh.error, geometry.offset[1] + float(q[1]) * p_mesh.error,
					geometry.offset[2] + float(q[2]) * p_mesh.error });
			return true;
		};
		auto push = [&](uint16_t a, uint16_t b, uint16_t c) {
			if (a == b || b == c || a == c) {
				return;
			}
			Tri tri;
			if (vertex(a, tri.v[0]) && vertex(b, tri.v[1]) && vertex(c, tri.v[2])) {
				tri.material = chunk.material;
				tri.part = chunk.part;
				r_out.push_back(tri);
			}
		};
		size_t at = 0;
		for (uint16_t strip_length : geometry.strip_lengths) {
			const size_t strip = strip_length;
			if (at + strip > geometry.indices.size()) {
				break;
			}
			const uint16_t *indices = geometry.indices.data() + at;
			for (size_t k = 0; k + 2 < strip; k++) {
				if (k % 2 == 0) {
					push(indices[k], indices[k + 1], indices[k + 2]);
				} else {
					push(indices[k + 1], indices[k], indices[k + 2]);
				}
			}
			at += strip;
		}
		if (at <= geometry.indices.size()) {
			for (size_t k = at; k + 3 <= geometry.indices.size(); k += 3) {
				push(geometry.indices[k], geometry.indices[k + 1], geometry.indices[k + 2]);
			}
		}
	}
	for (const BigTriangle &triangle : p_mesh.big_triangles) {
		Transform scratch;
		const Transform &placed = placed_by(p_mesh, triangle.transform_index, p_transform, scratch);
		if (triangle.indices[0] < p_mesh.big_vertices.size() && triangle.indices[1] < p_mesh.big_vertices.size() &&
				triangle.indices[2] < p_mesh.big_vertices.size()) {
			Tri tri;
			for (int k = 0; k < 3; k++) {
				tri.v[k] = placed.transform_point(p_mesh.big_vertices[triangle.indices[k]]);
			}
			tri.material = triangle.material;
			tri.part = triangle.part;
			r_out.push_back(tri);
		}
	}
}

void subpart_triangles(const TrianglesSubpart &p_subpart, const Transform &p_transform, std::vector<Tri> &r_out) {
	const Transform placed = p_transform.then_inner(p_subpart.transform);
	for (size_t index = 0; index < p_subpart.triangles.size(); index++) {
		const auto &triangle = p_subpart.triangles[index];
		if (triangle[0] < p_subpart.vertices.size() && triangle[1] < p_subpart.vertices.size() && triangle[2] < p_subpart.vertices.size()) {
			Tri tri;
			for (int k = 0; k < 3; k++) {
				tri.v[k] = placed.transform_point(p_subpart.vertices[triangle[k]]);
			}
			tri.material = index < p_subpart.materials.size() ? p_subpart.materials[index] : 0;
			tri.part = 0;
			tri.user_data = p_subpart.user_data;
			r_out.push_back(tri);
		}
	}
}

}

std::vector<Tri> havok_triangles(const Shape &p_shape, const Transform &p_transform) {
	std::vector<Tri> out;
	Visitor visitor;
	visitor.convex = [&](const Convex &p_convex, const Transform &p_t, uint32_t p_part) {
		const Hull hull = convex_hull(p_convex.points);
		const bool mirrored = p_t.determinant() < 0.0f;
		for (const auto &face : hull.faces) {
			const Vec3 a = p_t.transform_point(hull.points[face[0]]);
			const Vec3 b = p_t.transform_point(hull.points[face[1]]);
			const Vec3 c = p_t.transform_point(hull.points[face[2]]);
			Tri tri;
			tri.v = mirrored ? std::array<Vec3, 3>{ a, c, b } : std::array<Vec3, 3>{ a, b, c };
			tri.part = p_part;
			tri.user_data = p_convex.user_data;
			out.push_back(tri);
		}
	};
	visitor.compressed = [&](const CompressedMesh &p_mesh, const Transform &p_t, uint32_t) { compressed_triangles(p_mesh, p_t, out); };
	visitor.triangles = [&](const TrianglesSubpart &p_subpart, const Transform &p_t, uint32_t) { subpart_triangles(p_subpart, p_t, out); };
	visit(p_shape, p_transform, 0, visitor);
	return out;
}

std::vector<ConvexPiece> havok_convex_pieces(const Shape &p_shape, const Transform &p_transform) {
	std::vector<ConvexPiece> out;
	Visitor visitor;
	visitor.convex = [&](const Convex &p_convex, const Transform &p_t, uint32_t p_part) {
		ConvexPiece piece;
		for (const Vec3 &p : p_convex.points) {
			piece.points.push_back(p_t.transform_point(p));
		}
		const std::vector<Vec4> planes = p_convex.has_planes ? p_convex.planes : convex_hull(p_convex.points).planes;
		for (const Vec4 &plane : planes) {
			piece.planes.push_back(p_t.transform_plane(plane));
		}
		piece.radius = p_convex.radius;
		piece.part = p_part;
		piece.user_data = p_convex.user_data;
		out.push_back(std::move(piece));
	};
	visit(p_shape, p_transform, 0, visitor);
	return out;
}

}
