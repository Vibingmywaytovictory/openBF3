#pragma once

#include "bf3/host/level_load.h"

#include <string>

namespace godot {
class CanvasLayer;
class ColorRect;
class Label;
class Node;
class ProgressBar;
}

namespace bf3::godot_side {

class LoadingScreen {
public:
	void attach(godot::Node *p_parent, const std::string &p_title);

	void update(const bf3::host::LoadProgress &p_progress, uint64_t p_now_us, const std::string &p_detail);
	void remove();
	bool shown() const { return layer_ != nullptr; }

	const std::string &stage_text() const { return stage_text_; }
	const std::string &stages_text() const { return stages_text_; }
	uint32_t permille() const { return permille_; }

private:
	godot::CanvasLayer *layer_ = nullptr;
	godot::ColorRect *background_ = nullptr;
	godot::Label *title_ = nullptr;
	godot::Label *stage_ = nullptr;
	godot::ProgressBar *bar_ = nullptr;
	godot::Label *stages_ = nullptr;
	std::string stage_text_;
	std::string stages_text_;
	uint32_t permille_ = 0;
};

const char *cache_warmth_name(bf3::host::CacheWarmth p_warmth);

}
