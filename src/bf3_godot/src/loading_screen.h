#pragma once
// The M1 loading screen (docs/design/m1.md §11.7). Main thread only.
//
// A CanvasLayer over the level with the stage name, a progress bar from LoadProgress::permille() (stages weighted by
// their share of the M1 budget, level_load.cpp), one line per stage with its items and times, the cache warmth
// (cold, partial or warm) and a free-form detail line (the upload counters). It reads only the LoadProgress atomics
// and the strings it is given. The session removes it at LoadStage::Done (the first frame with the own soldier
// spawned and a valid presentation, after the upload).

#include "bf3/host/level_load.h"

#include <string>

namespace godot {
class CanvasLayer;
class ColorRect;
class Label;
class Node;
class ProgressBar;
} // namespace godot

namespace bf3::godot_side {

class LoadingScreen {
public:
	void attach(godot::Node *p_parent, const std::string &p_title);
	// Refreshes the texts from p_progress; p_now_us on LoadProgress's clock (steady_clock microseconds) for the
	// running stages' times.
	void update(const bf3::host::LoadProgress &p_progress, uint64_t p_now_us, const std::string &p_detail);
	void remove(); // queue_free; idempotent
	bool shown() const { return layer_ != nullptr; }

	// The texts of the last update (the checks print them).
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

// "cold" / "partial" / "warm" / "unknown".
const char *cache_warmth_name(bf3::host::CacheWarmth p_warmth);

} // namespace bf3::godot_side
