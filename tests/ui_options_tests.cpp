#include <doctest/doctest.h>

#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/ui/hud_model.h"
#include "bf3/core/ui/key_bindings.h"
#include "bf3/core/ui/options.h"
#include "bf3/core/ui/options_model.h"
#include "bf3/core/ui/settings_effects.h"
#include "bf3/core/ui/settings_store.h"
#include "bf3/core/ui/ui_graph.h"
#include "bf3/core/ui/ui_input.h"
#include "test_support.h"
#include "ui_retail_support.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
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

ProfileOption numeric(
		std::string_view p_id, ProfileOptionKind p_kind, double p_min, double p_max, double p_step, double p_value) {
	ProfileOption o;
	o.id = std::string(p_id);
	o.kind = p_kind;
	o.min = p_min;
	o.max = p_max;
	o.step = p_step;
	o.value = p_value;
	return o;
}

OptionsCatalog sample_catalog() {
	OptionsCatalog c;
	c.stored.push_back(numeric("Volume", ProfileOptionKind::Float, 0.0, 1.0, 0.01, 1.0));
	c.stored.push_back(numeric("VSync", ProfileOptionKind::Bool, 0.0, 1.0, 1.0, 0.0));
	ProfileOption map = numeric("MinimapType", ProfileOptionKind::Enum, 0.0, 2.0, 1.0, 0.0);
	map.items = { { "ID_M_SATELLITE", true }, { "ID_M_HYBRID", false }, { "ID_M_DETAILTEXTURE", false } };
	c.stored.push_back(map);
	c.stored.push_back(numeric("FieldOfView", ProfileOptionKind::Float, 60.0, 90.0, 1.0, 70.0));
	c.stored.push_back(numeric(kResolutionWidth, ProfileOptionKind::Int, 0.0, 100000.0, 1.0, 0.0));
	c.stored.push_back(numeric(kResolutionHeight, ProfileOptionKind::Int, 0.0, 100000.0, 1.0, 0.0));
	c.stored.push_back(numeric(kResolutionHertz, ProfileOptionKind::Int, 1.0, 10000.0, 1.0, 0.0));
	c.stored.push_back(numeric("FullscreenEnabled", ProfileOptionKind::Bool, 0.0, 1.0, 1.0, 0.0));
	c.stored.push_back(numeric("SoundSystemSize", ProfileOptionKind::Int, 5.0, 100.0, 5.0, 20.0));
	ProfileOption award = numeric("TrackAward0", ProfileOptionKind::String, 0.0, 0.0, 0.0, 0.0);
	award.max_length = 8;
	c.stored.push_back(award);
	const auto add = [&](std::string_view p_source, OptionKind p_kind, std::string_view p_option, OptionRowType p_row,
							 std::vector<std::string> p_choices) {
		OptionSetting s;
		s.source = std::string(p_source);
		s.kind = p_kind;
		s.option_id = std::string(p_option);
		s.row = p_row;
		s.label = "ID_" + s.source;
		s.data_key = option_source_named(p_source)->data_key;
		const ProfileOption *o = c.option(p_option);
		s.min = o->min;
		s.max = o->max;
		s.step = o->step;
		s.default_value = o->default_number();
		s.choices = std::move(p_choices);
		c.settings.push_back(std::move(s));
	};
	std::vector<std::string> fov;
	for (int v = 60; v <= 90; ++v) {
		fov.push_back(std::to_string(v));
	}
	add("MasterVolume", OptionKind::Number, "Volume", OptionRowType::BarSelector, {});
	add("VSync", OptionKind::Bool, "VSync", OptionRowType::TextSelector, { "ID_M_OPTIONS_OFF", "ID_M_OPTIONS_ON" });
	add("MinimapType", OptionKind::Enum, "MinimapType", OptionRowType::TextSelector,
			{ "ID_M_SATELLITE", "ID_M_HYBRID", "ID_M_DETAILTEXTURE" });
	add("FieldOfView", OptionKind::Number, "FieldOfView", OptionRowType::TextSelector, fov);
	add("Resolution", OptionKind::DisplayMode, kResolutionWidth, OptionRowType::TextSelector, {});
	add("FullscreenEnabled", OptionKind::Bool, "FullscreenEnabled", OptionRowType::TextSelector,
			{ "ID_M_OPTIONS_OFF", "ID_M_OPTIONS_ON" });
	return c;
}

OptionsCatalog effects_catalog() {
	OptionsCatalog c;
	c.stored.push_back(numeric("Volume", ProfileOptionKind::Float, 0.0, 1.0, 0.01, 1.0));
	c.stored.push_back(numeric("MusicVolume", ProfileOptionKind::Float, 0.0, 1.0, 0.01, 1.0));
	c.stored.push_back(numeric("DialogueVolume", ProfileOptionKind::Float, 0.0, 1.0, 0.01, 1.0));
	c.stored.push_back(numeric("MouseSensitivity", ProfileOptionKind::Float, 0.0, 1.5, 0.01, 0.5));
	c.stored.push_back(numeric("Scheme0FlipY", ProfileOptionKind::Bool, 0.0, 1.0, 1.0, 0.0));
	c.stored.push_back(numeric("HoldButtonToZoom", ProfileOptionKind::Bool, 0.0, 1.0, 1.0, 1.0));
	c.stored.push_back(numeric("FieldOfView", ProfileOptionKind::Int, 60.0, 90.0, 1.0, 70.0));
	c.stored.push_back(numeric("VSync", ProfileOptionKind::Bool, 0.0, 1.0, 1.0, 0.0));
	c.stored.push_back(numeric(kResolutionWidth, ProfileOptionKind::Int, 0.0, 100000.0, 1.0, 0.0));
	c.stored.push_back(numeric(kResolutionHeight, ProfileOptionKind::Int, 0.0, 100000.0, 1.0, 0.0));
	c.stored.push_back(numeric(kResolutionHertz, ProfileOptionKind::Int, 1.0, 10000.0, 1.0, 0.0));
	c.stored.push_back(numeric("FullscreenEnabled", ProfileOptionKind::Bool, 0.0, 1.0, 1.0, 0.0));
	c.stored.push_back(numeric("FullscreenScreen", ProfileOptionKind::Int, 0.0, 1000.0, 1.0, 0.0));
	return c;
}

int32_t key_of(std::string_view p_source) {
	return option_source_named(p_source)->data_key;
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

	TEST_CASE("settings store: save, load, unknown keys and the version round-trip") {
		const OptionsCatalog cat = sample_catalog();
		SettingsStore s(cat, sample_table());
		CHECK(s.value("Volume") == 1.0);
		CHECK(s.value("FieldOfView") == 70.0);
		CHECK_FALSE(s.settings_changed());
		CHECK(s.change("MasterVolume", 0.25));
		CHECK(s.change("MinimapType", 1.0));
		CHECK(s.change("VSync", 1.0));
		CHECK(s.set_value("SoundSystemSize", 35.0));
		CHECK(s.set_text("TrackAward0", "medal"));
		REQUIRE(s.key_bindings().set({ 0, 2, KeyColumn::Keyboard }, "IDK_J"));
		CHECK_FALSE(s.change("MasterVolume", 1.5));
		CHECK_FALSE(s.change("MinimapType", 3.0));
		CHECK_FALSE(s.change("VSync", 0.5));
		CHECK_FALSE(s.set_value("SoundSystemSize", 35.5));
		CHECK_FALSE(s.set_text("TrackAward0", "far too long"));
		CHECK_FALSE(s.set_text("TrackAward0", "a#b"));
		CHECK_FALSE(s.change("Resolution", 1.0));
		CHECK_FALSE(s.change("Nothing", 1.0));
		CHECK(s.settings_changed());
		s.save();
		CHECK_FALSE(s.settings_changed());
		const std::string text = s.saved_text();
		CHECK(text ==
				"version = 1\nVolume = 0.25\nVSync = true\nMinimapType = ID_M_HYBRID\nFieldOfView = "
				"70\nResolutionWidth = 0\n"
				"ResolutionHeight = 0\nResolutionHertz = 0\nFullscreenEnabled = false\nSoundSystemSize = "
				"35\nTrackAward0 = medal\n"
				"bind.infantry.ConceptJump.keyboard = IDK_J\n");
		SettingsStore back(cat, sample_table());
		Result<uint32_t> v =
				back.load_text(text + "# a note\nFutureOption = 3 4\nbind.jet.ConceptGone.mouse = IDB_Button_4\n");
		REQUIRE_MESSAGE(v.ok(), (v.ok() ? std::string() : v.error().message));
		CHECK(*v == 1);
		CHECK(back.value("Volume") == 0.25);
		CHECK(back.value("MinimapType") == 1.0);
		CHECK(back.text("TrackAward0") == "medal");
		CHECK(back.key_bindings().input({ 0, 2, KeyColumn::Keyboard }) == "IDK_J");
		CHECK_FALSE(back.settings_changed());
		REQUIRE(back.unknown().size() == 2);
		CHECK(back.unknown()[0] == std::pair<std::string, std::string>("FutureOption", "3 4"));
		const std::string kept = back.saved_text();
		CHECK(kept == text + "FutureOption = 3 4\nbind.jet.ConceptGone.mouse = IDB_Button_4\n");
		SettingsStore again(cat, sample_table());
		REQUIRE(again.load_text(kept).ok());
		CHECK(again.saved_text() == kept);
		SettingsStore older(cat, sample_table());
		REQUIRE(older.load_text("version = 1\nVolume = 0.5\n").ok());
		CHECK(older.value("Volume") == 0.5);
		CHECK(older.value("FieldOfView") == 70.0);
		CHECK(older.key_bindings().is_default());
		const uint64_t r = older.revision();
		CHECK_FALSE(older.load_text("Volume = 0.5\n").ok());
		CHECK(older.load_text("version = 2\nVolume = 0.5\n").error().message == "settings: version 2 is newer than 1");
		CHECK_FALSE(older.load_text("version = 1\nVolume = 2\n").ok());
		CHECK_FALSE(older.load_text("version = 1\nMinimapType = ID_M_ROUND\n").ok());
		CHECK_FALSE(older.load_text("version = 1\nVSync = maybe\n").ok());
		CHECK_FALSE(older.load_text("version = 1\nSoundSystemSize = 7.5\n").ok());
		CHECK_FALSE(older.load_text("version = 1\nVolume\n").ok());
		CHECK(older.value("Volume") == 0.5);
		CHECK(older.revision() == r);
	}

	TEST_CASE("settings store: changes apply at once, Save keeps them, revert and reset, video apply and revert") {
		const OptionsCatalog cat = sample_catalog();
		SettingsStore s(cat, sample_table());
		const uint64_t r0 = s.revision();
		CHECK(s.change("MasterVolume", 0.5));
		CHECK(s.value("Volume") == 0.5);
		CHECK(s.revision() > r0);
		CHECK(s.settings_changed());
		CHECK_FALSE(s.video_settings_changed());
		s.revert();
		CHECK(s.value("Volume") == 1.0);
		CHECK_FALSE(s.settings_changed());
		REQUIRE(s.key_bindings().set({ 0, 2, KeyColumn::Keyboard }, "IDK_J"));
		CHECK(s.settings_changed());
		s.revert();
		CHECK(s.key_bindings().input({ 0, 2, KeyColumn::Keyboard }) == "IDK_Space");
		CHECK(s.change("MasterVolume", 0.5));
		s.save();
		s.reset_to_defaults();
		CHECK(s.value("Volume") == 1.0);
		CHECK(s.settings_changed());
		s.revert();
		CHECK(s.value("Volume") == 0.5);
		CHECK(s.set_display_mode(1280, 720, 60));
		CHECK_FALSE(s.set_display_mode(1280, 720, 10001));
		CHECK(s.set_display_mode(0, 0, 0));
		CHECK(s.set_display_mode(1280, 720, 60));
		CHECK(s.change("FullscreenEnabled", 1.0));
		CHECK(s.video_settings_changed());
		CHECK(s.settings_changed());
		s.save();
		s.apply_video();
		CHECK_FALSE(s.video_settings_changed());
		s.revert_video();
		CHECK(s.value(kResolutionWidth) == 0.0);
		CHECK(s.value("FullscreenEnabled") == 0.0);
		CHECK_FALSE(s.video_settings_changed());
		CHECK_FALSE(s.settings_changed());
		CHECK(s.value("Volume") == 0.5);
		CHECK(s.set_display_mode(1920, 1080, 144));
		s.apply_video();
		s.keep_video();
		s.revert_video();
		CHECK(s.value(kResolutionWidth) == 1920.0);
		CHECK(s.setting_value("FullscreenEnabled") == 0.0);
	}

	TEST_CASE("options model: AS2 row data from the store, SetOptionValue, actions and key capture") {
		const OptionsCatalog cat = sample_catalog();
		OptionsSession o(cat, sample_table());
		o.runtime().display_modes = { { 1280, 720, 60 }, { 1920, 1080, 144 } };
		o.runtime().monitors = 2;
		OptionsModel m = o.model();
		REQUIRE(m.rows.size() == 6);
		const OptionRowData *vol = m.row_of_field("master_volume");
		REQUIRE(vol != nullptr);
		CHECK(vol->row == OptionRowType::BarSelector);
		CHECK(vol->label == "ID_MasterVolume");
		CHECK(vol->hint == "ID_MasterVolume_HINT");
		CHECK(vol->from == 0.0);
		CHECK(vol->to == 1.0);
		CHECK(vol->step == 0.01);
		CHECK(vol->value == 1.0);
		const OptionRowData *vsync = m.row_of_field("v_sync");
		REQUIRE(vsync != nullptr);
		CHECK(vsync->choices == std::vector<OptionChoice>{ { "ID_M_OPTIONS_OFF", 0 }, { "ID_M_OPTIONS_ON", 1 } });
		CHECK(vsync->highlighted == 0);
		const OptionRowData *fov = m.row_of_key(key_of("FieldOfView"));
		REQUIRE(fov != nullptr);
		CHECK(fov->choices.size() == 31);
		CHECK(fov->choices[10] == OptionChoice{ "70", 70 });
		CHECK(fov->highlighted == 10);
		const OptionRowData *res = m.row_of_field("resolution");
		REQUIRE(res != nullptr);
		CHECK(res->choices == std::vector<OptionChoice>{ { "1280x720 60.00Hz", 0 }, { "1920x1080 144.00Hz", 1 } });
		CHECK(m.has_field("settings_changed"));
		CHECK(m.has_field("key_bindings_row"));
		CHECK_FALSE(m.has_field("music_volume"));
		CHECK_FALSE(m.settings_changed);

		CHECK(o.set_option_value(key_of("MasterVolume"), 0.333));
		CHECK(o.store().value("Volume") == doctest::Approx(0.33));
		CHECK(o.set_option_value(key_of("FieldOfView"), 75.0));
		CHECK_FALSE(o.set_option_value(key_of("FieldOfView"), 91.0));
		CHECK(o.set_option_value(key_of("MinimapType"), 2.0));
		CHECK(o.set_option_value(key_of("Resolution"), 1.0));
		CHECK_FALSE(o.set_option_value(key_of("Resolution"), 2.0));
		CHECK_FALSE(o.set_option_value(12345, 1.0));
		m = o.model();
		CHECK(m.row_of_field("field_of_view")->highlighted == 15);
		CHECK(m.row_of_field("minimap_type")->highlighted == 2);
		CHECK(m.row_of_field("resolution")->highlighted == 1);
		CHECK(m.settings_changed);
		CHECK(m.video_settings_changed);
		CHECK(o.reset_to_default("FieldOfView"));
		CHECK(o.store().value("FieldOfView") == 70.0);
		CHECK(o.reset_to_default("Resolution"));
		CHECK(o.store().value(kResolutionWidth) == 0.0);
		CHECK_FALSE(o.reset_to_default("Nothing"));
		CHECK(o.run(OptionsAction::Save));
		CHECK_FALSE(o.model().settings_changed);
		CHECK(o.run(OptionsAction::Reset));
		CHECK(o.store().value("MinimapType") == 0.0);
		CHECK(o.run(OptionsAction::Revert));
		CHECK(o.store().value("MinimapType") == 2.0);
		CHECK_FALSE(o.run(OptionsAction::None));
		o.exit();
		CHECK(o.model().exiting_options);
		o.enter();
		CHECK_FALSE(o.model().exiting_options);

		REQUIRE(o.bind_key(2, KeyColumn::Keyboard));
		m = o.model();
		CHECK(m.key_binding_mode);
		CHECK(m.key_bindings_row == 2);
		CHECK(m.key_bindings_column == 0);
		CHECK(o.key_pressed("IDK_X") == KeyBindResult::Conflict);
		CHECK(o.model().key_bind_conflict);
		CHECK(o.run(OptionsAction::ConfirmBind));
		m = o.model();
		CHECK_FALSE(m.key_binding_mode);
		CHECK(m.key_bindings_row == -1);
		CHECK(m.key_bindings[0][2].inputs[0] == "ID_IDK_X");
		CHECK(m.key_bindings[0][2].label == "ID_M_OPTIONS_CONCEPTJUMP");
		CHECK(m.key_tab_labels.size() == 5);
		CHECK(m.settings_changed);

		const std::vector<std::string> save = { "SAVE" };
		const std::vector<std::string> revert = { "REVERT" };
		const std::vector<std::string> apply = { "APPLY" };
		const std::vector<std::string> one = { "1" };
		const std::vector<std::string> zero = { "0" };
		CHECK(options_action_of(-2113864226, save) == OptionsAction::Save);
		CHECK(options_action_of(-2113864226, revert) == OptionsAction::Revert);
		CHECK(options_action_of(-145439803, apply) == OptionsAction::ApplyVideo);
		CHECK(options_action_of(-145439803, revert) == OptionsAction::RevertVideo);
		CHECK(options_action_of(-936324416, {}) == OptionsAction::Reset);
		CHECK(options_action_of(-164353320, {}) == OptionsAction::BindKey);
		CHECK(options_action_of(1887665057, one) == OptionsAction::ConfirmBind);
		CHECK(options_action_of(1887665057, zero) == OptionsAction::RevertBind);
		CHECK(options_action_of(-2113864226, one) == OptionsAction::None);
		CHECK(options_action_of(7, {}) == OptionsAction::None);
		CHECK(options_action_of(-317184342, {}) == OptionsAction::StartResolutionTimer);

		REQUIRE(o.store().set_display_mode(1280, 720, 60));
		REQUIRE(o.run(OptionsAction::ApplyVideo));
		REQUIRE(o.store().set_display_mode(1920, 1080, 60));
		REQUIRE(o.run(OptionsAction::ApplyVideo));
		CHECK_FALSE(o.resolution_timer_running());
		CHECK_FALSE(o.step_resolution_timer(100.0));
		CHECK(o.run(OptionsAction::StartResolutionTimer));
		CHECK(o.resolution_timer_running());
		CHECK_FALSE(o.step_resolution_timer(kResolutionKeepSeconds - 0.5));
		CHECK(o.step_resolution_timer(0.5));
		CHECK_FALSE(o.resolution_timer_running());
		CHECK(o.run(OptionsAction::RevertVideo));
		CHECK(o.store().applied_value(kResolutionWidth) == 1280.0);
		REQUIRE(o.store().set_display_mode(1920, 1080, 60));
		REQUIRE(o.run(OptionsAction::ApplyVideo));
		CHECK(o.run(OptionsAction::StartResolutionTimer));
		o.keep_resolution();
		CHECK_FALSE(o.resolution_timer_running());
		CHECK(o.run(OptionsAction::RevertVideo));
		CHECK(o.store().applied_value(kResolutionWidth) == 1920.0);
		CHECK(o.run(OptionsAction::StartResolutionTimer));
		CHECK(o.run(OptionsAction::RevertVideo));
		CHECK_FALSE(o.resolution_timer_running());
	}

	TEST_CASE("settings effects: the bindings feed the game actions; volumes, look and video follow the store") {
		KeyBindingTable t = sample_table();
		const ActionConcept *jump = action_concept("bf3_jump");
		REQUIRE(jump != nullptr);
		CHECK(action_concept("bf3_nothing") == nullptr);
		CHECK(action_inputs(t, *jump) == std::vector<std::string>{ "IDK_Space" });
		CHECK(action_inputs(t, *action_concept("bf3_forward")) == std::vector<std::string>{ "IDK_W" });
		CHECK(action_inputs(t, *action_concept("bf3_back")) == std::vector<std::string>{ "IDK_S" });
		CHECK(action_inputs(t, *action_concept("bf3_fire")) == std::vector<std::string>{ "IDB_Button_0" });
		CHECK(action_inputs(t, *action_concept("bf3_prone")).empty());
		REQUIRE(t.set({ 0, 2, KeyColumn::Keyboard }, "IDK_J"));
		CHECK(action_inputs(t, *jump) == std::vector<std::string>{ "IDK_J" });
		REQUIRE(t.set({ 0, 3, KeyColumn::Keyboard }, "IDK_K"));
		CHECK(action_inputs(t, *action_concept("bf3_fire")) == std::vector<std::string>{ "IDK_K", "IDB_Button_0" });
		REQUIRE(t.set({ 0, 2, KeyColumn::Keyboard }, ""));
		CHECK(action_inputs(t, *jump).empty());

		const OptionsCatalog cat = effects_catalog();
		SettingsStore s(cat, sample_table());
		SettingsEffects e = settings_effects(s);
		for (const float g : e.bus_gains) {
			CHECK(g == 1.0f);
		}
		CHECK(e.look_scale == 1.0f);
		CHECK(e.hip_fov_deg == 70.0f);
		CHECK_FALSE(e.invert_look);
		CHECK(e.hold_to_zoom);
		CHECK(e.video == VideoEffects{});
		REQUIRE(s.set_value("Volume", 0.5));
		REQUIRE(s.set_value("MusicVolume", 0.5));
		REQUIRE(s.set_value("DialogueVolume", 0.0));
		REQUIRE(s.set_value("MouseSensitivity", 1.0));
		REQUIRE(s.set_value("Scheme0FlipY", 1.0));
		REQUIRE(s.set_value("HoldButtonToZoom", 0.0));
		REQUIRE(s.set_value("FieldOfView", 84.0));
		REQUIRE(s.set_value("VSync", 1.0));
		REQUIRE(s.set_value("FullscreenEnabled", 1.0));
		REQUIRE(s.set_display_mode(1280, 720, 60));
		e = settings_effects(s);
		CHECK(e.bus_gains[size_t(audio::MasterBus::GameMixMain)] == 0.5f);
		CHECK(e.bus_gains[size_t(audio::MasterBus::Reverb)] == 0.5f);
		CHECK(e.bus_gains[size_t(audio::MasterBus::Music)] == 0.25f);
		CHECK(e.bus_gains[size_t(audio::MasterBus::LoadingMusic)] == 0.25f);
		CHECK(e.bus_gains[size_t(audio::MasterBus::Dialog)] == 0.0f);
		CHECK(e.bus_gains[size_t(audio::MasterBus::NarrVoice)] == 0.0f);
		CHECK(e.look_scale == 2.0f);
		CHECK(e.hip_fov_deg == 84.0f);
		CHECK(e.invert_look);
		CHECK_FALSE(e.hold_to_zoom);
		CHECK(e.video.vsync);
		CHECK_FALSE(e.video.fullscreen);
		CHECK(e.video.width == 0);
		const uint64_t before = s.revision();
		s.apply_video();
		CHECK(s.revision() > before);
		e = settings_effects(s);
		CHECK(e.video == VideoEffects{ true, 0, 1280, 720, true });
		s.revert_video();
		CHECK(settings_effects(s).video == VideoEffects{ false, 0, 0, 0, true });
		CHECK(s.saved_revision() == 0);
		s.save();
		CHECK(s.saved_revision() == 1);
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

	TEST_CASE("settings effects: every game action's concept is a key binding row with the install's inputs") {
		OptionsLevel *l = options_level_or_skip();
		if (l == nullptr) {
			return;
		}
		Result<KeyBindingMaps> maps = read_key_binding_maps(*l->lib);
		Result<std::vector<std::string>> tabs = read_key_binding_tabs(*l->lib);
		REQUIRE(maps.ok());
		REQUIRE(tabs.ok());
		Result<KeyBindingTable> t = KeyBindingTable::build(*maps, *tabs);
		REQUIRE(t.ok());
		std::string got;
		for (const ActionConcept &a : kActionConcepts) {
			CAPTURE(a.action);
			CHECK(t->row(a.map, a.concept_id, a.negative) != nullptr);
			got += std::string(a.action) + "=" + joined(action_inputs(*t, a)) + " ";
		}
		std::printf("ui.options game actions %s\n", got.c_str());
		CHECK(got ==
				"bf3_forward=IDK_W bf3_back=IDK_S bf3_left=IDK_A bf3_right=IDK_D bf3_sprint=IDK_LeftShift "
				"bf3_jump=IDK_Space bf3_crouch=IDK_X bf3_crouch_hold=IDK_LeftCtrl bf3_prone=IDK_Z "
				"bf3_fire=IDB_Button_0 bf3_ads=IDB_Button_1 bf3_reload=IDK_R bf3_primary=IDK_1 bf3_sidearm=IDK_2 "
				"bf3_gadget=IDK_3 bf3_grenade=IDK_G bf3_knife=IDK_F bf3_fire_mode=IDK_V bf3_scoreboard=IDK_Tab ");
		Result<OptionsCatalog> c = read_options_catalog(*l->lib);
		REQUIRE(c.ok());
		for (std::string_view id : { kVolumeOption, kMusicVolumeOption, kDialogueVolumeOption, kMouseSensitivityOption,
					 kFlipLookOption, kHoldToZoomOption, kFullscreenOption, kMonitorOption, kVSyncOption }) {
			CAPTURE(id);
			CHECK(c->option(id) != nullptr);
		}
		CHECK(c->option(kVolumeOption)->default_number() == 1.0);
		CHECK(c->option(kMusicVolumeOption)->default_number() == double(0.7f));
		CHECK(c->option(kDialogueVolumeOption)->default_number() == double(0.7f));
		SettingsStore s(*c, std::move(*t));
		const SettingsEffects e = settings_effects(s);
		for (size_t b = 0; b < kMasterBusCount; ++b) {
			const audio::MasterBus bus = audio::MasterBus(b);
			const bool voice_or_music = bus == audio::MasterBus::Music || bus == audio::MasterBus::LoadingMusic ||
					bus == audio::MasterBus::Dialog || bus == audio::MasterBus::NarrVoice;
			CAPTURE(b);
			CHECK(e.bus_gains[b] == (voice_or_music ? 0.7f : 1.0f));
		}
		CHECK(e.look_scale == 1.0f);
		CHECK_FALSE(e.invert_look);
		CHECK(e.hold_to_zoom);
		CHECK(e.video == VideoEffects{});
	}

	TEST_CASE("settings store: the install's defaults round-trip through the text format") {
		OptionsLevel *l = options_level_or_skip();
		if (l == nullptr) {
			return;
		}
		Result<OptionsCatalog> c = read_options_catalog(*l->lib);
		Result<KeyBindingMaps> maps = read_key_binding_maps(*l->lib);
		Result<std::vector<std::string>> tabs = read_key_binding_tabs(*l->lib);
		REQUIRE((c.ok() && maps.ok() && tabs.ok()));
		Result<KeyBindingTable> t = KeyBindingTable::build(*maps, *tabs);
		REQUIRE(t.ok());
		SettingsStore s(*c, *t);
		const std::string defaults = s.saved_text();
		std::printf("ui.options settings text %zu bytes\n", defaults.size());
		CHECK(defaults.size() < kMaxSettingsBytes);
		CHECK(s.value("Volume") == 1.0);
		CHECK(s.value("FieldOfView") == 70.0);
		CHECK(s.value(kResolutionHertz) == 0.0);
		SettingsStore back(*c, *t);
		REQUIRE(back.load_text(defaults).ok());
		CHECK(back.saved_text() == defaults);
		CHECK_FALSE(back.settings_changed());
		CHECK(back.change("MasterVolume", 0.3));
		REQUIRE(back.key_bindings().set({ 0, 0, KeyColumn::Keyboard }, "IDK_ArrowUp"));
		back.save();
		SettingsStore again(*c, *t);
		REQUIRE(again.load_text(back.saved_text()).ok());
		CHECK(again.value("Volume") == doctest::Approx(0.3));
		CHECK(again.key_bindings().input({ 0, 0, KeyColumn::Keyboard }) == "IDK_ArrowUp");
	}

	TEST_CASE("options model: every UISettingsComp model row is a model field; optionsgraph's actions map") {
		OptionsLevel *l = options_level_or_skip();
		if (l == nullptr) {
			return;
		}
		Result<OptionsCatalog> c = read_options_catalog(*l->lib);
		Result<KeyBindingMaps> maps = read_key_binding_maps(*l->lib);
		Result<std::vector<std::string>> tabs = read_key_binding_tabs(*l->lib);
		REQUIRE((c.ok() && maps.ok() && tabs.ok()));
		Result<KeyBindingTable> t = KeyBindingTable::build(*maps, *tabs);
		REQUIRE(t.ok());
		OptionsSession o(*c, *t);
		const OptionsModel m = o.model();
		CHECK(m.rows.size() == 41);
		size_t served = 0;
		for (const HudBinding &b : kHudBindings) {
			if (b.component != "UISettingsCompData" || b.kind != HudBindKind::Model) {
				continue;
			}
			if (b.field == "current_game_mode" || b.field == "current_game_mode_name" || b.field == "is_pc") {
				continue;
			}
			CAPTURE(b.source);
			CHECK(m.has_field(b.field));
			++served;
		}
		CHECK(served == 49);
		for (const OptionRowData &r : m.rows) {
			CAPTURE(r.source);
			if (r.row == OptionRowType::TextSelector && r.source != "Resolution") {
				CHECK(!r.choices.empty());
				CHECK(r.highlighted >= 0);
				CHECK(size_t(r.highlighted) < r.choices.size());
			}
		}
		CHECK(m.row_of_field("minimap_type")->hint == "ID_M_MINIMAPTYPE_HINT");
		Result<UiGraphAsset> g = read_ui_graph_asset(*l->lib, "ui/flow/graph/options/optionsgraph");
		REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
		std::string actions;
		for (const UiGraphNode &n : g->nodes) {
			const OptionsAction a = options_action_of(n.action_key, n.params);
			if (a != OptionsAction::None) {
				actions += n.name + "=" + std::to_string(int(a)) + " ";
			}
		}
		std::printf("ui.options graph actions %s\n", actions.c_str());
		CHECK(actions ==
				"SaveOptions=1 RevertResolution=4 RevertOptions=2 Reset Options=5 ApplyVideoSettings=3 BindKey=6 "
				"ConfirmBindKey=7 RevertBindKey=8 "
				"Reset Options=5 StartChangeResolution Timer=9 ");
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

namespace {

constexpr std::string_view kOptionsGameplayScreen = "ui/flow/screen/optionsgameplayscreen";
constexpr std::string_view kOptionsAudioScreen = "ui/flow/screen/optionsaudioscreen";
constexpr std::string_view kOptionsVideoScreen = "ui/flow/screen/optionsvideoscreen";
constexpr std::string_view kOptionsKeyBindingScreen = "ui/flow/screen/optionskeybindingscreen";
constexpr std::string_view kPopupBindKey = "ui/flow/screen/popups/popupbindkey";
constexpr std::string_view kEnterTop = "UIWidgetEventID_EnterTop";
constexpr int32_t kListHighlighted = 0;
constexpr int32_t kListSelectorChanged = 8;
constexpr int32_t kListSelectorChangedKeyValue = 23;
constexpr int32_t kMenuHintKey = 606381875;
constexpr int32_t kKeyPressedKey = 1279028100;

std::unique_ptr<OptionsSession> retail_options(ui_retail::Retail &r_r) {
	static std::unique_ptr<OptionsCatalog> catalog;
	if (catalog == nullptr) {
		Result<OptionsCatalog> c = read_options_catalog(*r_r.level);
		REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message));
		catalog = std::make_unique<OptionsCatalog>(std::move(*c));
	}
	Result<KeyBindingMaps> maps = read_key_binding_maps(*r_r.level);
	Result<std::vector<std::string>> tabs = read_key_binding_tabs(*r_r.level);
	REQUIRE((maps.ok() && tabs.ok()));
	Result<KeyBindingTable> t = KeyBindingTable::build(*maps, *tabs);
	REQUIRE(t.ok());
	return std::make_unique<OptionsSession>(*catalog, *t);
}

struct OptionsRig {
	std::vector<HudWidgetEvent> events;
	OptionsModel model;
	std::map<int32_t, std::string> flow;
	ui_retail::RetailScreen s;

	As2Clip clip(std::string_view p_path) const {
		return s.hud->clip(std::string(kHudScreenInstance) + "." + std::string(p_path));
	}
	std::vector<std::string> rows(std::string_view p_list) const {
		std::vector<std::pair<float, std::string>> found;
		const SwfSceneNode *n = clip(std::string(p_list) + ".ListHolder").node();
		REQUIRE(n != nullptr);
		for (const SwfNodeId id : n->children) {
			const SwfSceneNode *c = s.scene->node(id);
			if (c != nullptr && c->visible && c->kind == SwfNodeKind::Sprite) {
				found.emplace_back(c->matrix.ty, c->path);
			}
		}
		std::sort(found.begin(), found.end());
		std::vector<std::string> out;
		for (const auto &f : found) {
			out.push_back(f.second);
		}
		return out;
	}
	std::vector<HudWidgetEvent> fired(std::string_view p_instance, int32_t p_index) const {
		std::vector<HudWidgetEvent> out;
		for (const HudWidgetEvent &e : events) {
			if (e.instance == p_instance && e.index == p_index) {
				out.push_back(e);
			}
		}
		return out;
	}
	std::string text(const std::string &p_path) const { return s.hud->clip(p_path).text(); }
};

void open_options(ui_retail::Retail &r_r, OptionsRig &r_b, std::string_view p_partition, OptionsModel p_model) {
	r_b.model = std::move(p_model);
	HudClientState client = ui_retail::retail_text_client(ui_retail::retail_text_db(r_r));
	client.options = &r_b.model;
	client.flow_data = [&r_b](std::string_view, int32_t p_key) -> std::optional<std::string> {
		const auto it = r_b.flow.find(p_key);
		return it == r_b.flow.end() ? std::nullopt : std::optional<std::string>(it->second);
	};
	r_b.s = ui_retail::open_retail_screen(r_r, p_partition, std::move(client));
	r_b.s.hud->set_event_sink([&r_b](const HudWidgetEvent &p_e) { r_b.events.push_back(p_e); });
	const HudModel m;
	r_b.s.hud->initialize(m);
	r_b.s.hud->set_focus(true);
	r_b.s.hud->refresh(m);
}

void step(OptionsRig &r_b, UiInputConcept p_concept) {
	r_b.s.hud->input_concept(int32_t(p_concept), true);
	r_b.s.hud->input_concept(int32_t(p_concept), false);
}

std::vector<const OptionRowData *> tab_rows(const OptionsModel &p_model, OptionsTab p_tab, std::string_view p_list) {
	std::vector<const OptionRowData *> out;
	for (const OptionRowData &r : p_model.rows) {
		if (r.tab == p_tab && r.list == p_list) {
			out.push_back(&r);
		}
	}
	return out;
}

std::string sid(ui_retail::Retail &r_r, std::string_view p_sid) {
	return std::string(ui_retail::retail_text_db(r_r).find_sid(p_sid).value_or("?"));
}
}

TEST_SUITE("ui-options-screens") {
	TEST_CASE("options screens: the tab bar names the five tabs and the gameplay rows bind the options model") {
		ui_retail::Retail *r = ui_retail::retail_or_skip();
		if (r == nullptr) {
			return;
		}
		std::unique_ptr<OptionsSession> o = retail_options(*r);
		OptionsRig b;
		open_options(*r, b, kOptionsGameplayScreen, o->model());
		const std::string holder = "TabBar_01.tabBarHolder.button_";
		const std::array<std::string_view, 5> tabs{ "ID_M_CONTROLS", "ID_M_GAMEPLAY", "ID_M_AUDIO", "ID_M_VIDEO",
			"ID_M_OPTIONS_KEY_BINDINGS" };
		for (size_t i = 0; i < tabs.size(); ++i) {
			CAPTURE(i);
			REQUIRE(b.clip(holder + std::to_string(i)).exists());
			CHECK(b.clip(holder + std::to_string(i) + ".button.label.txt").text() == sid(*r, tabs[i]));
		}
		CHECK_FALSE(b.clip(holder + "5").exists());
		CHECK(b.clip(holder + "1.button.bgHolder.selected_mc").visible());
		CHECK_FALSE(b.clip(holder + "0.button.bgHolder.selected_mc").visible());
		const std::vector<const OptionRowData *> want = tab_rows(b.model, OptionsTab::Gameplay, "List_01");
		const std::vector<std::string> rows = b.rows("List_01");
		REQUIRE(rows.size() == want.size());
		for (size_t i = 0; i < rows.size(); ++i) {
			CAPTURE(want[i]->source);
			CHECK(b.text(rows[i] + ".Label.txtField") == sid(*r, want[i]->label));
			if (want[i]->row == OptionRowType::TextSelector) {
				CHECK(b.text(rows[i] + ".stHolder.selectorLabel") ==
						sid(*r, want[i]->choices[size_t(want[i]->highlighted)].label));
			}
		}
	}

	TEST_CASE("options screens: Right steps a text selector, fires the new value and the row's DataKey") {
		ui_retail::Retail *r = ui_retail::retail_or_skip();
		if (r == nullptr) {
			return;
		}
		std::unique_ptr<OptionsSession> o = retail_options(*r);
		OptionsRig b;
		open_options(*r, b, kOptionsGameplayScreen, o->model());
		const std::vector<const OptionRowData *> want = tab_rows(b.model, OptionsTab::Gameplay, "List_01");
		REQUIRE_FALSE(want.empty());
		REQUIRE(want[0]->row == OptionRowType::TextSelector);
		REQUIRE(b.s.hud->deliver_in_event("List_01", kEnterTop));
		b.events.clear();
		step(b, UiInputConcept::NavigateRight);
		const std::vector<HudWidgetEvent> changed = b.fired("List_01", kListSelectorChanged);
		const std::vector<HudWidgetEvent> keyed = b.fired("List_01", kListSelectorChangedKeyValue);
		REQUIRE(changed.size() == 1);
		REQUIRE(keyed.size() == 1);
		const size_t next = (size_t(want[0]->highlighted) + 1) % want[0]->choices.size();
		CHECK(changed[0].param.to_number() == doctest::Approx(want[0]->choices[next].index));
		CHECK(keyed[0].param.to_number() == doctest::Approx(want[0]->data_key));
		CHECK(b.text(b.rows("List_01")[0] + ".stHolder.selectorLabel") == sid(*r, want[0]->choices[next].label));
		const int32_t key = want[0]->data_key;
		const double value = want[0]->choices[next].index;
		const std::string label = want[0]->choices[next].label;
		REQUIRE(o->set_option_value(key, value));
		b.model = o->model();
		b.s.hud->refresh(HudModel{});
		CHECK(b.text(b.rows("List_01")[0] + ".stHolder.selectorLabel") == sid(*r, label));
	}

	TEST_CASE("options screens: a bar selector steps by its step size and draws the inner bar") {
		ui_retail::Retail *r = ui_retail::retail_or_skip();
		if (r == nullptr) {
			return;
		}
		std::unique_ptr<OptionsSession> o = retail_options(*r);
		OptionsRig b;
		open_options(*r, b, kOptionsAudioScreen, o->model());
		const std::vector<const OptionRowData *> want = tab_rows(b.model, OptionsTab::Audio, "List_01");
		const std::vector<std::string> rows = b.rows("List_01");
		REQUIRE(rows.size() == want.size());
		REQUIRE(want[0]->row == OptionRowType::BarSelector);
		const float border = b.s.hud->clip(rows[0] + ".bar.border").width();
		const auto ratio = [&](double p_v) { return float((p_v - want[0]->from) / (want[0]->to - want[0]->from)); };
		CHECK(b.s.hud->clip(rows[0] + ".bar.innerBar").width() ==
				doctest::Approx((border - 8.0f) * ratio(want[0]->value)));
		REQUIRE(b.s.hud->deliver_in_event("List_01", kEnterTop));
		b.events.clear();
		step(b, UiInputConcept::NavigateLeft);
		const std::vector<HudWidgetEvent> changed = b.fired("List_01", kListSelectorChanged);
		REQUIRE(changed.size() == 2);
		const double v = std::max(want[0]->from, want[0]->value - want[0]->step);
		CHECK(changed[0].param.to_number() == doctest::Approx(v));
		CHECK(changed[1].param.to_number() == doctest::Approx(v));
		CHECK(b.fired("List_01", kListHighlighted).size() == 1);
		const std::vector<HudWidgetEvent> keyed = b.fired("List_01", kListSelectorChangedKeyValue);
		REQUIRE(keyed.size() == 1);
		CHECK(keyed[0].param.to_number() == doctest::Approx(want[0]->data_key));
		CHECK(b.s.hud->clip(rows[0] + ".bar.innerBar").width() == doctest::Approx((border - 8.0f) * ratio(v)));
	}

	TEST_CASE("options screens: the video screen lists its nested rows") {
		ui_retail::Retail *r = ui_retail::retail_or_skip();
		if (r == nullptr) {
			return;
		}
		std::unique_ptr<OptionsSession> o = retail_options(*r);
		OptionsRig b;
		open_options(*r, b, kOptionsVideoScreen, o->model());
		const std::vector<std::string> rows = b.rows("List_01");
		const std::vector<const OptionRowData *> want = tab_rows(b.model, OptionsTab::Video, "List_01");
		std::printf("ui.options.screens video rows %zu of %zu\n", rows.size(), want.size());
		REQUIRE(rows.size() == want.size());
		for (size_t i = 0; i < rows.size(); ++i) {
			CAPTURE(want[i]->source);
			CHECK(b.text(rows[i] + ".Label.txtField") == sid(*r, want[i]->label));
		}
	}

	TEST_CASE("options screens: the key binding list shows the on-foot rows and Right moves the column") {
		ui_retail::Retail *r = ui_retail::retail_or_skip();
		if (r == nullptr) {
			return;
		}
		std::unique_ptr<OptionsSession> o = retail_options(*r);
		OptionsRig b;
		open_options(*r, b, kOptionsKeyBindingScreen, o->model());
		REQUIRE_FALSE(b.model.key_bindings.empty());
		const std::vector<KeyBindingRowData> foot = b.model.key_bindings[0];
		const std::vector<std::string> rows = b.rows("List_01");
		REQUIRE_FALSE(rows.empty());
		CHECK(rows.size() <= foot.size());
		const auto shown = [&](const std::string &p_sid) { return p_sid.empty() ? p_sid : sid(*r, p_sid); };
		CHECK(b.text(rows[0] + ".Label_0.txtField") == shown(foot[0].label));
		CHECK(b.text(rows[0] + ".Label_1.txtField") == shown(foot[0].inputs[0]));
		CHECK(b.s.hud->clip(rows[0] + ".col_0").visible());
		CHECK_FALSE(b.s.hud->clip(rows[0] + ".col_1").visible());
		REQUIRE(b.s.hud->deliver_in_event("List_01", kEnterTop));
		b.events.clear();
		step(b, UiInputConcept::NavigateRight);
		const std::vector<HudWidgetEvent> changed = b.fired("List_01", kListSelectorChanged);
		REQUIRE(changed.size() == 2);
		CHECK(changed[0].param.to_number() == doctest::Approx(1.0));
		CHECK(changed[1].param.to_number() == doctest::Approx(1.0));
		CHECK(b.fired("List_01", kListSelectorChangedKeyValue).empty());
		CHECK(b.s.hud->clip(rows[0] + ".col_0").visible());
		o->select_key_column(1);
		b.model = o->model();
		b.s.hud->refresh(HudModel{});
		CHECK(b.s.hud->clip(rows[0] + ".col_1").visible());
		CHECK_FALSE(b.s.hud->clip(rows[0] + ".col_0").visible());
		CHECK(b.text(rows[0] + ".Label_0.txtField") == shown(foot[0].label));
	}

	TEST_CASE("options screens: the menu hint shows the flow's hint SID") {
		ui_retail::Retail *r = ui_retail::retail_or_skip();
		if (r == nullptr) {
			return;
		}
		std::unique_ptr<OptionsSession> o = retail_options(*r);
		OptionsRig b;
		open_options(*r, b, kOptionsGameplayScreen, o->model());
		CHECK(b.clip("MenuHint_01.hint.text.txt").text().empty());
		b.flow[kMenuHintKey] = "ID_M_MINIMAPTYPE_HINT";
		b.s.hud->refresh(HudModel{});
		CHECK(b.clip("MenuHint_01.hint.text.txt").text() == sid(*r, "ID_M_MINIMAPTYPE_HINT"));
		CHECK(b.clip("MenuHint_01.hint.bg").width() > b.clip("MenuHint_01.hint.text.txt").text_width());
	}

	TEST_CASE("options screens: the bind-key popup's change listener fires KeyChanged when the key arrives") {
		ui_retail::Retail *r = ui_retail::retail_or_skip();
		if (r == nullptr) {
			return;
		}
		std::unique_ptr<OptionsSession> o = retail_options(*r);
		OptionsRig b;
		open_options(*r, b, kPopupBindKey, o->model());
		CHECK_FALSE(b.clip("ChangeListener_01").visible());
		CHECK(b.fired("ChangeListener_01", 0).empty());
		b.flow[kKeyPressedKey] = "False";
		b.s.hud->refresh(HudModel{});
		const std::vector<HudWidgetEvent> pressed = b.fired("ChangeListener_01", 0);
		REQUIRE(pressed.size() == 1);
		CHECK(pressed[0].param.to_string() == "False");
	}
}
