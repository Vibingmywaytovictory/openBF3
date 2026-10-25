#include "button.h"

namespace bf3::core::ui {

namespace {

constexpr int32_t kPressed = 0;
constexpr int32_t kOver = 1;
constexpr int32_t kOut = 2;
constexpr int32_t kReleased = 3;
constexpr int32_t kToggledOn = 4;
constexpr int32_t kToggledOff = 5;
constexpr int32_t kNavigateUp = 6;
constexpr int32_t kNavigateDown = 7;
constexpr int32_t kNavigateLeft = 8;
constexpr int32_t kNavigateRight = 9;
constexpr int32_t kShow = 10;
constexpr int32_t kHide = 11;
constexpr int32_t kToggle = 12;
constexpr int32_t kToggleOn = 13;
constexpr int32_t kToggleOff = 14;
constexpr int32_t kFocus = 15;

constexpr int32_t kConceptNavigateUp = 0;
constexpr int32_t kConceptNavigateDown = 1;
constexpr int32_t kConceptNavigateLeft = 2;
constexpr int32_t kConceptNavigateRight = 3;
constexpr int32_t kConceptActivate = 6;

constexpr uint32_t kPremiumText = 15302420;

enum class ButtonState : uint8_t {
	Normal,
	Over,
	Press,
	Highlight,
	OverSelected,
	PressSelected,
	Selected,
	Inactive,
};

std::string_view state_clip(ButtonState p_state) {
	switch (p_state) {
		case ButtonState::Normal:
			return "normal_mc";
		case ButtonState::Over:
			return "over_mc";
		case ButtonState::Press:
			return "press_mc";
		case ButtonState::Highlight:
			return "highlight_mc";
		case ButtonState::OverSelected:
			return "overSelected_mc";
		case ButtonState::PressSelected:
			return "pressSelected_mc";
		case ButtonState::Selected:
			return "selected_mc";
		case ButtonState::Inactive:
			return "inactive_mc";
	}
	return "normal_mc";
}

ButtonState selected_variant(ButtonState p_state) {
	switch (p_state) {
		case ButtonState::Normal:
			return ButtonState::Selected;
		case ButtonState::Over:
			return ButtonState::OverSelected;
		case ButtonState::Press:
			return ButtonState::PressSelected;
		default:
			return p_state;
	}
}

const std::string &item_from(const std::vector<std::string> &p_items, int32_t p_index) {
	static const std::string kEmpty;
	if (p_items.empty()) {
		return kEmpty;
	}
	if (p_index < 0) {
		return p_items.front();
	}
	if (size_t(p_index) >= p_items.size() - 1) {
		return p_items.back();
	}
	return p_items[size_t(p_index)];
}

enum class ButtonKind : uint8_t {
	Base,
	Resizable,
	Text,
	Icon,
	LargeArrow,
	TextIcon,
	TextIconAlignedRight,
};

constexpr float kAlignedRightPadding = 26.0f;
constexpr float kTextResizeOffset = 20.0f;

class ButtonTypeClass final : public HudWidgetController {
public:
	explicit ButtonTypeClass(ButtonKind p_kind) : kind_(p_kind) {}

	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		r_host.self().set_visible(false);
		ignore_height_ = kind_ != ButtonKind::Resizable && kind_ != ButtonKind::LargeArrow;
		align_ = has_label() ? "center" : "";
	}

	bool has_label() const { return kind_ == ButtonKind::Text || text_icon(); }
	bool has_icon() const { return kind_ == ButtonKind::Icon || kind_ == ButtonKind::LargeArrow || text_icon(); }
	bool text_icon() const { return kind_ == ButtonKind::TextIcon || kind_ == ButtonKind::TextIconAlignedRight; }
	bool selected() const { return selected_; }
	bool highlighted() const { return highlighted_; }
	float start_width() const { return start_w_; }

	void init() { host_->self().set_visible(true); }

	void set_size(float p_width, float p_height) {
		start_w_ = p_width;
		start_h_ = p_height;
		const As2Clip self = host_->self();
		self.set_xscale(100.0f);
		self.set_yscale(100.0f);
		setup_bg();
	}

	void set_state(ButtonState p_state) {
		const As2Clip bg = host_->clip("bgHolder");
		const auto hide = [&bg](ButtonState p_hidden) {
			const As2Clip c = bg.child(state_clip(p_hidden));
			if (c.exists()) {
				c.set_visible(false);
			}
		};
		for (const ButtonState s : { ButtonState::Normal, ButtonState::Over, ButtonState::Press }) {
			hide(s);
		}
		if (!highlighted_) {
			hide(ButtonState::Highlight);
		}
		for (const ButtonState s : { ButtonState::OverSelected, ButtonState::PressSelected, ButtonState::Selected,
					 ButtonState::Inactive }) {
			hide(s);
		}
		ButtonState state = p_state;
		if (highlighted_ && state == ButtonState::Normal) {
			state = ButtonState::Highlight;
		}
		if (selected_) {
			state = selected_variant(state);
		}
		current_bg_ = bg.child(state_clip(state));
		current_bg_.set_visible(true);
		start_playing_current_bg();
		if (kind_ == ButtonKind::Base || kind_ == ButtonKind::Resizable) {
			return;
		}
		const ButtonState derived = selected_ ? selected_variant(p_state) : p_state;
		if (kind_ == ButtonKind::Icon) {
			icon_state(derived);
		} else if (kind_ == ButtonKind::LargeArrow) {
			large_arrow_state(derived);
		} else {
			const bool inverted = derived == ButtonState::OverSelected || derived == ButtonState::PressSelected ||
					derived == ButtonState::Selected || (text_icon() && derived == ButtonState::Highlight);
			label_state(inverted);
		}
		current_bg_.set_visible(true);
		start_playing_current_bg();
	}

	void set_selected(bool p_selected) {
		selected_ = p_selected;
		set_state(selected_ ? ButtonState::Selected : ButtonState::Normal);
	}

	void set_highlighted(bool p_highlighted) {
		highlighted_ = p_highlighted;
		set_state(highlighted_ ? ButtonState::Highlight : ButtonState::Normal);
	}

	void set_text(const std::string &p_text) {
		text_ = p_text;
		const std::string shown = host_->translate(p_text);
		host_->clip("label.txt").set_text(shown);
		host_->clip("labelInvert.txt").set_text(shown);
		if (kind_ == ButtonKind::Text && p_text == "ID_M_PREMIUM") {
			host_->clip("label.txt").set_text_color(kPremiumText);
			host_->clip("label.txt").set_filters({ as2_glow_filter(0u, 1.0, 5.0f, 5.0f, 1.0f, 1) });
		}
		set_text_field_width();
	}

	void set_align(std::string p_align) { align_ = std::move(p_align); }
	void set_resize_if_text_too_wide(bool p_resize) { resize_ = p_resize; }

	void set_text_align(const std::string &p_align) {
		host_->clip("label").goto_and_stop(std::string_view(p_align));
		host_->clip("labelInvert").goto_and_stop(std::string_view(p_align));
	}

	void set_data(const std::string &p_type, const std::string &p_frame) {
		const As2Clip holder = host_->clip("iconHolder");
		holder.goto_and_stop(std::string_view(p_type));
		const As2Clip icon = holder.child("icon_mc");
		const As2Clip inverted = holder.child("icon_mc_inverted");
		const As2Value frame = As2Value::of(p_frame);
		const double n = frame.to_number();
		for (const As2Clip &c : { icon, inverted }) {
			if (!c.exists()) {
				continue;
			}
			if (std::isfinite(n) && !p_frame.empty()) {
				c.goto_and_stop(int32_t(n));
			} else {
				c.goto_and_stop(std::string_view(p_frame));
			}
		}
		set_state(ButtonState::Normal);
	}

private:
	void setup_bg() {
		const As2Clip self = host_->self();
		self.set_yscale(100.0f);
		const As2Clip bg = host_->clip("bgHolder");
		bg.set_xscale(100.0f);
		bg.set_y(-1.0f);
		bg.set_x(-1.0f);
		bg.set_width(start_w_ + 2.0f);
		if (kind_ == ButtonKind::LargeArrow) {
			bg.child("mouseCatcher_mc").set_width(bg.child("normal_mc").width());
			host_->clip("iconHolder").set_x(start_w_ * 0.5f);
			if (bg.child("outline").exists()) {
				bg.child("outline").set_width(start_w_);
			}
		}
		if (!ignore_height_) {
			bg.set_height(start_h_ + 2.0f);
			if (kind_ == ButtonKind::LargeArrow) {
				host_->clip("iconHolder").set_y(start_h_ * 0.5f);
				if (bg.child("outline").exists()) {
					bg.child("outline").set_height(start_h_);
				}
			}
		}
		set_state(ButtonState::Normal);
	}

	void start_playing_current_bg() {
		if (current_bg_.total_frames() > 1) {
			current_bg_.goto_and_play(0);
		}
	}

	void icon_state(ButtonState p_state) {
		bool normal = false;
		bool inverted = false;
		float scale = 100.0f;
		switch (p_state) {
			case ButtonState::PressSelected:
				scale = 90.0f;
				[[fallthrough]];
			case ButtonState::Selected:
			case ButtonState::Highlight:
			case ButtonState::OverSelected:
				inverted = true;
				break;
			case ButtonState::Press:
				scale = 90.0f;
				[[fallthrough]];
			default:
				normal = true;
				break;
		}
		const As2Clip holder = host_->clip("iconHolder");
		holder.child("icon_mc").set_visible(normal);
		holder.child("icon_mc_inverted").set_visible(inverted);
		holder.set_xscale(scale);
		holder.set_yscale(scale);
	}

	void large_arrow_state(ButtonState p_state) {
		bool normal = false;
		bool inverted = false;
		float scale = 100.0f;
		switch (p_state) {
			case ButtonState::PressSelected:
			case ButtonState::Press:
				scale = 90.0f;
				[[fallthrough]];
			case ButtonState::Highlight:
			case ButtonState::OverSelected:
			case ButtonState::Selected:
				inverted = true;
				break;
			default:
				normal = true;
				break;
		}
		const As2Clip holder = host_->clip("iconHolder");
		holder.child("icon_mc").set_visible(normal);
		holder.child("icon_mc_inverted").set_visible(inverted);
		holder.set_xscale(scale);
	}

	void label_state(bool p_inverted) {
		if (text_icon()) {
			host_->clip("iconHolder.icon_mc").set_visible(!p_inverted);
			host_->clip("iconHolder.icon_mc_inverted").set_visible(p_inverted);
		}
		host_->clip("label").set_visible(!p_inverted);
		host_->clip("labelInvert").set_visible(p_inverted);
	}

public:
	void set_text_field_width() {
		const As2Clip self = host_->self();
		const As2Clip label = host_->clip("label");
		const bool aligned_right = kind_ == ButtonKind::TextIconAlignedRight;
		const float offset = kind_ == ButtonKind::TextIcon ? label.x() : 0.0f;
		const float start_width = start_w_;
		const float start_pos = self.x();
		const float height = text_icon() ? start_h_ : host_->clip("bgHolder").height();
		if (resize_) {
			const float extra = aligned_right ? kTextResizeOffset + kAlignedRightPadding : offset + kTextResizeOffset;
			const float wanted = float(as2_round(double(host_->clip("label.txt").text_width() + extra)));
			if (wanted > start_width) {
				if (align_ == "right") {
					self.set_x(start_pos - wanted + start_width);
				} else if (align_ == "center") {
					self.set_x(start_pos - (wanted - start_width) / 2.0f);
				}
				set_size(wanted, height);
			} else {
				set_size(start_width, height);
				self.set_x(start_pos);
			}
		}
		const float text_width = aligned_right ? start_w_ - kAlignedRightPadding : start_w_ - offset;
		for (const std::string_view name : { "label", "labelInvert" }) {
			const As2Clip holder = host_->clip(name);
			const As2Clip txt = holder.child("txt");
			txt.set_width(text_width);
			txt.set_x(0.0f);
			holder.set_x(offset);
		}
		if (aligned_right) {
			host_->clip("iconHolder").set_x(start_w_ - 13.0f);
		}
	}

private:
	ButtonKind kind_;
	HudWidgetHost *host_ = nullptr;
	float start_w_ = 0.0f;
	float start_h_ = 0.0f;
	bool ignore_height_ = true;
	bool selected_ = false;
	bool highlighted_ = false;
	bool resize_ = true;
	std::string align_;
	std::string text_;
	As2Clip current_bg_;
};

class ButtonClass final : public HudWidgetController, public HudButton {
public:
	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		r_host.self().set_visible(false);
		button_type_ = param_or(r_host, "buttonType", "LargeTextButton");
		button_text_ = param_or(r_host, "buttonText", "Button");
		icon_type_ = param_or(r_host, "iconType", "none");
		button_icon_frame_ = param_or(r_host, "buttonIconFrame", "");
		resize_if_too_wide_ = param_or(r_host, "resizeIfTextFieldIsToWide", "false") == "true";
		align_text_button_ = param_or(r_host, "alignTextButton", "left");
		align_text_ = param_or(r_host, "alignText", "left");
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		const As2Clip self = r_host.self();
		self.set_x(float(as2_round(self.x())));
		self.set_y(float(as2_round(self.y())));
		start_w_ = float(as2_round(self.width()));
		start_h_ = float(as2_round(self.height()));
		self.set_yscale(100.0f);
		self.set_xscale(100.0f);
		r_host.clip("tempBg").set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		if (const std::string *h = p_init.property("HideOnConsole")) {
			hide_on_console_ = *h == "true";
		}
		add_button_type(r_host);
		const std::string *text_data = p_init.property("TextData");
		update_texts(r_host, as2_split(text_data != nullptr && !text_data->empty() ? *text_data : button_text_, ";"));
		const std::string *icon_data = p_init.property("IconData");
		update_icons(
				r_host, as2_split(icon_data != nullptr && !icon_data->empty() ? *icon_data : button_icon_frame_, ";"));
		const std::string *toggled = p_init.property("Toggled");
		update_toggle(r_host, toggled != nullptr && *toggled == "true" ? 1 : 0, false);
		const As2Value *visible = p_init.value("Visible");
		r_host.self().set_visible(visible == nullptr || !visible->defined());
		const std::string &platform = r_host.client().platform;
		if (hide_on_console_ && platform != "win32") {
			r_host.self().set_visible(false);
		}
		if (platform == "win32" || platform.empty()) {
			mouse_enabled_ = true;
			r_host.set_mouse_target(r_host.self(), active_);
		}
		setup_refresh_data(*this, r_host, p_init.data);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "ButtonItem") {
			const As2Value &label = p_value.field("Label");
			if (label.defined()) {
				update_texts(r_host, { label.to_string() });
			}
			const As2Value &visible = p_value.field("Visible");
			if (visible.defined()) {
				r_host.self().set_visible(
						!(visible.kind == As2Value::Kind::Bool ? !visible.boolean : visible.to_string() == "false"));
			}
			if (hide_on_console_ && r_host.client().platform != "win32") {
				r_host.self().set_visible(false);
			}
		} else if (p_key == "Visible" && p_value.defined()) {
			r_host.self().set_visible(p_value.truthy());
		}
	}

	bool mouse_move(HudWidgetHost &r_host, const HudMouse &p_mouse) override {
		if (p_mouse.target != r_host.path()) {
			return false;
		}
		if (p_mouse.kind == HudMouseKind::RollOver) {
			set_state(r_host, ButtonState::Over);
			call_or_fire(callbacks_.over, kOver);
			return true;
		}
		if (p_mouse.kind == HudMouseKind::RollOut) {
			set_state(r_host, ButtonState::Normal);
			call_or_fire(callbacks_.out, kOut);
			return true;
		}
		return false;
	}

	bool mouse_press(HudWidgetHost &r_host, const HudMouse &p_mouse) override {
		if (p_mouse.target != r_host.path()) {
			return false;
		}
		press();
		return true;
	}

	void press() override {
		set_state(*host_, ButtonState::Press);
		call_or_fire(callbacks_.press, kPressed);
	}

	bool mouse_release(HudWidgetHost &r_host, const HudMouse &p_mouse) override {
		if (p_mouse.target != r_host.path()) {
			return false;
		}
		if (p_mouse.kind == HudMouseKind::ReleaseOutside) {
			set_state(r_host, ButtonState::Normal);
			return true;
		}
		button_released(r_host);
		return true;
	}

	bool input_concept(HudWidgetHost &r_host, int32_t p_concept, bool p_pressed) override {
		if (!p_pressed) {
			return false;
		}
		switch (p_concept) {
			case kConceptNavigateUp:
				r_host.fire_event(kNavigateUp, As2Value::of(0));
				return true;
			case kConceptNavigateDown:
				r_host.fire_event(kNavigateDown, As2Value::of(0));
				return true;
			case kConceptNavigateLeft:
				r_host.fire_event(kNavigateLeft, As2Value::of(0));
				return true;
			case kConceptNavigateRight:
				r_host.fire_event(kNavigateRight, As2Value::of(0));
				return true;
			case kConceptActivate:
				button_released(r_host);
				return true;
			default:
				return false;
		}
	}

	void in_event(HudWidgetHost &r_host, int32_t p_index, const As2Value &) override {
		switch (p_index) {
			case kShow:
				r_host.self().set_visible(true);
				break;
			case kHide:
				r_host.self().set_visible(false);
				break;
			case kToggle:
				update_toggle(r_host, toggle_index_ + 1, true);
				break;
			case kToggleOn:
				update_toggle(r_host, 1, false);
				break;
			case kToggleOff:
				update_toggle(r_host, 0, false);
				break;
			case kFocus:
				r_host.request_focus();
				break;
			default:
				break;
		}
	}

	void focus_gained(HudWidgetHost &) override { set_selected(true); }

	void focus_lost(HudWidgetHost &) override { set_selected(false); }

	int32_t toggle_index() const { return toggle_index_; }
	const std::string &current_label() const override { return current_label_; }

	void set_button_type(std::string p_type) override { button_type_ = std::move(p_type); }
	void set_button_text(std::string p_text) override { button_text_ = std::move(p_text); }
	void set_align_text(std::string p_align) override { align_text_ = std::move(p_align); }
	void set_resize_if_too_wide(bool p_resize) override { resize_if_too_wide_ = p_resize; }
	void set_callbacks(HudButtonCallbacks p_callbacks) override { callbacks_ = std::move(p_callbacks); }
	void set_send_event_with_function(bool p_send) override { send_event_with_function_ = p_send; }

	float button_width() override {
		ButtonTypeClass *b = button(*host_);
		return b != nullptr ? b->start_width() : 0.0f;
	}

	void set_button_width(float p_width) override {
		if (ButtonTypeClass *b = button(*host_)) {
			b->set_size(p_width, start_h_);
			if (b->has_label()) {
				b->set_text_field_width();
			}
		}
		align_plus();
	}

	void set_selected(bool p_selected) override {
		selected_ = p_selected;
		if (ButtonTypeClass *b = button(*host_)) {
			b->set_selected(selected_);
		}
	}

	bool selected() override {
		ButtonTypeClass *b = button(*host_);
		return b != nullptr && b->selected();
	}

	void set_highlighted(bool p_highlighted) override {
		highlighted_ = p_highlighted;
		if (ButtonTypeClass *b = button(*host_)) {
			b->set_highlighted(highlighted_);
		}
	}

	void set_active(bool p_active) override {
		active_ = p_active;
		host_->set_mouse_target(host_->self(), active_ && mouse_enabled_);
		if (active_) {
			set_highlighted(highlighted_);
		} else {
			set_state(*host_, ButtonState::Inactive);
		}
	}

	void set_plus() override {
		if (!plus_path_.empty()) {
			return;
		}
		const As2Clip self = host_->self();
		plus_path_ = host_->attach_movie(self, "PlusSign", "plusSign", self.next_highest_depth()).path();
		align_plus();
	}

	void remove_plus() override {
		if (plus_path_.empty()) {
			return;
		}
		host_->remove_movie_clip(host_->screen().clip(plus_path_));
		plus_path_.clear();
	}

private:
	void call_or_fire(const HudButtonCallback &p_callback, int32_t p_event) {
		if (p_callback) {
			p_callback(std::string(clip_leaf(host_->path())));
			if (send_event_with_function_) {
				host_->fire_event(p_event, As2Value::of(current_label_));
			}
			return;
		}
		host_->fire_event(p_event, As2Value::of(current_label_));
	}

	void align_plus() {
		if (plus_path_.empty()) {
			return;
		}
		const As2Clip plus = host_->screen().clip(plus_path_);
		const float width = resize_if_too_wide_ ? host_->self().width() : start_w_;
		plus.set_x(std::floor(width - plus.width() - 1.0f));
	}

	static std::string param_or(const HudWidgetHost &p_host, std::string_view p_name, std::string_view p_default) {
		const As2Value *v = p_host.construct_param(p_name);
		return v != nullptr && v->defined() ? v->to_string() : std::string(p_default);
	}

	ButtonTypeClass *button(HudWidgetHost &r_host) {
		return button_path_.empty() ? nullptr
									: dynamic_cast<ButtonTypeClass *>(r_host.screen().controller(button_path_));
	}

	void add_button_type(HudWidgetHost &r_host) {
		const As2Clip self = r_host.self();
		const As2Clip made = r_host.attach_movie(self, button_type_, "button", self.next_highest_depth());
		button_path_ = made.path();
		if (ButtonTypeClass *b = button(r_host)) {
			b->set_size(start_w_, start_h_);
			b->init();
			if (selected_) {
				b->set_selected(true);
			}
			if (highlighted_) {
				b->set_highlighted(true);
			}
		}
	}

	void set_state(HudWidgetHost &r_host, ButtonState p_state) {
		if (ButtonTypeClass *b = button(r_host)) {
			b->set_state(p_state);
		}
	}

	void button_released(HudWidgetHost &r_host) {
		if (toggle_length_ > 1) {
			update_toggle(r_host, toggle_index_ + 1, true);
		}
		set_state(r_host, ButtonState::Over);
		if (!callbacks_.release && !callbacks_.press) {
			r_host.fire_query(kSoundSelect, As2Value::of(false));
		}
		call_or_fire(callbacks_.release, kReleased);
	}

	void update_texts(HudWidgetHost &r_host, std::vector<std::string> p_texts) {
		texts_ = std::move(p_texts);
		toggle_length_ = std::max(toggle_length_, int32_t(texts_.size()));
		update_text(r_host, item_from(texts_, toggle_index_));
	}

	void update_text(HudWidgetHost &r_host, const std::string &p_text) {
		ButtonTypeClass *b = button(r_host);
		if (b == nullptr || !b->has_label()) {
			return;
		}
		b->set_resize_if_text_too_wide(resize_if_too_wide_);
		b->set_align(align_text_button_);
		b->set_text_align(align_text_);
		b->set_text(p_text);
		current_label_ = p_text;
	}

	void update_icons(HudWidgetHost &r_host, std::vector<std::string> p_icons) {
		icon_frames_ = std::move(p_icons);
		toggle_length_ = std::max(toggle_length_, int32_t(icon_frames_.size()));
		update_icon(r_host, item_from(icon_frames_, toggle_index_));
	}

	void update_icon(HudWidgetHost &r_host, const std::string &p_frame) {
		ButtonTypeClass *b = button(r_host);
		if (b == nullptr || !b->has_icon()) {
			return;
		}
		b->set_data(icon_type_ != "none" ? icon_type_ : "kitIcon", p_frame.empty() ? button_icon_frame_ : p_frame);
	}

	void update_toggle(HudWidgetHost &r_host, int32_t p_index, bool p_fire) {
		toggle_index_ = p_index;
		if (!(toggle_index_ < toggle_length_) || toggle_index_ < 0) {
			toggle_index_ = 0;
		}
		if (ButtonTypeClass *b = button(r_host)) {
			if (b->has_label()) {
				b->set_text(item_from(texts_, toggle_index_));
			}
			if (b->has_icon()) {
				b->set_data(icon_type_ != "none" ? icon_type_ : "kitIcon", item_from(icon_frames_, toggle_index_));
			}
		}
		if (!p_fire) {
			return;
		}
		if (toggle_index_ == 1) {
			r_host.fire_event(kToggledOn, As2Value::of(current_label_));
		} else if (toggle_index_ == 0) {
			r_host.fire_event(kToggledOff, As2Value::of(current_label_));
		}
	}

	HudWidgetHost *host_ = nullptr;
	HudButtonCallbacks callbacks_;
	bool send_event_with_function_ = false;
	bool active_ = true;
	bool mouse_enabled_ = false;
	std::string plus_path_;
	std::string button_type_;
	std::string button_text_;
	std::string icon_type_;
	std::string button_icon_frame_;
	bool resize_if_too_wide_ = false;
	std::string align_text_button_;
	std::string align_text_;
	bool hide_on_console_ = false;
	bool selected_ = false;
	bool highlighted_ = false;
	float start_w_ = 0.0f;
	float start_h_ = 0.0f;
	std::string button_path_;
	std::vector<std::string> texts_;
	std::vector<std::string> icon_frames_;
	int32_t toggle_length_ = 1;
	int32_t toggle_index_ = 0;
	std::string current_label_;
};

}

void register_hud_widgets_button(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Button.Button", [] { return std::make_unique<ButtonClass>(); });
	r_registry.add(
			"Widget.Button.Buttons.ButtonBase", [] { return std::make_unique<ButtonTypeClass>(ButtonKind::Base); });
	r_registry.add("Widget.Button.Buttons.ResizableButton",
			[] { return std::make_unique<ButtonTypeClass>(ButtonKind::Resizable); });
	r_registry.add(
			"Widget.Button.Buttons.TextButton", [] { return std::make_unique<ButtonTypeClass>(ButtonKind::Text); });
	r_registry.add(
			"Widget.Button.Buttons.IconButton", [] { return std::make_unique<ButtonTypeClass>(ButtonKind::Icon); });
	r_registry.add("Widget.Button.Buttons.LargeArrowButton",
			[] { return std::make_unique<ButtonTypeClass>(ButtonKind::LargeArrow); });
	r_registry.add("Widget.Button.Buttons.TextIconButton",
			[] { return std::make_unique<ButtonTypeClass>(ButtonKind::TextIcon); });
	r_registry.add("Widget.Button.Buttons.TextIconButtonAlignedRight",
			[] { return std::make_unique<ButtonTypeClass>(ButtonKind::TextIconAlignedRight); });
}

}
