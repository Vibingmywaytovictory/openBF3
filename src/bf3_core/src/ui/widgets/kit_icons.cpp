#include "kit_icons.h"

#include "hud_widget_support.h"

namespace bf3::core::ui {

namespace {

constexpr std::string_view kStringSeparator = ";;";
constexpr std::string_view kArrayOpen = "[[";
constexpr std::string_view kArraySeparator = ",,";
constexpr std::string_view kImagePathPrefix = "UI/Art/";
constexpr size_t kObjectToArrayMax = 65536;

std::string as2_substr(std::string_view p_text, size_t p_start, int64_t p_length) {
	if (p_start >= p_text.size() || p_length <= 0) {
		return {};
	}
	return std::string(p_text.substr(p_start, size_t(p_length)));
}

size_t as2_string_length(std::string_view p_text) {
	size_t n = 0;
	for (const char c : p_text) {
		n += (uint8_t(c) & 0xC0u) != 0x80u ? 1 : 0;
	}
	return n;
}

}

std::string as2_service_stars_text(double p_stars) {
	if (p_stars < 0.0) {
		p_stars = 0.0;
	}
	return p_stars > 100.0 ? std::string("100") : as2_number_string(p_stars);
}

void KitIconsWidget::construct(HudWidgetHost &r_host) {
	service_stars_ = r_host.clip("serviceStars");
	service_stars_.set_visible(false);
}

void KitIconsWidget::set_stars(double p_stars) const {
	service_stars_.child("txt").set_text(as2_service_stars_text(p_stars));
	service_stars_.set_visible(true);
}

void ServiceStarsWidget::construct(HudWidgetHost &r_host) {
	txt_ = r_host.clip("txt");
	txt_.set_text("");
}

void ServiceStarsWidget::set_stars(double p_stars) const {
	txt_.set_text(as2_service_stars_text(p_stars));
}

float ServiceStarsWidget::width() const {
	return txt_.x() + txt_.text_width();
}

std::string game_utils_kit_frame_label(std::string_view p_kit) {
	for (const GameUtilsKit &k : kGameUtilsKits) {
		if (p_kit == k.us || p_kit == k.ru || p_kit == k.sid) {
			return std::string(k.label);
		}
	}
	return {};
}

bool game_utils_is_image_path(const As2Value &p_path) {
	return std::string_view(p_path.to_string()).substr(0, kImagePathPrefix.size()) == kImagePathPrefix;
}

As2Value game_utils_string2object(std::string_view p_text) {
	As2Value obj = As2Value::object();
	const std::vector<std::string> parts = as2_split(p_text, kStringSeparator);
	for (size_t i = 0; i < parts.size(); i += 2) {
		if (i + 1 >= parts.size()) {
			obj.set(parts[i], As2Value::undefined());
			continue;
		}
		const std::string &value = parts[i + 1];
		if (std::string_view(value).substr(0, kArrayOpen.size()) == kArrayOpen) {
			As2Value list = As2Value::array();
			for (std::string &item : as2_split(as2_substr(value, 2, int64_t(value.size()) - 4), kArraySeparator)) {
				list.push(As2Value::of(std::move(item)));
			}
			obj.set(parts[i], std::move(list));
		} else {
			obj.set(parts[i], As2Value::of(value));
		}
	}
	return obj;
}

std::string game_utils_object2string(const As2Value &p_object) {
	std::string out;
	for (size_t i = 0; i < p_object.names.size() && i < p_object.items.size(); ++i) {
		if (i > 0) {
			out += ";;";
		}
		const As2Value &v = p_object.items[i];
		out += p_object.names[i] + ";;";
		if (v.kind == As2Value::Kind::List) {
			out += "[[";
			for (size_t j = 0; j < v.items.size(); ++j) {
				out += (j > 0 ? ",," : "") + v.items[j].to_string();
			}
			out += "]]";
		} else {
			out += v.to_string();
		}
	}
	return out;
}

std::optional<As2Value> ui_base_object_to_array(const As2Value &p_object) {
	As2Value out = As2Value::array();
	switch (p_object.kind) {
		case As2Value::Kind::List:
			out.items = p_object.items;
			return out;
		case As2Value::Kind::NumberList:
			for (const double n : p_object.list) {
				out.push(As2Value::of(n));
			}
			return out;
		case As2Value::Kind::String:
			out.items.resize(std::min(as2_string_length(p_object.text), kObjectToArrayMax));
			return out;
		case As2Value::Kind::Object: {
			const As2Value &length = p_object.field("length");
			if (!length.defined()) {
				return std::nullopt;
			}
			const double n = length.to_number();
			for (size_t i = 0; double(i) < n && i < kObjectToArrayMax; ++i) {
				out.push(p_object.field(std::to_string(i)));
			}
			return out;
		}
		case As2Value::Kind::Undefined:
		case As2Value::Kind::Bool:
		case As2Value::Kind::Number:
			return std::nullopt;
	}
	return std::nullopt;
}

void register_hud_widgets_kit_icons(HudWidgetRegistry &r_registry) {
	r_registry.add("Shared.KitIcons", [] { return std::make_unique<KitIconsWidget>(); });
	r_registry.add("Shared.ServiceStars", [] { return std::make_unique<ServiceStarsWidget>(); });
}

}
