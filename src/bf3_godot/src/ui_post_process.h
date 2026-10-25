#pragma once

#include "bf3/core/ui/minimap.h"
#include "bf3/core/ui/ui_post_process.h"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace bf3::godot_side {

inline constexpr int32_t kUiGradeLayer = 0;
inline constexpr int32_t kUiGlitchLayer = 100;
inline constexpr int32_t kUiCursorLayer = 101;
inline constexpr float kUiStageHeight = 720.0f;

struct UiPostProcessStats {
	uint32_t glitch_triggers = 0;
	float last_trigger_power = 0.0f;
	uint32_t glitch_frames = 0;
	uint32_t dim_frames = 0;
	uint32_t dim_ins = 0;
	uint32_t dim_outs = 0;
	uint32_t pending_applied = 0;
};

class Bf3UiPostProcess : public godot::Node {
	GDCLASS(Bf3UiPostProcess, godot::Node)

public:
	Bf3UiPostProcess();
	~Bf3UiPostProcess() override;

	static Bf3UiPostProcess *current();
	static Bf3UiPostProcess *ensure(godot::Node *p_parent, const std::string &p_level);

	void _ready() override;
	void _process(double p_delta) override;
	void _exit_tree() override;

	void set_level(const std::string &p_level);
	const std::string &level() const { return level_; }

	void trigger_glitch(float p_power);
	void dim_in();
	void dim_out();

	void set_manual(bool p_manual) { manual_ = p_manual; }
	void step(double p_seconds);
	double clock() const { return clock_; }

	bool is_ready() const { return built_ && !failed_; }
	bool has_failed() const { return failed_; }
	const std::string &error() const { return error_; }

	float glitch_power() const;
	float dim_visibility() const;
	bool dim_enabled() const;
	const bf3::core::ui::UiPostProcessData *data() const;
	const bf3::core::ui::MenuGrade *grade() const;
	const bf3::core::ui::UiDimmerRamp *ramp() const;
	const bf3::core::ui::L8Image *noise() const;
	const UiPostProcessStats &stats() const { return stats_; }
	godot::Ref<godot::ShaderMaterial> glitch_material() const { return glitch_material_; }
	godot::Ref<godot::ShaderMaterial> grade_material() const { return grade_material_; }
	bool glitch_drawn() const;
	bool grade_drawn() const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	struct Loaded;
	void start_loader();
	void join_loader();
	void build();
	void apply();
	godot::CanvasLayer *make_layer(const char *p_name, int32_t p_layer, const godot::Ref<godot::ShaderMaterial> &p_material, godot::RID &r_item);
	void cover(godot::RID p_item);

	std::string level_ = "MP_Subway";
	bool manual_ = false;
	bool started_ = false;
	bool built_ = false;
	bool failed_ = false;
	std::string error_;
	std::thread loader_;
	std::atomic<bool> loader_done_{ false };
	std::unique_ptr<Loaded> loaded_;

	std::optional<bf3::core::ui::UiPostProcessData> data_;
	std::optional<bf3::core::ui::MenuGrade> grade_;
	std::unique_ptr<bf3::core::ui::UiDimmer> dimmer_;
	bf3::core::ui::L8Image noise_;
	double clock_ = 0.0;
	double glitch_at_ = 0.0;
	float glitch_start_ = 0.0f;
	std::optional<float> pending_glitch_;
	int32_t pending_dim_ = 0;
	UiPostProcessStats stats_;

	godot::CanvasLayer *grade_layer_ = nullptr;
	godot::CanvasLayer *glitch_layer_ = nullptr;
	godot::RID grade_item_;
	godot::RID glitch_item_;
	godot::Ref<godot::ShaderMaterial> grade_material_;
	godot::Ref<godot::ShaderMaterial> glitch_material_;
	godot::Ref<godot::ImageTexture> noise_texture_;
	godot::Vector2 covered_;
};

}
