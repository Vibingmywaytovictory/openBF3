#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

inline constexpr std::string_view kProfileOptionsPartition = "profile/profileoptions";
inline constexpr size_t kMaxProfileOptions = 256;
inline constexpr size_t kMaxProfileOptionItems = 32;
inline constexpr size_t kMaxOptionsListRows = 64;

enum class ProfileOptionKind : uint8_t { Bool, Int, Float, Enum, String };

struct ProfileOptionItem {
	std::string display_name;
	bool is_default = false;
	bool operator==(const ProfileOptionItem &) const = default;
};

struct ProfileOption {
	std::string id;
	std::string category;
	ProfileOptionKind kind = ProfileOptionKind::Bool;
	double min = 0.0;
	double max = 0.0;
	double step = 0.0;
	double value = 0.0;
	std::vector<ProfileOptionItem> items;
	std::string text;
	uint32_t max_length = 0;
	bool from_profile = true;

	uint32_t default_item() const;
	double default_number() const;
};

struct ProfileOptions {
	std::string file_name;
	uint32_t file_size = 0;
	bool auto_save_on_quit = false;
	std::vector<ProfileOption> options;

	const ProfileOption *find(std::string_view p_id) const;
};

Result<ProfileOptions> read_profile_options(const EbxResolver &p_db);

enum class OptionsTab : uint8_t { Controls, Gameplay, Audio, Video, KeyBindings, Count };
inline constexpr size_t kOptionsTabCount = size_t(OptionsTab::Count);
inline constexpr std::array<std::string_view, kOptionsTabCount> kOptionsScreens = {
	"ui/flow/screen/optionscontrolsscreen",
	"ui/flow/screen/optionsgameplayscreen",
	"ui/flow/screen/optionsaudioscreen",
	"ui/flow/screen/optionsvideoscreen",
	"ui/flow/screen/optionskeybindingscreen",
};
std::string_view options_tab_name(OptionsTab p_tab);

enum class OptionRowType : uint8_t { TextSelector, BarSelector };

struct OptionsListRow {
	OptionsTab tab = OptionsTab::Controls;
	std::string list;
	std::string label;
	OptionRowType row = OptionRowType::TextSelector;
	int32_t data_key = 0;
	int32_t show_key = 0;
	bool hidden_on_pc = false;
};

Result<std::vector<OptionsListRow>> read_options_rows(const EbxResolver &p_db, OptionsTab p_tab);

enum class OptionValue : uint8_t { Profile, DisplayMode, Monitor, Unbacked };

struct OptionSource {
	int32_t data_key = 0;
	std::string_view source;
	std::string_view profile_id;
	OptionValue value = OptionValue::Profile;
	std::string_view off_sid;
	std::string_view on_sid;
};

std::span<const OptionSource> option_sources();
const OptionSource *option_source_of_key(int32_t p_data_key);
const OptionSource *option_source_named(std::string_view p_source);

inline constexpr std::string_view kResolutionWidth = "ResolutionWidth";
inline constexpr std::string_view kResolutionHeight = "ResolutionHeight";
inline constexpr std::string_view kResolutionHertz = "ResolutionHertz";

enum class OptionKind : uint8_t { Bool, Number, Enum, DisplayMode, Monitor };

struct OptionSetting {
	std::string source;
	OptionsTab tab = OptionsTab::Controls;
	std::string list;
	std::string label;
	OptionRowType row = OptionRowType::TextSelector;
	int32_t data_key = 0;
	OptionKind kind = OptionKind::Bool;
	std::string option_id;
	double min = 0.0;
	double max = 0.0;
	double step = 0.0;
	double default_value = 0.0;
	std::vector<std::string> choices;
};

struct OptionsCatalog {
	std::vector<ProfileOption> stored;
	std::vector<OptionSetting> settings;
	std::string profile_file;

	const ProfileOption *option(std::string_view p_id) const;
	size_t option_index(std::string_view p_id) const;
	const OptionSetting *setting(std::string_view p_source) const;
};

Result<OptionsCatalog> read_options_catalog(const EbxResolver &p_db);

}
