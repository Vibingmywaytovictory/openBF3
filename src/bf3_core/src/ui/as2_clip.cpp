#include "bf3/core/ui/as2_clip.h"

#include "bf3/core/level/budget.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>

namespace bf3::core::ui {

namespace {

struct BoundsAcc {
	bool any = false;
	SwfRect r;
	void add(float p_x, float p_y) {
		if (!any) {
			r = SwfRect{ p_x, p_x, p_y, p_y };
			any = true;
			return;
		}
		r.x_min = std::min(r.x_min, p_x);
		r.x_max = std::max(r.x_max, p_x);
		r.y_min = std::min(r.y_min, p_y);
		r.y_max = std::max(r.y_max, p_y);
	}
	void add(const SwfRect &p_rect, const SwfMatrix &p_m) {
		const float xs[2] = { p_rect.x_min, p_rect.x_max };
		const float ys[2] = { p_rect.y_min, p_rect.y_max };
		for (const float x : xs) {
			for (const float y : ys) {
				add(p_m.a * x + p_m.c * y + p_m.tx, p_m.b * x + p_m.d * y + p_m.ty);
			}
		}
	}
};

void accumulate(const SwfScene &p_scene, const SwfSceneNode &p_node, const SwfMatrix &p_to, BoundsAcc &r_acc, uint32_t p_depth) {
	switch (p_node.kind) {
		case SwfNodeKind::Shape:
			if (p_node.shape != nullptr) {
				r_acc.add(p_node.shape->bounds, p_to);
			}
			return;
		case SwfNodeKind::EditText:
			r_acc.add(swf_node_text_bounds(p_node), p_to);
			return;
		case SwfNodeKind::Bitmap:
			if (p_node.sub_image != nullptr) {
				r_acc.add(SwfRect{ 0.0f, float(p_node.sub_image->width()), 0.0f, float(p_node.sub_image->height()) }, p_to);
			}
			return;
		default:
			break;
	}
	if (!p_node.image.empty()) {
		r_acc.add(SwfRect{ 0.0f, float(p_node.image_width), 0.0f, float(p_node.image_height) }, p_to);
	}
	if (p_node.drawing != nullptr && !p_node.drawing->meshes.empty()) {
		r_acc.add(p_node.drawing->bounds, p_to);
	}
	if (p_depth >= kSwfSceneMaxNesting) {
		return;
	}
	for (const SwfNodeId c : p_node.children) {
		const SwfSceneNode *child = p_scene.node(c);
		if (child != nullptr) {
			accumulate(p_scene, *child, swf_concat(p_to, child->matrix), r_acc, p_depth + 1);
		}
	}
}

SwfRect field_bounds(const SwfSceneNode &p_node, const SwfFontSet *p_fonts) {
	if (p_node.text_options.auto_size == SwfTextAutoSize::None || p_fonts == nullptr) {
		return swf_node_text_bounds(p_node);
	}
	level::WorkBudget budget(kSwfTextBudgetUnits);
	const Result<SwfTextLayout> layout = swf_layout_node_text(*p_fonts, p_node, budget);
	return layout ? layout->bounds : swf_node_text_bounds(p_node);
}

int32_t clamp_frame(int32_t p_as2_frame, uint32_t p_total) {
	if (p_total == 0) {
		return -1;
	}
	return std::clamp(p_as2_frame, 1, int32_t(p_total)) - 1;
}

}

SwfRect swf_node_local_bounds(const SwfScene &p_scene, const SwfSceneNode &p_node) {
	BoundsAcc acc;
	accumulate(p_scene, p_node, SwfMatrix{}, acc, 0);
	return acc.any ? acc.r : SwfRect{};
}

std::string as2_number_string(double p_value) {
	if (std::isnan(p_value)) {
		return "NaN";
	}
	if (std::isinf(p_value)) {
		return p_value > 0 ? "Infinity" : "-Infinity";
	}
	if (p_value == std::floor(p_value) && std::fabs(p_value) < 1e15) {
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%.0f", p_value == 0.0 ? 0.0 : p_value);
		return buf;
	}
	char buf[40];
	std::snprintf(buf, sizeof(buf), "%.15g", p_value);
	return buf;
}

As2Clip::As2Clip(SwfScene *p_scene, std::string p_path, As2Stats *p_stats, const SwfFontSet *p_fonts) :
		scene_(p_scene), path_(std::move(p_path)), stats_(p_stats), fonts_(p_fonts) {}

As2Clip As2Clip::child(std::string_view p_name) const {
	return As2Clip(scene_, path_.empty() ? std::string(p_name) : path_ + "." + std::string(p_name), stats_, fonts_);
}

As2Clip As2Clip::parent() const {
	const size_t dot = path_.rfind('.');
	return As2Clip(scene_, dot == std::string::npos ? std::string() : path_.substr(0, dot), stats_, fonts_);
}

const SwfSceneNode *As2Clip::node() const {
	return scene_ != nullptr ? scene_->find_node(path_) : nullptr;
}

SwfSceneNode *As2Clip::mut() const {
	const SwfSceneNode *n = node();
	if (n == nullptr) {
		miss();
	}
	return const_cast<SwfSceneNode *>(n);
}

void As2Clip::miss() const {
	if (stats_ != nullptr) {
		stats_->missing_paths++;
	}
}

void As2Clip::note(const Status &p_status) const {
	if (!p_status.ok() && stats_ != nullptr) {
		stats_->failed_ops++;
		stats_->last_failure = p_status.error().message;
	}
}

float As2Clip::x() const {
	const SwfSceneNode *n = node();
	if (n == nullptr) {
		return std::numeric_limits<float>::quiet_NaN();
	}
	return n->kind == SwfNodeKind::EditText ? n->matrix.tx + field_bounds(*n, fonts_).x_min : n->matrix.tx;
}

float As2Clip::y() const {
	const SwfSceneNode *n = node();
	if (n == nullptr) {
		return std::numeric_limits<float>::quiet_NaN();
	}
	return n->kind == SwfNodeKind::EditText ? n->matrix.ty + field_bounds(*n, fonts_).y_min : n->matrix.ty;
}

void As2Clip::set_x(float p_x) const {
	const SwfSceneNode *n = mut();
	if (n == nullptr || !std::isfinite(p_x)) {
		return;
	}
	SwfMatrix m = n->matrix;
	m.tx = n->kind == SwfNodeKind::EditText ? p_x - field_bounds(*n, fonts_).x_min : p_x;
	note(scene_->set_matrix(path_, m));
}

void As2Clip::set_y(float p_y) const {
	const SwfSceneNode *n = mut();
	if (n == nullptr || !std::isfinite(p_y)) {
		return;
	}
	SwfMatrix m = n->matrix;
	m.ty = n->kind == SwfNodeKind::EditText ? p_y - field_bounds(*n, fonts_).y_min : p_y;
	note(scene_->set_matrix(path_, m));
}

float As2Clip::xscale() const {
	const SwfSceneNode *n = node();
	if (n == nullptr) {
		return std::numeric_limits<float>::quiet_NaN();
	}
	const float s = std::hypot(n->matrix.a, n->matrix.b) * 100.0f;
	return n->matrix.a < 0.0f && n->matrix.b == 0.0f ? -s : s;
}

float As2Clip::yscale() const {
	const SwfSceneNode *n = node();
	if (n == nullptr) {
		return std::numeric_limits<float>::quiet_NaN();
	}
	const float s = std::hypot(n->matrix.c, n->matrix.d) * 100.0f;
	return n->matrix.d < 0.0f && n->matrix.c == 0.0f ? -s : s;
}

void As2Clip::set_xscale(float p_percent) const {
	const SwfSceneNode *n = mut();
	if (n == nullptr || !std::isfinite(p_percent)) {
		return;
	}
	SwfMatrix m = n->matrix;
	const float s = p_percent / 100.0f;
	if (m.b == 0.0f) {
		m.a = s;
	} else {
		const float angle = std::atan2(m.b, m.a);
		m.a = s * std::cos(angle);
		m.b = s * std::sin(angle);
	}
	note(scene_->set_matrix(path_, m));
}

void As2Clip::set_yscale(float p_percent) const {
	const SwfSceneNode *n = mut();
	if (n == nullptr || !std::isfinite(p_percent)) {
		return;
	}
	SwfMatrix m = n->matrix;
	const float s = p_percent / 100.0f;
	if (m.c == 0.0f) {
		m.d = s;
	} else {
		const float angle = std::atan2(-m.c, m.d);
		m.c = -s * std::sin(angle);
		m.d = s * std::cos(angle);
	}
	note(scene_->set_matrix(path_, m));
}

SwfRect As2Clip::local_bounds() const {
	const SwfSceneNode *n = node();
	return n != nullptr ? swf_node_local_bounds(*scene_, *n) : SwfRect{};
}

SwfRect As2Clip::parent_bounds() const {
	const SwfSceneNode *n = node();
	if (n == nullptr) {
		return SwfRect{};
	}
	BoundsAcc acc;
	acc.add(n->kind == SwfNodeKind::EditText ? field_bounds(*n, fonts_) : swf_node_local_bounds(*scene_, *n),
			n->matrix);
	return acc.r;
}

float As2Clip::width() const {
	return parent_bounds().width();
}

float As2Clip::height() const {
	return parent_bounds().height();
}

void As2Clip::set_width(float p_width) const {
	const SwfSceneNode *n = mut();
	if (n == nullptr || !std::isfinite(p_width) || p_width < 0.0f) {
		return;
	}
	if (n->kind == SwfNodeKind::EditText) {
		SwfRect b = swf_node_text_bounds(*n);
		b.x_max = b.x_min + p_width;
		note(scene_->set_text_bounds(path_, b));
		return;
	}
	const float local = swf_node_local_bounds(*scene_, *n).width();
	if (!(local > 0.0f)) {
		return;
	}
	SwfMatrix m = n->matrix;
	if (m.b == 0.0f && m.c == 0.0f) {
		m.a = (m.a < 0.0f ? -1.0f : 1.0f) * p_width / local;
	} else {
		const float current = width();
		if (!(current > 0.0f)) {
			return;
		}
		const float f = p_width / current;
		m.a *= f;
		m.b *= f;
	}
	note(scene_->set_matrix(path_, m));
}

void As2Clip::set_height(float p_height) const {
	const SwfSceneNode *n = mut();
	if (n == nullptr || !std::isfinite(p_height) || p_height < 0.0f) {
		return;
	}
	if (n->kind == SwfNodeKind::EditText) {
		SwfRect b = swf_node_text_bounds(*n);
		b.y_max = b.y_min + p_height;
		note(scene_->set_text_bounds(path_, b));
		return;
	}
	const float local = swf_node_local_bounds(*scene_, *n).height();
	if (!(local > 0.0f)) {
		return;
	}
	SwfMatrix m = n->matrix;
	if (m.b == 0.0f && m.c == 0.0f) {
		m.d = (m.d < 0.0f ? -1.0f : 1.0f) * p_height / local;
	} else {
		const float current = height();
		if (!(current > 0.0f)) {
			return;
		}
		const float f = p_height / current;
		m.c *= f;
		m.d *= f;
	}
	note(scene_->set_matrix(path_, m));
}

float As2Clip::rotation() const {
	const SwfSceneNode *n = node();
	if (n == nullptr) {
		return std::numeric_limits<float>::quiet_NaN();
	}
	return float(std::atan2(double(n->matrix.b), double(n->matrix.a)) * 180.0 / std::numbers::pi);
}

void As2Clip::set_rotation(float p_degrees) const {
	const SwfSceneNode *n = mut();
	if (n == nullptr || !std::isfinite(p_degrees)) {
		return;
	}
	SwfMatrix m = n->matrix;
	const double xs = std::hypot(double(m.a), double(m.b));
	const double ys = std::hypot(double(m.c), double(m.d)) *
			(double(m.a) * double(m.d) - double(m.b) * double(m.c) < 0.0 ? -1.0 : 1.0);
	const double r = std::remainder(double(p_degrees), 360.0) * std::numbers::pi / 180.0;
	m.a = float(xs * std::cos(r));
	m.b = float(xs * std::sin(r));
	m.c = float(-ys * std::sin(r));
	m.d = float(ys * std::cos(r));
	note(scene_->set_matrix(path_, m));
}

float As2Clip::alpha() const {
	const SwfSceneNode *n = node();
	return n != nullptr ? n->color.mul[3] * 100.0f : std::numeric_limits<float>::quiet_NaN();
}

void As2Clip::set_alpha(float p_percent) const {
	if (mut() == nullptr || !std::isfinite(p_percent)) {
		return;
	}
	note(scene_->set_alpha(path_, p_percent / 100.0f));
}

void As2Clip::set_rgb(uint32_t p_rgb) const {
	const SwfSceneNode *n = mut();
	if (n == nullptr) {
		return;
	}
	SwfColorXform c = n->color;
	c.mul[0] = c.mul[1] = c.mul[2] = 0.0f;
	c.add[0] = float((p_rgb >> 16) & 0xFFu);
	c.add[1] = float((p_rgb >> 8) & 0xFFu);
	c.add[2] = float(p_rgb & 0xFFu);
	note(scene_->set_color(path_, c));
}

As2ColorTransform As2Clip::color_transform() const {
	const SwfSceneNode *n = node();
	if (n == nullptr) {
		return {};
	}
	const SwfColorXform &c = n->color;
	return { double(c.mul[0]) * 100.0, double(c.add[0]), double(c.mul[1]) * 100.0, double(c.add[1]),
		double(c.mul[2]) * 100.0, double(c.add[2]), double(c.mul[3]) * 100.0, double(c.add[3]) };
}

void As2Clip::set_color_transform(const As2ColorTransformPatch &p_patch) const {
	const SwfSceneNode *n = mut();
	if (n == nullptr) {
		return;
	}
	SwfColorXform c = n->color;
	const std::optional<double> mul[4] = { p_patch.ra, p_patch.ga, p_patch.ba, p_patch.aa };
	const std::optional<double> add[4] = { p_patch.rb, p_patch.gb, p_patch.bb, p_patch.ab };
	for (int i = 0; i < 4; ++i) {
		if (mul[i] && std::isfinite(*mul[i])) {
			c.mul[size_t(i)] = float(*mul[i] / 100.0);
		}
		if (add[i] && std::isfinite(*add[i])) {
			c.add[size_t(i)] = float(*add[i]);
		}
	}
	note(scene_->set_color(path_, c));
}

bool As2Clip::visible() const {
	const SwfSceneNode *n = node();
	return n != nullptr && n->visible;
}

void As2Clip::set_visible(bool p_visible) const {
	if (mut() == nullptr) {
		return;
	}
	note(scene_->set_visible(path_, p_visible));
}

uint32_t As2Clip::current_frame() const {
	const SwfSceneNode *n = node();
	return n != nullptr && n->timeline != nullptr ? n->frame + 1 : 0;
}

uint32_t As2Clip::total_frames() const {
	const SwfSceneNode *n = node();
	return n != nullptr ? n->total_frames() : 0;
}

void As2Clip::jump(int32_t p_as2_frame, bool p_play) const {
	const SwfSceneNode *n = mut();
	if (n == nullptr || n->timeline == nullptr) {
		return;
	}
	const int32_t f = clamp_frame(p_as2_frame, n->timeline->frames);
	if (f < 0) {
		return;
	}
	note(scene_->goto_frame(path_, uint32_t(f), p_play));
}

void As2Clip::jump(std::string_view p_label, bool p_play) const {
	const SwfSceneNode *n = mut();
	if (n == nullptr || n->timeline == nullptr) {
		return;
	}
	if (!n->timeline->label_frame(p_label)) {
		if (stats_ != nullptr) {
			stats_->missing_labels++;
			stats_->missing_label_names.push_back(path_ + " " + std::string(p_label));
		}
		return;
	}
	note(scene_->goto_label(path_, p_label, p_play));
}

void As2Clip::goto_and_stop(int32_t p_as2_frame) const {
	jump(p_as2_frame, false);
}

void As2Clip::goto_and_play(int32_t p_as2_frame) const {
	jump(p_as2_frame, true);
}

void As2Clip::goto_and_stop(std::string_view p_label) const {
	jump(p_label, false);
}

void As2Clip::goto_and_play(std::string_view p_label) const {
	jump(p_label, true);
}

void As2Clip::stop() const {
	const SwfSceneNode *n = mut();
	if (n != nullptr && n->timeline != nullptr) {
		note(scene_->stop(path_));
	}
}

void As2Clip::play() const {
	const SwfSceneNode *n = mut();
	if (n != nullptr && n->timeline != nullptr) {
		note(scene_->play(path_));
	}
}

std::string As2Clip::text() const {
	const SwfSceneNode *n = node();
	return n != nullptr ? n->text : std::string();
}

void As2Clip::set_text(std::string_view p_text) const {
	const SwfSceneNode *n = mut();
	if (n == nullptr || n->kind != SwfNodeKind::EditText) {
		return;
	}
	if (n->text == p_text && !n->html) {
		return;
	}
	note(scene_->set_text(path_, p_text, false));
}

std::optional<uint32_t> As2Clip::text_color() const {
	const SwfSceneNode *n = node();
	if (n == nullptr || n->kind != SwfNodeKind::EditText || n->edit_text == nullptr || !n->movie) {
		return std::nullopt;
	}
	const SwfColor c = n->text_color ? *n->text_color : swf_edit_text_format(*n->movie, *n->edit_text).color;
	return (uint32_t(c.r) << 16) | (uint32_t(c.g) << 8) | uint32_t(c.b);
}

void As2Clip::set_text_color(uint32_t p_rgb) const {
	const SwfSceneNode *n = mut();
	if (n == nullptr || n->kind != SwfNodeKind::EditText) {
		return;
	}
	note(scene_->set_text_color(path_, SwfColor{ uint8_t((p_rgb >> 16) & 0xFFu), uint8_t((p_rgb >> 8) & 0xFFu), uint8_t(p_rgb & 0xFFu), 255 }));
}

std::optional<float> As2Clip::text_size() const {
	const SwfSceneNode *n = node();
	if (n == nullptr || n->kind != SwfNodeKind::EditText || n->edit_text == nullptr || !n->movie) {
		return std::nullopt;
	}
	return n->text_size ? *n->text_size : swf_edit_text_format(*n->movie, *n->edit_text).size;
}

void As2Clip::set_text_size(float p_size) const {
	const SwfSceneNode *n = mut();
	if (n == nullptr || n->kind != SwfNodeKind::EditText) {
		return;
	}
	note(scene_->set_text_size(path_, p_size));
}

void As2Clip::set_filters(std::vector<SwfFilter> p_filters) const {
	if (mut() == nullptr) {
		return;
	}
	note(scene_->set_filters(path_, std::move(p_filters)));
}

void As2Clip::set_auto_size(bool p_on) const {
	set_auto_size(p_on ? SwfTextAutoSize::Left : SwfTextAutoSize::None);
}

void As2Clip::set_auto_size(SwfTextAutoSize p_mode) const {
	const SwfSceneNode *n = mut();
	if (n == nullptr || n->kind != SwfNodeKind::EditText) {
		return;
	}
	SwfTextOptions o = n->text_options;
	o.auto_size = p_mode;
	note(scene_->set_text_options(path_, o));
}

void As2Clip::set_text_fit(SwfTextFit p_mode) const {
	const SwfSceneNode *n = mut();
	if (n == nullptr || n->kind != SwfNodeKind::EditText) {
		return;
	}
	SwfTextOptions o = n->text_options;
	o.fit = p_mode;
	note(scene_->set_text_options(path_, o));
}

float As2Clip::text_width() const {
	const std::optional<SwfTextLayout> l = layout();
	return l ? l->text_width : 0.0f;
}

float As2Clip::text_height() const {
	const std::optional<SwfTextLayout> l = layout();
	return l ? l->text_height : 0.0f;
}

SwfRect As2Clip::text_layout_bounds() const {
	const std::optional<SwfTextLayout> l = layout();
	return l ? l->bounds : SwfRect{};
}

std::optional<SwfTextLayout> As2Clip::layout() const {
	const SwfSceneNode *n = mut();
	if (n == nullptr || n->kind != SwfNodeKind::EditText || fonts_ == nullptr) {
		if (stats_ != nullptr) {
			stats_->failed_ops++;
		}
		return std::nullopt;
	}
	level::WorkBudget budget(kSwfTextBudgetUnits);
	Result<SwfTextLayout> layout = swf_layout_node_text(*fonts_, *n, budget);
	if (stats_ != nullptr) {
		stats_->text_layouts++;
	}
	if (!layout) {
		if (stats_ != nullptr) {
			stats_->failed_ops++;
		}
		return std::nullopt;
	}
	return std::move(*layout);
}

int32_t As2Clip::depth() const {
	const SwfSceneNode *n = node();
	return n != nullptr ? n->as2_depth() : 0;
}

void As2Clip::swap_depths(const As2Clip &p_other) const {
	if (mut() == nullptr || p_other.mut() == nullptr) {
		return;
	}
	note(scene_->swap_depths(path_, p_other.path()));
}

void As2Clip::swap_depths(int32_t p_as2_depth) const {
	if (mut() == nullptr) {
		return;
	}
	note(scene_->swap_depths(path_, p_as2_depth));
}

int32_t As2Clip::next_highest_depth() const {
	const SwfSceneNode *n = node();
	if (n == nullptr) {
		return 0;
	}
	int32_t best = -1;
	for (const SwfNodeId c : n->children) {
		if (const SwfSceneNode *child = scene_->node(c)) {
			best = std::max(best, child->as2_depth());
		}
	}
	return best + 1;
}

As2Clip As2Clip::attach_movie(std::string_view p_linkage, std::string_view p_name, int32_t p_depth) const {
	if (mut() == nullptr) {
		return As2Clip();
	}
	Result<SwfNodeId> made = scene_->attach_movie(path_, p_linkage, p_name, p_depth);
	if (!made) {
		if (stats_ != nullptr) {
			stats_->failed_ops++;
			stats_->last_failure = made.error().message;
		}
		return As2Clip();
	}
	return child(p_name);
}

As2Clip As2Clip::create_empty_movie_clip(std::string_view p_name, int32_t p_depth) const {
	if (mut() == nullptr) {
		return As2Clip();
	}
	Result<SwfNodeId> made = scene_->create_empty_movie_clip(path_, p_name, p_depth);
	if (!made) {
		if (stats_ != nullptr) {
			stats_->failed_ops++;
			stats_->last_failure = made.error().message;
		}
		return As2Clip();
	}
	return child(p_name);
}

void As2Clip::remove_movie_clip() const {
	if (mut() == nullptr) {
		return;
	}
	note(scene_->remove_movie(path_));
}

void As2Clip::set_mask(const As2Clip &p_mask) const {
	if (mut() == nullptr || p_mask.mut() == nullptr) {
		return;
	}
	note(scene_->set_mask(path_, p_mask.path()));
}

void As2Clip::clear_mask() const {
	if (mut() == nullptr) {
		return;
	}
	note(scene_->clear_mask(path_));
}

void As2Clip::draw(const SwfDrawOp &p_op) const {
	if (mut() == nullptr) {
		return;
	}
	note(scene_->draw(path_, p_op));
}

void As2Clip::clear() const {
	draw(SwfDrawOp::clear());
}

void As2Clip::begin_fill(uint32_t p_rgb, double p_alpha_percent) const {
	draw(SwfDrawOp::begin_fill(swf_draw_fill_color(p_rgb, p_alpha_percent)));
}

void As2Clip::move_to(float p_x, float p_y) const {
	draw(SwfDrawOp::move_to(p_x, p_y));
}

void As2Clip::line_to(float p_x, float p_y) const {
	draw(SwfDrawOp::line_to(p_x, p_y));
}

void As2Clip::curve_to(float p_control_x, float p_control_y, float p_x, float p_y) const {
	draw(SwfDrawOp::curve_to(p_control_x, p_control_y, p_x, p_y));
}

void As2Clip::end_fill() const {
	draw(SwfDrawOp::end_fill());
}

}
