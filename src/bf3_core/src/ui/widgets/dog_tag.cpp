#include "dog_tag.h"

#include "hud_widget_support.h"
#include "image_manager.h"

#include "bf3/core/ui/ui_input.h"

#include <array>

namespace bf3::core::ui {

namespace {

constexpr std::array<std::string_view, 4> kDogTagTextLayers{ "bg_text", "inner_glow_text", "bottom_drop_text",
	"top_drop_text" };

bool as2_set(const As2Value &p_v) {
	return p_v.defined() && !(p_v.kind == As2Value::Kind::String && p_v.text.empty());
}

}

void DogTagTextWidget::construct(HudWidgetHost &r_host) {
	host_ = &r_host;
}

void DogTagTextWidget::set_text(std::string_view p_text) {
	text_ = std::string(p_text);
	const std::string shown = host_->translate(p_text);
	for (const std::string_view layer : kDogTagTextLayers) {
		const As2Clip field = host_->clip(layer);
		field.set_width(kTextFieldWidth);
		field.set_text(shown);
		field.set_text_fit(SwfTextFit::Shrink);
	}
}

void DogTagWidget::construct(HudWidgetHost &r_host) {
	host_ = &r_host;
}

void DogTagWidget::initialize_tag(std::string_view p_type, bool p_is_static) {
	type_ = std::string(p_type);
	is_static_ = p_is_static;
	add_image_manager();
}

void DogTagWidget::add_image_manager() {
	const As2Clip &self = host_->self();
	if (!image_manager_.exists()) {
		image_manager_ = host_->attach_movie(self, "ImageManager", "ImageManager", self.next_highest_depth());
	}
	image_manager_.set_width(kTagWidth);
	image_manager_.set_height(kTagHeight);
	if (ImageManagerWidget *m = image_manager()) {
		m->resize_to_target_width = true;
		m->center_image = false;
	}
}

ImageManagerWidget *DogTagWidget::image_manager() const {
	return image_manager_.exists() ? controller_at<ImageManagerWidget>(*host_, image_manager_) : nullptr;
}

void DogTagWidget::update_dog_tag(As2Value p_data) {
	const As2Value &image = p_data.field("Image");
	if (image.kind == As2Value::Kind::String && image.text.empty()) {
		p_data.set("Image",
				As2Value::of(std::string(type_ == DogTagPairWidget::kBasic ? kInactiveBasic : kInactiveAdvanced)));
	}
	ImageManagerWidget *m = image_manager();
	HudWidgetHost *mh = m != nullptr ? host_->screen().host(image_manager_.path()) : nullptr;
	if (mh != nullptr) {
		m->update(*mh, "ImageUrl", p_data.field("Image"));
	}
	if (as2_set(p_data.field("Image"))) {
		update_text_fields(p_data);
	}
}

void DogTagWidget::update_text_fields(const As2Value &p_data) {
	const bool inverted = as2_equals_true(p_data.field("InvertText"));
	const auto apply = [&](As2Clip &r_field, std::string_view p_key, bool p_right, float p_x, float p_y,
							   bool p_zero_hides) {
		const As2Value &v = p_data.field(p_key);
		const bool show = as2_set(v) && !(p_zero_hides && v.to_string() == "0");
		if (show) {
			place_field(r_field, p_right, inverted, p_x, p_y, v);
		} else if (r_field.exists()) {
			r_field.set_visible(false);
		}
	};
	if (type_ == DogTagPairWidget::kBasic) {
		apply(name_field_, "PlayerName", false, kGamertagX, kGamertagY, false);
		apply(clan_field_, "ClanTag", false, kClanX, kClanY, false);
		return;
	}
	apply(tag_name_field_, "StatSID", true, kStatNameX, kStatNameY, false);
	apply(stats_field_, "YourStatValue", true, kStatsX, kStatsY, true);
}

void DogTagWidget::place_field(
		As2Clip &r_field, bool p_right_aligned, bool p_inverted, float p_x, float p_y, const As2Value &p_content) {
	r_field = text_field(r_field, p_right_aligned, p_inverted);
	r_field.set_x(p_x);
	r_field.set_y(p_y);
	if (DogTagTextWidget *t = r_field.exists() ? controller_at<DogTagTextWidget>(*host_, r_field) : nullptr) {
		t->set_text(p_content.to_string());
	}
}

As2Clip DogTagWidget::text_field(As2Clip p_field, bool p_right_aligned, bool p_inverted) {
	if (p_field.exists()) {
		const DogTagTextWidget *t = controller_at<DogTagTextWidget>(*host_, p_field);
		if (t == nullptr || t->inverted != p_inverted) {
			host_->remove_movie_clip(p_field);
			p_field = As2Clip();
		}
	}
	if (!p_field.exists()) {
		const As2Clip &self = host_->self();
		const std::string linkage =
				std::string(p_right_aligned ? "DogTagTextRight" : "DogTagText") + (p_inverted ? "Inverted" : "");
		const int32_t depth = self.next_highest_depth();
		p_field = host_->attach_movie(self, linkage, "textField" + std::to_string(depth), depth);
		if (DogTagTextWidget *t = p_field.exists() ? controller_at<DogTagTextWidget>(*host_, p_field) : nullptr) {
			t->inverted = p_inverted;
		}
	}
	p_field.set_visible(true);
	return p_field;
}

void DogTagPairWidget::construct(HudWidgetHost &r_host) {
	host_ = &r_host;
	loaded_ = false;
	holder_ = r_host.clip("holder");
	holder_.child("sizeClip").set_visible(false);
	advanced_ = r_host.attach_movie(holder_, "DogTag", "advancedTag", holder_.next_highest_depth());
	basic_ = r_host.attach_movie(holder_, "DogTag", "basicTag", holder_.next_highest_depth());
}

DogTagWidget *DogTagPairWidget::basic() const {
	return basic_.exists() ? controller_at<DogTagWidget>(*host_, basic_) : nullptr;
}

DogTagWidget *DogTagPairWidget::advanced() const {
	return advanced_.exists() ? controller_at<DogTagWidget>(*host_, advanced_) : nullptr;
}

void DogTagPairWidget::initialize_tag(
		const As2Clip &p_tag, std::string_view p_type, float p_x, float p_y, float p_rotation) {
	DogTagWidget *tag = p_tag.exists() ? controller_at<DogTagWidget>(*host_, p_tag) : nullptr;
	if (tag == nullptr) {
		return;
	}
	tag->initialize_tag(p_type, static_);
	if (ImageManagerWidget *m = tag->image_manager()) {
		m->register_callback([this](std::string_view, ImageManagerWidget &) { image_callback(); });
	}
	p_tag.set_x(p_x);
	p_tag.set_y(p_y);
	p_tag.set_rotation(p_rotation);
}

void DogTagPairWidget::initialize_pair(const As2Value &p_init_data) {
	loaded_ = false;
	const As2Value &p_static = p_init_data.field("p_static");
	const bool static_param = p_static.defined() && as2_parse_int(p_static) == 1.0;
	static_ = static_param || param_true(*host_, "f_static");
	initialize_tag(basic_, kBasic, kBasicX, kBasicY, kBasicRotation);
	initialize_tag(advanced_, kAdvanced, kAdvancedX, kAdvancedY, kAdvancedRotation);
	const As2Value *selected = host_->construct_param("p_defaultSelected");
	if (selected != nullptr && selected->defined() && selected->to_string() != kBasic) {
		advanced_.swap_depths(basic_);
	}
	if (!static_) {
		host_->fire_event(kCurrentlySelectedTag, As2Value::of(0));
		set_color(basic_, false);
		set_color(advanced_, true);
	}
	const As2Value &data = p_init_data.field("data");
	if (data.kind == As2Value::Kind::Object) {
		for (size_t i = 0; i < data.names.size() && i < data.items.size(); ++i) {
			if (data.items[i].defined()) {
				update(*host_, data.names[i], data.items[i]);
			}
		}
	}
}

void DogTagPairWidget::set_color(const As2Clip &p_tag, bool p_inactive) {
	As2ColorTransformPatch t;
	t.aa = 100.0;
	t.ab = 0.0;
	t.ra = t.ga = t.ba = p_inactive ? 30.0 : 100.0;
	t.rb = p_inactive ? 7.0 : 0.0;
	t.gb = t.bb = p_inactive ? 11.0 : 0.0;
	p_tag.set_color_transform(t);
}

void DogTagPairWidget::switch_active_tag() {
	const bool to_advanced = selected_ == kBasic;
	selected_ = std::string(to_advanced ? kAdvanced : kBasic);
	host_->fire_event(kCurrentlySelectedTag, As2Value::of(to_advanced ? 1 : 0));
	set_color(basic_, to_advanced);
	set_color(advanced_, !to_advanced);
	advanced_.swap_depths(basic_);
}

bool DogTagPairWidget::input_concept(HudWidgetHost &, int32_t p_concept, bool p_pressed) {
	if (!p_pressed || static_) {
		return false;
	}
	if ((p_concept == int32_t(UiInputConcept::TabRight) && selected_ != kAdvanced) ||
			(p_concept == int32_t(UiInputConcept::TabLeft) && selected_ != kBasic)) {
		switch_active_tag();
		return true;
	}
	return false;
}

void DogTagPairWidget::image_callback() {
	const DogTagWidget *b = basic();
	const DogTagWidget *a = advanced();
	const bool basic_loaded = b != nullptr && b->image_manager() != nullptr && b->image_manager()->is_loaded();
	const bool advanced_loaded = a != nullptr && a->image_manager() != nullptr && a->image_manager()->is_loaded();
	basic_.set_visible(basic_loaded);
	advanced_.set_visible(advanced_loaded);
	const bool loaded = basic_loaded && advanced_loaded;
	if (loaded == loaded_) {
		return;
	}
	loaded_ = loaded;
	if (static_) {
		host_->self().set_visible(loaded);
	}
	if (callback_) {
		callback_(loaded);
	}
}

void DogTagPairWidget::update(HudWidgetHost &, std::string_view p_key, const As2Value &p_value) {
	if (p_key == "basicData") {
		update_basic_data(p_value);
	} else if (p_key == "advancedData") {
		update_advanced_data(p_value);
	}
}

void DogTagPairWidget::update_basic_data(const As2Value &p_data) {
	loaded_ = false;
	if (DogTagWidget *t = basic()) {
		t->update_dog_tag(p_data);
	}
}

void DogTagPairWidget::update_advanced_data(const As2Value &p_data) {
	loaded_ = false;
	if (DogTagWidget *t = advanced()) {
		t->update_dog_tag(p_data);
	}
}

void register_hud_widgets_dog_tag(HudWidgetRegistry &r_registry) {
	r_registry.add("Widget.DogTag.DogTagPair", [] { return std::make_unique<DogTagPairWidget>(); });
	r_registry.add("Widget.DogTag.DogTag", [] { return std::make_unique<DogTagWidget>(); });
	r_registry.add("Widget.DogTag.DogTagText", [] { return std::make_unique<DogTagTextWidget>(); });
}

}
