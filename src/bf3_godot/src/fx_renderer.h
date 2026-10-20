#pragma once

#include "bf3/fx/atlas_pixels.h"
#include "bf3/fx/render_prep.h"
#include "bf3/fx/simulator.h"

#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bf3::godot_side {

inline constexpr int32_t kFxStepTasks = 4;
inline constexpr size_t kFxSerialItems = 16;
inline constexpr uint32_t kFxMinCapacity = 64;

inline constexpr float kFxSelfLitScale = 0.3f;

inline constexpr float kFxEmissiveScale = 0.125f;

inline constexpr float kFxLightEnergyScale = 1.0f;
inline constexpr int32_t kFxParamsRowsPerLine = 64;

struct FxRendererStats {
	uint32_t drawn_instances = 0;
	uint32_t batches = 0;
	uint32_t lights = 0;
	uint32_t capacity = 0;
	double step_ms = 0;
	double prepare_ms = 0;
	double upload_ms = 0;
	uint64_t params_uploads = 0;
	uint64_t reallocations = 0;
	bool atlas_ok = false;
	uint32_t atlas_mips_base = 0, atlas_mips_normal = 0;
};

class Bf3FxRenderer : public godot::Node3D {
	GDCLASS(Bf3FxRenderer, godot::Node3D)

public:
	Bf3FxRenderer();
	~Bf3FxRenderer() override;

	bool upload_atlas(const bf3::fx::AtlasPixels &p_pixels, std::string *r_error);
	void set_atlas_textures(const godot::Ref<godot::Texture2D> &p_base, const godot::Ref<godot::Texture2D> &p_normal);
	void set_soft_particles(bool p_enabled);

	void set_debug_view(int p_mode);

	void set_first_person_projection(float p_fov_override, float p_z_clip_scale);

	void step_world(bf3::fx::FxWorld &r_world, double p_dt, const bf3::fx::FxView &p_view);
	void draw(const bf3::fx::FxWorld &p_world, const bf3::fx::FxView &p_view, const bf3::fx::EmitterAtlas &p_atlas);

	void hide_all();

	void clear();

	const FxRendererStats &stats() const { return stats_; }
	const bf3::fx::RenderPrep &prep() const { return prep_; }

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	struct Batch {
		godot::RID multimesh;
		godot::RID instance;
		godot::Ref<godot::ShaderMaterial> material;
		godot::PackedFloat32Array buffer;
		uint32_t capacity = 0;
		uint32_t visible = 0;
		bool shown = false;
	};
	struct Light {
		godot::RID light;
		godot::RID instance;
		bool shown = false;
	};

	bool ensure_resources();
	void run_chunk(uint32_t p_index);
	void upload_params();

	bf3::fx::RenderPrep prep_;
	std::array<Batch, bf3::fx::kMaxFxBatches> batches_{};
	std::array<Light, bf3::fx::kMaxFxLights> lights_{};
	godot::RID scenario_;
	godot::Ref<godot::Resource> quad_mesh_;
	godot::Ref<godot::Resource> box_mesh_;
	godot::Ref<godot::Texture2D> base_atlas_;
	godot::Ref<godot::Texture2D> normal_atlas_;
	godot::Ref<godot::ImageTexture> params_tex_;
	std::vector<float> params_staging_;
	std::vector<float> params_uploaded_;
	int32_t params_lines_ = 0;
	bool resources_ = false;
	bool soft_ = true;
	int debug_view_ = 0;
	float fp_fov_ = 55.0f;
	float fp_z_clip_ = 0.25f;

	bf3::fx::FxWorld *chunk_world_ = nullptr;
	size_t chunk_items_ = 0;
	size_t chunk_size_ = 0;
	FxRendererStats stats_;
};

}
