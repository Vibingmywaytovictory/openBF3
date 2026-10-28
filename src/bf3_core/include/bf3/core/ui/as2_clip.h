#pragma once

#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/swf_text.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

struct As2ColorTransform {
	double ra = 100.0;
	double rb = 0.0;
	double ga = 100.0;
	double gb = 0.0;
	double ba = 100.0;
	double bb = 0.0;
	double aa = 100.0;
	double ab = 0.0;
};

struct As2ColorTransformPatch {
	std::optional<double> ra;
	std::optional<double> rb;
	std::optional<double> ga;
	std::optional<double> gb;
	std::optional<double> ba;
	std::optional<double> bb;
	std::optional<double> aa;
	std::optional<double> ab;
};

struct As2Stats {
	uint32_t missing_paths = 0;
	uint32_t missing_labels = 0;
	uint32_t failed_ops = 0;
	uint32_t text_layouts = 0;
	std::string last_failure;
	std::vector<std::string> missing_label_names;
};

class As2Clip {
public:
	As2Clip() = default;
	As2Clip(SwfScene *p_scene, std::string p_path, As2Stats *p_stats = nullptr, const SwfFontSet *p_fonts = nullptr);

	As2Clip child(std::string_view p_name) const;
	As2Clip parent() const;
	const std::string &path() const { return path_; }
	const SwfSceneNode *node() const;
	bool exists() const { return node() != nullptr; }
	SwfScene *scene() const { return scene_; }

	float x() const;
	float y() const;
	void set_x(float p_x) const;
	void set_y(float p_y) const;
	float xscale() const;
	float yscale() const;
	void set_xscale(float p_percent) const;
	void set_yscale(float p_percent) const;
	SwfRect local_bounds() const;
	SwfRect parent_bounds() const;
	float width() const;
	float height() const;
	void set_width(float p_width) const;
	void set_height(float p_height) const;
	float rotation() const;
	void set_rotation(float p_degrees) const;
	float alpha() const;
	void set_alpha(float p_percent) const;
	void set_rgb(uint32_t p_rgb) const;
	As2ColorTransform color_transform() const;
	void set_color_transform(const As2ColorTransformPatch &p_patch) const;
	bool visible() const;
	void set_visible(bool p_visible) const;

	uint32_t current_frame() const;
	uint32_t total_frames() const;
	void goto_and_stop(int32_t p_as2_frame) const;
	void goto_and_play(int32_t p_as2_frame) const;
	void goto_and_stop(std::string_view p_label) const;
	void goto_and_play(std::string_view p_label) const;
	void stop() const;
	void play() const;

	std::string text() const;
	void set_text(std::string_view p_text) const;
	std::optional<uint32_t> text_color() const;
	void set_text_color(uint32_t p_rgb) const;
	std::optional<float> text_size() const;
	void set_text_size(float p_size) const;
	void set_text_align(std::string_view p_align) const;
	void set_filters(std::vector<SwfFilter> p_filters) const;
	void set_auto_size(bool p_on) const;
	void set_auto_size(SwfTextAutoSize p_mode) const;
	void set_text_fit(SwfTextFit p_mode) const;
	float text_width() const;
	float text_height() const;
	SwfRect text_layout_bounds() const;

	int32_t depth() const;
	int32_t next_highest_depth() const;
	void swap_depths(const As2Clip &p_other) const;
	void swap_depths(int32_t p_as2_depth) const;
	As2Clip attach_movie(std::string_view p_linkage, std::string_view p_name, int32_t p_depth) const;
	As2Clip create_empty_movie_clip(std::string_view p_name, int32_t p_depth) const;
	void remove_movie_clip() const;
	void set_mask(const As2Clip &p_mask) const;
	void clear_mask() const;
	void clear() const;
	void begin_fill(uint32_t p_rgb, double p_alpha_percent = 100.0) const;
	void move_to(float p_x, float p_y) const;
	void line_to(float p_x, float p_y) const;
	void curve_to(float p_control_x, float p_control_y, float p_x, float p_y) const;
	void end_fill() const;

private:
	SwfSceneNode *mut() const;
	std::optional<SwfTextLayout> layout() const;
	void note(const Status &p_status) const;
	void miss() const;
	void jump(int32_t p_as2_frame, bool p_play) const;
	void jump(std::string_view p_label, bool p_play) const;
	void draw(const SwfDrawOp &p_op) const;

	SwfScene *scene_ = nullptr;
	std::string path_;
	As2Stats *stats_ = nullptr;
	const SwfFontSet *fonts_ = nullptr;
};

SwfRect swf_node_local_bounds(const SwfScene &p_scene, const SwfSceneNode &p_node);

std::string as2_number_string(double p_value);

}
