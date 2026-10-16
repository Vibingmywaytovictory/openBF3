#pragma once
// Bf3FxRenderer: VFX2's Godot side (docs/design/m2a.md §12.1; as built: docs/design/m2a-fx2.md). Draws one FxWorld:
//  * step_world(): the frame's sub-steps, each sub-step's emitter items on Godot's WorkerThreadPool (<= kFxStepTasks
//    group tasks; a sub-step with fewer than kFxSerialItems items runs on the calling thread);
//  * draw(): bf3::fx::RenderPrep (instances per batch, back-to-front by emitter depth, the light pool), then one
//    MultiMesh per batch (kMaxFxBatches RenderingServer instances, multimesh_set_buffer of the whole capacity,
//    visible_instance_count = the batch's instances, a custom AABB), the params rows as a small RGBAF texture
//    (re-uploaded only when they change) and up to kMaxFxLights omni lights (no shadows).
// Materials: one ShaderMaterial per batch (res://shaders/fx_alpha|fx_additive|fx_premultiplied.gdshader, uniform
// shader_class and the 1p flags); render_priority is the batch's draw position, so batches draw in RenderPrep's order
// (alpha and premultiplied before additive, the FirstPerson pass last). Shadows are off for every FX instance.
// Atlas: upload_atlas() makes the two ImageTextures from bf3::fx::AtlasPixels (BC3 sRGB base, BC5 normal; every mip),
// or set_atlas_textures() takes textures (the fx_ref bench's synthetic one).
// Main thread only (RenderingServer calls); the node frees every RID it made in clear() / NOTIFICATION_PREDELETE.

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

inline constexpr int32_t kFxStepTasks = 4; // WorkerThreadPool group tasks per sub-step
inline constexpr size_t kFxSerialItems = 16; // fewer items: run on the calling thread
inline constexpr uint32_t kFxMinCapacity = 64; // MultiMesh instances allocated at least (then powers of two)
// Omni energy = summed intensity x the colour's largest component x this [inferred; the user judges the look].
inline constexpr float kFxLightEnergyScale = 1.0f;
inline constexpr int32_t kFxParamsRowsPerLine = 64; // fx_common.gdshaderinc's constant

struct FxRendererStats {
	uint32_t drawn_instances = 0; // visible MultiMesh instances this frame (== FxWorld::particles().size(), E8 (3))
	uint32_t batches = 0;
	uint32_t lights = 0; // omni lights shown this frame (<= kMaxFxLights)
	uint32_t capacity = 0; // allocated MultiMesh instances over every batch
	double step_ms = 0; // the last step_world, wall
	double prepare_ms = 0; // RenderPrep::prepare
	double upload_ms = 0; // buffers, params texture, lights
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

	// Creates the two atlas textures (Image::create_from_data with every mip). False with r_error on a refused chain.
	bool upload_atlas(const bf3::fx::AtlasPixels &p_pixels, std::string *r_error);
	void set_atlas_textures(const godot::Ref<godot::Texture2D> &p_base, const godot::Ref<godot::Texture2D> &p_normal);
	void set_soft_particles(bool p_enabled);
	// The viewmodel's projection for the FirstPerson pass (render_bridge.h MaterialOptions defaults).
	void set_first_person_projection(float p_fov_override, float p_z_clip_scale);

	void step_world(bf3::fx::FxWorld &r_world, double p_dt, const bf3::fx::FxView &p_view);
	void draw(const bf3::fx::FxWorld &p_world, const bf3::fx::FxView &p_view, const bf3::fx::EmitterAtlas &p_atlas);
	// Hides every batch and light (the bench's off windows) without freeing anything.
	void hide_all();
	// Frees every RID and material; the next draw() recreates them.
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
	std::vector<float> params_staging_; // line-padded rows as uploaded
	std::vector<float> params_uploaded_;
	int32_t params_lines_ = 0;
	bool resources_ = false;
	bool soft_ = true;
	float fp_fov_ = 55.0f;
	float fp_z_clip_ = 0.25f;
	// The sub-step being run by run_chunk.
	bf3::fx::FxWorld *chunk_world_ = nullptr;
	size_t chunk_items_ = 0;
	size_t chunk_size_ = 0;
	FxRendererStats stats_;
};

} // namespace bf3::godot_side
