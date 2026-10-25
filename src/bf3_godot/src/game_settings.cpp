#include "game_settings.h"

#include "godot_util.h"
#include "input_mapper.h"

#include "bf3/audio/mixer_core.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/ui/key_bindings.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>

#include <algorithm>
#include <cmath>

namespace bf3::godot_side {

using namespace godot;
namespace ui = bf3::core::ui;

std::string settings_file_of_args() {
	const std::string arg = user_arg_value("settings-file");
	return arg.empty() ? std::string(kSettingsFile) : arg;
}

core::Result<std::unique_ptr<GameSettings>> GameSettings::load(const core::EbxResolver &p_db) {
	core::Result<ui::OptionsCatalog> catalog = ui::read_options_catalog(p_db);
	if (!catalog) {
		return catalog.error();
	}
	core::Result<ui::KeyBindingMaps> maps = ui::read_key_binding_maps(p_db);
	if (!maps) {
		return maps.error();
	}
	core::Result<std::vector<std::string>> tabs = ui::read_key_binding_tabs(p_db);
	if (!tabs) {
		return tabs.error();
	}
	core::Result<ui::KeyBindingTable> table = ui::KeyBindingTable::build(*maps, *tabs);
	if (!table) {
		return table.error();
	}
	std::unique_ptr<GameSettings> s(new GameSettings());
	s->catalog_ = std::move(*catalog);
	s->session_ = std::make_unique<ui::OptionsSession>(s->catalog_, std::move(*table));
	s->video_shown_ = ui::settings_effects(s->store()).video;
	return s;
}

core::Result<bool> GameSettings::read_file(const std::string &p_path) {
	const String path = to_godot(p_path);
	if (!FileAccess::file_exists(path)) {
		return false;
	}
	const std::string text = to_std(FileAccess::get_file_as_string(path));
	if (core::Result<uint32_t> v = store().load_text(text); !v) {
		const std::string kept = p_path + std::string(kRefusedSettingsSuffix);
		const String from = ProjectSettings::get_singleton()->globalize_path(path);
		const String to = ProjectSettings::get_singleton()->globalize_path(to_godot(kept));
		if (FileAccess::file_exists(to)) {
			DirAccess::remove_absolute(to);
		}
		const Error moved = DirAccess::rename_absolute(from, to);
		return core::error_at(p_path,
				core::make_error(v.error().message +
						(moved == OK ? ", kept as " + kept : ", and it could not be kept as " + kept)));
	}
	written_revision_ = store().saved_revision();
	video_shown_ = ui::settings_effects(store()).video;
	return true;
}

core::Status GameSettings::write_file(const std::string &p_path) {
	Ref<FileAccess> f = FileAccess::open(to_godot(p_path), FileAccess::WRITE);
	if (f.is_null()) {
		return core::make_error(
				p_path + ": cannot write (error " + std::to_string(int(FileAccess::get_open_error())) + ")");
	}
	f->store_string(to_godot(store().saved_text()));
	f->close();
	written_revision_ = store().saved_revision();
	return core::Status::success();
}

core::Result<bool> GameSettings::save_if_changed(const std::string &p_path) {
	if (store().saved_revision() == written_revision_) {
		return false;
	}
	if (core::Status s = write_file(p_path); !s) {
		written_revision_ = store().saved_revision();
		return s.error();
	}
	return true;
}

size_t GameSettings::apply_input(InputMapper &r_mapper) {
	if (store().revision() == input_revision_) {
		return missed_inputs_;
	}
	input_revision_ = store().revision();
	missed_inputs_ = 0;
	for (const ui::ActionConcept &a : ui::kActionConcepts) {
		const std::vector<std::string> inputs = ui::action_inputs(store().key_bindings(), a);
		missed_inputs_ += r_mapper.rebind(a.action, inputs);
	}
	const ui::SettingsEffects e = ui::settings_effects(store());
	r_mapper.base_sensitivity = kBaseLookRadiansPerCount * e.look_scale;
	r_mapper.mouse_sensitivity = r_mapper.base_sensitivity;
	r_mapper.invert_look = e.invert_look;
	r_mapper.hold_to_zoom = e.hold_to_zoom;
	return missed_inputs_;
}

bool GameSettings::apply_audio(audio::MixerCore &r_mixer) {
	if (&r_mixer == audio_mixer_ && store().revision() == audio_revision_) {
		return false;
	}
	const ui::SettingsEffects e = ui::settings_effects(store());
	for (size_t b = 0; b < ui::kMasterBusCount; ++b) {
		audio::MixerCommand c;
		c.type = audio::MixerCommandType::SetBusGain;
		c.name_hash = uint32_t(b);
		c.value = e.bus_gains[b];
		if (!r_mixer.push(c)) {
			return false;
		}
	}
	audio_mixer_ = &r_mixer;
	audio_revision_ = store().revision();
	return true;
}

bool GameSettings::apply_video() {
	DisplayServer *ds = DisplayServer::get_singleton();
	const ui::VideoEffects v = ui::settings_effects(store()).video;
	if (ds == nullptr || ds->get_name() == String("headless") || v == video_shown_) {
		return false;
	}
	if (v.monitor != video_shown_.monitor && int64_t(v.monitor) < ds->get_screen_count()) {
		ds->window_set_current_screen(int32_t(v.monitor));
	}
	if (v.fullscreen != video_shown_.fullscreen) {
		ds->window_set_mode(v.fullscreen ? DisplayServer::WINDOW_MODE_FULLSCREEN : DisplayServer::WINDOW_MODE_WINDOWED);
	}
	if (!v.fullscreen && v.width > 0 && v.height > 0 &&
			(v.width != video_shown_.width || v.height != video_shown_.height ||
					v.fullscreen != video_shown_.fullscreen)) {
		ds->window_set_size(Vector2i(int32_t(v.width), int32_t(v.height)));
	}
	if (v.vsync != video_shown_.vsync) {
		ds->window_set_vsync_mode(v.vsync ? DisplayServer::VSYNC_ENABLED : DisplayServer::VSYNC_DISABLED);
	}
	video_shown_ = v;
	return true;
}

core::Status GameSettings::frame(InputMapper &r_mapper, audio::MixerCore *p_mixer, const std::string &p_path) {
	apply_input(r_mapper);
	if (p_mixer != nullptr) {
		apply_audio(*p_mixer);
	}
	apply_video();
	if (core::Result<bool> w = save_if_changed(p_path); !w) {
		return w.error();
	}
	return core::Status::success();
}

void GameSettings::read_display() {
	DisplayServer *ds = DisplayServer::get_singleton();
	ui::OptionsRuntime &rt = session_->runtime();
	rt.display_modes.clear();
	rt.monitors = 1;
	if (ds == nullptr) {
		return;
	}
	rt.monitors = uint32_t(std::max<int64_t>(1, ds->get_screen_count()));
	const int32_t screen = ds->window_get_current_screen();
	const Vector2i size = ds->screen_get_size(screen);
	const double hertz = ds->screen_get_refresh_rate(screen);
	if (size.x > 0 && size.y > 0 && std::isfinite(hertz) && hertz >= 1.0) {
		rt.display_modes.push_back({ uint32_t(size.x), uint32_t(size.y), uint32_t(std::lround(hertz)) });
	}
}

}
