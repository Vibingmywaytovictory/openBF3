#include "bf3/render/soldier_assets.h"

#include "bf3/anim/crowd_eval.h"

#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/kit_appearance.h"
#include "bf3/core/gameplay/kit_def.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/render/mesh_cache.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>

namespace bf3::render {

using Clock = std::chrono::steady_clock;

namespace {

using bf3::core::seconds_since;

std::string name_of_instance(const core::EbxLibrary &p_library, const core::EbxInstance &p_instance) {
	const std::optional<std::string_view> n = p_library.name_of(p_instance.partition().guid());
	return n ? std::string(*n) : std::string();
}

uint32_t variation_hash_of(const core::EbxLibrary &p_library, const std::string &p_variation) {
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

core::Result<std::vector<SoldierPart>> appearance_parts(const core::EbxLibrary &p_library, const std::string &p_appearance, std::vector<std::string> &r_notes) {
	const core::EbxPartition *part = p_library.partition_by_name(p_appearance);
	if (part == nullptr) {
		return core::make_error("appearance " + p_appearance + " not found");
	}
	std::vector<SoldierPart> out;
	std::set<std::string> seen;
	auto add_mesh = [&](const core::EbxInstance &p_mesh, const std::optional<core::EbxInstance> &p_variation) {
		const std::string mesh = name_of_instance(p_library, p_mesh);
		if (mesh.empty() || mesh.find("arms1p") != std::string::npos || !seen.insert(mesh).second) {
			return;
		}
		SoldierPart sp;
		sp.mesh = mesh;
		if (p_variation) {
			sp.variation = name_of_instance(p_library, *p_variation);
		}
		out.push_back(std::move(sp));
	};
	size_t budget = 4096;
	for (const core::EbxInstance &unlock : p_library.follow_all(part->primary().data(), "LinkedTo")) {
		if (budget-- == 0) {
			return core::make_error("appearance " + p_appearance + ": too many linked parts");
		}
		for (const core::EbxInstance &linked : p_library.follow_all(unlock.data(), "LinkedTo")) {
			if (budget-- == 0) {
				return core::make_error("appearance " + p_appearance + ": too many linked parts");
			}
			if (linked.is_a("BlueprintAndVariationPair")) {
				const std::optional<core::EbxInstance> base = p_library.follow(linked.data(), "BaseAsset");
				if (base && base->is_a("MeshAsset")) {
					add_mesh(*base, p_library.follow(linked.data(), "Variation"));
				}
			} else if (linked.is_a("MeshAsset")) {
				add_mesh(linked, std::nullopt);
			}
		}
	}
	if (out.empty()) {
		return core::make_error("appearance " + p_appearance + " links no 3p mesh");
	}
	std::string line = "appearance " + p_appearance + ":";
	for (const SoldierPart &p : out) {
		line += " " + p.mesh + (p.variation.empty() ? "" : " (" + p.variation + ")");
	}
	r_notes.push_back(line);
	return out;
}

core::Result<anim::EbxSkeleton> load_skeleton(const core::EbxLibrary &p_library, const std::string &p_name) {
	const core::EbxPartition *part = p_library.partition_by_name(p_name);
	if (part == nullptr) {
		return core::make_error("no skeleton partition " + p_name);
	}
	return anim::read_ebx_skeleton(*part, p_name);
}

anim::Xform xform_of(const anim::Mat34 &p_m) {
	return anim::Xform{ anim::quat_from_mat(p_m), p_m.origin() };
}

core::Result<uint32_t> weapon_ant_ref(const core::EbxLibrary &p_library, const std::string &p_partition) {
	const core::EbxPartition *part = p_library.partition_by_name(p_partition);
	if (part == nullptr) {
		return core::make_error("weapon " + p_partition + " not found");
	}
	const std::vector<core::EbxInstance> data = part->instances_of("SoldierWeaponData");
	if (data.empty()) {
		return core::make_error("weapon " + p_partition + ": no SoldierWeaponData");
	}
	const std::optional<core::EbxValue> id = data.front().get_path("WeaponStates[0].Weapon.AssetId");
	if (!id) {
		return core::make_error("weapon " + p_partition + ": no WeaponStates[0].Weapon.AssetId");
	}
	const std::optional<int64_t> v = id->as_i64();
	if (!v || *v < 0 || *v > int64_t(UINT32_MAX)) {
		return core::make_error("weapon " + p_partition + ": WeaponStates[0].Weapon.AssetId is not a valid AntRef");
	}
	return uint32_t(*v);
}

void join_kit_parts(const core::EbxLibrary &p_library, const SoldierTeamSpec &p_spec, SoldierTeam &r_team, std::vector<std::string> &r_notes);

}

const PreparedTexture *SoldierAssets::texture(const std::string &p_name) const {
	for (const PreparedTexture &t : textures) {
		if (t.name == p_name) {
			return &t;
		}
	}
	return nullptr;
}

core::Result<std::unique_ptr<SoldierAssets>> load_soldier_assets(const core::InstallReader &p_install, const SoldierSpec &p_spec, LoadProgress *r_progress,
		core::Cache *p_cache) {
	const Clock::time_point start = Clock::now();
	auto progress = [&](const char *p_stage, int p_step) {
		if (r_progress != nullptr) {
			r_progress->set(p_stage, p_step);
		}
	};
	auto a = std::make_unique<SoldierAssets>();
	progress("opening the install", 1);
	core::Result<std::shared_ptr<core::Install>> install = core::Install::open(p_install);
	if (!install) {
		return core::error_at("install", install.error());
	}

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
	core::Result<anim::CrowdClipSet> clips = anim::load_crowd_clips(**source, *a->library, p_spec.clips);
	if (!clips) {
		return clips.error();
	}
	a->clips = std::move(*clips);
	a->seconds_ant = seconds_since(t0);

	progress("mounting the level superbundle", 3);
	t0 = Clock::now();
	core::Result<std::unique_ptr<core::EbxLibrary>> ebx = core::EbxLibrary::open(*install, { p_spec.superbundle });
	if (!ebx) {
		return core::error_at(p_spec.superbundle, ebx.error());
	}
	const core::EbxLibrary &lib = **ebx;
	core::Result<anim::EbxSkeleton> body = load_skeleton(lib, p_spec.body_skeleton);
	if (!body) {
		return body.error();
	}
	a->body_skeleton = std::move(*body);
	core::Result<anim::EbxSkeleton> weapon_skeleton = load_skeleton(lib, p_spec.weapon_skeleton);
	if (!weapon_skeleton) {
		return weapon_skeleton.error();
	}
	a->weapon_skeleton = std::move(*weapon_skeleton);
	const anim::Skeleton &rig = a->clips.rig;

	anim::BoneMask driven;
	for (const anim::CrowdClip c : { anim::CrowdClip::Idle, anim::CrowdClip::RunFwd, anim::CrowdClip::RunBwd, anim::CrowdClip::RunLeft,
				 anim::CrowdClip::RunRight, anim::CrowdClip::AimUp, anim::CrowdClip::AimDown }) {
		driven = anim::mask_union(driven, a->clips.get(c).mask);
	}
	anim::BoneMask weapon_joints;
	weapon_joints.weight.assign(rig.size(), 0.0f);
	std::vector<anim::Xform> weapon_local(rig.size(), anim::xform_identity());
	{
		const anim::EbxSkeleton &w = a->weapon_skeleton;
		const int32_t w_root = w.find("Wep_Root");
		uint32_t layout = 0, parent_mismatch = 0;
		std::string mismatched;
		for (size_t j = 0; j < rig.size(); ++j) {
			const int32_t wi = w.find(rig.bone(j).name);
			if (wi < 0 || wi == w_root || (j < driven.size() && driven.weight[j] > 0.0f)) {
				continue;
			}
			bool below = false;
			int32_t p = w.parents[size_t(wi)];
			for (size_t hops = 0; p >= 0 && hops < w.size(); ++hops) {
				if (p == w_root) {
					below = true;
					break;
				}
				p = w.parents[size_t(p)];
			}
			if (!below) {
				continue;
			}
			const int32_t wp = w.parents[size_t(wi)];
			const int32_t rp = rig.bone(j).parent;
			if (wp < 0 || rp < 0 || w.bone_names[size_t(wp)] != rig.bone(size_t(rp)).name) {
				++parent_mismatch;
				mismatched += " " + rig.bone(j).name + " (rig parent " + (rp < 0 ? std::string("none") : rig.bone(size_t(rp)).name) + ", weaponske01 parent " +
						(wp < 0 ? std::string("none") : w.bone_names[size_t(wp)]) + ")";
				continue;
			}
			weapon_joints.weight[j] = 1.0f;
			weapon_local[j] = xform_of(w.local_pose[size_t(wi)]);
			++layout;
		}
		char line[160];
		std::snprintf(line, sizeof(line), "weapon layout: %u undriven weaponske01 joints below Wep_Root take the weapon's LocalPose (%u skipped: parent differs:",
				layout, parent_mismatch);
		a->notes.push_back(line + mismatched + ")");
	}

	progress("finding parts and materials", 4);
	const std::vector<std::string> databases = variation_database_names(lib);
	for (size_t t = 0; t < a->teams.size(); ++t) {
		const SoldierTeamSpec &ts = p_spec.teams[t];
		SoldierTeam &team = a->teams[t];
		team.appearance = ts.appearance;
		core::Result<std::vector<SoldierPart>> parts = appearance_parts(lib, ts.appearance, a->notes);
		if (!parts) {
			return parts.error();
		}
		team.parts = std::move(*parts);
		join_kit_parts(lib, ts, team, a->notes);
		SoldierPart weapon;
		weapon.mesh = ts.weapon_mesh;
		weapon.weapon = true;
		team.parts.push_back(std::move(weapon));
		for (SoldierPart &part : team.parts) {
			part.variation_hash = variation_hash_of(lib, part.variation);
			if (!part.variation.empty() && part.variation_hash == 0) {
				a->notes.push_back("variation " + part.variation + " has no NameHash: default materials");
			}
			core::Result<MeshMaterials> m = mesh_materials(lib, part.mesh, part.variation_hash, databases);
			if (!m) {
				return m.error();
			}
			part.materials = std::move(*m);
		}
		team.weapon_joints = weapon_joints;
		team.weapon_local = weapon_local;

		core::Result<uint32_t> ref = weapon_ant_ref(lib, ts.weapon_unlock);
		if (!ref) {
			return ref.error();
		}
		core::Result<uint32_t> index = anim::weapon_choice_index(*a->library, *ref);
		if (!index) {
			return index.error();
		}
		team.reload_index = *index;
		core::Result<anim::CrowdClipData> reload = anim::load_chosen_clip(a->clips, *a->library, p_spec.clips.reload_chooser, *index);
		if (!reload) {
			return reload.error();
		}
		team.reload = std::move(*reload);
		a->clips.upper_body = anim::mask_union(a->clips.upper_body, team.reload.mask);
		char line[256];
		std::snprintf(line, sizeof(line), "%s: AntRef %u -> chooser index %u -> controller 0x%x, %u frames (%.2f s)", ts.weapon_unlock.c_str(), *ref, *index,
				team.reload.controller_index, team.reload.tracks.end_frame, double(team.reload.tracks.duration_seconds()));
		a->notes.push_back(line);
		if (!ts.weapon_def.empty()) {
			core::Result<core::gameplay::WeaponDef> w = core::gameplay::resolve_weapon(lib, ts.weapon_def);
			if (w) {
				team.reload_time = w->fire.reload_time;
			} else {
				a->notes.push_back("weapon " + ts.weapon_def + " did not resolve (reload time unknown, rate 1): " + w.error().message);
			}
		}
	}
	a->seconds_ebx = seconds_since(t0);

	{
		core::Result<std::unique_ptr<core::EbxLibrary>> settings = core::EbxLibrary::open(*install, { p_spec.settings_superbundle });
		const core::EbxPartition *part = settings ? (*settings)->partition_by_name(p_spec.settings_partition) : nullptr;
		core::Result<anim::TemporalLodTable> lod = part != nullptr ? anim::read_game_animation_settings(*part)
																	: core::Result<anim::TemporalLodTable>(core::make_error(
																			  "partition " + p_spec.settings_partition + " not found in " + p_spec.settings_superbundle));
		if (lod) {
			a->lod = *lod;
			char line[256];
			std::snprintf(line, sizeof(line), "temporal LOD: %.0f/%.0f/%.0f/%.0f/%.0f/%.0f m, %.2f..%.2f s, far %.0f m, client %s", a->lod.tiers[0].distance,
					a->lod.tiers[1].distance, a->lod.tiers[2].distance, a->lod.tiers[3].distance, a->lod.tiers[4].distance, a->lod.tiers[5].distance,
					a->lod.tiers[0].delta_time, a->lod.tiers[5].delta_time, a->lod.far_distance, a->lod.client_enable ? "on" : "off");
			a->notes.push_back(line);
		} else {
			a->notes.push_back("temporal LOD: GameAnimationSettings unreadable, retail defaults used: " + lod.error().message);
		}
	}

	progress("decoding meshes", 5);
	t0 = Clock::now();
	const core::LevelMount &mount = lib.mount(0);
	for (SoldierTeam &team : a->teams) {
		for (SoldierPart &part : team.parts) {
			const Clock::time_point tm = Clock::now();
			core::Result<MeshHead> head = read_mesh_head(mount, part.mesh);
			if (!head) {
				return head.error();
			}
			if (head->mesh.lods.size() <= p_spec.lod) {
				return core::make_error(part.mesh + ": no LOD " + std::to_string(p_spec.lod));
			}
			const anim::EbxSkeleton &skeleton = part.weapon ? a->weapon_skeleton : a->body_skeleton;
			core::Result<std::vector<std::vector<int32_t>>> lanes = team.binds.bind_lod(head->mesh.lods[p_spec.lod], skeleton, rig);
			if (!lanes) {
				return core::error_at(part.mesh, lanes.error());
			}
			if (team.binds.size() > anim::kMaxPaletteBinds) {
				return core::make_error(part.mesh + ": the team needs " + std::to_string(team.binds.size()) + " binds (cap " + std::to_string(anim::kMaxPaletteBinds) +
						")");
			}
			MeshRequest request;
			request.lods = { p_spec.lod };
			request.filter = core::res::SubsetFilter::Drawn;
			request.lane_binds = &*lanes;
			core::Result<CachedMesh> mesh = load_mesh_cached(p_cache, mount, *head, request);
			if (!mesh) {
				return mesh.error();
			}
			if (mesh->mesh.meshes.size() != 1) {
				return core::make_error(part.mesh + ": LOD " + std::to_string(p_spec.lod) + " not prepared");
			}
			part.prepared = std::move(mesh->mesh.meshes[0]);
			part.cache = mesh->item;
			part.seconds_mesh = seconds_since(tm);
			team.vertices += part.prepared.vertices;
			team.triangles += part.prepared.triangles;
			char line[320];
			std::snprintf(line, sizeof(line), "mesh %s: %zu vertices, %zu triangles, %zu surfaces, %s (%.1f KiB) in %.2f ms", part.mesh.c_str(), part.prepared.vertices,
					part.prepared.triangles, part.prepared.surfaces.size(),
					p_cache != nullptr && p_cache->is_open() ? core::cache_outcome_name(part.cache.outcome) : "no cache", double(part.cache.payload_bytes) / 1024.0,
					part.seconds_mesh * 1000.0);
			a->notes.push_back(line);
		}
		char line[160];
		std::snprintf(line, sizeof(line), "team %s: %zu binds, %llu vertices, %llu triangles", team.appearance.c_str(), team.binds.size(),
				(unsigned long long)team.vertices, (unsigned long long)team.triangles);
		a->notes.push_back(line);
	}
	a->seconds_meshes = seconds_since(t0);

	progress("decoding textures", 6);
	t0 = Clock::now();
	std::vector<std::pair<std::string, TextureRole>> wanted;
	std::set<std::string> seen;
	for (const SoldierTeam &team : a->teams) {
		for (const SoldierPart &part : team.parts) {
			for (const PreparedSurface &s : part.prepared.surfaces) {
				if (s.material_index >= part.materials.slots.size()) {
					continue;
				}
				const MaterialSlot &slot = part.materials.slots[s.material_index];
				for (auto [param, role] : { std::pair{ "Diffuse", TextureRole::Color }, std::pair{ "Normal", TextureRole::Normal } }) {
					const std::string name = slot.texture(param);
					if (!name.empty() && seen.insert(name).second) {
						wanted.emplace_back(name, role);
					}
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
	size_t skipped = 0;
	for (size_t i = 0; i < wanted.size(); ++i) {
		if (!errors[i].empty()) {
			++skipped;
			a->notes.push_back("texture skipped: " + errors[i]);
			continue;
		}
		a->textures.push_back(std::move(textures[i]));
	}
	a->notes.push_back("textures: " + std::to_string(a->textures.size()) + " decoded, " + std::to_string(skipped) + " skipped");
	a->seconds_textures = seconds_since(t0);
	a->seconds_total = seconds_since(start);
	progress("uploading", 7);
	return a;
}

PinchReport measure_pinch(const SoldierTeam &p_team, const anim::Skeleton &p_rig, std::span<const anim::Xform> p_model,
		std::span<const anim::Mat34> p_palette, float p_radius) {
	PinchReport r;
	const std::vector<anim::SkinBind> &binds = p_team.binds.binds();
	if (p_model.size() != p_rig.size() || p_palette.size() < binds.size()) {
		return r;
	}
	std::vector<anim::Xform> rest;
	anim::model_pose(p_rig, anim::rest_pose(p_rig), rest);
	struct Side {
		const char *arm;
		const char *roll;
		const char *fore;
		int32_t arm_j = -1, roll_j = -1, fore_j = -1;
		float *min_ratio = nullptr;
		uint32_t *count = nullptr;
		uint32_t *below = nullptr;
	};
	std::array<Side, 2> sides{ { { "LeftArm", "LeftArmRoll", "LeftForeArm" }, { "RightArm", "RightArmRoll", "RightForeArm" } } };
	sides[0].min_ratio = &r.min_ratio_left;
	sides[0].count = &r.vertices_left;
	sides[0].below = &r.below_left;
	sides[1].min_ratio = &r.min_ratio_right;
	sides[1].count = &r.vertices_right;
	sides[1].below = &r.below_right;
	for (Side &s : sides) {
		s.arm_j = p_rig.find(s.arm);
		s.roll_j = p_rig.find(s.roll);
		s.fore_j = p_rig.find(s.fore);
	}
	const int32_t hand = p_rig.find("LeftHand"), ik = p_rig.find("IK_Joint_LeftHand");
	if (hand >= 0 && ik >= 0) {
		r.hand_ik_m = float(anim::vec_dist(p_model[size_t(hand)].t, p_model[size_t(ik)].t));
	}

	auto radial = [](const anim::Vec3 &p_p, const anim::Vec3 &p_a, const anim::Vec3 &p_b) -> float {
		const anim::Vec3 ab = anim::vec_sub(p_b, p_a), ap = anim::vec_sub(p_p, p_a);
		const float len2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
		if (!(len2 > 1.0e-10f)) {
			return float(anim::vec_dist(p_p, p_a));
		}
		const float t = (ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / len2;
		const anim::Vec3 c{ p_a.x + ab.x * t, p_a.y + ab.y * t, p_a.z + ab.z * t };
		return float(anim::vec_dist(p_p, c));
	};
	for (const SoldierPart &part : p_team.parts) {
		if (part.weapon) {
			continue;
		}
		for (const PreparedSurface &surface : part.prepared.surfaces) {
			const size_t nv = surface.vertex_count();
			if (surface.bones.size() < nv * 4 || surface.weights.size() < nv * 4) {
				continue;
			}
			for (size_t v = 0; v < nv; ++v) {

				size_t best = 0;
				for (size_t k = 1; k < 4; ++k) {
					if (surface.weights[v * 4 + k] > surface.weights[v * 4 + best]) {
						best = k;
					}
				}
				const int32_t main_bind = surface.bones[v * 4 + best];
				if (main_bind < 0 || size_t(main_bind) >= binds.size()) {
					continue;
				}
				const int32_t bone = binds[size_t(main_bind)].bone;
				for (Side &s : sides) {
					if (s.arm_j < 0 || s.fore_j < 0 || (bone != s.arm_j && bone != s.roll_j)) {
						continue;
					}
					const anim::Vec3 p{ surface.positions[v * 3], surface.positions[v * 3 + 1], surface.positions[v * 3 + 2] };
					if (anim::vec_dist(p, rest[size_t(s.arm_j)].t) > p_radius) {
						continue;
					}
					const float bind_r = radial(p, rest[size_t(s.arm_j)].t, rest[size_t(s.fore_j)].t);
					if (!(bind_r > 1.0e-3f)) {
						continue;
					}
					anim::Vec3 q{};
					float wsum = 0.0f;
					for (size_t k = 0; k < 4; ++k) {
						const float w = surface.weights[v * 4 + k];
						const int32_t b = surface.bones[v * 4 + k];
						if (!(w > 0.0f) || b < 0 || size_t(b) >= binds.size()) {
							continue;
						}
						const anim::Vec3 x = anim::mat_point(p_palette[size_t(b)], p);
						q = anim::Vec3{ q.x + x.x * w, q.y + x.y * w, q.z + x.z * w };
						wsum += w;
					}
					if (!(wsum > 0.0f)) {
						continue;
					}
					q = anim::vec_scale(q, 1.0f / wsum);
					const float ratio = radial(q, p_model[size_t(s.arm_j)].t, p_model[size_t(s.fore_j)].t) / bind_r;
					++*s.count;
					*s.min_ratio = std::min(*s.min_ratio, ratio);
					*s.below += ratio < 0.7f ? 1 : 0;
				}
			}
		}
	}
	return r;
}

}

namespace bf3::render {
namespace {

int kit_role(std::string_view p_kit) {
	const std::string k = core::to_lower_ascii(p_kit);
	const char *words[4] = { "assault", "engineer", "support", "recon" };
	const size_t slash = k.find_last_of('/');
	const std::string_view tail = std::string_view(k).substr(slash == std::string::npos ? 0 : slash + 1);
	for (int r = 0; r < 4; ++r) {
		if (tail.find(words[r]) != std::string_view::npos) {
			return r;
		}
	}
	return -1;
}

void join_kit_parts(const core::EbxLibrary &p_library, const SoldierTeamSpec &p_spec, SoldierTeam &r_team, std::vector<std::string> &r_notes) {
	if (p_spec.team_data.empty()) {
		return;
	}
	const core::EbxPartition *team = p_library.partition_by_name(p_spec.team_data);
	if (team == nullptr) {
		r_notes.push_back("kits: " + p_spec.team_data + " not found (every kit wears " + p_spec.appearance + ")");
		return;
	}
	core::gameplay::WorkBudget budget;
	core::Result<core::gameplay::TeamDef> td = core::gameplay::read_team_data(p_library, team->primary(), &budget);
	if (!td) {
		r_notes.push_back("kits: " + p_spec.team_data + ": " + td.error().message + " (every kit wears " + p_spec.appearance + ")");
		return;
	}
	for (const std::string &kit : td->kits) {
		const int role = kit_role(kit);
		if (role < 0 || !r_team.kit_appearances[size_t(role)].empty()) {
			r_notes.push_back("kits: " + kit + ": no role or a second kit of its role (skipped)");
			continue;
		}
		core::Result<core::gameplay::KitAppearance> ka = core::gameplay::read_kit_appearance(p_library, kit, &budget);
		if (!ka) {
			r_notes.push_back("kits: " + ka.error().message + " (the kit wears " + p_spec.appearance + ")");
			continue;
		}
		r_team.kit_appearances[size_t(role)] = ka->appearance;
	}

	uint8_t base_mask = 0;
	for (size_t k = 0; k < 4; ++k) {
		if (r_team.kit_appearances[k].empty() || core::iequals_ascii(r_team.kit_appearances[k], p_spec.appearance)) {
			base_mask = uint8_t(base_mask | (1u << k));
		}
	}
	for (SoldierPart &p : r_team.parts) {
		p.kit_mask = base_mask;
	}
	std::vector<SoldierPart> joined;
	std::string line = "kits " + p_spec.team_data + ":";
	for (size_t k = 0; k < 4; ++k) {
		if ((base_mask & (1u << k)) != 0) {
			line += " " + std::to_string(k) + "=" + p_spec.appearance;
			continue;
		}
		std::vector<std::string> notes;
		core::Result<std::vector<SoldierPart>> parts = appearance_parts(p_library, r_team.kit_appearances[k], notes);
		if (!parts) {
			r_notes.push_back("kits: " + parts.error().message + " (kit " + std::to_string(k) + " wears " + p_spec.appearance + ")");
			for (SoldierPart &p : r_team.parts) {
				p.kit_mask = uint8_t(p.kit_mask | (1u << k));
			}
			r_team.kit_appearances[k].clear();
			continue;
		}
		for (const std::string &n : notes) {
			r_notes.push_back(n);
		}
		line += " " + std::to_string(k) + "=" + r_team.kit_appearances[k];
		for (SoldierPart &np : *parts) {
			auto same = [&](const SoldierPart &p_old) { return p_old.mesh == np.mesh && p_old.variation == np.variation; };
			const auto it = std::find_if(r_team.parts.begin(), r_team.parts.end(), same);
			if (it != r_team.parts.end()) {
				it->kit_mask = uint8_t(it->kit_mask | (1u << k));
				continue;
			}
			const auto jt = std::find_if(joined.begin(), joined.end(), same);
			if (jt != joined.end()) {
				jt->kit_mask = uint8_t(jt->kit_mask | (1u << k));
				continue;
			}
			np.kit_mask = uint8_t(1u << k);
			joined.push_back(std::move(np));
		}
	}
	for (SoldierPart &p : joined) {
		r_team.parts.push_back(std::move(p));
	}

	r_team.parts.erase(std::remove_if(r_team.parts.begin(), r_team.parts.end(), [](const SoldierPart &p_part) { return p_part.kit_mask == 0; }),
			r_team.parts.end());
	line += "; " + std::to_string(r_team.parts.size()) + " body parts in all";
	r_notes.push_back(line);
}

}
}
