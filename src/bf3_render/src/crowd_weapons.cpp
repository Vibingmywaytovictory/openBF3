#include "bf3/render/crowd_weapons.h"

#include "bf3/anim/crowd_clips.h"
#include "bf3/core/ebx.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/weapon_resolver.h"
#include "bf3/core/ids.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/thread_pool.h"
#include "bf3/render/mesh_cache.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>

namespace bf3::render {

namespace {

using Clock = std::chrono::steady_clock;

constexpr float kReloadRateMin = 0.5f;
constexpr float kReloadRateMax = 2.0f;
constexpr float kDegToRad = 0.0174532925f;

anim::Quat axis_quat(float p_x, float p_y, float p_z, float p_rad) {
	const float s = std::sin(0.5f * p_rad);
	return anim::Quat{ p_x * s, p_y * s, p_z * s, std::cos(0.5f * p_rad) };
}

anim::Xform xform_of(const anim::Mat34 &p_m) {
	return anim::Xform{ anim::quat_from_mat(p_m), p_m.origin() };
}

std::string partition_name(const core::EbxLibrary &p_library, const core::EbxInstance &p_instance) {
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

core::Result<core::EbxInstance> soldier_weapon_data(const core::EbxLibrary &p_library, const std::string &p_partition) {
	const core::EbxPartition *part = p_library.partition_by_name(p_partition);
	if (part == nullptr) {
		return core::make_error("weapon " + p_partition + " not found");
	}
	const std::vector<core::EbxInstance> data = part->instances_of("SoldierWeaponData");
	if (data.empty()) {
		return core::make_error("weapon " + p_partition + ": no SoldierWeaponData");
	}
	return data.front();
}

core::Result<uint32_t> weapon_ant_ref(const core::EbxInstance &p_data, const std::string &p_partition) {
	const std::optional<core::EbxValue> id = p_data.get_path("WeaponStates[0].Weapon.AssetId");
	if (!id) {
		return core::make_error("weapon " + p_partition + ": no WeaponStates[0].Weapon.AssetId");
	}
	const std::optional<int64_t> v = id->as_i64();
	if (!v || *v < 0 || *v > int64_t(UINT32_MAX)) {
		return core::make_error("weapon " + p_partition + ": WeaponStates[0].Weapon.AssetId is not a valid AntRef");
	}
	return uint32_t(*v);
}

std::string mesh_3p(const core::EbxLibrary &p_library, const core::EbxInstance &p_data) {
	const std::optional<core::EbxValue> state = p_data.get_path("WeaponStates[0]");
	if (!state) {
		return {};
	}
	const std::optional<core::EbxStruct> s = state->as_struct();
	if (!s) {
		return {};
	}
	const std::optional<core::EbxInstance> mesh = p_library.follow(*s, "Mesh3p");
	return mesh ? partition_name(p_library, *mesh) : std::string();
}

std::optional<anim::Vec3> vec3_field(const core::EbxStruct &p_s, std::string_view p_field) {
	const std::optional<core::EbxValue> v = p_s.get(p_field);
	if (!v) {
		return std::nullopt;
	}
	const std::optional<core::Vec3> a = v->as_vec3();
	if (!a || !std::isfinite((*a)[0]) || !std::isfinite((*a)[1]) || !std::isfinite((*a)[2])) {
		return std::nullopt;
	}
	return anim::Vec3{ (*a)[0], (*a)[1], (*a)[2] };
}

struct MuzzleEntry {
	anim::Vec3 offset{};
	anim::Vec3 rotation_deg{};
	std::string effect;
	size_t index = 0;
	size_t entries = 0;
};
core::Result<MuzzleEntry> muzzle_entry(const core::EbxLibrary &p_library, const std::string &p_firing_data) {
	const size_t hash = p_firing_data.find('#');
	if (hash == std::string::npos) {
		return core::make_error("firing data '" + p_firing_data + "' is not <partition>#<guid>");
	}
	const core::EbxPartition *part = p_library.partition_by_name(p_firing_data.substr(0, hash));
	const std::optional<core::Guid> guid = core::Guid::parse(std::string_view(p_firing_data).substr(hash + 1));
	if (part == nullptr || !guid) {
		return core::make_error("firing data " + p_firing_data + " not found");
	}
	const std::optional<core::EbxInstance> wfd = part->instance_by_guid(*guid);
	if (!wfd) {
		return core::make_error("firing data " + p_firing_data + ": no such instance");
	}
	const std::optional<core::EbxInstance> ffd = p_library.follow(wfd->data(), "PrimaryFire");
	if (!ffd) {
		return core::make_error("firing data " + p_firing_data + ": no PrimaryFire");
	}
	const std::optional<core::EbxValue> list = ffd->get("FireEffects3p");
	if (!list) {
		return core::make_error("firing data " + p_firing_data + ": no FireEffects3p");
	}
	MuzzleEntry best;
	bool found = false, muzzle = false;
	size_t index = 0;
	for (const core::EbxValue e : *list->as_array()) {
		const size_t i = index++;
		if (i >= 64) {
			break;
		}
		const std::optional<core::EbxStruct> s = e.as_struct();
		if (!s) {
			continue;
		}
		const std::optional<anim::Vec3> offset = vec3_field(*s, "Offset");
		if (!offset) {
			continue;
		}
		std::string effect;
		if (const std::optional<core::EbxInstance> fx = p_library.follow(*s, "Effect")) {
			effect = partition_name(p_library, *fx);
		}
		const bool is_muzzle = effect.find("muzz") != std::string::npos;
		if (!found || (is_muzzle && !muzzle)) {
			best.offset = *offset;
			best.rotation_deg = vec3_field(*s, "Rotation").value_or(anim::Vec3{});
			best.effect = effect;
			best.index = i;
			found = true;
			muzzle = is_muzzle;
		}
	}
	best.entries = index;
	if (!found) {
		return core::make_error("firing data " + p_firing_data + ": FireEffects3p has no entry with an offset");
	}
	return best;
}

core::Status prepare_item(const core::EbxLibrary &p_lib, const SoldierSpec &p_spec, const SoldierAssets &p_assets, const SoldierTeam &p_team,
		const std::vector<std::string> &p_databases, core::Cache *p_cache, CrowdWeaponPart &r_part, std::vector<std::string> &r_notes) {
	const anim::Skeleton &rig = p_assets.clips.rig;
	CrowdWeaponSpec &spec = r_part.spec;
	std::optional<core::EbxInstance> data;
	if (!spec.weapon_unlock.empty()) {
		core::Result<core::EbxInstance> d = soldier_weapon_data(p_lib, spec.weapon_unlock);
		if (d) {
			data = *d;
		} else if (spec.mesh.empty()) {
			return d.error();
		} else {
			r_notes.push_back(d.error().message + " (no reload clip)");
		}
	}
	if (spec.mesh.empty()) {
		spec.mesh = data ? mesh_3p(p_lib, *data) : std::string();
		if (spec.mesh.empty()) {
			return core::make_error("weapon " + spec.weapon_unlock + ": no WeaponStates[0].Mesh3p");
		}
	}

	core::Result<MeshMaterials> materials = mesh_materials(p_lib, spec.mesh, variation_hash_of(p_lib, std::string()), p_databases);
	if (!materials) {
		return core::error_at(spec.mesh, materials.error());
	}
	r_part.materials = std::move(*materials);

	const core::LevelMount &mount = p_lib.mount(0);
	core::Result<MeshHead> head = read_mesh_head(mount, spec.mesh);
	if (!head) {
		return head.error();
	}
	if (head->mesh.lods.size() <= p_spec.lod) {
		return core::make_error(spec.mesh + ": no LOD " + std::to_string(p_spec.lod));
	}
	core::Result<std::vector<std::vector<int32_t>>> lanes = r_part.binds.bind_lod(head->mesh.lods[p_spec.lod], p_assets.weapon_skeleton, rig);
	if (!lanes) {
		return core::error_at(spec.mesh, lanes.error());
	}
	if (r_part.binds.size() == 0 || r_part.binds.size() > anim::kMaxPaletteBinds) {
		return core::make_error(spec.mesh + ": " + std::to_string(r_part.binds.size()) + " binds (1.." + std::to_string(anim::kMaxPaletteBinds) + ")");
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
		return core::make_error(spec.mesh + ": LOD " + std::to_string(p_spec.lod) + " not prepared");
	}
	r_part.prepared = std::move(mesh->mesh.meshes[0]);
	r_part.cache = mesh->item;
	r_part.vertices = r_part.prepared.vertices;
	r_part.triangles = r_part.prepared.triangles;

	r_part.weapon_joints = p_team.weapon_joints;
	r_part.weapon_local = p_team.weapon_local;

	std::string reload_line = "no reload clip";
	if (data) {
		core::Result<uint32_t> ref = weapon_ant_ref(*data, spec.weapon_unlock);
		core::Result<uint32_t> index = ref ? anim::weapon_choice_index(*p_assets.library, *ref) : core::Result<uint32_t>(ref.error());
		core::Result<anim::CrowdClipData> reload = index ? anim::load_chosen_clip(p_assets.clips, *p_assets.library, p_spec.clips.reload_chooser, *index)
														 : core::Result<anim::CrowdClipData>(index.error());
		if (reload && reload->mask.size() == rig.size() && reload->tracks.end_frame > 0) {
			r_part.reload = std::move(*reload);
			r_part.have_reload = true;
			char line[160];
			std::snprintf(line, sizeof(line), "reload AntRef %u -> index %u, %u frames (%.2f s)", *ref, *index, r_part.reload.tracks.end_frame,
					double(r_part.reload.tracks.duration_seconds()));
			reload_line = line;
		} else {
			reload_line = "no reload clip (" + (reload ? std::string("unusable clip") : reload.error().message) + ")";
		}
	}

	std::optional<core::gameplay::WeaponDef> def;
	if (!spec.weapon_def.empty()) {
		core::Result<core::gameplay::WeaponDef> w = core::gameplay::resolve_weapon(p_lib, spec.weapon_def);
		if (w) {
			def = std::move(*w);
		} else {
			r_notes.push_back("weapon " + spec.weapon_def + " did not resolve (reload rate 1, no muzzle): " + w.error().message);
		}
	}
	if (r_part.have_reload && def && std::isfinite(def->fire.reload_time) && def->fire.reload_time > 0.0f) {
		const float rate = r_part.reload.tracks.duration_seconds() / def->fire.reload_time;
		r_part.reload_rate = std::isfinite(rate) ? std::clamp(rate, kReloadRateMin, kReloadRateMax) : 1.0f;
		char line[96];
		std::snprintf(line, sizeof(line), ", reload time %.2f s -> rate %.3f", double(def->fire.reload_time), double(r_part.reload_rate));
		reload_line += line;
	}

	std::string muzzle_line = "no muzzle";
	const int32_t rig_root = rig.find("Wep_Root");
	const int32_t w_root = p_assets.weapon_skeleton.find("Wep_Root");
	if (def && rig_root >= 0 && w_root >= 0 && size_t(w_root) < p_assets.weapon_skeleton.model_pose.size()) {
		core::Result<MuzzleEntry> entry = muzzle_entry(p_lib, def->firing_data);
		if (entry) {
			const anim::Xform inv_bind = anim::xform_inverse(xform_of(p_assets.weapon_skeleton.model_pose[size_t(w_root)]));
			const anim::Vec3 r = entry->rotation_deg;

			const anim::Quat q = anim::quat_mul(anim::quat_mul(axis_quat(0, 1, 0, r.y * kDegToRad), axis_quat(1, 0, 0, r.x * kDegToRad)),
					axis_quat(0, 0, 1, r.z * kDegToRad));
			const anim::Xform in_mesh{ anim::quat_mul(q, axis_quat(1, 0, 0, 90.0f * kDegToRad)), entry->offset };
			r_part.muzzle_joint = rig_root;
			r_part.muzzle_offset = anim::xform_mul(inv_bind, in_mesh);
			char line[256];
			std::snprintf(line, sizeof(line), "muzzle Wep_Root + FireEffects3p[%zu/%zu] %s at (%.3f, %.3f, %.3f) rot (%.1f, %.1f, %.1f)", entry->index, entry->entries,
					entry->effect.c_str(), double(entry->offset.x), double(entry->offset.y), double(entry->offset.z), double(r.x), double(r.y), double(r.z));
			muzzle_line = line;
		} else {
			muzzle_line = "no muzzle (" + entry.error().message + ")";
		}
	} else if (def) {
		muzzle_line = "no muzzle (no Wep_Root)";
	}
	char line[512];
	std::snprintf(line, sizeof(line), "item def %u %s: mesh %s, %zu binds, %llu vertices, %llu triangles; %s; %s", unsigned(spec.def), spec.weapon_def.c_str(),
			spec.mesh.c_str(), r_part.binds.size(), (unsigned long long)r_part.vertices, (unsigned long long)r_part.triangles, reload_line.c_str(), muzzle_line.c_str());
	r_notes.push_back(line);
	return core::Status::success();
}

}

core::Result<std::unique_ptr<CrowdWeaponSet>> load_crowd_weapons(const core::InstallReader &p_install, const SoldierSpec &p_spec, const SoldierAssets &p_assets,
		const std::array<std::span<const CrowdWeaponSpec>, 2> &p_items, core::Cache *p_cache) {
	const Clock::time_point start = Clock::now();
	if (p_assets.library == nullptr || p_assets.clips.rig.size() == 0) {
		return core::make_error("crowd weapons: the soldier assets have no clip library or rig");
	}
	for (size_t t = 0; t < p_items.size(); ++t) {
		if (p_items[t].size() > anim::kMaxCrowdWeapons) {
			return core::make_error("crowd weapons: team " + std::to_string(t) + " has " + std::to_string(p_items[t].size()) + " items (cap " +
					std::to_string(anim::kMaxCrowdWeapons) + ")");
		}
	}
	auto set = std::make_unique<CrowdWeaponSet>();
	core::Result<std::shared_ptr<core::Install>> install = core::Install::open(p_install);
	if (!install) {
		return core::error_at("install", install.error());
	}
	core::Result<std::unique_ptr<core::EbxLibrary>> ebx = core::EbxLibrary::open(*install, { p_spec.superbundle });
	if (!ebx) {
		return core::error_at(p_spec.superbundle, ebx.error());
	}
	const core::EbxLibrary &lib = **ebx;
	const std::vector<std::string> databases = variation_database_names(lib);
	for (size_t t = 0; t < p_items.size(); ++t) {
		for (size_t i = 0; i < p_items[t].size(); ++i) {
			CrowdWeaponPart part;
			part.spec = p_items[t][i];
			const core::Status st = prepare_item(lib, p_spec, p_assets, p_assets.teams[t], databases, p_cache, part, set->notes);
			if (!st.ok()) {
				if (i == 0) {
					return core::error_at("crowd weapons: team " + std::to_string(t) + " item 0", st.error());
				}
				set->notes.push_back("team " + std::to_string(t) + " item def " + std::to_string(part.spec.def) + " (" + part.spec.weapon_def +
						") dropped: " + st.error().message);
				continue;
			}
			set->teams[t].push_back(std::move(part));
		}
	}

	std::vector<std::pair<std::string, TextureRole>> wanted;
	std::set<std::string> seen;
	for (const std::vector<CrowdWeaponPart> &team : set->teams) {
		for (const CrowdWeaponPart &part : team) {
			for (const PreparedSurface &s : part.prepared.surfaces) {
				if (s.material_index >= part.materials.slots.size()) {
					continue;
				}
				const MaterialSlot &slot = part.materials.slots[s.material_index];
				for (auto [param, role] : { std::pair{ "Diffuse", TextureRole::Color }, std::pair{ "Normal", TextureRole::Normal } }) {
					const std::string name = slot.texture(param);
					if (!name.empty() && p_assets.texture(name) == nullptr && seen.insert(name).second) {
						wanted.emplace_back(name, role);
					}
				}
			}
		}
	}
	const core::LevelMount &mount = lib.mount(0);
	std::vector<std::string> errors(wanted.size());
	std::vector<PreparedTexture> textures(wanted.size());
	core::parallel_for(wanted.size(), [&](size_t i) {
		core::Result<core::res::LoadedTexture> tex = core::res::load_texture(mount, wanted[i].first);
		if (!tex) {
			errors[i] = tex.error().message;
			return;
		}
		core::Result<PreparedTexture> p = prepare_texture(*tex, wanted[i].second);
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
			set->notes.push_back("weapon texture skipped: " + errors[i]);
			continue;
		}
		set->textures.push_back(std::move(textures[i]));
	}
	set->seconds = std::chrono::duration<double>(Clock::now() - start).count();
	char line[160];
	std::snprintf(line, sizeof(line), "crowd weapons: %zu + %zu items, %zu extra textures (%zu skipped), %.2f s", set->teams[0].size(), set->teams[1].size(),
			set->textures.size(), skipped, set->seconds);
	set->notes.push_back(line);
	return set;
}

}
