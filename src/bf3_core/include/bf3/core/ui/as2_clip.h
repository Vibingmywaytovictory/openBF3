#pragma once

#include "bf3/core/ui/swf_scene.h"
#include "bf3/core/ui/swf_text.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

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
	float alpha() const;
	void set_alpha(float p_percent) const;
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
	void set_auto_size(bool p_on) const;
	float text_width() const;

	int32_t next_highest_depth() const;
	As2Clip attach_movie(std::string_view p_linkage, std::string_view p_name, int32_t p_depth) const;
	void remove_movie_clip() const;

private:
	SwfSceneNode *mut() const;
	void note(const Status &p_status) const;
	void miss() const;
	void jump(int32_t p_as2_frame, bool p_play) const;
	void jump(std::string_view p_label, bool p_play) const;

	SwfScene *scene_ = nullptr;
	std::string path_;
	As2Stats *stats_ = nullptr;
	const SwfFontSet *fonts_ = nullptr;
};

SwfRect swf_node_local_bounds(const SwfScene &p_scene, const SwfSceneNode &p_node);

std::string as2_number_string(double p_value);

}
