#include "bf3/render/viewmodel_set.h"

#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/kit_def.h"
#include "bf3/core/gameplay/mode_def.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/text.h"
#include "bf3/render/look_materials.h"
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

using bf3::core::seconds_since;

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
					break;
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
	std::array<const char *, 3> names;
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

void clip_model(const anim::Skeleton &p_rig, const anim::LocalPose &p_base, const ViewmodelClip &p_clip, float p_frame, std::vector<anim::Xform> &r_model) {
	anim::LocalPose pose = p_base;
	core::ant::ClipPose sample;
	if (core::ant::sample_clip(p_clip.clip.clip, p_frame, sample)) {
		anim::apply_clip_pose(p_clip.tracks, sample, pose);
	}
	anim::model_pose(p_rig, pose, r_model);
}

double driven_diff(const std::vector<anim::Xform> &p_a, const std::vector<anim::Xform> &p_b, const anim::ClipTracks &p_tracks) {
	std::vector<bool> mask(p_a.size(), false);
	for (const int32_t b : p_tracks.rotation_bone) {
		if (b >= 0 && size_t(b) < mask.size()) {
			mask[size_t(b)] = true;
		}
	}
	return anim::pose_diff(p_a, p_b, mask).max_deg;
}

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

constexpr size_t kMaxAttachments = 16;

void read_default_attachments(const core::EbxLibrary &p_lib, const core::EbxInstance &p_swd, const anim::Skeleton &p_rig, const std::string &p_unlock,
		std::vector<ViewmodelAttachment> &r_out, std::vector<std::string> &r_notes) {
	auto mat = [](const std::optional<core::EbxValue> &p_v) {
		const std::optional<std::array<core::Vec3, 4>> rows = p_v ? p_v->as_transform() : std::nullopt;
		return rows ? anim::mat_from_linear_transform(*rows) : anim::Mat34{};
	};
	for (const core::EbxInstance &socket : p_lib.follow_all(p_swd.data(), "Sockets")) {
		if (!socket.is_a("SocketData")) {
			continue;
		}
		const std::optional<core::EbxInstance> unlock = p_lib.follow(socket.data(), "UnlockAsset");
		const std::string unlock_name = unlock ? name_of(p_lib, *unlock).value_or("") : std::string();
		if (unlock_name.rfind(kDefaultSocketUnlockPrefix, 0) != 0) {
			continue;
		}
		const std::optional<core::EbxValue> bone_v = socket.get("BoneName");
		const std::string bone(bone_v && bone_v->as_str() ? *bone_v->as_str() : std::string_view());
		const int32_t rig_bone = bone.empty() ? -1 : p_rig.find(bone);
		if (rig_bone < 0) {
			r_notes.push_back(p_unlock + ": socket " + unlock_name + " on bone `" + bone + "` not on the rig (skipped)");
			continue;
		}
		const anim::Mat34 socket_m = anim::mat_mul(mat(socket.get("BoneRigidTransform")), mat(socket.get("Transform")));
		for (const core::EbxInstance &object : p_lib.follow_all(socket.data(), "AvailableObjects")) {
			if (!object.is_a("WeaponRegularSocketObjectData")) {
				r_notes.push_back(p_unlock + ": socket " + unlock_name + ": " + std::string(object.type_name()) + " skipped (rigid objects only)");
				continue;
			}
			const std::optional<core::EbxInstance> hip = p_lib.follow(object.data(), "Asset1p");
			const std::optional<core::EbxInstance> zoom = p_lib.follow(object.data(), "Asset1pzoom");
			const std::string hip_name = hip ? name_of(p_lib, *hip).value_or("") : std::string();
			const std::string zoom_name = zoom ? name_of(p_lib, *zoom).value_or("") : std::string();
			const anim::Mat34 m = anim::mat_mul(socket_m, mat(object.get("Transform")));
			for (int which = 0; which < 2; ++which) {
				const std::string &mesh = which == 0 ? hip_name : zoom_name;
				if (mesh.empty() || (which == 1 && mesh == hip_name)) {
					continue;
				}
				if (r_out.size() >= kMaxAttachments) {
					r_notes.push_back(p_unlock + ": more than " + std::to_string(kMaxAttachments) + " accessories (rest skipped)");
					return;
				}
				ViewmodelAttachment a;
				a.socket_unlock = unlock_name;
				a.bone = bone;
				a.rig_bone = rig_bone;
				a.transform = m;
				a.hip = mesh == hip_name;
				a.zoom = mesh == zoom_name;
				a.part.mesh = mesh;
				r_out.push_back(std::move(a));
			}
		}
	}
}

core::Status prepare_rigid_part(const core::LevelMount &p_mount, ViewmodelPart &r_part, core::Cache *p_cache, std::vector<std::string> &r_notes) {
	const Clock::time_point tm = Clock::now();
	core::Result<MeshHead> head = read_mesh_head(p_mount, r_part.mesh);
	if (!head) {
		return head.error();
	}
	MeshRequest request;
	request.lods = { 0 };
	request.filter = core::res::SubsetFilter::Drawn;
	core::Result<CachedMesh> mesh = load_mesh_cached(p_cache, p_mount, *head, request);
	if (!mesh) {
		return mesh.error();
	}
	if (mesh->mesh.meshes.size() != 1) {
		return core::make_error(r_part.mesh + ": no LOD 0 prepared");
	}
	r_part.prepared = std::move(mesh->mesh.meshes[0]);
	r_part.cache = mesh->item;
	r_part.seconds_mesh = seconds_since(tm);
	char line[320];
	std::snprintf(line, sizeof(line), "mesh %s (rigid accessory): %s in %.2f ms", r_part.mesh.c_str(),
			p_cache != nullptr && p_cache->is_open() ? core::cache_outcome_name(r_part.cache.outcome) : "no cache", r_part.seconds_mesh * 1000.0);
	r_notes.push_back(line);
	return core::Status::success();
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
	r_part.seconds_mesh = seconds_since(tm);
	char line[320];
	std::snprintf(line, sizeof(line), "mesh %s: %s (%.1f KiB blob) in %.2f ms", r_part.mesh.c_str(),
			p_cache != nullptr && p_cache->is_open() ? core::cache_outcome_name(r_part.cache.outcome) : "no cache", double(r_part.cache.payload_bytes) / 1024.0,
			r_part.seconds_mesh * 1000.0);
	r_notes.push_back(line);
	return core::Status::success();
}

}

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
	s->seconds_ant = seconds_since(t0);
	progress("mounting the level superbundle", 3);
	t0 = Clock::now();
	core::Result<std::unique_ptr<core::EbxLibrary>> ebx = core::EbxLibrary::open(s->install, { p_superbundle });
	if (!ebx) {
		return core::error_at(p_superbundle, ebx.error());
	}
	s->ebx = std::move(*ebx);
	s->seconds_ebx = seconds_since(t0);
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

		std::string mesh = spec.mesh;
		std::string package = spec.ant_package;
		std::optional<int64_t> pt1_ant_ref;
		if (const core::EbxPartition *bp = lib.partition_by_name(def->blueprint)) {
			if (const std::optional<core::EbxInstance> swd = lib.follow(bp->primary().data(), "Object")) {
				uint32_t clamped = 0;
				if (const std::optional<core::EbxValue> id = swd->get_path("WeaponStates[0].Weapon.AssetId")) {
					pt1_ant_ref = id->as_i64();
				}
				read_default_attachments(lib, *swd, set->rig, spec.unlock, item.attachments, set->notes);
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
		if (pt1_ant_ref && *pt1_ant_ref >= 0 && *pt1_ant_ref <= int64_t(UINT32_MAX)) {
			load_viewmodel_locomotion(ant, set->rig, uint32_t(*pt1_ant_ref), item, set->notes);
		} else {
			set->notes.push_back(spec.unlock + " 1p locomotion: no WeaponStates[0].Weapon.AssetId (the procedural bob)");
		}

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
		set->seconds_total = seconds_since(start);
		progress("done", LoadProgress::kSteps);
		return set;
	}

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
		for (ViewmodelAttachment &a : item.attachments) {
			if (core::Status st = materials(a.part); !st.ok()) {
				return core::error_at(item.unlock, st.error());
			}
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
		if (item.kind == ViewmodelItemKind::Knife && !p_spec.knife_dog_tags) {

			std::vector<PreparedSurface> &surfaces = item.weapon.prepared.surfaces;
			for (size_t i = surfaces.size(); i-- > 0;) {
				const PreparedSurface &sf = surfaces[i];
				if (sf.material_index >= item.weapon.materials.slots.size()) {
					continue;
				}
				const std::string diffuse =
						core::to_lower_ascii(item.weapon.materials.slots[sf.material_index].texture("Diffuse"));
				if (diffuse.find(kDogTagTexture) == std::string::npos) {
					continue;
				}
				set->notes.push_back(item.unlock + ": dog-tag surface " + std::to_string(sf.subset) + " (" + sf.material_name + ", " + diffuse +
						") hidden (stand-in: no dog-tag customization or chain physics in M2a)");
				item.weapon.prepared.vertices -= std::min(item.weapon.prepared.vertices, sf.vertex_count());
				item.weapon.prepared.triangles -= std::min(item.weapon.prepared.triangles, sf.indices.size() / 3);
				surfaces.erase(surfaces.begin() + std::ptrdiff_t(i));
				++item.hidden_surfaces;
			}
		}
		for (ViewmodelAttachment &a : item.attachments) {
			if (core::Status st = prepare_rigid_part(mount, a.part, p_cache, set->notes); !st.ok()) {
				return core::error_at(item.unlock, st.error());
			}
		}
	}

	{
		std::unique_ptr<LookResolver> look = LookResolver::open(mount);
		auto looks_of = [&](const ViewmodelPart &p_part) {
			std::vector<ViewmodelSlotLook> out(p_part.materials.slots.size());
			for (size_t i = 0; look && i < out.size(); ++i) {
				const LookMaterial lm = look->resolve(p_part.materials.slots[i], p_part.mesh);
				out[i].alpha_test = lm.blend == LookBlend::AlphaTest;
				out[i].double_sided = lm.double_sided;
				out[i].in_database = lm.shader_in_database;
				if (out[i].alpha_test) {
					set->notes.push_back("alpha test: " + p_part.mesh + " slot " + std::to_string(i) + " (" + p_part.materials.slots[i].shader + ")");
				}
			}
			return out;
		};
		if (!look) {
			set->notes.push_back("no level shader database: every viewmodel slot is drawn opaque");
		}
		set->arms_looks = looks_of(set->arms);
		for (ViewmodelItem &item : set->items) {
			item.weapon_looks = looks_of(item.weapon);
			for (ViewmodelAttachment &a : item.attachments) {
				a.looks = looks_of(a.part);
			}
		}
	}

	progress("decoding textures", 7);
	std::vector<std::pair<std::string, TextureRole>> wanted;
	std::set<std::string> seen;
	std::vector<const ViewmodelPart *> parts = { &set->arms };
	for (const ViewmodelItem &item : set->items) {
		parts.push_back(&item.weapon);
		for (const ViewmodelAttachment &a : item.attachments) {
			parts.push_back(&a.part);
		}
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
			set->notes.push_back("texture skipped: " + errors[i]);
			continue;
		}
		const PreparedTexture &t = textures[i];
		char line[320];
		std::snprintf(line, sizeof(line), "texture %s: %ux%u %s%s -> %s", t.name.c_str(), t.width, t.height, texture_format_name(t.source_format),
				t.srgb ? " sRGB" : "", gpu_format_name(t.format));
		set->notes.push_back(line);
		set->textures.push_back(std::move(textures[i]));
	}
	set->seconds_total = seconds_since(start);
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

bool ViewmodelSurfaceSkin::stretched() const {
	return bad_weights > 0 || (posed_edge > kSkinStretchFloor && posed_edge > kSkinStretchFactor * bind_edge);
}

std::vector<ViewmodelSurfaceSkin> viewmodel_surface_skin(const PreparedMesh &p_mesh, std::span<const anim::SkinBind> p_binds,
		std::span<const anim::Xform> p_model) {
	std::vector<anim::Mat34> skin(p_binds.size());
	std::vector<bool> valid(p_binds.size(), false);
	for (size_t i = 0; i < p_binds.size(); ++i) {
		const int32_t b = p_binds[i].bone;
		if (b >= 0 && size_t(b) < p_model.size()) {
			skin[i] = anim::mat_mul(anim::mat_from_xform(p_model[size_t(b)]), p_binds[i].inverse_bind);
			valid[i] = true;
		}
	}
	std::vector<ViewmodelSurfaceSkin> out;
	std::vector<anim::Vec3> posed;
	for (const PreparedSurface &sf : p_mesh.surfaces) {
		ViewmodelSurfaceSkin r;
		r.subset = sf.subset;
		r.material = sf.material_name;
		r.vertices = sf.vertex_count();
		r.triangles = sf.indices.size() / 3;
		const bool skinned = sf.bones.size() == r.vertices * 4 && sf.weights.size() == r.vertices * 4;
		posed.assign(r.vertices, anim::Vec3{});
		std::vector<int32_t> heaviest(r.vertices, -1);
		for (size_t v = 0; v < r.vertices; ++v) {
			const anim::Vec3 p{ sf.positions[v * 3], sf.positions[v * 3 + 1], sf.positions[v * 3 + 2] };
			if (!skinned) {
				posed[v] = p;
				continue;
			}
			float sum = 0, best = -1;
			anim::Vec3 acc{};
			bool bad = false;
			for (size_t l = 0; l < 4; ++l) {
				const float w = sf.weights[v * 4 + l];
				if (w <= 0) {
					continue;
				}
				const int32_t bi = sf.bones[v * 4 + l];
				if (bi < 0 || size_t(bi) >= skin.size() || !valid[size_t(bi)]) {
					bad = true;
					continue;
				}
				acc = anim::vec_add(acc, anim::vec_scale(anim::mat_point(skin[size_t(bi)], p), w));
				if (std::find(r.binds.begin(), r.binds.end(), bi) == r.binds.end()) {
					r.binds.push_back(bi);
				}
				sum += w;
				if (w > best) {
					best = w;
					heaviest[v] = bi;
				}
			}
			if (bad || std::fabs(sum - 1.0f) > 0.01f) {
				++r.bad_weights;
			}
			posed[v] = acc;
		}
		std::sort(r.binds.begin(), r.binds.end());
		r.posed_min = { 1e30f, 1e30f, 1e30f };
		r.posed_max = { -1e30f, -1e30f, -1e30f };
		for (const anim::Vec3 &p : posed) {
			const float c[3] = { p.x, p.y, p.z };
			for (size_t k = 0; k < 3; ++k) {
				r.posed_min[k] = std::min(r.posed_min[k], c[k]);
				r.posed_max[k] = std::max(r.posed_max[k], c[k]);
			}
		}
		size_t worst_v = 0;
		for (size_t t = 0; t + 2 < sf.indices.size(); t += 3) {
			for (size_t e = 0; e < 3; ++e) {
				const int32_t a = sf.indices[t + e], b = sf.indices[t + (e + 1) % 3];
				if (a < 0 || b < 0 || size_t(a) >= r.vertices || size_t(b) >= r.vertices) {
					continue;
				}
				const anim::Vec3 pa{ sf.positions[size_t(a) * 3], sf.positions[size_t(a) * 3 + 1], sf.positions[size_t(a) * 3 + 2] };
				const anim::Vec3 pb{ sf.positions[size_t(b) * 3], sf.positions[size_t(b) * 3 + 1], sf.positions[size_t(b) * 3 + 2] };
				r.bind_edge = std::max(r.bind_edge, float(anim::vec_dist(pa, pb)));
				const float d = float(anim::vec_dist(posed[size_t(a)], posed[size_t(b)]));
				if (d > r.posed_edge) {
					r.posed_edge = d;
					worst_v = size_t(a);
				}
			}
		}
		if (worst_v < heaviest.size() && heaviest[worst_v] >= 0) {
			r.worst_bone = p_binds[size_t(heaviest[worst_v])].name;
		}
		out.push_back(std::move(r));
	}
	return out;
}

namespace {

constexpr std::array<core::gameplay::WeaponSlot, 4> kKitSlots{ core::gameplay::WeaponSlot::Primary, core::gameplay::WeaponSlot::Sidearm,
	core::gameplay::WeaponSlot::Grenade, core::gameplay::WeaponSlot::Knife };

constexpr const char *kM9 = "weapons/m9/u_m9";
constexpr const char *kMp443 = "weapons/mp443/u_mp443";
constexpr const char *kM67 = "weapons/m67/u_m67";
constexpr const char *kKnife = "weapons/knife/u_knife";

const ViewmodelKit kDocumentedKits[] = {
	{ "US", "gameplay/kits/usassault", { "weapons/m16a4/u_m16a4", kM9, kM67, kKnife } },
	{ "US", "gameplay/kits/usengineer", { "weapons/m4a1/u_m4a1", kM9, kM67, kKnife } },
	{ "US", "gameplay/kits/ussupport", { "weapons/m27iar/u_m27iar", kM9, kM67, kKnife } },
	{ "US", "gameplay/kits/usrecon", { "weapons/mk11/u_mk11", kM9, kM67, kKnife } },
	{ "RU", "gameplay/kits/ruassault", { "weapons/ak74m/u_ak74m", kMp443, kM67, kKnife } },
	{ "RU", "gameplay/kits/ruengineer", { "weapons/aks74u/u_aks74u", kMp443, kM67, kKnife } },
	{ "RU", "gameplay/kits/rusupport", { "weapons/rpk/u_rpk-74m", kMp443, kM67, kKnife } },
	{ "RU", "gameplay/kits/rurecon", { "weapons/svd/u_svd", kMp443, kM67, kKnife } },
};

std::string kit_line(const ViewmodelKit &p_kit) {
	std::string l = p_kit.team + " " + p_kit.kit + ":";
	for (const std::string &u : p_kit.unlocks) {
		l += " " + u;
	}
	return l;
}

}

core::Result<std::vector<ViewmodelKit>> read_viewmodel_kits(const core::EbxResolver &p_db, std::string_view p_level_id, std::string_view p_mode) {

	core::gameplay::WorkBudget budget;
	core::Result<core::gameplay::ModeDef> mode = core::gameplay::read_mode_def(p_db, p_level_id, p_mode, &budget);
	if (!mode) {
		return core::make_error("viewmodel kits: " + mode.error().message);
	}
	core::Result<core::gameplay::UnlockProgression> progression = core::gameplay::read_unlock_progression(p_db, &budget);
	if (!progression) {
		return core::make_error("viewmodel kits: " + progression.error().message);
	}
	if (!progression->known()) {
		return core::make_error("viewmodel kits: no unlock progression tables (the kits' defaults are unknown)");
	}
	std::vector<ViewmodelKit> kits;
	for (const int team : { 1, 2 }) {
		const core::gameplay::TeamDef *t = nullptr;
		for (const core::gameplay::TeamDef &d : mode->teams) {
			if (d.team == team) {
				t = &d;
				break;
			}
		}
		if (t == nullptr) {
			return core::make_error("viewmodel kits: " + std::string(p_mode) + " has no team " + std::to_string(team));
		}
		if (t->kits.size() != kViewmodelKitsPerTeam) {
			return core::make_error("viewmodel kits: team " + std::to_string(team) + " lists " + std::to_string(t->kits.size()) + " kits (expected " +
					std::to_string(kViewmodelKitsPerTeam) + ")");
		}
		for (const std::string &asset : t->kits) {
			core::Result<core::gameplay::KitDef> kit = core::gameplay::read_kit_def(p_db, asset, &*progression, &budget);
			if (!kit) {
				return core::make_error("viewmodel kits: " + asset + ": " + kit.error().message);
			}
			ViewmodelKit k;
			k.team = team == 2 ? "RU" : "US";
			k.kit = kit->asset;
			for (size_t i = 0; i < kKitSlots.size(); ++i) {
				const core::gameplay::KitUnlock *d = kit->default_unlock(kKitSlots[i]);
				if (d == nullptr || d->unlock.empty()) {
					return core::make_error("viewmodel kits: " + asset + " has no default " + core::gameplay::weapon_slot_name(kKitSlots[i]));
				}
				k.unlocks[i] = d->unlock;
			}
			kits.push_back(std::move(k));
		}
	}
	return kits;
}

std::string viewmodel_level_id(std::string_view p_superbundle) {
	const size_t slash = p_superbundle.find_last_of("/\\");
	return std::string(slash == std::string_view::npos ? p_superbundle : p_superbundle.substr(slash + 1));
}

std::span<const ViewmodelKit> documented_viewmodel_kits() {
	return kDocumentedKits;
}

std::string viewmodel_kits_difference(std::span<const ViewmodelKit> p_data, std::span<const ViewmodelKit> p_documented) {
	if (p_data.size() != p_documented.size()) {
		return "the data lists " + std::to_string(p_data.size()) + " loadouts, the documented table " + std::to_string(p_documented.size());
	}
	for (size_t i = 0; i < p_data.size(); ++i) {
		const ViewmodelKit &a = p_data[i];
		const ViewmodelKit &b = p_documented[i];
		if (a.team != b.team || a.kit != b.kit || a.unlocks != b.unlocks) {
			return "loadout " + std::to_string(i) + ": data " + kit_line(a) + " | documented " + kit_line(b);
		}
	}
	return {};
}

ViewmodelSetSpec team_viewmodel_set(std::span<const ViewmodelKit> p_kits, int p_team) {
	ViewmodelSetSpec spec;
	const char *team = p_team == 2 ? "RU" : "US";
	if (p_team == 2) {

		spec.appearance = "persistence/unlocks/soldiers/visual/mp/ru/upperbody/mp_ru_assault_upperbody01";
		spec.arms_mesh = "characters/arms/arms1p_ru/arms1p_rus_mesh";
		spec.arms_variation = "characters/arms/arms1p_ru/arms1p_rus_default";
	}
	std::set<std::string> seen;
	for (const ViewmodelKit &k : p_kits) {
		if (k.team != team) {
			continue;
		}
		for (const std::string &u : k.unlocks) {
			if (seen.insert(u).second) {
				ViewmodelItemSpec item;
				item.unlock = u;
				spec.items.push_back(item);
			}
		}
		if (!k.gadget.empty() && seen.insert(k.gadget).second) {
			ViewmodelItemSpec item;
			item.unlock = k.gadget;
			spec.items.push_back(item);
		}
	}
	return spec;
}

}

namespace bf3::render {

size_t add_assault_gadget(std::span<ViewmodelKit> r_kits, std::string_view p_unlock) {
	if (p_unlock.empty()) {
		return 0;
	}
	size_t n = 0;
	for (ViewmodelKit &k : r_kits) {
		const size_t slash = k.kit.find_last_of('/');
		const std::string_view name = std::string_view(k.kit).substr(slash == std::string::npos ? 0 : slash + 1);
		if (name.size() >= 7 && name.substr(name.size() - 7) == "assault") {
			k.gadget = std::string(p_unlock);
			++n;
		}
	}
	return n;
}

}

namespace bf3::render {

namespace {

std::string pt1_route_path(const anim::AntClipRoute &p_route) {
	std::string out;
	for (const std::string &h : p_route.path) {
		out += (out.empty() ? "" : " -> ") + h;
	}
	return out;
}

void pt1_load_delta(const core::ant::AntLibrary &p_library, const anim::Skeleton &p_rig, std::string_view p_chooser, const anim::ChooserIndices &p_indices,
		const std::string &p_where, ViewmodelClip &r_clip, float &r_start, float &r_end, std::string &r_path, std::vector<std::string> &r_notes) {
	core::Result<anim::AntClipRoute> route = anim::route_ant_controller(p_library, "IndexChooserControllerAsset", p_chooser, p_indices);
	if (!route) {
		r_notes.push_back(p_where + " " + std::string(p_chooser) + ": " + route.error().message + " (no delta)");
		return;
	}
	r_path = pt1_route_path(*route);
	const core::ant::AntPackage *sp = p_library.static_package();
	const core::ant::AntPackage *pkg = route->controller_package != nullptr ? route->controller_package : sp;
	core::ant::GdStruct c = pkg != nullptr ? pkg->find(route->anim_index) : core::ant::GdStruct{};
	if ((!c.valid() || !core::ant::is_clip(c)) && sp != nullptr && pkg != sp) {
		pkg = sp;
		c = sp->find(route->anim_index);
	}
	if (!c.valid() || !core::ant::is_clip(c)) {
		r_notes.push_back(p_where + " " + std::string(p_chooser) + ": " + route->controller + "'s Anim is not a clip (no delta)");
		return;
	}
	bool unsupported = false;
	ViewmodelClip clip;
	if (const core::Status st = load_role_clip(p_library, *pkg, c, p_rig, clip, unsupported); !st.ok()) {
		r_notes.push_back(p_where + " " + std::string(p_chooser) + ": " + st.error().message + " (no delta)");
		return;
	}
	if (!clip.clip.clip.additive || clip.tracks.end_frame == 0) {
		r_notes.push_back(p_where + " " + std::string(p_chooser) + ": " + clip.name + " is not an additive cycle (no delta)");
		return;
	}

	const float scale = std::isfinite(route->fps_scale) && route->fps_scale > 0.0f ? route->fps_scale : 0.5f;
	const float end = float(clip.tracks.end_frame);
	r_start = std::clamp(route->start_in_tick * scale, 0.0f, end);
	r_end = std::clamp(route->end_in_tick * scale, 0.0f, end);
	if (!(r_end > r_start)) {
		r_start = 0.0f;
		r_end = end;
	}
	r_clip = std::move(clip);
	char line[200];
	std::snprintf(line, sizeof(line), ": %s frames %.0f .. %.0f of %u (%s)", r_clip.name.c_str(), double(r_start), double(r_end), r_clip.tracks.end_frame,
			r_clip.controller.c_str());
	r_notes.push_back(p_where + " " + std::string(p_chooser) + line);
}

anim::VmDeltaClip pt1_delta_ref(const ViewmodelClip &p_clip, float p_start, float p_end) {
	anim::VmDeltaClip d;
	if (p_clip.source == Source::Clip) {
		d.clip.clip = &p_clip.clip.clip;
		d.clip.tracks = &p_clip.tracks;
		d.clip.seconds = p_clip.seconds;
		d.start_frame = p_start;
		d.end_frame = p_end;
	}
	return d;
}

}

void load_viewmodel_locomotion(const core::ant::AntLibrary &p_library, const anim::Skeleton &p_rig, uint32_t p_ant_ref, ViewmodelItem &r_item,
		std::vector<std::string> &r_notes) {
	const std::string where = r_item.unlock + " 1p locomotion";
	core::Result<uint32_t> type = anim::weapon_type_index(p_library, p_ant_ref);
	if (!type) {
		r_notes.push_back(where + ": " + type.error().message + " (the procedural bob)");
		return;
	}
	auto l = std::make_shared<ViewmodelLocomotionClips>();
	l->weapon_type = *type;
	anim::ChooserIndices idx;
	idx.weapon_type = *type;
	pt1_load_delta(p_library, p_rig, kVmRunDeltaChooser, idx, where, l->run, l->run_start_frame, l->run_end_frame, l->run_path, r_notes);
	pt1_load_delta(p_library, p_rig, kVmSprintDeltaChooser, idx, where, l->sprint, l->sprint_start_frame, l->sprint_end_frame, l->sprint_path, r_notes);
	if (l->run.source != Source::Clip && l->sprint.source != Source::Clip) {
		r_notes.push_back(where + ": weapon type " + std::to_string(*type) + ", no delta (the procedural bob)");
		return;
	}
	r_item.pt1_locomotion = std::move(l);
}

anim::VmLocomotion viewmodel_locomotion(const ViewmodelItem &p_item) {
	anim::VmLocomotion out;
	if (const ViewmodelLocomotionClips *l = p_item.pt1_locomotion.get()) {
		out.run = pt1_delta_ref(l->run, l->run_start_frame, l->run_end_frame);
		out.sprint = pt1_delta_ref(l->sprint, l->sprint_start_frame, l->sprint_end_frame);
	}
	return out;
}

}
