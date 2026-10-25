#include <doctest/doctest.h>

#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/ui/key_bindings.h"
#include "bf3/core/ui/options.h"
#include "test_support.h"

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;

namespace {

struct ExpectedSetting {
	std::string_view source;
	OptionsTab tab;
	std::string_view list;
	std::string_view label;
	OptionRowType row;
	int32_t data_key;
	OptionKind kind;
	std::string_view option_id;
	double min;
	double max;
	double step;
	double default_value;
	std::string_view choices;
};

const ExpectedSetting kExpectedSettings[] = {
	{ "VerticalLook", OptionsTab::Controls, "List_01", "ID_M_VERTICAL_LOOK", OptionRowType::TextSelector, 2036655012,
			OptionKind::Bool, "Scheme0FlipY", 0.0, 1.0, 1.0, 0.0, "ID_M_OPTIONS_NORMAL;ID_M_OPTIONS_INVERT" },
	{ "VerticalFlight", OptionsTab::Controls, "List_01", "ID_M_VERTICAL_FLIGHT", OptionRowType::TextSelector,
			1197323515, OptionKind::Bool, "Scheme2FlipY", 0.0, 1.0, 1.0, 0.0,
			"ID_M_OPTIONS_NORMAL;ID_M_OPTIONS_INVERT" },
	{ "MouseSensitivity", OptionsTab::Controls, "List_01", "ID_M_MOUSE_SENSITIVITY", OptionRowType::BarSelector,
			-1726867587, OptionKind::Number, "MouseSensitivity", 0.0, 1.5, 0.01, 0.5, "" },
	{ "MouseSensitivityVehicle", OptionsTab::Controls, "List_01", "ID_M_MOUSE_VEHICLE_SENSITIVITY",
			OptionRowType::BarSelector, 745800453, OptionKind::Number, "MouseSensitivityVehicle", 0.0, 5.0, 0.01, 0.5,
			"" },
	{ "MouseRawInput", OptionsTab::Controls, "List_01", "ID_M_RAW_MOUSE_INPUT", OptionRowType::TextSelector,
			-2059361310, OptionKind::Bool, "MouseRawInput", 0.0, 1.0, 1.0, 1.0, "ID_M_OPTIONS_OFF;ID_M_OPTIONS_ON" },
	{ "Sensitivity", OptionsTab::Controls, "List_01", "ID_M_JOYSTICK_SENSITIVITY", OptionRowType::BarSelector,
			-875263044, OptionKind::Number, "Scheme0Sensitivity", 0.0, 4.0, 0.125, 1.0, "" },
	{ "Vibration", OptionsTab::Controls, "List_01", "ID_M_VIBRATION", OptionRowType::TextSelector, -1557825949,
			OptionKind::Bool, "Vibration", 0.0, 1.0, 1.0, 1.0, "ID_M_OPTIONS_OFF;ID_M_OPTIONS_ON" },
	{ "Subtitles", OptionsTab::Gameplay, "List_01", "ID_M_SUBTITLES", OptionRowType::TextSelector, 282440422,
			OptionKind::Bool, "EnableSubtitles", 0.0, 1.0, 1.0, 1.0, "ID_M_OPTIONS_NO;ID_M_OPTIONS_YES" },
	{ "HintSystem", OptionsTab::Gameplay, "List_01", "ID_M_HINT_SYSTEM", OptionRowType::TextSelector, 380131439,
			OptionKind::Bool, "HintSystem", 0.0, 1.0, 1.0, 1.0, "ID_M_OPTIONS_NO;ID_M_OPTIONS_YES" },
	{ "ReloadHint", OptionsTab::Gameplay, "List_01", "ID_M_RELOAD_HINT", OptionRowType::TextSelector, 996376763,
			OptionKind::Bool, "ReloadHint", 0.0, 1.0, 1.0, 1.0, "ID_M_OPTIONS_NO;ID_M_OPTIONS_YES" },
	{ "ZoomToggle", OptionsTab::Gameplay, "List_01", "ID_M_ZOOM_TOGGLE", OptionRowType::TextSelector, 1511321364,
			OptionKind::Bool, "HoldButtonToZoom", 0.0, 1.0, 1.0, 1.0, "ID_M_TOGGLE;ID_M_OPTIONS_HOLD" },
	{ "BreathToggle", OptionsTab::Gameplay, "List_01", "ID_M_BREATH_TOGGLE", OptionRowType::TextSelector, 1163588491,
			OptionKind::Bool, "HoldForBreathControl", 0.0, 1.0, 1.0, 1.0, "ID_M_TOGGLE;ID_M_OPTIONS_HOLD" },
	{ "NetworkFrequencyFactor", OptionsTab::Gameplay, "List_01", "ID_M_FREQUENCY_FACTOR", OptionRowType::BarSelector,
			2123762470, OptionKind::Number, "UserFrequencyFactor", 0.0, 1.0, 0.01, 1.0, "" },
	{ "MinimapType", OptionsTab::Gameplay, "List_01", "ID_M_MINIMAPTYPE", OptionRowType::TextSelector, -1506947434,
			OptionKind::Enum, "MinimapType", 0.0, 2.0, 1.0, 0.0, "ID_M_SATELLITE;ID_M_HYBRID;ID_M_DETAILTEXTURE" },
	{ "MinimapTextureOpacity", OptionsTab::Gameplay, "List_01", "ID_M_MINIMAPTEXTUREOPACITY",
			OptionRowType::BarSelector, 1768075368, OptionKind::Number, "MinimapTextureOpacity", 0.0, 1.0, 0.1, 0.5,
			"" },
	{ "MasterVolume", OptionsTab::Audio, "List_01", "ID_M_MASTER", OptionRowType::BarSelector, -446156347,
			OptionKind::Number, "Volume", 0.0, 1.0, 0.01, 1.0, "" },
	{ "YourSoundSystem", OptionsTab::Audio, "List_01", "ID_M_YOUR_SOUND_SYSTEM", OptionRowType::TextSelector,
			-1243243194, OptionKind::Enum, "YourSoundSystem", 0.0, 4.0, 1.0, 2.0,
			"ID_M_OPTIONS_WAR_TAPES;ID_M_OPTIONS_TV;ID_M_OPTIONS_HIFI;ID_M_OPTIONS_HOME_CINEMA;ID_M_OPTIONS_"
			"HEADPHONES" },
	{ "StereoMode", OptionsTab::Audio, "List_01", "ID_M_STEREOMODE", OptionRowType::TextSelector, -28028280,
			OptionKind::Bool, "StereoMode", 0.0, 1.0, 1.0, 0.0, "ID_M_OPTIONS_OFF;ID_M_OPTIONS_ON" },
	{ "VoiceOverLanguage", OptionsTab::Audio, "List_01", "ID_M_VOICEOVER_LANGUAGE", OptionRowType::TextSelector,
			1021755323, OptionKind::Enum, "VOLanguage", 0.0, 7.0, 1.0, 0.0,
			"ID_M_OPTIONS_ENGLISH;ID_M_OPTIONS_FRENCH;ID_M_OPTIONS_GERMAN;ID_M_OPTIONS_ITALIAN;ID_M_OPTIONS_JAPANESE;"
			"ID_M_OPTIONS_POLISH;ID_M_OPTIONS_RUSSIAN;ID_M_OPTIONS_SPANISH" },
	{ "FullscreenScreen", OptionsTab::Video, "List_01", "ID_M_OPTIONS_FULLSCREEN_MONITOR", OptionRowType::TextSelector,
			-738813438, OptionKind::Monitor, "FullscreenScreen", 0.0, 1000.0, 1.0, 0.0, "" },
	{ "Resolution", OptionsTab::Video, "List_01", "ID_M_OPTIONS_RESOLUTION", OptionRowType::TextSelector, -505783905,
			OptionKind::DisplayMode, "ResolutionWidth", 0.0, 0.0, 0.0, 0.0, "" },
	{ "FullscreenEnabled", OptionsTab::Video, "List_01", "ID_M_OPTIONS_FULLSCREEN", OptionRowType::TextSelector,
			225189035, OptionKind::Bool, "FullscreenEnabled", 0.0, 1.0, 1.0, 0.0, "ID_M_OPTIONS_OFF;ID_M_OPTIONS_ON" },
	{ "Brightness", OptionsTab::Video, "List_01", "ID_M_BRIGHTNESS", OptionRowType::BarSelector, 1759039576,
			OptionKind::Number, "Brightness", 0.0, 1.0, 0.05, 0.5, "" },
	{ "VSync", OptionsTab::Video, "List_01", "ID_M_OPTIONS_VERTICAL_SYNC", OptionRowType::TextSelector, 1574386880,
			OptionKind::Bool, "VSync", 0.0, 1.0, 1.0, 0.0, "ID_M_OPTIONS_OFF;ID_M_OPTIONS_ON" },
	{ "FieldOfView", OptionsTab::Video, "List_01", "ID_M_OPTIONS_FIELD_OF_VIEW", OptionRowType::TextSelector, 944518935,
			OptionKind::Number, "FieldOfView", 60.0, 90.0, 1.0, 70.0,
			"60;61;62;63;64;65;66;67;68;69;70;71;72;73;74;75;76;77;78;79;80;81;82;83;84;85;86;87;88;89;90" },
	{ "MotionBlur", OptionsTab::Video, "List_01", "ID_M_OPTIONS_MOTION_BLUR_AMOUNT", OptionRowType::BarSelector,
			-1903337626, OptionKind::Number, "MotionBlur", 0.0, 1.0, 0.05, 0.5, "" },
	{ "Stereoscopy", OptionsTab::Video, "List_01", "ID_M_OPTIONS_STEREOSCOPY_ENABLED", OptionRowType::TextSelector,
			-908194755, OptionKind::Bool, "Stereoscopy", 0.0, 1.0, 1.0, 0.0, "ID_M_OPTIONS_OFF;ID_M_OPTIONS_ON" },
	{ "StereoConvergence", OptionsTab::Video, "List_01", "ID_M_OPTIONS_STEREOSCOPY_CONVERGENCE",
			OptionRowType::BarSelector, 778093026, OptionKind::Number, "StereoConvergence", 0.0, 2.0, 0.02, 1.0, "" },
	{ "ColorblindSupport", OptionsTab::Video, "List_01", "ID_M_OPTIONS_COLORBLIND_SUPPORT", OptionRowType::TextSelector,
			32383342, OptionKind::Bool, "ColorblindSupport", 0.0, 1.0, 1.0, 0.0, "ID_M_OPTIONS_OFF;ID_M_OPTIONS_ON" },
	{ "OverallGraphicsQuality", OptionsTab::Video, "List_02", "ID_M_OPTIONS_GRAPHICS_QUALITY",
			OptionRowType::TextSelector, 1089114246, OptionKind::Enum, "OverallGraphicsQuality", 0.0, 5.0, 1.0, 0.0,
			"ID_M_OPTIONS_AUTO;ID_M_OPTIONS_LOW;ID_M_OPTIONS_MEDIUM;ID_M_OPTIONS_HIGH;ID_M_OPTIONS_ULTRA;ID_M_OPTIONS_"
			"CUSTOM" },
	{ "TextureQuality", OptionsTab::Video, "List_02", "ID_M_OPTIONS_TEXTURE_QUALITY", OptionRowType::TextSelector,
			-522365085, OptionKind::Enum, "TextureQuality", 0.0, 3.0, 1.0, 2.0,
			"ID_M_OPTIONS_LOW;ID_M_OPTIONS_MEDIUM;ID_M_OPTIONS_HIGH;ID_M_OPTIONS_ULTRA" },
	{ "ShadowQuality", OptionsTab::Video, "List_02", "ID_M_OPTIONS_SHADOW_QUALITY", OptionRowType::TextSelector,
			-408184070, OptionKind::Enum, "ShadowQuality", 0.0, 3.0, 1.0, 2.0,
			"ID_M_OPTIONS_LOW;ID_M_OPTIONS_MEDIUM;ID_M_OPTIONS_HIGH;ID_M_OPTIONS_ULTRA" },
	{ "EffectsQuality", OptionsTab::Video, "List_02", "ID_M_OPTIONS_EFFECTS_QUALITY", OptionRowType::TextSelector,
			1319531928, OptionKind::Enum, "EffectsQuality", 0.0, 3.0, 1.0, 2.0,
			"ID_M_OPTIONS_LOW;ID_M_OPTIONS_MEDIUM;ID_M_OPTIONS_HIGH;ID_M_OPTIONS_ULTRA" },
	{ "MeshQuality", OptionsTab::Video, "List_02", "ID_M_OPTIONS_MESH_QUALITY", OptionRowType::TextSelector, -102852753,
			OptionKind::Enum, "MeshQuality", 0.0, 3.0, 1.0, 2.0,
			"ID_M_OPTIONS_LOW;ID_M_OPTIONS_MEDIUM;ID_M_OPTIONS_HIGH;ID_M_OPTIONS_ULTRA" },
	{ "TerrainQuality", OptionsTab::Video, "List_02", "ID_M_OPTIONS_TERRAIN_QUALITY", OptionRowType::TextSelector,
			300599499, OptionKind::Enum, "TerrainQuality", 0.0, 3.0, 1.0, 2.0,
			"ID_M_OPTIONS_LOW;ID_M_OPTIONS_MEDIUM;ID_M_OPTIONS_HIGH;ID_M_OPTIONS_ULTRA" },
	{ "UndergrowthQuality", OptionsTab::Video, "List_02", "ID_M_OPTIONS_UNDERGROWTH_QUALITY",
			OptionRowType::TextSelector, 1475987109, OptionKind::Enum, "UndergrowthQuality", 0.0, 3.0, 1.0, 2.0,
			"ID_M_OPTIONS_LOW;ID_M_OPTIONS_MEDIUM;ID_M_OPTIONS_HIGH;ID_M_OPTIONS_ULTRA" },
	{ "AntiAliasingDeferred", OptionsTab::Video, "List_02", "ID_M_OPTIONS_ANTIALIASING_DEFERRED",
			OptionRowType::TextSelector, -78368714, OptionKind::Enum, "AntiAliasingDeferred", 0.0, 2.0, 1.0, 0.0,
			"ID_M_OPTIONS_OFF;ID_M_OPTIONS_2X_MSAA;ID_M_OPTIONS_4X_MSAA" },
	{ "AntiAliasingPost", OptionsTab::Video, "List_02", "ID_M_OPTIONS_ANTIALIASING_POST", OptionRowType::TextSelector,
			-502331603, OptionKind::Enum, "AntiAliasingPost", 0.0, 3.0, 1.0, 0.0,
			"ID_M_OPTIONS_OFF;ID_M_OPTIONS_LOW;ID_M_OPTIONS_MEDIUM;ID_M_OPTIONS_HIGH" },
	{ "MotionBlurEnabled", OptionsTab::Video, "List_02", "ID_M_OPTIONS_MOTION_BLUR", OptionRowType::TextSelector,
			2082600515, OptionKind::Bool, "MotionBlurEnabled", 0.0, 1.0, 1.0, 1.0, "ID_M_OPTIONS_OFF;ID_M_OPTIONS_ON" },
	{ "AnisotropicFilter", OptionsTab::Video, "List_02", "ID_M_OPTIONS_ANISOTROPIC_FILTER", OptionRowType::TextSelector,
			-1132558216, OptionKind::Enum, "AnisotropicFilter", 0.0, 4.0, 1.0, 4.0, "1X;2X;4X;8X;16X" },
	{ "AmbientOcclusion", OptionsTab::Video, "List_02", "ID_M_OPTIONS_AMBIENT_OCCLUSION", OptionRowType::TextSelector,
			312315940, OptionKind::Enum, "AmbientOcclusion", 0.0, 2.0, 1.0, 2.0,
			"ID_M_OPTIONS_OFF;ID_M_OPTIONS_SSAO;ID_M_OPTIONS_HBAO" },
};

std::string joined(const std::vector<std::string> &p_items) {
	std::string out;
	for (const std::string &s : p_items) {
		out += out.empty() ? "" : ";";
		out += s;
	}
	return out;
}

struct OptionsLevel {
	bf3::test::Mounted *mount = nullptr;
	std::unique_ptr<EbxLibrary> lib;
};

OptionsLevel *options_level_or_skip() {
	static OptionsLevel l;
	static bool tried = false;
	if (!tried) {
		tried = true;
		l.mount = bf3::test::shared_mount_or_skip();
		if (l.mount == nullptr) {
			return nullptr;
		}
		Result<std::unique_ptr<EbxLibrary>> lib =
				EbxLibrary::open(l.mount->install, { "Win32/Levels/MP_Subway/MP_Subway" });
		REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
		l.lib = std::move(*lib);
	} else if (l.mount == nullptr) {
		bf3::test::shared_mount_or_skip();
		return nullptr;
	}
	return &l;
}

ConceptInput in(ConceptDevice p_device, std::string_view p_input, bool p_negate = false) {
	return { p_device, std::string(p_input), p_negate, false };
}

KeyBindingMaps sample_maps() {
	KeyBindingMaps m;
	for (std::string_view id : kKeyBindingTabMaps) {
		InputActionMap a;
		a.id = std::string(id);
		m.maps.push_back(std::move(a));
	}
	std::vector<InputConcept> &inf = m.maps[0].concepts;
	inf.push_back({ "ConceptMoveFB", "ID_M_OPTIONS_CONCEPTMOVEFB_FOOT", false, "ConceptUndefined",
			{ in(ConceptDevice::Pad, "IDA_Axis0Y"), in(ConceptDevice::Keyboard, "IDK_W"),
					in(ConceptDevice::Keyboard, "IDK_S", true) } });
	inf.push_back({ "ConceptJump", "", false, "ConceptUndefined",
			{ in(ConceptDevice::Pad, "IDB_Rdown"), in(ConceptDevice::Keyboard, "IDK_Space") } });
	inf.push_back({ "ConceptFire", "", false, "ConceptUndefined",
			{ in(ConceptDevice::Pad, "IDB_Rtrigger"), in(ConceptDevice::Mouse, "IDB_Button_0"),
					in(ConceptDevice::Keyboard, "IDK_Undefined") } });
	inf.push_back({ "ConceptCommMenu1", "", true, "ConceptCommMenu1", { in(ConceptDevice::Keyboard, "IDK_Q") } });
	inf.push_back({ "ConceptCrouch", "", false, "ConceptUndefined", { in(ConceptDevice::Keyboard, "IDK_X") } });
	m.maps[4].concepts.push_back(
			{ "ConceptMapSize", "", false, "ConceptUndefined", { in(ConceptDevice::Keyboard, "IDK_M") } });
	return m;
}

KeyBindingTable sample_table() {
	const std::vector<std::string> labels = { "A", "B", "C", "D", "E" };
	Result<KeyBindingTable> t = KeyBindingTable::build(sample_maps(), labels);
	REQUIRE(t.ok());
	return std::move(*t);
}

std::string cells(const KeyBindingRow &p_row) {
	return p_row.label + "|" + p_row.inputs[0] + "|" + p_row.inputs[1] + "|" + p_row.inputs[2];
}

}

TEST_SUITE("ui-settings-unit") {
	TEST_CASE("key bindings: rows per concept, a negative row for an axis, hidden concepts left out") {
		const KeyBindingTable t = sample_table();
		REQUIRE(t.tabs().size() == 5);
		const std::vector<KeyBindingRow> &rows = t.tabs()[0].rows;
		REQUIRE(rows.size() == 5);
		CHECK(cells(rows[0]) == "ID_M_OPTIONS_CONCEPTMOVEFB_FOOT|IDK_W||IDA_Axis0Y");
		CHECK(cells(rows[1]) == "ID_M_OPTIONS_CONCEPTMOVEFB_FOOT_NEG|IDK_S||IDA_Axis0Y");
		CHECK(rows[1].negative);
		CHECK(cells(rows[2]) == "ID_M_OPTIONS_CONCEPTJUMP|IDK_Space||IDB_Rdown");
		CHECK(cells(rows[3]) == "ID_M_OPTIONS_CONCEPTFIRE||IDB_Button_0|IDB_Rtrigger");
		CHECK(cells(rows[4]) == "ID_M_OPTIONS_CONCEPTCROUCH|IDK_X||");
		CHECK(t.tabs()[4].map_id == "default");
		CHECK(t.tabs()[4].label == "E");
		CHECK(key_input_sid("IDK_LeftShift") == "ID_IDK_LeftShift");
		CHECK(key_input_sid("IDK_None").empty());
		CHECK_FALSE(input_is_bound("IDK_None"));
		CHECK_FALSE(input_is_bound("IDB_Button_Undefined"));
		CHECK_FALSE(input_is_bound(""));
		CHECK(input_is_bound("IDA_Axis0Y"));
		CHECK(input_fits(KeyColumn::Keyboard, "IDK_V"));
		CHECK_FALSE(input_fits(KeyColumn::Keyboard, "IDB_Button_1"));
		CHECK(input_fits(KeyColumn::Mouse, "IDB_Button_1"));
		CHECK_FALSE(input_fits(KeyColumn::Mouse, "IDB_Rdown"));
		CHECK(input_fits(KeyColumn::Joystick, "IDB_Rdown"));
		CHECK_FALSE(input_fits(KeyColumn::Joystick, "IDB_Button_0"));
		const std::vector<std::string> four = { "A", "B", "C", "D" };
		CHECK_FALSE(KeyBindingTable::build(sample_maps(), four).ok());
	}

	TEST_CASE("key bindings: capture binds, reports a key in use, cancels on Escape and clears") {
		KeyBindingTable t = sample_table();
		KeyBindCapture cap;
		const KeyCell jump{ 0, 2, KeyColumn::Keyboard };
		const KeyCell crouch{ 0, 4, KeyColumn::Keyboard };
		CHECK(cap.press(t, "IDK_J") == KeyBindResult::Refused);
		REQUIRE(cap.begin(t, jump));
		CHECK(cap.press(t, "IDB_Button_1") == KeyBindResult::Refused);
		CHECK(cap.waiting());
		CHECK(cap.press(t, "IDK_X") == KeyBindResult::Conflict);
		CHECK(cap.conflict_pending());
		REQUIRE(cap.conflicts().size() == 1);
		CHECK(cap.conflicts()[0] == crouch);
		CHECK(cap.press(t, "IDK_J") == KeyBindResult::Refused);
		CHECK(cap.confirm(t, false) == KeyBindResult::Cancelled);
		CHECK(t.input(jump) == "IDK_Space");
		REQUIRE(cap.begin(t, jump));
		CHECK(cap.press(t, "IDK_X") == KeyBindResult::Conflict);
		CHECK(cap.confirm(t, true) == KeyBindResult::Bound);
		CHECK(t.input(jump) == "IDK_X");
		CHECK(t.input(crouch) == "IDK_X");
		CHECK(t.holders(0, KeyColumn::Keyboard, "IDK_X").size() == 2);
		REQUIRE(cap.begin(t, jump));
		CHECK(cap.press(t, "IDK_Escape") == KeyBindResult::Cancelled);
		CHECK(t.input(jump) == "IDK_X");
		REQUIRE(cap.begin(t, jump));
		CHECK(cap.press(t, "IDK_K") == KeyBindResult::Bound);
		CHECK(t.input(jump) == "IDK_K");
		REQUIRE(cap.begin(t, jump));
		CHECK(cap.clear(t) == KeyBindResult::Cleared);
		CHECK(t.input(jump).empty());
		CHECK_FALSE(cap.waiting());
		CHECK(cap.confirm(t, true) == KeyBindResult::Refused);
		CHECK_FALSE(cap.begin(t, { 0, 99, KeyColumn::Keyboard }));
	}

	TEST_CASE("key bindings: overrides round-trip and reset; UI bindings follow the common tab") {
		KeyBindingTable t = sample_table();
		CHECK(t.is_default());
		CHECK(t.overrides().empty());
		REQUIRE(t.set({ 0, 2, KeyColumn::Keyboard }, "IDK_J"));
		REQUIRE(t.set({ 0, 1, KeyColumn::Keyboard }, ""));
		REQUIRE(t.set({ 4, 0, KeyColumn::Keyboard }, "IDK_Comma"));
		CHECK_FALSE(t.set({ 0, 2, KeyColumn::Keyboard }, "IDB_Button_0"));
		const std::vector<std::pair<std::string, std::string>> o = t.overrides();
		REQUIRE(o.size() == 3);
		CHECK(o[0] == std::pair<std::string, std::string>("bind.infantry.ConceptMoveFB.neg.keyboard", ""));
		CHECK(o[1] == std::pair<std::string, std::string>("bind.infantry.ConceptJump.keyboard", "IDK_J"));
		CHECK(o[2] == std::pair<std::string, std::string>("bind.default.ConceptMapSize.keyboard", "IDK_Comma"));
		KeyBindingTable back = sample_table();
		for (const auto &[k, v] : o) {
			CHECK(back.apply_override(k, v));
		}
		CHECK(back.overrides() == o);
		CHECK_FALSE(back.apply_override("bind.infantry.ConceptNothing.keyboard", "IDK_A"));
		CHECK_FALSE(back.apply_override("bind.infantry.ConceptJump.feet", "IDK_A"));
		CHECK_FALSE(back.apply_override("other.infantry.ConceptJump.keyboard", "IDK_A"));
		UiInputBindings base;
		base.bindings = { { "ConceptMapSize", UiInputDevice::Keyboard, "IDK_M" },
			{ "ConceptActivate", UiInputDevice::Keyboard, "IDK_Enter" } };
		const UiInputBindings ui = ui_input_bindings_with(base, back);
		CHECK(ui.keys_of("ConceptMapSize") == std::vector<std::string>{ "IDK_Comma" });
		CHECK(ui.keys_of("ConceptActivate") == std::vector<std::string>{ "IDK_Enter" });
		back.reset();
		CHECK(back.is_default());
		CHECK(ui_input_bindings_with(base, back).bindings == base.bindings);
	}
}

TEST_SUITE("ui-options") {
	TEST_CASE("options: profile/profileoptions holds the 89 stored options") {
		OptionsLevel *l = options_level_or_skip();
		if (l == nullptr) {
			return;
		}
		Result<ProfileOptions> p = read_profile_options(*l->lib);
		REQUIRE_MESSAGE(p.ok(), (p.ok() ? std::string() : p.error().message));
		std::printf(
				"ui.options profile %s %u bytes, %zu options\n", p->file_name.c_str(), p->file_size, p->options.size());
		CHECK(p->file_name == "PROF_SAVE");
		CHECK(p->file_size == 16000);
		CHECK_FALSE(p->auto_save_on_quit);
		CHECK(p->options.size() == 89);
		for (const ProfileOption &o : p->options) {
			CAPTURE(o.id);
			CHECK(o.from_profile);
			if (o.kind == ProfileOptionKind::Int || o.kind == ProfileOptionKind::Float) {
				CHECK(o.min <= o.max);
			}
			if (o.kind == ProfileOptionKind::Enum) {
				CHECK(o.default_item() < o.items.size());
			}
		}
		const ProfileOption *volume = p->find("Volume");
		REQUIRE(volume != nullptr);
		CHECK(volume->category == "GstAudio");
		CHECK(volume->kind == ProfileOptionKind::Float);
		CHECK(volume->value == doctest::Approx(1.0));
		const ProfileOption *music = p->find("MusicVolume");
		REQUIRE(music != nullptr);
		CHECK(music->value == doctest::Approx(0.7).epsilon(1e-6));
		const ProfileOption *award = p->find("TrackAward0");
		REQUIRE(award != nullptr);
		CHECK(award->kind == ProfileOptionKind::String);
		CHECK(award->max_length == 128);
		const ProfileOption *size = p->find("SoundSystemSize");
		REQUIRE(size != nullptr);
		CHECK(size->kind == ProfileOptionKind::Int);
		CHECK(size->value == 20.0);
	}

	TEST_CASE("options: the four list tabs give 41 settings, each backed by the install") {
		OptionsLevel *l = options_level_or_skip();
		if (l == nullptr) {
			return;
		}
		Result<std::vector<OptionsListRow>> keys = read_options_rows(*l->lib, OptionsTab::KeyBindings);
		REQUIRE(keys.ok());
		CHECK(keys->empty());
		Result<OptionsCatalog> c = read_options_catalog(*l->lib);
		REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message));
		std::printf("ui.options catalog %zu settings, %zu stored values\n", c->settings.size(), c->stored.size());
		CHECK(c->profile_file == "PROF_SAVE");
		CHECK(c->stored.size() == 90);
		REQUIRE(c->settings.size() == std::size(kExpectedSettings));
		for (size_t i = 0; i < c->settings.size(); ++i) {
			const OptionSetting &s = c->settings[i];
			const ExpectedSetting &e = kExpectedSettings[i];
			CAPTURE(e.source);
			CHECK(s.source == e.source);
			CHECK(s.tab == e.tab);
			CHECK(s.list == e.list);
			CHECK(s.label == e.label);
			CHECK(s.row == e.row);
			CHECK(s.data_key == e.data_key);
			CHECK(s.kind == e.kind);
			CHECK(s.option_id == e.option_id);
			CHECK(s.min == doctest::Approx(e.min).epsilon(1e-6));
			CHECK(s.max == doctest::Approx(e.max).epsilon(1e-6));
			CHECK(s.step == doctest::Approx(e.step).epsilon(1e-6));
			CHECK(s.default_value == doctest::Approx(e.default_value).epsilon(1e-6));
			CHECK(joined(s.choices) == e.choices);
			const ProfileOption *o = c->option(s.option_id);
			REQUIRE(o != nullptr);
			CHECK(o->from_profile == (s.source != "ColorblindSupport"));
			CHECK(option_source_of_key(s.data_key) == option_source_named(s.source));
		}
		CHECK(option_sources().size() == std::size(kExpectedSettings));
	}

	TEST_CASE("key bindings: the five tabs of input/inputconfiguration's user maps") {
		OptionsLevel *l = options_level_or_skip();
		if (l == nullptr) {
			return;
		}
		Result<KeyBindingMaps> maps = read_key_binding_maps(*l->lib);
		REQUIRE_MESSAGE(maps.ok(), (maps.ok() ? std::string() : maps.error().message));
		std::string ids;
		for (const InputActionMap &m : maps->maps) {
			ids += m.id + "=" + m.partition + "/" + std::to_string(m.settings_scheme) + " ";
		}
		std::printf("ui.options maps %s\n", ids.c_str());
		CHECK(ids ==
				"infantry=input/venicesoldierinputconcepts/0 mav=input/venicemavinputconcepts/0 "
				"jet=input/venicejetinputconcepts/3 "
				"heli=input/veniceheliinputconcepts/2 gunner=input/venicegunnerinputconcepts/0 "
				"gunship=input/veniceac130inputconcepts/1 "
				"vehicle=input/venicetankinputconcepts/1 default=input/defaultinputconcepts/0 ");
		Result<std::vector<std::string>> tabs = read_key_binding_tabs(*l->lib);
		REQUIRE(tabs.ok());
		CHECK(joined(*tabs) ==
				"ID_M_OPTIONS_ON_FOOT;ID_M_OPTIONS_IN_VEHICLE;ID_M_OPTIONS_IN_JET;ID_M_OPTIONS_IN_HELI;ID_M_OPTIONS_"
				"ALL");
		Result<KeyBindingTable> t = KeyBindingTable::build(*maps, *tabs);
		REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
		for (const KeyBindingTab &tab : t->tabs()) {
			std::printf("ui.options key tab %s %s %zu rows\n", tab.label.c_str(), tab.map_id.c_str(), tab.rows.size());
		}
		REQUIRE(t->tabs().size() == 5);
		CHECK(t->tabs()[0].rows.size() == 46);
		CHECK(t->tabs()[1].rows.size() == 32);
		CHECK(t->tabs()[2].rows.size() == 22);
		CHECK(t->tabs()[3].rows.size() == 28);
		CHECK(t->tabs()[4].rows.size() == 10);
		const auto row_cells = [&](std::string_view p_map, std::string_view p_concept, bool p_negative) {
			const KeyBindingRow *r = t->row(p_map, p_concept, p_negative);
			return r == nullptr ? std::string("missing") : cells(*r);
		};
		CHECK(row_cells("infantry", "ConceptMoveFB", false) == "ID_M_OPTIONS_CONCEPTMOVEFB_FOOT|IDK_W||IDA_Axis0Y");
		CHECK(row_cells("infantry", "ConceptMoveFB", true) == "ID_M_OPTIONS_CONCEPTMOVEFB_FOOT_NEG|IDK_S||IDA_Axis0Y");
		CHECK(row_cells("infantry", "ConceptPitch", false) == "ID_M_OPTIONS_CONCEPTPITCH_FOOT||IDA_Axis0Y|IDA_Axis1Y");
		CHECK(row_cells("infantry", "ConceptYaw", false) == "ID_M_OPTIONS_CONCEPTYAW_FOOT||IDA_Axis0X|IDA_Axis1X");
		CHECK(row_cells("infantry", "ConceptSprint", false) == "ID_M_OPTIONS_CONCEPTSPRINT|IDK_LeftShift||IDB_Lthumb");
		CHECK(row_cells("infantry", "ConceptCrouch", false) == "ID_M_OPTIONS_CONCEPTCROUCH|IDK_X||");
		CHECK(row_cells("infantry", "ConceptCrouchOnHold", false) == "ID_M_OPTIONS_CONCEPTCROUCHONHOLD|IDK_LeftCtrl||");
		CHECK(row_cells("infantry", "ConceptFire", false) == "ID_M_OPTIONS_CONCEPTFIRE||IDB_Button_0|IDB_Rtrigger");
		CHECK(row_cells("infantry", "ConceptZoom", false) == "ID_M_OPTIONS_CONCEPTZOOM||IDB_Button_1|IDB_Ltrigger");
		CHECK(row_cells("infantry", "ConceptCycleFireMode", false) ==
				"ID_M_OPTIONS_CONCEPTCYCLEFIREMODE|IDK_V||IDB_Ldown");
		CHECK(row_cells("infantry", "ConceptSwitchToStaticGadget", false) ==
				"ID_M_OPTIONS_CONCEPTSELECTINVENTORYITEM3|IDK_3||");
		CHECK(row_cells("infantry", "ConceptCommMenu1", false) == "missing");
		CHECK(row_cells("default", "ConceptScoreboard", false) == "ID_M_OPTIONS_CONCEPTSCOREBOARD|IDK_Tab||IDB_alt");
		CHECK(row_cells("default", "ConceptToggleMinimapType", false) == "ID_M_OPTIONS_CONCEPTTOGGLEMINIMAP|IDK_B||");
		const KeyBindingTab *heli = t->tab_of_map("heli");
		REQUIRE(heli != nullptr);
		std::vector<std::string> footage;
		for (const KeyBindingRow &r : heli->rows) {
			footage.push_back(cells(r));
		}
		const std::vector<std::string> heli_footage = {
			"ID_M_OPTIONS_CONCEPTMOVEFB_HELI|IDK_W||IDB_Rtrigger",
			"ID_M_OPTIONS_CONCEPTMOVEFB_HELI_NEG|IDK_S||IDB_Ltrigger",
			"ID_M_OPTIONS_CONCEPTYAW_HELI|IDK_D||IDA_Axis0X",
			"ID_M_OPTIONS_CONCEPTYAW_HELI_NEG|IDK_A||IDA_Axis0X",
			"ID_M_OPTIONS_CONCEPTPITCH_HELI|IDK_ArrowUp|IDA_Axis0Y|IDA_Axis1Y",
			"ID_M_OPTIONS_CONCEPTPITCH_HELI_NEG|IDK_ArrowDown|IDA_Axis0Y|IDA_Axis1Y",
			"ID_M_OPTIONS_CONCEPTROLL|IDK_ArrowRight|IDA_Axis0X|IDA_Axis1X",
			"ID_M_OPTIONS_CONCEPTROLL_NEG|IDK_ArrowLeft|IDA_Axis0X|IDA_Axis1X",
			"ID_M_OPTIONS_CONCEPTTOGGLECAMERA|IDK_C||IDB_Rthumb",
			"ID_M_OPTIONS_CONCEPTZOOM_AIRCRAFT||IDB_Button_1|IDB_Ldown",
			"ID_M_OPTIONS_CONCEPTCAMERAPITCH||IDA_Axis0Y|IDA_Axis1Y",
			"ID_M_OPTIONS_CONCEPTCAMERAPITCH_NEG||IDA_Axis0Y|IDA_Axis1Y",
			"ID_M_OPTIONS_CONCEPTCAMERAYAW||IDA_Axis0X|IDA_Axis1X",
		};
		REQUIRE(footage.size() >= heli_footage.size());
		CHECK(std::vector<std::string>(footage.begin(), footage.begin() + std::ptrdiff_t(heli_footage.size())) ==
				heli_footage);
		Result<UiInputBindings> ui = read_ui_input_bindings(*l->lib);
		REQUIRE(ui.ok());
		CHECK(ui_input_bindings_with(*ui, *t).bindings == ui->bindings);
	}

	TEST_CASE("options: install unchanged") {
		OptionsLevel *l = options_level_or_skip();
		if (l == nullptr) {
			return;
		}
		const SnapshotDiff diff = diff_snapshots(l->mount->before, l->mount->reader.snapshot());
		std::printf("ui.options install.unchanged %s\n", diff.clean() ? "yes" : diff.summary().c_str());
		CHECK(diff.clean());
	}
}
