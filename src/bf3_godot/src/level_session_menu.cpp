#include "level_session_menu.h"

#include "godot_util.h"
#include "input_mapper.h"
#include "level_session.h"
#include "level_session_game.h"
#include "level_session_tdm.h"
#include "menu_check.h"
#include "native_flow.h"
#include "native_hud.h"
#include "native_screens.h"
#include "native_tags.h"
#include "overview_map.h"
#include "swf_view.h"
#include "ui_cursor.h"
#include "ui_post_process.h"

#include "bf3/host/client_session.h"
#include "bf3/host/ui_flow_session.h"

#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <charconv>
#include <string>

using namespace godot;

namespace bf3::godot_side {

namespace {

bool param_truthy(const bf3::core::ui::UiFlowAction &p_action) {
	return !p_action.params.empty() && bf3::core::ui::ui_flow_truthy(p_action.params.front());
}

float param_float(const bf3::core::ui::UiFlowAction &p_action, float p_default) {
	if (p_action.params.empty()) {
		return p_default;
	}
	const std::string &s = p_action.params.front();
	float v = p_default;
	const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
	return ec == std::errc() ? v : p_default;
}

}

void NativeMenu::build(Bf3UiScreens &r_screens) {
	for (const std::string_view p : kMenuDrawnPartitions) {
		r_screens.set_drawn(p, true);
	}
}

bool NativeMenu::take(const bf3::core::ui::UiFlowAction &p_action) {
	const std::string_view n = p_action.name;
	if (n != kUiGameInputAction && n != kUiMouseInputAction && n != kUiMouseCursorAction &&
			n != kUiCameraChangedAction && n != kUiSuicideAction && n != kUiQuitGameAction && n != kUiShutDownAction) {
		return false;
	}
	if (pending_.size() >= kNativeMenuMaxPending) {
		++stats_.dropped;
		return true;
	}
	pending_.push_back(p_action);
	return true;
}

void NativeMenu::frame(Bf3UiScreens &r_screens, const bf3::host::UiFlowSession *p_session, const NativeMenuIo &p_io) {
	std::vector<bf3::core::ui::UiFlowAction> pending;
	pending.swap(pending_);
	for (const bf3::core::ui::UiFlowAction &a : pending) {
		if (quit_requested_) {
			break;
		}
		apply(a, p_io);
	}
	const bool in_menu = p_session != nullptr &&
			bf3::core::ui::ui_flow_truthy(p_session->data(kUserCompCategory, kIsInMenuKey).value_or(std::string()));
	if (in_menu != in_menu_) {
		in_menu_ = in_menu;
		if (Bf3UiPostProcess *post = Bf3UiPostProcess::ensure(p_io.parent, p_io.level)) {
			if (in_menu) {
				post->dim_in();
				++stats_.dims_in;
			} else {
				post->dim_out();
				++stats_.dims_out;
			}
		}
	}
	show_cursor(p_io, cursor_wanted_ && mouse_input_ && !game_input_);
	attach_overview(r_screens, p_io);
}

void NativeMenu::apply(const bf3::core::ui::UiFlowAction &p_action, const NativeMenuIo &p_io) {
	const std::string_view n = p_action.name;
	if (n == kUiGameInputAction) {
		game_input_ = param_truthy(p_action);
		++(game_input_ ? stats_.game_input_on : stats_.game_input_off);
		if (p_io.mapper != nullptr) {
			p_io.mapper->set_game_input(game_input_);
		}
	} else if (n == kUiMouseInputAction) {
		mouse_input_ = param_truthy(p_action);
	} else if (n == kUiMouseCursorAction) {
		cursor_wanted_ = param_truthy(p_action);
	} else if (n == kUiCameraChangedAction) {
		if (Bf3UiPostProcess *post = Bf3UiPostProcess::ensure(p_io.parent, p_io.level)) {
			post->trigger_glitch(param_float(p_action, 0.0f));
			++stats_.glitches;
		}
	} else if (n == kUiSuicideAction) {
		++stats_.suicides;
		if (p_io.suicide) {
			p_io.suicide();
		}
	} else if (n == kUiQuitGameAction || n == kUiShutDownAction) {
		++stats_.quits;
		quit_requested_ = true;
	}
}

void NativeMenu::show_cursor(const NativeMenuIo &p_io, bool p_shown) {
	if (p_shown == cursor_shown_) {
		return;
	}
	Bf3UiCursor *cursor = Bf3UiCursor::ensure(p_io.parent);
	if (cursor == nullptr) {
		return;
	}
	cursor_shown_ = p_shown;
	cursor->set_follow_mouse(p_shown);
	if (p_shown) {
		cursor->show_cursor();
		++stats_.cursor_shown;
	} else {
		cursor->hide_cursor();
		++stats_.cursor_hidden;
	}
}

void NativeMenu::attach_overview(Bf3UiScreens &r_screens, const NativeMenuIo &p_io) {
	const UiScreenSlot slot = r_screens.find_screen(kIngameMenuPartition);
	if (slot.view == nullptr) {
		overview_view_ = 0;
		return;
	}
	Bf3OverviewMap *overview = Bf3OverviewMap::ensure(p_io.parent);
	if (overview == nullptr) {
		return;
	}
	const uint64_t view_id = slot.view->get_instance_id();
	if (overview_view_ != view_id) {
		if (overview->attach(slot.view)) {
			overview_view_ = view_id;
			++stats_.overview_attached;
		}
	}
	std::vector<OverviewMark> marks =
			p_io.tags != nullptr ? overview_marks(p_io.tags->minimap()) : std::vector<OverviewMark>{};
	if (p_io.local) {
		marks.push_back(*p_io.local);
	}
	overview->set_marks(std::move(marks));
}

}

namespace bf3::godot_side {

bool Bf3LevelSession::tdm_menu_frame(double p_now_s) {
	if (!game_ || !game_->tdm || game_->tdm->native_menu == nullptr || game_->tdm->native_flow == nullptr) {
		return false;
	}
	TdmState &m = *game_->tdm;
	Bf3UiScreens *screens = tdm_ui_screens(m);
	if (screens == nullptr) {
		return false;
	}
	NativeMenuIo io;
	io.parent = this;
	io.level = level_id();
	io.mapper = &mapper_;
	io.tags = game_->native_hud != nullptr ? game_->native_hud->tags() : nullptr;
	if (game_->native_hud != nullptr && game_->alive && game_->native_hud->minimap_pose().valid) {
		const NativeHudMinimapPose &pose = game_->native_hud->minimap_pose();
		io.local = OverviewMark{ OverviewMarkKind::Local, pose.x, pose.z, pose.yaw };
	}
	io.suicide = [this] {
		if (host::ClientSession *c = client()) {
			c->send_suicide_request();
		}
	};
	m.native_menu->frame(*screens, m.native_flow->session(), io);
#if OPENBF3_BUILD_CHECKS
	if (m.menu_check_on && m.menu_check == nullptr) {
		m.menu_check = std::unique_ptr<MenuCheck, void (*)(MenuCheck *)>(
				new MenuCheck(user_arg_value("menu-shots")), &delete_part<MenuCheck>);
	}
	if (m.menu_check != nullptr && !m.finished) {
		MenuCheckIo cio;
		cio.press = [this](Key p_key, bool p_pressed) {
			Ref<InputEventKey> k;
			k.instantiate();
			k->set_keycode(p_key);
			k->set_physical_keycode(p_key);
			k->set_pressed(p_pressed);
			tdm_ui_input(k);
		};
		cio.move = [this](Vector2 p_at) {
			Ref<InputEventMouseMotion> e;
			e.instantiate();
			e->set_position(p_at);
			tdm_ui_input(e);
		};
		cio.click = [this](Vector2 p_at, bool p_pressed) {
			Ref<InputEventMouseButton> e;
			e.instantiate();
			e->set_position(p_at);
			e->set_button_index(MOUSE_BUTTON_LEFT);
			e->set_pressed(p_pressed);
			tdm_ui_input(e);
		};
		cio.shot = [this](const std::string &p_path) { return save_screenshot(p_path); };
		cio.capture = [this] { return get_viewport()->get_texture()->get_image(); };
		cio.alive = game_->alive;
		cio.hud_visible = game_->native_hud != nullptr && game_->native_hud->is_visible();
		cio.game_input = mapper_.game_input();
		const Bf3UiCursor *cursor = Bf3UiCursor::current();
		cio.cursor_shown = cursor != nullptr && cursor->cursor_shown();
		if (const Bf3OverviewMap *overview = Bf3OverviewMap::current()) {
			const OverviewMapStats &st = overview->stats();
			cio.overview_icons = st.icons;
			cio.overview = "attached " + std::to_string(st.attached) + ", paints " + std::to_string(st.paints) +
					", maps " + std::to_string(st.maps) + ", icons " + std::to_string(st.icons) + ", menu attaches " +
					std::to_string(m.native_menu->stats().overview_attached);
		}
		if (m.menu_check->frame(*screens, *m.native_flow, *m.native_menu, p_now_s, cio)) {
			m.finished = true;
			UtilityFunctions::print(to_godot(m.menu_check->report()));
			finish(m.menu_check->exit_code());
			return true;
		}
	}
#endif
	if (m.native_menu->quit_requested() && !m.menu_check_on) {
		m.finished = true;
		finish(0);
		return true;
	}
	return false;
}

}
