#pragma once

#include "hud_widget_support.h"

#include <functional>

namespace bf3::core::ui {

class ScrollBarClass final : public HudWidgetController {
public:
	static constexpr int32_t kGetPercentage = 0;
	static constexpr int32_t kSetPercentage = 1;
	static constexpr double kBarSizeMin = 10.0;
	static constexpr double kBarSizeMaxOffset = 20.0;
	static constexpr int32_t kPageJumpDelay = 12;
	static constexpr int32_t kPageJumpFrequency = 5;
	static constexpr double kPageJumpAnimFrames = 3.0;

	void construct(HudWidgetHost &r_host) override;
	void on_clip_load(HudWidgetHost &r_host) override;
	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override;
	void in_event(HudWidgetHost &r_host, int32_t p_index, const As2Value &p_param) override;
	bool mouse_press(HudWidgetHost &r_host, const HudMouse &p_mouse) override;
	bool mouse_release(HudWidgetHost &r_host, const HudMouse &p_mouse) override;
	void enter_frame(HudWidgetHost &r_host) override;
	std::optional<float> tween_property(HudWidgetHost &r_host, std::string_view p_name) override;
	void set_tween_property(HudWidgetHost &r_host, std::string_view p_name, float p_value) override;

	void init();
	void set_target_size(double p_size);
	void set_percentage(double p_percentage, bool p_fire_event = true);
	void set_size(double p_size);
	double size() const { return size_; }
	double target_size() const { return target_size_; }
	double percentage() const { return percentage_; }
	bool is_showing() const;
	bool is_scrolling() const { return mode_ != Mode::None; }
	void set_scroll_updated(std::function<void(double)> p_callback) { scroll_updated_ = std::move(p_callback); }

private:
	enum class Mode : uint8_t {
		None,
		Drag,
		PageJump,
	};

	void setup_size();
	void setup_bg();
	void setup_mouse_actions();
	void set_bar_height(double p_height);
	void mouse_pressed(std::string_view p_target);
	void scroll_page(int32_t p_dir, bool p_using_mouse);
	void start_page_jump_anim(double p_target);
	void update_bar_position();
	void set_page_jump_perc(double p_percentage);
	void stop_enter_frame();
	void notify();
	double ymouse() const;

	HudWidgetHost *host_ = nullptr;
	double size_ = 0.0;
	double target_size_ = 0.0;
	double page_perc_ = 0.0;
	double percentage_ = 0.0;
	double bar_offset_ = 0.0;
	double jump_offset_ = 0.0;
	int32_t page_jump_count_ = 0;
	int32_t page_jump_frequency_count_ = 0;
	Mode mode_ = Mode::None;
	std::function<void(double)> scroll_updated_;
};

}
