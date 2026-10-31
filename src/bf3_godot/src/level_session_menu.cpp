#include "level_session_menu.h"

#include "game_audio.h"
#include "godot_util.h"
#include "input_mapper.h"
#include "level_session.h"
#include "level_session_game.h"
#include "level_session_tdm.h"
#include "menu_check.h"
#include "menu_frames_check.h"
#include "native_flow.h"
#include "native_hud.h"
#include "native_options.h"
#include "native_screens.h"
#include "native_tags.h"
#include "options_check.h"
#include "overview_map.h"
#include "swf_view.h"
#include "ui_cursor.h"
#include "ui_post_process.h"
#include "ui_sizes_check.h"
#include "video_memory.h"
#include "viewmodel.h"

#include "bf3/core/util/key_values.h"
#include "bf3/host/client_session.h"
#include "bf3/host/ui_flow_session.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/performance.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <string>

using namespace godot;

namespace bf3::godot_side {

void Bf3LevelSession::push_window_mouse(Vector2 p_window_px, int32_t p_button, bool p_pressed) {
	Viewport *vp = get_viewport();
	if (vp == nullptr) {
		return;
	}
	if (p_button == 0) {
		Ref<InputEventMouseMotion> e;
		e.instantiate();
		e->set_position(p_window_px);
		e->set_global_position(p_window_px);
		vp->push_input(e);
		return;
	}
	Ref<InputEventMouseButton> e;
	e.instantiate();
	e->set_position(p_window_px);
	e->set_global_position(p_window_px);
	e->set_button_index(MouseButton(p_button));
	e->set_pressed(p_pressed);
	vp->push_input(e);
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
		game_input_ = bf3::core::ui::ui_flow_action_truthy(p_action);
		++(game_input_ ? stats_.game_input_on : stats_.game_input_off);
		if (p_io.mapper != nullptr) {
			p_io.mapper->set_game_input(game_input_);
		}
	} else if (n == kUiMouseInputAction) {
		mouse_input_ = bf3::core::ui::ui_flow_action_truthy(p_action);
	} else if (n == kUiMouseCursorAction) {
		cursor_wanted_ = bf3::core::ui::ui_flow_action_truthy(p_action);
	} else if (n == kUiCameraChangedAction) {
		if (Bf3UiPostProcess *post = Bf3UiPostProcess::ensure(p_io.parent, p_io.level)) {
			post->trigger_glitch(bf3::core::ui::ui_flow_action_float(p_action).value_or(0.0f));
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
		Bf3OverviewMap *overview = Bf3OverviewMap::current();
		godot::Object *owner = ObjectDB::get_instance(ObjectID(overview_owner_));
		if (overview != nullptr && owner != nullptr) {
			overview->detach(Object::cast_to<Bf3SwfView>(owner));
		}
		overview_view_ = 0;
		overview_owner_ = 0;
		return;
	}
	Bf3OverviewMap *overview = Bf3OverviewMap::ensure(p_io.parent);
	if (overview == nullptr) {
		return;
	}
	const uint64_t view_open = slot.view->open_serial();
	if (overview_view_ != view_open) {
		overview->detach(slot.view);
		if (overview->attach(slot.view)) {
			overview_view_ = view_open;
			overview_owner_ = slot.view->get_instance_id();
			++stats_.overview_attached;
		}
	}
	std::vector<OverviewMark> marks = p_io.tags != nullptr
			? overview_marks(p_io.tags->minimap(), p_io.downed)
			: std::vector<OverviewMark>(p_io.downed.begin(), p_io.downed.end());
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
	if (game_->native_hud != nullptr) {
		io.downed = game_->native_hud->downed_marks();
	}
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
	if (m.menu_frames_check_on && m.menu_frames_check == nullptr) {
		const std::string baseline_bytes = user_arg_value("menu-frames-baseline-bytes");
		m.menu_frames_check = std::unique_ptr<MenuFramesCheck, void (*)(MenuFramesCheck *)>(
				has_user_arg("menu-frames-baseline")
						? new MenuFramesCheck(MenuFramesCheck::no_menu_baseline())
						: new MenuFramesCheck(user_arg_value("menu-frames-shots"),
								  baseline_bytes.empty() ? std::nullopt
														 : std::optional<uint64_t>(std::stoull(baseline_bytes))),
				&delete_part<MenuFramesCheck>);
		RenderingServer::get_singleton()->viewport_set_measure_render_time(get_viewport()->get_viewport_rid(), true);
	}
	if (m.menu_frames_check != nullptr && !m.finished) {
		MenuFramesIo fio;
		fio.press = [this](Key p_key, bool p_pressed) { scripted_ui_key(p_key, p_pressed); };
		fio.move = [this](Vector2 p_at) {
			Ref<InputEventMouseMotion> e;
			e.instantiate();
			e->set_position(p_at);
			tdm_ui_input(e);
		};
		fio.click = [this](Vector2 p_at, bool p_pressed) {
			Ref<InputEventMouseButton> e;
			e.instantiate();
			e->set_position(p_at);
			e->set_button_index(MOUSE_BUTTON_LEFT);
			e->set_pressed(p_pressed);
			tdm_ui_input(e);
		};
		fio.shot = [this](const std::string &p_path) { return save_screenshot(p_path); };
		fio.alive = game_->alive;
		fio.game_input = mapper_.game_input();
		fio.session_ms = game_->ms_total;
		fio.views_seconds_total = Bf3SwfView::sync_seconds_total();
		fio.setup_ms = RenderingServer::get_singleton()->get_frame_setup_time_cpu();
		fio.ui_ms = m.ms_tdm_hud;
		fio.video_mb =
				Performance::get_singleton()->get_monitor(Performance::RENDER_VIDEO_MEM_USED) / (1024.0 * 1024.0);
		if (const std::optional<ProcessVideoMemory> local = process_video_memory()) {
			fio.local_video_bytes = local->local_bytes;
		}
		fio.flow_tick_ms = m.ms_flow_tick;
		fio.working_set_bytes = process_working_set_bytes();
		fio.pipeline_canvas =
				uint64_t(Performance::get_singleton()->get_monitor(Performance::PIPELINE_COMPILATIONS_CANVAS));
		fio.pipeline_specialization =
				uint64_t(Performance::get_singleton()->get_monitor(Performance::PIPELINE_COMPILATIONS_SPECIALIZATION));
		const RID viewport = get_viewport()->get_viewport_rid();
		fio.render_cpu_ms = RenderingServer::get_singleton()->viewport_get_measured_render_time_cpu(viewport);
		fio.render_gpu_ms = RenderingServer::get_singleton()->viewport_get_measured_render_time_gpu(viewport);
		if (const Bf3UiPostProcess *post = Bf3UiPostProcess::current()) {
			fio.glitch_triggers = post->stats().glitch_triggers;
			fio.glitch_drawn = post->glitch_drawn();
		}
		if (m.menu_frames_check->frame(*screens, *m.native_flow, *m.native_menu, p_now_s, fio)) {
			m.finished = true;
			UtilityFunctions::print(to_godot(m.menu_frames_check->report()));
			finish(m.menu_frames_check->exit_code());
			return true;
		}
	}
	if (m.options_check_on && m.options_check == nullptr && m.native_options != nullptr) {
		m.options_check = std::unique_ptr<OptionsCheck, void (*)(OptionsCheck *)>(
				new OptionsCheck(user_arg_value("options-shots")), &delete_part<OptionsCheck>);
	}
	if (m.options_check != nullptr && !m.finished && !m.options_check_reported) {
		OptionsCheckIo oio;
		oio.press = [this](Key p_key, bool p_pressed) { scripted_ui_key(p_key, p_pressed); };
		oio.move = [this](Vector2 p_at) {
			Ref<InputEventMouseMotion> e;
			e.instantiate();
			e->set_position(p_at);
			tdm_ui_input(e);
		};
		oio.click = [this](Vector2 p_at, bool p_pressed) {
			Ref<InputEventMouseButton> e;
			e.instantiate();
			e->set_position(p_at);
			e->set_button_index(MOUSE_BUTTON_LEFT);
			e->set_pressed(p_pressed);
			tdm_ui_input(e);
		};
		oio.shot = [this](const std::string &p_path) { return save_screenshot(p_path); };
		oio.settings = game_->settings.get();
		oio.settings_path = game_->settings_path;
		oio.alive = game_->alive;
		oio.game_input = mapper_.game_input();
		oio.look_sensitivity = mapper_.base_sensitivity;
		oio.game_key = [this](Key p_key, bool p_pressed) {
			Ref<InputEventKey> e;
			e.instantiate();
			e->set_keycode(p_key);
			e->set_physical_keycode(p_key);
			e->set_pressed(p_pressed);
			_unhandled_input(e);
		};
		oio.hold_zoom = [this](bool p_held) {
			if (p_held) {
				mapper_.set_scripted(true);
			}
			mapper_.script_hold(InputMapper::WeaponAction::Ads, p_held);
			if (!p_held) {
				mapper_.set_scripted(false);
			}
		};
		oio.jump_presses = mapper_.sample().jump_presses;
		if (const std::optional<bf3::core::ui::HudStageRects> rects =
						game_->native_hud != nullptr ? game_->native_hud->stage_rects() : std::nullopt) {
			oio.hud_safe_x = rects->safe_x;
			oio.hud_safe_y = rects->safe_y;
			oio.hud_visible_width = rects->visible.width();
			oio.hud_visible_height = rects->visible.height();
		}
		if (const Camera3D *camera = cameras_.first_person()) {
			oio.camera_fov = float(camera->get_fov());
		}
		if (const Bf3Viewmodel *vm = game_->own_viewmodel()) {
			oio.zoom_fov = vm->ads_params().zoom_fov;
			oio.can_zoom = vm->ads_params().can_zoom;
			oio.fov_progress = vm->ads().fov_progress;
		}
		oio.air_ticks = frame_ != nullptr && frame_->valid ? frame_->own_curr.air_ticks : uint16_t(0);
		if (game_->audio != nullptr && game_->audio->mixer() != nullptr) {
			oio.master_gain = game_->audio->mixer()->bus_gain(bf3::core::audio::MasterBus::GameMixMain);
		}
		if (m.options_check->frame(*screens, *m.native_flow, *m.native_menu, *m.native_options, p_now_s, oio)) {
			m.options_check_reported = true;
			UtilityFunctions::print(to_godot(m.options_check->report()));
			if (!m.options_check->passed()) {
				m.finished = true;
				finish(1);
				return true;
			}
		}
	}
	if (m.ui_sizes_check_on && m.ui_sizes_check == nullptr) {
		m.ui_sizes_check = std::unique_ptr<UiSizesCheck, void (*)(UiSizesCheck *)>(
				new UiSizesCheck(user_arg_value("ui-sizes-shots")), &delete_part<UiSizesCheck>);
	}
	if (m.ui_sizes_check != nullptr && !m.finished) {
		Window *window = get_window();
		UiSizesCheckIo uio;
		uio.press = [this](Key p_key, bool p_pressed) { scripted_ui_key(p_key, p_pressed); };
		uio.mouse = [this](Vector2 p_window_px, int32_t p_button, bool p_pressed) {
			push_window_mouse(p_window_px, p_button, p_pressed);
		};
		uio.set_window = [window](Vector2i p_size, bool p_fullscreen) {
			if (window == nullptr) {
				return;
			}
			window->set_mode(p_fullscreen ? Window::MODE_FULLSCREEN : Window::MODE_WINDOWED);
			if (!p_fullscreen) {
				window->set_size(p_size);
				window->move_to_center();
			}
		};
		uio.shot = [this](const std::string &p_path) { return save_screenshot(p_path); };
		uio.window_size = window != nullptr ? window->get_size() : Vector2i();
		uio.fullscreen = window != nullptr && window->get_mode() == Window::MODE_FULLSCREEN;
		uio.screen_size = window != nullptr
				? DisplayServer::get_singleton()->screen_get_size(window->get_current_screen())
				: Vector2i();
		uio.canvas_size = get_viewport()->get_visible_rect().size;
		const Bf3UiCursor *cursor = Bf3UiCursor::current();
		uio.cursor_shown = cursor != nullptr && cursor->cursor_shown();
		uio.cursor_scale = cursor != nullptr ? cursor->cursor_scale() : 0.0f;
		uio.alive = game_->alive;
		uio.game_input = mapper_.game_input();
		uio.hud_visible = game_->native_hud != nullptr && game_->native_hud->is_visible();
		if (m.ui_sizes_check->frame(*screens, *m.native_flow, *m.native_menu, m.native_options.get(), p_now_s, uio)) {
			m.finished = true;
			UtilityFunctions::print(to_godot(m.ui_sizes_check->report()));
			finish(m.ui_sizes_check->exit_code());
			return true;
		}
	}
#endif
	if (m.native_menu->quit_requested() && !m.menu_check_on) {
		m.finished = true;
		UtilityFunctions::print("[ui] QuitGame from the in-game menu QuitPopup");
		finish(0);
		return true;
	}
	return false;
}

}
