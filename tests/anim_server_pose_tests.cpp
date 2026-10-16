// M2a lc: the server pose of the hit capsules (docs/design/m2a.md §6.5, E18 (a); as built: docs/design/m2a-lc.md).
//
// Suite "anim-server-pose" (ctest anim.server_pose): the retail install, read-only (the level's capsules, SoldierDef and
// material table through bf3_sim's weapon_install.h; the clips through AntInstallSource); exits 77 without one.
//   * the table: 9 capsules mapped to rig joints, every row tagged (0 stand-in rows expected), the aim per stance, the
//     structural checks, the hash, the capsules/ blob round trip, a second build bit-equal, plausible stand-idle heights;
//   * E18 (a): posed_target from the table against the reference evaluation (bf3/anim/server_pose_reference.h: per
//     query sample_pose -> apply_additive at the exact pitch weight -> model_pose, capsules placed in double; no
//     buckets, no fraction steps) at the 32 mid-pitches (steady rows, 3 stances) and 15 mid-fractions x 32 mid-pitches
//     (6 transition rows): head and chest capsule centres within 3 cm (steady) and 5 cm (transitions); body and legs
//     reported;
//   * E18 (a): the stand rows against S3's CrowdEvaluator (its own clip set, loco blend and layers) within 3 cm; a run
//     soldier against the stand table reported (locomotion is not modelled);
//   * install.unchanged.
// Asserts numbers only; nothing from BF3 is stored in the repo.
#include <doctest/doctest.h>

#include "bf3/anim/clip_asset.h"
#include "bf3/anim/crowd_clips.h"
#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/server_pose.h"
#include "bf3/anim/server_pose_reference.h"
#include "bf3/anim/skinning.h"
#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/gameplay/capsule_pose_table.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/sim/weapons/posed_target.h"
#include "bf3/sim/weapons/weapon_install.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace bf3;
namespace an = bf3::anim;
namespace gp = bf3::core::gameplay;
namespace sw = bf3::sim::weapons;
namespace ref = bf3::anim::server_pose_reference;

namespace {

constexpr double kDeg = 3.14159265358979323846 / 180.0;
constexpr double kSteadyGateCm = 3.0; // E18 [inferred threshold, m2a.md §16 Q14]
constexpr double kTransitionGateCm = 5.0;
const char *kStance[3] = { "stand", "crouch", "prone" };
const char *kTransition[6] = { "stand->crouch", "stand->prone", "crouch->stand", "crouch->prone", "prone->stand", "prone->crouch" };
constexpr uint8_t kFrom[6] = { 0, 0, 1, 1, 2, 2 };
constexpr uint8_t kTo[6] = { 1, 2, 0, 2, 0, 1 };

struct PoseData {
	bool ok = false;
	std::string why;
	core::InstallReader reader;
	core::InstallSnapshot before;
	std::shared_ptr<core::Install> install;
	std::unique_ptr<core::ant::AntInstallSource> source;
	std::unique_ptr<core::ant::AntLibrary> library;
	sw::InstallWeapons weapons;
	sw::CapsuleTableData data;
	an::ServerPoseClips clips;
	an::CapsuleTableInputs inputs;
	an::CapsuleTableReport report;
	gp::CapsulePoseTable table;
};

PoseData *pose_data() {
	static PoseData *data = [] {
		auto *d = new PoseData();
		if (!d->reader.locate({})) {
			d->why = d->reader.describe();
			return d;
		}
		d->before = d->reader.snapshot();
		auto install = core::Install::open(d->reader);
		REQUIRE_MESSAGE(install.ok(), (install.ok() ? "" : install.error().message));
		d->install = *install;
		auto weapons = sw::open_install_weapons(d->install);
		REQUIRE_MESSAGE(weapons.ok(), (weapons.ok() ? "" : weapons.error().message));
		d->weapons = std::move(*weapons);
		auto cd = sw::capsule_table_data(d->weapons.soldier, d->weapons.capsules, *d->weapons.materials);
		REQUIRE_MESSAGE(cd.ok(), (cd.ok() ? "" : cd.error().message));
		d->data = std::move(*cd);
		auto source = core::ant::AntInstallSource::open(d->install);
		REQUIRE_MESSAGE(source.ok(), (source.ok() ? "" : source.error().message));
		d->source = std::move(*source);
		auto lib = an::load_ant_library(*d->source);
		REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? "" : lib.error().message));
		d->library = std::move(*lib);
		auto clips = an::load_server_pose_clips(*d->source, *d->library);
		REQUIRE_MESSAGE(clips.ok(), (clips.ok() ? "" : clips.error().message));
		d->clips = std::move(*clips);
		d->inputs.capsules = d->weapons.capsules;
		d->inputs.property = d->data.property;
		d->inputs.zone = d->data.zone;
		d->inputs.standin = d->data.standin;
		auto table = an::build_capsule_pose_table(d->clips.rig, d->clips, d->inputs, &d->report);
		REQUIRE_MESSAGE(table.ok(), (table.ok() ? "" : table.error().message));
		d->table = std::move(*table);
		d->ok = true;
		return d;
	}();
	return data;
}

PoseData *data_or_skip() {
	PoseData *d = pose_data();
	if (!d->ok) {
		test::skip_install_case(d->why.c_str());
		return nullptr;
	}
	return d;
}

std::vector<double> mid_pitches(const gp::CapsulePoseTable &p_table) {
	std::vector<double> m;
	for (size_t i = 0; i + 1 < p_table.pitch_deg.size(); ++i) {
		m.push_back(0.5 * (double(p_table.pitch_deg[i]) + double(p_table.pitch_deg[i + 1])));
	}
	return m;
}

sw::HitTarget posed_local(const gp::CapsulePoseTable &p_table, uint8_t p_from, uint8_t p_to, double p_fraction, double p_pitch_deg) {
	sw::TargetPose tp;
	tp.pose = sim::Pose(p_from);
	tp.target_pose = sim::Pose(p_to);
	tp.transition = float(p_fraction);
	tp.pitch = float(p_pitch_deg * kDeg);
	return sw::posed_target(p_table, tp);
}

struct Dev {
	double head = 0, chest = 0, body = 0, legs = 0; // cm, max over the queries
	size_t queries = 0;
	size_t failed = 0; // queries the reference could not evaluate
};

void accumulate(Dev &r_dev, const sw::HitTarget &p_posed, const std::vector<ref::RefCapsule> &p_ref) {
	REQUIRE(p_posed.capsule_count == p_ref.size());
	for (size_t c = 0; c < p_ref.size(); ++c) {
		const sw::ZoneCapsule &z = p_posed.capsules[c];
		CHECK(size_t(z.capsule) == p_ref[c].list_index);
		double d2 = 0;
		for (size_t k = 0; k < 3; ++k) {
			const double pc = 0.5 * (double(z.a[k]) + double(z.b[k]));
			d2 += (pc - p_ref[c].centre[k]) * (pc - p_ref[c].centre[k]);
		}
		const double cm = std::sqrt(d2) * 100.0;
		double &slot = z.zone == sw::HitZone::Head ? r_dev.head : z.zone == sw::HitZone::Chest ? r_dev.chest : z.zone == sw::HitZone::Legs ? r_dev.legs : r_dev.body;
		slot = std::max(slot, cm);
	}
	++r_dev.queries;
}

const char *tag(uint8_t p_source) {
	return p_source == gp::capsule_row_source::kClipDerived ? "clip-derived" : "stand-in";
}

} // namespace

TEST_SUITE("anim-server-pose") {
	TEST_CASE("server pose: the table from the install (capsules, joints, tags, aim, hash, blob, determinism)") {
		PoseData *d = data_or_skip();
		if (d == nullptr) {
			return;
		}
		const gp::CapsulePoseTable &t = d->table;
		const an::CapsuleTableReport &r = d->report;
		MESSAGE("capsule pose table: " << unsigned(t.capsule_count) << " capsules, hash " << t.hash << ", bound " << t.bound_radius << " m, build " << r.seconds
										<< " s, " << r.pose_evaluations << " pose evaluations");
		for (const std::string &n : d->clips.notes) {
			MESSAGE("clips note: " << n);
		}
		for (const std::string &n : r.notes) {
			MESSAGE("build note: " << n);
		}
		CHECK(t.capsule_count == 9);
		CHECK(gp::capsule_pose_table_valid(t));
		CHECK(t.hash == gp::capsule_pose_table_hash(t));
		// Every role's clip is present (verified with antdump list: exactly once each), every capsule maps to a joint.
		for (size_t k = 0; k < an::kServerPoseClips; ++k) {
			CHECK_MESSAGE(d->clips.present[k], an::ServerPoseClipNames{}.controllers[k]);
		}
		REQUIRE(r.joint.size() == t.capsule_count);
		for (size_t c = 0; c < r.joint.size(); ++c) {
			const gp::HitCapsule &hc = d->weapons.capsules[t.steady[c].capsule];
			CHECK_MESSAGE(r.joint[c] >= 0, hc.bone);
			MESSAGE("capsule " << c << " " << hc.bone << " -> joint " << r.joint[c] << " zone " << std::string(sw::hit_zone_name(sw::HitZone(t.steady[c].zone))) << " r "
							   << hc.radius << " len " << hc.length << " axis " << hc.bone_axis << " sign " << an::capsule_axis_sign(d->clips.rig, r.joint[c], hc.bone_axis));
		}
		// Row tags: printed for E18, 0 stand-in rows expected on the retail install.
		size_t standins = 0;
		for (size_t row = 0; row < gp::kCapsuleRows; ++row) {
			MESSAGE("row " << std::string(row < 3 ? kStance[row] : kTransition[row - 3]) << ": " << std::string(tag(t.row_source[row])));
			standins += t.row_source[row] == gp::capsule_row_source::kClipDerived ? 0 : 1;
		}
		CHECK(standins == 0);
		// Aim per stance: stand takes S3's measured values; crouch and prone are measured (a fallback is reported).
		const an::CrowdConfig aim;
		CHECK(r.aim[0].up_full_deg == aim.aim_up_full_pitch_deg);
		CHECK(r.aim[0].down_full_deg == aim.aim_down_full_pitch_deg);
		for (size_t s = 0; s < 3; ++s) {
			MESSAGE("aim " << std::string(kStance[s]) << ": up " << r.aim[s].up_full_deg << " down " << r.aim[s].down_full_deg << std::string(r.aim[s].measured ? " (measured)" : " (CrowdConfig)"));
		}
		for (size_t s = 1; s < 3; ++s) {
			if (!r.aim[s].measured) {
				MESSAGE("aim " << std::string(kStance[s]) << ": not measured, the stand values are used (reported, docs/design/m2a-lc.md)");
			}
		}
		// Pitch buckets: 0 is a bucket, the ends are the stand's full-aim pitches.
		CHECK(t.pitch_deg[an::kCapsuleDownBuckets] == 0.0f);
		CHECK(t.pitch_deg.front() == doctest::Approx(-aim.aim_down_full_pitch_deg));
		CHECK(t.pitch_deg.back() == doctest::Approx(aim.aim_up_full_pitch_deg));
		// Plausible stand idle at pitch 0 (frame check of the [inferred] placement: head ~1.5-1.8 m, chest above the
		// hips, legs lowest, the body inside a 1 m column about the feet).
		const size_t zero = an::kCapsuleDownBuckets;
		double head_y = 0, chest_y = 0, legs_min = 10, max_r = 0;
		for (size_t c = 0; c < t.capsule_count; ++c) {
			const gp::CapsuleLocal &e = t.steady[t.steady_index(0, zero) + c];
			const double cy = 0.5 * (double(e.a[1]) + double(e.b[1]));
			const double cx = 0.5 * (double(e.a[0]) + double(e.b[0])), cz = 0.5 * (double(e.a[2]) + double(e.b[2]));
			max_r = std::max(max_r, std::sqrt(cx * cx + cz * cz));
			if (e.zone == uint8_t(sw::HitZone::Head)) {
				head_y = cy;
			} else if (e.zone == uint8_t(sw::HitZone::Chest)) {
				chest_y = cy;
			} else if (e.zone == uint8_t(sw::HitZone::Legs)) {
				legs_min = std::min(legs_min, std::min(double(e.a[1]), double(e.b[1])));
			}
		}
		MESSAGE("stand idle pitch 0: head centre y " << head_y << " m, chest centre y " << chest_y << " m, lowest leg end y " << legs_min << " m, max horizontal "
													 << max_r << " m");
		CHECK(head_y > 1.4);
		CHECK(head_y < 1.9);
		CHECK(chest_y > 1.0);
		CHECK(chest_y < head_y);
		CHECK(legs_min < 0.6);
		CHECK(max_r < 1.0);
		// The capsules/ blob round trip, and a second build from the same inputs gives the same bits.
		const std::vector<uint8_t> blob = gp::encode_capsule_pose_table(t);
		gp::CapsulePoseTable back;
		std::string err;
		REQUIRE_MESSAGE(gp::decode_capsule_pose_table(blob, back, &err), err);
		CHECK(back.hash == t.hash);
		CHECK(gp::capsule_pose_table_hash(back) == t.hash);
		auto again = an::build_capsule_pose_table(d->clips.rig, d->clips, d->inputs);
		REQUIRE(again.ok());
		CHECK(again->hash == t.hash);
		CHECK(an::capsule_table_source_hash(d->clips.rig, d->clips, d->inputs) == an::capsule_table_source_hash(d->clips.rig, d->clips, d->inputs));
	}

	TEST_CASE("E18 (a): steady and transition rows against the reference evaluation (off-grid)") {
		PoseData *d = data_or_skip();
		if (d == nullptr) {
			return;
		}
		const gp::CapsulePoseTable &t = d->table;
		auto r = ref::Reference::make(d->clips, d->weapons.capsules, d->report.aim, an::CrowdConfig{}.aim_order);
		REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
		REQUIRE(r->capsule_count() == t.capsule_count);
		const std::vector<double> pitches = mid_pitches(t);
		REQUIRE(pitches.size() == 32);
		std::vector<ref::RefCapsule> rc;
		for (uint8_t s = 0; s < 3; ++s) {
			Dev dev;
			for (const double p : pitches) {
				ref::Query q;
				q.row = s;
				q.pitch_deg = p;
				if (!r->evaluate(q, rc)) {
					++dev.failed;
					continue;
				}
				accumulate(dev, posed_local(t, s, s, 0, p), rc);
			}
			std::printf("anim.server_pose E18 steady %-6s [%s] %zu queries: head %.3f cm chest %.3f cm (gate %.0f cm) body %.3f cm legs %.3f cm (reported)\n", kStance[s],
					tag(t.row_source[s]), dev.queries, dev.head, dev.chest, kSteadyGateCm, dev.body, dev.legs);
			CHECK(dev.failed == 0);
			CHECK(dev.queries == 32);
			CHECK_MESSAGE(dev.head <= kSteadyGateCm, std::string(kStance[s]) << " head " << dev.head << " cm");
			CHECK_MESSAGE(dev.chest <= kSteadyGateCm, std::string(kStance[s]) << " chest " << dev.chest << " cm");
		}
		for (uint8_t tr = 0; tr < 6; ++tr) {
			Dev dev;
			for (size_t f = 0; f + 1 < gp::kCapsuleTransitionFractions; ++f) {
				const double frac = (double(f) + 0.5) / double(gp::kCapsuleTransitionFractions - 1);
				for (const double p : pitches) {
					ref::Query q;
					q.transition = true;
					q.row = tr;
					q.pitch_deg = p;
					q.fraction = frac;
					if (!r->evaluate(q, rc)) {
						++dev.failed;
						continue;
					}
					accumulate(dev, posed_local(t, kFrom[tr], kTo[tr], frac, p), rc);
				}
			}
			std::printf("anim.server_pose E18 transition %-14s [%s] %zu queries: head %.3f cm chest %.3f cm (gate %.0f cm) body %.3f cm legs %.3f cm (reported)\n",
					kTransition[tr], tag(t.row_source[3 + tr]), dev.queries, dev.head, dev.chest, kTransitionGateCm, dev.body, dev.legs);
			CHECK(dev.failed == 0);
			CHECK(dev.queries == (gp::kCapsuleTransitionFractions - 1) * 32);
			CHECK_MESSAGE(dev.head <= kTransitionGateCm, std::string(kTransition[tr]) << " head " << dev.head << " cm");
			CHECK_MESSAGE(dev.chest <= kTransitionGateCm, std::string(kTransition[tr]) << " chest " << dev.chest << " cm");
		}
		// The on-grid points reproduce the reference to float rounding (the builder and the reference agree where the
		// table has an entry): a check on the shared rules, not a gate row.
		Dev grid;
		for (uint8_t s = 0; s < 3; ++s) {
			for (const float p : t.pitch_deg) {
				ref::Query q;
				q.row = s;
				q.pitch_deg = double(p);
				REQUIRE(r->evaluate(q, rc));
				accumulate(grid, posed_local(t, s, s, 0, double(p)), rc);
			}
		}
		std::printf("anim.server_pose on-grid steady entries: head %.4f cm chest %.4f cm body %.4f cm legs %.4f cm\n", grid.head, grid.chest, grid.body, grid.legs);
		CHECK(grid.head < 0.05);
		CHECK(grid.chest < 0.05);
		CHECK(grid.body < 0.05);
		CHECK(grid.legs < 0.05);
	}

	TEST_CASE("E18 (a): the stand rows against S3's CrowdEvaluator; run reported") {
		PoseData *d = data_or_skip();
		if (d == nullptr) {
			return;
		}
		const gp::CapsulePoseTable &t = d->table;
		auto r = ref::Reference::make(d->clips, d->weapons.capsules, d->report.aim, an::CrowdConfig{}.aim_order);
		REQUIRE(r.ok());
		auto crowd = an::load_crowd_clips(*d->source, *d->library);
		REQUIRE_MESSAGE(crowd.ok(), (crowd.ok() ? "" : crowd.error().message));
		an::EbxSkeleton ebx;
		ebx.partition = "rig";
		std::vector<an::Xform> rest;
		an::model_pose(crowd->rig, an::rest_pose(crowd->rig), rest);
		for (size_t i = 0; i < crowd->rig.size(); ++i) {
			ebx.bone_names.push_back(crowd->rig.bone(i).name);
			ebx.parents.push_back(crowd->rig.bone(i).parent);
			ebx.local_pose.push_back(an::mat_from_xform(crowd->rig.bone(i).rest));
			ebx.model_pose.push_back(an::mat_from_xform(rest[i]));
		}
		an::SkinBinds binds;
		REQUIRE(binds.bind(ebx, 0, crowd->rig).ok());
		an::CrowdTeam team;
		team.binds = &binds;
		an::CrowdConfig config;
		config.temporal_lod = false;
		const std::vector<double> pitches = mid_pitches(t);
		const uint32_t n = uint32_t(pitches.size()) + 1;
		auto ev = an::CrowdEvaluator::make(*crowd, std::span<const an::CrowdTeam>(&team, 1), n, config);
		REQUIRE_MESSAGE(ev.ok(), (ev.ok() ? "" : ev.error().message));
		// The same rig: the crowd's joints are the server pose's (both VeniceAntSke01 through skeleton_from_rig).
		REQUIRE(crowd->rig.size() == d->clips.rig.size());
		std::vector<an::CrowdSoldierInput> in(n);
		for (size_t i = 0; i < pitches.size(); ++i) {
			in[i].pitch = float(pitches[i] * kDeg);
		}
		in[n - 1].velocity = an::Vec3{ 0, 0, 5.0f };
		for (int f = 0; f < 30; ++f) {
			(*ev)->set_inputs(in, 1.0 / 60.0);
			(*ev)->evaluate_all_here();
		}
		std::vector<ref::RefCapsule> rc;
		Dev dev;
		for (size_t i = 0; i < pitches.size(); ++i) {
			r->place((*ev)->model(uint32_t(i)), rc);
			accumulate(dev, posed_local(t, 0, 0, 0, pitches[i]), rc);
		}
		std::printf("anim.server_pose E18 crowd stand (CrowdEvaluator) [%s] %zu queries: head %.3f cm chest %.3f cm (gate %.0f cm) body %.3f cm legs %.3f cm (reported)\n",
				tag(t.row_source[0]), dev.queries, dev.head, dev.chest, kSteadyGateCm, dev.body, dev.legs);
		CHECK(dev.queries == 32);
		CHECK_MESSAGE(dev.head <= kSteadyGateCm, "crowd stand head " << dev.head << " cm");
		CHECK_MESSAGE(dev.chest <= kSteadyGateCm, "crowd stand chest " << dev.chest << " cm");
		Dev run;
		r->place((*ev)->model(n - 1), rc);
		accumulate(run, posed_local(t, 0, 0, 0, 0), rc);
		std::printf("anim.server_pose E18 run 5 m/s vs the stand table (locomotion not modelled, reported): head %.3f cm chest %.3f cm body %.3f cm legs %.3f cm\n",
				run.head, run.chest, run.body, run.legs);
	}

	TEST_CASE("anim.server_pose: install unchanged") {
		PoseData *d = data_or_skip();
		if (d == nullptr) {
			return;
		}
		const core::SnapshotDiff diff = core::diff_snapshots(d->before, d->reader.snapshot());
		std::printf("anim.server_pose install.unchanged %s\n", diff.clean() ? "yes" : diff.summary().c_str());
		CHECK(diff.clean());
	}
}
