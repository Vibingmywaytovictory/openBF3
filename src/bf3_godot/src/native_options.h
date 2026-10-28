#pragma once

#include "bf3/core/ui/options_model.h"
#include "bf3/core/ui/ui_graph.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

namespace bf3::host {
class UiFlowSession;
struct UiFlowDataWrite;
}

namespace bf3::godot_side {

class Bf3UiScreens;

inline constexpr std::string_view kPopupBindKeyPartition = "ui/flow/screen/popups/popupbindkey";
inline constexpr std::string_view kPopupDelayPartition = "ui/flow/screen/popups/popupdelay";
inline constexpr std::string_view kScreenAdjustPartition = "ui/flow/screen/screenadjustscreen";
inline constexpr std::string_view kOptionsSettingsCategory = "ui/uicomponents/uisettingscomp";
inline constexpr std::string_view kOptionsMemCardCategory = "ui/uicomponents/uimemcardcomp";
inline constexpr std::string_view kOptionsSettingsLeaf = "uisettingscomp";

struct NativeOptionsStats {
	uint64_t values = 0;
	uint64_t refused_values = 0;
	uint64_t tabs = 0;
	uint64_t key_tabs = 0;
	uint64_t saves = 0;
	uint64_t reverts = 0;
	uint64_t resets = 0;
	uint64_t video_applies = 0;
	uint64_t video_reverts = 0;
	uint64_t captures = 0;
	uint64_t keys_bound = 0;
	uint64_t conflicts = 0;
	uint64_t confirms = 0;
	uint64_t cancels = 0;
	uint64_t refused_keys = 0;
	uint64_t entered = 0;
	uint64_t exited = 0;
	uint64_t resolution_timers = 0;
	uint64_t resolutions_kept = 0;
	uint64_t resolution_timeouts = 0;
	uint64_t safe_area_writes = 0;
};

class NativeOptions {
public:
	void build(Bf3UiScreens &r_screens);
	void feed(bf3::host::UiFlowSession &r_session, const bf3::core::ui::OptionsSession &p_options);
	void connect(bf3::host::UiFlowSession &r_session, bf3::core::ui::OptionsSession &r_options);
	void apply_write(const bf3::host::UiFlowDataWrite &p_write, bf3::core::ui::OptionsSession &r_options);
	bool take(const bf3::core::ui::UiFlowAction &p_action, bf3::host::UiFlowSession &r_session,
			bf3::core::ui::OptionsSession &r_options);
	bool key(std::string_view p_input, bf3::host::UiFlowSession &r_session, bf3::core::ui::OptionsSession &r_options);
	void update_model(const bf3::core::ui::OptionsSession &p_options);
	void step(double p_seconds, bf3::host::UiFlowSession &r_session, bf3::core::ui::OptionsSession &r_options);

	bool capturing() const { return capturing_; }
	bool inside() const { return inside_; }
	const bf3::core::ui::OptionsModel &model() const { return model_; }
	const NativeOptionsStats &stats() const { return stats_; }

private:
	bf3::core::ui::OptionsModel model_;
	NativeOptionsStats stats_;
	double pending_value_ = std::numeric_limits<double>::quiet_NaN();
	uint16_t row_ = 0;
	bool capturing_ = false;
	bool inside_ = false;
};

}
