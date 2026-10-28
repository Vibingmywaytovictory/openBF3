#include "customize_kit.h"
#include "button.h"
#include "deploy_widgets.h"
#include "image_manager.h"
#include "kit_icons.h"
#include "scroll_bar.h"
#include "scroll_text.h"

#include "bf3/core/gameplay/weapon_def.h"
#include "bf3/core/ui/hud_icons.h"
#include "bf3/core/ui/text_db.h"

#include <limits>

namespace bf3::core::ui {

namespace {

constexpr int32_t kNavigateUp = 0;
constexpr int32_t kNavigateDown = 1;
constexpr int32_t kBtn1Released = 2;
constexpr int32_t kBtn2Released = 3;
constexpr int32_t kMouseOver = 4;
constexpr int32_t kMouseOut = 5;
constexpr int32_t kSelected = 6;
constexpr int32_t kGeneralInfo = 7;
constexpr int32_t kActivate = 8;
constexpr int32_t kSelectorChanged = 9;
constexpr double kSlotPadding = 2.0;
constexpr int32_t kSlotCount = 5;
constexpr float kFocusedAlpha = 60.0f;
constexpr float kOverAlpha = 20.0f;
constexpr uint32_t kBarColor = 7920639u;
constexpr uint32_t kBarGlowColor = 47871u;
constexpr float kScrollBarInset = 16.0f;
constexpr std::string_view kDefaultViewType = "KitView_1";

class KitViewClass;

float shown_height(const As2Clip &p_clip) {
	const SwfSceneNode *n = p_clip.node();
	if (n == nullptr || p_clip.scene() == nullptr) {
		return 0.0f;
	}
	const SwfScene &scene = *p_clip.scene();
	float lo = std::numeric_limits<float>::max();
	float hi = std::numeric_limits<float>::lowest();
	for (const SwfNodeId id : n->children) {
		const SwfSceneNode *c = scene.node(id);
		if (c == nullptr || !c->visible) {
			continue;
		}
		const SwfRect b = swf_node_local_bounds(scene, *c);
		const SwfMatrix &m = c->matrix;
		for (const float x : { b.x_min, b.x_max }) {
			for (const float y : { b.y_min, b.y_max }) {
				const float v = m.b * x + m.d * y + m.ty;
				lo = std::min(lo, v);
				hi = std::max(hi, v);
			}
		}
	}
	return hi >= lo ? (hi - lo) * std::fabs(n->matrix.d) : 0.0f;
}

void set_scroll_text(HudWidgetHost &r_host, const As2Clip &p_clip, std::string_view p_text) {
	if (ScrollTextWidget *t = controller_at<ScrollTextWidget>(r_host, p_clip)) {
		t->set_text(p_text);
	} else {
		p_clip.set_text(p_text);
	}
}

void show_image(HudWidgetHost &r_host, const As2Clip &p_clip, const As2Value &p_path) {
	if (!p_clip.exists()) {
		return;
	}
	if (!p_path.defined() || p_path.to_string().empty() || !game_utils_is_image_path(p_path)) {
		p_clip.set_visible(false);
		return;
	}
	ImageManagerWidget *m = controller_at<ImageManagerWidget>(r_host, p_clip);
	HudWidgetHost *h = r_host.screen().host(p_clip.path());
	if (m != nullptr && h != nullptr) {
		m->update_image_url(*h, p_path.to_string());
	}
	p_clip.set_visible(true);
}

class KitSlotClass final : public HudWidgetController {
public:
	explicit KitSlotClass(bool p_selector) : selector_(p_selector) {}

	void construct(HudWidgetHost &r_host) override { host_ = &r_host; }

	void initialize_slot(
			KitViewClass &r_owner, int32_t p_index, const std::string &p_label1, const std::string &p_label2);
	void update_data(const As2Value &p_data);
	void on_focused(bool p_focused);
	void step_left();
	void step_right();
	const As2Value &identifier() const { return id_; }

	bool mouse_move(HudWidgetHost &r_host, const HudMouse &p_mouse) override {
		if (p_mouse.target != r_host.clip("mouseArea").path()) {
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
		if (p_mouse.target != r_host.clip("mouseArea").path()) {
			return false;
		}
		activate();
		return true;
	}

private:
	As2Clip clip(std::string_view p_name) const { return host_->clip(p_name); }
	void setup_mouse_interaction();
	void mouse_over();
	void mouse_out();
	void on_button_released(int32_t p_button);
	void activate();
	void update_slot(const As2Value &p_data);
	void select(int32_t p_index, bool p_event);

	HudWidgetHost *host_ = nullptr;
	KitViewClass *owner_ = nullptr;
	bool selector_ = false;
	int32_t index_ = 0;
	bool focused_ = false;
	bool over_ = false;
	As2Value id_;
	As2Value general_info_;
	std::vector<As2Value> data_;
	int32_t current_ = 0;
};

class KitViewClass final : public HudWidgetController {
public:
	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override {
		setup_.reset();
		for (const HudDataSource &s : p_node.sources) {
			if (s.name == "Setup" && s.category == kUiCustomizationCategory) {
				setup_ = s;
			}
		}
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		if (!setup_) {
			return;
		}
		if (std::optional<As2Value> v = customize_kit_setup(p_model, p_client, setup_->key)) {
			r_out.push_back({ setup_->name, std::move(*v), setup_->on_initialize });
		}
	}

	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		const std::string type = param_string(r_host, "m_viewType");
		view_type_ = type == "undefined" ? std::string(kDefaultViewType) : type;
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		const As2Clip self = r_host.self();
		const float w = float(as2_round(self.width()));
		const float h = float(as2_round(self.height()));
		self.set_xscale(100.0f);
		self.set_yscale(100.0f);
		mask_ = r_host.clip("sizeDummy");
		mask_.set_width(w);
		mask_.set_height(h);
		self.set_mask(mask_);
		container_ = r_host.create_empty_movie_clip(self, "mcContainer", self.next_highest_depth());
		self.set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		if (const std::string *v = p_init.property("BtnLabel1")) {
			label1_ = *v;
		}
		if (const std::string *v = p_init.property("BtnLabel2")) {
			label2_ = *v;
		}
		slots_.clear();
		setup_refresh_data(*this, r_host, p_init.data);
	}

	void update(HudWidgetHost &, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Setup") {
			update_setup_data(p_value);
		} else if (p_key == "Visibility") {
			should_be_visible_ = p_value.defined() && p_value.truthy();
		}
	}

	void enter_frame(HudWidgetHost &r_host) override {
		r_host.set_enter_frame(false);
		if (pending_) {
			initialize_slots(*std::exchange(pending_, std::nullopt));
		}
	}

	void in_event(HudWidgetHost &, int32_t p_index, const As2Value &p_param) override {
		if (p_index == kActivate) {
			activate(p_param);
		}
	}

	void focus_gained(HudWidgetHost &) override {
		focused_ = true;
		if (KitSlotClass *s = slot(index_)) {
			s->on_focused(true);
		}
	}

	void focus_lost(HudWidgetHost &) override {
		focused_ = false;
		if (KitSlotClass *s = slot(index_)) {
			s->on_focused(false);
		}
	}

	bool input_concept(HudWidgetHost &r_host, int32_t p_concept, bool p_pressed) override {
		if (!focused_) {
			return false;
		}
		const UiInputConcept c = UiInputConcept(p_concept);
		if (!p_pressed) {
			if (c == UiInputConcept::Activate && hud_platform_win32(r_host.client())) {
				play_sound();
				const KitSlotClass *s = slot(index_);
				fire_event(kBtn1Released, s != nullptr ? s->identifier() : As2Value::undefined());
			}
			return false;
		}
		const int32_t n = int32_t(slots_.size());
		if (c == UiInputConcept::NavigateUp) {
			if (index_ > 0) {
				select_slot(index_ - 1, false);
			} else {
				fire_event(kNavigateUp, As2Value::of("up"));
			}
		} else if (c == UiInputConcept::NavigateDown) {
			if (index_ + 1 < n) {
				select_slot(index_ + 1, false);
			} else {
				fire_event(kNavigateDown, As2Value::of("down"));
			}
		} else if (c == UiInputConcept::NavigateLeft) {
			if (KitSlotClass *s = slot(index_)) {
				s->step_left();
			}
		} else if (c == UiInputConcept::NavigateRight) {
			if (KitSlotClass *s = slot(index_)) {
				s->step_right();
			}
		}
		return false;
	}

	void select_slot(int32_t p_index, bool p_force_focus) {
		if (KitSlotClass *s = slot(index_)) {
			s->on_focused(false);
		}
		index_ = p_index;
		if (p_force_focus) {
			host_->request_focus();
		}
		if (focused_) {
			if (KitSlotClass *s = slot(index_)) {
				s->on_focused(true);
			}
			scroll_slots(index_);
		}
	}

	void fire_general_info(const As2Value &p_info) {
		fire_event(kGeneralInfo, As2Value::of(game_utils_object2string(p_info)));
	}

	bool fire_event(int32_t p_index, const As2Value &p_param) { return host_->fire_event(p_index, p_param); }

	void play_sound() { host_->fire_query(kSoundSelect, As2Value::of(false)); }

	size_t slot_count() const { return slots_.size(); }

private:
	KitSlotClass *slot(int32_t p_index) const {
		if (p_index < 0 || size_t(p_index) >= slots_.size()) {
			return nullptr;
		}
		return controller_at<KitSlotClass>(*host_, host_->screen().clip(slots_[size_t(p_index)]));
	}

	void activate(const As2Value &p_param) {
		const std::string text = p_param.to_string();
		const int32_t n = int32_t(slots_.size());
		if (n == 0) {
			if (text == "down") {
				fire_event(kNavigateDown, As2Value::of("down"));
			}
			if (text == "up") {
				fire_event(kNavigateUp, As2Value::of("up"));
			}
			return;
		}
		double i = 0.0;
		if (text == "down") {
			i = 0.0;
		} else if (text == "up") {
			i = double(n - 1);
		} else {
			i = p_param.to_number();
		}
		if (i >= 0.0 && i < double(n)) {
			select_slot(int32_t(i), true);
		} else {
			host_->request_focus();
		}
	}

	void update_setup_data(const As2Value &p_data) {
		std::vector<As2Value> slots;
		if (p_data.kind == As2Value::Kind::List) {
			slots = p_data.items;
		} else if (p_data.defined()) {
			slots.push_back(p_data);
		} else {
			return;
		}
		if (setup_slots(int32_t(slots.size()))) {
			update_slots(slots);
		} else {
			pending_ = std::move(slots);
			host_->set_enter_frame(true);
		}
	}

	bool setup_slots(int32_t p_count) {
		for (size_t i = size_t(std::max(p_count, 0)); i < slots_.size(); ++i) {
			host_->remove_movie_clip(host_->screen().clip(slots_[i]));
		}
		if (slots_.size() > size_t(std::max(p_count, 0))) {
			slots_.resize(size_t(std::max(p_count, 0)));
		}
		const int32_t before = int32_t(slots_.size());
		for (int32_t i = before; i < p_count; ++i) {
			const As2Clip s = host_->attach_movie(
					container_, view_type_, "Slot_" + std::to_string(i), container_.next_highest_depth());
			slot_height_ = std::ceil(double(shown_height(s)) + kSlotPadding);
			s.set_y(float(std::floor(double(i) * slot_height_)));
			slots_.push_back(s.path());
		}
		index_ = std::min(index_, p_count - 1);
		visible_slots_ = mask_.exists() && slot_height_ > 0.0
				? int32_t(std::floor((double(mask_.height()) + kSlotPadding + 4.0) / slot_height_))
				: std::numeric_limits<int32_t>::max();
		scroll_slots(index_);
		if (int32_t(slots_.size()) > visible_slots_) {
			add_scrollbar();
			if (ScrollBarClass *b = scrollbar()) {
				b->set_target_size(slot_height_ * double(slots_.size()) - kSlotPadding);
			}
		} else {
			remove_scrollbar();
		}
		return !(p_count > before);
	}

	void initialize_slots(const std::vector<As2Value> &p_slots) {
		for (size_t i = 0; i < slots_.size(); ++i) {
			if (KitSlotClass *s = slot(int32_t(i))) {
				s->initialize_slot(*this, int32_t(i), label1_, label2_);
			}
		}
		update_slots(p_slots);
	}

	void update_slots(const std::vector<As2Value> &p_slots) {
		for (size_t i = 0; i < slots_.size(); ++i) {
			if (KitSlotClass *s = slot(int32_t(i))) {
				s->update_data(i < p_slots.size() ? p_slots[i] : As2Value::undefined());
			}
		}
		select_slot(index_, false);
		host_->self().set_visible(should_be_visible_);
	}

	ScrollBarClass *scrollbar() const {
		return scrollbar_.empty() ? nullptr : controller_at<ScrollBarClass>(*host_, host_->screen().clip(scrollbar_));
	}

	void scroll_slots(int32_t p_index) {
		ScrollBarClass *b = scrollbar();
		const int32_t n = int32_t(slots_.size());
		if (b == nullptr || slot_height_ <= 0.0) {
			return;
		}
		const double offset = b->percentage() * slot_height_ * double(n - visible_slots_) / 100.0;
		const int32_t first = int32_t(std::ceil(offset / slot_height_));
		const int32_t last =
				int32_t(std::floor((offset + slot_height_ * double(visible_slots_ - 1) - kSlotPadding) / slot_height_));
		double index = double(p_index);
		if (p_index < first) {
			index = std::max(0.0, std::min(index, double(n - visible_slots_)));
		} else if (p_index > last) {
			index = std::max(0.0, std::min(index - double(visible_slots_) + 1.0, double(n - visible_slots_)));
		} else {
			return;
		}
		b->set_percentage(index / std::max(1.0, double(n - visible_slots_)) * 100.0);
	}

	void scroll_bar_updated(double p_percentage) {
		container_.set_y(
				float(-(p_percentage * slot_height_ * double(int32_t(slots_.size()) - visible_slots_)) / 100.0));
	}

	void add_scrollbar() {
		if (!scrollbar_.empty()) {
			return;
		}
		const As2Clip self = host_->self();
		const As2Clip b = host_->attach_movie(self, "ScrollBar", "ScrollBar", self.next_highest_depth());
		scrollbar_ = b.path();
		if (ScrollBarClass *c = scrollbar()) {
			c->init();
			c->set_scroll_updated([this](double p_percentage) { scroll_bar_updated(p_percentage); });
			b.set_x(self.width() - kScrollBarInset);
			c->set_size(mask_.height());
		}
	}

	void remove_scrollbar() {
		if (scrollbar_.empty()) {
			return;
		}
		host_->remove_movie_clip(host_->screen().clip(scrollbar_));
		scrollbar_.clear();
	}

	HudWidgetHost *host_ = nullptr;
	std::optional<HudDataSource> setup_;
	std::string view_type_;
	std::string label1_;
	std::string label2_;
	As2Clip mask_;
	As2Clip container_;
	std::vector<std::string> slots_;
	std::optional<std::vector<As2Value>> pending_;
	std::string scrollbar_;
	int32_t index_ = 0;
	double slot_height_ = 0.0;
	int32_t visible_slots_ = 0;
	bool focused_ = false;
	bool should_be_visible_ = true;
};

void KitSlotClass::initialize_slot(
		KitViewClass &r_owner, int32_t p_index, const std::string &p_label1, const std::string &p_label2) {
	owner_ = &r_owner;
	index_ = p_index;
	on_focused(false);
	clip("mainLock").set_visible(false);
	clip("plus").set_visible(false);
	const bool win32 = hud_platform_win32(host_->client());
	const As2Clip b1 = clip("button1");
	const As2Clip b2 = clip("button2");
	if (HudButton *b = b1.exists() ? hud_initialized_button(*host_, b1) : nullptr) {
		b1.set_visible(p_label1 != "");
		b->update_button_text(p_label1);
	}
	if (HudButton *b = b2.exists() ? hud_initialized_button(*host_, b2) : nullptr) {
		b2.set_visible(p_label2 != "");
		b->update_button_text(p_label2);
	}
	if (b2.exists() && !b2.visible()) {
		b1.set_x(b2.x());
	}
	if (HudAdvancedBar *bar = controller_at<HudAdvancedBar>(*host_, clip("bar"))) {
		bar->initialize_bars({ HudBarSpec{ true, 100.0f, kBarColor, false, 1.0, 0.0 },
				HudBarSpec{ true, 100.0f, 0xFFFFFFu, false, 1.0, 0.0 } });
		bar->set_use_glow(true);
		bar->set_glow_color(kBarGlowColor);
	}
	if (selector_) {
		for (const char *arrow : { "buttonLeft", "buttonRight" }) {
			const As2Clip a = clip(arrow);
			if (a.exists()) {
				hud_initialized_button(*host_, a);
			}
		}
	}
	if (win32) {
		setup_mouse_interaction();
	}
}

void KitSlotClass::setup_mouse_interaction() {
	host_->set_mouse_target(clip("mouseArea"), true);
	const std::array<std::pair<const char *, int32_t>, 2> buttons{ { { "button1", 1 }, { "button2", 2 } } };
	for (const auto &[name, n] : buttons) {
		if (HudButton *b = controller_at<HudButton>(*host_, clip(name))) {
			HudButtonCallbacks c;
			c.over = [this](const std::string &) { mouse_over(); };
			c.release = [this, n](const std::string &) { on_button_released(n); };
			b->set_callbacks(std::move(c));
		}
	}
	if (!selector_) {
		return;
	}
	const std::array<std::pair<const char *, bool>, 2> arrows{ { { "buttonLeft", true }, { "buttonRight", false } } };
	for (const auto &[name, left] : arrows) {
		if (HudButton *b = controller_at<HudButton>(*host_, clip(name))) {
			HudButtonCallbacks c;
			c.press = [this, left](const std::string &) {
				activate();
				if (left) {
					step_left();
				} else {
					step_right();
				}
			};
			c.over = [this](const std::string &) { mouse_over(); };
			c.out = [this](const std::string &) { mouse_out(); };
			b->set_callbacks(std::move(c));
		}
	}
}

void KitSlotClass::mouse_over() {
	clip("bg_selected").set_alpha(focused_ ? kFocusedAlpha : kOverAlpha);
	clip("bg_normal").set_visible(!focused_);
	if (owner_ == nullptr) {
		return;
	}
	owner_->fire_general_info(general_info_);
	if (!over_) {
		owner_->fire_event(kMouseOver, As2Value::of(0));
		over_ = true;
	}
}

void KitSlotClass::mouse_out() {
	over_ = false;
	clip("bg_selected").set_alpha(focused_ ? kFocusedAlpha : 0.0f);
	clip("bg_normal").set_visible(!focused_);
	if (owner_ != nullptr) {
		owner_->fire_event(kMouseOut, As2Value::of(0));
	}
}

void KitSlotClass::activate() {
	if (owner_ != nullptr) {
		owner_->select_slot(index_, true);
	}
}

void KitSlotClass::on_focused(bool p_focused) {
	focused_ = p_focused;
	clip("bg_selected").set_alpha(p_focused ? kFocusedAlpha : 0.0f);
	clip("bg_normal").set_visible(!p_focused);
	if (focused_ && owner_ != nullptr) {
		owner_->fire_event(kSelected, id_);
		owner_->fire_general_info(general_info_);
	}
}

void KitSlotClass::on_button_released(int32_t p_button) {
	if (owner_ == nullptr) {
		return;
	}
	owner_->select_slot(index_, true);
	owner_->play_sound();
	owner_->fire_event(p_button == 1 ? kBtn1Released : kBtn2Released, id_);
}

void KitSlotClass::update_slot(const As2Value &p_data) {
	set_scroll_text(*host_, clip("header"), as2_text(p_data.field("Label")));
	const As2Clip icon = clip("headerIcon");
	if (icon.exists()) {
		icon.set_visible(false);
		if (const As2Value &h = p_data.field("HeaderIcon"); h.defined()) {
			icon.set_visible(true);
			icon.goto_and_stop(h.to_string());
		}
		if (const As2Value &stars = p_data.field("ServiceStars"); stars.defined() && !(stars.to_number() < 0.0)) {
			icon.set_visible(true);
			if (KitIconsWidget *k = controller_at<KitIconsWidget>(*host_, icon)) {
				k->set_stars(as2_parse_int(stars));
			} else if (ServiceStarsWidget *w = controller_at<ServiceStarsWidget>(*host_, icon)) {
				w->set_stars(as2_parse_int(stars));
			}
		}
	}
	id_ = p_data.field("Index");
	if (const As2Clip main = clip("mainImage"); main.exists()) {
		show_image(*host_, main, p_data.field("ItemImage"));
	}
	const As2Clip bar_clip = clip("bar");
	const std::optional<As2Value> bars = ui_base_object_to_array(p_data.field("BarValues"));
	if (!bars) {
		bar_clip.set_visible(false);
	} else if (HudAdvancedBar *bar = controller_at<HudAdvancedBar>(*host_, bar_clip)) {
		bar_clip.set_visible(true);
		for (size_t i = 0; i < bars->size(); ++i) {
			const As2Value &v = bars->at(i);
			bar->set_value_on_index(i, v.defined() ? v : As2Value::of(0));
		}
	}
	const std::optional<As2Value> slots = ui_base_object_to_array(p_data.field("SlotsData"));
	for (int32_t i = 0; i < kSlotCount; ++i) {
		const std::string n = std::to_string(i + 1);
		const As2Clip image = clip("slot" + n);
		const As2Clip lock = clip("slotLock" + n);
		const As2Clip label = clip("slotLabel" + n);
		if (!slots) {
			image.set_visible(false);
			continue;
		}
		const As2Value &s = slots->at(size_t(i));
		if (!s.defined()) {
			image.set_visible(false);
			lock.set_visible(false);
			continue;
		}
		show_image(*host_, image, s.field("ItemImage"));
		lock.set_visible(as2_equals_true(s.field("Locked")));
		if (label.exists()) {
			const As2Value &l = s.field("Label");
			set_scroll_text(*host_, label, l.truthy() ? l.to_string() : std::string());
		}
	}
	clip("mainLock").set_visible(as2_equals_true(p_data.field("Locked")));
	const As2Clip plus = clip("plus");
	if (plus.exists()) {
		plus.set_visible(false);
		if (!hud_platform_win32(host_->client()) &&
				(p_data.field("ShowButton1Plus").truthy() || p_data.field("ShowButton2Plus").truthy())) {
			plus.set_visible(true);
		}
		if (p_data.field("ShowPlus").truthy()) {
			plus.set_visible(true);
		}
	}
	const std::array<std::pair<const char *, const char *>, 2> buttons{ { { "button1", "ShowButton1Plus" },
			{ "button2", "ShowButton2Plus" } } };
	for (const auto &[name, key] : buttons) {
		const As2Clip b = clip(name);
		HudButton *button = b.exists() && b.visible() ? controller_at<HudButton>(*host_, b) : nullptr;
		if (button == nullptr) {
			continue;
		}
		if (p_data.field(key).truthy()) {
			button->set_plus();
		} else {
			button->remove_plus();
		}
	}
	general_info_ = p_data.field("Description");
	if (focused_ && owner_ != nullptr) {
		owner_->fire_general_info(general_info_);
	}
}

void KitSlotClass::update_data(const As2Value &p_data) {
	if (!selector_) {
		update_slot(p_data);
		return;
	}
	const std::optional<As2Value> items = ui_base_object_to_array(p_data.field("Items"));
	data_ = items ? items->items : std::vector<As2Value>();
	update_slot(size_t(current_) < data_.size() ? data_[size_t(current_)] : As2Value::undefined());
	clip("category").set_text(p_data.field("ItemCategory").to_string());
	for (size_t i = 0; i < data_.size(); ++i) {
		if (as2_equals_true(data_[i].field("DefaultSelected"))) {
			select(int32_t(i), false);
			break;
		}
	}
	const bool many = data_.size() > 1;
	for (const char *name : { "buttonLeft", "buttonRight" }) {
		if (HudButton *b = controller_at<HudButton>(*host_, clip(name))) {
			b->set_active(many);
			if (p_data.field("ShowStepPlus").truthy() && many) {
				b->set_plus();
			} else {
				b->remove_plus();
			}
		}
	}
}

void KitSlotClass::step_left() {
	if (!selector_ || data_.size() <= 1 || owner_ == nullptr) {
		return;
	}
	owner_->play_sound();
	const int32_t n = int32_t(data_.size());
	select((current_ - 1 + n) % n, true);
}

void KitSlotClass::step_right() {
	if (!selector_ || data_.size() <= 1 || owner_ == nullptr) {
		return;
	}
	owner_->play_sound();
	select((current_ + 1) % int32_t(data_.size()), true);
}

void KitSlotClass::select(int32_t p_index, bool p_event) {
	if (p_index < 0 || size_t(p_index) >= data_.size()) {
		return;
	}
	current_ = p_index;
	update_slot(data_[size_t(current_)]);
	if (p_event) {
		activate();
		if (owner_ != nullptr) {
			owner_->fire_event(kSelectorChanged, data_[size_t(current_)].field("Index"));
		}
	}
}

class KitInfoBoxClass final : public HudWidgetController {
public:
	static constexpr int32_t kShow = 0;
	static constexpr int32_t kHide = 1;
	static constexpr int32_t kToggle = 2;
	static constexpr int32_t kInfo = 3;
	static constexpr float kGap = 10.0f;

	void construct(HudWidgetHost &r_host) override { host_ = &r_host; }

	void on_clip_load(HudWidgetHost &r_host) override {
		const As2Clip header = r_host.clip("header");
		const As2Clip separator = r_host.clip("separator");
		const As2Clip label = r_host.clip("label");
		label_right_edge_ = label.x() + label.width();
		separator.set_x(header.x() + header.text_width() + kGap);
		label.set_x(separator.x() + separator.text_width() + kGap);
		clear_box();
		r_host.self().set_visible(false);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
	}

	void in_event(HudWidgetHost &r_host, int32_t p_index, const As2Value &p_param) override {
		const As2Clip self = r_host.self();
		if (p_index == kShow) {
			self.set_visible(true);
		} else if (p_index == kHide) {
			self.set_visible(false);
		} else if (p_index == kToggle) {
			self.set_visible(!self.visible());
		} else if (p_index == kInfo) {
			on_general_info(p_param);
		}
	}

	std::string label_text() const { return label_; }

private:
	void on_general_info(const As2Value &p_param) {
		if (!p_param.defined()) {
			clear_box();
			return;
		}
		const As2Value info = game_utils_string2object(p_param.to_string());
		const std::string type = info.field("Type").to_string();
		if (type == "0") {
			show_stats(info);
		} else if (type == "1") {
			show_description(info);
		} else {
			clear_box();
		}
	}

	void clear_box() {
		As2Value empty = As2Value::object();
		empty.set("Description", As2Value::of(""));
		empty.set("Label", As2Value::of("NO DATA"));
		show_description(empty);
	}

	void set_label(const As2Value &p_label) {
		label_ = as2_text(p_label);
		set_scroll_text(*host_, host_->clip("label"), label_);
		const As2Clip label = host_->clip("label");
		const float w = label_right_edge_ - label.x();
		if (w > 0.0f) {
			if (ScrollTextWidget *t = controller_at<ScrollTextWidget>(*host_, label)) {
				t->set_width(w);
			}
			label.set_visible(true);
		} else {
			label.set_visible(false);
		}
	}

	void show_stats(const As2Value &p_data) {
		host_->clip("description").set_visible(false);
		for (const char *name : { "ammoHeader", "ammoValue", "rofHeader", "rofValue", "rangeHeader", "rangeValue",
					 "firemodesHeader" }) {
			host_->clip(name).set_visible(true);
		}
		host_->clip("ammoValue").set_text(as2_text(p_data.field("Ammo")));
		host_->clip("rofValue").set_text(as2_text(p_data.field("RateOfFire")));
		host_->clip("rangeValue").set_text(as2_text(p_data.field("Range")));
		set_label(p_data.field("Label"));
		for (int32_t i = 0; i < 3; ++i) {
			host_->clip("fireMode" + std::to_string(i)).set_visible(false);
		}
		const std::optional<As2Value> modes = ui_base_object_to_array(p_data.field("FireModes"));
		const size_t n = modes ? modes->size() : 0;
		for (size_t i = 0; i < n && i < 3; ++i) {
			const As2Clip m = host_->clip("fireMode" + std::to_string(n - i - 1));
			m.set_visible(true);
			m.goto_and_stop(modes->at(i).to_string());
		}
	}

	void show_description(const As2Value &p_data) {
		host_->clip("description").set_visible(true);
		for (const char *name : { "ammoHeader", "ammoValue", "rofHeader", "rofValue", "rangeHeader", "rangeValue",
					 "firemodesHeader", "fireMode0", "fireMode1", "fireMode2" }) {
			host_->clip(name).set_visible(false);
		}
		set_label(p_data.field("Label"));
		host_->clip("description").set_text(as2_text(p_data.field("Description")));
	}

	HudWidgetHost *host_ = nullptr;
	float label_right_edge_ = 0.0f;
	std::string label_;
};

class SquadBoosterInfoClass final : public HudWidgetController {
public:
	void on_clip_load(HudWidgetHost &r_host) override { r_host.self().set_visible(false); }

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key != "SquadBoosters") {
			return;
		}
		for (size_t i = 0; i < p_value.size(); ++i) {
			r_host.clip("icon_" + std::to_string(i)).goto_and_stop(p_value.at(i).field("BoosterIcon").to_string());
		}
	}
};

std::string localised(const HudClientState &p_client, uint32_t p_hash) {
	return p_hash != 0 && p_client.localise_hash ? p_client.localise_hash(p_hash) : std::string();
}

const ItemIcon *item_icon(const HudClientState &p_client, const HudItem &p_item) {
	return p_client.items != nullptr && p_item.item_id != 0 ? p_client.items->find(p_item.item_id) : nullptr;
}

std::string localised_sid(const HudClientState &p_client, const std::string &p_sid) {
	return p_sid.empty() ? std::string() : localised(p_client, text_hash(p_sid));
}

As2Value item_info(const HudClientState &p_client, const HudItem &p_item) {
	As2Value info = As2Value::object();
	info.set("Label", As2Value::of(localised(p_client, p_item.name_sid_hash)));
	const ItemIcon *icon = item_icon(p_client, p_item);
	if (icon == nullptr || icon->ammo.empty()) {
		info.set("Type", As2Value::of(1));
		info.set("Description",
				As2Value::of(icon != nullptr ? localised_sid(p_client, icon->description_sid) : std::string()));
		return info;
	}
	info.set("Type", As2Value::of(0));
	info.set("Ammo", As2Value::of(icon->ammo));
	info.set("RateOfFire", As2Value::of(icon->rate_of_fire));
	info.set("Range", As2Value::of(localised_sid(p_client, icon->range_sid)));
	As2Value modes = As2Value::array();
	for (uint32_t l = 0; l < 8; ++l) {
		if ((icon->fire_modes >> l) & 1u) {
			modes.push(As2Value::of(gameplay::fire_logic_name(gameplay::FireLogic(l))));
		}
	}
	info.set("FireModes", modes);
	return info;
}

As2Value slot_of(const HudClientState &p_client, const HudItem &p_item, bool p_texture) {
	As2Value slot = As2Value::object();
	slot.set("Label", As2Value::of(localised(p_client, p_item.name_sid_hash)));
	const ItemIcon *icon = p_texture ? item_icon(p_client, p_item) : nullptr;
	std::string image = icon != nullptr ? icon->texture_path : std::string();
	if (image.empty()) {
		image = hud_string(p_item.image_path);
	}
	if (!image.empty()) {
		slot.set("ItemImage", As2Value::of(image));
	}
	return slot;
}

As2Value accessory_slots(const HudModel &p_model, const HudClientState &p_client) {
	As2Value slots = As2Value::array();
	for (const HudItem *a : { &p_model.customization.weapon_accessory1, &p_model.customization.weapon_accessory2,
				 &p_model.customization.weapon_accessory3 }) {
		slots.push(slot_of(p_client, *a, false));
	}
	return slots;
}

As2Value selector_items(const HudModel &p_model, const HudClientState &p_client, const HudItemList &p_list,
		int16_t p_selected, bool p_stars, bool p_accessories) {
	As2Value items = As2Value::array();
	for (size_t i = 0; i < p_list.count; ++i) {
		const HudItem &it = p_list.items[i];
		As2Value item = slot_of(p_client, it, true);
		item.set("Index", As2Value::of(int32_t(i)));
		item.set("DefaultSelected", As2Value::of(int32_t(i) == p_selected));
		if (p_stars) {
			item.set("ServiceStars", As2Value::of(0));
		}
		if (p_accessories && int32_t(i) == p_selected) {
			item.set("SlotsData", accessory_slots(p_model, p_client));
		}
		item.set("Description", item_info(p_client, it));
		items.push(std::move(item));
	}
	return items;
}

As2Value selector(As2Value p_items, std::string p_category) {
	As2Value out = As2Value::object();
	out.set("Items", std::move(p_items));
	out.set("ItemCategory", As2Value::of(std::move(p_category)));
	return out;
}

}

std::optional<As2Value> customize_kit_setup(const HudModel &p_model, const HudClientState &p_client, int32_t p_key) {
	const HudCustomization &c = p_model.customization;
	if (p_key == kCustomizeKitsSetupKey) {
		As2Value kits = As2Value::array();
		for (size_t k = 0; k < c.kits_data.size(); ++k) {
			const HudKitEntry &e = c.kits_data[k];
			As2Value kit = As2Value::object();
			kit.set("Label", As2Value::of(localised(p_client, e.name_sid_hash)));
			kit.set("HeaderIcon", As2Value::of(std::string(kGameUtilsKits[k].label)));
			kit.set("ServiceStars", As2Value::of(0));
			kit.set("Index", As2Value::of(int32_t(k)));
			kit.set("BarValues", As2Value::array({ As2Value::of(0), As2Value::of(0) }));
			As2Value slots = As2Value::array();
			for (const HudItem &item : e.items) {
				slots.push(slot_of(p_client, item, false));
			}
			kit.set("SlotsData", std::move(slots));
			kits.push(std::move(kit));
		}
		return kits;
	}
	for (size_t i = 0; i < kCustomizeItemSetupKeys.size(); ++i) {
		if (p_key == kCustomizeItemSetupKeys[i]) {
			const bool stars = i <= size_t(kCustomizeGadget1Item);
			return selector(
					selector_items(p_model, p_client, c.kit_grid_data[i], c.kit_grid_selected[i], stars, i == 0),
					localised(p_client, c.kit_grid_category[i]));
		}
	}
	if (p_key == kCustomizeWeaponSetupKey) {
		return selector(selector_items(p_model, p_client, c.kit_grid_data[0], c.kit_grid_selected[0], true, true),
				localised(p_client, c.kit_grid_category[0]));
	}
	for (size_t a = 0; a < kCustomizeAccessorySetupKeys.size(); ++a) {
		if (p_key == kCustomizeAccessorySetupKeys[a]) {
			return selector(
					selector_items(p_model, p_client, c.accessory_grid[a], c.accessory_selected[a], false, false), "");
		}
	}
	return std::nullopt;
}

void register_hud_widgets_customize_kit(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.CustomizeKit.KitView", [] { return std::make_unique<KitViewClass>(); });
	r_registry.add("Widget.CustomizeKit.KitViewSlot", [] { return std::make_unique<KitSlotClass>(false); });
	r_registry.add("Widget.CustomizeKit.KitSelectorSlot", [] { return std::make_unique<KitSlotClass>(true); });
	r_registry.add("Widget.CustomizeKit.KitInfoBox", [] { return std::make_unique<KitInfoBoxClass>(); });
	r_registry.add("Widget.Info.SquadBoosterInfo", [] { return std::make_unique<SquadBoosterInfoClass>(); });
}

}
