#include "loading_screen.h"

#include "godot_util.h"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/progress_bar.hpp>

#include <cstdio>

namespace bf3::godot_side {

using namespace godot;
using bf3::host::LoadStage;

const char *cache_warmth_name(bf3::host::CacheWarmth p_warmth) {
	switch (p_warmth) {
		case bf3::host::CacheWarmth::Cold:
			return "cold";
		case bf3::host::CacheWarmth::Partial:
			return "partial";
		case bf3::host::CacheWarmth::Warm:
			return "warm";
		case bf3::host::CacheWarmth::Unknown:
			break;
	}
	return "unknown";
}

namespace {

Label *make_label(int p_size, const Vector2 &p_position) {
	Label *l = memnew(Label);
	l->set_position(p_position);
	l->add_theme_color_override("font_color", Color(0.92f, 0.93f, 0.95f));
	l->add_theme_font_size_override("font_size", p_size);
	return l;
}

}

void LoadingScreen::attach(Node *p_parent, const std::string &p_title) {
	layer_ = memnew(CanvasLayer);
	layer_->set_name("LoadingScreen");
	layer_->set_layer(10);
	background_ = memnew(ColorRect);
	background_->set_color(Color(0.05f, 0.06f, 0.07f, 1.0f));
	background_->set_anchors_preset(Control::PRESET_FULL_RECT);
	layer_->add_child(background_);
	title_ = make_label(26, Vector2(48, 40));
	title_->set_text(to_godot(p_title));
	layer_->add_child(title_);
	stage_ = make_label(18, Vector2(48, 90));
	layer_->add_child(stage_);
	bar_ = memnew(ProgressBar);
	bar_->set_position(Vector2(48, 124));
	bar_->set_size(Vector2(640, 22));
	bar_->set_max(1000.0);
	bar_->set_show_percentage(false);
	layer_->add_child(bar_);
	stages_ = make_label(15, Vector2(48, 164));
	layer_->add_child(stages_);
	p_parent->add_child(layer_);
}

void LoadingScreen::update(const bf3::host::LoadProgress &p_progress, uint64_t p_now_us, const std::string &p_detail) {
	permille_ = p_progress.permille();
	const LoadStage furthest = LoadStage(p_progress.stage.load(std::memory_order_acquire));
	const auto warmth = bf3::host::CacheWarmth(p_progress.warmth.load(std::memory_order_relaxed));
	char line[256];
	std::snprintf(line, sizeof(line), "%s  %.1f %%  (cache %s)", bf3::host::load_stage_name(furthest), double(permille_) / 10.0, cache_warmth_name(warmth));
	stage_text_ = line;
	std::string stages;
	for (size_t i = 0; i < size_t(LoadStage::Done); ++i) {
		const uint64_t start = p_progress.start_us[i].load(std::memory_order_relaxed);
		if (start == 0) {
			continue;
		}
		const uint64_t end = p_progress.end_us[i].load(std::memory_order_acquire);
		const double seconds = double((end != 0 ? end : p_now_us) - start) / 1e6;
		const uint32_t done = p_progress.done[i].load(std::memory_order_relaxed);
		const uint32_t total = p_progress.total[i].load(std::memory_order_relaxed);
		if (total > 0) {
			std::snprintf(line, sizeof(line), "%-10s %6u / %-6u %7.2f s%s\n", bf3::host::load_stage_name(LoadStage(i)), done, total, seconds, end != 0 ? "" : "  ...");
		} else {
			std::snprintf(line, sizeof(line), "%-10s %15s %7.2f s%s\n", bf3::host::load_stage_name(LoadStage(i)), "", seconds, end != 0 ? "" : "  ...");
		}
		stages += line;
	}
	stages_text_ = stages;
	if (layer_ == nullptr) {
		return;
	}
	stage_->set_text(to_godot(stage_text_));
	bar_->set_value(double(permille_));
	stages_->set_text(to_godot(stages_text_ + (p_detail.empty() ? "" : "\n" + p_detail)));
}

void LoadingScreen::remove() {
	if (layer_ != nullptr) {
		layer_->queue_free();
		layer_ = nullptr;
		background_ = nullptr;
		title_ = nullptr;
		stage_ = nullptr;
		bar_ = nullptr;
		stages_ = nullptr;
	}
}

}
