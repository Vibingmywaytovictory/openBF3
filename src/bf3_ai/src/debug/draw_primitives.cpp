#include "bf3/ai/debug/draw_primitives.h"

namespace bf3::ai {

namespace {

template <class Store>
void draw_store(Store &r_store, sim::Tick p_now, double p_tick_seconds, DebugRenderer &r_renderer) {
	for (auto it = r_store.begin(); it != r_store.end();) {
		if (it->second->expired(p_now, p_tick_seconds) || !it->second->draw(r_renderer)) {
			it = r_store.erase(it);
		} else {
			++it;
		}
	}
}

template <class Store> void update_store(Store &r_store, sim::Tick p_now, double p_tick_seconds) {
	for (auto it = r_store.begin(); it != r_store.end();) {
		if (it->second->expired(p_now, p_tick_seconds)) {
			it = r_store.erase(it);
		} else {
			++it;
		}
	}
}

}

bool DrawPrimitive::expired(sim::Tick p_now, double p_tick_seconds) const {
	return (double(p_now) - double(creation_)) * p_tick_seconds > double(life_time_);
}

bool DPStaticLine::draw(DebugRenderer &r_renderer) const {
	r_renderer.draw_line(points, colour(), width);
	return true;
}

bool DPStaticCircle::draw(DebugRenderer &r_renderer) const {
	r_renderer.draw_circle(normal, position, radius, colour(), filled, segments);
	return true;
}

DPStaticTriangle::DPStaticTriangle(sim::Tick p_creation, float p_life_time, std::vector<Vec3> p_vertices,
		std::vector<uint32_t> p_colours, bool p_filled) :
		DrawPrimitive(p_creation, p_life_time, p_colours.at(0)), vertices(std::move(p_vertices)),
		colours(std::move(p_colours)), filled(p_filled) {}

bool DPStaticTriangle::draw(DebugRenderer &r_renderer) const {
	r_renderer.draw_triangle(vertices, colours, filled);
	return true;
}

DPStaticPolygon::DPStaticPolygon(sim::Tick p_creation, float p_life_time, const std::vector<Vec3> &p_vertices,
		const std::vector<uint32_t> &p_colours, bool p_filled) :
		DrawPrimitive(p_creation, p_life_time, p_colours.at(0)), filled(p_filled) {
	const size_t count = p_vertices.size();
	if (count < 3 || p_colours.size() < count) {
		return;
	}
	triangles_.resize(count - 2);
	colours_.resize(count - 2);
	for (size_t i = 1; i + 1 < count; ++i) {
		triangles_[i - 1] = { p_vertices[0], p_vertices[i], p_vertices[i + 1] };
		colours_[i - 1] = { p_colours[0], p_colours[i], p_colours[i + 1] };
	}
}

bool DPStaticPolygon::draw(DebugRenderer &r_renderer) const {
	for (size_t i = 0; i < triangles_.size(); ++i) {
		r_renderer.draw_triangle(triangles_[i], colours_[i], filled);
	}
	return true;
}

DPStaticCircle *DPManager::create_static_circle(float p_life_time, uint32_t p_colour, const Vec3 &p_position,
		float p_radius, const Vec3 &p_normal, bool p_filled, int p_segments) {
	return insert(circles_,
			std::make_unique<DPStaticCircle>(
					timer_->game_tick(), p_life_time, p_colour, p_position, p_radius, p_normal, p_filled, p_segments));
}

DPStaticLine *DPManager::create_static_line(
		float p_life_time, uint32_t p_colour, const Vec3 &p_start, const Vec3 &p_end, float p_width) {
	return create_static_line(p_life_time, p_colour, std::vector<Vec3>{ p_start, p_end, p_end }, p_width);
}

DPStaticLine *DPManager::create_static_line(
		float p_life_time, uint32_t p_colour, const std::vector<Vec3> &p_points, float p_width) {
	return insert(
			lines_, std::make_unique<DPStaticLine>(timer_->game_tick(), p_life_time, p_colour, p_points, p_width));
}

DPStatic2dText *DPManager::create_static_2d_text(float p_life_time, uint32_t p_colour, const Vec2 &p_position,
		const std::vector<std::string> &p_lines, int p_size, TextAlignX p_align_x, TextAlignY p_align_y) {
	return insert(text_2d_,
			std::make_unique<DPStatic2dText>(
					timer_->game_tick(), p_life_time, p_colour, p_position, p_lines, p_size, p_align_x, p_align_y));
}

DPStatic3dText *DPManager::create_static_3d_text(float p_life_time, uint32_t p_colour, const Vec3 &p_position,
		const std::vector<std::string> &p_lines, int p_size, TextAlignX p_align_x, TextAlignY p_align_y) {
	return insert(text_3d_,
			std::make_unique<DPStatic3dText>(
					timer_->game_tick(), p_life_time, p_colour, p_position, p_lines, p_size, p_align_x, p_align_y));
}

DPStaticTriangle *DPManager::create_static_triangle(
		float p_life_time, const std::vector<Vec3> &p_vertices, const std::vector<uint32_t> &p_colours, bool p_filled) {
	return insert(triangles_,
			std::make_unique<DPStaticTriangle>(timer_->game_tick(), p_life_time, p_vertices, p_colours, p_filled));
}

DPStaticPolygon *DPManager::create_static_polygon(
		float p_life_time, const std::vector<Vec3> &p_vertices, const std::vector<uint32_t> &p_colours, bool p_filled) {
	return insert(polygons_,
			std::make_unique<DPStaticPolygon>(timer_->game_tick(), p_life_time, p_vertices, p_colours, p_filled));
}

void DPManager::draw_all(DebugRenderer &r_renderer) {
	const sim::Tick now = timer_->game_tick();
	const double tick_seconds = timer_->tick_seconds();
	draw_store(circles_, now, tick_seconds, r_renderer);
	draw_store(text_2d_, now, tick_seconds, r_renderer);
	draw_store(text_3d_, now, tick_seconds, r_renderer);
	draw_store(triangles_, now, tick_seconds, r_renderer);
	draw_store(polygons_, now, tick_seconds, r_renderer);
	draw_store(lines_, now, tick_seconds, r_renderer);
}

void DPManager::nodraw_update() {
	const sim::Tick now = timer_->game_tick();
	const double tick_seconds = timer_->tick_seconds();
	update_store(circles_, now, tick_seconds);
	update_store(text_2d_, now, tick_seconds);
	update_store(text_3d_, now, tick_seconds);
	update_store(triangles_, now, tick_seconds);
	update_store(polygons_, now, tick_seconds);
	update_store(lines_, now, tick_seconds);
}

void DPManager::delete_all() {
	circles_.clear();
	text_2d_.clear();
	text_3d_.clear();
	triangles_.clear();
	polygons_.clear();
	lines_.clear();
}

size_t DPManager::size() const {
	return circles_.size() + text_2d_.size() + text_3d_.size() + triangles_.size() + polygons_.size() + lines_.size();
}

}
