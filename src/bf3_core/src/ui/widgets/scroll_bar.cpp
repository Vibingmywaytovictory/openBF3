#include "scroll_bar.h"

namespace bf3::core::ui {

namespace {

constexpr std::string_view kPageJumpPerc = "pageJumpPerc";

double acceptable_percentage(double p_percentage) {
	if (p_percentage > 100.0) {
		return 100.0;
	}
	if (p_percentage < 0.0) {
		return 0.0;
	}
	return p_percentage;
}

std::string_view leaf_of(std::string_view p_path) {
	const size_t dot = p_path.rfind('.');
	return dot == std::string_view::npos ? p_path : p_path.substr(dot + 1);
}

}

void ScrollBarClass::construct(HudWidgetHost &r_host) {
	host_ = &r_host;
	setup_size();
}

void ScrollBarClass::on_clip_load(HudWidgetHost &) {
	setup_bg();
}

void ScrollBarClass::initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) {
	setup_refresh_data(*this, r_host, p_init.data);
	init();
}

void ScrollBarClass::init() {
	setup_mouse_actions();
	host_->self().set_visible(true);
}

void ScrollBarClass::setup_size() {
	const As2Clip self = host_->self();
	size_ = as2_round(self.height() > self.width() ? self.height() : self.width());
	self.set_xscale(100.0f);
	self.set_yscale(100.0f);
}

void ScrollBarClass::setup_bg() {
	const As2Clip bg = host_->clip("backGround");
	bg.set_x(8.0f);
	bg.set_y(0.0f);
	bg.set_width(8.0f);
	bg.set_height(float(size_));
	const As2Clip glow = host_->clip("bgGlowMc");
	glow.set_x(bg.x() - 4.0f);
	glow.set_y(bg.y() - 4.0f);
	glow.set_width(bg.width() + 8.0f);
	glow.set_height(bg.height() + 8.0f);
	const As2Clip catcher = host_->clip("mouseCatcher");
	catcher.set_x(0.0f);
	catcher.set_y(-8.0f);
	catcher.set_width(24.0f);
	catcher.set_height(bg.height() + 16.0f);
}

void ScrollBarClass::setup_mouse_actions() {
	for (const std::string_view name : { "whiteBar", "backGround", "mouseCatcher" }) {
		host_->set_mouse_target(host_->clip(name), true);
	}
}

void ScrollBarClass::set_bar_height(double p_height) {
	double h = p_height;
	if (h > size_ - kBarSizeMaxOffset) {
		h = size_ - kBarSizeMaxOffset;
	} else if (h < kBarSizeMin) {
		h = kBarSizeMin;
	}
	const As2Clip bar = host_->clip("whiteBar");
	bar.set_x(8.0f);
	bar.set_y(0.0f);
	bar.set_width(8.0f);
	bar.set_height(float(as2_round(h)));
	const As2Clip glow = host_->clip("barGlowMc");
	if (!glow.exists()) {
		return;
	}
	glow.set_x(bar.x() - 4.0f);
	glow.set_y(bar.y() - 4.0f);
	glow.set_width(bar.width() + 8.0f);
	glow.set_height(bar.height() + 8.0f);
}

double ScrollBarClass::ymouse() const {
	return host_->ymouse(host_->self());
}

bool ScrollBarClass::mouse_press(HudWidgetHost &, const HudMouse &p_mouse) {
	mouse_pressed(leaf_of(p_mouse.target));
	return true;
}

bool ScrollBarClass::mouse_release(HudWidgetHost &, const HudMouse &) {
	bar_offset_ = 0.0;
	stop_enter_frame();
	return true;
}

void ScrollBarClass::mouse_pressed(std::string_view p_target) {
	const As2Clip bar = host_->clip("whiteBar");
	const double y = ymouse();
	const double top = bar.y();
	const double bottom = top + bar.height();
	if (p_target == "whiteBar") {
		bar_offset_ = -((y - top) - bar.height() * 0.5);
		mode_ = Mode::Drag;
		host_->set_enter_frame(true);
	} else if (p_target == "mouseCatcher") {
		if (y > top && y < bottom) {
			bar_offset_ = -((y - top) - bar.height() * 0.5);
			mode_ = Mode::Drag;
			host_->set_enter_frame(true);
		} else if (y < top) {
			scroll_page(-1, true);
		} else if (y > bottom) {
			scroll_page(1, true);
		}
	} else if (p_target == "backGround") {
		if (y < top) {
			scroll_page(-1, true);
		} else if (y > bottom) {
			scroll_page(1, true);
		}
	}
}

void ScrollBarClass::scroll_page(int32_t p_dir, bool p_using_mouse) {
	const double offset = p_dir == 1 ? page_perc_ * 100.0 : -(page_perc_ * 100.0);
	start_page_jump_anim(acceptable_percentage(percentage_ + offset));
	if (p_using_mouse) {
		page_jump_count_ = 0;
		page_jump_frequency_count_ = 0;
		jump_offset_ = offset;
		mode_ = Mode::PageJump;
		host_->set_enter_frame(true);
	}
}

void ScrollBarClass::enter_frame(HudWidgetHost &) {
	if (mode_ == Mode::Drag) {
		const double bar_h = host_->clip("whiteBar").height();
		const double p = acceptable_percentage((((ymouse() - bar_h * 0.5) + bar_offset_) / (size_ - bar_h)) * 100.0);
		if (p != percentage_) {
			percentage_ = p;
			update_bar_position();
			notify();
		}
		return;
	}
	if (mode_ != Mode::PageJump) {
		return;
	}
	if (page_jump_count_ < kPageJumpDelay) {
		page_jump_count_++;
	} else {
		page_jump_frequency_count_++;
		if (page_jump_frequency_count_ >= kPageJumpFrequency) {
			start_page_jump_anim(acceptable_percentage(percentage_ + jump_offset_));
			page_jump_frequency_count_ = 0;
		}
	}
	const As2Clip bar = host_->clip("whiteBar");
	const double y = ymouse();
	if (y > bar.y() && y < bar.y() + bar.height()) {
		stop_enter_frame();
	}
}

void ScrollBarClass::stop_enter_frame() {
	mode_ = Mode::None;
	host_->set_enter_frame(false);
}

void ScrollBarClass::start_page_jump_anim(double p_target) {
	host_->remove_tweens(host_->self());
	const float fps = host_->screen().scene().frame_rate();
	const double seconds = fps > 0.0f ? kPageJumpAnimFrames / double(fps) : 0.0;
	host_->add_custom_tween(kPageJumpPerc, float(p_target), seconds, HudTweenEase::EaseOutQuad);
}

std::optional<float> ScrollBarClass::tween_property(HudWidgetHost &, std::string_view p_name) {
	if (p_name == kPageJumpPerc) {
		return float(percentage_);
	}
	return std::nullopt;
}

void ScrollBarClass::set_tween_property(HudWidgetHost &, std::string_view p_name, float p_value) {
	if (p_name == kPageJumpPerc) {
		set_page_jump_perc(p_value);
	}
}

void ScrollBarClass::set_page_jump_perc(double p_percentage) {
	percentage_ = p_percentage;
	update_bar_position();
	notify();
}

void ScrollBarClass::notify() {
	if (scroll_updated_) {
		scroll_updated_(percentage_);
	}
	host_->fire_event(kGetPercentage, As2Value::of(percentage_));
}

void ScrollBarClass::update_bar_position() {
	const As2Clip bar = host_->clip("whiteBar");
	bar.set_y(float((size_ - bar.height()) * (percentage_ * 0.01)));
	const As2Clip glow = host_->clip("barGlowMc");
	if (glow.exists()) {
		glow.set_y(bar.y() - 4.0f);
	}
}

void ScrollBarClass::set_target_size(double p_size) {
	target_size_ = p_size;
	if (!(size_ < target_size_)) {
		host_->self().set_visible(false);
		return;
	}
	if (target_size_ > size_) {
		const double ratio = size_ / target_size_;
		page_perc_ = size_ / (target_size_ - size_);
		set_bar_height(size_ * ratio);
		host_->self().set_visible(true);
	}
}

void ScrollBarClass::set_percentage(double p_percentage, bool p_fire_event) {
	const double p = acceptable_percentage(p_percentage);
	if (p != percentage_) {
		percentage_ = p;
		if (p_fire_event) {
			notify();
		}
	}
	update_bar_position();
}

void ScrollBarClass::set_size(double p_size) {
	size_ = p_size;
	setup_bg();
	set_target_size(target_size_);
}

bool ScrollBarClass::is_showing() const {
	return host_->self().visible();
}

void ScrollBarClass::in_event(HudWidgetHost &, int32_t p_index, const As2Value &p_param) {
	if (p_index == kSetPercentage) {
		set_percentage(p_param.to_number());
	}
}

void register_hud_widgets_scroll_bar(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Bar.ScrollBar", [] { return std::make_unique<ScrollBarClass>(); });
}

}
