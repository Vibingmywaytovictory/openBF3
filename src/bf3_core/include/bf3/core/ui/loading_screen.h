#pragma once

#include "bf3/core/level/level.h"
#include "bf3/core/ui/hud_screen.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

inline constexpr std::string_view kLoadingScreenGameTypeSingleplayer = "singleplayer";
inline constexpr std::string_view kLoadingScreenGameTypeCoop = "coop";
inline constexpr std::string_view kLoadingScreenGameTypeMultiplayer = "multiplayer";
inline constexpr std::string_view kLoadingMessageClass = "Widget.Message.LoadingMessage";

std::string_view loading_screen_game_mode(std::string_view p_mode_id);

std::string loading_screen_movie(std::string_view p_asset_path);

std::string loading_message_path(const HudScreen &p_screen);

struct LoadingScreenSource {
	std::string game_type;
	std::string game_mode;
	std::string level_image;
	std::string level_name;
	std::string level_description;
	std::string platform;
	std::vector<std::string> hints;
	uint64_t seed = 0;
};

LoadingScreenSource loading_screen_source(const level::LevelInfo &p_info, std::string_view p_mode_id,
		std::vector<std::string> p_hints, std::string p_platform, uint64_t p_seed);

class LoadingScreenData final : public HudRootData {
public:
	explicit LoadingScreenData(LoadingScreenSource p_source);

	As2Value get_data(std::string_view p_key) const override;
	void set_data(std::string_view p_key, const As2Value &p_value) override;

	const LoadingScreenSource &source() const { return source_; }
	uint64_t hints_served() const { return served_; }
	const std::string &last_hint() const { return last_hint_; }

private:
	std::string next_hint() const;

	LoadingScreenSource source_;
	std::vector<size_t> order_;
	std::map<std::string, As2Value, std::less<>> written_;
	mutable size_t next_ = 0;
	mutable uint64_t served_ = 0;
	mutable std::string last_hint_;
};

}
