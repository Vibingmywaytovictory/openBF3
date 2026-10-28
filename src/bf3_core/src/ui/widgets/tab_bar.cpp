#include "button.h"
#include "kit_icons.h"

#include <optional>
#include <string>
#include <vector>

namespace bf3::core::ui {

namespace {

constexpr int32_t kPressed = 0;
constexpr int32_t kReleased = 1;
constexpr int32_t kHighlight = 2;
constexpr int32_t kUnhighlight = 3;
constexpr int32_t kShow = 4;
constexpr int32_t kHide = 5;
constexpr float kShoulderIconRowHeight = 34.0f;
constexpr std::string_view kEndOfRoundCategory = "ui/uicomponents/uiendofroundcomp";
constexpr int32_t kEorTabsSetupKey = -995000405;
constexpr std::string_view kCustomizationCategory = "ui/uicomponents/uicustomizationcomp";
constexpr int32_t kCustomizeSubTabsSetupKey = -1696904176;
constexpr std::array<std::string_view, 2> kUnrankedEorTabs{ "ID_M_SCOREBOARD", "ID_M_STATS" };

const std::string *non_empty_property(const HudWidgetInit &p_init, std::string_view p_name) {
	const std::string *v = p_init.property(p_name);
	return v != nullptr && !v->empty() ? v : nullptr;
}

class TabBarClass final : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		if (const As2Value *v = r_host.construct_param("m_tabsDataStr")) {
			tabs_text_ = v->to_string();
		}
	}

	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override {
		eor_setup_.reset();
		kits_setup_.reset();
		eor_tabs_.clear();
		for (const HudDataSource &s : p_node.sources) {
			if (s.category == kEndOfRoundCategory && s.key == kEorTabsSetupKey) {
				eor_setup_ = s;
			}
			if (s.category == kCustomizationCategory && s.key == kCustomizeSubTabsSetupKey) {
				kits_setup_ = s;
			}
		}
		for (const HudWidgetProperty &p : p_node.properties) {
			if (p.name == "Tabs") {
				eor_tabs_ = as2_split(p.value, ";");
			}
		}
	}

	void bind(const HudModel &p_model, const HudClientState &, HudData &r_out) const override {
		if (kits_setup_) {
			As2Value enabled = As2Value::array();
			const size_t tabs = as2_split(tabs_text_, ";").size();
			for (size_t i = 0; i < tabs; ++i) {
				enabled.push(As2Value::of(i == 0));
			}
			As2Value setup = As2Value::object();
			setup.set("EnabledTabs", std::move(enabled));
			r_out.push_back({ kits_setup_->name, std::move(setup), kits_setup_->on_initialize });
		}
		if (!eor_setup_) {
			return;
		}
		As2Value enabled = As2Value::array();
		for (const std::string &tab : eor_tabs_) {
			const bool always =
					std::find(kUnrankedEorTabs.begin(), kUnrankedEorTabs.end(), tab) != kUnrankedEorTabs.end();
			enabled.push(As2Value::of(always || p_model.end_of_round.is_ranked));
		}
		As2Value setup = As2Value::object();
		setup.set("EnabledTabs", std::move(enabled));
		r_out.push_back({ eor_setup_->name, std::move(setup), eor_setup_->on_initialize });
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		r_host.clip("tabBarSize").set_visible(false);
		start_width_ = as2_round(r_host.self().width());
		start_height_ = as2_round(r_host.self().height());
		holder_ = r_host.create_empty_movie_clip(r_host.self(), "tabBarHolder", r_host.self().next_highest_depth())
						  .path();
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		if (const As2Value *v = r_host.construct_param("m_tabsDataStr")) {
			tabs_text_ = v->to_string();
		}
		if (const As2Value *v = r_host.construct_param("m_selectedTab")) {
			default_selected_ = v->to_number();
		}
		if (const As2Value *v = r_host.construct_param("m_tabSpacing")) {
			tab_spacing_ = v->to_number();
		}
		if (const As2Value *v = r_host.construct_param("m_textSize")) {
			text_size_ = v->to_string();
		}
		if (const std::string *v = non_empty_property(p_init, "HideOnConsole")) {
			hide_on_console_ = *v == "true";
		}
		if (const std::string *v = non_empty_property(p_init, "HideOnPC")) {
			hide_on_pc_ = *v == "true";
		}
		if (const std::string *v = non_empty_property(p_init, "Tabs")) {
			tabs_text_ = *v;
		}
		if (const std::string *v = non_empty_property(p_init, "DefaultSelected")) {
			default_selected_ = As2Value::of(*v).to_number();
		}
		if (const std::string *v = non_empty_property(p_init, "TabSpacing")) {
			tab_spacing_ = As2Value::of(*v).to_number();
		}
		if (const std::string *v = non_empty_property(p_init, "TextSize")) {
			text_size_ = *v;
		}
		const As2Clip self = r_host.self();
		self.set_x(float(as2_round(self.x())));
		self.set_y(float(as2_round(self.y())));
		self.set_xscale(100.0f);
		self.set_yscale(100.0f);
		r_host.clip("tabBarSize").set_width(float(start_width_));
		r_host.clip("tabBarSize").set_height(float(start_height_));
		platform_ = r_host.client().platform.empty() ? std::string("win32") : r_host.client().platform;
		tabs_.clear();
		enabled_.clear();
		As2Value from_text = As2Value::array();
		const std::vector<std::string> labels = as2_split(tabs_text_, ";");
		for (size_t i = 0; i < labels.size(); ++i) {
			As2Value tab = As2Value::object();
			tab.set("ButtonLabel", As2Value::of(labels[i]));
			tab.set("ButtonId", As2Value::of("ID" + std::to_string(i)));
			from_text.push(std::move(tab));
			enabled_.push_back(true);
			total_tabs_++;
		}
		const As2Value *data_tabs = p_init.value("Tabs");
		const As2Value start_tabs = data_tabs != nullptr ? *data_tabs : from_text;
		if (const As2Value *v = p_init.value("SelectedTab")) {
			default_selected_ = v->to_number();
		}
		if (const As2Value *v = p_init.value("tabSpacing")) {
			tab_spacing_ = v->to_number();
		}
		self.set_visible(platform_ == "win32" ? !hide_on_pc_ : !hide_on_console_);
		setup_refresh_data(*this, r_host, p_init.data);
		add_shoulder_icons();
		update_tabs(start_tabs);
		update_setup(hud_init_value(p_init, "Setup"));
	}

	void update(HudWidgetHost &, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "SelectedTab") {
			select_handler(p_value.to_number());
		} else if (p_key == "Setup") {
			update_setup(p_value);
		}
	}

	bool input_concept(HudWidgetHost &r_host, int32_t p_concept, bool p_pressed) override {
		if (p_pressed || platform_.empty() || platform_ == "win32" || hide_on_console_) {
			return false;
		}
		const UiInputConcept c = UiInputConcept(p_concept);
		if (c != UiInputConcept::TabLeft && c != UiInputConcept::TabRight) {
			return false;
		}
		select_handler(next_enabled_tab(c == UiInputConcept::TabLeft ? -1.0 : 1.0));
		r_host.fire_query(kSoundSelect, As2Value::of(false));
		r_host.fire_event(kReleased, As2Value::of(tab_id(selected_tab_.value_or(0.0))));
		return false;
	}

	void in_event(HudWidgetHost &r_host, int32_t p_index, const As2Value &) override {
		if (p_index == kShow) {
			r_host.self().set_visible(true);
		} else if (p_index == kHide) {
			r_host.self().set_visible(false);
		}
	}

	size_t tab_count() const { return tabs_.size(); }
	const std::string &tab_path(size_t p_index) const { return tabs_[p_index].path; }
	std::optional<double> selected_tab() const { return selected_tab_; }

private:
	std::optional<HudDataSource> eor_setup_;
	std::optional<HudDataSource> kits_setup_;
	std::vector<std::string> eor_tabs_;
	struct Tab {
		std::string id;
		std::string path;
	};

	As2Clip holder() const { return host_->screen().clip(holder_); }

	HudButton *button(size_t p_index) const {
		if (p_index >= tabs_.size()) {
			return nullptr;
		}
		return dynamic_cast<HudButton *>(host_->screen().controller(tabs_[p_index].path));
	}

	std::string tab_id(double p_index) const {
		if (!(p_index >= 0.0) || size_t(p_index) >= tabs_.size()) {
			return "undefined";
		}
		return tabs_[size_t(p_index)].id;
	}

	static double tab_index_from_name(std::string_view p_name) {
		const size_t us = p_name.find('_');
		if (us == std::string_view::npos) {
			return std::nan("");
		}
		const size_t end = p_name.find('_', us + 1);
		return As2Value::of(std::string(p_name.substr(us + 1, end == std::string_view::npos ? end : end - us - 1)))
				.to_number();
	}

	double total_num_tabs() const { return double(as2_split(tabs_text_, ";").size()); }

	double num_enabled_tabs() const {
		double n = total_num_tabs();
		for (const bool e : enabled_) {
			if (!e) {
				n -= 1.0;
			}
		}
		return n;
	}

	void update_setup(const As2Value &p_data) {
		if (!p_data.defined()) {
			return;
		}
		if (p_data.field("PlusTabs").defined()) {
			if (const std::optional<As2Value> plus = ui_base_object_to_array(p_data.field("PlusTabs"))) {
				const size_t n = std::min(plus->size(), tabs_.size());
				for (size_t i = 0; i < n; ++i) {
					if (as2_equals_true(plus->at(i))) {
						if (HudButton *b = button(i)) {
							b->set_plus();
						}
					}
				}
			}
		}
		if (p_data.field("EnabledTabs").defined()) {
			if (const std::optional<As2Value> on = ui_base_object_to_array(p_data.field("EnabledTabs"))) {
				for (size_t i = 0; i < on->size(); ++i) {
					if (as2_equals_false(on->at(i))) {
						if (i >= enabled_.size()) {
							enabled_.resize(i + 1, false);
						}
						enabled_[i] = false;
					}
				}
			}
			start_width_ = as2_round(host_->self().width() * (num_enabled_tabs() / total_num_tabs()));
		}
	}

	void add_shoulder_icons() {
		if (platform_ == "win32" || platform_.empty() || num_enabled_tabs() < 2.0) {
			icon_offset_ = 0.0;
			return;
		}
		const As2Clip h = holder();
		const As2Clip left = host_->attach_movie(h, "Buttons", "btnIcon1", h.next_highest_depth());
		left.goto_and_stop(std::string_view(platform_));
		left.child("icon").goto_and_stop("IDB_Ltrigger2");
		const As2Clip right = host_->attach_movie(h, "Buttons", "btnIcon2", h.next_highest_depth());
		right.goto_and_stop(std::string_view(platform_));
		right.child("icon").goto_and_stop("IDB_Rtrigger2");
		shoulder_icons_ = { left.path(), right.path() };
		position_shoulder_icons();
	}

	void position_shoulder_icons() {
		if (shoulder_icons_.size() < 2) {
			return;
		}
		double inset = std::nan("");
		if (platform_ == "ps3") {
			inset = 10.0;
		} else if (platform_ == "xenon") {
			inset = 8.0;
		}
		const As2Clip left = host_->screen().clip(shoulder_icons_[0]);
		const As2Clip right = host_->screen().clip(shoulder_icons_[1]);
		if (tabs_width_ < 1.0 || std::isnan(tabs_width_)) {
			tabs_width_ = start_width_;
		}
		left.set_x(float(right.width() * 0.5 - inset));
		right.set_x(float(tabs_width_ + right.width() * 0.5));
		left.set_y(kShoulderIconRowHeight * 0.5f);
		right.set_y(kShoulderIconRowHeight * 0.5f);
		icon_offset_ = left.width() - inset;
	}

	void update_tabs(const As2Value &p_tabs) {
		clear_tabs();
		for (size_t i = 0; i < p_tabs.size(); ++i) {
			if (i < enabled_.size() && enabled_[i]) {
				add_tab(p_tabs.at(i));
			}
		}
		reposition_tabs();
		select_handler(enabled_tab_index(default_selected_));
	}

	void clear_tabs() {
		const As2Clip h = holder();
		for (const Tab &t : tabs_) {
			const As2Clip mc = host_->screen().clip(t.path);
			if (mc.depth() < 0) {
				mc.swap_depths(h.next_highest_depth());
			}
			host_->remove_movie_clip(mc);
		}
		tabs_.clear();
	}

	void add_tab(const As2Value &p_data) {
		const size_t index = tabs_.size();
		const As2Clip h = holder();
		const As2Clip mc = host_->attach_movie(h, "Button", "button_" + std::to_string(index), h.next_highest_depth());
		HudScreen &screen = host_->screen();
		HudWidgetController *controller = screen.controller(mc.path());
		HudWidgetHost *mc_host = screen.host(mc.path());
		HudButton *b = dynamic_cast<HudButton *>(controller);
		if (controller == nullptr || mc_host == nullptr || b == nullptr) {
			return;
		}
		controller->on_clip_load(*mc_host);
		b->set_button_text(p_data.field("ButtonLabel").to_string());
		if (text_size_ == "small") {
			b->set_button_type("tabBarButtonSmallText");
		} else if (text_size_ == "largePremium") {
			b->set_button_type("tabBarButtonPremium");
		} else {
			b->set_button_type("tabBarButton");
		}
		b->set_align_text("center");
		b->set_resize_if_too_wide(true);
		controller->initialize(*mc_host, HudWidgetInit{});
		if (platform_ == "win32") {
			HudButtonCallbacks c;
			c.release = [this](const std::string &p_name) { button_released(p_name); };
			c.press = [this](const std::string &p_name) { fire_for_name(kPressed, p_name); };
			c.over = [this](const std::string &p_name) { fire_for_name(kHighlight, p_name); };
			c.out = [this](const std::string &p_name) { fire_for_name(kUnhighlight, p_name); };
			b->set_callbacks(std::move(c));
		}
		start_combined_width_ += b->button_width();
		tabs_.push_back({ p_data.field("ButtonId").to_string(), mc.path() });
	}

	void reposition_tabs() {
		const double n = num_enabled_tabs();
		const double extra =
				(start_width_ - icon_offset_ * 2.0 - (start_combined_width_ + tab_spacing_ * (n - 1.0))) / n;
		tabs_width_ = 0.0;
		for (size_t i = 0; double(i) < n && i < tabs_.size(); ++i) {
			HudButton *b = button(i);
			if (b == nullptr) {
				continue;
			}
			b->set_button_width(float(as2_round(extra + b->button_width())));
			const As2Clip mc = host_->screen().clip(tabs_[i].path);
			if (i > 0) {
				const As2Clip prev = host_->screen().clip(tabs_[i - 1].path);
				HudButton *pb = button(i - 1);
				mc.set_x(float(prev.x() + (pb != nullptr ? pb->button_width() : 0.0f) + tab_spacing_));
			} else {
				mc.set_x(float(icon_offset_));
			}
			tabs_width_ = mc.x() + b->button_width();
		}
		position_shoulder_icons();
	}

	void select_handler(double p_index) {
		if (selected_tab_ && *selected_tab_ >= 0.0) {
			if (HudButton *b = button(size_t(*selected_tab_))) {
				b->set_selected(false);
			}
		}
		if (p_index >= 0.0) {
			if (HudButton *b = button(size_t(p_index))) {
				b->set_selected(true);
			}
		}
		selected_tab_ = p_index;
	}

	double next_enabled_tab(double p_dir) const {
		double next = selected_tab_.value_or(std::nan("")) + p_dir;
		if (!(next < double(tabs_.size()))) {
			next = 0.0;
		} else if (next < 0.0) {
			next = double(tabs_.size()) - 1.0;
		}
		return next;
	}

	double enabled_tab_index(double p_index) const {
		double n = 0.0;
		for (size_t i = 0; double(i) < total_tabs_; ++i) {
			if (i < enabled_.size() && enabled_[i]) {
				if (p_index == double(i)) {
					return n;
				}
				n += 1.0;
			}
		}
		return selected_tab_.value_or(std::nan(""));
	}

	void fire_for_name(int32_t p_event, const std::string &p_name) {
		host_->fire_event(p_event, As2Value::of(tab_id(tab_index_from_name(p_name))));
	}

	void button_released(const std::string &p_name) {
		const double index = tab_index_from_name(p_name);
		select_handler(index);
		host_->fire_query(kSoundSelect, As2Value::of(false));
		host_->fire_event(kReleased, As2Value::of(tab_id(index)));
	}

	HudWidgetHost *host_ = nullptr;
	std::string holder_;
	std::vector<Tab> tabs_;
	std::vector<bool> enabled_;
	std::vector<std::string> shoulder_icons_;
	std::string tabs_text_ = "tab1;tab2";
	std::string text_size_ = "large";
	std::string platform_;
	bool hide_on_console_ = false;
	bool hide_on_pc_ = false;
	double default_selected_ = 0.0;
	std::optional<double> selected_tab_;
	double tab_spacing_ = 2.0;
	double start_width_ = 0.0;
	double start_height_ = 0.0;
	double start_combined_width_ = 0.0;
	double icon_offset_ = 100.0;
	double tabs_width_ = 0.0;
	double total_tabs_ = 0.0;
};

}

void register_hud_widgets_tab_bar(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Button.TabBar", [] { return std::make_unique<TabBarClass>(); });
}

}
