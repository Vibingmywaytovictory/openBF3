#pragma once
// Bf3FxRef: the Godot reference workloads of research gap-r2-vfx §6.2, re-created, and VFX2's 16k-particle row
// (docs/design/m2a-fx2.md §7; E9 item 8). Scene res://scenes/fx_ref.tscn = Node3D root > FxRef (Bf3FxRef); no
// install, no level: a floor plane, a directional light, ambient light, a synthetic 256 x 256 smoke-puff texture and
// 16,384 quads of 1 m at seeded positions in a 40 x 7.5 x 30 m box in front of the camera, 1920 x 1080 borderless,
// vsync off.
//
// Rows (each the S3 G1b method: alternating on / off windows of kFxRefWindowSeconds, the first kFxRefSkipSeconds of
// a window dropped; cost = p50 GPU time of the on windows - p50 of the off windows, viewport measured render time):
//   ref-unlit     MultiMesh 16k, StandardMaterial3D billboard, alpha, unshaded
//   ref-lit-soft  MultiMesh 16k, the same lit per pixel with proximity fade 1 m (the gate's reference row)
//   ref-lit-soft+64  the same plus 64 omni lights (range 6 m, no shadows)
//   gpu-16x1000   16 GPUParticles3D of 1,000 with the lit-soft material (reported)
//   vfx2          VFX2: one FxWorld of 16 effects of 1,024 particles uniform in the 16 sub-boxes of the same box (a
//                 synthetic SAQ emitter with a Box spawn position, lit, soft fade 1 m), drawn by Bf3FxRenderer
//                 (fx_alpha.gdshader)
//   vfx2+64       the same plus the 64 omni lights
// Gate (godot.fx_ref): cost(vfx2) <= kFxRefRatioMax x cost(ref-lit-soft) (§16 Q17 default: the relative GPU row).
// Shared machine: GPU times are noisy; the wrapper runs it under the quiet lock.
//
// User args (after "--"): --fx-ref-check (the gate run: report and exit 0 / 1), --fx-ref-pairs=<n> (on / off pairs per
// row, default 3), --fx-ref-window=<s> (default 5), --fx-ref-shot-dir=<dir> (one picture per row; synthetic content).

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
} // namespace godot

namespace bf3::godot_side {

inline constexpr double kFxRefRatioMax = 1.25; // §16 Q17 default
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
		std::vector<double> step_ms, prepare_ms, upload_ms; // VFX2 rows: Bf3FxRenderer's main-thread parts
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
	bf3::fx::EmitterAtlas empty_atlas_; // no entries: every emitter samples the whole synthetic texture
	bool built_ = false, finished_ = false, check_ = false;
	std::string shot_dir_; // --fx-ref-shot-dir: one synthetic picture per row
	bool static_ = false, no_soft_ = false; // diagnostics (--fx-ref-static, --fx-ref-nosoft)
	double busy_ms_ = 0; // diagnostics (--fx-ref-busy=<ms>)
	uint32_t pairs_ = 3;
	double window_ = kFxRefWindowSeconds;
	int warmup_frames_ = 120;
	size_t row_index_ = 0;
	uint32_t window_index_ = 0; // even: on, odd: off
	double window_t_ = 0;
	std::vector<RowResult> results_;
	std::vector<std::string> report_;
	bool vfx2_drawing_ = false;
	uint64_t vfx2_mismatch_ = 0;
};

} // namespace bf3::godot_side
