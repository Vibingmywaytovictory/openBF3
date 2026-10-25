#include <doctest/doctest.h>

#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/install_snapshot.h"
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
