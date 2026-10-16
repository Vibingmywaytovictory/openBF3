// A kit's first-person viewmodels (docs/design/m2a.md §11.1; docs/design/m2a-vm.md). See viewmodel_set.h.
// M9's load_viewmodel (viewmodel_assets.cpp) generalised to every item of a team's default kits: the shared Ant
// library and level superbundle (ViewmodelSources), the team's 1p arms, each item's 1p mesh, its Ant package's role
// clips (by name), the aiming controller's zoom data, the stand-ins, and the textures of every drawn surface.
#include "bf3/render/viewmodel_set.h"

#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/thread_pool.h"
#include "bf3/render/material_lookup.h"
#include "bf3/render/mesh_cache.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>

namespace bf3::render {

using Clock = std::chrono::steady_clock;
using Role = ViewmodelClipRole;
using Source = ViewmodelClipSource;

static_assert(size_t(ViewmodelClipRole::Count) == anim::kVmClips, "viewmodel_set.h and viewmodel_anim.h list the same roles");
static_assert(uint8_t(ViewmodelClipRole::Sprint) == uint8_t(anim::VmClip::Sprint));
static_assert(uint8_t(ViewmodelClipRole::ZoomIdle) == uint8_t(anim::VmClip::ZoomIdle));
static_assert(uint8_t(ViewmodelClipRole::Knife) == uint8_t(anim::VmClip::Knife));

namespace {

constexpr const char *kRigName = "Venice1pAntSke01";

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

std::optional<std::string> name_of(const core::EbxLibrary &p_lib, const core::EbxInstance &p_instance) {
	const std::optional<std::string_view> n = p_lib.name_of(p_instance.partition().guid());
	if (!n) {
		return std::nullopt;
	}
	return std::string(*n);
}

// The 1p arms pair of an appearance unlock (as viewmodel_assets.cpp): (mesh partition, variation partition).
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
		const std::optional<std::string> name = name_of(p_library, *base);
		if (!name || name->find("arms1p") == std::string::npos) {
			continue;
		}
		r_mesh = *name;
		r_variation.clear();
		if (const std::optional<core::EbxInstance> v = p_library.follow(pair.data(), "Variation")) {
			if (const std::optional<std::string> vn = name_of(p_library, *v)) {
				r_variation = *vn;
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

std::optional<double> number(const std::optional<core::EbxValue> &p_v) {
	if (!p_v) {
		return std::nullopt;
	}
	if (const std::optional<double> d = p_v->as_f64()) {
		return d;
	}
	if (const std::optional<float> f = p_v->as_f32()) {
		return double(*f);
	}
	if (const std::optional<int64_t> i = p_v->as_i64()) {
		return double(*i);
	}
	if (const std::optional<uint64_t> u = p_v->as_u64()) {
		return double(*u);
	}
	return std::nullopt;
}

// A finite, plausible float of the data or p_fallback (hostile data: a counted clamp, never NaN downstream).
float sane(std::optional<double> p_v, float p_fallback, float p_min, float p_max, uint32_t &r_clamped) {
	if (!p_v || !std::isfinite(*p_v)) {
		return p_fallback;
	}
	const double v = *p_v;
	if (v < p_min || v > p_max) {
		++r_clamped;
		return float(std::clamp(v, double(p_min), double(p_max)));
	}
	return float(v);
}

ViewmodelAdsData read_ads(const core::EbxLibrary &p_lib, const std::string &p_controller, std::vector<std::string> &r_notes) {
	ViewmodelAdsData a;
	a.controller = p_controller;
	const core::EbxPartition *part = p_controller.empty() ? nullptr : p_lib.partition_by_name(p_controller);
	if (part == nullptr) {
		r_notes.push_back("aiming controller " + (p_controller.empty() ? std::string("(none)") : p_controller) + " not found: hip only");
		return a;
	}
	const std::vector<core::EbxInstance> sims = part->instances_of("SoldierAimingSimulationData");
	if (sims.empty()) {
		r_notes.push_back("aiming controller " + p_controller + " has no SoldierAimingSimulationData: hip only");
		return a;
	}
	const core::EbxStruct d = sims.front().data();
	uint32_t clamped = 0;
	a.fov_transition = sane(number(d.get("FovTransitionTime")), 0.15f, 0.0f, 5.0f, clamped);
	a.zoom_transition = sane(number(d.get("ZoomTransitionTime")), 0.2f, 0.0f, 5.0f, clamped);
	const std::vector<core::EbxInstance> levels = p_lib.follow_all(d, "ZoomLevels");
	a.zoom_levels = uint32_t(std::min<size_t>(levels.size(), 64));
	if (!levels.empty()) {
		a.hip_fov = sane(number(levels[0].get("FieldOfView")), 55.0f, 1.0f, 170.0f, clamped);
		a.look_speed_hip = sane(number(levels[0].get("LookSpeedMultiplier")), 1.0f, 0.0f, 10.0f, clamped);
		a.zoom_fov = a.hip_fov;
		a.look_speed_zoom = a.look_speed_hip;
	}
	if (levels.size() >= 2) {
		a.zoom_fov = sane(number(levels[1].get("FieldOfView")), a.hip_fov, 1.0f, 170.0f, clamped);
		a.look_speed_zoom = sane(number(levels[1].get("LookSpeedMultiplier")), 1.0f, 0.0f, 10.0f, clamped);
	}
	if (const std::optional<core::EbxValue> arr = d.get("ZoomTransitionTimeArray")) {
		const core::OptionalEbxArray items = arr->as_array();
		if (items) {
			size_t n = 0;
			for (const core::EbxValue e : *items) {
				if (++n > 64) {
					break; // hostile bound
				}
				const std::optional<double> from = number(e.get("FromZoomLevel"));
				const std::optional<double> to = number(e.get("ToZoomLevel"));
				const float fov = sane(number(e.get("FovTransitionTime")), -1.0f, 0.0f, 5.0f, clamped);
				const float zoom = sane(number(e.get("ZoomTransitionTime")), -1.0f, 0.0f, 5.0f, clamped);
				if (from && to && *from == 0 && *to == 1) {
					a.fov_in = fov;
					a.zoom_in = zoom;
				} else if (from && to && *from == 1 && *to == 0) {
					a.fov_out = fov;
					a.zoom_out = zoom;
				}
			}
		}
	}
	if (clamped > 0) {
		r_notes.push_back("aiming controller " + p_controller + ": " + std::to_string(clamped) + " value(s) clamped");
	}
	return a;
}

struct RoleRule {
	Role role;
	std::array<const char *, 3> names; // "__name" candidates, first match wins
};
const RoleRule kRoleRules[] = {
	{ Role::Deploy, { "Deploy Anim", nullptr, nullptr } },
	{ Role::Idle, { "IdlePose Anim", nullptr, nullptr } },
	{ Role::Fire, { "Fire Anim", nullptr, nullptr } },
	{ Role::FireSingle, { "FireSingle Anim", nullptr, nullptr } },
	{ Role::Reload, { "ReloadClipFull Anim", "ReloadFull Anim", "Reload Anim" } },
	{ Role::ReloadEmpty, { "ReloadClipEmpty Anim", "ReloadEmpty Anim", nullptr } },
	{ Role::Bolt, { "BoltAction Anim", "Bolt Anim", "Pump Anim" } },
	{ Role::ZoomIn, { "zoomInOut Anim", "ZoomIn Anim", "zoomIn Anim" } },
	{ Role::ZoomOut, { "ZoomOut Anim", "zoomOut Anim", nullptr } },
	{ Role::ZoomIdle, { "ZoomPose Anim", nullptr, nullptr } },
	{ Role::Throw, { "Throw Anim", nullptr, nullptr } },
	{ Role::Knife, { "Knife Anim", "Stab Anim", nullptr } },
	{ Role::Sprint, { "SprintPose Anim", nullptr, nullptr } },
};

std::string codec_of(const core::ant::GdStruct &p_clip) {
	std::string t(p_clip.type_name());
	const std::string suffix = "AnimationAsset";
	if (t.size() > suffix.size() && t.compare(t.size() - suffix.size(), suffix.size(), suffix) == 0) {
		t.resize(t.size() - suffix.size());
	}
	return t;
}

// Decodes and binds one clip of p_package onto the rig. Unsupported (VBR, Curve) is reported through r_unsupported.
core::Status load_role_clip(const core::ant::AntLibrary &p_library, const core::ant::AntPackage &p_package, const core::ant::GdStruct &p_clip,
		const anim::Skeleton &p_rig, ViewmodelClip &r_clip, bool &r_unsupported) {
	r_unsupported = false;
	r_clip.name = std::string(p_clip.name());
	r_clip.codec = codec_of(p_clip);
	const core::ant::AntResult d = core::ant::decode_clip(p_clip, r_clip.clip.clip);
	if (!d.ok()) {
		r_unsupported = d.status == core::ant::AntStatus::Unsupported;
		return core::make_error("decode " + r_clip.name + ": " + d.message);
	}
	const core::ant::AntResult b = p_library.bind(p_package, p_clip, r_clip.clip.binding);
	if (!b.ok()) {
		return core::make_error("bind " + r_clip.name + ": " + b.message);
	}
	if (r_clip.clip.binding.rig == nullptr || r_clip.clip.binding.rig->name != kRigName) {
		return core::make_error(r_clip.name + ": not on " + std::string(kRigName));
	}
	r_clip.controller = std::string(r_clip.clip.binding.controller_name);
	core::Result<anim::ClipTracks> tracks = anim::bind_clip_tracks(r_clip.clip.binding, r_clip.clip.clip, p_rig);
	if (!tracks) {
		return tracks.error();
	}
	r_clip.tracks = std::move(*tracks);
	r_clip.seconds = r_clip.tracks.duration_seconds();
	r_clip.source = Source::Clip;
	return core::Status::success();
}

// The model-space pose of a clip frame over the base pose (the clip's undriven bones keep the base).
void clip_model(const anim::Skeleton &p_rig, const anim::LocalPose &p_base, const ViewmodelClip &p_clip, float p_frame, std::vector<anim::Xform> &r_model) {
	anim::LocalPose pose = p_base;
	core::ant::ClipPose sample;
	if (core::ant::sample_clip(p_clip.clip.clip, p_frame, sample)) {
		anim::apply_clip_pose(p_clip.tracks, sample, pose);
	}
	anim::model_pose(p_rig, pose, r_model);
}

// Worst rotation (degrees) between two model poses over the bones a clip drives.
double driven_diff(const std::vector<anim::Xform> &p_a, const std::vector<anim::Xform> &p_b, const anim::ClipTracks &p_tracks) {
	std::vector<bool> mask(p_a.size(), false);
	for (const int32_t b : p_tracks.rotation_bone) {
		if (b >= 0 && size_t(b) < mask.size()) {
			mask[size_t(b)] = true;
		}
	}
	return anim::pose_diff(p_a, p_b, mask).max_deg;
}

// The controller name of the static package's idle clip for items without an Ant package [verified: antdump list
// s_basicassets: "IdlePose Anim" under these controllers, on Venice1pAntSke01].
const char *static_idle_controller(ViewmodelItemKind p_kind) {
	switch (p_kind) {
		case ViewmodelItemKind::Knife:
			return "Knife_IdlePose";
		case ViewmodelItemKind::Grenade:
			return "HandGrenade_IdlePose";
		default:
			return nullptr;
	}
}

std::string last_component(const std::string &p_path) {
	const size_t slash = p_path.find_last_of('/');
	return slash == std::string::npos ? p_path : p_path.substr(slash + 1);
}

core::Status prepare_part(const core::LevelMount &p_mount, ViewmodelPart &r_part, const anim::EbxSkeleton &p_skeleton, const anim::Skeleton &p_rig,
		anim::SkinBinds &r_binds, core::Cache *p_cache, std::vector<std::string> &r_notes) {
	const Clock::time_point tm = Clock::now();
	core::Result<MeshHead> head = read_mesh_head(p_mount, r_part.mesh);
	if (!head) {
		return head.error();
	}
	if (head->mesh.lods.empty()) {
		return core::make_error(r_part.mesh + ": no LODs");
	}
	core::Result<std::vector<std::vector<int32_t>>> lanes = r_binds.bind_lod(head->mesh.lods[0], p_skeleton, p_rig);
	if (!lanes) {
		return core::error_at(r_part.mesh, lanes.error());
	}
	MeshRequest request;
	request.lods = { 0 };
	request.filter = core::res::SubsetFilter::Drawn;
	request.lane_binds = &*lanes;
	core::Result<CachedMesh> mesh = load_mesh_cached(p_cache, p_mount, *head, request);
	if (!mesh) {
		return mesh.error();
	}
	if (mesh->mesh.meshes.size() != 1) {
		return core::make_error(r_part.mesh + ": no LOD 0 prepared");
	}
	r_part.prepared = std::move(mesh->mesh.meshes[0]);
	r_part.cache = mesh->item;
	r_part.seconds_mesh = since(tm);
	char line[320];
	std::snprintf(line, sizeof(line), "mesh %s: %s (%.1f KiB blob) in %.2f ms", r_part.mesh.c_str(),
			p_cache != nullptr && p_cache->is_open() ? core::cache_outcome_name(r_part.cache.outcome) : "no cache", double(r_part.cache.payload_bytes) / 1024.0,
			r_part.seconds_mesh * 1000.0);
	r_notes.push_back(line);
	return core::Status::success();
}

} // namespace

const char *viewmodel_clip_role_name(ViewmodelClipRole p_role) {
	return anim::vm_clip_name(anim::VmClip(uint8_t(p_role)));
}

const char *viewmodel_item_kind_name(ViewmodelItemKind p_kind) {
	switch (p_kind) {
		case ViewmodelItemKind::Gun:
			return "gun";
		case ViewmodelItemKind::Grenade:
			return "grenade";
		case ViewmodelItemKind::Knife:
			return "knife";
	}
	return "?";
}

const PreparedTexture *ViewmodelSet::texture(const std::string &p_name) const {
	for (const PreparedTexture &t : textures) {
		if (t.name == p_name) {
			return &t;
		}
	}
	return nullptr;
}

std::array<anim::VmClipRef, anim::kVmClips> viewmodel_clip_refs(const ViewmodelItem &p_item) {
	std::array<anim::VmClipRef, anim::kVmClips> refs{};
	for (size_t r = 0; r < kViewmodelClipRoles; ++r) {
		const ViewmodelClip &c = p_item.clips[r];
		anim::VmClipRef &ref = refs[r];
		switch (c.source) {
			case Source::Clip:
				ref.clip = &c.clip.clip;
				ref.tracks = &c.tracks;
				ref.seconds = c.seconds;
				ref.stand_in = c.blend_instead;
				break;
			case Source::VbrStandIn:
			case Source::ProceduralStandIn:
				ref.seconds = c.seconds;
				ref.stand_in = true;
				break;
			case Source::Missing:
				break;
		}
	}
	return refs;
}

anim::AdsParams viewmodel_ads_params(const ViewmodelItem &p_item, const anim::ViewmodelTuning &p_tuning) {
	const ViewmodelAdsData &a = p_item.ads;
	anim::AdsParams p;
	p.hip_fov = a.hip_fov;
	p.zoom_fov = a.zoom_fov;
	p.look_speed_hip = a.look_speed_hip;
	p.look_speed_zoom = a.look_speed_zoom;
	p.fov_in_seconds = p_tuning.fov_transition_specific && a.fov_in >= 0.0f ? a.fov_in : a.fov_transition;
	p.fov_out_seconds = p_tuning.fov_transition_specific && a.fov_out >= 0.0f ? a.fov_out : a.fov_transition;
	p.zoom_in_seconds = a.zoom_in >= 0.0f ? a.zoom_in : p_tuning.ads_in_seconds;
	p.zoom_out_seconds = a.zoom_out >= 0.0f ? a.zoom_out : p_tuning.ads_out_seconds;
	p.render_fov = p_item.render_fov > 0.0f ? p_item.render_fov : a.hip_fov;
	p.zoom_render_fov = p_item.zoom_render_fov;
	p.can_zoom = a.zoom_levels >= 2;
	return p;
}

core::Result<std::unique_ptr<ViewmodelSources>> open_viewmodel_sources(const core::InstallReader &p_install, const std::string &p_superbundle,
		LoadProgress *r_progress) {
	auto progress = [&](const char *p_stage, int p_step) {
		if (r_progress != nullptr) {
			r_progress->set(p_stage, p_step);
		}
	};
	auto s = std::make_unique<ViewmodelSources>();
	s->superbundle = p_superbundle;
	progress("opening the install", 1);
	core::Result<std::shared_ptr<core::Install>> install = core::Install::open(p_install);
	if (!install) {
		return core::error_at("install", install.error());
	}
	s->install = *install;
	progress("reading the Ant animation banks", 2);
	Clock::time_point t0 = Clock::now();
	core::Result<std::unique_ptr<core::ant::AntInstallSource>> source = core::ant::AntInstallSource::open(s->install);
	if (!source) {
		return core::error_at("ant source", source.error());
	}
	s->ant = std::move(*source);
	core::Result<std::unique_ptr<core::ant::AntLibrary>> library = anim::load_ant_library(*s->ant);
	if (!library) {
		return library.error();
	}
	s->library = std::shared_ptr<core::ant::AntLibrary>(std::move(*library));
	s->seconds_ant = since(t0);
	progress("mounting the level superbundle", 3);
	t0 = Clock::now();
	core::Result<std::unique_ptr<core::EbxLibrary>> ebx = core::EbxLibrary::open(s->install, { p_superbundle });
	if (!ebx) {
		return core::error_at(p_superbundle, ebx.error());
	}
	s->ebx = std::move(*ebx);
	s->seconds_ebx = since(t0);
	return s;
}

core::Result<std::unique_ptr<ViewmodelSet>> load_viewmodel_set(const core::InstallReader &p_install, const ViewmodelSetSpec &p_spec, LoadProgress *r_progress,
		core::Cache *p_cache) {
	core::Result<std::unique_ptr<ViewmodelSources>> sources = open_viewmodel_sources(p_install, p_spec.superbundle, r_progress);
	if (!sources) {
		return sources.error();
	}
	return load_viewmodel_set(**sources, p_spec, r_progress, p_cache);
}

core::Result<std::unique_ptr<ViewmodelSet>> load_viewmodel_set(ViewmodelSources &p_sources, const ViewmodelSetSpec &p_spec, LoadProgress *r_progress,
		core::Cache *p_cache) {
	const Clock::time_point start = Clock::now();
	auto progress = [&](const std::string &p_stage, int p_step) {
		if (r_progress != nullptr) {
			r_progress->set(p_stage, p_step);
		}
	};
	if (p_sources.ebx == nullptr || p_sources.library == nullptr || p_sources.ant == nullptr) {
		return core::make_error("viewmodel set: the sources are not open");
	}
	if (p_spec.superbundle != p_sources.superbundle) {
		return core::make_error("viewmodel set: spec superbundle " + p_spec.superbundle + " != sources " + p_sources.superbundle);
	}
	if (p_spec.items.size() > 16) {
		return core::make_error("viewmodel set: " + std::to_string(p_spec.items.size()) + " items (at most 16)");
	}
	const core::EbxLibrary &lib = *p_sources.ebx;
	const core::ant::AntLibrary &ant = *p_sources.library;
	auto set = std::make_unique<ViewmodelSet>();
	set->library = p_sources.library;

	// The rig: Venice1pAntSke01 of the static package.
	const core::ant::AntRig *rig = nullptr;
	for (const core::ant::AntRig &r : ant.rigs()) {
		if (r.name == kRigName) {
			rig = &r;
			break;
		}
	}
	if (rig == nullptr) {
		return core::make_error(std::string("ant: no rig ") + kRigName);
	}
	core::Result<anim::Skeleton> rig_skeleton = anim::skeleton_from_rig(*rig);
	if (!rig_skeleton) {
		return rig_skeleton.error();
	}
	set->rig = std::move(*rig_skeleton);
	const anim::LocalPose rest = anim::rest_pose(set->rig);

	// Arms and skeletons.
	set->arms.mesh = p_spec.arms_mesh;
	set->arms.variation = p_spec.arms_variation;
	if (!p_spec.appearance.empty() && arms_of_appearance(lib, p_spec.appearance, set->arms.mesh, set->arms.variation)) {
		set->notes.push_back("arms from " + p_spec.appearance + ": " + set->arms.mesh + " / " + (set->arms.variation.empty() ? "default" : set->arms.variation));
	} else {
		set->notes.push_back("arms: appearance " + p_spec.appearance + " has no 1p arms pair, using " + set->arms.mesh + " / " + set->arms.variation);
	}
	set->arms.variation_hash = variation_hash(lib, set->arms.variation);
	if (!p_spec.clips_only) {
		core::Result<anim::EbxSkeleton> arms_skeleton = load_skeleton(lib, p_spec.arms_skeleton);
		if (!arms_skeleton) {
			return arms_skeleton.error();
		}
		set->arms_skeleton = std::move(*arms_skeleton);
		core::Result<anim::EbxSkeleton> weapon_skeleton = load_skeleton(lib, p_spec.weapon_skeleton);
		if (!weapon_skeleton) {
			return weapon_skeleton.error();
		}
		set->weapon_skeleton = std::move(*weapon_skeleton);
	}

	// Items: the WeaponDef, the SoldierWeaponData's mesh and package, the role clips, the aiming controller.
	int item_no = 0;
	for (const ViewmodelItemSpec &spec : p_spec.items) {
		++item_no;
		progress("clips of " + spec.unlock, 4);
		ViewmodelItem item;
		item.unlock = spec.unlock;
		core::Result<core::gameplay::WeaponDef> def = core::gameplay::resolve_weapon(lib, spec.unlock);
		if (!def) {
			return core::error_at(spec.unlock, def.error());
		}
		item.render_fov = def->render_fov;
		item.zoom_render_fov = def->zoom_render_fov;
		item.deploy_time = def->deploy_time;
		item.reload_time = def->fire.reload_time;
		item.reload_time_bullets_left = def->fire.reload_time_bullets_left;
		item.reload_threshold = def->fire.reload_threshold;
		item.magazine_capacity = def->fire.magazine_capacity;
		item.weapon_class = def->weapon_class;
		item.kind = def->fire.logic == core::gameplay::FireLogic::HoldAndRelease ? ViewmodelItemKind::Grenade
				: spec.unlock.find("knife") != std::string::npos				   ? ViewmodelItemKind::Knife
																				   : ViewmodelItemKind::Gun;
		// SoldierWeaponData: WeaponStates[0].Mesh1p, AnimationData, ZoomRenderFov.
		std::string mesh = spec.mesh;
		std::string package = spec.ant_package;
		if (const core::EbxPartition *bp = lib.partition_by_name(def->blueprint)) {
			if (const std::optional<core::EbxInstance> swd = lib.follow(bp->primary().data(), "Object")) {
				uint32_t clamped = 0;
				item.zoom_render_fov_data = sane(number(swd->get("ZoomRenderFov")), 0.0f, 0.0f, 170.0f, clamped);
				if (mesh.empty()) {
					if (const std::optional<core::EbxValue> states = swd->get("WeaponStates")) {
						const core::OptionalEbxArray arr = states->as_array();
						if (arr && arr->size() > 0) {
							if (const std::optional<core::EbxValue> s0 = arr->get(0)) {
								if (const std::optional<core::EbxStruct> st = s0->as_struct()) {
									if (const std::optional<core::EbxInstance> m = lib.follow(*st, "Mesh1p")) {
										mesh = name_of(lib, *m).value_or("");
									}
								}
							}
						}
					}
				}
				if (package.empty()) {
					if (const std::optional<core::EbxInstance> a = lib.follow(swd->data(), "AnimationData")) {
						package = last_component(name_of(lib, *a).value_or(""));
					}
				}
			}
		}
		if (item.zoom_render_fov <= 0.0f) {
			item.zoom_render_fov = item.zoom_render_fov_data;
		}
		if (mesh.empty() && !p_spec.clips_only) {
			return core::make_error(spec.unlock + ": no 1p mesh (WeaponStates[0].Mesh1p)");
		}
		item.mesh = mesh;
		item.ant_package = package;
		item.ads = read_ads(lib, def->aiming_controller, set->notes);

		// Role clips from the item's package (by the role's names, or the spec's override).
		const core::ant::AntPackage *pkg = nullptr;
		if (!package.empty()) {
			core::Result<std::unique_ptr<core::ant::AntPackage>> p = anim::load_ant_package(*p_sources.ant, package);
			if (!p) {
				set->notes.push_back(spec.unlock + ": Ant package " + package + ": " + p.error().message + " (every role a stand-in)");
			} else {
				pkg = p->get();
				item.packages.push_back(std::move(*p));
			}
		}
		for (const RoleRule &rule : kRoleRules) {
			ViewmodelClip &clip = item.clips[size_t(rule.role)];
			if (pkg == nullptr) {
				break;
			}
			std::vector<std::string> names;
			if (!spec.clips[size_t(rule.role)].empty()) {
				names.push_back(spec.clips[size_t(rule.role)]);
			} else {
				for (const char *n : rule.names) {
					if (n != nullptr) {
						names.push_back(n);
					}
				}
			}
			for (const std::string &n : names) {
				const core::ant::GdStruct c = core::ant::AntLibrary::find_clip(*pkg, n);
				if (!c.valid()) {
					continue;
				}
				bool unsupported = false;
				const core::Status st = load_role_clip(ant, *pkg, c, set->rig, clip, unsupported);
				if (!st.ok()) {
					const bool zoom = rule.role == Role::ZoomIn || rule.role == Role::ZoomOut || rule.role == Role::ZoomIdle;
					clip = ViewmodelClip{};
					clip.name = n;
					clip.codec = codec_of(c);
					clip.source = unsupported && zoom ? Source::VbrStandIn : Source::Missing;
					set->notes.push_back(spec.unlock + " " + viewmodel_clip_role_name(rule.role) + ": " + st.error().message);
				}
				break;
			}
		}
		// Knife and M67: the static package's idle under the item's controller.
		if (item.clips[size_t(Role::Idle)].source == Source::Missing) {
			if (const char *controller = static_idle_controller(item.kind); controller && ant.static_package()) {
				const core::ant::AntPackage &sp = *ant.static_package();
				for (const uint32_t index : core::ant::AntLibrary::clips(sp)) {
					const core::ant::GdStruct c = sp.object(index);
					if (c.name() != "IdlePose Anim") {
						continue;
					}
					core::ant::ClipBinding probe;
					if (!ant.bind(sp, c, probe).ok() || probe.controller_name != controller) {
						continue;
					}
					bool unsupported = false;
					ViewmodelClip &idle = item.clips[size_t(Role::Idle)];
					const core::Status st = load_role_clip(ant, sp, c, set->rig, idle, unsupported);
					if (!st.ok()) {
						idle = ViewmodelClip{};
						set->notes.push_back(spec.unlock + " Idle (s_basicassets " + controller + "): " + st.error().message);
					}
					break;
				}
			}
		}
		// A zoom clip drives the transition only when its ends land on the poses it joins.
		const ViewmodelClip &idle = item.clips[size_t(Role::Idle)];
		const ViewmodelClip &zoom = item.clips[size_t(Role::ZoomIdle)];
		if (idle.source == Source::Clip && zoom.source == Source::Clip) {
			std::vector<anim::Xform> idle_m, zoom_m, a_m, b_m;
			clip_model(set->rig, rest, idle, 0.0f, idle_m);
			anim::LocalPose zoom_base = rest;
			clip_model(set->rig, rest, zoom, 0.0f, zoom_m);
			for (const Role r : { Role::ZoomIn, Role::ZoomOut }) {
				ViewmodelClip &z = item.clips[size_t(r)];
				if (z.source != Source::Clip) {
					continue;
				}
				// Sampled over the pose it starts from (undriven bones), as the animator does.
				anim::LocalPose from = rest;
				{
					core::ant::ClipPose s;
					const ViewmodelClip &start_pose = r == Role::ZoomIn ? idle : zoom;
					if (core::ant::sample_clip(start_pose.clip.clip, 0.0f, s)) {
						anim::apply_clip_pose(start_pose.tracks, s, from);
					}
				}
				clip_model(set->rig, from, z, 0.0f, a_m);
				clip_model(set->rig, from, z, float(z.tracks.end_frame), b_m);
				const double d0 = driven_diff(a_m, r == Role::ZoomIn ? idle_m : zoom_m, z.tracks);
				const double d1 = driven_diff(b_m, r == Role::ZoomIn ? zoom_m : idle_m, z.tracks);
				z.endpoint_deg = float(std::max(d0, d1));
				z.blend_instead = z.endpoint_deg > kZoomPathToleranceDeg;
				char line[256];
				std::snprintf(line, sizeof(line), "%s %s %s: first frame %.2f deg from %s, last %.2f deg from %s -> %s", spec.unlock.c_str(),
						viewmodel_clip_role_name(r), z.name.c_str(), d0, r == Role::ZoomIn ? "IdlePose" : "ZoomPose", d1, r == Role::ZoomIn ? "ZoomPose" : "IdlePose",
						z.blend_instead ? "IdlePose->ZoomPose blend instead" : "clip drives the transition");
				set->notes.push_back(line);
			}
			(void)zoom_base;
		}
		// Stand-ins for what the data does not have.
		auto stand_in = [&](Role p_role, Source p_source, const char *p_what, float p_seconds) {
			ViewmodelClip &c = item.clips[size_t(p_role)];
			if (c.source != Source::Missing) {
				return;
			}
			c.source = p_source;
			c.name = p_what;
			c.seconds = p_seconds;
		};
		const ViewmodelAdsData &ads = item.ads;
		const float zoom_seconds_in = ads.zoom_in >= 0.0f ? ads.zoom_in : ads.zoom_transition;
		const float zoom_seconds_out = ads.zoom_out >= 0.0f ? ads.zoom_out : ads.zoom_transition;
		stand_in(Role::Deploy, Source::ProceduralStandIn, "procedural raise from below over DeployTime", std::max(0.05f, item.deploy_time));
		if (item.kind == ViewmodelItemKind::Knife) {
			stand_in(Role::Knife, Source::ProceduralStandIn, "procedural stab", kKnifeStandInSeconds);
		} else if (item.kind == ViewmodelItemKind::Grenade) {
			stand_in(Role::Throw, Source::ProceduralStandIn, "procedural throw swing", kThrowStandInSeconds);
			if (item.reload_time > 0.0f) {
				stand_in(Role::Reload, Source::ProceduralStandIn, "procedural lower and raise over ReloadTime (next grenade)", item.reload_time);
			}
		} else {
			if (item.clips[size_t(Role::Fire)].source == Source::Missing && item.clips[size_t(Role::FireSingle)].source == Source::Missing) {
				stand_in(Role::Fire, Source::ProceduralStandIn, "procedural kick (kick springs)", 0.0f);
			}
			stand_in(Role::Reload, Source::ProceduralStandIn, "procedural lower and raise over ReloadTime", std::max(0.05f, item.reload_time_bullets_left));
			if (item.clips[size_t(Role::ReloadEmpty)].source == Source::Missing && item.clips[size_t(Role::Reload)].source == Source::Clip) {
				// The tactical clip plays for an empty reload too (counted).
				item.clips[size_t(Role::ReloadEmpty)] = ViewmodelClip{};
				ViewmodelClip &e = item.clips[size_t(Role::ReloadEmpty)];
				e.source = Source::ProceduralStandIn;
				e.name = "the Reload clip (no empty-magazine clip)";
				e.seconds = 0.0f;
			}
			if (ads.zoom_levels >= 2) {
				stand_in(Role::ZoomIn, Source::VbrStandIn, "IdlePose -> ZoomPose blend", zoom_seconds_in);
				stand_in(Role::ZoomOut, Source::VbrStandIn, "ZoomPose -> IdlePose blend", zoom_seconds_out);
			}
			for (const Role r : { Role::ZoomIn, Role::ZoomOut }) {
				ViewmodelClip &c = item.clips[size_t(r)];
				if (c.source == Source::VbrStandIn && c.seconds <= 0.0f) {
					c.seconds = r == Role::ZoomIn ? zoom_seconds_in : zoom_seconds_out;
				}
			}
		}
		for (const ViewmodelClip &c : item.clips) {
			if (c.source == Source::VbrStandIn || c.source == Source::ProceduralStandIn || c.blend_instead) {
				++item.stand_ins;
			}
		}
		char line[320];
		std::snprintf(line, sizeof(line), "%s: %s, mesh %s, package %s, RenderFov %.1f, ZoomRenderFov %.1f, zoom %.0f -> %.0f, DeployTime %.2f, ReloadTime %.2f / %.2f, %u stand-ins",
				spec.unlock.c_str(), viewmodel_item_kind_name(item.kind), mesh.empty() ? "-" : mesh.c_str(), package.empty() ? "-" : package.c_str(),
				double(item.render_fov), double(item.zoom_render_fov), double(ads.hip_fov), double(ads.zoom_fov), double(item.deploy_time),
				double(item.reload_time_bullets_left), double(item.reload_time), item.stand_ins);
		set->notes.push_back(line);
		set->items.push_back(std::move(item));
	}
	(void)item_no;

	if (p_spec.clips_only) {
		set->seconds_total = since(start);
		progress("done", LoadProgress::kSteps);
		return set;
	}

	// Materials and meshes: the arms once, each item's weapon with its own binds (arms first, so every item's binds
	// begin with the same arms binds and the arms mesh's lanes are valid for every item's skin).
	progress("finding materials", 5);
	const std::vector<std::string> databases = variation_database_names(lib);
	auto materials = [&](ViewmodelPart &r_part) -> core::Status {
		core::Result<MeshMaterials> m = mesh_materials(lib, r_part.mesh, r_part.variation_hash, databases);
		if (!m) {
			return core::error_at(r_part.mesh, m.error());
		}
		r_part.materials = std::move(*m);
		return core::Status::success();
	};
	if (core::Status st = materials(set->arms); !st.ok()) {
		return st.error();
	}
	for (ViewmodelItem &item : set->items) {
		item.weapon.mesh = item.mesh;
		if (core::Status st = materials(item.weapon); !st.ok()) {
			return st.error();
		}
	}
	progress("decoding meshes", 6);
	const core::LevelMount &mount = lib.mount(0);
	anim::SkinBinds arms_binds;
	if (core::Status st = prepare_part(mount, set->arms, set->arms_skeleton, set->rig, arms_binds, p_cache, set->notes); !st.ok()) {
		return st.error();
	}
	for (ViewmodelItem &item : set->items) {
		item.binds = arms_binds;
		if (core::Status st = prepare_part(mount, item.weapon, set->weapon_skeleton, set->rig, item.binds, p_cache, set->notes); !st.ok()) {
			return core::error_at(item.unlock, st.error());
		}
	}

	// Textures: Diffuse and Normal of every material a drawn surface uses, decoded on all cores.
	progress("decoding textures", 7);
	std::vector<std::pair<std::string, TextureRole>> wanted;
	std::set<std::string> seen;
	std::vector<const ViewmodelPart *> parts = { &set->arms };
	for (const ViewmodelItem &item : set->items) {
		parts.push_back(&item.weapon);
	}
	for (const ViewmodelPart *part : parts) {
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
			set->notes.push_back("texture skipped: " + errors[i]); // drawn untextured rather than failing the load
			continue;
		}
		const PreparedTexture &t = textures[i];
		char line[320];
		std::snprintf(line, sizeof(line), "texture %s: %ux%u %s%s -> %s", t.name.c_str(), t.width, t.height, texture_format_name(t.source_format),
				t.srgb ? " sRGB" : "", gpu_format_name(t.format));
		set->notes.push_back(line);
		set->textures.push_back(std::move(textures[i]));
	}
	set->seconds_total = since(start);
	progress("uploading", LoadProgress::kSteps - 1);
	return set;
}

const char *viewmodel_clip_source_name(ViewmodelClipSource p_source) {
	switch (p_source) {
		case Source::Missing:
			return "missing";
		case Source::Clip:
			return "clip";
		case Source::VbrStandIn:
			return "vbr-stand-in";
		case Source::ProceduralStandIn:
			return "procedural-stand-in";
	}
	return "?";
}

ViewmodelCoverage viewmodel_coverage(const ViewmodelItem &p_item) {
	ViewmodelCoverage c;
	auto pick = [&](ViewmodelCoverageEntry &r_e, const char *p_what, std::initializer_list<Role> p_roles) {
		r_e.what = p_what;
		r_e.role = *p_roles.begin();
		for (const Role r : p_roles) {
			const ViewmodelClip &clip = p_item.clips[size_t(r)];
			if (clip.source != Source::Missing) {
				r_e.role = r;
				r_e.source = clip.source;
				r_e.name = clip.name;
				r_e.seconds = clip.seconds;
				return;
			}
		}
	};
	pick(c.entries[0], "deploy", { Role::Deploy });
	switch (p_item.kind) {
		case ViewmodelItemKind::Knife:
			pick(c.entries[1], "fire", { Role::Knife, Role::Fire });
			pick(c.entries[2], "reload", { Role::Reload });
			if (c.entries[2].source == Source::Missing) {
				c.entries[2].applicable = false;
				c.entries[2].name = "no magazine";
			}
			break;
		case ViewmodelItemKind::Grenade:
			pick(c.entries[1], "fire", { Role::Throw, Role::Fire });
			pick(c.entries[2], "reload", { Role::Reload });
			break;
		case ViewmodelItemKind::Gun:
			pick(c.entries[1], "fire", { Role::FireSingle, Role::Fire });
			pick(c.entries[2], "reload", { Role::Reload, Role::ReloadEmpty });
			break;
	}
	c.covered = true;
	for (const ViewmodelCoverageEntry &e : c.entries) {
		if (!e.applicable) {
			++c.not_applicable;
		} else if (e.source == Source::Clip) {
			++c.clips;
		} else if (e.source == Source::Missing) {
			c.covered = false;
		} else {
			++c.stand_ins;
		}
	}
	return c;
}

// ---- the default kits ---------------------------------------------------------------------------------------

namespace {

constexpr const char *kM9 = "weapons/m9/u_m9";
constexpr const char *kMp443 = "weapons/mp443/u_mp443";
constexpr const char *kM67 = "weapons/m67/u_m67";
constexpr const char *kKnife = "weapons/knife/u_knife";

const ViewmodelKit kKits[] = {
	{ "US", "gameplay/kits/usassault", { "weapons/m16a4/u_m16a4", kM9, kM67, kKnife } },
	{ "US", "gameplay/kits/usengineer", { "weapons/m4a1/u_m4a1", kM9, kM67, kKnife } },
	{ "US", "gameplay/kits/ussupport", { "weapons/m27iar/u_m27iar", kM9, kM67, kKnife } },
	{ "US", "gameplay/kits/usrecon", { "weapons/mk11/u_mk11", kM9, kM67, kKnife } },
	{ "RU", "gameplay/kits/ruassault", { "weapons/ak74m/u_ak74m", kMp443, kM67, kKnife } },
	{ "RU", "gameplay/kits/ruengineer", { "weapons/aks74u/u_aks74u", kMp443, kM67, kKnife } },
	{ "RU", "gameplay/kits/rusupport", { "weapons/rpk/u_rpk-74m", kMp443, kM67, kKnife } },
	{ "RU", "gameplay/kits/rurecon", { "weapons/svd/u_svd", kMp443, kM67, kKnife } },
};

} // namespace

std::span<const ViewmodelKit> default_viewmodel_kits() {
	return kKits;
}

ViewmodelSetSpec team_viewmodel_set(int p_team) {
	ViewmodelSetSpec spec;
	const char *team = p_team == 2 ? "RU" : "US";
	if (p_team == 2) {
		// RU arms from the RU appearance [inferred]; the fallback is the RU arms mesh [verified: bf3dump on MP_Subway].
		spec.appearance = "persistence/unlocks/soldiers/visual/mp/ru/upperbody/mp_ru_assault_upperbody01";
		spec.arms_mesh = "characters/arms/arms1p_ru/arms1p_rus_mesh";
		spec.arms_variation = "characters/arms/arms1p_ru/arms1p_rus_default";
	}
	std::set<std::string> seen;
	for (const ViewmodelKit &k : kKits) {
		if (std::string_view(k.team) != team) {
			continue;
		}
		for (const char *u : k.unlocks) {
			if (seen.insert(u).second) {
				ViewmodelItemSpec item;
				item.unlock = u;
				spec.items.push_back(item);
			}
		}
	}
	return spec;
}

} // namespace bf3::render
