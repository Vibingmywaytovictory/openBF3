#include "scroll_text.h"

#include "hud_widget_support.h"

#include <map>
#include <numbers>

namespace bf3::core::ui {

namespace {

double ease_in_out_sine(double p_t, double p_from, double p_change, double p_duration) {
	return -p_change / 2.0 * (std::cos(std::numbers::pi * p_t / p_duration) - 1.0) + p_from;
}

uint8_t field_align(const As2Clip &p_field) {
	const SwfSceneNode *n = p_field.node();
	return n != nullptr && n->edit_text != nullptr ? n->edit_text->align : swf_align::kLeft;
}

bool finite_number(double p_v) {
	return std::isfinite(p_v);
}

}

std::shared_ptr<ScrollTextTimer> ScrollTextTimer::instance(const HudScreen &p_screen) {
	static std::map<const HudScreen *, std::weak_ptr<ScrollTextTimer>> timers;
	std::erase_if(timers, [](const auto &p_entry) { return p_entry.second.expired(); });
	std::weak_ptr<ScrollTextTimer> &slot = timers[&p_screen];
	std::shared_ptr<ScrollTextTimer> t = slot.lock();
	if (!t) {
		t = std::make_shared<ScrollTextTimer>();
		slot = t;
	}
	return t;
}

void ScrollTextTimer::register_listener(const void *p_key, Listener p_listener) {
	for (const auto &[key, fn] : listeners_) {
		if (key == p_key) {
			return;
		}
	}
	listeners_.emplace_back(p_key, p_listener);
	p_listener(value_);
	if (listeners_.size() == 1) {
		set_timeout(Phase::BeginDelay);
	}
}

void ScrollTextTimer::unregister_listener(const void *p_key) {
	for (size_t i = 0; i < listeners_.size(); ++i) {
		if (listeners_[i].first == p_key) {
			listeners_.erase(listeners_.begin() + std::ptrdiff_t(i));
			return;
		}
	}
}

void ScrollTextTimer::destroy() {
	phase_ = Phase::Stopped;
	remaining_ = 0.0;
	tween_.reset();
	listeners_.clear();
}

void ScrollTextTimer::tick(const void *p_clock, double p_seconds) {
	if (!finite_number(p_seconds) || p_seconds < 0.0) {
		return;
	}
	if (clock_ == nullptr) {
		clock_ = p_clock;
	}
	if (p_clock != clock_) {
		return;
	}
	if (tween_) {
		tween_->elapsed += p_seconds;
		const bool done = tween_->elapsed >= tween_->seconds;
		value_ = done ? tween_->to : ease_in_out_sine(tween_->elapsed, tween_->from, tween_->to - tween_->from, tween_->seconds);
		if (done) {
			tween_.reset();
		}
		update_listeners();
	}
	if (phase_ != Phase::Stopped) {
		remaining_ -= p_seconds;
		if (remaining_ <= 0.0) {
			timeout_fired();
		}
	}
}

void ScrollTextTimer::release_clock(const void *p_clock) {
	if (clock_ == p_clock) {
		clock_ = nullptr;
	}
}

void ScrollTextTimer::set_timeout(Phase p_phase) {
	phase_ = p_phase;
	switch (p_phase) {
		case Phase::BeginDelay: remaining_ = kBeginDelay; break;
		case Phase::ScrollForward: remaining_ = kScrollForwardTime; break;
		case Phase::EndDelay: remaining_ = kEndDelay; break;
		case Phase::ScrollBack: remaining_ = kScrollBackTime; break;
		case Phase::Stopped: remaining_ = 0.0; break;
	}
}

void ScrollTextTimer::timeout_fired() {
	switch (phase_) {
		case Phase::BeginDelay:
			value_ = 0.0;
			if (!listeners_.empty()) {
				update_listeners();
				start_tween(1.0, kScrollForwardTime);
			}
			set_timeout(Phase::ScrollForward);
			break;
		case Phase::ScrollForward:
			value_ = 1.0;
			set_timeout(Phase::EndDelay);
			break;
		case Phase::EndDelay:
			value_ = 1.0;
			if (!listeners_.empty()) {
				update_listeners();
				start_tween(0.0, kScrollBackTime);
			}
			set_timeout(Phase::ScrollBack);
			break;
		case Phase::ScrollBack:
			value_ = 0.0;
			update_listeners();
			set_timeout(Phase::BeginDelay);
			break;
		case Phase::Stopped: break;
	}
}

void ScrollTextTimer::update_listeners() {
	const std::vector<std::pair<const void *, Listener>> calls = listeners_;
	for (const auto &[key, fn] : calls) {
		fn(value_);
	}
}

void ScrollTextTimer::start_tween(double p_to, double p_seconds) {
	tween_ = Tween{ value_, p_to, p_seconds, 0.0 };
}

ScrollTextWidget::~ScrollTextWidget() {
	if (timer_) {
		timer_->unregister_listener(this);
		timer_->release_clock(this);
	}
}

void ScrollTextWidget::on_clip_load(HudWidgetHost &r_host) {
	self_ = r_host.self();
	screen_ = &r_host.screen();
	loaded_ = true;
	original_this_x_ = self_.x();
	txt_ = self_.child("txt");
	original_mask_width_ = txt_.width();
	mask_width_ = original_mask_width_;
	mask_ = r_host.create_empty_movie_clip(self_, "mask", self_.next_highest_depth());
	as2_draw_rect(mask_, mask_width_, txt_.height(), 0);
	self_.set_mask(mask_);
	original_align_ = field_align(txt_);
	current_text_width_ = txt_.text_width();
	if (on_load_callback) {
		on_load_callback();
	}
}

void ScrollTextWidget::advance(HudWidgetHost &, double p_seconds) {
	if (timer_) {
		timer_->tick(this, p_seconds);
	}
}

void ScrollTextWidget::set_text(std::string_view p_text) {
	txt_.set_text(p_text);
	update_align();
}

float ScrollTextWidget::text_width() const {
	return std::min(mask_width_, txt_.text_width());
}

void ScrollTextWidget::set_width(float p_width) {
	if (std::isnan(p_width)) {
		return;
	}
	txt_.set_width(p_width);
	mask_.set_width(p_width);
	mask_width_ = p_width;
	update_align();
}

float ScrollTextWidget::preferred_width() const {
	const SwfSceneNode *n = txt_.node();
	if (n == nullptr) {
		return 0.0f;
	}
	const SwfTextAutoSize was = n->text_options.auto_size;
	txt_.set_auto_size(SwfTextAutoSize::Left);
	const float w = txt_.text_layout_bounds().width() * std::fabs(txt_.xscale()) / 100.0f;
	txt_.set_auto_size(was);
	return w;
}

void ScrollTextWidget::update_align() {
	if (!loaded_) {
		return;
	}
	current_text_width_ = txt_.text_width();
	scroll_distance_ = current_text_width_ - mask_width_;
	if (scroll_distance_ > 0.0f) {
		txt_.set_auto_size(SwfTextAutoSize::Left);
		if (!registered_) {
			shared_timer().register_listener(this, [this](double p_value) { on_scroll_update(p_value); });
			registered_ = true;
		}
	} else {
		if (registered_) {
			shared_timer().unregister_listener(this);
			registered_ = false;
		}
		txt_.set_auto_size(SwfTextAutoSize::None);
		txt_.set_width(mask_width_);
		txt_.set_x(0.0f);
	}
	if (original_align_ == swf_align::kRight) {
		self_.set_x(original_this_x_ + original_mask_width_ - mask_width_);
	}
}

void ScrollTextWidget::on_scroll_update(double p_value) const {
	txt_.set_x(float(-(double(scroll_distance_) + 5.0) * p_value));
}

ScrollTextTimer &ScrollTextWidget::shared_timer() {
	if (!timer_) {
		timer_ = ScrollTextTimer::instance(*screen_);
	}
	return *timer_;
}

void As2Interval::set(double p_seconds) {
	handle_ = Handle::Running;
	period_ = p_seconds;
	elapsed_ = 0.0;
}

void As2Interval::clear() {
	handle_ = Handle::Cleared;
	elapsed_ = 0.0;
}

uint32_t As2Interval::due(double p_seconds) {
	if (handle_ != Handle::Running || !finite_number(p_seconds) || p_seconds < 0.0 || !(period_ > 0.0)) {
		return 0;
	}
	elapsed_ += p_seconds;
	const double n = std::floor(elapsed_ / period_);
	elapsed_ -= n * period_;
	return uint32_t(n);
}

void SimpleScrollTextWidget::construct(HudWidgetHost &r_host) {
	self_ = r_host.self();
	delay_count_ = 0.0;
	txt_ = self_.child("txt");
	default_mask_width_ = txt_.width();
	mask_width_ = default_mask_width_;
	mask_ = r_host.create_empty_movie_clip(self_, "mask", self_.next_highest_depth());
	as2_draw_rect(mask_, mask_width_, txt_.height(), 0);
	self_.set_mask(mask_);
	original_align_ = field_align(txt_);
	current_text_width_ = txt_.text_width();
	update_align();
}

void SimpleScrollTextWidget::advance(HudWidgetHost &, double p_seconds) {
	for (uint32_t n = interval_.due(p_seconds); n > 0 && interval_.handle() == As2Interval::Handle::Running; --n) {
		on_scroll_update();
	}
}

void SimpleScrollTextWidget::set_text(std::string_view p_text) {
	txt_.set_text(p_text);
	update_align();
}

void SimpleScrollTextWidget::set_width(float p_width) {
	if (std::isnan(p_width)) {
		return;
	}
	self_.set_xscale(100.0f);
	txt_.set_width(p_width);
	mask_.set_width(p_width);
	mask_width_ = p_width;
	update_align();
}

void SimpleScrollTextWidget::set_height(float p_height) {
	if (std::isnan(p_height)) {
		return;
	}
	self_.set_yscale(100.0f);
	txt_.set_height(p_height);
	mask_.set_height(p_height);
	update_align();
}

float SimpleScrollTextWidget::text_width() const {
	return std::min(mask_width_, txt_.text_width());
}

void SimpleScrollTextWidget::update_align() {
	current_text_width_ = txt_.text_width();
	scroll_distance_ = current_text_width_ - mask_width_;
	if (scroll_distance_ > 0.0f) {
		txt_.set_auto_size(SwfTextAutoSize::Left);
		if (interval_.handle() == As2Interval::Handle::Cleared) {
			interval_.set(kFps * 0.001);
		}
	} else {
		txt_.set_auto_size(SwfTextAutoSize::None);
		txt_.set_width(mask_width_);
		txt_.set_x(0.0f);
		interval_.clear();
	}
	if (original_align_ == swf_align::kRight) {
		self_.set_x(default_mask_width_ - mask_width_);
	}
}

void SimpleScrollTextWidget::on_scroll_update() {
	delay_count_ += kFps * 0.001;
	if (delay_count_ < kScrollDelay) {
		return;
	}
	txt_.set_x(float(double(txt_.x()) - kScrollSpeed * (kFps * 0.001)));
	if (txt_.x() + current_text_width_ < 0.0f) {
		txt_.set_x(0.0f);
		delay_count_ = 0.0;
	}
}

void SimpleTextBlockScrollWidget::construct(HudWidgetHost &r_host) {
	self_ = r_host.self();
	delay_count_ = 0.0;
	txt_ = self_.child("txt");
	default_mask_height_ = txt_.height();
	mask_height_ = default_mask_height_;
	mask_ = r_host.create_empty_movie_clip(self_, "mask", self_.next_highest_depth());
	as2_draw_rect(mask_, txt_.width(), mask_height_, 0);
	self_.set_mask(mask_);
	current_text_height_ = txt_.text_height();
	update_align();
}

void SimpleTextBlockScrollWidget::advance(HudWidgetHost &, double p_seconds) {
	for (uint32_t n = interval_.due(p_seconds); n > 0 && interval_.handle() == As2Interval::Handle::Running; --n) {
		on_scroll_update();
	}
}

void SimpleTextBlockScrollWidget::set_text(std::string_view p_text) {
	txt_.set_text(p_text);
	update_align();
}

void SimpleTextBlockScrollWidget::set_width(float p_width) {
	if (std::isnan(p_width)) {
		return;
	}
	self_.set_xscale(100.0f);
	txt_.set_width(p_width);
	mask_.set_width(p_width);
	update_align();
}

void SimpleTextBlockScrollWidget::set_height(float p_height) {
	if (std::isnan(p_height)) {
		return;
	}
	self_.set_yscale(100.0f);
	txt_.set_height(p_height);
	mask_.set_height(p_height);
	mask_height_ = p_height;
	update_align();
}

float SimpleTextBlockScrollWidget::text_height() const {
	return std::min(mask_height_, txt_.text_height());
}

void SimpleTextBlockScrollWidget::update_align() {
	current_text_height_ = txt_.text_height();
	scroll_distance_ = current_text_height_ - mask_height_;
	if (scroll_distance_ > 0.0f) {
		txt_.set_auto_size(SwfTextAutoSize::Left);
		if (interval_.handle() == As2Interval::Handle::Cleared) {
			interval_.set(kFps * 0.001);
		}
	} else {
		txt_.set_auto_size(SwfTextAutoSize::None);
		txt_.set_height(mask_height_);
		txt_.set_y(0.0f);
		interval_.clear();
	}
}

void SimpleTextBlockScrollWidget::on_scroll_update() {
	delay_count_ += kFps * 0.001;
	if (delay_count_ < kScrollDelay) {
		return;
	}
	txt_.set_y(float(double(txt_.y()) - kScrollSpeed * (kFps * 0.001)));
	if (txt_.y() + current_text_height_ < 0.0f) {
		txt_.set_y(0.0f);
		delay_count_ = 0.0;
	}
}

void as2_draw_rect(const As2Clip &p_clip, float p_width, float p_height, uint32_t p_rgb) {
	p_clip.begin_fill(p_rgb);
	p_clip.move_to(0.0f, 0.0f);
	p_clip.line_to(p_width, 0.0f);
	p_clip.line_to(p_width, p_height);
	p_clip.line_to(0.0f, p_height);
	p_clip.line_to(0.0f, 0.0f);
	p_clip.end_fill();
}

void register_hud_widgets_scroll_text(HudWidgetRegistry &r_registry) {
	r_registry.add("Util.ScrollText", [] { return std::make_unique<ScrollTextWidget>(); });
	r_registry.add("Util.SimpleScrollText", [] { return std::make_unique<SimpleScrollTextWidget>(); });
	r_registry.add("Util.SimpleTextBlockScroll", [] { return std::make_unique<SimpleTextBlockScrollWidget>(); });
}

}
