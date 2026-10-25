#pragma once

#include "bf3/core/ui/as2_clip.h"
#include "bf3/core/ui/hud_screen.h"

#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::core::ui {

class ScrollTextTimer {
public:
	using Listener = std::function<void(double p_value)>;

	static constexpr double kBeginDelay = 2.5;
	static constexpr double kScrollForwardTime = 2.0;
	static constexpr double kEndDelay = 1.0;
	static constexpr double kScrollBackTime = 0.5;

	enum class Phase : uint8_t {
		Stopped,
		BeginDelay,
		ScrollForward,
		EndDelay,
		ScrollBack,
	};

	static std::shared_ptr<ScrollTextTimer> instance(const HudScreen &p_screen);

	void register_listener(const void *p_key, Listener p_listener);
	void unregister_listener(const void *p_key);
	void destroy();
	void tick(const void *p_clock, double p_seconds);
	void release_clock(const void *p_clock);

	double value() const { return value_; }
	Phase phase() const { return phase_; }
	bool tweening() const { return tween_.has_value(); }
	size_t listeners() const { return listeners_.size(); }

private:
	struct Tween {
		double from = 0.0;
		double to = 0.0;
		double seconds = 0.0;
		double elapsed = 0.0;
	};

	void set_timeout(Phase p_phase);
	void timeout_fired();
	void update_listeners();
	void start_tween(double p_to, double p_seconds);

	std::vector<std::pair<const void *, Listener>> listeners_;
	double value_ = std::numeric_limits<double>::quiet_NaN();
	Phase phase_ = Phase::BeginDelay;
	double remaining_ = kBeginDelay;
	std::optional<Tween> tween_;
	const void *clock_ = nullptr;
};

class ScrollTextWidget final : public HudWidgetController {
public:
	~ScrollTextWidget() override;

	void on_clip_load(HudWidgetHost &r_host) override;
	void advance(HudWidgetHost &r_host, double p_seconds) override;

	std::string text() const { return txt_.text(); }
	void set_text(std::string_view p_text);
	std::optional<uint32_t> text_color() const { return txt_.text_color(); }
	void set_text_color(uint32_t p_rgb) const { txt_.set_text_color(p_rgb); }
	void set_filters(std::vector<SwfFilter> p_filters) const { txt_.set_filters(std::move(p_filters)); }
	float text_width() const;
	void set_width(float p_width);
	float preferred_width() const;
	void update_align();

	const As2Clip &field() const { return txt_; }
	const As2Clip &mask() const { return mask_; }
	float mask_width() const { return mask_width_; }
	float scroll_distance() const { return scroll_distance_; }
	bool registered() const { return registered_; }
	const std::shared_ptr<ScrollTextTimer> &timer() const { return timer_; }

	std::function<void()> on_load_callback;

private:
	void on_scroll_update(double p_value) const;
	ScrollTextTimer &shared_timer();

	As2Clip self_;
	As2Clip txt_;
	As2Clip mask_;
	const HudScreen *screen_ = nullptr;
	std::shared_ptr<ScrollTextTimer> timer_;
	uint8_t original_align_ = 0;
	float original_this_x_ = 0.0f;
	float original_mask_width_ = 0.0f;
	float mask_width_ = 0.0f;
	float current_text_width_ = 0.0f;
	float scroll_distance_ = 0.0f;
	bool loaded_ = false;
	bool registered_ = false;
};

class As2Interval {
public:
	enum class Handle : uint8_t {
		Undefined,
		Cleared,
		Running,
	};

	void set(double p_seconds);
	void clear();
	uint32_t due(double p_seconds);

	Handle handle() const { return handle_; }

private:
	Handle handle_ = Handle::Undefined;
	double period_ = 0.0;
	double elapsed_ = 0.0;
};

class SimpleScrollTextWidget final : public HudWidgetController {
public:
	static constexpr double kFps = 30.0;
	static constexpr double kScrollSpeed = 50.0;
	static constexpr double kScrollDelay = 3.0;

	void construct(HudWidgetHost &r_host) override;
	void advance(HudWidgetHost &r_host, double p_seconds) override;

	std::string text() const { return txt_.text(); }
	void set_text(std::string_view p_text);
	float width() const { return txt_.width(); }
	float height() const { return txt_.height(); }
	void set_width(float p_width);
	void set_height(float p_height);
	std::optional<uint32_t> text_color() const { return txt_.text_color(); }
	void set_text_color(uint32_t p_rgb) const { txt_.set_text_color(p_rgb); }
	void set_filters(std::vector<SwfFilter> p_filters) const { txt_.set_filters(std::move(p_filters)); }
	float text_width() const;
	void update_align();

	const As2Clip &field() const { return txt_; }
	const As2Clip &mask() const { return mask_; }
	const As2Interval &interval() const { return interval_; }
	float scroll_distance() const { return scroll_distance_; }

private:
	void on_scroll_update();

	As2Clip self_;
	As2Clip txt_;
	As2Clip mask_;
	As2Interval interval_;
	uint8_t original_align_ = 0;
	double delay_count_ = 0.0;
	float default_mask_width_ = 0.0f;
	float mask_width_ = 0.0f;
	float current_text_width_ = 0.0f;
	float scroll_distance_ = 0.0f;
};

class SimpleTextBlockScrollWidget final : public HudWidgetController {
public:
	static constexpr double kFps = 30.0;
	static constexpr double kScrollSpeed = 15.0;
	static constexpr double kScrollDelay = 3.0;

	void construct(HudWidgetHost &r_host) override;
	void advance(HudWidgetHost &r_host, double p_seconds) override;

	std::string text() const { return txt_.text(); }
	void set_text(std::string_view p_text);
	float width() const { return txt_.width(); }
	float height() const { return txt_.height(); }
	void set_width(float p_width);
	void set_height(float p_height);
	std::optional<uint32_t> text_color() const { return txt_.text_color(); }
	void set_text_color(uint32_t p_rgb) const { txt_.set_text_color(p_rgb); }
	void set_filters(std::vector<SwfFilter> p_filters) const { txt_.set_filters(std::move(p_filters)); }
	float text_height() const;
	void update_align();

	const As2Clip &field() const { return txt_; }
	const As2Clip &mask() const { return mask_; }
	const As2Interval &interval() const { return interval_; }
	float scroll_distance() const { return scroll_distance_; }

private:
	void on_scroll_update();

	As2Clip self_;
	As2Clip txt_;
	As2Clip mask_;
	As2Interval interval_;
	double delay_count_ = 0.0;
	float default_mask_height_ = 0.0f;
	float mask_height_ = 0.0f;
	float current_text_height_ = 0.0f;
	float scroll_distance_ = 0.0f;
};

void as2_draw_rect(const As2Clip &p_clip, float p_width, float p_height, uint32_t p_rgb);

}
