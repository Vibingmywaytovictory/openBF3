#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::core::ui {

inline constexpr std::string_view kUiPostProcessCompPartition = "ui/uicomponents/uipostprocesscomponent";
inline constexpr std::string_view kMenuVisualEnvironmentPartition = "ui/assets/menuvisualenvironment";
inline constexpr std::string_view kIngameDimmerPartition = "ui/flow/logic/uiingamedimmer";
inline constexpr int32_t kPostProcessEffectActionKey = 2033918385;
inline constexpr size_t kMaxDimmerKeys = 16;

struct UiPostProcessData {
	bool enabled = false;
	float r_shift = 0.0f;
	float g_shift = 0.0f;
	float b_shift = 0.0f;
	float distort = 0.0f;
	float x_power = 0.0f;
	float y_power = 0.0f;
	float time_scale = 0.0f;
	float highlight = 0.0f;
	float highlight_threshold = 0.0f;
	std::string noise_texture;
	std::vector<std::string> data_sources;
};

Result<UiPostProcessData> read_ui_post_process(const EbxResolver &p_db);

float ui_glitch_power(const UiPostProcessData &p_data, float p_start_power, double p_elapsed_s);
double ui_glitch_seconds(const UiPostProcessData &p_data, float p_start_power);

struct MenuGrade {
	std::array<float, 3> brightness{ 1.0f, 1.0f, 1.0f };
	std::array<float, 3> contrast{ 1.0f, 1.0f, 1.0f };
	std::array<float, 3> saturation{ 1.0f, 1.0f, 1.0f };
	std::array<float, 3> vignette_color{ 0.0f, 0.0f, 0.0f };
	std::array<float, 2> vignette_scale{ 1.0f, 1.0f };
	float vignette_opacity = 0.0f;
	float vignette_exponent = 1.0f;
	float blur_add = 0.0f;
	float blur_deviation = 0.0f;
	float focus_distance = 0.0f;
	float near_distance_scale = 0.0f;
	float far_distance_scale = 0.0f;
	bool correction = false;
	bool vignette = false;
	bool dof = false;
};

Result<MenuGrade> read_menu_visual_environment(const EbxResolver &p_db);

std::array<float, 3> menu_grade_rgb(const MenuGrade &p_grade, const std::array<float, 3> &p_rgb, float p_visibility);
float menu_vignette(const MenuGrade &p_grade, float p_u, float p_v);

struct DimmerKey {
	float ms = 0.0f;
	float value = 0.0f;
};

struct DimmerTrack {
	int32_t event_id = 0;
	std::vector<DimmerKey> keys;
	float length_ms = 0.0f;
};

struct UiDimmerRamp {
	DimmerTrack in;
	DimmerTrack out;
	std::string visual_environment;
};

Result<UiDimmerRamp> read_ingame_dimmer(const EbxResolver &p_db);

float dimmer_track_value(const DimmerTrack &p_track, double p_elapsed_ms);

class UiDimmer {
public:
	explicit UiDimmer(UiDimmerRamp p_ramp) :
			ramp_(std::move(p_ramp)) {}

	void fade_in(double p_now_s);
	void fade_out(double p_now_s);
	float visibility(double p_now_s) const;
	bool enabled(double p_now_s) const;
	uint32_t fade_ins() const { return fade_ins_; }
	uint32_t fade_outs() const { return fade_outs_; }
	const UiDimmerRamp &ramp() const { return ramp_; }

private:
	UiDimmerRamp ramp_;
	bool in_started_ = false;
	bool out_started_ = false;
	double in_at_s_ = 0.0;
	double out_at_s_ = 0.0;
	uint32_t fade_ins_ = 0;
	uint32_t fade_outs_ = 0;
};

struct PostProcessAction {
	std::string name;
	float power = 0.0f;
};

Result<std::vector<PostProcessAction>> read_post_process_actions(const EbxResolver &p_db, std::string_view p_graph_partition);

}
