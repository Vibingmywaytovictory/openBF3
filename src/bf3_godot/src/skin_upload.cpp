// Skin palette upload backends for the 3p crowd (S3; docs/design/s3.md §7). See skin_upload.h.
#include "skin_upload.h"

#include "godot_util.h"
#include "render_bridge.h"

#include "bf3/render/soldier_assets.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/skin.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/aabb.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <string>

namespace bf3::godot_side {

using namespace godot;

const char *skin_backend_name(SkinBackend p_backend) {
	switch (p_backend) {
		case SkinBackend::Skeleton3D:
			return "skeleton3d";
		case SkinBackend::RsSkeleton:
			return "rs";
		case SkinBackend::BoneTexture:
			return "texture";
	}
	return "?";
}

namespace {

// One team's drawable: every part's surfaces merged into one mesh (bones = the team's bind indices), its bind-pose
// bounds grown by 1 m (a skinned instance's bounds are not recomputed, s3.md §7.1), and the team's Skin (backend A).
struct TeamMesh {
	Ref<ArrayMesh> mesh;
	AABB aabb;
	Ref<Skin> skin;
};

// PT1 iff [kits] (replaces build_team_meshes; docs/design/pt1-iff.md §2.1): one mesh per (team, kit) over the parts
// whose SoldierPart::kit_mask names the kit; a kit wearing the same parts as an earlier kit of its team shares that
// mesh (S3's assets, every mask 0x0F: one mesh per team, as before). Textures and materials are made once for all.
// r_teams = kit 0's meshes; r_kits (when given) every kit's.
bool build_team_meshes(const bf3::render::SoldierAssets &p_assets, std::array<TeamMesh, 2> &r_teams, std::string *r_error,
		std::array<std::array<TeamMesh, 2>, 4> *r_kits = nullptr) {
	std::map<std::string, Ref<Texture2D>> textures;
	for (const bf3::render::PreparedTexture &t : p_assets.textures) {
		const Ref<ImageTexture> tex = make_texture(t);
		if (tex.is_valid()) {
			textures[t.name] = tex;
		}
	}
	auto find_texture = [&](const std::string &p_name) -> Ref<Texture2D> {
		const auto it = textures.find(p_name);
		return it == textures.end() ? Ref<Texture2D>() : it->second;
	};
	std::array<std::array<TeamMesh, 2>, 4> kits;
	for (size_t t = 0; t < r_teams.size(); ++t) {
		const bf3::render::SoldierTeam &team = p_assets.teams[t];
		std::map<std::pair<size_t, uint32_t>, Ref<StandardMaterial3D>> by_slot;
		std::array<std::vector<bool>, 4> worn;
		for (size_t k = 0; k < kits.size(); ++k) {
			worn[k].resize(team.parts.size());
			for (size_t p = 0; p < team.parts.size(); ++p) {
				worn[k][p] = (team.parts[p].kit_mask & (1u << k)) != 0;
			}
			bool shared = false;
			for (size_t e = 0; e < k; ++e) {
				if (worn[e] == worn[k] && kits[e][t].mesh.is_valid()) {
					kits[k][t] = kits[e][t];
					shared = true;
					break;
				}
			}
			if (shared) {
				continue;
			}
			bf3::render::PreparedMesh merged;
			merged.name = team.kit_appearances[k].empty() ? team.appearance : team.kit_appearances[k];
			merged.skinned = true;
			std::vector<Ref<Material>> materials;
			bool have_bounds = false;
			Vector3 lo, hi;
			for (size_t p = 0; p < team.parts.size(); ++p) {
				if (!worn[k][p]) {
					continue;
				}
				const bf3::render::SoldierPart &part = team.parts[p];
				for (const bf3::render::PreparedSurface &s : part.prepared.surfaces) {
					merged.surfaces.push_back(s);
					Ref<Material> m;
					if (s.material_index < part.materials.slots.size()) {
						Ref<StandardMaterial3D> &cached = by_slot[{ p, s.material_index }];
						if (cached.is_null()) {
							cached = make_standard_material(part.materials.slots[s.material_index], !s.tangents.empty(), find_texture, MaterialOptions{});
						}
						m = cached;
					}
					materials.push_back(m);
					const Vector3 a(s.aabb_min[0], s.aabb_min[1], s.aabb_min[2]), b(s.aabb_max[0], s.aabb_max[1], s.aabb_max[2]);
					lo = have_bounds ? lo.min(a) : a;
					hi = have_bounds ? hi.max(b) : b;
					have_bounds = true;
				}
			}
			if (merged.surfaces.empty()) {
				if (r_error != nullptr) {
					*r_error = "team " + std::to_string(t) + " kit " + std::to_string(k) + " has no surfaces";
				}
				return false;
			}
			const bf3::render::PreparedSurface *first = merged.surfaces.data();
			kits[k][t].mesh = make_array_mesh(merged, [&](const bf3::render::PreparedSurface &p_s) -> Ref<Material> {
				const size_t index = size_t(&p_s - first);
				return index < materials.size() ? materials[index] : Ref<Material>();
			});
			if (kits[k][t].mesh.is_null() || kits[k][t].mesh->get_surface_count() != int32_t(merged.surfaces.size())) {
				if (r_error != nullptr) {
					*r_error = "team " + std::to_string(t) + " kit " + std::to_string(k) + ": the merged mesh was not created";
				}
				return false;
			}
			const Vector3 grow(1.0f, 1.0f, 1.0f);
			kits[k][t].aabb = AABB(lo - grow, (hi - lo) + grow * 2.0f);
			kits[k][t].skin = k == 0 ? make_skin(team.binds.binds()) : kits[0][t].skin;
		}
	}
	r_teams = kits[0];
	if (r_kits != nullptr) {
		*r_kits = kits;
	}
	return true;
}

// PT1 iff [kits]: distinct meshes of a [kit][team] table.
uint32_t count_kit_meshes(const std::array<std::array<TeamMesh, 2>, 4> &p_kits) {
	std::vector<const ArrayMesh *> seen;
	for (const auto &kit : p_kits) {
		for (const TeamMesh &m : kit) {
			if (m.mesh.is_valid() && std::find(seen.begin(), seen.end(), m.mesh.ptr()) == seen.end()) {
				seen.push_back(m.mesh.ptr());
			}
		}
	}
	return uint32_t(seen.size());
}

// ---- A: Skeleton3D ----------------------------------------------------------------------------------------------

class Skeleton3DUploader final : public SkinUploader {
public:
	explicit Skeleton3DUploader(Node *p_parent) :
			parent_(p_parent) {}
	~Skeleton3DUploader() override { clear(); }

	SkinBackend backend() const override { return SkinBackend::Skeleton3D; }

	bool build(const bf3::render::SoldierAssets &p_assets, const bf3::anim::CrowdEvaluator &p_eval, std::span<const uint8_t> p_team_of, RID p_scenario,
			std::string *r_error) override {
		(void)p_scenario; // the nodes join the parent's world
		if (parent_ == nullptr) {
			if (r_error != nullptr) {
				*r_error = "skeleton3d backend without a parent node";
			}
			return false;
		}
		if (!build_team_meshes(p_assets, teams_, r_error, &kits_)) { // PT1 iff [kits]
			return false;
		}
		const bf3::anim::Skeleton &rig = p_assets.clips.rig;
		parents_.clear();
		for (const bf3::anim::Bone &b : rig.bones()) {
			parents_.push_back(b.parent);
		}
		// The bones any layer can move: every clip's channels, both reloads and the weapon layout.
		std::vector<bool> driven(rig.size(), false);
		auto add = [&](const bf3::anim::BoneMask &p_mask) {
			for (size_t j = 0; j < std::min(p_mask.size(), driven.size()); ++j) {
				driven[j] = driven[j] || p_mask.weight[j] > 0.0f;
			}
		};
		for (const bf3::anim::CrowdClipData &c : p_assets.clips.clips) {
			add(c.mask);
		}
		for (const bf3::render::SoldierTeam &t : p_assets.teams) {
			add(t.reload.mask);
			add(t.weapon_joints);
		}
		driven_.clear();
		for (size_t j = 0; j < driven.size(); ++j) {
			if (driven[j]) {
				driven_.push_back(int32_t(j));
			}
		}
		const uint32_t n = p_eval.soldiers();
		for (uint32_t i = 0; i < n; ++i) {
			const size_t team = i < p_team_of.size() ? std::min<size_t>(p_team_of[i], 1) : 0;
			Skeleton3D *sk = make_skeleton(rig);
			sk->set_name(to_godot("CrowdSkeleton" + std::to_string(i)));
			parent_->add_child(sk);
			MeshInstance3D *mi = memnew(MeshInstance3D);
			mi->set_name("Mesh");
			mi->set_mesh(teams_[team].mesh);
			mi->set_skin(teams_[team].skin);
			mi->set_custom_aabb(teams_[team].aabb);
			sk->add_child(mi);
			mi->set_skeleton_path(NodePath(".."));
			nodes_.push_back(sk->get_instance_id());
			skeletons_.push_back(sk);
			meshes_.push_back(mi); // PT1 iff [kits]
			kit_of_.push_back(uint16_t(team << 8)); // PT1 iff [kits]: team << 8 | kit
		}
		return true;
	}

	// PT1 iff [kits]
	void set_soldier_kit(uint32_t p_soldier, uint8_t p_kit) override {
		if (p_soldier >= meshes_.size()) {
			return;
		}
		const size_t team = kit_of_[p_soldier] >> 8;
		const size_t kit = p_kit < kits_.size() ? p_kit : 0;
		if ((kit_of_[p_soldier] & 0xFF) == kit) {
			return;
		}
		kit_of_[p_soldier] = uint16_t(team << 8 | kit);
		const TeamMesh &m = kits_[kit][team];
		if (m.mesh.is_valid() && meshes_[p_soldier]->get_mesh() != m.mesh) {
			meshes_[p_soldier]->set_mesh(m.mesh);
			meshes_[p_soldier]->set_custom_aabb(m.aabb);
		}
	}
	uint32_t kit_meshes() const override { return count_kit_meshes(kits_); } // PT1 iff [kits]

	void upload(const bf3::anim::CrowdEvaluator &p_eval, std::span<const Transform3D> p_roots, SkinUploadStats &r_stats) override {
		r_stats = SkinUploadStats{};
		const size_t n = std::min(skeletons_.size(), p_roots.size());
		for (size_t i = 0; i < n; ++i) {
			Skeleton3D *sk = skeletons_[i];
			sk->set_transform(p_roots[i]);
			++r_stats.calls;
			if (!palette_upload_ || !p_eval.changed(uint32_t(i))) {
				continue;
			}
			const std::span<const bf3::anim::Xform> model = p_eval.model(uint32_t(i));
			if (model.size() != parents_.size()) {
				continue;
			}
			for (const int32_t j : driven_) {
				const int32_t p = parents_[size_t(j)];
				const bf3::anim::Xform local = p < 0 ? model[size_t(j)] : bf3::anim::xform_mul(bf3::anim::xform_inverse(model[size_t(p)]), model[size_t(j)]);
				sk->set_bone_pose_rotation(j, to_godot(local.q));
				sk->set_bone_pose_position(j, to_godot(local.t));
			}
			r_stats.calls += uint32_t(driven_.size() * 2);
			++r_stats.soldiers_uploaded;
		}
	}

	void set_visible(bool p_visible) override {
		for (Skeleton3D *sk : skeletons_) {
			sk->set_visible(p_visible);
		}
	}

	void set_soldier_visible(uint32_t p_soldier, bool p_visible) override {
		if (p_soldier < skeletons_.size()) {
			skeletons_[p_soldier]->set_visible(p_visible);
		}
	}

	void clear() override {
		for (const uint64_t id : nodes_) {
			if (Object *o = ObjectDB::get_instance(id)) {
				if (Node *node = Object::cast_to<Node>(o)) {
					node->queue_free();
				}
			}
		}
		nodes_.clear();
		skeletons_.clear();
		meshes_.clear(); // PT1 iff [kits]
		kit_of_.clear(); // PT1 iff [kits]
		kits_ = {}; // PT1 iff [kits]
		teams_ = {};
	}

private:
	Node *parent_ = nullptr;
	std::array<TeamMesh, 2> teams_;
	std::array<std::array<TeamMesh, 2>, 4> kits_; // PT1 iff [kits]: [kit][team]
	std::vector<MeshInstance3D *> meshes_; // PT1 iff [kits]
	std::vector<uint16_t> kit_of_; // PT1 iff [kits]
	std::vector<int32_t> parents_;
	std::vector<int32_t> driven_;
	std::vector<uint64_t> nodes_;
	std::vector<Skeleton3D *> skeletons_;
};

// ---- B: RenderingServer skeletons -------------------------------------------------------------------------------

class RsSkeletonUploader final : public SkinUploader {
public:
	~RsSkeletonUploader() override { clear(); }

	SkinBackend backend() const override { return SkinBackend::RsSkeleton; }

	bool build(const bf3::render::SoldierAssets &p_assets, const bf3::anim::CrowdEvaluator &p_eval, std::span<const uint8_t> p_team_of, RID p_scenario,
			std::string *r_error) override {
		if (!build_team_meshes(p_assets, teams_, r_error, &kits_)) { // PT1 iff [kits]
			return false;
		}
		RenderingServer *rs = RenderingServer::get_singleton();
		const uint32_t n = p_eval.soldiers();
		for (uint32_t i = 0; i < n; ++i) {
			const size_t team = i < p_team_of.size() ? std::min<size_t>(p_team_of[i], 1) : 0;
			const int32_t binds = int32_t(p_assets.teams[team].binds.size());
			Soldier s;
			s.instance = rs->instance_create2(teams_[team].mesh->get_rid(), p_scenario);
			s.skeleton = rs->skeleton_create();
			if (!s.instance.is_valid() || !s.skeleton.is_valid()) {
				if (r_error != nullptr) {
					*r_error = "RenderingServer refused an instance or a skeleton";
				}
				soldiers_.push_back(s);
				return false;
			}
			rs->skeleton_allocate_data(s.skeleton, binds, false);
			for (int32_t b = 0; b < binds; ++b) {
				rs->skeleton_bone_set_transform(s.skeleton, b, Transform3D());
			}
			rs->instance_attach_skeleton(s.instance, s.skeleton);
			rs->instance_set_custom_aabb(s.instance, teams_[team].aabb);
			s.binds = binds;
			s.team = uint8_t(team); // PT1 iff [kits]
			soldiers_.push_back(s);
		}
		return true;
	}

	void upload(const bf3::anim::CrowdEvaluator &p_eval, std::span<const Transform3D> p_roots, SkinUploadStats &r_stats) override {
		r_stats = SkinUploadStats{};
		RenderingServer *rs = RenderingServer::get_singleton();
		const size_t n = std::min(soldiers_.size(), p_roots.size());
		for (size_t i = 0; i < n; ++i) {
			const Soldier &s = soldiers_[i];
			rs->instance_set_transform(s.instance, p_roots[i]);
			++r_stats.calls;
			if (!palette_upload_ || !p_eval.changed(uint32_t(i))) {
				continue;
			}
			const std::span<const bf3::anim::Mat34> palette = p_eval.palette(uint32_t(i));
			const int32_t count = std::min<int32_t>(s.binds, int32_t(palette.size()));
			for (int32_t b = 0; b < count; ++b) {
				rs->skeleton_bone_set_transform(s.skeleton, b, to_godot(palette[size_t(b)]));
			}
			r_stats.calls += uint32_t(count);
			++r_stats.soldiers_uploaded;
		}
	}

	void set_visible(bool p_visible) override {
		RenderingServer *rs = RenderingServer::get_singleton();
		for (const Soldier &s : soldiers_) {
			if (s.instance.is_valid()) {
				rs->instance_set_visible(s.instance, p_visible);
			}
		}
	}

	void set_soldier_visible(uint32_t p_soldier, bool p_visible) override {
		if (p_soldier < soldiers_.size() && soldiers_[p_soldier].instance.is_valid()) {
			RenderingServer::get_singleton()->instance_set_visible(soldiers_[p_soldier].instance, p_visible);
		}
	}

	void clear() override {
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs != nullptr) {
			for (const Soldier &s : soldiers_) {
				if (s.instance.is_valid()) {
					rs->free_rid(s.instance);
				}
				if (s.skeleton.is_valid()) {
					rs->free_rid(s.skeleton);
				}
			}
		}
		soldiers_.clear();
		kits_ = {}; // PT1 iff [kits]
		teams_ = {};
	}

	// PT1 iff [kits]: the instance's base becomes the kit's mesh (the skeleton stays attached; the bounds are set again).
	void set_soldier_kit(uint32_t p_soldier, uint8_t p_kit) override {
		if (p_soldier >= soldiers_.size() || !soldiers_[p_soldier].instance.is_valid()) {
			return;
		}
		Soldier &s = soldiers_[p_soldier];
		const uint8_t kit = p_kit < kits_.size() ? p_kit : uint8_t(0);
		if (s.kit == kit) {
			return;
		}
		const TeamMesh &from = kits_[s.kit][s.team];
		const TeamMesh &to = kits_[kit][s.team];
		s.kit = kit;
		if (to.mesh.is_null() || to.mesh == from.mesh) {
			return;
		}
		RenderingServer *rs = RenderingServer::get_singleton();
		rs->instance_set_base(s.instance, to.mesh->get_rid());
		rs->instance_set_custom_aabb(s.instance, to.aabb);
	}
	uint32_t kit_meshes() const override { return count_kit_meshes(kits_); } // PT1 iff [kits]

private:
	struct Soldier {
		RID instance;
		RID skeleton;
		int32_t binds = 0;
		uint8_t team = 0; // PT1 iff [kits]
		uint8_t kit = 0; // PT1 iff [kits]: the mesh drawn (kit 0 from build)
	};
	std::array<TeamMesh, 2> teams_;
	std::array<std::array<TeamMesh, 2>, 4> kits_; // PT1 iff [kits]: [kit][team]
	std::vector<Soldier> soldiers_;
};

} // namespace

std::unique_ptr<SkinUploader> make_skin_uploader(SkinBackend p_backend, Node *p_parent) {
	switch (p_backend) {
		case SkinBackend::Skeleton3D:
			return std::make_unique<Skeleton3DUploader>(p_parent);
		case SkinBackend::RsSkeleton:
			return std::make_unique<RsSkeletonUploader>();
		case SkinBackend::BoneTexture:
			return nullptr; // not built: s3.md §7.3 (needs the L track's material path and the user's approval)
	}
	return nullptr;
}

} // namespace bf3::godot_side
