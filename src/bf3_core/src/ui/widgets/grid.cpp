#include "deploy_widgets.h"
#include "image_manager.h"
#include "kit_icons.h"
#include "scroll_bar.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace bf3::core::ui {

namespace {

constexpr std::string_view kMouseOut = "out";
constexpr std::string_view kMouseOver = "over";
constexpr std::string_view kMousePress = "press";
constexpr std::string_view kMouseRelease = "release";

class GridCell : public HudWidgetController {
public:
	using StateFunction = std::function<void(std::string_view p_state, int32_t p_index)>;

	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		if (hud_platform_win32(r_host.client())) {
			const As2Clip bg = r_host.clip("bg");
			target_ = bg.exists() ? bg : r_host.self();
			r_host.set_mouse_target(target_, true);
		}
	}

	bool mouse_move(HudWidgetHost &, const HudMouse &p_mouse) override {
		if (p_mouse.target != target_.path()) {
			return false;
		}
		if (p_mouse.kind == HudMouseKind::RollOver) {
			set_mouse_state(kMouseOver);
			return true;
		}
		if (p_mouse.kind == HudMouseKind::RollOut) {
			set_mouse_state(kMouseOut);
			return true;
		}
		return false;
	}

	bool mouse_press(HudWidgetHost &, const HudMouse &p_mouse) override {
		if (p_mouse.target != target_.path()) {
			return false;
		}
		set_mouse_state(kMousePress);
		return true;
	}

	bool mouse_release(HudWidgetHost &, const HudMouse &p_mouse) override {
		if (p_mouse.target != target_.path()) {
			return false;
		}
		set_mouse_state(p_mouse.kind == HudMouseKind::Release ? kMouseRelease : kMouseOut);
		return true;
	}

	virtual void set_data(const As2Value &p_data, int32_t p_index) {
		data_ = p_data;
		index_ = p_index;
	}
	virtual float cell_width() const { return host_->self().width(); }
	virtual float cell_height() const { return host_->self().height(); }
	virtual void set_selected(bool p_selected) { selected_ = p_selected; }
	virtual bool is_active() const { return active_; }
	bool selected() const { return selected_; }
	void set_state_function(StateFunction p_function) { state_function_ = std::move(p_function); }

protected:
	virtual void set_mouse_state(std::string_view p_state) {
		if (state_function_) {
			state_function_(p_state, index_);
		}
	}

	static uint32_t state_frame(std::string_view p_state) {
		if (p_state == kMouseOut) {
			return 1;
		}
		return p_state == kMousePress ? 3 : 2;
	}

	HudWidgetHost *host_ = nullptr;
	As2Clip target_;
	As2Value data_;
	int32_t index_ = -1;
	bool selected_ = false;
	bool active_ = false;
	StateFunction state_function_;
};

class KitCellClass final : public GridCell {
public:
	static constexpr float kTargetWidth = 113.0f;
	static constexpr float kTargetHeight = 47.0f;

	void construct(HudWidgetHost &r_host) override {
		GridCell::construct(r_host);
		const As2Clip highlight = r_host.clip("highlight");
		highlight.set_width(kTargetWidth + 1.0f);
		highlight.set_height(kTargetHeight + 1.0f);
		highlight.set_visible(false);
		r_host.clip("image").set_visible(false);
	}

	void set_data(const As2Value &p_data, int32_t p_index) override {
		GridCell::set_data(p_data, p_index);
		host_->set_enter_frame(true);
	}

	void enter_frame(HudWidgetHost &r_host) override {
		r_host.set_enter_frame(false);
		load_content(r_host);
	}

	float cell_width() const override { return kTargetWidth; }
	float cell_height() const override { return kTargetHeight; }
	bool is_active() const override { return true; }

	void set_selected(bool p_selected) override {
		selected_ = p_selected;
		host_->clip("highlight").set_visible(p_selected);
	}

private:
	void set_mouse_state(std::string_view p_state) override {
		host_->self().goto_and_stop(state_frame(p_state));
		GridCell::set_mouse_state(p_state);
	}

	void load_content(HudWidgetHost &r_host) {
		r_host.clip("label.txt").set_text(data_.field("Label").to_string());
		const As2Value &path = data_.field("ImagePath");
		if (!game_utils_is_image_path(path)) {
			return;
		}
		const As2Clip image = r_host.clip("image");
		ImageManagerWidget *m = controller_at<ImageManagerWidget>(r_host, image);
		HudWidgetHost *image_host = r_host.screen().host(image.path());
		if (m != nullptr && image_host != nullptr) {
			m->update_image_url(*image_host, path.to_string());
		}
		image.set_visible(true);
	}
};

class TextCellClass final : public GridCell {
public:
	void construct(HudWidgetHost &r_host) override {
		GridCell::construct(r_host);
		r_host.clip("highlight_mc").set_visible(false);
	}

	void set_data(const As2Value &p_data, int32_t p_index) override {
		GridCell::set_data(p_data, p_index);
		host_->clip("textField").set_text(p_data.field("label").to_string());
	}

	float cell_width() const override { return host_->self().width() - 1.0f; }
	float cell_height() const override { return host_->self().height() - 1.0f; }

	void set_selected(bool p_selected) override {
		selected_ = p_selected;
		host_->clip("highlight_mc").set_visible(p_selected);
	}

private:
	void set_mouse_state(std::string_view p_state) override {
		host_->self().goto_and_stop(state_frame(p_state));
		GridCell::set_mouse_state(p_state);
	}
};

class GridClass final : public HudWidgetController, public HudGrid {
public:
	static constexpr int32_t kOver = 0;
	static constexpr int32_t kHighlighted = 1;
	static constexpr int32_t kPressed = 2;
	static constexpr int32_t kReleased = 3;
	static constexpr int32_t kForwardShow = 4;
	static constexpr int32_t kForwardHide = 5;
	static constexpr int32_t kChangeButtonBar = 6;
	static constexpr int32_t kCellActiveChanged = 7;
	static constexpr int32_t kShow = 10;
	static constexpr int32_t kHide = 11;
	static constexpr std::string_view kButtonBarWithSelect =
			"ID_M_BACK,IDB_Rright,false,0;ID_M_SELECT,IDB_Rdown,false,0;";
	static constexpr std::string_view kButtonBarNoSelect = "ID_M_BACK,IDB_Rright,false,0;";

	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		const As2Clip self = r_host.self();
		r_host.create_empty_movie_clip(self, "gridItems", self.next_highest_depth());
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		const As2Clip self = r_host.self();
		grid_width_ = float(as2_round(self.width()));
		grid_height_ = float(as2_round(self.height()));
		self.set_xscale(100.0f);
		self.set_yscale(100.0f);
		const As2Clip temp = r_host.clip("tmpSize");
		temp.set_visible(false);
		temp.set_width(grid_width_);
		temp.set_height(grid_height_);
		r_host.clip("backplate").set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		const std::string *cell_type = p_init.property("p_cellType");
		cell_type_ = cell_type == nullptr || cell_type->empty() ? std::string("TextCell") : *cell_type;
		const double index = param_number(p_init, "p_selectedIndex", 0.0);
		selected_ = int32_t(index);
		keyboard_navigation_ = param_number(p_init, "p_keyboardNavigation", 1.0) != 0.0;
		highlight_inactive_ = param_number(p_init, "p_highlightInactive", 0.0) != 0.0;
		item_picker_ = param_number(p_init, "p_itemPicker", 0.0) != 0.0;
		anim_time_ = param_number(p_init, "p_animTime", 1.0);
		alpha_min_ = float(param_number(p_init, "p_itemPickerAlphaMin", 0.0));
		alpha_max_ = float(param_number(p_init, "p_itemPickerAlphaMax", 100.0));
		if (item_picker_) {
			const As2Clip self = r_host.self();
			self.set_y(self.y() - 2.0f);
			grid_width_ += 1.0f;
			grid_height_ += 1.0f;
			const As2Clip temp = r_host.clip("tmpSize");
			temp.set_height(temp.height() + 1.0f);
			const As2Clip backplate = r_host.clip("backplate");
			backplate.set_alpha(alpha_min_);
			grid_clips().set_alpha(alpha_min_);
			backplate.set_y(grid_height_);
			grid_clips().set_y(grid_height_);
			const As2Clip holder = r_host.clip("itemPickerItemsHolder");
			const As2Clip items =
					r_host.attach_movie(holder, "ItemPickerItems", "itemPickerItems", holder.next_highest_depth());
			items.set_y(grid_height_ - 3.0f);
			if (hud_platform_win32(r_host.client())) {
				for (const char *n : { "gridHeaderTextConsole1", "gridHeaderTextConsole2", "gridHeaderBtnConsole1",
							 "gridHeaderBtnConsole2" }) {
					items.child(n).set_visible(false);
				}
			} else {
				items.child("gridHeaderTextPC").set_visible(false);
			}
		}
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "GridItems") {
			update_grid_items(p_value);
		} else if (p_key == "selectedIndex") {
			const double n = p_value.to_number();
			if (!std::isnan(n)) {
				set_selected_index(int32_t(n));
			}
			current_height_ = 0.0f;
			draw_grid();
		}
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		const HudCustomization &c = p_model.customization;
		const size_t slot = size_t(std::clamp<int32_t>(c.selected_slot, 0, int32_t(kHudSpawnKitItems) - 1));
		const HudItemList &list = c.kit_grid_data[slot];
		As2Value items = As2Value::array();
		for (size_t i = 0; i < list.count; ++i) {
			const HudItem &item = list.items[i];
			As2Value v = As2Value::object();
			v.set("Label",
					As2Value::of(item.name_sid_hash != 0 && p_client.localise_hash
									? p_client.localise_hash(item.name_sid_hash)
									: std::string()));
			if (const std::string image = hud_string(item.image_path); !image.empty()) {
				v.set("ImagePath", As2Value::of(image));
			}
			v.set("ItemIndex", As2Value::of(int32_t(i)));
			items.push(std::move(v));
		}
		r_out.push_back({ "GridItems", std::move(items) });
		r_out.push_back({ "selectedIndex", As2Value::of(int32_t(c.kit_grid_selected[slot])) });
	}

	void in_event(HudWidgetHost &, int32_t p_index, const As2Value &) override {
		if (p_index == kShow) {
			on_show();
		} else if (p_index == kHide) {
			on_hide();
		}
	}

	bool input_concept(HudWidgetHost &, int32_t p_concept, bool p_pressed) override {
		const bool navigate = p_concept == int32_t(UiInputConcept::NavigateLeft) ||
				p_concept == int32_t(UiInputConcept::NavigateRight) ||
				p_concept == int32_t(UiInputConcept::NavigateUp) || p_concept == int32_t(UiInputConcept::NavigateDown);
		if (p_pressed) {
			if (navigate && keyboard_navigation_) {
				move_selection(UiInputConcept(p_concept));
			} else if (p_concept == int32_t(UiInputConcept::Activate)) {
				release_locked_ = false;
			}
			return false;
		}
		if (p_concept == int32_t(UiInputConcept::Activate) && keyboard_navigation_ && !release_locked_ &&
				item_picker_) {
			if (GridCell *c = cell(selected_); c != nullptr && c->is_active()) {
				host_->fire_query(kSoundSelect, As2Value::of(false));
				host_->fire_event(kReleased, As2Value::of(selected_));
			}
		}
		return false;
	}

	void tween_complete(HudWidgetHost &, uint32_t p_id) override {
		if (p_id == kAnimationDone) {
			on_animation_complete();
		}
	}

	int32_t selected_index() const override { return selected_; }
	size_t cell_count() const override { return items_.size(); }
	int32_t columns() const override { return columns_; }
	bool closing() const override { return closing_; }
	std::string cell_name(size_t p_index) const override {
		return "cell-" + std::to_string(p_index) + "-s-" + std::to_string(cell_name_start_);
	}

private:
	static constexpr uint32_t kAnimationDone = 1;
	static constexpr float kBackplateHeaderHeight = 32.0f;

	static double param_number(const HudWidgetInit &p_init, std::string_view p_name, double p_default) {
		const std::string *v = p_init.property(p_name);
		if (v == nullptr || v->empty()) {
			return p_default;
		}
		const double n = As2Value::of(*v).to_number();
		return std::isnan(n) ? p_default : n;
	}

	As2Clip grid_clips() const { return host_->clip("gridItems"); }
	As2Clip backplate() const { return host_->clip("backplate"); }
	As2Clip cell_clip(int32_t p_index) const { return host_->clip("gridItems." + cell_name(size_t(p_index))); }
	GridCell *cell(int32_t p_index) const {
		if (p_index < 0 || size_t(p_index) >= items_.size()) {
			return nullptr;
		}
		return controller_at<GridCell>(*host_, cell_clip(p_index));
	}
	ScrollBarClass *scrollbar() const { return controller_at<ScrollBarClass>(*host_, host_->clip("ScrollBar")); }

	void update_grid_items(const As2Value &p_items) {
		items_.clear();
		for (size_t i = 0; i < p_items.size(); ++i) {
			if (!p_items.at(i).field("IsExcluded").truthy()) {
				items_.push_back({ p_items.at(i), int32_t(i) });
			}
		}
		current_height_ = 0.0f;
		draw_grid();
	}

	void draw_grid() {
		if (items_.empty()) {
			return;
		}
		float cell_w = 0.0f;
		for (size_t i = 0; i < items_.size(); ++i) {
			As2Clip mc = cell_clip(int32_t(i));
			if (!mc.exists()) {
				mc = host_->attach_movie(grid_clips(), cell_type_, cell_name(i), int32_t(i));
			}
			GridCell *c = controller_at<GridCell>(*host_, mc);
			if (c == nullptr) {
				return;
			}
			row_height_ = c->cell_height();
			if (i == 0) {
				cell_w = c->cell_width();
				columns_ = std::max(1, int32_t(std::floor(grid_width_ / cell_w)));
				rows_ = int32_t((items_.size() - 1) / size_t(columns_)) + 1;
				setup_mask(cell_w * float(columns_));
				if (float(rows_) * row_height_ > grid_height_) {
					add_scrollbar(cell_w * float(columns_), rows_, row_height_);
				} else {
					remove_scrollbar();
				}
			}
			mc.set_x(float(int32_t(i) % columns_) * cell_w);
			mc.set_y(std::floor(float(i) / float(columns_)) * row_height_);
			c->set_data(items_[i].data, int32_t(i));
			c->set_state_function(
					[this](std::string_view p_state, int32_t p_index) { cell_mouse_state(p_state, p_index); });
		}
		for (size_t i = items_.size(); i < drawn_cells_; ++i) {
			host_->remove_movie_clip(cell_clip(int32_t(i)));
		}
		drawn_cells_ = items_.size();
		set_selected_index(selected_);
		adjust_scroll_poss(selected_);
		if (item_picker_) {
			backplate().set_width(float(columns_) * cell_w);
			backplate().set_visible(true);
			const float height = float(rows_) * row_height_;
			const bool changed = height != current_height_;
			current_height_ = height;
			if (changed && host_->self().visible()) {
				animate();
			}
		}
	}

	void setup_mask(float p_width) {
		float height = host_->clip("tmpSize").height();
		float width = p_width;
		if (item_picker_) {
			width += 1.0f;
			height += 1.0f;
		}
		const As2Clip self = host_->self();
		for (const char *old : { "GridScrollMaskMc", "GridBackplateMaskMc" }) {
			if (const As2Clip m = host_->clip(old); m.exists()) {
				host_->remove_movie_clip(m);
			}
		}
		const As2Clip mask =
				host_->attach_movie(self, "GridScrollMaskMc", "GridScrollMaskMc", self.next_highest_depth());
		mask.set_width(width);
		mask.set_height(height);
		mask.set_alpha(0.0f);
		grid_clips().set_mask(mask);
		if (item_picker_) {
			const As2Clip bm =
					host_->attach_movie(self, "GridScrollMaskMc", "GridBackplateMaskMc", self.next_highest_depth());
			bm.set_width(width);
			bm.set_height(height);
			bm.set_alpha(0.0f);
			backplate().set_mask(bm);
		}
	}

	void add_scrollbar(float p_x, int32_t p_rows, float p_cell_height) {
		const As2Clip self = host_->self();
		As2Clip bar = host_->clip("ScrollBar");
		if (!bar.exists()) {
			bar = host_->attach_movie(self, "ScrollBar", "ScrollBar", self.next_highest_depth());
		}
		ScrollBarClass *s = scrollbar();
		if (s == nullptr) {
			return;
		}
		s->init();
		s->set_scroll_updated([this](double p_percentage) { scroll_bar_updated(p_percentage); });
		bar.set_x(p_x - bar.child("whiteBar").width() + 1.0f);
		s->set_target_size(double(p_rows) * double(p_cell_height));
		s->set_size(grid_height_);
	}

	void remove_scrollbar() {
		if (const As2Clip bar = host_->clip("ScrollBar"); bar.exists()) {
			host_->remove_movie_clip(bar);
		}
	}

	void adjust_scroll_poss(int32_t p_index) {
		ScrollBarClass *s = scrollbar();
		const As2Clip c = cell_clip(p_index);
		if (s == nullptr || !c.exists()) {
			return;
		}
		const float base = grid_clips().y();
		const float top = base + c.y();
		const float bottom = top + row_height_;
		double offset = 0.0;
		if (top < 0.0f) {
			offset = double(top - base);
		} else if (bottom > grid_height_) {
			offset = double(bottom - base) - s->size();
		} else {
			return;
		}
		s->set_percentage(offset / (s->target_size() - s->size()) * 100.0, true);
	}

	void scroll_bar_updated(double p_percentage) {
		const ScrollBarClass *s = scrollbar();
		if (s == nullptr) {
			return;
		}
		grid_clips().set_y(float(grid_start_ + (s->size() - s->target_size()) * (p_percentage * 0.01)));
	}

	void set_selected_index(int32_t p_value) {
		const int32_t old = selected_;
		const int32_t count = int32_t(items_.size());
		selected_ = p_value;
		if (p_value < 0) {
			if (GridCell *o = cell(old)) {
				o->set_selected(false);
			}
			return;
		}
		if (p_value < count) {
			if (GridCell *o = cell(old)) {
				o->set_selected(false);
			}
			GridCell *c = cell(p_value);
			if (c != nullptr && (c->is_active() || highlight_inactive_)) {
				c->set_selected(true);
				host_->fire_event(kHighlighted, As2Value::of(selected_));
				host_->fire_event(kChangeButtonBar,
						As2Value::of(std::string(c->is_active() ? kButtonBarWithSelect : kButtonBarNoSelect)));
				host_->fire_event(kCellActiveChanged, As2Value::of(c->is_active()));
			} else if (c != nullptr) {
				set_selected_index(old > p_value ? p_value - 1 : p_value + 1);
			}
		} else if (p_value > count) {
			set_selected_index(0);
		}
	}

	void move_selection(UiInputConcept p_direction) {
		int32_t index = selected_;
		const int32_t count = int32_t(items_.size());
		if (p_direction == UiInputConcept::NavigateUp) {
			if (selected_ < columns_) {
				return;
			}
			index -= columns_;
		} else if (p_direction == UiInputConcept::NavigateDown) {
			if (selected_ / std::max(columns_, 1) >= rows_ - 1) {
				return;
			}
			index += columns_;
		} else if (p_direction == UiInputConcept::NavigateLeft) {
			index -= 1;
		} else if (p_direction == UiInputConcept::NavigateRight) {
			index += 1;
		}
		index = std::max(std::min(index, count - 1), 0);
		adjust_scroll_poss(index);
		if (index != selected_) {
			host_->fire_query(kSoundNavigate, As2Value::of(false));
		}
		set_selected_index(index);
	}

	void cell_mouse_state(std::string_view p_state, int32_t p_index) {
		if (p_state == kMouseOver) {
			host_->fire_event(kOver, As2Value::of(p_index));
		} else if (p_state == kMousePress) {
			host_->fire_event(kPressed, As2Value::of(p_index));
		} else if (p_state == kMouseRelease) {
			host_->fire_query(kSoundSelect, As2Value::of(false));
			host_->fire_event(kHighlighted, As2Value::of(p_index));
			host_->fire_event(kReleased, As2Value::of(p_index));
			set_selected_index(p_index);
		}
	}

	void on_show() {
		host_->request_focus();
		host_->self().set_visible(true);
		if (item_picker_) {
			closing_ = false;
			animate();
		}
		host_->fire_event(kForwardShow);
	}

	void on_hide() {
		if (item_picker_) {
			closing_ = true;
			animate();
		} else {
			host_->self().set_visible(false);
		}
		host_->fire_event(kForwardHide);
	}

	void animate() {
		if (!host_->self().visible()) {
			closing_ = false;
			return;
		}
		host_->clip("itemPickerItemsHolder.itemPickerItems").set_visible(!closing_);
		adjust_height();
	}

	void adjust_height() {
		const As2Clip plate = backplate();
		const As2Clip clips = grid_clips();
		host_->remove_tweens(plate);
		host_->remove_tweens(clips);
		const float height = closing_ ? 0.0f : std::min(grid_height_, current_height_);
		float y = grid_height_ - height;
		const float plate_y = y;
		if (const ScrollBarClass *s = scrollbar(); s != nullptr && !closing_) {
			y += std::min(0.0f, grid_height_ - current_height_) * float(s->percentage() / 100.0);
		}
		const double seconds = row_height_ > 0.0f
				? std::min(anim_time_, anim_time_ * std::abs(double(plate.y() - y)) / double(row_height_))
				: 0.0;
		const float alpha = closing_ ? alpha_min_ : alpha_max_;
		animating_ = true;
		if (!(seconds > 0.0)) {
			plate.set_height(height + kBackplateHeaderHeight);
			plate.set_alpha(alpha);
			plate.set_y(plate_y);
			clips.set_alpha(alpha);
			clips.set_y(y);
			on_animation_complete();
			return;
		}
		host_->add_tween(
				plate, HudTweenProperty::Height, height + kBackplateHeaderHeight, seconds, HudTweenEase::ExpoOut);
		host_->add_tween(plate, HudTweenProperty::Alpha, alpha, seconds, HudTweenEase::ExpoOut);
		host_->add_tween(plate, HudTweenProperty::Y, plate_y, seconds, HudTweenEase::ExpoOut, kAnimationDone);
		host_->add_tween(clips, HudTweenProperty::Alpha, alpha, seconds, HudTweenEase::ExpoOut);
		host_->add_tween(clips, HudTweenProperty::Y, y, seconds, HudTweenEase::ExpoOut);
	}

	void on_animation_complete() {
		host_->self().set_visible(!closing_);
		animating_ = false;
		closing_ = false;
	}

	struct Item {
		As2Value data;
		int32_t index = 0;
	};

	HudWidgetHost *host_ = nullptr;
	std::string cell_type_;
	std::vector<Item> items_;
	float grid_width_ = 0.0f;
	float grid_height_ = 0.0f;
	float row_height_ = 0.0f;
	float current_height_ = 0.0f;
	float alpha_min_ = 0.0f;
	float alpha_max_ = 100.0f;
	double anim_time_ = 1.0;
	double grid_start_ = 0.0;
	int32_t selected_ = -1;
	int32_t columns_ = 0;
	int32_t rows_ = 0;
	int32_t cell_name_start_ = 0;
	size_t drawn_cells_ = 0;
	bool keyboard_navigation_ = true;
	bool highlight_inactive_ = false;
	bool item_picker_ = false;
	bool release_locked_ = true;
	bool closing_ = false;
	bool animating_ = false;
};

}

void register_hud_widgets_grid(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Grid.Grid", [] { return std::make_unique<GridClass>(); });
	r_registry.add("Widget.Grid.Cell.KitCell", [] { return std::make_unique<KitCellClass>(); });
	r_registry.add("Widget.Grid.Cell.TextCell", [] { return std::make_unique<TextCellClass>(); });
}

}
