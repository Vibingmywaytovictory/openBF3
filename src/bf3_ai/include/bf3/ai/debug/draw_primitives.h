#pragma once

#include "bf3/ai/knowledge/knowledge_context.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bf3::ai {

enum class TextAlignX : uint8_t { Left = 0, Centre = 1, Right = 2 };
enum class TextAlignY : uint8_t { Top = 0, Centre = 1, Bottom = 2 };

class DebugRenderer {
public:
	virtual ~DebugRenderer() = default;
	virtual void draw_line(const std::vector<Vec3> &p_points, uint32_t p_colour, float p_width) = 0;
	virtual void draw_circle(const Vec3 &p_normal, const Vec3 &p_centre, float p_radius, uint32_t p_colour,
			bool p_filled, int p_segments) = 0;
	virtual void draw_screen_text(const Vec2 &p_position, const std::vector<std::string> &p_lines, int p_size,
			uint32_t p_colour, TextAlignX p_align_x, TextAlignY p_align_y) = 0;
	virtual void draw_3d_text(const Vec3 &p_position, const std::vector<std::string> &p_lines, int p_size,
			uint32_t p_colour, TextAlignX p_align_x, TextAlignY p_align_y) = 0;
	virtual void draw_triangle(
			const std::vector<Vec3> &p_vertices, const std::vector<uint32_t> &p_colours, bool p_filled) = 0;
};

class DrawPrimitive {
public:
	DrawPrimitive(sim::Tick p_creation, float p_life_time, uint32_t p_colour) :
			creation_(p_creation), life_time_(p_life_time), colour_(p_colour) {}
	virtual ~DrawPrimitive() = default;
	virtual bool draw(DebugRenderer &r_renderer) const = 0;
	bool expired(sim::Tick p_now, double p_tick_seconds) const;
	float life_time() const { return life_time_; }
	uint32_t colour() const { return colour_; }
	void set_life_time(float p_life_time) { life_time_ = p_life_time; }
	void set_colour(uint32_t p_colour) { colour_ = p_colour; }

private:
	sim::Tick creation_ = 0;
	float life_time_ = 0.0f;
	uint32_t colour_ = 0;
};

class DPStaticLine final : public DrawPrimitive {
public:
	DPStaticLine(
			sim::Tick p_creation, float p_life_time, uint32_t p_colour, std::vector<Vec3> p_points, float p_width) :
			DrawPrimitive(p_creation, p_life_time, p_colour), points(std::move(p_points)), width(p_width) {}
	bool draw(DebugRenderer &r_renderer) const override;

	std::vector<Vec3> points;
	float width = 0.0f;
};

class DPStaticCircle final : public DrawPrimitive {
public:
	DPStaticCircle(sim::Tick p_creation, float p_life_time, uint32_t p_colour, const Vec3 &p_position, float p_radius,
			const Vec3 &p_normal, bool p_filled, int p_segments) :
			DrawPrimitive(p_creation, p_life_time, p_colour), position(p_position), radius(p_radius), normal(p_normal),
			filled(p_filled), segments(p_segments) {}
	bool draw(DebugRenderer &r_renderer) const override;

	Vec3 position{};
	float radius = 0.0f;
	Vec3 normal{};
	bool filled = false;
	int segments = 0;
};

inline void draw_text(DebugRenderer &r_renderer, const Vec2 &p_position, const std::vector<std::string> &p_lines,
		int p_size, uint32_t p_colour, TextAlignX p_align_x, TextAlignY p_align_y) {
	r_renderer.draw_screen_text(p_position, p_lines, p_size, p_colour, p_align_x, p_align_y);
}

inline void draw_text(DebugRenderer &r_renderer, const Vec3 &p_position, const std::vector<std::string> &p_lines,
		int p_size, uint32_t p_colour, TextAlignX p_align_x, TextAlignY p_align_y) {
	r_renderer.draw_3d_text(p_position, p_lines, p_size, p_colour, p_align_x, p_align_y);
}

template <class P> class DPStaticText final : public DrawPrimitive {
public:
	DPStaticText(sim::Tick p_creation, float p_life_time, uint32_t p_colour, const P &p_position,
			std::vector<std::string> p_lines, int p_size, TextAlignX p_align_x, TextAlignY p_align_y) :
			DrawPrimitive(p_creation, p_life_time, p_colour), position(p_position), lines(std::move(p_lines)),
			size(p_size), align_x(p_align_x), align_y(p_align_y) {}
	bool draw(DebugRenderer &r_renderer) const override {
		draw_text(r_renderer, position, lines, size, colour(), align_x, align_y);
		return true;
	}

	P position{};
	std::vector<std::string> lines;
	int size = 0;
	TextAlignX align_x = TextAlignX::Left;
	TextAlignY align_y = TextAlignY::Top;
};

using DPStatic2dText = DPStaticText<Vec2>;
using DPStatic3dText = DPStaticText<Vec3>;

class DPStaticTriangle final : public DrawPrimitive {
public:
	DPStaticTriangle(sim::Tick p_creation, float p_life_time, std::vector<Vec3> p_vertices,
			std::vector<uint32_t> p_colours, bool p_filled);
	bool draw(DebugRenderer &r_renderer) const override;

	std::vector<Vec3> vertices;
	std::vector<uint32_t> colours;
	bool filled = false;
};

class DPStaticPolygon final : public DrawPrimitive {
public:
	DPStaticPolygon(sim::Tick p_creation, float p_life_time, const std::vector<Vec3> &p_vertices,
			const std::vector<uint32_t> &p_colours, bool p_filled);
	bool draw(DebugRenderer &r_renderer) const override;
	size_t triangle_count() const { return triangles_.size(); }

	bool filled = false;

private:
	std::vector<std::vector<Vec3>> triangles_;
	std::vector<std::vector<uint32_t>> colours_;
};

class DPManager {
public:
	explicit DPManager(const AiTimer &p_timer) : timer_(&p_timer) {}

	DPStaticCircle *create_static_circle(float p_life_time, uint32_t p_colour, const Vec3 &p_position, float p_radius,
			const Vec3 &p_normal, bool p_filled, int p_segments);
	DPStaticLine *create_static_line(
			float p_life_time, uint32_t p_colour, const Vec3 &p_start, const Vec3 &p_end, float p_width);
	DPStaticLine *create_static_line(
			float p_life_time, uint32_t p_colour, const std::vector<Vec3> &p_points, float p_width);
	DPStatic2dText *create_static_2d_text(float p_life_time, uint32_t p_colour, const Vec2 &p_position,
			const std::vector<std::string> &p_lines, int p_size, TextAlignX p_align_x, TextAlignY p_align_y);
	DPStatic3dText *create_static_3d_text(float p_life_time, uint32_t p_colour, const Vec3 &p_position,
			const std::vector<std::string> &p_lines, int p_size, TextAlignX p_align_x, TextAlignY p_align_y);
	DPStaticTriangle *create_static_triangle(float p_life_time, const std::vector<Vec3> &p_vertices,
			const std::vector<uint32_t> &p_colours, bool p_filled);
	DPStaticPolygon *create_static_polygon(float p_life_time, const std::vector<Vec3> &p_vertices,
			const std::vector<uint32_t> &p_colours, bool p_filled);

	bool delete_circle(DrawPrimitive *&r_primitive) { return erase(circles_, r_primitive); }
	bool delete_line(DrawPrimitive *&r_primitive) { return erase(lines_, r_primitive); }
	bool delete_2d_text(DrawPrimitive *&r_primitive) { return erase(text_2d_, r_primitive); }
	bool delete_3d_text(DrawPrimitive *&r_primitive) { return erase(text_3d_, r_primitive); }
	bool delete_triangle(DrawPrimitive *&r_primitive) { return erase(triangles_, r_primitive); }
	bool delete_polygon(DrawPrimitive *&r_primitive) { return erase(polygons_, r_primitive); }

	DPStaticCircle *circle(const DrawPrimitive *p_primitive) const { return find(circles_, p_primitive); }
	DPStaticLine *line(const DrawPrimitive *p_primitive) const { return find(lines_, p_primitive); }
	DPStatic2dText *text_2d(const DrawPrimitive *p_primitive) const { return find(text_2d_, p_primitive); }
	DPStatic3dText *text_3d(const DrawPrimitive *p_primitive) const { return find(text_3d_, p_primitive); }
	DPStaticTriangle *triangle(const DrawPrimitive *p_primitive) const { return find(triangles_, p_primitive); }
	DPStaticPolygon *polygon(const DrawPrimitive *p_primitive) const { return find(polygons_, p_primitive); }

	void set_text_gamma(float p_gamma) { text_gamma_ = p_gamma; }
	float text_gamma() const { return text_gamma_; }
	void draw_all(DebugRenderer &r_renderer);
	void nodraw_update();
	void delete_all();
	size_t size() const;

private:
	template <class T> using Store = std::map<const DrawPrimitive *, std::unique_ptr<T>>;

	template <class T> static bool erase(Store<T> &r_store, DrawPrimitive *&r_primitive) {
		if (r_store.erase(r_primitive) == 0) {
			return false;
		}
		r_primitive = nullptr;
		return true;
	}
	template <class T> static T *find(const Store<T> &p_store, const DrawPrimitive *p_primitive) {
		const auto found = p_store.find(p_primitive);
		return found != p_store.end() ? found->second.get() : nullptr;
	}
	template <class T> T *insert(Store<T> &r_store, std::unique_ptr<T> p_primitive) {
		T *const raw = p_primitive.get();
		r_store.emplace(raw, std::move(p_primitive));
		return raw;
	}

	const AiTimer *timer_ = nullptr;
	Store<DPStaticLine> lines_;
	Store<DPStaticCircle> circles_;
	Store<DPStatic2dText> text_2d_;
	Store<DPStatic3dText> text_3d_;
	Store<DPStaticTriangle> triangles_;
	Store<DPStaticPolygon> polygons_;
	float text_gamma_ = 1.0f;
};

}
