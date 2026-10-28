#pragma once

#include "bf3/core/result.h"
#include "bf3/core/ui/options.h"
#include "bf3/core/ui/options_model.h"
#include "bf3/core/ui/screen_adjust.h"
#include "bf3/core/ui/settings_effects.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace bf3::core {
class EbxResolver;
}

namespace bf3::audio {
class MixerCore;
}

namespace bf3::godot_side {

class InputMapper;

inline constexpr std::string_view kSettingsFile = "user://settings.cfg";
inline constexpr std::string_view kRefusedSettingsSuffix = ".refused";

std::string settings_file_of_args();

class GameSettings {
public:
	static core::Result<std::unique_ptr<GameSettings>> load(const core::EbxResolver &p_db);
	GameSettings(const GameSettings &) = delete;
	GameSettings &operator=(const GameSettings &) = delete;

	core::ui::OptionsSession &session() { return *session_; }
	const core::ui::OptionsSession &session() const { return *session_; }
	core::ui::SettingsStore &store() { return session_->store(); }
	const core::ui::SettingsStore &store() const { return session_->store(); }

	core::Result<bool> read_file(const std::string &p_path);
	core::Status write_file(const std::string &p_path);
	core::Result<bool> save_if_changed(const std::string &p_path);

	size_t apply_input(InputMapper &r_mapper);
	bool apply_audio(audio::MixerCore &r_mixer);
	bool apply_video();
	void read_display();
	core::Status frame(InputMapper &r_mapper, audio::MixerCore *p_mixer, const std::string &p_path);
	size_t missed_inputs() const { return missed_inputs_; }
	float hip_fov_deg() const { return hip_fov_deg_; }
	bf3::core::ui::ScreenAdjustFractions safe_area() const { return safe_area_; }

private:
	GameSettings() = default;

	core::ui::OptionsCatalog catalog_;
	std::unique_ptr<core::ui::OptionsSession> session_;
	uint64_t input_revision_ = ~0ull;
	uint64_t audio_revision_ = ~0ull;
	const audio::MixerCore *audio_mixer_ = nullptr;
	uint64_t written_revision_ = 0;
	core::ui::VideoEffects video_shown_;
	size_t missed_inputs_ = 0;
	float hip_fov_deg_ = 70.0f;
	bf3::core::ui::ScreenAdjustFractions safe_area_;
};

}
