#include "list.h"
#include "scroll_text.h"

#include <cmath>
#include <limits>

namespace bf3::core::ui {

namespace {

constexpr uint32_t kStandardColor = 0xFFFFFFu;
constexpr uint32_t kInvertedColor = 0x000000u;
constexpr uint32_t kInactiveColor = 0x000000u;
constexpr uint32_t kStandardGlowColor = 7586815u;
constexpr uint32_t kInvertedGlowColor = 0x000000u;
constexpr uint32_t kInactiveGlowColor = 0x000000u;
constexpr float kGlowBlur = 8.0f;
constexpr float kStandardGlowStrength = 1.0f;
constexpr float kInvertedGlowStrength = 0.75f;
constexpr int32_t kGlowQuality = 1;
constexpr float kStandardAlpha = 100.0f;
constexpr float kInvertedAlpha = 100.0f;
constexpr float kInactiveAlpha = 60.0f;
constexpr uint32_t kMaskColor = 0xFF0000u;
constexpr float kScrollPad = 5.0f;

class ListLabelClass final : public HudWidgetController, public RowStateListener, public HudListLabel {
public:
	~ListLabelClass() override {
		if (timer_ != nullptr) {
			timer_->unregister_listener(this);
			timer_->release_clock(this);
		}
	}

	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		if (const As2Value *t = r_host.construct_param("TEXT")) {
			text_ = t->to_string();
		}
		if (r_host.construct_param("scrollable") != nullptr) {
			scrollable_ = param_true(r_host, "scrollable");
		}
		if (r_host.construct_param("interactive") != nullptr) {
			interactive_ = param_true(r_host, "interactive");
		}
		set_label();
		const As2Clip parent = r_host.self().parent();
		if (RowBaseClass *row = dynamic_cast<RowBaseClass *>(r_host.screen().controller(parent.path()))) {
			row->register_state_callback(r_host.path());
		}
	}

	void advance(HudWidgetHost &, double p_seconds) override {
		if (timer_ != nullptr) {
			timer_->tick(this, p_seconds);
		}
	}

	void row_state(const std::string &p_state) override {
		if (!interactive_) {
			return;
		}
		if (p_state == "out" || p_state == "over" || p_state == "press") {
			state_ = "standard";
		} else if (p_state == "inactive") {
			state_ = "inactive";
		} else {
			state_ = "inverted";
		}
		set_text_format();
		set_text_glow_filter();
	}

	void set_text(std::string p_text) override {
		text_ = std::move(p_text);
		set_label();
	}

	void direct_access(HudWidgetHost &, const UiDirectAccessApply &p_apply) override {
		if (p_apply.op == UiDirectAccessOp::Text) {
			set_text(p_apply.value.to_string());
		}
	}

	const std::string &text() const override { return text_; }

	void set_label_width(float p_width) override {
		width_ = p_width;
		field().set_width(width_);
	}

	float label_width() const override { return width_; }

	void set_type(std::string_view p_type) override {
		host_->self().goto_and_stop(std::string_view(to_lower_ascii(p_type)));
		set_label_width(width_);
	}

	std::string type() const override {
		switch (host_->self().current_frame()) {
			case 1:
				return "medium";
			case 2:
				return "bold";
			default:
				return "undefined";
		}
	}

	float text_width() const override { return field().text_width(); }

	void set_scrollable(bool p_scrollable) override { scrollable_ = p_scrollable; }

	void set_interactive(bool p_interactive) override { interactive_ = p_interactive; }

	const std::string &label_state() const override { return state_; }

	void set_state_style(std::string_view p_state, uint32_t p_color, const SwfFilter &p_glow) override {
		if (p_state == "standard") {
			standard_ = Style{ p_color, p_glow };
		} else if (p_state == "inactive") {
			inactive_ = Style{ p_color, p_glow };
		}
		set_text_format();
		set_text_glow_filter();
	}

	void set_text_size(float p_size) override {
		text_size_ = p_size;
		set_text_format();
	}

	void set_text_align(std::string p_align) override {
		text_align_ = std::move(p_align);
		set_text_format();
	}

private:
	struct Style {
		uint32_t color = 0;
		SwfFilter glow;
	};

	As2Clip field() const { return host_->clip("txtField"); }

	void set_label() {
		const As2Clip txt = field();
		txt.set_text(text_);
		txt.set_width(width_);
		set_text_format();
		set_text_glow_filter();
		check_scrolling();
	}

	void check_scrolling() {
		const As2Clip txt = field();
		txt.set_x(0.0f);
		if (!scrollable_) {
			return;
		}
		const float w = txt.width();
		const float tw = txt.text_width();
		if (tw > w) {
			if (mask_.empty()) {
				const As2Clip self = host_->self();
				const As2Clip mask = host_->create_empty_movie_clip(self, "mask", self.next_highest_depth());
				as2_draw_rect(mask, w, txt.height(), kMaskColor);
				self.set_mask(mask);
				mask_ = mask.path();
			}
			txt.set_width(tw + kScrollPad);
			scroll_distance_ = tw - w;
			if (!registered_) {
				if (timer_ == nullptr) {
					timer_ = ScrollTextTimer::instance(host_->screen());
				}
				timer_->register_listener(this, [this](double p_value) {
					field().set_x(float(-(double(scroll_distance_) + kScrollPad) * p_value));
				});
				registered_ = true;
			}
		} else if (registered_) {
			timer_->unregister_listener(this);
			registered_ = false;
		}
	}

	void set_text_format() const {
		const As2Clip txt = field();
		if (!txt.exists()) {
			return;
		}
		txt.set_alpha(100.0f);
		if (text_size_) {
			txt.set_text_size(*text_size_);
		}
		if (text_align_) {
			txt.set_text_align(*text_align_);
		}
		if (state_ == "standard") {
			txt.set_text_color(standard_ ? standard_->color : kStandardColor);
			txt.set_alpha(kStandardAlpha);
		} else if (state_ == "inverted") {
			txt.set_text_color(kInvertedColor);
			txt.set_alpha(kInvertedAlpha);
		} else if (state_ == "inactive") {
			txt.set_text_color(inactive_ ? inactive_->color : kInactiveColor);
			txt.set_alpha(kInactiveAlpha);
		}
		txt.set_height(txt.text_height());
	}

	void set_text_glow_filter() const {
		const As2Clip txt = field();
		if (!txt.exists()) {
			return;
		}
		if (state_ == "standard") {
			txt.set_filters({ standard_ ? standard_->glow
										: as2_glow_filter(kStandardGlowColor, 1.0, kGlowBlur, kGlowBlur,
												  kStandardGlowStrength, kGlowQuality) });
		} else if (state_ == "inverted") {
			txt.set_filters({ as2_glow_filter(
					kInvertedGlowColor, 1.0, kGlowBlur, kGlowBlur, kInvertedGlowStrength, kGlowQuality) });
		} else if (state_ == "inactive") {
			txt.set_filters({ inactive_ ? inactive_->glow
										: as2_glow_filter(kInactiveGlowColor, 1.0, kGlowBlur, kGlowBlur,
												  kInvertedGlowStrength, kGlowQuality) });
		}
	}

	HudWidgetHost *host_ = nullptr;
	std::shared_ptr<ScrollTextTimer> timer_;
	std::string text_ = "LABEL";
	std::string state_ = "standard";
	std::string mask_;
	std::optional<Style> standard_;
	std::optional<Style> inactive_;
	std::optional<float> text_size_;
	std::optional<std::string> text_align_;
	float width_ = std::numeric_limits<float>::quiet_NaN();
	float scroll_distance_ = 0.0f;
	bool interactive_ = true;
	bool scrollable_ = false;
	bool registered_ = false;
};

}

void register_hud_widgets_list_label(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.List.ListLabel", [] { return std::make_unique<ListLabelClass>(); });
}

}
