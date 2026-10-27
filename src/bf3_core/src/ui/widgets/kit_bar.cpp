#include "button.h"
#include "deploy_widgets.h"
#include "image_manager.h"
#include "kit_icons.h"
#include "scroll_text.h"

namespace bf3::core::ui {

namespace {

class KitBarClass final : public HudWidgetController, public HudKitBar {
public:
	static constexpr int32_t kKitChanged = 0;
	static constexpr int32_t kSetSlot = 1;
	static constexpr int32_t kShowGrid = 2;
	static constexpr int32_t kHideGrid = 3;
	static constexpr int32_t kStepLeft = 4;
	static constexpr int32_t kStepRight = 5;
	static constexpr int32_t kDoneSelectingSlotItem = 6;
	static constexpr int32_t kKitCount = 4;
	static constexpr int32_t kSlotCount = 5;

	void construct(HudWidgetHost &r_host) override {
		host_ = &r_host;
		r_host.self().set_visible(false);
	}

	void on_clip_load(HudWidgetHost &r_host) override {
		win32_ = hud_platform_win32(r_host.client());
		const std::string platform = r_host.client().platform;
		if (win32_) {
			r_host.clip("btnSlotLeft").set_visible(false);
			r_host.clip("btnSlotRight").set_visible(false);
			r_host.clip("changeItemBtn").set_visible(false);
		} else {
			r_host.clip("btnSlotLeft").goto_and_stop(platform);
			r_host.clip("btnSlotRight").goto_and_stop(platform);
			r_host.clip("changeItemBtn").goto_and_stop(platform);
			r_host.clip("btnSlotLeft.icon").goto_and_stop("IDB_Ltrigger2");
			r_host.clip("btnSlotRight.icon").goto_and_stop("IDB_Rtrigger2");
			r_host.clip("changeItemBtn.icon").goto_and_stop("IDB_Rdown");
		}
		const As2Clip deploy_as = r_host.clip("deployAs");
		r_host.clip("kitName").set_x(deploy_as.x() + kTextGap + deploy_as.text_width());
		const As2Clip change = r_host.clip("changeItemText");
		change.set_x(slot_button(0).x() - change.text_width() - kTextGap);
		change.set_visible(!win32_);
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_buttons(r_host);
		if (!win32_) {
			set_selected_slot(0);
		}
		setup_refresh_data(*this, r_host, p_init.data);
		setup_platform_specific_items(r_host);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "SelectedKit") {
			const double k = p_value.to_number();
			if (current_kit_ && double(*current_kit_) == k) {
				return;
			}
			on_kit_changed(int32_t(k));
			if (kits_.defined()) {
				update_kits(r_host, kits_);
			}
		} else if (p_key == "Kits") {
			kits_ = p_value;
			update_kits(r_host, p_value);
		} else if (p_key == "ScoreboardEnabled") {
			scoreboard_enabled_ = p_value.truthy();
		}
	}

	void bind(const HudModel &p_model, const HudClientState &p_client, HudData &r_out) const override {
		r_out.push_back({ "SelectedKit", As2Value::of(p_model.kit.selected_kit) });
		As2Value kits = As2Value::array();
		for (size_t i = 0; i < p_model.customization.kits_data.size(); ++i) {
			const HudKitEntry &k = p_model.customization.kits_data[i];
			As2Value kit = As2Value::object();
			kit.set("HeaderIcon", As2Value::of(std::string(kGameUtilsKits[i].label)));
			kit.set("Label", As2Value::of(localised(p_client, k.name_sid_hash)));
			As2Value slots = As2Value::array();
			for (const HudItem &item : k.items) {
				As2Value slot = As2Value::object();
				slot.set("Label", As2Value::of(localised(p_client, item.name_sid_hash)));
				if (const std::string image = hud_string(item.image_path); !image.empty()) {
					slot.set("ItemImage", As2Value::of(image));
				}
				slots.push(std::move(slot));
			}
			kit.set("SlotsData", std::move(slots));
			kits.push(std::move(kit));
		}
		r_out.push_back({ "Kits", std::move(kits) });
		r_out.push_back({ "ScoreboardEnabled", As2Value::of(p_model.team.scoreboard_enabled) });
	}

	void in_event(HudWidgetHost &, int32_t p_index, const As2Value &) override {
		if (p_index == kStepLeft) {
			step_kit(-1);
		} else if (p_index == kStepRight) {
			step_kit(1);
		} else if (p_index == kDoneSelectingSlotItem) {
			done_selecting_slot_item();
		}
	}

	bool input_concept(HudWidgetHost &, int32_t p_concept, bool p_pressed) override {
		if (!p_pressed) {
			return false;
		}
		switch (p_concept) {
			case int32_t(UiInputConcept::NavigateLeft):
				if (!changing_ && !win32_) {
					step_kit(-1);
				}
				break;
			case int32_t(UiInputConcept::NavigateRight):
				if (!changing_ && !win32_) {
					step_kit(1);
				}
				break;
			case int32_t(UiInputConcept::Tab):
				if (!win32_) {
					if (changing_) {
						hide_grid();
					} else if (!scoreboard_enabled_) {
						show_grid();
					}
				}
				break;
			case int32_t(UiInputConcept::Deactivate):
				if (!win32_ && changing_) {
					hide_grid();
				}
				break;
			case int32_t(UiInputConcept::TabLeft):
				set_selected_slot((current_slot_.value_or(0) - 1 + kSlotCount) % kSlotCount);
				break;
			case int32_t(UiInputConcept::TabRight):
				set_selected_slot((current_slot_.value_or(0) + 1) % kSlotCount);
				break;
			default:
				break;
		}
		return false;
	}

	int32_t current_kit() const override { return current_kit_.value_or(-1); }
	int32_t current_slot() const override { return current_slot_.value_or(-1); }
	bool changing_item() const override { return changing_; }

	void slot_clicked(int32_t p_slot) override {
		if (changing_ && current_slot_ == p_slot) {
			hide_grid();
			return;
		}
		set_selected_slot(p_slot);
		show_grid();
	}

	void kit_button_pressed(const std::string &p_name) override {
		const size_t at = p_name.rfind('_');
		const double k = As2Value::of(p_name.substr(at == std::string::npos ? 0 : at + 1)).to_number();
		if (current_kit_ && double(*current_kit_) == k) {
			return;
		}
		on_kit_changed(int32_t(k));
		if (kits_.defined()) {
			update_kits(*host_, kits_);
		}
	}

private:
	static constexpr float kTextGap = 10.0f;
	static constexpr uint32_t kProgressColor = 7920639u;
	static constexpr uint32_t kProgressGlowColor = 47871u;

	static std::string localised(const HudClientState &p_client, uint32_t p_hash) {
		return p_hash != 0 && p_client.localise_hash ? p_client.localise_hash(p_hash) : std::string();
	}

	As2Clip slot_button(int32_t p_index) const { return host_->clip("slotButton_" + std::to_string(p_index)); }
	HudButton *button_at(const std::string &p_name) const {
		return controller_at<HudButton>(*host_, host_->clip(p_name));
	}

	void setup_platform_specific_items(HudWidgetHost &r_host) {
		if (!win32_) {
			return;
		}
		r_host.clip("consoleLeftBtn").set_visible(false);
		r_host.clip("consoleRightBtn").set_visible(false);
		for (int32_t i = 0; i < kSlotCount; ++i) {
			if (HudButton *b = initialized_button(r_host, "slotButton_" + std::to_string(i))) {
				HudButtonCallbacks c;
				c.over = [this, i](const std::string &) { on_slot_mouse_over(i); };
				c.out = [this](const std::string &) { on_slot_mouse_over(-1); };
				c.release = [this, i](const std::string &) { slot_clicked(i); };
				b->set_callbacks(std::move(c));
			}
		}
	}

	static HudButton *initialized_button(HudWidgetHost &r_host, const std::string &p_name) {
		HudScreen &screen = r_host.screen();
		const As2Clip mc = r_host.clip(p_name);
		HudWidgetController *controller = screen.controller(mc.path());
		HudWidgetHost *mc_host = screen.host(mc.path());
		HudButton *b = dynamic_cast<HudButton *>(controller);
		if (controller == nullptr || mc_host == nullptr || b == nullptr) {
			return nullptr;
		}
		controller->initialize(*mc_host, HudWidgetInit{});
		return b;
	}

	void setup_buttons(HudWidgetHost &r_host) {
		for (int32_t i = 0; i < kKitCount; ++i) {
			HudButton *b = initialized_button(r_host, "kitButton_" + std::to_string(i));
			if (b == nullptr) {
				continue;
			}
			HudButtonCallbacks c;
			c.release = [this](const std::string &p_name) { kit_button_pressed(p_name); };
			b->set_callbacks(std::move(c));
			if (current_kit_ == i) {
				b->set_selected(true);
			}
		}
	}

	void on_slot_mouse_over(int32_t p_index) {
		if (!win32_ || changing_) {
			return;
		}
		const As2Clip text = host_->clip("changeItemText");
		if (p_index >= 0 && p_index < kSlotCount) {
			center_on_slot(text, text.text_width(), 0.0f, p_index);
			text.set_visible(true);
		} else {
			text.set_visible(false);
		}
	}

	void done_selecting_slot_item() {
		if (!changing_) {
			return;
		}
		changing_ = false;
		hide_grid();
		host_->request_focus();
	}

	void set_selected_slot(int32_t p_index) {
		for (int32_t i = 0; i < kSlotCount; ++i) {
			if (HudButton *b = button_at("slotButton_" + std::to_string(i))) {
				b->set_selected(false);
			}
		}
		if (p_index < 0 || p_index >= kSlotCount) {
			return;
		}
		if (HudButton *b = button_at("slotButton_" + std::to_string(p_index))) {
			b->set_selected(true);
		}
		current_slot_ = p_index;
		if (!win32_) {
			const As2Clip btn = host_->clip("changeItemBtn");
			const float w = btn.width();
			center_on_slot(btn, w, w / 2.0f, p_index);
		}
		host_->fire_event(kSetSlot, As2Value::of(p_index));
	}

	void center_on_slot(const As2Clip &p_mc, float p_width, float p_offset, int32_t p_index) const {
		const float offset = float(as2_round(p_offset));
		const As2Clip slot = slot_button(p_index);
		p_mc.set_x(slot.x() + (slot.width() - p_width) / 2.0f + offset);
		const float left = slot_button(0).x();
		const float right = slot_button(kSlotCount - 1).x() + slot_button(kSlotCount - 1).width();
		if (p_mc.x() - offset < left) {
			p_mc.set_x(left + offset);
		}
		if (p_mc.x() + p_width - offset > right) {
			p_mc.set_x(right - p_width + offset);
		}
	}

	void update_kits(HudWidgetHost &r_host, const As2Value &p_kits) {
		for (size_t i = 0; i < p_kits.size(); ++i) {
			if (HudButton *b = button_at("kitButton_" + std::to_string(i))) {
				b->update_button_icon(p_kits.at(i).field("HeaderIcon").to_string());
			}
		}
		const As2Value &kit = p_kits.at(size_t(std::max(current_kit_.value_or(0), 0)));
		update_kit_items(r_host, kit);
		update_kit_progression(kit.field("BarValues"));
		r_host.clip("kitName").set_text(kit.field("Label").to_string());
	}

	void update_kit_items(HudWidgetHost &r_host, const As2Value &p_kit) {
		const As2Value &slots = p_kit.field("SlotsData");
		for (int32_t i = 0; i < kSlotCount; ++i) {
			const As2Value &slot = slots.at(size_t(i));
			const As2Clip item = r_host.clip("kitItem_" + std::to_string(i));
			if (ScrollTextWidget *t = controller_at<ScrollTextWidget>(r_host, item)) {
				t->set_text(slot.field("Label").to_string());
			}
			const As2Clip icon = r_host.clip("icon_" + std::to_string(i));
			const As2Value &image = slot.field("ItemImage");
			if (image.defined() && game_utils_is_image_path(image)) {
				ImageManagerWidget *m = controller_at<ImageManagerWidget>(r_host, icon);
				HudWidgetHost *icon_host = r_host.screen().host(icon.path());
				if (m != nullptr && icon_host != nullptr) {
					m->update_image_url(*icon_host, image.to_string());
				}
				icon.set_visible(true);
			} else {
				icon.set_visible(false);
			}
		}
	}

	void update_kit_progression(const As2Value &p_values) {
		HudAdvancedBar *bar = controller_at<HudAdvancedBar>(*host_, host_->clip("progressBar"));
		if (bar == nullptr) {
			return;
		}
		if (!bar_initialized_) {
			bar->initialize_bars({ HudBarSpec{ true, 100.0f, kProgressColor, false, 1.0, 0.0 },
					HudBarSpec{ true, 100.0f, 0xFFFFFFu, false, 1.0, 0.0 } });
			bar->set_use_glow(true);
			bar->set_glow_color(kProgressGlowColor);
			bar_initialized_ = true;
		}
		const As2Clip progress = host_->clip("progressBar");
		if (!p_values.defined()) {
			progress.set_visible(false);
			return;
		}
		progress.set_visible(true);
		for (size_t i = 0; i < p_values.size(); ++i) {
			const As2Value &v = p_values.at(i);
			bar->set_value_on_index(i, v.defined() ? v : As2Value::of(0));
		}
	}

	void on_kit_changed(int32_t p_kit) {
		current_kit_ = p_kit;
		for (int32_t i = 0; i < kKitCount; ++i) {
			if (HudButton *b = button_at("kitButton_" + std::to_string(i))) {
				b->set_selected(false);
			}
		}
		if (HudButton *b = button_at("kitButton_" + std::to_string(p_kit))) {
			b->set_selected(true);
		}
		host_->fire_query(kSoundNavigate, As2Value::of(false));
		host_->fire_event(kKitChanged, As2Value::of(p_kit));
		host_->fire_event(kSetSlot, current_slot_ ? As2Value::of(*current_slot_) : As2Value::undefined());
	}

	void step_kit(int32_t p_step) {
		const char *arrow = p_step < 0 ? "consoleLeftBtn" : "consoleRightBtn";
		if (HudButton *b = button_at(arrow)) {
			b->set_selected(true);
		}
		on_kit_changed((current_kit_.value_or(0) + p_step + kKitCount) % kKitCount);
	}

	void show_grid() {
		if (win32_) {
			host_->clip("changeItemText").set_visible(false);
		} else {
			host_->clip("changeItemBtn").set_visible(false);
		}
		changing_ = true;
		host_->fire_event(kShowGrid, As2Value::of(0));
		host_->fire_query(kSoundNavigate, As2Value::of(false));
	}

	void hide_grid() {
		if (win32_) {
			set_selected_slot(-1);
			host_->clip("changeItemText").set_visible(false);
		} else {
			host_->clip("changeItemBtn").set_visible(true);
		}
		changing_ = false;
		host_->fire_event(kHideGrid, As2Value::of(0));
		host_->fire_query(kSoundNavigate, As2Value::of(false));
	}

	HudWidgetHost *host_ = nullptr;
	std::optional<int32_t> current_kit_;
	std::optional<int32_t> current_slot_;
	bool win32_ = true;
	bool changing_ = false;
	bool bar_initialized_ = false;
	bool scoreboard_enabled_ = false;
	As2Value kits_;
};

}

void register_hud_widgets_kit_bar(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Bar.KitBar", [] { return std::make_unique<KitBarClass>(); });
}

}
