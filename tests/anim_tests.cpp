// bf3_anim: skeletons, clip sampling to poses, model space and skinning binds.
//
// Suite "anim-unit": synthetic skeletons and clips, no data.
// Suite "anim": the retail install, read-only (Ant banks through AntInstallSource, the EBX skeletons and the
// skinned meshes from Win32/Levels/MP_Subway/MP_Subway); exits 77 (skipped) without an install. With
// OPENBF3_ANT_ORACLE = the Ant probe's output folder it also compares model-space poses built from the probe's
// m9_ReloadEmpty_Anim.json. Asserts numbers only; nothing from BF3 is stored in the repo.
#include <doctest/doctest.h>

#include "bf3/anim/clip_asset.h"
#include "bf3/anim/pose.h"
#include "bf3/anim/pose_math.h"
#include "bf3/anim/skeleton.h"
#include "bf3/anim/skinning.h"
#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/res/res_loader.h"
#include "mini_json.h"
#include "test_support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

using namespace bf3::anim;
using bf3::core::ant::ClipBinding;
using bf3::core::ant::ClipPose;
using bf3::core::ant::DecodedClip;

namespace {

Quat axis_angle(float x, float y, float z, float p_deg) {
	const float h = p_deg * float(std::numbers::pi) / 360.0f;
	const float s = std::sin(h);
	return Quat{ x * s, y * s, z * s, std::cos(h) };
}

bool near_vec(const Vec3 &a, const Vec3 &b, double p_eps = 1e-5) {
	return vec_dist(a, b) < p_eps;
}

// A 3-bone chain: root at origin, child 1 m up X rotated 90 deg about Z, grandchild 1 m along its X.
Skeleton chain() {
	std::vector<Bone> bones(3);
	bones[0].name = "Root";
	bones[1].name = "Mid";
	bones[1].parent = 0;
	bones[1].rest = Xform{ axis_angle(0, 0, 1, 90), Vec3{ 1, 0, 0 } };
	bones[2].name = "Tip";
	bones[2].parent = 1;
	bones[2].rest = Xform{ Quat{}, Vec3{ 1, 0, 0 } };
	return *Skeleton::make(std::move(bones));
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

} // namespace

TEST_SUITE("anim-unit") {
	TEST_CASE("anim: quaternion and transform algebra") {
		const Quat a = axis_angle(0, 1, 0, 90);
		const Vec3 r = quat_rotate(a, Vec3{ 1, 0, 0 });
		CHECK(near_vec(r, Vec3{ 0, 0, -1 })); // +X turns to -Z about +Y
		CHECK(quat_angle_deg(a, Quat{}) == doctest::Approx(90.0).epsilon(1e-5));
		CHECK(quat_angle_deg(a, Quat{ -a.x, -a.y, -a.z, -a.w }) < 1e-3); // q and -q
		const Xform x{ quat_mul(axis_angle(1, 0, 0, 30), axis_angle(0, 0, 1, 50)), Vec3{ 0.3f, -2.0f, 5.0f } };
		const Xform id = xform_mul(x, xform_inverse(x));
		CHECK(quat_angle_deg(id.q, Quat{}) < 1e-3);
		CHECK(near_vec(id.t, Vec3{}));
		// Matrix and quaternion forms agree, and the affine inverse undoes them.
		const Mat34 m = mat_from_xform(x);
		const Vec3 p{ 0.25f, 1.5f, -0.75f };
		CHECK(near_vec(mat_point(m, p), xform_point(x, p)));
		Mat34 inv;
		REQUIRE(mat_inverse(m, inv));
		CHECK(near_vec(mat_point(inv, mat_point(m, p)), p));
		CHECK(quat_angle_deg(quat_from_mat(m), x.q) < 1e-3);
		Mat34 singular;
		singular.m[1] = { 0, 0, 0, 0 };
		CHECK_FALSE(mat_inverse(singular, inv));
		// A Frostbite LinearTransform's rows (right, up, forward, trans) become the matrix columns.
		const Mat34 lt = mat_from_linear_transform({ { { 0, 0, -1 }, { 0, 1, 0 }, { 1, 0, 0 }, { 1, 2, 3 } } });
		CHECK(near_vec(lt.column(0), Vec3{ 0, 0, -1 }));
		CHECK(near_vec(lt.column(2), Vec3{ 1, 0, 0 }));
		CHECK(near_vec(lt.origin(), Vec3{ 1, 2, 3 }));
		CHECK(near_vec(mat_point(lt, Vec3{ 1, 0, 0 }), Vec3{ 1, 2, 2 }));
	}

	TEST_CASE("anim: skeleton order, lookups and model pose") {
		std::vector<Bone> bad(2);
		bad[0].parent = 1; // parent after the bone
		CHECK_FALSE(Skeleton::make(bad).ok());
		const Skeleton s = chain();
		CHECK(s.find("Mid") == 1);
		CHECK(s.find("mid") == -1); // names are case-sensitive, like the data
		CHECK(map_names({ "Tip", "Nope", "Root" }, s) == std::vector<int32_t>{ 2, -1, 0 });
		std::vector<Xform> model;
		model_pose(s, rest_pose(s), model);
		REQUIRE(model.size() == 3);
		CHECK(near_vec(model[1].t, Vec3{ 1, 0, 0 }));
		CHECK(near_vec(model[2].t, Vec3{ 1, 1, 0 })); // the Mid bone's +X points along +Y
		// Rotating the root carries the children.
		LocalPose p = rest_pose(s);
		p.joints[0].q = axis_angle(0, 1, 0, 90);
		model_pose(s, p, model);
		CHECK(near_vec(model[2].t, Vec3{ 0, 1, -1 }));
		const PoseDiff d = pose_diff(model, model);
		CHECK(d.max_deg < 1e-3);
		CHECK(d.max_m < 1e-9);
	}

	TEST_CASE("anim: clip channels drive bones by name") {
		// A two-channel clip (one rotation, one translation) bound to the chain through a fake rig.
		DecodedClip clip;
		clip.num_keys = 2;
		clip.num_quats = 1;
		clip.num_vec3s = 1;
		clip.end_frame = 10;
		clip.key_times = { 0, 10 };
		clip.quats = { Quat{}, axis_angle(0, 0, 1, 90) };
		clip.vec3s = { Vec3{ 1, 0, 0 }, Vec3{ 3, 0, 0 } };
		bf3::core::ant::AntRig rig;
		rig.joints.resize(2);
		rig.joints[0].name = "Tip";
		rig.joints[1].name = "Mid";
		ClipBinding b;
		b.rig = &rig;
		b.fps = 30.0f;
		b.dofs.resize(2);
		b.dofs[0].kind = bf3::core::ant::DofKind::Rotation;
		b.dofs[1].kind = bf3::core::ant::DofKind::Translation;
		b.tracks.resize(2);
		b.tracks[0] = { 0, 'q', 0, 1, true }; // rotation of rig joint 1 = "Mid"
		b.tracks[1] = { 1, 'v', 1, 0, true }; // translation of rig joint 0 = "Tip"
		const Skeleton s = chain();
		bf3::core::Result<ClipTracks> t = bind_clip_tracks(b, clip, s);
		REQUIRE(t.ok());
		CHECK(t->rotation_bone == std::vector<int32_t>{ 1 });
		CHECK(t->translation_bone == std::vector<int32_t>{ 2 });
		CHECK(t->channels_bound == 2);
		CHECK(t->duration_seconds() == doctest::Approx(1.0f / 3.0f));
		LocalPose pose;
		ClipPose scratch;
		REQUIRE(sample_pose(s, clip, *t, 5.0f, pose, scratch)); // halfway: Mid at 45 deg, Tip at 2 m
		CHECK(quat_angle_deg(pose.joints[1].q, axis_angle(0, 0, 1, 45)) < 1e-3);
		CHECK(near_vec(pose.joints[2].t, Vec3{ 2, 0, 0 }));
		CHECK(near_vec(pose.joints[0].t, Vec3{})); // undriven bones keep the rest pose
		// A scale DOF is not applied; a track count mismatch is an error.
		b.dofs[1].kind = bf3::core::ant::DofKind::Scale;
		t = bind_clip_tracks(b, clip, s);
		REQUIRE(t.ok());
		CHECK(t->translation_bone == std::vector<int32_t>{ -1 });
		b.tracks.pop_back();
		CHECK_FALSE(bind_clip_tracks(b, clip, s).ok());
		b.rig = nullptr;
		CHECK_FALSE(bind_clip_tracks(b, clip, s).ok());
	}

	TEST_CASE("anim: skin binds by name; skin matrices are identity at the bind pose") {
		const Skeleton s = chain();
		std::vector<Xform> model;
		model_pose(s, rest_pose(s), model);
		// The mesh skeleton lists the same bones in another order, with the chain's model pose as bind pose.
		EbxSkeleton ebx;
		ebx.partition = "test/skeleton";
		ebx.bone_names = { "Tip", "Root", "Mid", "Extra" };
		ebx.parents = { -1, -1, -1, -1 };
		ebx.model_pose = { mat_from_xform(model[2]), mat_from_xform(model[0]), mat_from_xform(model[1]), Mat34{} };
		ebx.local_pose = ebx.model_pose;
		SkinBinds binds;
		bf3::core::Result<int32_t> b0 = binds.bind(ebx, 0, s);
		REQUIRE(b0.ok());
		CHECK(*b0 == 0);
		CHECK(binds.binds()[0].bone == 2);
		CHECK(*binds.bind(ebx, 2, s) == 1);
		CHECK(*binds.bind(ebx, 0, s) == 0); // shared, not added twice
		CHECK(binds.size() == 2);
		CHECK_FALSE(binds.bind(ebx, 3, s).ok()); // no bone "Extra" in the posed skeleton
		CHECK_FALSE(binds.bind(ebx, 9, s).ok()); // out of range
		std::vector<Mat34> skin;
		skin_matrices(model, binds.binds(), skin);
		for (const Mat34 &m : skin) {
			CHECK(mat_max_diff(m, Mat34{}) < 1e-5f);
		}
		// Bending Mid moves a vertex bound to Tip with the bone.
		LocalPose p = rest_pose(s);
		p.joints[1].q = Quat{};
		model_pose(s, p, model);
		skin_matrices(model, binds.binds(), skin);
		CHECK(near_vec(mat_point(skin[0], Vec3{ 1, 1, 0 }), Vec3{ 2, 0, 0 }));
	}
}

// =============================================================================================
namespace {

constexpr const char *kSubway = "Win32/Levels/MP_Subway/MP_Subway";
constexpr const char *kArmsMesh = "characters/arms/arms1p_bareglove03/arms1p_bareglove03_mesh";
constexpr const char *kM9Mesh = "weapons/m9/m9_1p_mesh";

struct AnimData {
	std::shared_ptr<bf3::core::Install> install;
	std::unique_ptr<bf3::core::ant::AntInstallSource> source;
	std::unique_ptr<bf3::core::ant::AntLibrary> library;
	std::unique_ptr<bf3::core::EbxLibrary> ebx;
	LoadedClip reload, idle, undeploy;
	Skeleton rig;
	EbxSkeleton venice1p, weapon;
	bool ok = false;
	std::string why;
};

AnimData *anim_data() {
	static AnimData *data = [] {
		auto *d = new AnimData();
		bf3::core::InstallReader reader;
		if (!reader.locate({})) {
			d->why = reader.describe();
			return d;
		}
		bf3::core::Result<std::shared_ptr<bf3::core::Install>> install = bf3::core::Install::open(reader);
		REQUIRE_MESSAGE(install.ok(), (install.ok() ? "" : install.error().message));
		d->install = *install;
		auto source = bf3::core::ant::AntInstallSource::open(d->install);
		REQUIRE_MESSAGE(source.ok(), (source.ok() ? "" : source.error().message));
		d->source = std::move(*source);
		auto lib = load_ant_library(*d->source);
		REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? "" : lib.error().message));
		d->library = std::move(*lib);
		for (auto [slot, name] : { std::pair{ &d->reload, "ReloadEmpty Anim" }, std::pair{ &d->idle, "IdlePose Anim" }, std::pair{ &d->undeploy, "Undeploy Anim" } }) {
			auto p = load_ant_package(*d->source, "m9");
			REQUIRE(p.ok());
			auto c = load_clip(std::move(*p), *d->library, name);
			REQUIRE_MESSAGE(c.ok(), (c.ok() ? "" : c.error().message));
			*slot = std::move(*c);
		}
		auto rig = skeleton_from_rig(*d->reload.binding.rig);
		REQUIRE_MESSAGE(rig.ok(), (rig.ok() ? "" : rig.error().message));
		d->rig = std::move(*rig);
		auto ebx = bf3::core::EbxLibrary::open(d->install, { kSubway });
		REQUIRE_MESSAGE(ebx.ok(), (ebx.ok() ? "" : ebx.error().message));
		d->ebx = std::move(*ebx);
		for (auto [slot, name] : { std::pair{ &d->venice1p, "animations/skeletons/venice1pske01" }, std::pair{ &d->weapon, "animations/skeletons/weapon/weaponske01" } }) {
			const bf3::core::EbxPartition *part = d->ebx->partition_by_name(name);
			REQUIRE_MESSAGE(part != nullptr, name);
			auto s = read_ebx_skeleton(*part, name);
			REQUIRE_MESSAGE(s.ok(), (s.ok() ? "" : s.error().message));
			*slot = std::move(*s);
		}
		d->ok = true;
		return d;
	}();
	return data;
}

AnimData *anim_or_skip() {
	AnimData *d = anim_data();
	if (!d->ok) {
		bf3::test::skip_install_case(d->why.c_str());
		return nullptr;
	}
	return d;
}

std::vector<Xform> clip_model(const AnimData &p_d, const LoadedClip &p_clip, float p_frame) {
	const ClipTracks tracks = *bind_clip_tracks(p_clip.binding, p_clip.clip, p_d.rig);
	LocalPose pose;
	ClipPose scratch;
	sample_pose(p_d.rig, p_clip.clip, tracks, p_frame, pose, scratch);
	std::vector<Xform> model;
	model_pose(p_d.rig, pose, model);
	return model;
}

} // namespace

TEST_SUITE("anim") {
	TEST_CASE("anim: the 1P rig skeleton and the ReloadEmpty tracks") {
		AnimData *d = anim_or_skip();
		if (!d) {
			return;
		}
		CHECK(d->rig.size() == 181);
		CHECK(d->rig.find("Wep_Root") == 154);
		CHECK(d->rig.bone(size_t(d->rig.find("Wep_Root"))).parent == d->rig.find("SpineX"));
		const ClipTracks t = *bind_clip_tracks(d->reload.binding, d->reload.clip, d->rig);
		CHECK(t.channels_bound == 135);
		CHECK(t.channels_unbound == 0);
		CHECK(t.fps == 30.0f);
		CHECK(t.end_frame == 65);
		CHECK(t.duration_seconds() == doctest::Approx(65.0f / 30.0f));
		CHECK(d->venice1p.size() == 179);
		CHECK(d->weapon.size() == 25);
		CHECK(d->weapon.bone_names[1] == "Wep_Root");
		// Every EBX bone of both mesh skeletons names a rig joint, except the two ankle effector aux joints
		// the README lists the other way round (rig joints without an EBX bone).
		const std::vector<int32_t> v = map_names(d->venice1p.bone_names, d->rig);
		const std::vector<int32_t> w = map_names(d->weapon.bone_names, d->rig);
		CHECK(std::count(v.begin(), v.end(), -1) == 0);
		CHECK(std::count(w.begin(), w.end(), -1) == 1); // exportAnimation
		CHECK(w[0] == -1);
	}

	TEST_CASE("anim: M9 Undeploy frame 0 equals IdlePose in model space") {
		AnimData *d = anim_or_skip();
		if (!d) {
			return;
		}
		const std::vector<Xform> idle = clip_model(*d, d->idle, 0.0f);
		const std::vector<Xform> undeploy = clip_model(*d, d->undeploy, 0.0f);
		const PoseDiff diff = pose_diff(idle, undeploy);
		MESSAGE("Undeploy frame 0 vs IdlePose, 181 joints in model space: max " << diff.max_deg << " deg (" << d->rig.bone(size_t(diff.worst_rotation_bone)).name
																			  << "), " << diff.max_m * 1000.0 << " mm ("
																			  << d->rig.bone(size_t(diff.worst_translation_bone)).name << "); probe (local): 0.07 deg, 0.4 mm");
		CHECK(diff.max_deg < 0.25);
		CHECK(diff.max_m < 0.001);
	}

	TEST_CASE("anim: rig rest pose against venice1pske01 LocalPose") {
		AnimData *d = anim_or_skip();
		if (!d) {
			return;
		}
		uint32_t rot_ok = 0, trans_ok = 0, compared = 0;
		for (size_t i = 0; i < d->venice1p.size(); ++i) {
			const int32_t j = d->rig.find(d->venice1p.bone_names[i]);
			if (j < 0) {
				continue;
			}
			++compared;
			const Xform &rest = d->rig.bone(size_t(j)).rest;
			rot_ok += quat_angle_deg(rest.q, quat_from_mat(d->venice1p.local_pose[i])) < 0.5 ? 1 : 0;
			trans_ok += vec_dist(rest.t, d->venice1p.local_pose[i].origin()) < 0.001 ? 1 : 0;
		}
		MESSAGE("rig rest vs venice1pske01 LocalPose: " << compared << " bones, rotation within 0.5 deg " << rot_ok << ", translation within 1 mm " << trans_ok
														<< " (probe: 168 / 167)");
		CHECK(compared == 179);
		CHECK(rot_ok >= 168);
		CHECK(trans_ok >= 167);
	}

	TEST_CASE("anim: arms and M9 meshes skin to the rig by name") {
		AnimData *d = anim_or_skip();
		if (!d) {
			return;
		}
		const bf3::core::LevelMount &mount = d->ebx->mount(0);
		SkinBinds binds;
		auto arms = bf3::core::res::load_mesh_set(mount, kArmsMesh);
		REQUIRE_MESSAGE(arms.ok(), (arms.ok() ? "" : arms.error().message));
		auto m9 = bf3::core::res::load_mesh_set(mount, kM9Mesh);
		REQUIRE_MESSAGE(m9.ok(), (m9.ok() ? "" : m9.error().message));
		auto arm_lanes = binds.bind_lod(arms->mesh.lods[0], d->venice1p, d->rig);
		REQUIRE_MESSAGE(arm_lanes.ok(), (arm_lanes.ok() ? "" : arm_lanes.error().message));
		const size_t arm_binds = binds.size();
		auto m9_lanes = binds.bind_lod(m9->mesh.lods[0], d->weapon, d->rig);
		REQUIRE_MESSAGE(m9_lanes.ok(), (m9_lanes.ok() ? "" : m9_lanes.error().message));
		MESSAGE("binds: arms " << arm_binds << ", M9 " << binds.size() - arm_binds << " (LOD bone tables " << arms->mesh.lods[0].bones.size() << " / "
							   << m9->mesh.lods[0].bones.size() << ")");
		CHECK(arm_binds == arms->mesh.lods[0].bones.size());
		CHECK(binds.size() - arm_binds == m9->mesh.lods[0].bones.size());
		// The M9's bone table names Wep_Root, Wep_Trigger, Wep_Slide, Wep_Mag and Wep_Physic1 (hash 0xa6d64831).
		std::vector<std::string> m9_names;
		for (size_t i = arm_binds; i < binds.size(); ++i) {
			m9_names.push_back(binds.binds()[i].name);
		}
		std::sort(m9_names.begin(), m9_names.end());
		CHECK(m9_names == std::vector<std::string>{ "Wep_Mag", "Wep_Physic1", "Wep_Root", "Wep_Slide", "Wep_Trigger" });
		CHECK(bf3::core::res::hash_quick("wep_physic1") == 0xa6d64831u);

		// At the rig's rest pose every skin matrix of a mesh is (nearly) the same rigid transform: the offset
		// between the rig's model space and the mesh skeleton's (the rig's AITrajectory turns by 90 deg).
		std::vector<Xform> model;
		model_pose(d->rig, rest_pose(d->rig), model);
		std::vector<Mat34> skin;
		skin_matrices(model, binds.binds(), skin);
		auto spread = [&](size_t p_begin, size_t p_end, const Mat34 &p_ref) {
			float worst = 0.0f;
			uint32_t within = 0;
			for (size_t i = p_begin; i < p_end; ++i) {
				const float m = mat_max_diff(skin[i], p_ref);
				worst = std::max(worst, m);
				within += m < 0.01f ? 1 : 0;
			}
			return std::pair{ worst, within };
		};
		const Mat34 arms_ref = skin[size_t(binds.bind(d->venice1p, uint32_t(d->venice1p.find("RightHand")), d->rig).value())];
		const auto [arm_worst, arm_within] = spread(0, arm_binds, arms_ref);
		MESSAGE("arms skin matrices at rig rest: " << arm_within << " of " << arm_binds << " within 0.01 of RightHand's, worst " << arm_worst
												   << "; RightHand's turn: columns x (" << arms_ref.m[0][0] << ", " << arms_ref.m[1][0] << ", " << arms_ref.m[2][0] << ")");
		CHECK(arm_within >= arm_binds - 8);
	}

	TEST_CASE("anim: ReloadEmpty on the rig: hands on the gun, magazine out and back, loop closes") {
		AnimData *d = anim_or_skip();
		if (!d) {
			return;
		}
		const int32_t root = d->rig.find("Wep_Root"), mag = d->rig.find("Wep_Mag"), right = d->rig.find("RightHand"), left = d->rig.find("LeftHand");
		const int32_t slide = d->rig.find("Wep_Slide"), cam = d->rig.find("CameraJoint");
		REQUIRE(root >= 0);
		REQUIRE(mag >= 0);
		double max_mag = 0.0, max_slide = 0.0;
		int mag_out_frame = -1;
		for (int f = 0; f <= 65; ++f) {
			const std::vector<Xform> m = clip_model(*d, d->reload, float(f));
			const Xform in_gun = xform_mul(xform_inverse(m[size_t(root)]), m[size_t(mag)]);
			const double mag_m = vec_dist(in_gun.t, Vec3{});
			if (mag_m > max_mag) {
				max_mag = mag_m;
				mag_out_frame = f;
			}
			max_slide = std::max(max_slide, vec_dist(xform_mul(xform_inverse(m[size_t(root)]), m[size_t(slide)]).t, Vec3{}));
			if (f == 0 || f == 65) {
				const double hand = vec_dist(m[size_t(right)].t, m[size_t(root)].t);
				const double eye = vec_dist(m[size_t(cam)].t, m[size_t(root)].t);
				MESSAGE("frame " << f << ": RightHand-Wep_Root " << hand * 100.0 << " cm, CameraJoint-Wep_Root " << eye * 100.0 << " cm, LeftHand-Wep_Root "
								 << vec_dist(m[size_t(left)].t, m[size_t(root)].t) * 100.0 << " cm");
				CHECK(hand < 0.15); // the right hand holds the grip
				CHECK(eye > 0.15); // the gun is in front of the eye, not inside it
				CHECK(eye < 0.8);
			}
		}
		MESSAGE("Wep_Mag leaves Wep_Root by up to " << max_mag * 100.0 << " cm (frame " << mag_out_frame << "); Wep_Slide by up to " << max_slide * 100.0 << " cm");
		CHECK(max_mag > 0.10);
		// The empty reload starts with the slide locked back and ends with it released: everything else closes the loop.
		const std::vector<Xform> first = clip_model(*d, d->reload, 0.0f), last = clip_model(*d, d->reload, 65.0f);
		std::vector<bool> mask(d->rig.size(), true);
		mask[size_t(slide)] = false;
		const PoseDiff loop = pose_diff(first, last, mask);
		const double slide_travel = vec_dist(xform_mul(xform_inverse(first[size_t(root)]), first[size_t(slide)]).t,
				xform_mul(xform_inverse(last[size_t(root)]), last[size_t(slide)]).t);
		MESSAGE("frame 0 vs 65 in model space without Wep_Slide: " << loop.max_deg << " deg (" << d->rig.bone(size_t(loop.worst_rotation_bone)).name << "), "
																   << loop.max_m * 1000.0 << " mm (" << d->rig.bone(size_t(loop.worst_translation_bone)).name
																   << "); slide released by " << slide_travel * 1000.0 << " mm");
		CHECK(loop.max_deg < 2.0);
		CHECK(loop.max_m < 0.005);
		CHECK(slide_travel > 0.02);
		CHECK(slide_travel < 0.06);
	}

	TEST_CASE("anim: ReloadEmpty model-space poses equal the probe's clip JSON") {
		AnimData *d = anim_or_skip();
		if (!d) {
			return;
		}
		const std::string dir = env_var("OPENBF3_ANT_ORACLE");
		const std::filesystem::path file = std::filesystem::path(dir) / "m9_ReloadEmpty_Anim.json";
		if (dir.empty() || !std::filesystem::is_regular_file(file)) {
			MESSAGE("NOT RUN: OPENBF3_ANT_ORACLE does not name the probe's output directory");
			return;
		}
		std::ifstream in(file, std::ios::binary);
		std::stringstream ss;
		ss << in.rdbuf();
		const std::string text = ss.str();
		bf3::tools::json::Value root;
		REQUIRE(bf3::tools::json::Parser(text).parse(root));
		// The probe's own skeleton (joints: parent, t, q) and per-key local tracks -> model space.
		const bf3::tools::json::Value *joints = root.get("joints"), *tracks = root.get("tracks"), *keys = root.get("keyTimes");
		REQUIRE(joints != nullptr);
		REQUIRE(tracks != nullptr);
		REQUIRE(keys != nullptr);
		std::vector<Bone> bones;
		for (const auto &j : joints->items) {
			Bone b;
			b.name = j.string_or("name", "");
			b.parent = int32_t(j.number_or("parent", -1));
			const auto &t = j.get("t")->items;
			const auto &q = j.get("q")->items;
			b.rest = Xform{ Quat{ float(q[0].num), float(q[1].num), float(q[2].num), float(q[3].num) }, Vec3{ float(t[0].num), float(t[1].num), float(t[2].num) } };
			bones.push_back(std::move(b));
		}
		auto probe = Skeleton::make(std::move(bones));
		REQUIRE(probe.ok());
		CHECK(probe->size() == d->rig.size());
		double worst_deg = 0.0, worst_m = 0.0;
		const ClipTracks t = *bind_clip_tracks(d->reload.binding, d->reload.clip, d->rig);
		for (size_t k = 0; k < keys->items.size(); ++k) {
			LocalPose pose = rest_pose(*probe);
			for (const auto &tr : tracks->items) {
				const int32_t j = int32_t(tr.number_or("joint", -1));
				const auto &v = tr.get("values")->items[k].items;
				if (tr.string_or("kind", "") == "q") {
					pose.joints[size_t(j)].q = Quat{ float(v[0].num), float(v[1].num), float(v[2].num), float(v[3].num) };
				} else {
					pose.joints[size_t(j)].t = Vec3{ float(v[0].num), float(v[1].num), float(v[2].num) };
				}
			}
			std::vector<Xform> expected;
			model_pose(*probe, pose, expected);
			const std::vector<Xform> ours = clip_model(*d, d->reload, float(keys->items[k].num));
			const PoseDiff diff = pose_diff(ours, expected);
			worst_deg = std::max(worst_deg, diff.max_deg);
			worst_m = std::max(worst_m, diff.max_m);
		}
		MESSAGE("ReloadEmpty vs probe JSON, " << keys->items.size() << " keys x 181 joints in model space: max " << worst_deg << " deg, " << worst_m * 1000.0
											  << " mm (the JSON has 5 decimals)");
		CHECK(worst_deg < 0.05);
		CHECK(worst_m < 0.0002);
	}
}
