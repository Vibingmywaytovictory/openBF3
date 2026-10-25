#include "button.h"

namespace bf3::core::ui {

namespace {

constexpr int32_t kPopupReleased = 0;
constexpr float kButtonSpace = 12.0f;

As2Value popup_buttons(const HudPopup &p_popup) {
	As2Value out = As2Value::array();
	for (const HudPopupButton &b : p_popup.buttons) {
		As2Value item = As2Value::object();
		item.set("Label", As2Value::of(b.label));
		item.set("Concept", As2Value::of(b.input_concept));
		out.push(std::move(item));
	}
	return out;
}

class PopupGenericWidgetClass final : public HudWidgetController {
public:
	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override { sources_ = p_node.sources; }

	void on_clip_load(HudWidgetHost &r_host) override { r_host.self().set_visible(false); }

	void bind(const HudModel &, const HudClientState &p_client, HudData &r_out) const override {
		if (p_client.popup == nullptr) {
			return;
		}
		for (const HudDataSource &src : sources_) {
			if (src.name == "Title") {
				r_out.push_back({ src.name, As2Value::of(p_client.popup->title), src.on_initialize });
			} else if (src.name == "Text") {
				r_out.push_back({ src.name, As2Value::of(p_client.popup->text), src.on_initialize });
			}
		}
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key == "Title" && !title_set_) {
			r_host.clip("popupTitle").set_text(r_host.translate(p_value.to_string()));
			title_set_ = true;
		} else if (p_key == "Text" && !text_set_) {
			r_host.clip("popupText").set_text(r_host.translate(p_value.to_string()));
			text_set_ = true;
		}
	}

private:
	std::vector<HudDataSource> sources_;
	bool title_set_ = false;
	bool text_set_ = false;
};

class PopupButtonsManagerClass final : public HudWidgetController {
public:
	void construct(HudWidgetHost &r_host) override { host_ = &r_host; }

	void attach_node(HudWidgetHost &, const HudWidgetNode &p_node) override { sources_ = p_node.sources; }

	void on_clip_load(HudWidgetHost &r_host) override {
		const As2Clip self = r_host.self();
		orig_width_ = self.width();
		self.set_yscale(100.0f);
		self.set_xscale(100.0f);
		r_host.clip("placeholder").set_visible(false);
		self.set_visible(false);
		pc_mode_ = hud_platform_win32(r_host.client());
	}

	void bind(const HudModel &, const HudClientState &p_client, HudData &r_out) const override {
		if (p_client.popup == nullptr) {
			return;
		}
		for (const HudDataSource &src : sources_) {
			if (src.name == "Buttons") {
				r_out.push_back({ src.name, popup_buttons(*p_client.popup), src.on_initialize });
			}
		}
	}

	void initialize(HudWidgetHost &r_host, const HudWidgetInit &p_init) override {
		buttons_ = As2Value::array();
		setup_refresh_data(*this, r_host, p_init.data);
		r_host.self().set_visible(true);
	}

	void update(HudWidgetHost &r_host, std::string_view p_key, const As2Value &p_value) override {
		if (p_key != "Buttons") {
			return;
		}
		if (!holder_.empty()) {
			r_host.remove_movie_clip(r_host.screen().clip(holder_));
		}
		const As2Clip self = r_host.self();
		const As2Clip holder = r_host.create_empty_movie_clip(self, "holder", self.next_highest_depth());
		holder_ = holder.path();
		buttons_ = p_value;
		by_depth_.clear();
		selected_ = -1;
		float x = 0.0f;
		if (pc_mode_) {
			for (size_t i = 0; i < buttons_.size(); ++i) {
				const As2Value &label = buttons_.at(i).field("Label");
				if (!label.defined() || label.to_string().empty()) {
					continue;
				}
				const As2Clip mc = r_host.attach_movie(holder, "Button", "btn" + std::to_string(i), int32_t(i));
				HudWidgetController *controller = r_host.screen().controller(mc.path());
				HudWidgetHost *mc_host = r_host.screen().host(mc.path());
				HudButton *b = dynamic_cast<HudButton *>(controller);
				if (controller == nullptr || mc_host == nullptr || b == nullptr) {
					continue;
				}
				controller->on_clip_load(*mc_host);
				b->set_button_text(label.to_string());
				b->set_button_type("smallTextButton");
				b->set_align_text("center");
				b->set_resize_if_too_wide(true);
				controller->initialize(*mc_host, HudWidgetInit{});
				HudButtonCallbacks c;
				c.release = [this, i](const std::string &) { release_handler(i); };
				b->set_callbacks(std::move(c));
				mc.set_x(x);
				x += b->button_width() + kButtonSpace;
				by_depth_.emplace(int32_t(i), mc.path());
			}
			if (x > 0.0f) {
				set_selected_button(0);
			}
		}
		holder.set_x(float(as2_round(orig_width_ / 2.0f - holder.width() / 2.0f)));
	}

	bool input_concept(HudWidgetHost &r_host, int32_t p_concept, bool p_pressed) override {
		if (!p_pressed) {
			return false;
		}
		if (!pc_mode_) {
			for (size_t i = 0; i < buttons_.size(); ++i) {
				if (buttons_.at(i).field("Concept").to_number() == double(p_concept)) {
					r_host.fire_query(kSoundSelect, As2Value::of(false));
					r_host.fire_event(kPopupReleased, As2Value::of(buttons_.at(i).field("Label").to_string()));
					return false;
				}
			}
			return false;
		}
		switch (UiInputConcept(p_concept)) {
			case UiInputConcept::NavigateLeft:
				move_selection(-1);
				break;
			case UiInputConcept::NavigateRight:
				move_selection(1);
				break;
			case UiInputConcept::Activate:
				if (HudButton *b = button_at(selected_)) {
					b->press();
				}
				r_host.fire_event(kPopupReleased, As2Value::of(label_at(selected_)));
				break;
			default:
				break;
		}
		return false;
	}

	int32_t selected_button() const { return selected_; }

private:
	HudButton *button_at(int32_t p_depth) const {
		const auto it = by_depth_.find(p_depth);
		return it == by_depth_.end() ? nullptr : dynamic_cast<HudButton *>(host_->screen().controller(it->second));
	}

	std::string label_at(int32_t p_index) const {
		if (p_index < 0 || size_t(p_index) >= buttons_.size()) {
			return "undefined";
		}
		return buttons_.at(size_t(p_index)).field("Label").to_string();
	}

	void release_handler(size_t p_index) {
		host_->fire_query(kSoundSelect, As2Value::of(false));
		host_->fire_event(kPopupReleased, As2Value::of(label_at(int32_t(p_index))));
	}

	void set_selected_button(int32_t p_index) {
		if (selected_ != -1) {
			if (HudButton *b = button_at(selected_)) {
				b->set_selected(false);
			}
		}
		selected_ = p_index;
		if (HudButton *b = button_at(p_index)) {
			b->set_selected(true);
		}
	}

	void move_selection(int32_t p_dir) {
		if (p_dir < 0 && selected_ > 0) {
			set_selected_button(selected_ - 1);
			host_->fire_query(kSoundNavigate, As2Value::of(false));
		} else if (p_dir > 0 && double(selected_) < double(buttons_.size()) - 1.0) {
			set_selected_button(selected_ + 1);
			host_->fire_query(kSoundNavigate, As2Value::of(false));
		}
	}

	HudWidgetHost *host_ = nullptr;
	std::vector<HudDataSource> sources_;
	std::string holder_;
	std::map<int32_t, std::string> by_depth_;
	As2Value buttons_ = As2Value::array();
	float orig_width_ = 0.0f;
	bool pc_mode_ = true;
	int32_t selected_ = -1;
};

}

void register_hud_widgets_popup(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.Popup.PopupGenericWidget", [] { return std::make_unique<PopupGenericWidgetClass>(); });
	r_registry.add("Widget.Popup.PopupButtonsManager", [] { return std::make_unique<PopupButtonsManagerClass>(); });
}

}
