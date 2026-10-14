#include "m9_reload_check.h"

#include "godot_util.h"
#include "render_bridge.h"
#include "viewmodel_demo.h"

#include "bf3/anim/pose.h"
#include "bf3/anim/skinning.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/skin.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace bf3::godot_side {

using namespace godot;
using bf3::anim::Xform;

namespace {

std::string fmt(const char *p_fmt, ...) {
	char buf[640];
	va_list args;
	va_start(args, p_fmt);
	vsnprintf(buf, sizeof(buf), p_fmt, args);
	va_end(args);
	return buf;
}

std::string env_var(const char *p_name) {
	char *v = nullptr;
	size_t n = 0;
	if (_dupenv_s(&v, &n, p_name) != 0 || v == nullptr) {
		return {};
	}
	std::string s(v);
	std::free(v);
	return s;
}

struct Row {
	std::string check;
	int result; // 0 pass, 1 fail, 2 info, 3 not run
	std::string detail;
};

Xform from_godot(const Transform3D &p_t) {
	const Quaternion q = p_t.basis.get_rotation_quaternion();
	return Xform{ bf3::anim::Quat{ float(q.x), float(q.y), float(q.z), float(q.w) }, bf3::anim::Vec3{ float(p_t.origin.x), float(p_t.origin.y), float(p_t.origin.z) } };
}

// The Skeleton3D's global (skeleton-space) pose of every bone.
std::vector<Xform> godot_pose(Skeleton3D *p_skeleton) {
	p_skeleton->force_update_all_bone_transforms();
	std::vector<Xform> out(size_t(p_skeleton->get_bone_count()));
	for (int32_t b = 0; b < p_skeleton->get_bone_count(); ++b) {
		out[size_t(b)] = from_godot(p_skeleton->get_bone_global_pose(b));
	}
	return out;
}

double transform_diff(const Transform3D &p_a, const Transform3D &p_b) {
	double d = 0.0;
	for (int c = 0; c < 3; ++c) {
		d = std::max(d, double((p_a.basis.get_column(c) - p_b.basis.get_column(c)).length()));
	}
	return std::max(d, double((p_a.origin - p_b.origin).length()));
}

// Model-space poses rebuilt from the Ant probe's clip JSON (its own joints and per-key local tracks).
struct ProbeClip {
	bf3::anim::Skeleton skeleton;
	std::vector<double> key_times;
	std::vector<std::vector<Xform>> model; // per key
	std::string error;
};

bool load_probe(const std::string &p_file, ProbeClip &r_probe) {
	std::ifstream in(std::filesystem::path(std::u8string(p_file.begin(), p_file.end())), std::ios::binary);
	if (!in) {
		r_probe.error = "cannot read " + p_file;
		return false;
	}
	std::stringstream ss;
	ss << in.rdbuf();
	const Variant v = JSON::parse_string(to_godot(ss.str()));
	if (v.get_type() != Variant::DICTIONARY) {
		r_probe.error = "not a JSON object";
		return false;
	}
	const Dictionary d = v;
	const Array joints = d["joints"], tracks = d["tracks"], keys = d["keyTimes"];
	std::vector<bf3::anim::Bone> bones;
	for (int64_t i = 0; i < joints.size(); ++i) {
		const Dictionary j = joints[i];
		const Array t = j["t"], q = j["q"];
		bf3::anim::Bone b;
		b.name = to_std(String(j["name"]));
		b.parent = int32_t(j["parent"]);
		b.rest = Xform{ bf3::anim::Quat{ float(q[0]), float(q[1]), float(q[2]), float(q[3]) }, bf3::anim::Vec3{ float(t[0]), float(t[1]), float(t[2]) } };
		bones.push_back(std::move(b));
	}
	bf3::core::Result<bf3::anim::Skeleton> s = bf3::anim::Skeleton::make(std::move(bones));
	if (!s) {
		r_probe.error = s.error().message;
		return false;
	}
	r_probe.skeleton = std::move(*s);
	for (int64_t k = 0; k < keys.size(); ++k) {
		r_probe.key_times.push_back(double(keys[k]));
		bf3::anim::LocalPose pose = bf3::anim::rest_pose(r_probe.skeleton);
		for (int64_t c = 0; c < tracks.size(); ++c) {
			const Dictionary tr = tracks[c];
			const int32_t joint = int32_t(tr["joint"]);
			const Array values = tr["values"];
			const Array val = values[k];
			if (joint < 0 || size_t(joint) >= pose.joints.size()) {
				continue;
			}
			if (String(tr["kind"]) == String("q")) {
				pose.joints[size_t(joint)].q = bf3::anim::Quat{ float(val[0]), float(val[1]), float(val[2]), float(val[3]) };
			} else {
				pose.joints[size_t(joint)].t = bf3::anim::Vec3{ float(val[0]), float(val[1]), float(val[2]) };
			}
		}
		std::vector<Xform> model;
		bf3::anim::model_pose(r_probe.skeleton, pose, model);
		r_probe.model.push_back(std::move(model));
	}
	return true;
}

} // namespace

int run_m9_reload_check(Bf3ViewmodelDemo &p_demo) {
	std::vector<Row> rows;
	bool failed = false;
	auto add = [&](const std::string &p_check, int p_result, const std::string &p_detail) {
		rows.push_back({ p_check, p_result, p_detail });
		failed |= p_result == 1;
	};
	const bf3::render::ViewmodelAssets &a = p_demo.assets();
	Skeleton3D *sk = p_demo.skeleton();
	const bf3::anim::Skeleton &rig = a.rig;

	add("load", 0, fmt("%.2f s (ant %.2f, ebx %.2f, meshes %.3f, textures %.3f) + upload %.3f s", p_demo.load_seconds(), a.seconds_ant, a.seconds_ebx,
						   a.seconds_meshes, a.seconds_textures, p_demo.upload_seconds()));
	add("clip", (a.tracks.channels_bound == 135 && a.tracks.end_frame == 65 && a.tracks.fps == 30.0f) ? 0 : 1,
			fmt("%s: %u keys, frames 0..%u at %.0f fps (%.3f s), %u of %u channels on rig bones", std::string(a.clip.binding.controller_name).c_str(),
					a.clip.clip.num_keys, a.tracks.end_frame, a.tracks.fps, a.tracks.duration_seconds(), a.tracks.channels_bound,
					a.tracks.channels_bound + a.tracks.channels_unbound));
	add("skeleton", (sk->get_bone_count() == int32_t(rig.size()) && rig.size() == 181) ? 0 : 1,
			fmt("Skeleton3D %d bones from rig %s", sk->get_bone_count(), std::string(a.clip.binding.rig->name).c_str()));

	// Meshes and skin.
	size_t surfaces = 0, bound_vertices = 0, vertices = 0;
	for (const bf3::render::ViewmodelPart *part : { &a.arms, &a.weapon }) {
		for (const bf3::render::PreparedSurface &s : part->prepared.surfaces) {
			++surfaces;
			vertices += s.vertex_count();
			bound_vertices += s.bones.size() == s.vertex_count() * 4 ? s.vertex_count() : 0;
		}
	}
	add("meshes", (vertices > 0 && bound_vertices == vertices) ? 0 : 1,
			fmt("arms %s: %zu v / %zu t; M9: %zu v / %zu t; %zu drawn surfaces, every vertex skinned", a.arms.mesh.substr(a.arms.mesh.rfind('/') + 1).c_str(),
					a.arms.prepared.vertices, a.arms.prepared.triangles, a.weapon.prepared.vertices, a.weapon.prepared.triangles, surfaces));

	// Materials: 1P FOV from the weapon data, z clip scale, textures bound.
	{
		int with_fov = 0, total = 0, albedo = 0, normal = 0;
		float fov = 0.0f;
		for (int32_t c = 0; c < sk->get_child_count(); ++c) {
			MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(sk->get_child(c));
			if (mi == nullptr || mi->get_mesh().is_null()) {
				continue;
			}
			const Ref<Mesh> mesh = mi->get_mesh();
			for (int32_t s = 0; s < mesh->get_surface_count(); ++s) {
				const Ref<BaseMaterial3D> m = mesh->surface_get_material(s);
				++total;
				if (m.is_valid() && m->get_flag(BaseMaterial3D::FLAG_USE_FOV_OVERRIDE) && m->get_flag(BaseMaterial3D::FLAG_USE_Z_CLIP_SCALE)) {
					++with_fov;
					fov = m->get_fov_override();
				}
				albedo += (m.is_valid() && m->get_texture(BaseMaterial3D::TEXTURE_ALBEDO).is_valid()) ? 1 : 0;
				normal += (m.is_valid() && m->get_texture(BaseMaterial3D::TEXTURE_NORMAL).is_valid()) ? 1 : 0;
			}
		}
		add("materials.1p", (total > 0 && with_fov == total && a.render_fov > 0.0f && std::fabs(fov - a.render_fov) < 1e-3f) ? 0 : 1,
				fmt("%d of %d surfaces with fov_override %.1f (WeaponDef RenderFov %.1f) and z_clip_scale", with_fov, total, fov, a.render_fov));
		add("materials.textures", (albedo == total) ? 0 : 1, fmt("%d of %d surfaces with an albedo texture, %d with a normal map; %zu textures decoded", albedo, total,
																		normal, a.textures.size()));
	}

	// Poses: the Skeleton3D's global poses against bf3_anim's model space.
	const float frames[] = { 0.0f, 8.0f, 16.0f, 24.0f, 32.5f, 40.0f, 48.0f, 56.0f, 65.0f };
	double worst_deg = 0.0, worst_m = 0.0;
	std::string per_frame;
	for (float f : frames) {
		std::vector<Xform> model;
		p_demo.pose_at_frame(f, &model);
		const bf3::anim::PoseDiff d = bf3::anim::pose_diff(godot_pose(sk), model);
		worst_deg = std::max(worst_deg, d.max_deg);
		worst_m = std::max(worst_m, d.max_m);
		per_frame += fmt(" %g:%.3g/%.3g", f, d.max_deg, d.max_m * 1000.0);
	}
	add("pose.skeleton3d_vs_bf3_anim", (worst_deg < 0.05 && worst_m < 5e-5) ? 0 : 1,
			fmt("9 frames x 181 bones: max %.4f deg, %.4f mm (frame:deg/mm%s)", worst_deg, worst_m * 1000.0, per_frame.c_str()));

	// Poses against the probe's clip JSON (an independent decode).
	const std::string oracle_dir = env_var("OPENBF3_ANT_ORACLE");
	const std::string oracle = oracle_dir.empty() ? std::string() : oracle_dir + "/m9_ReloadEmpty_Anim.json";
	if (oracle.empty() || !std::filesystem::is_regular_file(std::filesystem::path(std::u8string(oracle.begin(), oracle.end())))) {
		add("pose.probe_json", 3, "OPENBF3_ANT_ORACLE does not name the probe's output folder");
	} else {
		ProbeClip probe;
		if (!load_probe(oracle, probe) || probe.skeleton.size() != rig.size()) {
			add("pose.probe_json", 1, "probe JSON unusable: " + probe.error);
		} else {
			double pd = 0.0, pm = 0.0;
			for (size_t k = 0; k < probe.key_times.size(); ++k) {
				p_demo.pose_at_frame(float(probe.key_times[k]));
				const bf3::anim::PoseDiff d = bf3::anim::pose_diff(godot_pose(sk), probe.model[k]);
				pd = std::max(pd, d.max_deg);
				pm = std::max(pm, d.max_m);
			}
			add("pose.probe_json", (pd < 0.05 && pm < 2e-4) ? 0 : 1,
					fmt("%zu keys x 181 bones, Skeleton3D vs the probe's ReloadEmpty JSON: max %.4f deg, %.4f mm (JSON has 5 decimals)", probe.key_times.size(), pd,
							pm * 1000.0));
		}
	}

	// Skin: Godot's skinning transform (bone global pose * bind pose) against bf3_anim's skin matrices.
	{
		const Ref<Skin> skin = Object::cast_to<MeshInstance3D>(sk->get_child(0)) ? Object::cast_to<MeshInstance3D>(sk->get_child(0))->get_skin() : Ref<Skin>();
		std::vector<Xform> model;
		p_demo.pose_at_frame(24.0f, &model);
		std::vector<bf3::anim::Mat34> ours;
		bf3::anim::skin_matrices(model, a.binds.binds(), ours);
		sk->force_update_all_bone_transforms();
		double worst = 0.0;
		const int32_t n = skin.is_valid() ? skin->get_bind_count() : 0;
		for (int32_t i = 0; i < n && size_t(i) < ours.size(); ++i) {
			const Transform3D godot_skin = sk->get_bone_global_pose(skin->get_bind_bone(i)) * skin->get_bind_pose(i);
			worst = std::max(worst, transform_diff(godot_skin, to_godot(ours[size_t(i)])));
		}
		add("skin.binds", (n == int32_t(a.binds.size()) && n == 74 && worst < 1e-4) ? 0 : 1,
				fmt("%d binds in one Skin shared by arms and M9 (bf3_anim %zu); Godot bone * bind vs bf3_anim skin matrices at frame 24: max %.2e", n,
						a.binds.size(), worst));
	}

	// Motion: hands on the gun, magazine out and back, slide released, weapon in front of the camera.
	const int32_t root = rig.find("Wep_Root"), mag = rig.find("Wep_Mag"), slide = rig.find("Wep_Slide"), rhand = rig.find("RightHand");
	const int32_t lsh = rig.find("LeftForeArm"), rsh = rig.find("RightForeArm");
	{
		std::vector<Xform> m;
		p_demo.pose_at_frame(0.0f, &m);
		const double hand = bf3::anim::vec_dist(m[size_t(rhand)].t, m[size_t(root)].t);
		const Transform3D cam_inv = p_demo.camera()->get_global_transform().affine_inverse();
		const Transform3D to_world = sk->get_global_transform();
		const Vector3 gun_cam = cam_inv.xform(to_world.xform(to_godot(m[size_t(root)].t)));
		const Vector3 lsh_cam = cam_inv.xform(to_world.xform(to_godot(m[size_t(lsh)].t)));
		const Vector3 rsh_cam = cam_inv.xform(to_world.xform(to_godot(m[size_t(rsh)].t)));
		add("pose.grip", hand < 0.15 ? 0 : 1, fmt("frame 0: RightHand %.1f cm from Wep_Root", hand * 100.0));
		const bool in_front = gun_cam.z < -0.15f && gun_cam.z > -0.8f && std::fabs(gun_cam.x) < 0.4f && std::fabs(gun_cam.y) < 0.4f;
		add("camera.weapon_in_front", in_front ? 0 : 1, fmt("frame 0: Wep_Root at camera (%.3f, %.3f, %.3f) m (Godot camera looks down -Z)", gun_cam.x, gun_cam.y, gun_cam.z));
		add("camera.handedness", rsh_cam.x > lsh_cam.x ? 0 : 1,
				fmt("RightForeArm (elbow) at camera x %.3f, LeftForeArm %.3f (right arm on the right: not mirrored)", rsh_cam.x, lsh_cam.x));

		// CPU-skinned M9 vertices (bf3_anim skin matrices) in camera space: the whole pistol is in front of the eye.
		std::vector<bf3::anim::Mat34> skin;
		bf3::anim::skin_matrices(m, a.binds.binds(), skin);
		Vector3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
		for (const bf3::render::PreparedSurface &s : a.weapon.prepared.surfaces) {
			for (size_t v = 0; v < s.vertex_count(); ++v) {
				const bf3::anim::Vec3 p{ s.positions[v * 3], s.positions[v * 3 + 1], s.positions[v * 3 + 2] };
				bf3::anim::Vec3 acc{};
				for (int l = 0; l < 4; ++l) {
					const float w = s.weights[v * 4 + size_t(l)];
					if (w > 0.0f) {
						acc = bf3::anim::vec_add(acc, bf3::anim::vec_scale(bf3::anim::mat_point(skin[size_t(s.bones[v * 4 + size_t(l)])], p), w));
					}
				}
				const Vector3 c = cam_inv.xform(to_world.xform(to_godot(acc)));
				lo = Vector3(std::min(lo.x, c.x), std::min(lo.y, c.y), std::min(lo.z, c.z));
				hi = Vector3(std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z));
			}
		}
		add("camera.m9_vertices", (hi.z < -0.05f && lo.z > -1.0f) ? 0 : 1,
				fmt("frame 0: skinned M9 vertices span camera x %.2f..%.2f, y %.2f..%.2f, z %.2f..%.2f m", lo.x, hi.x, lo.y, hi.y, lo.z, hi.z));
	}
	{
		std::vector<Xform> first, last;
		p_demo.pose_at_frame(0.0f, &first);
		p_demo.pose_at_frame(65.0f, &last);
		double max_mag = 0.0;
		int mag_frame = -1;
		for (int f = 0; f <= 65; ++f) {
			std::vector<Xform> m;
			p_demo.pose_at_frame(float(f), &m);
			const double d = bf3::anim::vec_dist(bf3::anim::xform_mul(bf3::anim::xform_inverse(m[size_t(root)]), m[size_t(mag)]).t, bf3::anim::Vec3{});
			if (d > max_mag) {
				max_mag = d;
				mag_frame = f;
			}
		}
		// The new magazine ends where the empty one started (both seated in the grip).
		const double rest_mag = bf3::anim::vec_dist(bf3::anim::xform_mul(bf3::anim::xform_inverse(last[size_t(root)]), last[size_t(mag)]).t,
				bf3::anim::xform_mul(bf3::anim::xform_inverse(first[size_t(root)]), first[size_t(mag)]).t);
		const double slide_travel = bf3::anim::vec_dist(bf3::anim::xform_mul(bf3::anim::xform_inverse(first[size_t(root)]), first[size_t(slide)]).t,
				bf3::anim::xform_mul(bf3::anim::xform_inverse(last[size_t(root)]), last[size_t(slide)]).t);
		add("motion.magazine", (max_mag > 0.10 && rest_mag < 0.005) ? 0 : 1,
				fmt("Wep_Mag up to %.1f cm from Wep_Root (frame %d, %.2f s); seated again at frame 65 (%.2f mm from its frame-0 place)", max_mag * 100.0,
						mag_frame, mag_frame / 30.0, rest_mag * 1000.0));
		add("motion.slide_release", (slide_travel > 0.02 && slide_travel < 0.06) ? 0 : 1,
				fmt("Wep_Slide moves %.1f mm between frame 0 (locked back) and frame 65 (released)", slide_travel * 1000.0));
		if (a.reload_time > 0.0f) {
			add("timing", 2, fmt("clip %.3f s; WeaponDef empty reload %.2f s, ReloadThreshold %.2f -> %.2f s of clip (probe §5: plausibly the refill point)",
									 a.tracks.duration_seconds(), a.reload_time, a.reload_threshold, a.reload_threshold * a.tracks.duration_seconds()));
		}
	}

	// Install unchanged.
	const bf3::core::InstallSnapshot &before = p_demo.install_snapshot_at_start();
	if (before.valid) {
		const bf3::core::SnapshotDiff d = bf3::core::diff_snapshots(before, p_demo.install().snapshot());
		add("install.unchanged", d.clean() ? 0 : 1, d.clean() ? fmt("%zu files: sizes and mtimes equal", before.files.size()) : d.summary());
	} else {
		add("install.unchanged", 1, "no snapshot at start");
	}
	p_demo.pose_at_frame(0.0f);

	UtilityFunctions::print("");
	UtilityFunctions::print("M9 RELOAD CHECK");
	for (const Row &r : rows) {
		static const char *names[] = { "PASS", "FAIL", "INFO", "NOT RUN" };
		UtilityFunctions::print(to_godot(fmt("  %-7s %-30s %s", names[r.result], r.check.c_str(), r.detail.c_str())));
	}
	UtilityFunctions::print(to_godot(std::string("M9 RELOAD CHECK: ") + (failed ? "FAIL" : "PASS")));
	return failed ? 1 : 0;
}

} // namespace bf3::godot_side
