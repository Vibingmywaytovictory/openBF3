#include "list.h"

#include "scroll_bar.h"

namespace bf3::core::ui {

namespace {

constexpr int32_t kHighlighted = 0;
constexpr int32_t kMouseOver = 1;
constexpr int32_t kPress = 2;
constexpr int32_t kRelease = 3;
constexpr int32_t kReachedTop = 4;
constexpr int32_t kReachedBottom = 5;
constexpr int32_t kNavigateLeft = 6;
constexpr int32_t kNavigateRight = 7;
constexpr int32_t kSelectorChanged = 8;
constexpr int32_t kChangeButtonBar = 9;
constexpr int32_t kChangeMenuHint = 10;
constexpr int32_t kReleaseIndex = 11;
constexpr int32_t kEnterTop = 12;
constexpr int32_t kEnterBottom = 13;
constexpr int32_t kEnterAny = 14;
constexpr int32_t kEnterSelected = 15;
constexpr int32_t kMoveToIndex = 16;
constexpr int32_t kMoveToPercentage = 17;
constexpr int32_t kShow = 18;
constexpr int32_t kHide = 19;
constexpr int32_t kClearList = 20;
constexpr int32_t kIgnoreKeyNavigation = 21;
constexpr int32_t kAllowKeyNavigation = 22;
constexpr int32_t kShowList = 25;
constexpr int32_t kHideList = 26;
constexpr int32_t kInvalidSelection = 27;
constexpr int32_t kSelectorChangedKeyValue = 23;
constexpr int32_t kSelectorStepped = 24;

constexpr double kTiltListIndex = 22.0;
constexpr std::string_view kSelectorRows =
		"TestSelectionRow,TextSelectorRow,TextSelectorRow_28px,BarSelectorRow,LeaderBoardRow,"
		"CoopLeaderBoardRow,BarSelectorRow_28px,KeyBindingRow";
constexpr std::string_view kToggleRows = "CheckBoxRow";
constexpr std::string_view kEmptyRowNone = "emptyRowNone";

int32_t g_loop_id = 0;

std::optional<int32_t> index_of(double p_index) {
	if (!std::isfinite(p_index) || p_index != std::trunc(p_index) || p_index < double(INT32_MIN) ||
			p_index > double(INT32_MAX)) {
		return std::nullopt;
	}
	return int32_t(p_index);
}

bool is_null(const As2Value &p_v) {
	return p_v.kind == As2Value::Kind::Undefined;
}

struct RowRequest {
	std::string row_mc;
	double index = 0.0;
	As2Value data;
};

class ListClass;

class ListLogic {
public:
	explicit ListLogic(ListClass &r_list) : list_(r_list) {}

	void initialize(const As2Clip &p_row_holder, double p_list_height, double p_list_width, double p_row_spacing,
			std::string p_navigation_type, bool p_use_3d, bool p_win32) {
		row_holder_ = p_row_holder;
		list_height_ = p_list_height;
		list_width_ = p_list_width;
		row_spacing_ = p_row_spacing;
		navigation_type = std::move(p_navigation_type);
		use_3d_ = p_use_3d;
		win32_ = p_win32;
	}

	void start(double p_length);
	void update_list_length(double p_length);
	void update_width(double p_width);
	void move_to_index(double p_index, std::string_view p_dir);
	void move_to_percentage(double p_percentage);
	void shift_highlighted(double p_dir, bool p_keyboard);
	void set_highlighted(double p_index, bool p_value, bool p_mouse_used = false);
	void select_current_highlighted();
	void highlight_current_selected();
	void destroy_list(double p_highlighted);
	void row_released(double p_index, bool p_value);
	void row_pressed(double p_index);
	void mouse_over_row(double p_index, bool p_value);
	void row_highlighted(double p_index, bool p_value);
	void row_selector_changed(double p_index, const As2Value &p_arg);
	void shift_selector(double p_dir, const As2Value &p_used_mouse);
	As2Value current_selector_value() const;

	double scroll_percentage() const { return current_scroll_percentage_ * 100.0; }
	bool active() const { return active_; }
	double row_height() const { return row_height_; }
	double first_visible_row_index() const { return current_top_; }
	double last_visible_row_index() const { return current_bottom_; }
	double current_highlighted() const { return current_highlighted_; }
	As2Value id_of(double p_index) const;
	As2Value label_of(double p_index) const;
	bool disabled_of(double p_index) const;
	As2Value type_of(double p_index) const;
	As2Value current_button_layout() const;
	RowBaseClass *row(double p_index) const;
	As2Clip row_clip(double p_index) const;

	std::string navigation_type;
	std::string current_dir = "normal";
	int32_t reached_top_event = kReachedTop;
	int32_t reached_bottom_event = kReachedBottom;

private:
	struct Entry {
		std::string row_type;
		std::string path;
	};

	void handle_row(std::optional<RowRequest> p_request);
	std::string row_movie_clip(const std::string &p_type);
	void handle_cache(std::string_view p_dir, bool p_force_cleanup);
	void cache_object(Entry p_entry);
	void clear_cache(const std::string &p_type);
	void top_and_bottom_index();
	void row_handler(bool p_force_cleanup);
	void highlight_row(double p_index, bool p_value, bool p_mouse_used, bool p_keyboard_used);
	void highlight_logic(double p_index, bool p_value);
	bool all_disabled_from_now_on(double p_index, std::string_view p_dir) const;
	double next_enabled_row(double p_index, std::string_view p_dir) const;
	bool info_disabled(double p_index) const;
	const Entry *entry(double p_index) const;
	RowBaseClass *controller(const std::string &p_path) const;

	ListClass &list_;
	As2Clip row_holder_;
	double list_height_ = 0.0;
	double list_width_ = 0.0;
	double row_spacing_ = 0.0;
	bool use_3d_ = false;
	bool win32_ = false;
	bool active_ = false;
	double list_length_ = 0.0;
	double row_height_ = 0.0;
	double total_list_height_ = 0.0;
	double top_offset_ = 0.0;
	double current_top_ = 0.0;
	double current_bottom_ = 0.0;
	double list_pos_ = 0.0;
	double current_scroll_percentage_ = 0.0;
	double current_highlighted_ = std::nan("");
	bool only_do_once_ = true;
	std::map<int32_t, Entry> rows_;
	std::map<std::string, std::vector<Entry>, std::less<>> cache_;
};

class ListClass final : public HudWidgetController, public ListOwner {
public:
	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override { binding_ = p_node.list; }

	void construct(HudWidgetHost &r_host) override { host_ = &r_host; }

	void on_clip_load(HudWidgetHost &r_host) override {
		setup_start_size();
		empty_holder_ =
				r_host.create_empty_movie_clip(r_host.self(), "EmptyListHolder", r_host.self().next_highest_depth());
		list_holder_ = r_host.create_empty_movie_clip(r_host.self(), "ListHolder", r_host.self().next_highest_depth());
		setup_mask();
		r_host.self().set_visible(false);
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_data) const override {
		if (!binding_) {
			return;
		}
		const HudListBinding &b = *binding_;
		const auto put = [&](std::string p_key, As2Value p_value) {
			r_data.push_back(HudDataEntry{ std::move(p_key), std::move(p_value), true });
		};
		put("ListIndex", As2Value::of(b.list_index));
		put("RowType", As2Value::of(b.row_type));
		put("RowSpacing", As2Value::of(b.row_spacing));
		const std::optional<int32_t> selected = hud_model_list_selection(b.default_selection_query, p_model);
		put("HighlightedRow", As2Value::of(selected.value_or(b.default_highlighted_row)));
		put("NavigationType", As2Value::of(b.navigation_type));
		put("ClearListAtNavigationEvent", As2Value::of(b.clear_list_at_navigation_event));
		put("UnFocusMode", As2Value::of(b.unfocus_mode));
		put("UseScrollBar", As2Value::of(b.use_scroll_bar));
		put("ScreenRotationEnabled", As2Value::of(b.screen_rotation_enabled));
		put("Use3DSelection", As2Value::of(b.use_3d_selection));
		put("FireHighlightOnMouseOut", As2Value::of(b.fire_highlight_on_mouse_out));
		put("DataIncludesButtonLayout", As2Value::of(b.data_includes_button_layout));
		put("SendIndexWithEvent", As2Value::of(b.send_index_with_event));
		put("EmptyRowType", As2Value::of(b.empty_row_type));
		put("SpinnerEnabled", As2Value::of(b.spinner_enabled));
		put("DisabledRows", As2Value::of(b.disabled_rows));
		put("HighLightOnUpdate", As2Value::of(b.high_light_on_update));
		put("Visible", As2Value::of(b.visible));
		if (b.list_query.use_direct_access) {
			r_data.push_back(HudDataEntry{
					"ListAccess", As2Value::of(hud_list_access_handle(b.list_query)), b.list_query.on_initialize });
		} else if (std::optional<As2Value> options = hud_options_list_rows(b, p_client)) {
			r_data.push_back(HudDataEntry{ "List", std::move(*options), true });
		} else if (std::optional<As2Value> rows = hud_model_list_rows(b.list_query, p_model, p_client)) {
			r_data.push_back(HudDataEntry{ "List", std::move(*rows), b.list_query.on_initialize });
		} else if (std::optional<As2Value> menu = hud_menu_list_rows(b.list_query, p_model)) {
			r_data.push_back(HudDataEntry{ "List", std::move(*menu), b.list_query.on_initialize });
		}
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		selected_index_ = -1.0;
		highlighted_index_ = -1.0;
		const auto value = [&](std::string_view p_key) { return hud_init_value(p_init, p_key); };
		list_index_ = value("ListIndex");
		p_row_type_ = value("RowType");
		p_row_spacing_ = value("RowSpacing");
		p_default_highlighted_row_ = value("HighlightedRow");
		p_navigation_type_ = value("NavigationType");
		clear_list_at_navigation_event_ = value("ClearListAtNavigationEvent").truthy();
		use_scrollbar_ = value("UseScrollBar").truthy();
		screen_rotation_enabled_ = value("ScreenRotationEnabled").truthy();
		use_3d_selection_ = value("Use3DSelection").truthy();
		fire_highlight_on_mouse_out_ = value("FireHighlightOnMouseOut").truthy();
		data_includes_button_layouts_ = value("DataIncludesButtonLayout");
		send_index_with_event_ = value("SendIndexWithEvent");
		empty_row_type_ = value("EmptyRowType");
		selector_width_ = value("SelectorWidth");
		spinner_enabled_ = value("SpinnerEnabled").truthy();
		const As2Value disabled = value("DisabledRows");
		high_light_on_update_ = value("HighLightOnUpdate").truthy();
		keep_scroll_offset_ = value("KeepScrollOffset").truthy();
		if (!as2_loose_equals(disabled, As2Value::of(""))) {
			disabled_rows_ = as2_split(disabled.to_string(), ";");
		} else {
			disabled_rows_.reset();
		}
		if (use_scrollbar_) {
			add_scrollbar();
		} else {
			remove_scrollbar();
		}
		sort_properties();
		setup_empty_rows();
		logic_.initialize(list_holder_, start_h_, mask().width() - 10.0, row_spacing_, navigation_type_,
				use_3d_selection_, hud_platform_win32(r_host.client()));
		setup_refresh_data(p_init.data);
		if (!is_null(p_default_highlighted_row_)) {
			highlighted_index_ = p_default_highlighted_row_.to_number();
		}
		const As2Value &visible = value("Visible");
		r_host.self().set_visible(visible.truthy() || is_null(visible));
	}

	void update(HudWidgetHost &, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "HighlightedRow") {
			update_highlighted_row_data(p_value);
			return;
		}
		update_data(p_key, p_value);
	}

	void drive(HudWidgetHost &r_host, const HudModel &p_model, const HudClientState &p_client) override {
		if (!binding_ || !binding_->list_query.use_direct_access) {
			return;
		}
		std::optional<std::vector<As2Value>> rows = hud_model_direct_rows(binding_->list_query, p_model);
		if (!rows) {
			rows = hud_options_direct_rows(binding_->list_query, p_client);
		}
		if (!rows || rows == direct_rows_) {
			return;
		}
		direct_rows_ = *rows;
		r_host.screen().direct_access().set_array(hud_list_access_handle(binding_->list_query), std::move(*rows));
	}

	void direct_access(HudWidgetHost &, const UiDirectAccessApply &p_apply) override {
		if (p_apply.op == UiDirectAccessOp::ArrayLengthInvoke && p_apply.name == "updateList") {
			update_list(p_apply.value.to_number());
		}
	}

	void in_event(HudWidgetHost &r_host, int32_t p_index, const As2Value &p_param) override {
		switch (p_index) {
			case kEnterTop:
				enter_on_top();
				break;
			case kEnterAny:
				change_focus();
				break;
			case kEnterSelected:
				enter_selected(p_param);
				break;
			case kEnterBottom:
				enter_bottom();
				break;
			case kMoveToIndex:
				change_focus();
				highlight_index(p_param.to_number());
				break;
			case kMoveToPercentage:
				change_focus();
				logic_.move_to_percentage(p_param.to_number());
				break;
			case kShow:
				r_host.self().set_visible(true);
				r_host.fire_event(kShowList);
				break;
			case kHide:
				r_host.self().set_visible(false);
				r_host.fire_event(kHideList);
				break;
			case kClearList:
				destroy_list();
				break;
			case kIgnoreKeyNavigation:
				allow_key_navigation_ = false;
				break;
			case kAllowKeyNavigation:
				allow_key_navigation_ = true;
				break;
			default:
				break;
		}
	}

	bool input_concept(HudWidgetHost &r_host, int32_t p_concept, bool p_pressed) override {
		if (!p_pressed || !allow_key_navigation_) {
			return false;
		}
		switch (UiInputConcept(p_concept)) {
			case UiInputConcept::NavigateUp:
				logic_.current_dir = "top";
				if (navigation_type_ != "none") {
					shift_highlighted(-1.0, true);
				}
				return false;
			case UiInputConcept::NavigateDown:
				logic_.current_dir = "bottom";
				if (navigation_type_ != "none") {
					shift_highlighted(1.0, true);
				}
				return false;
			case UiInputConcept::NavigateLeft:
				if (info_selector(highlighted_index_)) {
					logic_.shift_selector(-1.0, As2Value::of(false));
				} else {
					r_host.fire_event(kNavigateLeft, As2Value::of(0));
				}
				return false;
			case UiInputConcept::NavigateRight:
				if (info_selector(highlighted_index_)) {
					logic_.shift_selector(1.0, As2Value());
				} else {
					r_host.fire_event(kNavigateRight, As2Value::of(0));
				}
				return false;
			case UiInputConcept::Activate:
				if (info_toggable(highlighted_index_)) {
					logic_.shift_selector(0.0, As2Value());
				} else {
					fire_press_event(highlighted_index_, true);
					fire_release_event(highlighted_index_, true);
				}
				return false;
			default:
				return false;
		}
	}

	void focus_gained(HudWidgetHost &r_host) override {
		has_focus_ = true;
		if (focused_before_) {
			r_host.fire_query(kSoundNavigate, As2Value::of(false));
		}
		focused_before_ = true;
		if (logic_.active()) {
			logic_.highlight_current_selected();
		}
		if (row_count_ == 0.0 || !has_active_rows_) {
			trigger_invalid_selection();
		}
	}

	void focus_lost(HudWidgetHost &) override {
		has_focus_ = false;
		button_row_handler(As2Value::array());
		logic_.set_highlighted(highlighted_index_, false);
	}

	void enter_frame(HudWidgetHost &r_host) override {
		if (!row_out_enter_frame_) {
			return;
		}
		row_out_enter_frame_ = false;
		r_host.set_enter_frame(false);
		if (over_index_ == -1.0) {
			fire_highlight_event(highlighted_index_, true);
		}
	}

	std::vector<uint32_t> register_access_data(const std::vector<ListAccessItem> &p_items, double p_index) override {
		std::vector<uint32_t> ids;
		for (const ListAccessItem &item : p_items) {
			if (list_index_.to_number() == kTiltListIndex) {
				continue;
			}
			std::vector<As2Value> args{ As2Value::of(int32_t(UiDirectAccessOp::Array)),
				As2Value::of(direct_access_data_), As2Value::of(int32_t(item.type)), As2Value::of(p_index),
				As2Value::of(item.value) };
			if (!item.member.empty()) {
				args.push_back(As2Value::of(item.member));
			}
			if (const std::optional<uint32_t> id = host_->register_mc(host_->screen().clip(item.object), args)) {
				ids.push_back(*id);
			}
		}
		return ids;
	}

	void unregister_access_data(const std::vector<uint32_t> &p_ids) override {
		if (list_index_.to_number() == kTiltListIndex) {
			return;
		}
		for (const uint32_t id : p_ids) {
			host_->unregister_mc(id);
		}
	}

	void row_released(double p_index, bool p_value) override { logic_.row_released(p_index, p_value); }
	void row_pressed(double p_index) override { logic_.row_pressed(p_index); }
	void row_mouse_over(double p_index, bool p_value) override { logic_.mouse_over_row(p_index, p_value); }
	void row_highlighted(double p_index, bool p_value) override { logic_.row_highlighted(p_index, p_value); }
	void row_selector_changed(double p_index, const As2Value &p_arg) override {
		logic_.row_selector_changed(p_index, p_arg);
	}

	void selector_handler(double p_index, const As2Value &p_used_mouse) {
		if (row_count_ == 0.0) {
			return;
		}
		if (p_used_mouse.truthy()) {
			logic_.set_highlighted(p_index, true, true);
		}
		host_->fire_event(kSelectorChanged, logic_.current_selector_value());
		host_->fire_event(kSelectorStepped, logic_.current_selector_value());
		if (!use_direct_access_) {
			const As2Value &key = data_row(p_index).field("key");
			if (!is_null(key)) {
				host_->fire_event(kSelectorChangedKeyValue, key);
			}
		}
		host_->fire_query(kSoundSelect, As2Value::of(false));
	}

	void on_row_released(double p_index, bool p_selected) {
		if (!use_direct_access_) {
			set_data_field(p_index, "selected", As2Value::of(p_selected));
		}
		if (p_selected) {
			selected_index_ = p_index;
			fire_release_event(selected_index_, true);
		}
	}

	void on_row_pressed(double p_index) {
		selected_index_ = p_index;
		fire_press_event(selected_index_, true);
	}

	void on_row_mouse_over(double p_index, bool p_over) {
		if (p_over) {
			over_index_ = p_index;
			fire_mouse_over_event(over_index_, true);
			fire_menu_hint_event(over_index_, true);
			return;
		}
		over_index_ = -1.0;
		fire_menu_hint_event(over_index_, true);
		if (fire_highlight_on_mouse_out_) {
			row_out_enter_frame_ = true;
			host_->set_enter_frame(true);
		}
	}

	void on_row_highlighted(double p_index, bool p_highlighted) {
		if (!high_light_on_update_ && !has_focus_ && p_highlighted) {
			if (logic_.active()) {
				logic_.set_highlighted(p_index, false);
			}
			return;
		}
		if (row_count_ == 0.0) {
			return;
		}
		if (!use_direct_access_) {
			set_data_field(p_index, "highlighted", As2Value::of(p_highlighted));
		}
		if (!p_highlighted) {
			return;
		}
		highlighted_index_ = p_index;
		fire_highlight_event(highlighted_index_, true);
		fire_menu_hint_event(highlighted_index_, true);
		button_row_handler(logic_.current_button_layout());
		if (info_selector(highlighted_index_)) {
			host_->fire_event(kSelectorChanged, logic_.current_selector_value());
		}
		update_scroll_bar();
	}

	std::optional<RowRequest> update_list_row(double p_index);
	bool info_disabled(double p_index) const {
		if (use_direct_access_) {
			return logic_.disabled_of(p_index);
		}
		const As2Value &d = data_row(p_index).field("Disabled");
		return is_null(d) ? false : d.truthy();
	}
	void list_height_handler(double p_total);
	void request_focus_from_row() { change_focus(); }

	const std::string &row_type() const { return row_type_; }
	double row_count() const { return row_count_; }
	double highlighted_index() const { return highlighted_index_; }
	bool uses_direct_access() const { return use_direct_access_; }
	const ListLogic &logic() const { return logic_; }
	ListLogic &logic() { return logic_; }
	std::string scrollbar_path() const { return scrollbar_path_; }
	As2Clip mask() const { return host_->screen().clip(mask_path_); }

	void navigate_event(int32_t p_event) {
		if (row_count_ == 0.0 || row_count_ == -1.0 || std::isnan(row_count_)) {
			return;
		}
		if (clear_list_at_navigation_event_) {
			destroy_list();
		}
		host_->fire_event(p_event, As2Value::of(0));
	}

	void update_list_rows(const As2Value &p_rows) { update_list_data(p_rows); }

private:
	void setup_start_size() {
		const As2Clip self = host_->self();
		start_w_ = as2_round(self.width());
		start_h_ = as2_round(self.height());
		self.set_xscale(100.0f);
		self.set_yscale(100.0f);
		self.set_x(float(as2_round(self.x())));
		self.set_y(float(as2_round(self.y())));
		const As2Clip size = host_->clip("listSize");
		size.set_width(float(start_w_));
		size.set_height(float(start_h_));
		size.set_alpha(50.0f);
		size.set_visible(false);
	}

	void setup_mask() {
		if (!mask_path_.empty()) {
			host_->remove_movie_clip(mask());
			mask_path_.clear();
		}
		const As2Clip self = host_->self();
		const As2Clip m = host_->attach_movie(self, "listMask", "listMask", self.next_highest_depth());
		mask_path_ = m.path();
		m.set_x(-5.0f);
		m.set_width(float(start_w_ + 10.0));
		m.set_height(float(start_h_));
		m.set_alpha(0.0f);
		list_holder_.set_mask(m);
	}

	void setup_empty_rows() {
		const std::string type = empty_row_type_.to_string();
		if (type == kEmptyRowNone || is_null(empty_row_type_)) {
			return;
		}
		const As2Clip first =
				host_->attach_movie(empty_holder_, type, "emptyRow_0", empty_holder_.next_highest_depth());
		first.set_width(float(start_w_));
		const double h = first.height();
		empty_row_count_ = (start_h_ + row_spacing_) / (h + row_spacing_);
		for (int32_t i = 1; double(i) < std::floor(empty_row_count_); ++i) {
			const As2Clip row = host_->attach_movie(
					empty_holder_, type, "emptyRow_" + std::to_string(i), empty_holder_.next_highest_depth());
			row.set_y(float((h + row_spacing_) * i));
			row.set_width(float(start_w_));
		}
	}

	void handle_empty_row(double p_index, bool p_show = false) {
		if (empty_row_type_.kind == As2Value::Kind::String && empty_row_type_.text == kEmptyRowNone) {
			return;
		}
		if (p_index == -1.0) {
			const SwfSceneNode *n = empty_holder_.node();
			if (n == nullptr) {
				return;
			}
			const std::vector<SwfNodeId> children = n->children;
			for (const SwfNodeId id : children) {
				if (const SwfSceneNode *c = host_->screen().scene().node(id)) {
					if (c->kind == SwfNodeKind::Sprite) {
						host_->screen().clip(c->path).set_visible(p_show);
					}
				}
			}
			return;
		}
		if (const std::optional<int32_t> i = index_of(p_index)) {
			const As2Clip row = empty_holder_.child("emptyRow_" + std::to_string(*i));
			if (row.exists()) {
				row.set_visible(p_show);
			}
		}
	}

	void add_scrollbar() {
		const As2Clip self = host_->self();
		const As2Clip bar = host_->attach_movie(self, "ScrollBar", "ScrollBar", self.next_highest_depth());
		scrollbar_path_ = bar.path();
		if (ScrollBarClass *s = scrollbar()) {
			s->init();
			HudScreen &screen = host_->screen();
			const std::string path = host_->path();
			s->set_scroll_updated([&screen, path](double p_percentage) {
				if (ListClass *l = dynamic_cast<ListClass *>(screen.controller(path))) {
					l->logic().move_to_percentage(as2_round(p_percentage));
				}
			});
			bar.set_x(float(start_w_ - 16.0));
			s->set_size(start_h_);
		}
	}

	void remove_scrollbar() {
		if (scrollbar_path_.empty()) {
			return;
		}
		host_->remove_movie_clip(host_->screen().clip(scrollbar_path_));
		scrollbar_path_.clear();
	}

	ScrollBarClass *scrollbar() const {
		return scrollbar_path_.empty() ? nullptr
									   : dynamic_cast<ScrollBarClass *>(host_->screen().controller(scrollbar_path_));
	}

	void sort_properties() {
		if (!as2_loose_equals(p_row_type_, As2Value::of(""))) {
			row_type_ = p_row_type_.to_string();
		}
		if (!as2_loose_equals(p_row_spacing_, As2Value::of(0))) {
			row_spacing_ = p_row_spacing_.to_number();
		}
		if (!as2_loose_equals(p_navigation_type_, As2Value::of(""))) {
			navigation_type_ = p_navigation_type_.to_string();
		}
	}

	void setup_refresh_data(const HudData &p_data) {
		const As2Value *highlighted = nullptr;
		for (const HudDataEntry &e : p_data) {
			if (e.key == "HighlightedRow") {
				highlighted = &e.value;
				continue;
			}
			update_data(e.key, e.value);
		}
		if (highlighted != nullptr && !is_null(*highlighted)) {
			update_highlighted_row_data(*highlighted);
		}
	}

	void update_data(std::string_view p_key, const As2Value &p_value) {
		if (p_key == "ListAccess") {
			update_list_access_data(p_value);
		} else if (p_key == "List") {
			update_list_data(p_value);
		} else if (p_key == "Visible") {
			if (!is_null(p_value)) {
				host_->self().set_visible(p_value.truthy());
			}
		} else if (p_key == "NavigationTypeSource") {
			navigation_type_ = p_value.to_string();
			logic_.navigation_type = navigation_type_;
		}
	}

	void update_list_access_data(const As2Value &p_handle) {
		use_direct_access_ = true;
		direct_access_data_ = p_handle.to_string();
		destroy_list();
		setup_mask();
		host_->register_mc(host_->self(),
				{ As2Value::of(int32_t(UiDirectAccessOp::ArrayLengthInvoke)), As2Value::of(direct_access_data_),
						As2Value::of("updateList") });
	}

	void update_list(double p_length) {
		const double before = row_count_;
		row_count_ = p_length;
		if (!logic_.active()) {
			logic_.start(row_count_);
			update_highlighted_row_data(As2Value::of(highlighted_index_));
		} else {
			logic_.update_list_length(row_count_);
			if (!(empty_row_type_.kind == As2Value::Kind::String && empty_row_type_.text == kEmptyRowNone) &&
					before > row_count_) {
				for (double i = row_count_; i < before; i += 1.0) {
					handle_empty_row(i, true);
				}
			}
			update_highlighted_row_data(As2Value::of(highlighted_index_));
		}
		if (has_focus_ && row_count_ == 0.0) {
			trigger_invalid_selection();
		}
	}

	void update_list_data(const As2Value &p_data) {
		use_direct_access_ = false;
		if (p_data == data_array_ || is_null(p_data)) {
			return;
		}
		data_array_ = p_data;
		const double highlighted = highlighted_index_;
		destroy_list();
		row_count_ = double(data_array_.size());
		setup_mask();
		highlighted_index_ = highlighted;
		logic_.start(row_count_);
		if (highlighted > -1.0) {
			update_highlighted_row_data(As2Value::of(highlighted));
		}
		if (has_focus_ && !has_active_rows_) {
			trigger_invalid_selection();
		}
	}

	void trigger_invalid_selection() {
		if (local_selection_) {
			return;
		}
		local_selection_ = true;
		host_->fire_event(kInvalidSelection);
		local_selection_ = false;
	}

	const As2Value &data_row(double p_index) const {
		static const As2Value kUndefined;
		const std::optional<int32_t> i = index_of(p_index);
		return i && *i >= 0 && size_t(*i) < data_array_.size() ? data_array_.items[size_t(*i)] : kUndefined;
	}

	void set_data_field(double p_index, std::string_view p_field, As2Value p_value) {
		const std::optional<int32_t> i = index_of(p_index);
		if (i && *i >= 0 && size_t(*i) < data_array_.items.size() &&
				data_array_.items[size_t(*i)].kind == As2Value::Kind::Object) {
			data_array_.items[size_t(*i)].set(p_field, std::move(p_value));
		}
	}

	bool row_disabled(double p_row_number) const {
		if (!disabled_rows_) {
			return false;
		}
		for (const std::string &d : *disabled_rows_) {
			if (As2Value::of(d).to_number() == p_row_number) {
				return true;
			}
		}
		return false;
	}

	std::string info_type(double p_index) const {
		const As2Value &t = data_row(p_index).field("Type");
		return is_null(t) || t.to_string().empty() ? row_type_ : t.to_string();
	}

	bool info_selector(double p_index) const {
		return kSelectorRows.find(info_type(p_index)) != std::string_view::npos;
	}
	bool info_toggable(double p_index) const { return kToggleRows.find(info_type(p_index)) != std::string_view::npos; }

	void update_highlighted_row_data(const As2Value &p_data) {
		if (use_direct_access_) {
			double data = p_data.to_number();
			if (row_count_ > 0.0) {
				if (data < 0.0) {
					logic_.set_highlighted(highlighted_index_, false);
				} else {
					if (data > row_count_ - 1.0) {
						data = row_count_ - 1.0;
					}
					logic_.set_highlighted(data, true);
				}
			}
			highlighted_index_ = data;
			return;
		}
		double data = p_data.to_number();
		const double length = double(data_array_.size());
		if (data_array_.kind != As2Value::Kind::List || length == 0.0 || row_count_ < 1.0) {
			highlighted_index_ = data;
			return;
		}
		if (data < 0.0) {
			highlighted_index_ = -1.0;
			return;
		}
		if (data > row_count_ - 1.0) {
			data = row_count_ - 1.0;
			highlighted_index_ = data;
		}
		double index = highlighted_index_;
		if (data < length && !is_null(p_data) && p_data.kind == As2Value::Kind::Number && data != index &&
				!(data < 0.0)) {
			index = data;
		}
		set_data_field(index, "highlighted", As2Value::of(true));
		if (logic_.active()) {
			logic_.set_highlighted(index, true);
		}
		highlighted_index_ = data;
	}

	void destroy_list() {
		if (ScrollBarClass *s = scrollbar(); s != nullptr && use_scrollbar_ && s->is_showing()) {
			s->set_target_size(0.0);
		}
		row_count_ = 0.0;
		has_active_rows_ = false;
		handle_empty_row(-1.0, true);
		highlighted_index_ = is_null(p_default_highlighted_row_) ? -1.0 : p_default_highlighted_row_.to_number();
		show_spinner(true);
		logic_.destroy_list(highlighted_index_);
	}

	void show_spinner(bool p_show) {
		if (!spinner_enabled_) {
			return;
		}
		const As2Clip self = host_->self();
		if (p_show) {
			As2Clip spinner = spinner_path_.empty() ? As2Clip() : host_->screen().clip(spinner_path_);
			if (!spinner.exists()) {
				spinner = host_->attach_movie(self, "Spinner", "spinner_mc", self.next_highest_depth());
				spinner_path_ = spinner.path();
			} else {
				spinner.set_visible(true);
			}
			spinner.goto_and_play(1);
			spinner.set_x(float(start_w_ * 0.5));
			spinner.set_y(float(start_h_ * 0.5));
			return;
		}
		if (!spinner_path_.empty()) {
			const As2Clip spinner = host_->screen().clip(spinner_path_);
			spinner.stop();
			host_->remove_movie_clip(spinner);
			spinner_path_.clear();
		}
	}

	As2Value highlighted_index_value(double p_index) const {
		if (use_direct_access_) {
			if (!(p_index < row_count_)) {
				return As2Value::of(0);
			}
			return send_index_with_event_.truthy() ? logic_.id_of(p_index) : logic_.label_of(p_index);
		}
		if (!(p_index < double(data_array_.size()))) {
			return As2Value::of(0);
		}
		return data_row(p_index).field(send_index_with_event_.truthy() ? "id" : "Label");
	}

	As2Value highlighted_label_value(double p_index) const {
		if (use_direct_access_) {
			return p_index < row_count_ ? logic_.label_of(p_index) : As2Value::of("");
		}
		return p_index < double(data_array_.size()) ? data_row(p_index).field("Label") : As2Value::of("");
	}

	void shift_highlighted(double p_dir, bool p_keyboard) {
		const double before = highlighted_index_;
		logic_.shift_highlighted(p_dir, p_keyboard);
		if (highlighted_index_ != before) {
			host_->fire_query(kSoundNavigate, As2Value::of(false));
		}
	}

	void highlight_index(double p_index) {
		highlighted_index_ = p_index;
		logic_.set_highlighted(p_index, true);
	}

	void fire_highlight_event(double p_index, bool p_by_user) {
		const As2Value v = highlighted_index_value(p_index);
		if (!p_by_user || row_count_ == 0.0 || as2_loose_equals(v, As2Value::of("LABEL")) ||
				as2_loose_equals(v, As2Value::of(RowBaseClass::kNoId))) {
			return;
		}
		host_->fire_event(kHighlighted, v);
		const As2Value &sub = data_row(highlighted_index_).field("SubIndex");
		if (!is_null(sub)) {
			host_->fire_event(kSelectorChanged, sub);
		}
	}

	void fire_press_event(double p_index, bool p_by_user) {
		if (!p_by_user || row_count_ == 0.0) {
			return;
		}
		host_->fire_event(kPress, highlighted_index_value(p_index));
	}

	void fire_release_event(double p_index, bool p_by_user) {
		if (!p_by_user || row_count_ == 0.0) {
			return;
		}
		host_->fire_query(kSoundSelect, As2Value::of(false));
		host_->fire_event(kReleaseIndex, As2Value::of(p_index));
		host_->fire_event(kRelease, highlighted_index_value(p_index));
	}

	void fire_mouse_over_event(double p_index, bool p_by_user) {
		if (!p_by_user || row_count_ == 0.0) {
			return;
		}
		host_->fire_event(kMouseOver, highlighted_index_value(p_index));
	}

	void fire_menu_hint_event(double p_index, bool p_by_user) {
		if (!p_by_user || row_count_ == 0.0) {
			return;
		}
		if (p_index == -1.0) {
			host_->fire_event(kChangeMenuHint, As2Value::of(""));
			return;
		}
		const std::string label = highlighted_label_value(p_index).to_string();
		host_->fire_event(kChangeMenuHint, As2Value::of(label.starts_with("ID_") ? label + "_HINT" : std::string()));
	}

	void button_row_handler(const As2Value &p_layout) {
		if (row_count_ == 0.0 || is_null(p_layout)) {
			return;
		}
		std::string out;
		for (size_t i = 0; i < p_layout.size(); ++i) {
			const As2Value &b = p_layout.at(i);
			const As2Value &row = b.field("row");
			const As2Value &disabled = b.field("disabled");
			out += b.field("label").to_string() + "," + b.field("btnId").to_string() + "," +
					(is_null(disabled) ? std::string("false") : disabled.to_string()) + "," +
					(is_null(row) ? std::string("0") : row.to_string()) + (i + 1 == p_layout.size() ? "" : ";");
		}
		host_->fire_event(kChangeButtonBar, As2Value::of(out));
	}

	void update_scroll_bar() {
		if (ScrollBarClass *s = scrollbar(); s != nullptr && use_scrollbar_ && s->is_showing()) {
			s->set_percentage(logic_.scroll_percentage(), false);
		}
	}

	void change_focus() {
		if (!has_focus_) {
			host_->request_focus();
		}
	}

	void enter_on_top() {
		if (!has_active_rows_) {
			if (g_loop_id != local_loop_id_) {
				local_loop_id_ = g_loop_id;
				host_->fire_event(kReachedBottom, As2Value::of(g_loop_id));
			} else {
				g_loop_id++;
			}
			return;
		}
		g_loop_id++;
		local_loop_id_ = -1;
		logic_.current_dir = "bottom";
		highlight_index(0.0);
		change_focus();
	}

	void enter_selected(const As2Value &p_param) {
		const std::string wanted = p_param.to_string();
		for (size_t i = 0; i < data_array_.size(); ++i) {
			if (as2_loose_equals(data_array_.items[i].field("SubIndex"), As2Value::of(wanted))) {
				logic_.set_highlighted(double(i), true, false);
				break;
			}
		}
		change_focus();
	}

	void enter_bottom() {
		if (!has_active_rows_) {
			if (g_loop_id != local_loop_id_) {
				local_loop_id_ = g_loop_id;
				host_->fire_event(kReachedTop, As2Value::of(g_loop_id));
			} else {
				g_loop_id++;
			}
			return;
		}
		g_loop_id++;
		local_loop_id_ = -1;
		change_focus();
		logic_.current_dir = "top";
		highlight_index(std::max(0.0, (use_direct_access_ ? row_count_ : double(data_array_.size())) - 1.0));
	}

	HudWidgetHost *host_ = nullptr;
	std::optional<HudListBinding> binding_;
	std::optional<std::vector<As2Value>> direct_rows_;
	ListLogic logic_{ *this };
	As2Clip empty_holder_;
	As2Clip list_holder_;
	std::string mask_path_;
	std::string scrollbar_path_;
	std::string spinner_path_;
	double start_w_ = 0.0;
	double start_h_ = 0.0;
	double selected_index_ = -1.0;
	double highlighted_index_ = -1.0;
	double over_index_ = -1.0;
	double row_count_ = -1.0;
	double empty_row_count_ = 0.0;
	As2Value list_index_ = As2Value::of(0);
	As2Value p_row_type_ = As2Value::of("");
	As2Value p_row_spacing_ = As2Value::of(0);
	As2Value p_default_highlighted_row_ = As2Value::of(0);
	As2Value p_navigation_type_ = As2Value::of("");
	As2Value data_includes_button_layouts_ = As2Value::of(false);
	As2Value send_index_with_event_ = As2Value::of(false);
	As2Value empty_row_type_ = As2Value::of(std::string(kEmptyRowNone));
	As2Value selector_width_ = As2Value::of(200);
	As2Value data_array_;
	std::optional<std::vector<std::string>> disabled_rows_;
	std::string row_type_ = "MixedRows";
	double row_spacing_ = 0.0;
	std::string navigation_type_ = "loop";
	std::string direct_access_data_;
	bool clear_list_at_navigation_event_ = false;
	bool use_scrollbar_ = true;
	bool screen_rotation_enabled_ = false;
	bool use_3d_selection_ = false;
	bool fire_highlight_on_mouse_out_ = false;
	bool spinner_enabled_ = false;
	bool high_light_on_update_ = true;
	bool keep_scroll_offset_ = false;
	bool has_active_rows_ = false;
	bool has_focus_ = false;
	bool allow_key_navigation_ = true;
	bool use_direct_access_ = false;
	bool local_selection_ = false;
	bool row_out_enter_frame_ = false;
	bool focused_before_ = false;
	int32_t local_loop_id_ = -1;

	friend class ListLogic;
};

std::optional<RowRequest> ListClass::update_list_row(double p_index) {
	show_spinner(false);
	if (use_direct_access_) {
		has_active_rows_ = true;
		if (!(p_index < row_count_) || p_index < 0.0) {
			return std::nullopt;
		}
		RowRequest r;
		r.row_mc = row_type_ == "baseRow" ? std::string("basicRow") : row_type_;
		r.index = p_index;
		r.data = As2Value::object();
		r.data.set("Index", As2Value::of(p_index));
		r.data.set("useDirectAccess", As2Value::of(true));
		r.data.set("dataIncludesButtonLayouts", data_includes_button_layouts_);
		r.data.set("sendIdWithEvent", send_index_with_event_);
		if (r.row_mc == "TextSelectorRow") {
			r.data.set("SelectorWidth", selector_width_);
		}
		if (p_index == highlighted_index_) {
			r.data.set("highlighted", As2Value::of(true));
		}
		handle_empty_row(p_index);
		return r;
	}
	const std::optional<int32_t> at = index_of(p_index);
	if (!at || *at < 0 || size_t(*at) >= data_array_.items.size()) {
		return std::nullopt;
	}
	As2Value &row = data_array_.items[size_t(*at)];
	if (row.kind == As2Value::Kind::String) {
		As2Value o = As2Value::object();
		o.set("Index", As2Value::of(p_index));
		o.set("Label", As2Value::of(row.text));
		row = std::move(o);
	}
	if (row.kind != As2Value::Kind::Object) {
		row = As2Value::object();
	}
	std::string type = row_type_ == "baseRow" ? std::string("basicRow") : row_type_;
	if (!is_null(row.field("Type"))) {
		type = row.field("Type").to_string();
	}
	if (is_null(row.field("highlighted"))) {
		row.set("highlighted", As2Value::of(p_index == highlighted_index_));
	}
	if (is_null(row.field("selected"))) {
		row.set("selected", As2Value::of(false));
	}
	const As2Value &disabled = row.field("Disabled");
	if (is_null(disabled) || !disabled.truthy()) {
		row.set("Disabled", As2Value::of(false));
		has_active_rows_ = true;
	}
	if (is_null(row.field("id"))) {
		row.set("id", row.field("Index"));
		row.set("Index", As2Value::of(p_index));
	}
	if (type == "TextSelectorRow") {
		row.set("SelectorWidth", selector_width_);
	}
	if (disabled_rows_) {
		row.set("Disabled", As2Value::of(row_disabled(p_index + 1.0)));
	}
	RowRequest r;
	r.row_mc = type;
	r.index = p_index;
	r.data = row;
	handle_empty_row(p_index);
	return r;
}

void ListClass::list_height_handler(double p_total) {
	if (ScrollBarClass *s = scrollbar()) {
		s->set_target_size(p_total);
	}
	const As2Clip m = mask();
	if (p_total > start_h_) {
		m.set_width(float(start_w_ + 10.0 - 10.0));
		logic_.update_width(m.width() - 10.0);
		list_holder_.set_mask(m);
	} else {
		list_holder_.clear_mask();
		logic_.update_width(m.width() - 10.0);
	}
}

RowBaseClass *ListLogic::controller(const std::string &p_path) const {
	return dynamic_cast<RowBaseClass *>(list_.host_->screen().controller(p_path));
}

const ListLogic::Entry *ListLogic::entry(double p_index) const {
	const std::optional<int32_t> i = index_of(p_index);
	if (!i) {
		return nullptr;
	}
	const auto it = rows_.find(*i);
	return it == rows_.end() ? nullptr : &it->second;
}

RowBaseClass *ListLogic::row(double p_index) const {
	const Entry *e = entry(p_index);
	return e != nullptr ? controller(e->path) : nullptr;
}

As2Clip ListLogic::row_clip(double p_index) const {
	const Entry *e = entry(p_index);
	return e != nullptr ? list_.host_->screen().clip(e->path) : As2Clip();
}

As2Value ListLogic::id_of(double p_index) const {
	const RowBaseClass *r = row(p_index);
	return r != nullptr ? r->id() : As2Value();
}

As2Value ListLogic::label_of(double p_index) const {
	const RowBaseClass *r = row(p_index);
	return r != nullptr ? r->label() : As2Value();
}

bool ListLogic::disabled_of(double p_index) const {
	const RowBaseClass *r = row(p_index);
	return r != nullptr && r->disabled();
}

As2Value ListLogic::type_of(double p_index) const {
	const RowBaseClass *r = row(p_index);
	return r != nullptr ? As2Value::of(r->type()) : As2Value();
}

As2Value ListLogic::current_button_layout() const {
	const RowBaseClass *r = row(current_highlighted_);
	return r != nullptr ? r->button_layouts() : As2Value();
}

bool ListLogic::info_disabled(double p_index) const {
	return list_.info_disabled(p_index);
}

void ListLogic::start(double p_length) {
	active_ = true;
	list_length_ = p_length;
	if (std::isnan(current_top_)) {
		current_top_ = 0.0;
	}
	if (list_length_ != 0.0) {
		handle_row(list_.update_list_row(current_top_));
	}
}

void ListLogic::update_list_length(double p_length) {
	only_do_once_ = true;
	if (list_length_ < p_length) {
		const double from = list_length_;
		list_length_ = p_length;
		handle_row(list_.update_list_row(from));
		return;
	}
	for (double i = p_length; i < list_length_; i += 1.0) {
		const Entry *e = entry(i);
		if (e == nullptr) {
			break;
		}
		cache_object(*e);
	}
	if (list_length_ == p_length) {
		return;
	}
	list_length_ = p_length;
	total_list_height_ = row_height_ * list_length_ - row_spacing_;
	list_.list_height_handler(total_list_height_);
}

void ListLogic::handle_row(std::optional<RowRequest> p_request) {
	if (!p_request) {
		return;
	}
	const std::string type = p_request->row_mc;
	const std::string path = row_movie_clip(type);
	if (const std::optional<int32_t> i = index_of(p_request->index)) {
		rows_[*i] = Entry{ type, path };
	}
	RowBaseClass *r = controller(path);
	const As2Clip clip = list_.host_->screen().clip(path);
	if (r != nullptr) {
		r->set_type(type);
		r->set_row_index(p_request->data.field("Index").to_number());
		r->set_width(list_width_);
		r->set_list(list_.host_->path());
		if (list_.info_selector(p_request->index)) {
			r->set_selector_callback(true);
		}
		if (navigation_type == "none") {
			r->set_mouse_enabled(false);
		}
	}
	if (only_do_once_) {
		row_height_ = (r != nullptr ? r->height() : as2_round(clip.height())) + row_spacing_;
		total_list_height_ = row_height_ * list_length_ - row_spacing_;
		list_.list_height_handler(total_list_height_);
		top_offset_ = 0.0;
		row_holder_.set_y(float(top_offset_));
		only_do_once_ = false;
	}
	if (r != nullptr) {
		r->set_use_3d(use_3d_ && !win32_);
		r->set_data(p_request->data);
	}
	top_and_bottom_index();
	clip.set_y(float(row_height_ * p_request->index));
	for (double i = current_top_; i < current_bottom_ + 1.0; i += 1.0) {
		if (entry(i) == nullptr) {
			handle_row(list_.update_list_row(i));
			return;
		}
	}
}

void ListLogic::update_width(double p_width) {
	list_width_ = p_width;
	for (const auto &[index, e] : rows_) {
		if (RowBaseClass *r = controller(e.path)) {
			r->set_width(list_width_);
		}
	}
}

std::string ListLogic::row_movie_clip(const std::string &p_type) {
	const auto it = cache_.find(p_type);
	if (it != cache_.end() && !it->second.empty()) {
		const Entry e = it->second.back();
		it->second.pop_back();
		list_.host_->screen().clip(e.path).set_visible(true);
		return e.path;
	}
	const int32_t depth = row_holder_.next_highest_depth();
	return list_.host_->attach_movie(row_holder_, p_type, "row_" + p_type + "_" + std::to_string(depth), depth).path();
}

void ListLogic::handle_cache(std::string_view p_dir, bool p_force_cleanup) {
	if (p_dir == "top") {
		for (double i = current_top_ - 1.0; i > -1.0; i -= 1.0) {
			if (const Entry *e = entry(i)) {
				cache_object(*e);
			} else if (!p_force_cleanup) {
				return;
			}
		}
		return;
	}
	for (double i = current_bottom_ + 1.0; i < list_length_; i += 1.0) {
		if (const Entry *e = entry(i)) {
			cache_object(*e);
		} else if (!p_force_cleanup) {
			return;
		}
	}
}

void ListLogic::cache_object(Entry p_entry) {
	RowBaseClass *r = controller(p_entry.path);
	const double index = r != nullptr ? r->row_index() : std::nan("");
	clear_cache(p_entry.row_type);
	if (r != nullptr) {
		r->reset(false);
	}
	const As2Clip clip = list_.host_->screen().clip(p_entry.path);
	const double h = r != nullptr ? r->height() : as2_round(clip.height());
	cache_[p_entry.row_type].push_back(p_entry);
	clip.set_y(float(-(h + 10.0)));
	clip.set_visible(false);
	if (const std::optional<int32_t> i = index_of(index)) {
		rows_.erase(*i);
	}
}

void ListLogic::clear_cache(const std::string &p_type) {
	const auto it = cache_.find(p_type);
	if (it != cache_.end()) {
		const std::vector<Entry> cached = it->second;
		for (const Entry &e : cached) {
			if (RowBaseClass *r = controller(e.path)) {
				r->reset(true);
			}
			list_.host_->remove_movie_clip(list_.host_->screen().clip(e.path));
		}
	}
	cache_[p_type].clear();
}

void ListLogic::row_released(double p_index, bool p_value) {
	list_.request_focus_from_row();
	set_highlighted(p_index, p_value);
	list_.on_row_released(p_index, p_value);
}

void ListLogic::row_pressed(double p_index) {
	list_.on_row_pressed(p_index);
}

void ListLogic::mouse_over_row(double p_index, bool p_value) {
	list_.on_row_mouse_over(p_index, p_value);
}

void ListLogic::row_highlighted(double p_index, bool p_value) {
	if (p_value) {
		current_highlighted_ = p_index;
	}
	list_.on_row_highlighted(p_index, p_value);
}

void ListLogic::row_selector_changed(double p_index, const As2Value &p_arg) {
	list_.selector_handler(p_index, p_arg);
}

void ListLogic::shift_selector(double p_dir, const As2Value &p_used_mouse) {
	if (RowBaseClass *r = row(current_highlighted_)) {
		r->step_selection(p_dir, p_used_mouse);
	}
}

As2Value ListLogic::current_selector_value() const {
	const RowBaseClass *r = row(current_highlighted_);
	return r != nullptr ? r->selector_value() : As2Value();
}

void ListLogic::highlight_logic(double p_index, bool p_value) {
	RowBaseClass *r = row(p_index);
	if (r == nullptr) {
		current_highlighted_ = p_index;
		list_.on_row_highlighted(p_index, p_value);
		return;
	}
	r->set_highlighted(p_value);
}

void ListLogic::move_to_index(double p_index, std::string_view p_dir) {
	double perc = std::nan("");
	if (p_dir == "normal" || p_dir.empty()) {
		perc = p_index / (list_length_ - 1.0) * 100.0;
	} else if (p_dir == "top") {
		perc = row_height_ * p_index / (total_list_height_ - list_height_) * 100.0;
	} else if (p_dir == "bottom") {
		perc = (row_height_ * p_index - (list_height_ - row_height_) - row_spacing_) /
				(total_list_height_ - list_height_) * 100.0;
	}
	move_to_percentage(perc);
}

void ListLogic::move_to_percentage(double p_percentage) {
	double perc = p_percentage;
	if (perc > 100.0) {
		perc = 100.0;
	} else if (perc < 0.0) {
		perc = 0.0;
	}
	current_scroll_percentage_ = perc * 0.01;
	list_pos_ = as2_round(-((total_list_height_ - list_height_) * current_scroll_percentage_));
	top_and_bottom_index();
	row_handler(false);
	const double offset = current_scroll_percentage_ == 0.0 ? top_offset_ : 0.0;
	row_holder_.set_y(float(list_pos_ + offset));
}

void ListLogic::top_and_bottom_index() {
	current_top_ = std::floor(std::abs(list_pos_) / (total_list_height_ - top_offset_) * (list_length_ * 0.01) * 100.0);
	const double bottom = std::floor(
			(list_height_ + std::abs(list_pos_)) / (total_list_height_ + top_offset_) * list_length_ * 0.01 * 100.0);
	current_bottom_ = bottom < list_length_ ? bottom : list_length_ - 1.0;
}

void ListLogic::row_handler(bool p_force_cleanup) {
	handle_cache("top", p_force_cleanup);
	handle_cache("bottom", p_force_cleanup);
	if (entry(current_top_) == nullptr) {
		handle_row(list_.update_list_row(current_top_));
	}
	if (entry(current_bottom_) == nullptr) {
		handle_row(list_.update_list_row(current_bottom_));
	}
}

void ListLogic::shift_highlighted(double p_dir, bool p_keyboard) {
	highlight_row(current_highlighted_, false, false, false);
	if (std::isnan(current_highlighted_)) {
		current_highlighted_ = -1.0;
	}
	highlight_row(current_highlighted_ + p_dir, true, false, p_keyboard);
}

void ListLogic::set_highlighted(double p_index, bool p_value, bool p_mouse_used) {
	highlight_row(current_highlighted_, false, p_mouse_used, false);
	highlight_row(p_index, p_value, p_mouse_used, false);
	row_handler(true);
}

void ListLogic::select_current_highlighted() {
	if (std::isnan(current_highlighted_)) {
		return;
	}
	list_.on_row_released(current_highlighted_, true);
}

void ListLogic::highlight_current_selected() {
	if (std::isnan(current_highlighted_)) {
		return;
	}
	highlight_row(current_highlighted_, true, false, false);
}

void ListLogic::highlight_row(double p_index, bool p_value, bool p_mouse_used, bool p_keyboard_used) {
	bool no_move = false;
	const double original = p_index;
	double index = p_index;
	bool value = p_value;
	std::string dir = "normal";
	bool send_event = false;
	int32_t event = 0;
	std::optional<double> highlighted_after;
	if (value && !std::isnan(index) && info_disabled(index)) {
		if (current_dir == "normal") {
			if (index > current_highlighted_) {
				dir = "bottom";
			} else if (index < current_highlighted_) {
				dir = "top";
			}
		} else {
			dir = current_dir;
		}
		index = next_enabled_row(index, dir);
		if (dir == "bottom" && original > index) {
			if (total_list_height_ > list_height_) {
				move_to_index(list_length_ - 1.0, dir);
			}
			if (p_keyboard_used && (navigation_type == "sendEventBottom" || navigation_type == "sendEventTopBottom")) {
				send_event = true;
				event = reached_bottom_event;
			}
		} else if (dir == "top" && original < index) {
			move_to_index(0.0, dir);
			if (p_keyboard_used && (navigation_type == "sendEventTop" || navigation_type == "sendEventTopBottom")) {
				send_event = true;
				event = reached_top_event;
			}
		}
	}
	if (index < 0.0) {
		if (navigation_type == "loop") {
			index = next_enabled_row(list_length_ - 1.0, "top");
		} else if (navigation_type == "sendEventTop" || navigation_type == "sendEventTopBottom") {
			no_move = true;
			index = 0.0;
			value = false;
			highlighted_after = -1.0;
			if (p_keyboard_used) {
				send_event = true;
				event = reached_top_event;
			}
		} else {
			no_move = true;
			index = 0.0;
		}
	} else if (index > list_length_ - 1.0) {
		if (navigation_type == "loop") {
			index = next_enabled_row(0.0, "bottom");
		} else if (navigation_type == "sendEventBottom" || navigation_type == "sendEventTopBottom") {
			no_move = true;
			index = list_length_ - 1.0;
			value = false;
			highlighted_after = list_length_;
			if (p_keyboard_used) {
				send_event = true;
				event = reached_bottom_event;
			}
		} else {
			no_move = true;
			index = list_length_ - 1.0;
		}
	} else if (std::isnan(index)) {
		return;
	}
	if (index > current_top_ && index < current_bottom_) {
		no_move = true;
	} else if (!(index > current_top_)) {
		dir = "top";
	} else if (!(index < current_bottom_)) {
		dir = "bottom";
		if (total_list_height_ < list_height_) {
			no_move = true;
		}
	}
	if (p_mouse_used) {
		no_move = true;
	}
	if (value && !no_move) {
		move_to_index(index, dir);
	}
	if (dir == "top") {
		if (info_disabled(index - 1.0) && all_disabled_from_now_on(index - 1.0, dir) &&
				total_list_height_ > list_height_) {
			move_to_index(0.0, dir);
		}
	} else if (dir == "bottom") {
		if (info_disabled(index + 1.0) && all_disabled_from_now_on(index + 1.0, dir) &&
				total_list_height_ > list_height_) {
			move_to_index(list_length_ - 1.0, dir);
		}
	}
	highlight_logic(index, value);
	if (highlighted_after) {
		current_highlighted_ = *highlighted_after;
	}
	if (send_event) {
		list_.navigate_event(event);
	}
}

bool ListLogic::all_disabled_from_now_on(double p_index, std::string_view p_dir) const {
	double found = p_index;
	if (p_dir == "bottom") {
		for (double i = p_index; i < list_length_; i += 1.0) {
			if (!info_disabled(i)) {
				found = i;
				break;
			}
		}
	} else if (p_dir == "top") {
		for (double i = p_index; i > -1.0; i -= 1.0) {
			if (!info_disabled(i)) {
				found = i;
				break;
			}
		}
	}
	return p_index == found;
}

double ListLogic::next_enabled_row(double p_index, std::string_view p_dir) const {
	double out = current_highlighted_;
	if (p_dir == "bottom") {
		for (double i = p_index; i < list_length_; i += 1.0) {
			if (!info_disabled(i)) {
				out = i;
				break;
			}
		}
		if (out == current_highlighted_ && navigation_type == "loop") {
			for (double i = 0.0; i < list_length_; i += 1.0) {
				if (!info_disabled(i)) {
					out = i;
					break;
				}
			}
		}
	} else if (p_dir == "top") {
		for (double i = p_index; i > -1.0; i -= 1.0) {
			if (!info_disabled(i)) {
				out = i;
				break;
			}
		}
		if (navigation_type == "loop" && out == current_highlighted_) {
			for (double i = list_length_ - 1.0; i > -1.0; i -= 1.0) {
				if (!info_disabled(i)) {
					out = i;
					break;
				}
			}
		}
	}
	return out;
}

void ListLogic::destroy_list(double p_highlighted) {
	const std::map<int32_t, Entry> rows = rows_;
	for (const auto &[index, e] : rows) {
		if (RowBaseClass *r = controller(e.path)) {
			r->reset(true);
		}
		list_.host_->remove_movie_clip(list_.host_->screen().clip(e.path));
	}
	std::vector<std::string> types;
	for (const auto &[type, entries] : cache_) {
		types.push_back(type);
	}
	for (const std::string &t : types) {
		clear_cache(t);
	}
	rows_.clear();
	cache_.clear();
	current_highlighted_ = p_highlighted;
	current_top_ = 0.0;
	current_bottom_ = 0.0;
	current_scroll_percentage_ = 0.0;
	list_pos_ = 0.0;
	row_holder_.set_y(0.0f);
	only_do_once_ = true;
	list_length_ = 0.0;
}

}

void RowBaseClass::construct(HudWidgetHost &r_host) {
	host_ = &r_host;
	const As2Clip size = r_host.clip("size_mc");
	if (size.exists()) {
		size.set_visible(false);
	}
	const As2Clip plus = r_host.clip("plus");
	if (plus.exists()) {
		plus.set_visible(false);
	}
}

void RowBaseClass::set_data(const As2Value &p_data) {
	data_ = p_data;
	check_states(p_data);
	check_for_new_content();
}

void RowBaseClass::check_states(const As2Value &p_data) {
	if (!is_null(p_data.field("ButtonLayout"))) {
		button_layouts_ = p_data.field("ButtonLayout");
	}
	set_highlighted(p_data.field("highlighted").truthy());
	set_disabled(p_data.field("Disabled").truthy());
}

void RowBaseClass::check_for_new_content() const {
	const As2Clip plus = host_->clip("plus");
	if (plus.exists()) {
		const As2Value &n = data_.field("NewContent");
		plus.set_visible(n.kind == As2Value::Kind::String && n.text == "true");
	}
}

void RowBaseClass::state_changed() {
	std::string s;
	if (disabled_) {
		if (use_mouse_) {
			turn_off_mouse_interaction();
		}
		s = "inactive";
	} else {
		if (use_mouse_ && !has_mouse_listeners_) {
			setup_mouse_interaction();
		}
		if (highlighted_ && unfocus_mode_ == "normal") {
			s = pressed_ ? "pressSelected" : over_ ? "overSelected" : "selected";
		} else if (over_) {
			s = "over";
		} else {
			s = unfocus_mode_ == "ChangeState" ? "over" : "out";
		}
	}
	state_ = s;
	host_->self().goto_and_stop(std::string_view(s));
	set_bg_state(s);
	const std::vector<std::string> listeners = state_listeners_;
	for (const std::string &path : listeners) {
		if (path == host_->path()) {
			on_state(s);
		} else if (RowStateListener *l = dynamic_cast<RowStateListener *>(host_->screen().controller(path))) {
			l->row_state(s);
		}
	}
	handle_glow_size();
}

void RowBaseClass::set_bg_state(const std::string &p_state) const {
	for (const std::string_view name : { "bg", "bg1", "bg2", "bg3", "bg4", "bg5", "bg6" }) {
		const As2Clip bg = host_->clip(name);
		if (bg.exists()) {
			bg.goto_and_stop(std::string_view(p_state));
		}
	}
}

ListOwner *RowBaseClass::owner() const {
	return list_path_.empty() ? nullptr : dynamic_cast<ListOwner *>(host_->screen().controller(list_path_));
}

void RowBaseClass::set_highlighted(bool p_highlighted) {
	if (disabled_) {
		return;
	}
	highlighted_ = p_highlighted;
	state_changed();
	ListOwner *o = owner();
	if (o == nullptr) {
		return;
	}
	if (use_direct_access_ && as2_loose_equals(id_, As2Value::of(kNoId))) {
		return;
	}
	o->row_highlighted(index_, highlighted_);
}

void RowBaseClass::set_mouse_over_row(bool p_over) {
	if (over_ == p_over || disabled_) {
		return;
	}
	over_ = p_over;
	state_changed();
	if (ListOwner *o = owner()) {
		o->row_mouse_over(index_, over_);
	}
}

void RowBaseClass::set_disabled(bool p_disabled) {
	if (disabled_ == p_disabled) {
		return;
	}
	disabled_ = p_disabled;
	state_changed();
}

void RowBaseClass::setup_mouse_interaction() {
	if (has_mouse_listeners_) {
		return;
	}
	host_->set_mouse_target(host_->self(), true);
	has_mouse_listeners_ = true;
}

void RowBaseClass::turn_off_mouse_interaction() {
	if (!has_mouse_listeners_) {
		return;
	}
	host_->set_mouse_target(host_->self(), false);
	has_mouse_listeners_ = false;
}

void RowBaseClass::mouse_over() {
	set_mouse_over_row(true);
}

void RowBaseClass::mouse_out() {
	set_mouse_over_row(false);
}

void RowBaseClass::mouse_pressed() {
	if (disabled_) {
		return;
	}
	if (ListOwner *o = owner()) {
		o->row_pressed(index_);
	}
	pressed_ = true;
	state_changed();
}

void RowBaseClass::mouse_released() {
	if (disabled_) {
		return;
	}
	if (ListOwner *o = owner()) {
		o->row_released(index_, true);
	}
	pressed_ = false;
	state_changed();
}

void RowBaseClass::mouse_release_outside() {
	set_mouse_over_row(false);
	pressed_ = false;
	state_changed();
}

void RowBaseClass::selector_changed(const As2Value &p_arg) {
	if (!selector_callback_) {
		return;
	}
	if (ListOwner *o = owner()) {
		o->row_selector_changed(index_, p_arg);
	}
}

bool RowBaseClass::mouse_move(HudWidgetHost &r_host, const HudMouse &p_mouse) {
	if (p_mouse.target != r_host.path() || !has_mouse_listeners_) {
		return false;
	}
	if (p_mouse.kind == HudMouseKind::RollOver) {
		mouse_over();
		return true;
	}
	if (p_mouse.kind == HudMouseKind::RollOut) {
		mouse_out();
		return true;
	}
	return false;
}

bool RowBaseClass::mouse_press(HudWidgetHost &r_host, const HudMouse &p_mouse) {
	if (p_mouse.target != r_host.path() || !has_mouse_listeners_ || disabled_) {
		return false;
	}
	mouse_pressed();
	return true;
}

bool RowBaseClass::mouse_release(HudWidgetHost &r_host, const HudMouse &p_mouse) {
	if (p_mouse.target != r_host.path() || !has_mouse_listeners_) {
		return false;
	}
	if (p_mouse.kind == HudMouseKind::ReleaseOutside) {
		mouse_release_outside();
		return true;
	}
	if (disabled_) {
		return false;
	}
	mouse_released();
	return true;
}

void RowBaseClass::handle_glow_size() const {
	const As2Clip glow = host_->clip("glow_mc");
	if (!glow.exists()) {
		return;
	}
	const As2Clip size = host_->clip("size_mc");
	glow.set_width(size.width() + 8.0f);
	glow.set_height(size.height() + 8.0f);
}

void RowBaseClass::set_width(double p_width) {
	const float w = float(as2_round(p_width));
	const As2Clip size = host_->clip("size_mc");
	if (size.exists()) {
		size.set_width(w);
	}
	const As2Clip bg = host_->clip("bg");
	if (bg.exists()) {
		bg.set_width(w - bg.x());
		if (bg.x() == -1.0f) {
			bg.set_width(bg.width() + 1.0f);
		}
	}
	const As2Clip glow = host_->clip("glow_mc");
	if (glow.exists()) {
		glow.set_width(w + 8.0f);
	}
}

double RowBaseClass::width() const {
	const As2Clip size = host_->clip("size_mc");
	return as2_round(size.exists() ? size.width() : host_->self().width());
}

double RowBaseClass::height() const {
	const As2Clip size = host_->clip("size_mc");
	return as2_round(size.exists() ? size.height() : host_->self().height());
}

void RowBaseClass::set_mouse_enabled(bool p_enabled) {
	use_mouse_ = p_enabled;
	if (use_mouse_) {
		setup_mouse_interaction();
	} else {
		turn_off_mouse_interaction();
	}
}

void RowBaseClass::set_id(const As2Value &p_id) {
	if (as2_loose_equals(p_id, id_)) {
		return;
	}
	id_ = p_id;
	if (!highlighted_) {
		return;
	}
	if (ListOwner *o = owner()) {
		o->row_highlighted(index_, highlighted_);
	}
}

void RowBaseClass::register_state_callback(std::string p_listener_path) {
	state_listeners_.push_back(std::move(p_listener_path));
}

void RowBaseClass::reset(bool) {
	if (use_direct_access_) {
		if (ListOwner *o = owner()) {
			o->unregister_access_data(direct_access_ids_);
		}
		direct_access_ids_.clear();
	}
	turn_off_mouse_interaction();
	highlighted_ = false;
	disabled_ = false;
	over_ = false;
	id_ = As2Value::of(kNoId);
	state_changed();
}

void RowBaseClass::register_direct_access_array(std::vector<ListAccessItem> p_items) {
	use_direct_access_ = true;
	p_items.push_back(ListAccessItem{ "Index", "ID", UiDirectAccessOp::Member, host_->path() });
	const As2Clip plus = host_->clip("plus");
	if (plus.exists()) {
		p_items.push_back(ListAccessItem{ "NewContent", "", UiDirectAccessOp::Visible, plus.path() });
	}
	if (data_.field("dataIncludesButtonLayouts").truthy()) {
		p_items.push_back(ListAccessItem{ "ButtonLayout", "buttonLayouts", UiDirectAccessOp::Member, host_->path() });
	}
	if (ListOwner *o = owner()) {
		direct_access_ids_ = o->register_access_data(p_items, index_);
	}
}

void RowBaseClass::direct_access(HudWidgetHost &, const UiDirectAccessApply &p_apply) {
	if (p_apply.op == UiDirectAccessOp::Member) {
		member(p_apply.name, p_apply.value);
	}
}

void RowBaseClass::member(std::string_view p_name, const As2Value &p_value) {
	if (p_name == "ID") {
		set_id(p_value);
	} else if (p_name == "buttonLayouts") {
		button_layouts_ = p_value;
	}
}

std::string hud_list_access_handle(const HudDataSource &p_query) {
	return "ListAccess:" + p_query.category + ":" + std::to_string(p_query.key);
}

bool hud_list_set_rows(HudScreen &r_screen, std::string_view p_instance, const As2Value &p_rows) {
	ListClass *l = dynamic_cast<ListClass *>(
			r_screen.controller(std::string(kHudScreenInstance) + "." + std::string(p_instance)));
	if (l == nullptr) {
		return false;
	}
	l->update_list_rows(p_rows);
	return true;
}

bool hud_list_set_direct_rows(HudScreen &r_screen, std::string_view p_instance, std::vector<As2Value> p_rows) {
	const HudWidgetNode *n = r_screen.asset().find(p_instance);
	if (n == nullptr || !n->list || !n->list->list_query.use_direct_access) {
		return false;
	}
	r_screen.direct_access().set_array(hud_list_access_handle(n->list->list_query), std::move(p_rows));
	return true;
}

void register_hud_widgets_list(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.List.List", [] { return std::make_unique<ListClass>(); });
	r_registry.add("Widget.List.Rows.RowBase", [] { return std::make_unique<RowBaseClass>(); });
}

}
