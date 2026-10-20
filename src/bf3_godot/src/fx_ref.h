#pragma once

#include "fx_renderer.h"

#include "bf3/fx/emitter_atlas.h"
#include "bf3/fx/simulator.h"

#include <godot_cpp/classes/node3d.hpp>

#include <memory>
#include <string>
#include <vector>

namespace godot {
class Camera3D;
class Node3D;
}

namespace bf3::godot_side {

inline constexpr double kFxRefRatioMax = 1.25;
inline constexpr double kFxRefWindowSeconds = 5.0;
inline constexpr double kFxRefSkipSeconds = 1.0;
inline constexpr uint32_t kFxRefParticles = 16384;
inline constexpr uint32_t kFxRefLights = 64;

class Bf3FxRef : public godot::Node3D {
	GDCLASS(Bf3FxRef, godot::Node3D)

public:
	Bf3FxRef();
	~Bf3FxRef() override;

	void _ready() override;
	void _process(double p_delta) override;

protected:
	static void _bind_methods();

private:
	enum class Row { RefUnlit, RefLitSoft, RefLitSoft64, Gpu16x1000, Vfx2, Vfx2_64, Count };
	struct RowResult {
		std::string name;
		std::vector<double> on_gpu, off_gpu, on_frame, off_frame;
		uint32_t windows = 0;
		double cost_gpu = 0, on_p50 = 0, off_p50 = 0, frame_on_p50 = 0, frame_off_p50 = 0;
		uint32_t drawn_vfx2 = 0;
		bool shot = false;
		std::vector<double> step_ms, prepare_ms, upload_ms;
	};

	void build();
	void set_row(Row p_row, bool p_on);
	void report_and_finish();

	godot::Camera3D *camera_ = nullptr;
	godot::Node3D *ref_unlit_ = nullptr;
	godot::Node3D *ref_lit_ = nullptr;
	godot::Node3D *gpu_ = nullptr;
	godot::Node3D *lights_ = nullptr;
	Bf3FxRenderer *renderer_ = nullptr;
	std::unique_ptr<bf3::fx::FxWorld> world_;
	bf3::fx::EmitterAtlas empty_atlas_;
	bool built_ = false, finished_ = false, check_ = false;
	std::string shot_dir_;
	bool static_ = false, no_soft_ = false;
	double busy_ms_ = 0;
	uint32_t pairs_ = 3;
	double window_ = kFxRefWindowSeconds;
	int warmup_frames_ = 120;
	size_t row_index_ = 0;
	uint32_t window_index_ = 0;
	double window_t_ = 0;
	std::vector<RowResult> results_;
	std::vector<std::string> report_;
	bool vfx2_drawing_ = false;
	uint64_t vfx2_mismatch_ = 0;
};

}
