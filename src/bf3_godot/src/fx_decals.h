#pragma once
// FxDecalPool: VFX3's impact decals (docs/design/m2a.md §12.2; owner fx3; as built: docs/design/m2a-fx3.md §5). At most
// fx::kMaxDecals Godot Decal nodes under one Node3D; a new decal past the cap takes the oldest node. Each decal is a
// DecalTemplateData (fx::DecalTemplate: Size + U x RandomSize metres square, Rotation +- RandomRotation about the
// normal, fx::decal_placement from the cue's seed) with its diffuse texture (fx::pick_decal_texture [inferred]) as
// the albedo; it fades out over kDecalFadeOutSeconds once it is fx::kDecalFadeSeconds old.
// Godot's Decal projects along its local -Y, so the node's basis is the cue's effect frame (local +Y = the surface
// normal) turned about Y by the placement's rotation; its box is size x kDecalDepth x size, centred NormalOffset above
// the surface. ClipAngle becomes the normal fade (0.5 cos(ClipAngle) + 0.5: Godot fades by smoothstep(normal_fade, 1,
// 0.5 n.d + 0.5) [inferred from Godot's scene shader]).
// Loader thread: FxDecalAssets::load (templates and prepared textures, Godot-free); main thread: build, place, step.

#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"
#include "bf3/core/superbundle_mount.h"
#include "bf3/fx/triggers.h"
#include "bf3/render/texture_prep.h"

#include <godot_cpp/classes/decal.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/node3d.hpp>

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::godot_side {

inline constexpr float kDecalDepth = 0.12f; // metres the box reaches along the normal (half above, half below) [inferred]
inline constexpr float kDecalFadeOutSeconds = 5.0f; // [inferred]

// The level's decal templates and their textures, prepared on the loader thread.
struct FxDecalAssets {
	std::vector<fx::DecalTemplate> templates; // sorted by name
	std::map<std::string, render::PreparedTexture, std::less<>> textures; // by texture partition name
	std::vector<std::string> errors; // "<decal or texture>: <message>" (counted, the decal is skipped or untextured)
	uint32_t without_texture = 0; // templates with no texture found
	uint64_t texture_bytes = 0;

	// p_names: every decal the impact table's cells name and the scorch mark (fx::impact_decal_names + kScorchDecal).
	static FxDecalAssets load(const core::EbxLibrary &p_level, const core::LevelMount &p_mount, std::span<const std::string> p_names);
	const fx::DecalTemplate *find(std::string_view p_name) const;
};

struct FxDecalStats {
	uint64_t placed = 0;
	uint64_t replaced = 0; // placed on the oldest node (the pool was full)
	uint64_t no_template = 0; // a cue whose decal has no template (skipped)
	uint64_t untextured = 0; // placed without a texture: skipped (Godot would draw nothing)
	uint64_t faded = 0; // decals that faded out completely
	uint32_t live = 0;
	uint32_t textures = 0; // uploaded
};

class FxDecalPool {
public:
	FxDecalPool() = default;
	~FxDecalPool(); // clear()
	FxDecalPool(const FxDecalPool &) = delete;
	FxDecalPool &operator=(const FxDecalPool &) = delete;

	// Main thread: the parent node "FxDecals" under p_parent, one ImageTexture per prepared texture (render_bridge
	// make_texture). Keeps a pointer to p_assets (it must outlive the pool).
	bool build(godot::Node3D *p_parent, const FxDecalAssets &p_assets, std::string *r_error);
	bool built() const { return root_ != nullptr; }
	void place(const fx::DecalCue &p_cue);
	// Ages and fades every live decal (p_dt seconds).
	void step(double p_dt);
	// Frees the nodes (main thread; idempotent).
	void clear();
	const FxDecalStats &stats() const { return stats_; }

private:
	struct Slot {
		godot::Decal *node = nullptr;
		double age = 0;
		bool live = false;
		uint64_t order = 0;
	};
	const FxDecalAssets *assets_ = nullptr;
	godot::Node3D *root_ = nullptr;
	uint64_t root_id_ = 0; // ObjectID: the parent may free the node before clear()
	std::map<std::string, godot::Ref<godot::ImageTexture>, std::less<>> textures_;
	std::vector<Slot> slots_;
	uint64_t next_order_ = 0;
	FxDecalStats stats_;
};

} // namespace bf3::godot_side
