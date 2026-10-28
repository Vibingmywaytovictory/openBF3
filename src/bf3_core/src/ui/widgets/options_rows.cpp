#include "list.h"

#include "bf3/core/ui/options_model.h"
#include "bf3/core/ui/ui_graph.h"

namespace bf3::core::ui {

namespace {

constexpr std::string_view kSettingsCategory = "uisettingscomp";
constexpr int32_t kKeyBindingsListKey = 922801620;
constexpr double kSelectorScrollSteps = 2.0 * 30.0;
constexpr double kSelectorScrollWaitSeconds = 3.0;
constexpr uint32_t kSelectorActiveColor = 0xFFFFFFu;
constexpr uint32_t kSelectorHighlightedColor = 0x000000u;
constexpr float kSelectorDisabledAlpha = 60.0f;
constexpr size_t kKeyLabels = 4;

class OptionRowLabel {
protected:
	As2Clip attach_label(HudWidgetHost &r_host) {
		const As2Clip dummy = r_host.clip("dummyTextField");
		if (!dummy.exists()) {
			return As2Clip();
		}
		dummy.set_visible(false);
		const As2Clip self = r_host.self();
		const As2Clip label = r_host.attach_movie(self, "mc_label", "Label", self.next_highest_depth());
		label_path_ = label.path();
		if (HudListLabel *l = controller_at<HudListLabel>(r_host, label)) {
			if (const std::optional<float> size = dummy.text_size()) {
				l->set_text_size(*size);
			}
			label.set_x(dummy.x());
			label.set_y(dummy.y());
			l->set_interactive(true);
			l->set_type("bold");
		}
		return label;
	}

	void set_label_text(HudWidgetHost &r_host, const std::string &p_text) {
		label_text_ = p_text;
		if (HudListLabel *l = label_controller(r_host)) {
			l->set_text(r_host.translate(p_text));
		}
	}

	HudListLabel *label_controller(HudWidgetHost &r_host) const {
		return label_path_.empty() ? nullptr : dynamic_cast<HudListLabel *>(r_host.screen().controller(label_path_));
	}

	As2Clip label_clip(HudWidgetHost &r_host) const {
		return label_path_.empty() ? As2Clip() : r_host.screen().clip(label_path_);
	}

	void remove_label(HudWidgetHost &r_host) {
		if (!label_path_.empty()) {
			r_host.remove_movie_clip(label_clip(r_host));
			label_path_.clear();
		}
	}

	std::string label_path_;
	std::string label_text_;
};

class TextSelectorRowClass final : public RowBaseClass, public OptionRowLabel {
public:
	void construct(HudWidgetHost &r_host) override {
		RowBaseClass::construct(r_host);
		attach_label(r_host);
		const As2Clip self = r_host.self();
		for (const std::string_view name : { "hit", "leftArrow", "rightArrow" }) {
			const As2Clip c = r_host.clip(name);
			if (c.exists()) {
				c.swap_depths(self.next_highest_depth());
			}
		}
		register_state_callback(r_host.path());
	}

	void set_data(const As2Value &p_data) override {
		set_label_text(*host_, p_data.field("Label").to_string());
		set_mouse_enabled(true);
		should_scroll_ = false;
		sub_items_ = p_data.field("SubItems");
		const As2Value highlighted = p_data.field("Highlighted");
		RowBaseClass::set_data(p_data);
		if (highlighted.kind == As2Value::Kind::Number) {
			set_selected_index(highlighted.number);
			update_selector_text();
		} else if (highlighted.kind == As2Value::Kind::String) {
			for (size_t i = 0; i < sub_items_.size(); ++i) {
				if (as2_loose_equals(sub_items_.at(i).field("Index"), highlighted)) {
					set_selected_index(double(i));
					update_selector_text();
					break;
				}
			}
		}
		const As2Value &width = p_data.field("SelectorWidth");
		if (width.defined() && !std::isnan(width.to_number()) && width.to_number() != 0.0) {
			selector_width_ = width.to_number();
			set_width(host_->clip("hit").width());
			update_selector_text();
		}
	}

	As2Value selector_value() const override {
		const As2Value &index = sub_items_.at(size_t(selected_index())).field("Index");
		return index.defined() ? index : As2Value::of(selected_index());
	}

	void step_selection(double p_dir, const As2Value &p_used_mouse) override {
		set_selected_index(selected_index() + p_dir);
		if (!(selected_index() < double(sub_items_.size()))) {
			set_selected_index(0.0);
		} else if (selected_index() < 0.0) {
			set_selected_index(double(sub_items_.size()) - 1.0);
		}
		update_selector_text();
		animate_buttons(p_dir);
		selector_changed(p_used_mouse);
	}

	void set_width(double p_width) override {
		const As2Clip right = host_->clip("rightArrow");
		const As2Clip left = host_->clip("leftArrow");
		const As2Clip holder = host_->clip("stHolder");
		const double half = right.width() / 2.0;
		right.set_x(float(p_width - half - 4.0));
		holder.child("tsMask").set_width(float(selector_width_));
		holder.set_x(float(right.x() - half - holder.child("tsMask").width() + 10.0));
		left.set_x(float(holder.x() - half + 10.0));
		if (HudListLabel *l = label_controller(*host_)) {
			l->set_label_width(float(left.x() - half - label_clip(*host_).x() + 5.0));
		}
		host_->clip("hit").set_width(float(p_width));
		RowBaseClass::set_width(p_width);
	}

	As2Value label() const override { return As2Value::of(label_text_); }

	void reset(bool p_destroy) override {
		scroll_phase_ = ScrollPhase::Idle;
		host_->set_enter_frame(false);
		clear_scroll_interval();
		selector_label().set_x(0.0f);
		RowBaseClass::reset(p_destroy);
		if (p_destroy) {
			remove_label(*host_);
		}
	}

	bool mouse_move(HudWidgetHost &, const HudMouse &p_mouse) override {
		if (!mouse_listening() || !is_target(p_mouse.target)) {
			return false;
		}
		if (p_mouse.kind == HudMouseKind::RollOver) {
			mouse_over();
		} else if (p_mouse.kind == HudMouseKind::RollOut) {
			mouse_out();
		}
		return true;
	}

	bool mouse_press(HudWidgetHost &, const HudMouse &p_mouse) override {
		if (!mouse_listening() || !is_target(p_mouse.target)) {
			return false;
		}
		mouse_pressed();
		return true;
	}

	bool mouse_release(HudWidgetHost &r_host, const HudMouse &p_mouse) override {
		if (!mouse_listening() || !is_target(p_mouse.target)) {
			return false;
		}
		if (p_mouse.kind == HudMouseKind::ReleaseOutside) {
			mouse_release_outside();
			return true;
		}
		if (p_mouse.target == r_host.clip("leftArrow").path()) {
			arrow_released(-1.0);
		} else if (p_mouse.target == r_host.clip("rightArrow").path()) {
			arrow_released(1.0);
		} else {
			mouse_released();
		}
		return true;
	}

	void enter_frame(HudWidgetHost &r_host) override {
		if (scroll_phase_ != ScrollPhase::Scrolling) {
			r_host.set_enter_frame(false);
			return;
		}
		scroll_count_ += 1.0;
		if (!should_scroll_) {
			scroll_phase_ = ScrollPhase::Idle;
			r_host.set_enter_frame(false);
			return;
		}
		if (scroll_count_ > kSelectorScrollSteps) {
			scroll_count_ = 0.0;
			r_host.set_enter_frame(false);
			wait_then(ScrollPhase::Restart);
			return;
		}
		selector_label().set_x(float(-(step_size_ * scroll_count_)));
	}

	void interval(HudWidgetHost &r_host, uint32_t p_id) override {
		if (p_id != scroll_interval_) {
			return;
		}
		clear_scroll_interval();
		if (scroll_phase_ == ScrollPhase::WaitBeforeScroll) {
			scroll_phase_ = ScrollPhase::Scrolling;
			scroll_count_ = 0.0;
			r_host.set_enter_frame(true);
		} else if (scroll_phase_ == ScrollPhase::Restart) {
			start_scrolling_text();
		}
	}

protected:
	void on_state(const std::string &) override {
		uint32_t color = kSelectorActiveColor;
		float alpha = 100.0f;
		std::string_view frame = "active";
		if (highlighted()) {
			color = kSelectorHighlightedColor;
			frame = "inactive";
		} else if (disabled()) {
			alpha = kSelectorDisabledAlpha;
			color = kSelectorHighlightedColor;
			frame = "inactive";
		}
		const As2Clip label = selector_label();
		label.set_text_color(color);
		label.set_alpha(alpha);
		for (const std::string_view name : { "leftArrow", "rightArrow" }) {
			const As2Clip arrow = host_->clip(name);
			arrow.set_alpha(alpha);
			arrow.goto_and_stop(frame);
		}
	}

	void setup_mouse_interaction() override {
		if (mouse_listening()) {
			return;
		}
		for (const std::string_view name : { "leftArrow", "rightArrow", "hit" }) {
			host_->set_mouse_target(host_->clip(name), true);
		}
		set_mouse_listening(true);
	}

	void turn_off_mouse_interaction() override {
		if (!mouse_listening()) {
			return;
		}
		for (const std::string_view name : { "leftArrow", "rightArrow", "hit" }) {
			host_->set_mouse_target(host_->clip(name), false);
		}
		set_mouse_listening(false);
	}

private:
	enum class ScrollPhase : uint8_t { Idle, WaitBeforeScroll, Scrolling, Restart };

	As2Clip selector_label() const { return host_->clip("stHolder.selectorLabel"); }

	bool is_target(const std::string &p_target) const {
		for (const std::string_view name : { "leftArrow", "rightArrow", "hit" }) {
			if (p_target == host_->clip(name).path()) {
				return true;
			}
		}
		return false;
	}

	double selected_index() const { return std::isnan(selected_) ? 0.0 : selected_; }

	void set_selected_index(double p_index) { selected_ = std::isnan(p_index) ? 0.0 : p_index; }

	void update_selector_text() {
		scroll_phase_ = ScrollPhase::Idle;
		host_->set_enter_frame(false);
		clear_scroll_interval();
		const As2Clip label = selector_label();
		label.set_x(0.0f);
		label.set_text(host_->translate(sub_items_.at(size_t(selected_index())).field("Label").to_string()));
		label.set_width(label.text_width() + 10.0f);
		if (label.width() > selector_width_) {
			should_scroll_ = true;
			start_scrolling_text();
		} else {
			should_scroll_ = false;
			label.set_width(float(selector_width_));
		}
	}

	void start_scrolling_text() {
		const As2Clip label = selector_label();
		step_size_ = (label.width() - selector_width_) / kSelectorScrollSteps;
		label.set_x(0.0f);
		wait_then(ScrollPhase::WaitBeforeScroll);
	}

	void wait_then(ScrollPhase p_phase) {
		clear_scroll_interval();
		scroll_phase_ = p_phase;
		scroll_interval_ = host_->set_interval(kSelectorScrollWaitSeconds);
	}

	void clear_scroll_interval() {
		if (scroll_interval_ != 0) {
			host_->clear_interval(scroll_interval_);
			scroll_interval_ = 0;
		}
	}

	void arrow_released(double p_dir) {
		step_selection(p_dir, As2Value::of(true));
		mouse_released();
		animate_buttons(p_dir);
	}

	void animate_buttons(double p_dir) const {
		host_->clip(p_dir == -1.0 ? "leftArrow.holder" : "rightArrow.holder").goto_and_play("press");
	}

	As2Value sub_items_ = As2Value::array();
	double selected_ = 0.0;
	double selector_width_ = 200.0;
	double step_size_ = 0.0;
	double scroll_count_ = 0.0;
	bool should_scroll_ = false;
	ScrollPhase scroll_phase_ = ScrollPhase::Idle;
	uint32_t scroll_interval_ = 0;
};

class BarSelectorRowClass final : public RowBaseClass, public OptionRowLabel {
public:
	void construct(HudWidgetHost &r_host) override {
		RowBaseClass::construct(r_host);
		attach_label(r_host);
	}

	void set_data(const As2Value &p_data) override {
		set_label_text(*host_, p_data.field("Label").to_string());
		set_mouse_enabled(true);
		check_states(p_data);
		As2Value data = p_data;
		if (!data.field("From").defined()) {
			data.set("From", As2Value::of(0));
			data.set("To", As2Value::of(1));
			data.set("StepSize", As2Value::of(0.1));
			data.set("Value", As2Value::of(0.0));
		}
		RowBaseClass::set_data(data);
		value_ = number_or(data.field("Value"), 0.0);
		draw_bar();
	}

	As2Value selector_value() const override { return As2Value::of(value_); }

	void step_selection(double p_dir, const As2Value &) override {
		set_value(value_ + p_dir * step_size());
		if (value_ != prev_value_) {
			selector_changed(As2Value::of(value_));
		}
	}

	void set_width(double p_width) override {
		const As2Clip bar = host_->clip("bar");
		bar.set_x(float(p_width - bar.width() - 30.0));
		if (HudListLabel *l = label_controller(*host_)) {
			l->set_label_width(float(bar.x() - label_clip(*host_).x()));
		}
		host_->clip("hit").set_width(float(p_width));
		RowBaseClass::set_width(p_width);
	}

	As2Value label() const override { return As2Value::of(label_text_); }

	void reset(bool p_destroy) override {
		dragging_ = false;
		host_->set_enter_frame(false);
		RowBaseClass::reset(p_destroy);
		if (p_destroy) {
			remove_label(*host_);
		}
	}

	bool mouse_move(HudWidgetHost &r_host, const HudMouse &p_mouse) override {
		if (!mouse_listening()) {
			return false;
		}
		const bool bar = p_mouse.target == r_host.clip("bar.hit").path();
		const bool hit = p_mouse.target == r_host.clip("hit").path();
		if (!bar && !hit) {
			return false;
		}
		if (p_mouse.kind == HudMouseKind::RollOver) {
			mouse_over();
		} else if (p_mouse.kind == HudMouseKind::RollOut && hit) {
			mouse_out();
		}
		return true;
	}

	bool mouse_press(HudWidgetHost &r_host, const HudMouse &p_mouse) override {
		if (!mouse_listening()) {
			return false;
		}
		if (p_mouse.target == r_host.clip("bar.hit").path()) {
			mouse_pressed();
			dragging_ = true;
			r_host.set_enter_frame(true);
			return true;
		}
		if (p_mouse.target == r_host.clip("hit").path()) {
			mouse_pressed();
			return true;
		}
		return false;
	}

	bool mouse_release(HudWidgetHost &r_host, const HudMouse &p_mouse) override {
		if (!mouse_listening()) {
			return false;
		}
		const bool bar = p_mouse.target == r_host.clip("bar.hit").path();
		if (!bar && p_mouse.target != r_host.clip("hit").path()) {
			return false;
		}
		if (bar) {
			dragging_ = false;
			r_host.set_enter_frame(false);
		}
		if (p_mouse.kind == HudMouseKind::ReleaseOutside) {
			mouse_release_outside();
		} else {
			mouse_released();
		}
		return true;
	}

	void enter_frame(HudWidgetHost &r_host) override {
		if (!dragging_) {
			r_host.set_enter_frame(false);
			return;
		}
		const As2Clip hit = r_host.clip("bar.hit");
		const double ratio = double(r_host.xmouse(hit)) / double(hit.width());
		set_value((to() - from()) * ratio);
		if (value_ != prev_value_) {
			selector_changed(As2Value::of(value_));
		}
	}

protected:
	void setup_mouse_interaction() override {
		if (mouse_listening()) {
			return;
		}
		host_->set_mouse_target(host_->clip("bar.hit"), true);
		host_->set_mouse_target(host_->clip("hit"), true);
		set_mouse_listening(true);
	}

	void turn_off_mouse_interaction() override {
		if (!mouse_listening()) {
			return;
		}
		host_->set_mouse_target(host_->clip("bar.hit"), false);
		host_->set_mouse_target(host_->clip("hit"), false);
		set_mouse_listening(false);
	}

private:
	static double number_or(const As2Value &p_v, double p_default) {
		const double n = p_v.to_number();
		return std::isnan(n) ? p_default : n;
	}

	double step_size() const { return number_or(data().field("StepSize"), 1.0); }
	double from() const { return number_or(data().field("From"), 0.0); }
	double to() const { return number_or(data().field("To"), 100.0); }

	void set_value(double p_value) {
		prev_value_ = value_;
		double v = std::isnan(p_value) ? 0.0 : p_value;
		v = std::max(std::min(v, to()), from());
		value_ = v;
		draw_bar();
	}

	void draw_bar() const {
		const double ratio = std::max(0.0, std::min(1.0, (value_ - from()) / (to() - from())));
		host_->clip("bar.innerBar").set_width(float((host_->clip("bar.border").width() - 8.0) * ratio));
	}

	double value_ = 0.0;
	double prev_value_ = 0.0;
	bool dragging_ = false;
};

class KeyBindingRowClass final : public RowBaseClass {
public:
	void construct(HudWidgetHost &r_host) override {
		RowBaseClass::construct(r_host);
		column_handler(0.0);
		swap_text_fields(r_host);
		register_state_callback(r_host.path());
	}

	void set_data(const As2Value &p_data) override {
		if (as2_loose_equals(p_data.field("useDirectAccess"), As2Value::of(true))) {
			for (size_t i = 0; i < kKeyLabels; ++i) {
				set_label(i, "-");
			}
			setup_direct_access();
		} else {
			set_label(0, p_data.field("Label").to_string());
			set_label(1, p_data.field("Key").to_string());
			set_label(2, p_data.field("Mouse").to_string());
			set_label(3, p_data.field("Joystick").to_string());
			column_handler(p_data.field("Column").to_number());
		}
		set_mouse_enabled(true);
		RowBaseClass::set_data(p_data);
	}

	As2Value selector_value() const override { return As2Value::of(column_); }

	void step_selection(double p_dir, const As2Value &) override {
		column_ += p_dir;
		if (column_ < 0.0) {
			column_ = double(kKeyColumnCount - 1);
		} else if (column_ > double(kKeyColumnCount - 1)) {
			column_ = 0.0;
		}
		selector_changed(As2Value::of(column_));
	}

	As2Value label() const override {
		const HudListLabel *l = label_controller(0);
		return As2Value::of(l != nullptr ? l->text() : std::string());
	}

	void reset(bool p_destroy) override {
		RowBaseClass::reset(p_destroy);
		if (p_destroy) {
			for (size_t i = 0; i < kKeyLabels; ++i) {
				host_->remove_movie_clip(host_->clip("Label_" + std::to_string(i)));
			}
		}
	}

	bool mouse_move(HudWidgetHost &r_host, const HudMouse &p_mouse) override {
		if (!mouse_listening() || !column_of(r_host, p_mouse.target)) {
			return false;
		}
		if (p_mouse.kind == HudMouseKind::RollOver) {
			mouse_over();
		} else if (p_mouse.kind == HudMouseKind::RollOut) {
			mouse_out();
		}
		return true;
	}

	bool mouse_press(HudWidgetHost &r_host, const HudMouse &p_mouse) override {
		if (!mouse_listening() || !column_of(r_host, p_mouse.target)) {
			return false;
		}
		mouse_pressed();
		return true;
	}

	bool mouse_release(HudWidgetHost &r_host, const HudMouse &p_mouse) override {
		const std::optional<size_t> column = mouse_listening() ? column_of(r_host, p_mouse.target) : std::nullopt;
		if (!column) {
			return false;
		}
		if (p_mouse.kind == HudMouseKind::ReleaseOutside) {
			mouse_release_outside();
			return true;
		}
		column_ = double(*column);
		mouse_released();
		return true;
	}

protected:
	void member(std::string_view p_name, const As2Value &p_value) override {
		if (p_name == "column") {
			column_handler(p_value.to_number());
			return;
		}
		RowBaseClass::member(p_name, p_value);
	}

	void setup_mouse_interaction() override {
		if (mouse_listening()) {
			return;
		}
		for (size_t i = 0; i < kKeyColumnCount; ++i) {
			host_->set_mouse_target(host_->clip("hit_col_" + std::to_string(i)), true);
		}
		set_mouse_listening(true);
	}

	void turn_off_mouse_interaction() override {
		if (!mouse_listening()) {
			return;
		}
		for (size_t i = 0; i < kKeyColumnCount; ++i) {
			host_->set_mouse_target(host_->clip("hit_col_" + std::to_string(i)), false);
		}
		set_mouse_listening(false);
	}

private:
	std::optional<size_t> column_of(HudWidgetHost &r_host, const std::string &p_target) const {
		for (size_t i = 0; i < kKeyColumnCount; ++i) {
			if (p_target == r_host.clip("hit_col_" + std::to_string(i)).path()) {
				return i;
			}
		}
		return std::nullopt;
	}

	void swap_text_fields(HudWidgetHost &r_host) {
		const As2Clip self = r_host.self();
		for (size_t i = 0; i < kKeyLabels; ++i) {
			const As2Clip field = r_host.clip("Label" + std::to_string(i));
			if (!field.exists()) {
				continue;
			}
			field.set_visible(false);
			const As2Clip label =
					r_host.attach_movie(self, "mc_label", "Label_" + std::to_string(i), self.next_highest_depth());
			label.set_x(field.x());
			label.set_y(field.y());
			if (HudListLabel *l = controller_at<HudListLabel>(r_host, label)) {
				if (const std::optional<float> size = field.text_size()) {
					l->set_text_size(*size);
				}
				l->set_label_width(field.width());
				l->set_text(field.text());
			}
		}
	}

	HudListLabel *label_controller(size_t p_index) const {
		return dynamic_cast<HudListLabel *>(
				host_->screen().controller(host_->clip("Label_" + std::to_string(p_index)).path()));
	}

	void set_label(size_t p_index, const std::string &p_text) const {
		if (HudListLabel *l = label_controller(p_index)) {
			l->set_text(p_text);
		}
	}

	void setup_direct_access() {
		std::vector<ListAccessItem> items;
		constexpr std::array<std::string_view, kKeyLabels> kValues{ "Label", "Key", "Mouse", "Joystick" };
		for (size_t i = 0; i < kKeyLabels; ++i) {
			items.push_back(ListAccessItem{ std::string(kValues[i]), "", UiDirectAccessOp::Text,
					host_->clip("Label_" + std::to_string(i)).path() });
		}
		items.push_back(ListAccessItem{ "Column", "column", UiDirectAccessOp::Member, host_->path() });
		register_direct_access_array(std::move(items));
	}

	void column_handler(double p_column) {
		column_ = std::isnan(p_column) ? 0.0 : p_column;
		if (column_ < 0.0) {
			column_ = double(kKeyColumnCount - 1);
		} else if (column_ > double(kKeyColumnCount - 1)) {
			column_ = 0.0;
		}
		for (size_t i = 0; i < kKeyColumnCount; ++i) {
			host_->clip("col_" + std::to_string(i)).set_visible(false);
		}
		host_->clip("col_" + std::to_string(int32_t(column_))).set_visible(true);
	}

	double column_ = 0.0;
};

const OptionRowData *nested_row(const OptionsModel &p_model, const HudNestedList &p_list) {
	return p_model.row_of_key(p_list.source.key);
}

}

std::optional<As2Value> hud_options_list_rows(const HudListBinding &p_binding, const HudClientState &p_client) {
	if (p_binding.nested.empty() || p_client.options == nullptr) {
		return std::nullopt;
	}
	As2Value rows = As2Value::array();
	for (const HudNestedList &n : p_binding.nested) {
		if (n.hidden_on_pc && hud_platform_win32(p_client)) {
			continue;
		}
		const OptionRowData *d = nested_row(*p_client.options, n);
		if (d == nullptr) {
			continue;
		}
		As2Value row = As2Value::object();
		row.set("Type", As2Value::of(n.row_type));
		row.set("Label", As2Value::of(n.label));
		row.set("key", As2Value::of(n.source.key));
		if (d->row == OptionRowType::BarSelector) {
			row.set("From", As2Value::of(d->from));
			row.set("To", As2Value::of(d->to));
			row.set("StepSize", As2Value::of(d->step));
			row.set("Value", As2Value::of(d->value));
		} else {
			As2Value items = As2Value::array();
			for (const OptionChoice &c : d->choices) {
				As2Value item = As2Value::object();
				item.set("Label", As2Value::of(c.label));
				item.set("Index", As2Value::of(c.index));
				items.push(std::move(item));
			}
			row.set("SubItems", std::move(items));
			row.set("Highlighted", As2Value::of(d->highlighted));
		}
		rows.push(std::move(row));
	}
	return rows;
}

std::optional<std::vector<As2Value>> hud_options_direct_rows(
		const HudDataSource &p_query, const HudClientState &p_client) {
	if (ui_data_category_leaf(p_query.category) != kSettingsCategory || p_query.key != kKeyBindingsListKey ||
			p_client.options == nullptr) {
		return std::nullopt;
	}
	const OptionsModel &m = *p_client.options;
	std::vector<As2Value> rows;
	if (m.key_tab >= m.key_bindings.size()) {
		return rows;
	}
	const auto text = [&](const std::string &p_sid) {
		if (p_sid.empty() || !p_client.translate) {
			return p_sid;
		}
		return p_client.translate(p_sid).value_or(p_sid);
	};
	const std::vector<KeyBindingRowData> &tab = m.key_bindings[m.key_tab];
	for (size_t i = 0; i < tab.size(); ++i) {
		As2Value row = As2Value::object();
		row.set("Label", As2Value::of(text(tab[i].label)));
		row.set("Key", As2Value::of(text(tab[i].inputs[0])));
		row.set("Mouse", As2Value::of(text(tab[i].inputs[1])));
		row.set("Joystick", As2Value::of(text(tab[i].inputs[2])));
		row.set("Column", As2Value::of(int32_t(m.key_column)));
		row.set("Index", As2Value::of(int32_t(i)));
		rows.push_back(std::move(row));
	}
	return rows;
}

void register_hud_widgets_options_rows(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.List.Rows.TextSelectorRow", [] { return std::make_unique<TextSelectorRowClass>(); });
	r_registry.add("Widget.List.Rows.BarSelectorRow", [] { return std::make_unique<BarSelectorRowClass>(); });
	r_registry.add("Widget.List.Rows.KeyBindingRow", [] { return std::make_unique<KeyBindingRowClass>(); });
}

}
