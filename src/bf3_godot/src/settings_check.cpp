#include "settings_check.h"

#include "bf3_localisation.h"
#include "game_settings.h"
#include "godot_util.h"
#include "input_mapper.h"

#include "bf3/audio/mixer_core.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/ui/options.h"
#include "bf3/core/util/format.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_map.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>
#include <cstdlib>
#include <memory>

namespace bf3::godot_side {

using namespace godot;
using bf3::core::fmt;
namespace ui = bf3::core::ui;

namespace {

Ref<InputEventKey> key_event(Key p_key, bool p_pressed, KeyLocation p_location = KEY_LOCATION_UNSPECIFIED) {
	Ref<InputEventKey> ev;
	ev.instantiate();
	ev->set_keycode(p_key);
	ev->set_physical_keycode(p_key);
	ev->set_location(p_location);
	ev->set_pressed(p_pressed);
	return ev;
}

bool key_is_action(Key p_key, const char *p_action, KeyLocation p_location = KEY_LOCATION_UNSPECIFIED) {
	return InputMap::get_singleton()->event_is_action(key_event(p_key, true, p_location), p_action);
}

uint32_t jumps_after(Key p_key) {
	InputMapper m;
	m.install_actions();
	m.handle_event(key_event(p_key, true));
	return m.sample().jump_presses;
}

int jump_row(const ui::SettingsStore &p_store) {
	const ui::KeyBindingTab *tab = p_store.key_bindings().tab_of_map("infantry");
	if (tab == nullptr || &p_store.key_bindings().tabs()[0] != tab) {
		return -1;
	}
	for (size_t i = 0; i < tab->rows.size(); ++i) {
		if (tab->rows[i].concept_id == "ConceptJump" && !tab->rows[i].negative) {
			return int(i);
		}
	}
	return -1;
}

void remove_file(const std::string &p_path) {
	if (FileAccess::file_exists(to_godot(p_path))) {
		DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(to_godot(p_path)));
	}
}

}

void Bf3SettingsCheck::_bind_methods() {}

void Bf3SettingsCheck::_ready() {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	set_process(true);
}

void Bf3SettingsCheck::add(std::string p_name, bool p_pass, std::string p_detail) {
	rows_.push_back({ std::move(p_name), p_pass, std::move(p_detail) });
}

void Bf3SettingsCheck::_process(double p_delta) {
	(void)p_delta;
	if (done_) {
		return;
	}
	Bf3Localisation *loc = Bf3Localisation::current();
	if (loc == nullptr || loc->has_failed() || (loc->is_loaded() && !loc->install())) {
		UtilityFunctions::print(to_godot("SETTINGS CHECK: no usable install (" +
				(loc != nullptr ? to_std(loc->get_error()) : std::string("no Bf3Localisation")) + ")"));
		done_ = true;
		set_process(false);
		get_tree()->quit(1);
		return;
	}
	if (!loc->is_loaded()) {
		return;
	}
	run();
	finish();
}

void Bf3SettingsCheck::run() {
	Bf3Localisation *loc = Bf3Localisation::current();
	const std::string path = user_arg_value("settings-file");
	if (path.empty()) {
		add("settings.file_arg", false, "the row passes --settings-file=<a scratch path of its own>");
		return;
	}
	remove_file(path);
	bf3::core::Result<std::unique_ptr<bf3::core::EbxLibrary>> lib =
			bf3::core::EbxLibrary::open(loc->install(), { "Win32/Levels/MP_Subway/MP_Subway" });
	if (!lib) {
		add("settings.library", false, lib.error().message);
		return;
	}
	bf3::core::Result<std::unique_ptr<GameSettings>> loaded = GameSettings::load(**lib);
	add("settings.load", loaded.ok(),
			loaded ? fmt("%zu options", (*loaded)->store().catalog()->settings.size()) : loaded.error().message);
	if (!loaded) {
		return;
	}
	GameSettings &gs = **loaded;
	ui::OptionsSession &session = gs.session();
	audio::MixerCore mixer(audio::MixerConfig{});
	std::vector<float> out(size_t(4096) * 2);
	InputMapper mapper;
	mapper.install_actions();
	mapper.enable_weapons(true);
	mapper.enable_tdm(true);

	const bf3::core::Status first = gs.frame(mapper, &mixer, path);
	size_t mismatched = 0;
	for (const ui::ActionConcept &a : ui::kActionConcepts) {
		const size_t want = ui::action_inputs(gs.store().key_bindings(), a).size();
		const StringName name(String::utf8(a.action.data(), int64_t(a.action.size())));
		if (!InputMap::get_singleton()->has_action(name) ||
				size_t(InputMap::get_singleton()->action_get_events(name).size()) != want) {
			++mismatched;
		}
	}
	add("input.defaults", first.ok() && gs.missed_inputs() == 0 && mismatched == 0,
			fmt("%zu actions take the install's cells; %zu inputs without a Godot key, %zu action event counts off",
					std::size(ui::kActionConcepts), gs.missed_inputs(), mismatched));
	add("input.defaults_keys",
			key_is_action(KEY_SPACE, "bf3_jump") && key_is_action(KEY_X, "bf3_crouch") &&
					key_is_action(KEY_CTRL, "bf3_crouch_hold", KEY_LOCATION_LEFT) &&
					!key_is_action(KEY_CTRL, "bf3_crouch_hold", KEY_LOCATION_RIGHT) &&
					key_is_action(KEY_V, "bf3_fire_mode") && key_is_action(KEY_TAB, "bf3_scoreboard") &&
					!key_is_action(KEY_CTRL, "bf3_crouch", KEY_LOCATION_LEFT),
			"Space jumps, X crouches, LeftCtrl (not RightCtrl) holds crouch, V cycles the fire mode, Tab shows the "
			"scoreboard");
	add("input.modifier_sides",
			ui_idk_of_key(KEY_SHIFT, KEY_LOCATION_RIGHT) == "IDK_RightShift" &&
					ui_idk_of_key(KEY_SHIFT) == "IDK_LeftShift" && key_of_ui_idk("IDK_RightAlt") == KEY_ALT &&
					key_location_of_ui_idk("IDK_RightAlt") == KEY_LOCATION_RIGHT &&
					key_location_of_ui_idk("IDK_LeftCtrl") == KEY_LOCATION_LEFT &&
					key_location_of_ui_idk("IDK_X") == KEY_LOCATION_UNSPECIFIED,
			"IDK_Left*/IDK_Right* Shift, Ctrl and Alt name the Godot modifier and its side");
	add("input.jump_space", jumps_after(KEY_SPACE) == 1 && jumps_after(KEY_U) == 0, "Space jumps, U does not");

	InputMapper hold;
	hold.install_actions();
	hold.handle_event(key_event(KEY_CTRL, true, KEY_LOCATION_LEFT));
	const bool crouched = hold.requested_pose() == bf3::sim::Pose::Crouch;
	hold.handle_event(key_event(KEY_CTRL, false, KEY_LOCATION_LEFT));
	add("input.crouch_hold", crouched && hold.requested_pose() == bf3::sim::Pose::Stand,
			"LeftCtrl down crouches, LeftCtrl up stands");

	const int row = jump_row(gs.store());
	bool conflict = false, kept = false, bound = false;
	if (row >= 0) {
		session.select_key_tab(0);
		conflict = session.bind_key(uint16_t(row), ui::KeyColumn::Keyboard) &&
				session.key_pressed("IDK_X") == ui::KeyBindResult::Conflict;
		kept = session.run(ui::OptionsAction::RevertBind) &&
				gs.store().key_bindings().input({ 0, uint16_t(row), ui::KeyColumn::Keyboard }) == "IDK_Space";
		bound = session.bind_key(uint16_t(row), ui::KeyColumn::Keyboard) &&
				session.key_pressed("IDK_U") == ui::KeyBindResult::Bound;
	}
	add("bind.conflict", conflict && kept,
			fmt("on-foot row %d: X is crouch's, so the popup asks; NO keeps Space", row));
	add("bind.jump_u", bound && session.model().settings_changed, "the row takes U and the Options show a change");
	const bf3::core::Status second = gs.frame(mapper, &mixer, path);
	add("input.jump_u",
			second.ok() && key_is_action(KEY_U, "bf3_jump") && !key_is_action(KEY_SPACE, "bf3_jump") &&
					jumps_after(KEY_U) == 1 && jumps_after(KEY_SPACE) == 0,
			"after the rebinding U jumps and Space does not");

	const ui::OptionSource *master = ui::option_source_named("MasterVolume");
	const bool set_volume = master != nullptr && session.set_option_value(master->data_key, 0.25);
	const double volume = gs.store().value("Volume");
	const float gain = float(volume);
	const bf3::core::Status third = gs.frame(mapper, &mixer, path);
	const uint32_t mixed = mixer.mix(out.data(), 4096);
	const float main_gain = mixer.bus_gain(bf3::core::audio::MasterBus::GameMixMain);
	const float music_gain = mixer.bus_gain(bf3::core::audio::MasterBus::Music);
	const float dialog_gain = mixer.bus_gain(bf3::core::audio::MasterBus::Dialog);
	add("audio.master_volume",
			set_volume && std::fabs(volume - 0.25) < 1e-6 && third.ok() && mixed == 4096 && main_gain == gain &&
					music_gain == gain * 0.7f && dialog_gain == gain * 0.7f,
			fmt("MasterVolume bar step 25 (%.9f, the data's float step): the mixer folds GameMixMain x %.4f, Music x "
				"%.4f, Dialog x %.4f",
					volume, double(main_gain), double(music_gain), double(dialog_gain)));

	const bool look = session.change("MouseSensitivity", 1.0) && session.change("VerticalLook", 1.0) &&
			session.change("ZoomToggle", 0.0);
	(void)gs.frame(mapper, &mixer, path);
	add("input.look",
			look && std::fabs(mapper.base_sensitivity - 2.0f * kBaseLookRadiansPerCount) < 1e-9f &&
					mapper.invert_look && !mapper.hold_to_zoom,
			fmt("MouseSensitivity 1.0 doubles the look rate (%.5f rad/count), VerticalLook inverts, ZoomToggle 0 "
				"toggles",
					double(mapper.base_sensitivity)));

	const bool saved = session.run(ui::OptionsAction::Save);
	const bf3::core::Status fourth = gs.frame(mapper, &mixer, path);
	const std::string text = FileAccess::file_exists(to_godot(path))
			? to_std(FileAccess::get_file_as_string(to_godot(path)))
			: std::string();
	const size_t volume_at = text.find("\nVolume = ");
	const double written = volume_at == std::string::npos ? -1.0 : std::strtod(text.c_str() + volume_at + 10, nullptr);
	add("save.file",
			saved && fourth.ok() && text.starts_with("version = 1\n") && written == volume &&
					text.find("\nbind.infantry.ConceptJump.keyboard = IDK_U\n") != std::string::npos,
			fmt("%s holds %zu bytes with the volume and the binding", path.c_str(), text.size()));

	const bool changed = session.change("MasterVolume", 0.5);
	const bool reverted = changed && session.run(ui::OptionsAction::Revert) && gs.store().value("Volume") == volume;
	add("save.revert", reverted, "an unsaved change reverts to the saved volume");

	bf3::core::Result<std::unique_ptr<GameSettings>> again = GameSettings::load(**lib);
	bool reread = false;
	if (again) {
		const bf3::core::Result<bool> r = (*again)->read_file(path);
		InputMapper fresh;
		fresh.install_actions();
		(void)(*again)->frame(fresh, nullptr, path);
		reread = r.ok() && *r && (*again)->store().value("Volume") == volume && key_is_action(KEY_U, "bf3_jump") &&
				!(*again)->store().settings_changed();
	}
	add("save.reload", reread, "a new session reads the file back: the volume, U jumps, nothing unsaved");

	const std::string newer = "version = 99\nVolume = 0.3\n";
	const std::string refused_path = path + std::string(kRefusedSettingsSuffix);
	remove_file(refused_path);
	bool refused = false, moved = false, rewritten = false;
	if (Ref<FileAccess> f = FileAccess::open(to_godot(path), FileAccess::WRITE); f.is_valid()) {
		f->store_string(to_godot(newer));
		f->close();
	}
	bf3::core::Result<std::unique_ptr<GameSettings>> third_session = GameSettings::load(**lib);
	if (third_session) {
		GameSettings &t = **third_session;
		const bf3::core::Result<bool> r = t.read_file(path);
		refused = !r.ok() && t.store().value("Volume") == 1.0;
		moved = !FileAccess::file_exists(to_godot(path)) && FileAccess::file_exists(to_godot(refused_path)) &&
				to_std(FileAccess::get_file_as_string(to_godot(refused_path))) == newer;
		InputMapper fresh;
		fresh.install_actions();
		rewritten = t.session().change("MasterVolume", 0.5) && t.session().run(ui::OptionsAction::Save) &&
				t.frame(fresh, nullptr, path).ok() && FileAccess::file_exists(to_godot(path)) &&
				to_std(FileAccess::get_file_as_string(to_godot(refused_path))) == newer;
	}
	add("save.refused_kept", refused && moved && rewritten,
			"a version 99 file is refused, the defaults stay, the file is kept as " + refused_path +
					" and the next Save writes a new one beside it");
	remove_file(path);
	remove_file(refused_path);
	add("save.cleanup", !FileAccess::file_exists(to_godot(path)) && !FileAccess::file_exists(to_godot(refused_path)),
			"the check's settings files are removed");

	if (!loc->install_snapshot_at_start().valid) {
		add("install.unchanged", false, "no snapshot at start");
	} else {
		const bf3::core::SnapshotDiff d =
				bf3::core::diff_snapshots(loc->install_snapshot_at_start(), loc->reader().snapshot());
		add("install.unchanged", d.clean(),
				d.clean() ? fmt("%zu files: sizes and mtimes equal", loc->install_snapshot_at_start().files.size())
						  : d.summary());
	}
}

void Bf3SettingsCheck::finish() {
	done_ = true;
	set_process(false);
	size_t passed = 0;
	for (const Row &r : rows_) {
		passed += r.pass ? 1 : 0;
		UtilityFunctions::print(
				to_godot(fmt("  %-4s %-24s %s", r.pass ? "PASS" : "FAIL", r.name.c_str(), r.detail.c_str())));
	}
	const bool ok = !rows_.empty() && passed == rows_.size();
	UtilityFunctions::print(to_godot(fmt("SETTINGS CHECK: %s (%zu/%zu)", ok ? "PASS" : "FAIL", passed, rows_.size())));
	get_tree()->quit(ok ? 0 : 1);
}

}
