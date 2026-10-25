#pragma once

#include "bf3/core/math.h"
#include "bf3/core/ui/hud_messages.h"
#include "bf3/core/ui/hud_screen.h"
#include "bf3/core/ui/hud_widget_registry.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/swf_text.h"
#include "bf3/core/ui/ui_input.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace bf3::core::ui {

inline constexpr std::string_view kSoundSelect = "UIWidgetEventID_PlaySoundSelect";
inline constexpr std::string_view kSoundNavigate = "UIWidgetEventID_PlaySoundNavigate";

inline bool hud_platform_win32(const HudClientState &p_client) {
	return p_client.platform == "win32" || p_client.platform.empty();
}

inline SwfFilter as2_glow_filter(uint32_t p_rgb, double p_alpha, float p_blur_x, float p_blur_y, float p_strength,
		int32_t p_quality, bool p_inner = false, bool p_knockout = false) {
	SwfFilter f;
	f.kind = SwfFilterKind::Glow;
	f.color = SwfColor{ uint8_t((p_rgb >> 16) & 0xffu), uint8_t((p_rgb >> 8) & 0xffu), uint8_t(p_rgb & 0xffu),
		uint8_t(std::lround(std::clamp(p_alpha, 0.0, 1.0) * 255.0)) };
	f.blur_x = p_blur_x;
	f.blur_y = p_blur_y;
	f.strength = p_strength;
	f.inner = p_inner;
	f.knockout = p_knockout;
	f.composite_source = true;
	f.passes = uint8_t(std::clamp(p_quality, 0, 15));
	return f;
}

inline bool string_true(const As2Value &p_v) {
	return to_lower_ascii(p_v.to_string()) == "true";
}

inline bool hud_soldier_widgets_visible(const HudModel &p_model, const HudClientState &p_client) {
	return !p_client.hardcore && !p_model.user.is_in_menu && !p_model.kill_card.visible &&
			!p_model.spawn_screen.visible && !p_model.end_of_round.visible;
}

inline std::vector<std::string> as2_split(std::string_view p_text, std::string_view p_separator) {
	std::vector<std::string> out;
	size_t at = 0;
	for (size_t hit = p_text.find(p_separator); hit != std::string_view::npos; hit = p_text.find(p_separator, at)) {
		out.emplace_back(p_text.substr(at, hit - at));
		at = hit + p_separator.size();
	}
	out.emplace_back(p_text.substr(at));
	return out;
}

inline bool as2_loose_equals(const As2Value &p_a, const As2Value &p_b) {
	if (p_a.kind == As2Value::Kind::Undefined || p_b.kind == As2Value::Kind::Undefined) {
		return p_a.kind == p_b.kind;
	}
	if (p_a.kind == As2Value::Kind::String && p_b.kind == As2Value::Kind::String) {
		return p_a.text == p_b.text;
	}
	if (p_a.kind == As2Value::Kind::Object || p_a.kind == As2Value::Kind::List || p_b.kind == As2Value::Kind::Object ||
			p_b.kind == As2Value::Kind::List) {
		return p_a == p_b;
	}
	return p_a.to_number() == p_b.to_number();
}

inline bool as2_equals_true(const As2Value &p_v) {
	return p_v.kind == As2Value::Kind::Bool ? p_v.boolean : p_v.to_number() == 1.0;
}

inline bool as2_equals_false(const As2Value &p_v) {
	return p_v.kind == As2Value::Kind::Bool ? !p_v.boolean : p_v.to_number() == 0.0;
}

inline std::string_view clip_leaf(std::string_view p_path) {
	const size_t dot = p_path.rfind('.');
	return dot == std::string_view::npos ? p_path : p_path.substr(dot + 1);
}

inline double as2_round(double p_v) {
	return std::floor(p_v + 0.5);
}

inline std::string param_string(const HudWidgetHost &p_host, std::string_view p_name) {
	const As2Value *v = p_host.construct_param(p_name);
	return v != nullptr ? v->to_string() : std::string("undefined");
}

inline void setup_refresh_data(HudWidgetController &r_ctl, HudWidgetHost &r_host, const HudData &p_data) {
	for (const HudDataEntry &e : p_data) {
		if (e.value.defined()) {
			r_ctl.update(r_host, e.key, e.value);
		}
	}
}

template <class T>
inline T *controller_at(HudWidgetHost &r_host, const As2Clip &p_clip) {
	return dynamic_cast<T *>(r_host.screen().controller(p_clip.path()));
}

class HitAnimation {
public:
	void watch(const As2Clip &p_holder) {
		const uint32_t f = p_holder.current_frame();
		const As2Clip controller = p_holder.child("HitController");
		const uint32_t cf = controller.current_frame();
		if (f != last_holder_ && (f == kLevelLowFrame || f == kLevelHighFrame)) {
			controller.play();
		}
		if (cf != last_controller_ && cf == kControllerPlayFrame) {
			p_holder.play();
		}
		last_holder_ = p_holder.current_frame();
		last_controller_ = controller.current_frame();
	}

private:
	static constexpr uint32_t kLevelLowFrame = 2;
	static constexpr uint32_t kLevelHighFrame = 13;
	static constexpr uint32_t kControllerPlayFrame = 2;
	uint32_t last_holder_ = 0;
	uint32_t last_controller_ = 0;
};

inline constexpr std::array<int32_t, 4> kHitLevelFrames{ 0, 2, 2, 13 };

inline bool param_true(const HudWidgetHost &p_host, std::string_view p_name) {
	const As2Value *v = p_host.construct_param(p_name);
	if (v == nullptr) {
		return false;
	}
	return v->kind == As2Value::Kind::Bool ? v->boolean : string_true(*v);
}

inline std::string as2_id(const As2Value &p_id) {
	return p_id.to_string();
}

inline double as2_number(const As2Value &p_v) {
	if (p_v.kind == As2Value::Kind::List || p_v.kind == As2Value::Kind::NumberList) {
		return As2Value::of(p_v.to_string()).to_number();
	}
	return p_v.to_number();
}

inline int32_t as2_int(const As2Value &p_v) {
	const double n = as2_number(p_v);
	return std::isfinite(n) ? int32_t(std::trunc(n)) : 0;
}

inline void goto_and_stop_number(const As2Clip &p_clip, double p_frame) {
	if (std::isfinite(p_frame)) {
		p_clip.goto_and_stop(int32_t(p_frame));
	}
}

}
