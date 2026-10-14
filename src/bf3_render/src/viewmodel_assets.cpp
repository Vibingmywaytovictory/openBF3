#include "bf3/render/viewmodel_assets.h"

#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/thread_pool.h"

#include <chrono>
#include <cstdio>
#include <set>

namespace bf3::render {

using Clock = std::chrono::steady_clock;

namespace {

double since(Clock::time_point p_start) {
	return std::chrono::duration<double>(Clock::now() - p_start).count();
}

const char *texture_format_name(core::res::TextureFormat p_format) {
	using core::res::TextureFormat;
	switch (p_format) {
		case TextureFormat::Dxt1:
			return "DXT1";
		case TextureFormat::Dxt3:
			return "DXT3";
		case TextureFormat::Dxt5:
			return "DXT5";
		case TextureFormat::Dxt5A:
			return "DXT5A";
		case TextureFormat::Dxn:
			return "DXN";
		case TextureFormat::Argb8888:
			return "ARGB8888";
		case TextureFormat::L8:
			return "L8";
		case TextureFormat::NormalDxn:
			return "NormalDXN";
		case TextureFormat::NormalDxt1:
			return "NormalDXT1";
		case TextureFormat::NormalDxt5:
			return "NormalDXT5";
		case TextureFormat::NormalDxt5Rga:
			return "NormalDXT5RGA";
		default:
			return "other";
	}
}

// The 1P arms pair of an appearance unlock: (mesh partition, variation partition).
bool arms_of_appearance(const core::EbxLibrary &p_library, const std::string &p_appearance, std::string &r_mesh, std::string &r_variation) {
	const core::EbxPartition *part = p_library.partition_by_name(p_appearance);
	if (part == nullptr) {
		return false;
	}
	for (const core::EbxInstance &pair : part->instances_of("BlueprintAndVariationPair")) {
		const std::optional<core::EbxInstance> base = p_library.follow(pair.data(), "BaseAsset");
		if (!base) {
			continue;
		}
		const std::optional<std::string_view> name = p_library.name_of(base->partition().guid());
		if (!name || name->find("arms1p") == std::string_view::npos) {
			continue;
		}
		r_mesh = std::string(*name);
		r_variation.clear();
		if (const std::optional<core::EbxInstance> v = p_library.follow(pair.data(), "Variation")) {
			if (const std::optional<std::string_view> vn = p_library.name_of(v->partition().guid())) {
				r_variation = std::string(*vn);
			}
		}
		return true;
	}
	return false;
}

uint32_t variation_hash(const core::EbxLibrary &p_library, const std::string &p_variation) {
	if (p_variation.empty()) {
		return 0;
	}
	const core::EbxPartition *part = p_library.partition_by_name(p_variation);
	if (part == nullptr) {
		return 0;
	}
	const std::optional<core::EbxValue> h = part->primary().get("NameHash");
	return h ? h->as_u32().value_or(0) : 0;
}

core::Result<anim::EbxSkeleton> load_skeleton(const core::EbxLibrary &p_library, const std::string &p_name) {
	const core::EbxPartition *part = p_library.partition_by_name(p_name);
	if (part == nullptr) {
		return core::make_error("no skeleton partition " + p_name);
	}
	return anim::read_ebx_skeleton(*part, p_name);
}

} // namespace

void LoadProgress::set(std::string p_stage, int p_step) {
	std::lock_guard<std::mutex> lock(mutex_);
	stage_ = std::move(p_stage);
	step_ = p_step;
}

std::string LoadProgress::stage() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return stage_;
}

const PreparedTexture *ViewmodelAssets::texture(const std::string &p_name) const {
	for (const PreparedTexture &t : textures) {
		if (t.name == p_name) {
			return &t;
		}
	}
	return nullptr;
}

core::Result<std::unique_ptr<ViewmodelAssets>> load_viewmodel(const core::InstallReader &p_install, const ViewmodelSpec &p_spec, LoadProgress *r_progress) {
	const Clock::time_point start = Clock::now();
	auto progress = [&](const char *p_stage, int p_step) {
		if (r_progress != nullptr) {
			r_progress->set(p_stage, p_step);
		}
	};
	auto a = std::make_unique<ViewmodelAssets>();
	progress("opening the install", 1);
	core::Result<std::shared_ptr<core::Install>> install = core::Install::open(p_install);
	if (!install) {
		return core::error_at("install", install.error());
	}

	// Animation: Ant banks, the static library, the clip and the rig skeleton.
	progress("reading the Ant animation banks", 2);
	Clock::time_point t0 = Clock::now();
	core::Result<std::unique_ptr<core::ant::AntInstallSource>> source = core::ant::AntInstallSource::open(*install);
	if (!source) {
		return core::error_at("ant source", source.error());
	}
	core::Result<std::unique_ptr<core::ant::AntLibrary>> library = anim::load_ant_library(**source);
	if (!library) {
		return library.error();
	}
	a->library = std::move(*library);
	core::Result<anim::LoadedClip> clip = anim::load_clip(**source, *a->library, p_spec.ant_package, p_spec.clip);
	if (!clip) {
		return clip.error();
	}
	a->clip = std::move(*clip);
	core::Result<anim::Skeleton> rig = anim::skeleton_from_rig(*a->clip.binding.rig);
	if (!rig) {
		return rig.error();
	}
	a->rig = std::move(*rig);
	core::Result<anim::ClipTracks> tracks = anim::bind_clip_tracks(a->clip.binding, a->clip.clip, a->rig);
	if (!tracks) {
		return tracks.error();
	}
	a->tracks = std::move(*tracks);
	a->seconds_ant = since(t0);

	// EBX: the level superbundle (meshes, skeletons, variation databases).
	progress("mounting the level superbundle", 3);
	t0 = Clock::now();
	core::Result<std::unique_ptr<core::EbxLibrary>> ebx = core::EbxLibrary::open(*install, { p_spec.superbundle });
	if (!ebx) {
		return core::error_at(p_spec.superbundle, ebx.error());
	}
	const core::EbxLibrary &lib = **ebx;
	a->arms.mesh = p_spec.arms_mesh;
	a->arms.variation = p_spec.arms_variation;
	if (!p_spec.appearance.empty() && arms_of_appearance(lib, p_spec.appearance, a->arms.mesh, a->arms.variation)) {
		a->notes.push_back("arms from " + p_spec.appearance + ": " + a->arms.mesh + " / " + (a->arms.variation.empty() ? "default" : a->arms.variation));
	} else {
		a->notes.push_back("arms: appearance " + p_spec.appearance + " has no 1P arms pair, using " + a->arms.mesh);
	}
	a->arms.variation_hash = variation_hash(lib, a->arms.variation);
	a->weapon.mesh = p_spec.weapon_mesh;
	core::Result<anim::EbxSkeleton> arms_skeleton = load_skeleton(lib, p_spec.arms_skeleton);
	if (!arms_skeleton) {
		return arms_skeleton.error();
	}
	a->arms_skeleton = std::move(*arms_skeleton);
	core::Result<anim::EbxSkeleton> weapon_skeleton = load_skeleton(lib, p_spec.weapon_skeleton);
	if (!weapon_skeleton) {
		return weapon_skeleton.error();
	}
	a->weapon_skeleton = std::move(*weapon_skeleton);
	progress("finding materials", 4);
	const std::vector<std::string> databases = variation_database_names(lib);
	for (ViewmodelPart *part : { &a->arms, &a->weapon }) {
		core::Result<MeshMaterials> m = mesh_materials(lib, part->mesh, part->variation_hash, databases);
		if (!m) {
			return m.error();
		}
		part->materials = std::move(*m);
		char line[256];
		std::snprintf(line, sizeof(line), "%s: %zu materials, variation 0x%08x from %s", part->mesh.c_str(), part->materials.slots.size(),
				part->materials.variation_hash, part->materials.database.empty() ? "(no variation entry)" : part->materials.database.c_str());
		a->notes.push_back(line);
	}
	if (!p_spec.weapon_unlock.empty()) {
		core::Result<core::gameplay::WeaponDef> w = core::gameplay::resolve_weapon(lib, p_spec.weapon_unlock);
		if (w) {
			a->render_fov = w->render_fov;
			a->reload_time = w->fire.reload_time;
			a->reload_threshold = w->fire.reload_threshold;
			char line[256];
			std::snprintf(line, sizeof(line), "%s: RenderFov %.1f, empty reload %.2f s, ReloadThreshold %.2f", p_spec.weapon_unlock.c_str(), a->render_fov,
					a->reload_time, a->reload_threshold);
			a->notes.push_back(line);
		} else {
			a->notes.push_back("weapon " + p_spec.weapon_unlock + " did not resolve: " + w.error().message);
		}
	}
	a->seconds_ebx = since(t0);

	// Meshes: decode LOD 0 (drawn subsets) and bind their bone slots to the rig by name.
	progress("decoding meshes", 5);
	t0 = Clock::now();
	const core::LevelMount &mount = lib.mount(0);
	for (auto [part, skeleton] : { std::pair{ &a->arms, &a->arms_skeleton }, std::pair{ &a->weapon, &a->weapon_skeleton } }) {
		core::Result<core::res::LoadedMeshSet> mesh = core::res::load_mesh_set(mount, part->mesh);
		if (!mesh) {
			return mesh.error();
		}
		if (mesh->mesh.lods.empty()) {
			return core::make_error(part->mesh + ": no LODs");
		}
		core::Result<std::vector<std::vector<int32_t>>> lanes = a->binds.bind_lod(mesh->mesh.lods[0], *skeleton, a->rig);
		if (!lanes) {
			return core::error_at(part->mesh, lanes.error());
		}
		core::Result<PreparedMesh> prepared = prepare_mesh(*mesh, 0, core::res::SubsetFilter::Drawn, &*lanes);
		if (!prepared) {
			return prepared.error();
		}
		part->prepared = std::move(*prepared);
	}
	a->seconds_meshes = since(t0);

	// Textures: Diffuse and Normal of every material a drawn surface uses, decoded on all cores.
	progress("decoding textures", 6);
	t0 = Clock::now();
	std::vector<std::pair<std::string, TextureRole>> wanted;
	std::set<std::string> seen;
	for (const ViewmodelPart *part : { &a->arms, &a->weapon }) {
		for (const PreparedSurface &s : part->prepared.surfaces) {
			if (s.material_index >= part->materials.slots.size()) {
				continue;
			}
			const MaterialSlot &slot = part->materials.slots[s.material_index];
			for (auto [param, role] : { std::pair{ "Diffuse", TextureRole::Color }, std::pair{ "Normal", TextureRole::Normal } }) {
				const std::string name = slot.texture(param);
				if (!name.empty() && seen.insert(name).second) {
					wanted.emplace_back(name, role);
				}
			}
		}
	}
	std::vector<std::string> errors(wanted.size());
	std::vector<PreparedTexture> textures(wanted.size());
	core::parallel_for(wanted.size(), [&](size_t i) {
		core::Result<core::res::LoadedTexture> t = core::res::load_texture(mount, wanted[i].first);
		if (!t) {
			errors[i] = t.error().message;
			return;
		}
		core::Result<PreparedTexture> p = prepare_texture(*t, wanted[i].second);
		if (!p) {
			errors[i] = p.error().message;
			return;
		}
		textures[i] = std::move(*p);
	});
	for (size_t i = 0; i < wanted.size(); ++i) {
		if (!errors[i].empty()) {
			a->notes.push_back("texture skipped: " + errors[i]); // drawn untextured rather than failing the load
			continue;
		}
		const PreparedTexture &t = textures[i];
		char line[320];
		std::snprintf(line, sizeof(line), "texture %s: %ux%u %s%s -> %s, %u of %u levels stored", t.name.c_str(), t.width, t.height,
				texture_format_name(t.source_format), t.srgb ? " sRGB" : "", gpu_format_name(t.format), t.stored_levels, t.levels);
		a->notes.push_back(line);
		a->textures.push_back(std::move(textures[i]));
	}
	a->seconds_textures = since(t0);
	a->seconds_total = since(start);
	progress("uploading", 7);
	return a;
}

} // namespace bf3::render
