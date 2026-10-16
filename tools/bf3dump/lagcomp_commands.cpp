// bf3dump lagcomp (M2a lc; docs/design/m2a.md §6, E13, E18; as built: docs/design/m2a-lc.md): the capsule pose table
// from the install, E18 (a)'s deviation rows and E13's rewind bench rows. Read-only; nothing is written but stdout /
// stderr.
#include "lagcomp_commands.h"

#include "bf3/anim/clip_asset.h"
#include "bf3/anim/crowd_clips.h"
#include "bf3/anim/crowd_eval.h"
#include "bf3/anim/server_pose.h"
#include "bf3/anim/server_pose_reference.h"
#include "bf3/anim/skinning.h"
#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/gameplay/capsule_pose_table.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/host/lag_comp.h"
#include "bf3/net/other_sample.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/static_world.h"
#include "bf3/sim/weapons/ballistics.h"
#include "bf3/sim/weapons/posed_target.h"
#include "bf3/sim/weapons/shooting_range.h"
#include "bf3/sim/weapons/weapon_install.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <vector>

using namespace bf3;
namespace gp = bf3::core::gameplay;
namespace sw = bf3::sim::weapons;
namespace an = bf3::anim;

namespace bf3dump {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kSteadyGateCm = 3.0; // E18: head and chest, steady rows and the stand rows against CrowdEvaluator
constexpr double kTransitionGateCm = 5.0; // E18: transition rows
constexpr double kRewindGateMs = 0.5; // E13: rewind provider p99 per tick
constexpr std::string_view kM16A3 = "weapons/m16a4/u_m16a4"; // weapon_checks.cpp's kM16A3 (the u_m16a4 variant)
const char *kStance[3] = { "stand", "crouch", "prone" };
const char *kTransition[6] = { "stand->crouch", "stand->prone", "crouch->stand", "crouch->prone", "prone->stand", "prone->crouch" };
constexpr uint8_t kFrom[6] = { 0, 0, 1, 1, 2, 2 };
constexpr uint8_t kTo[6] = { 1, 2, 0, 2, 0, 1 };

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

bool install_unchanged(const EbxContext &p_ctx, const core::InstallSnapshot &p_before) {
	if (p_ctx.reader == nullptr) {
		std::printf("install.unchanged unknown (no reader)\n");
		return true;
	}
	const core::SnapshotDiff diff = core::diff_snapshots(p_before, p_ctx.reader->snapshot());
	const bool clean = p_before.valid && diff.clean();
	std::printf("install.unchanged %s\n", clean ? ("yes (" + std::to_string(p_before.files.size()) + " files: sizes and mtimes equal)").c_str()
												: ("NO: " + diff.summary()).c_str());
	return clean;
}

bool parse_u32(const std::string &p_s, uint32_t &r_v) {
	const auto r = std::from_chars(p_s.data(), p_s.data() + p_s.size(), r_v);
	return r.ec == std::errc() && r.ptr == p_s.data() + p_s.size();
}

uint64_t mix64(uint64_t p_x) {
	p_x ^= p_x >> 33;
	p_x *= 0xFF51AFD7ED558CCDull;
	p_x ^= p_x >> 33;
	p_x *= 0xC4CEB9FE1A85EC53ull;
	p_x ^= p_x >> 33;
	return p_x;
}
double unit_hash(uint64_t p_a, uint64_t p_b) { // [0, 1)
	return double(mix64(p_a * 0x9E3779B97F4A7C15ull + p_b) >> 11) / double(uint64_t(1) << 53);
}

// ---- the table from the install -----------------------------------------------------------------------------------

struct TableSet {
	sw::InstallWeapons weapons;
	sw::CapsuleTableData data;
	std::unique_ptr<core::ant::AntInstallSource> source;
	std::unique_ptr<core::ant::AntLibrary> library;
	an::ServerPoseClips clips;
	an::CapsuleTableInputs inputs;
	an::CapsuleTableReport report;
	gp::CapsulePoseTable table;
	double seconds_clips = 0;
};

core::Result<std::unique_ptr<TableSet>> open_table(const EbxContext &p_ctx, const std::string &p_level) {
	auto t = std::make_unique<TableSet>();
	core::Result<sw::InstallWeapons> w = sw::open_install_weapons(p_ctx.install, p_level);
	if (!w) {
		return w.error();
	}
	t->weapons = std::move(*w);
	core::Result<sw::CapsuleTableData> d = sw::capsule_table_data(t->weapons.soldier, t->weapons.capsules, *t->weapons.materials);
	if (!d) {
		return d.error();
	}
	t->data = std::move(*d);
	core::Result<std::unique_ptr<core::ant::AntInstallSource>> source = core::ant::AntInstallSource::open(p_ctx.install);
	if (!source) {
		return source.error();
	}
	t->source = std::move(*source);
	core::Result<std::unique_ptr<core::ant::AntLibrary>> lib = an::load_ant_library(*t->source);
	if (!lib) {
		return lib.error();
	}
	t->library = std::move(*lib);
	const auto t0 = std::chrono::steady_clock::now();
	core::Result<an::ServerPoseClips> clips = an::load_server_pose_clips(*t->source, *t->library);
	if (!clips) {
		return clips.error();
	}
	t->clips = std::move(*clips);
	t->seconds_clips = seconds_since(t0);
	t->inputs.capsules = t->weapons.capsules;
	t->inputs.property = t->data.property;
	t->inputs.zone = t->data.zone;
	t->inputs.standin = t->data.standin;
	core::Result<gp::CapsulePoseTable> table = an::build_capsule_pose_table(t->clips.rig, t->clips, t->inputs, &t->report);
	if (!table) {
		return table.error();
	}
	t->table = std::move(*table);
	return t;
}

double centre_of(const gp::CapsuleLocal &p_c, size_t p_k) {
	return 0.5 * (double(p_c.a[p_k]) + double(p_c.b[p_k]));
}

void print_table(const TableSet &p_t) {
	const gp::CapsulePoseTable &tab = p_t.table;
	const an::CapsuleTableReport &rep = p_t.report;
	std::printf("lagcomp.table version %u capsules %u hash %016" PRIx64 " bound_radius %.3f m; clips %.3f s, build %.3f s (%u pose evaluations)\n",
			tab.version, unsigned(tab.capsule_count), tab.hash, double(tab.bound_radius), p_t.seconds_clips, rep.seconds, rep.pose_evaluations);
	std::printf("lagcomp.table pitch buckets %.2f .. %.2f deg (%zu)\n", double(tab.pitch_deg.front()), double(tab.pitch_deg.back()), tab.pitch_deg.size());
	for (size_t s = 0; s < gp::kCapsuleStances; ++s) {
		std::printf("lagcomp.aim %s up_full %.2f deg down_full %.2f deg (%s)\n", kStance[s], double(rep.aim[s].up_full_deg), double(rep.aim[s].down_full_deg),
				rep.aim[s].measured ? "measured" : "the stand / CrowdConfig values");
	}
	size_t standins = 0;
	for (size_t r = 0; r < gp::kCapsuleRows; ++r) {
		const bool clip = tab.row_source[r] == gp::capsule_row_source::kClipDerived;
		standins += clip ? 0 : 1;
		std::printf("lagcomp.row %s %s\n", r < 3 ? kStance[r] : kTransition[r - 3], clip ? "clip-derived" : "stand-in");
	}
	std::printf("lagcomp.rows stand-in %zu of %zu\n", standins, gp::kCapsuleRows);
	for (const std::string &n : p_t.clips.notes) {
		std::printf("lagcomp.note clips: %s\n", n.c_str());
	}
	for (const std::string &n : rep.notes) {
		std::printf("lagcomp.note build: %s\n", n.c_str());
	}
	// Capsule -> joint map and the stand idle at pitch 0 (bucket 14): centres in the local frame (x left, y up, z fwd).
	const size_t zero = an::kCapsuleDownBuckets;
	for (size_t c = 0; c < tab.capsule_count; ++c) {
		const gp::CapsuleLocal &e = tab.steady[tab.steady_index(0, zero) + c];
		const gp::HitCapsule &hc = p_t.weapons.capsules[e.capsule];
		const int32_t joint = c < rep.joint.size() ? rep.joint[c] : -1;
		const std::string jname = joint >= 0 ? p_t.clips.rig.bone(size_t(joint)).name : std::string("-");
		std::printf("lagcomp.capsule %zu list %u bone %s joint %d (%s) zone %s radius %.3f length %.3f axis %d sign %+.0f; stand idle centre (%.3f, %.3f, "
					"%.3f) a (%.3f, %.3f, %.3f) b (%.3f, %.3f, %.3f)\n",
				c, unsigned(e.capsule), hc.bone.c_str(), joint, jname.c_str(), sw::hit_zone_name(sw::HitZone(e.zone)), double(e.radius), double(hc.length),
				int(hc.bone_axis), double(an::capsule_axis_sign(p_t.clips.rig, joint, hc.bone_axis)), centre_of(e, 0), centre_of(e, 1), centre_of(e, 2),
				double(e.a[0]), double(e.a[1]), double(e.a[2]), double(e.b[0]), double(e.b[1]), double(e.b[2]));
	}
}

// ---- E18 (a) ------------------------------------------------------------------------------------------------------

struct DevRow {
	std::string name;
	std::string source; // "clip-derived" / "stand-in" / the independent path
	size_t queries = 0;
	double head_cm = 0, chest_cm = 0, body_cm = 0, legs_cm = 0;
	double gate_cm = 0; // 0: reported only
	bool evaluated = true;
	bool pass() const { return gate_cm <= 0 || (evaluated && head_cm <= gate_cm && chest_cm <= gate_cm); }
};

void accumulate(DevRow &r_row, const sw::HitTarget &p_posed, const std::vector<an::server_pose_reference::RefCapsule> &p_ref) {
	const size_t n = std::min<size_t>(p_posed.capsule_count, p_ref.size());
	for (size_t c = 0; c < n; ++c) {
		const sw::ZoneCapsule &z = p_posed.capsules[c];
		double d2 = 0;
		for (size_t k = 0; k < 3; ++k) {
			const double pc = 0.5 * (double(z.a[k]) + double(z.b[k]));
			d2 += (pc - p_ref[c].centre[k]) * (pc - p_ref[c].centre[k]);
		}
		const double cm = std::sqrt(d2) * 100.0;
		double *slot = z.zone == sw::HitZone::Head ? &r_row.head_cm : z.zone == sw::HitZone::Chest ? &r_row.chest_cm : z.zone == sw::HitZone::Legs ? &r_row.legs_cm : &r_row.body_cm;
		*slot = std::max(*slot, cm);
	}
}

sw::HitTarget posed_local(const gp::CapsulePoseTable &p_table, uint8_t p_from, uint8_t p_to, double p_fraction, double p_pitch_deg) {
	sw::TargetPose tp;
	tp.pose = sim::Pose(p_from);
	tp.target_pose = sim::Pose(p_to);
	tp.transition = float(p_fraction);
	tp.pitch = float(p_pitch_deg * kDeg);
	return sw::posed_target(p_table, tp);
}

std::vector<double> mid_pitches(const gp::CapsulePoseTable &p_table) {
	std::vector<double> m;
	for (size_t i = 0; i + 1 < p_table.pitch_deg.size(); ++i) {
		m.push_back(0.5 * (double(p_table.pitch_deg[i]) + double(p_table.pitch_deg[i + 1])));
	}
	return m;
}

int run_deviation(const TableSet &p_t) {
	const gp::CapsulePoseTable &tab = p_t.table;
	core::Result<an::server_pose_reference::Reference> ref = an::server_pose_reference::Reference::make(p_t.clips, p_t.weapons.capsules, p_t.report.aim,
			an::CrowdConfig{}.aim_order);
	if (!ref) {
		std::fprintf(stderr, "lagcomp --deviation: %s\n", ref.error().message.c_str());
		return 1;
	}
	const std::vector<double> pitches = mid_pitches(tab);
	std::vector<DevRow> rows;
	std::vector<an::server_pose_reference::RefCapsule> rc;
	for (uint8_t s = 0; s < 3; ++s) {
		DevRow r;
		r.name = std::string("steady ") + kStance[s];
		r.source = tab.row_source[s] == gp::capsule_row_source::kClipDerived ? "clip-derived" : "stand-in";
		r.gate_cm = kSteadyGateCm;
		for (const double p : pitches) {
			an::server_pose_reference::Query q;
			q.row = s;
			q.pitch_deg = p;
			if (!ref->evaluate(q, rc)) {
				r.evaluated = false;
				continue;
			}
			accumulate(r, posed_local(tab, s, s, 0, p), rc);
			++r.queries;
		}
		rows.push_back(r);
	}
	for (uint8_t tr = 0; tr < 6; ++tr) {
		DevRow r;
		r.name = std::string("transition ") + kTransition[tr];
		r.source = tab.row_source[3 + tr] == gp::capsule_row_source::kClipDerived ? "clip-derived" : "stand-in";
		r.gate_cm = kTransitionGateCm;
		for (size_t f = 0; f + 1 < gp::kCapsuleTransitionFractions; ++f) {
			const double frac = (double(f) + 0.5) / double(gp::kCapsuleTransitionFractions - 1);
			for (const double p : pitches) {
				an::server_pose_reference::Query q;
				q.transition = true;
				q.row = tr;
				q.pitch_deg = p;
				q.fraction = frac;
				if (!ref->evaluate(q, rc)) {
					r.evaluated = false;
					continue;
				}
				accumulate(r, posed_local(tab, kFrom[tr], kTo[tr], frac, p), rc);
				++r.queries;
			}
		}
		rows.push_back(r);
	}
	// The stand rows against S3's CrowdEvaluator (an independent code path: its own clip set, loco blend and layers).
	{
		DevRow r;
		r.name = "crowd stand (CrowdEvaluator, backend B)";
		r.source = tab.row_source[0] == gp::capsule_row_source::kClipDerived ? "clip-derived" : "stand-in";
		r.gate_cm = kSteadyGateCm;
		DevRow run;
		run.name = "run 5 m/s fwd vs stand (locomotion not modelled; reported)";
		run.source = "CrowdEvaluator";
		core::Result<an::CrowdClipSet> crowd = an::load_crowd_clips(*p_t.source, *p_t.library);
		if (!crowd) {
			std::printf("lagcomp.e18 crowd: load_crowd_clips failed: %s\n", crowd.error().message.c_str());
			r.evaluated = false;
		} else {
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
			(void)binds.bind(ebx, 0, crowd->rig);
			an::CrowdTeam team;
			team.binds = &binds;
			an::CrowdConfig config;
			config.temporal_lod = false;
			const uint32_t n = uint32_t(pitches.size()) + 1;
			core::Result<std::unique_ptr<an::CrowdEvaluator>> ev = an::CrowdEvaluator::make(*crowd, std::span<const an::CrowdTeam>(&team, 1), n, config);
			if (!ev) {
				std::printf("lagcomp.e18 crowd: CrowdEvaluator::make failed: %s\n", ev.error().message.c_str());
				r.evaluated = false;
			} else {
				std::vector<an::CrowdSoldierInput> in(n);
				for (size_t i = 0; i < pitches.size(); ++i) {
					in[i].pitch = float(pitches[i] * kDeg);
				}
				in[n - 1].velocity = an::Vec3{ 0, 0, 5.0f };
				// A few frames so the run blend settles (the stand soldiers have no motion: idle weight 1 throughout).
				for (int f = 0; f < 30; ++f) {
					(*ev)->set_inputs(in, 1.0 / 60.0);
					(*ev)->evaluate_all_here();
				}
				for (size_t i = 0; i < pitches.size(); ++i) {
					const std::span<const an::Xform> model = (*ev)->model(uint32_t(i));
					ref->place(model, rc);
					accumulate(r, posed_local(tab, 0, 0, 0, pitches[i]), rc);
					++r.queries;
				}
				ref->place((*ev)->model(n - 1), rc);
				accumulate(run, posed_local(tab, 0, 0, 0, 0), rc);
				run.queries = 1;
			}
		}
		rows.push_back(r);
		rows.push_back(run);
	}
	size_t gated = 0, passed = 0, standin = 0;
	for (const DevRow &r : rows) {
		const bool g = r.gate_cm > 0;
		gated += g ? 1 : 0;
		passed += g && r.pass() ? 1 : 0;
		standin += r.source == std::string("stand-in") ? 1 : 0;
		std::printf("lagcomp.e18 %-44s [%s] queries %4zu head %.3f cm chest %.3f cm body %.3f cm legs %.3f cm (reported)%s%s\n", r.name.c_str(), r.source.c_str(),
				r.queries, r.head_cm, r.chest_cm, r.body_cm, r.legs_cm,
				g ? (std::string(" gate head/chest <= ") + std::to_string(int(r.gate_cm)) + " cm " + (r.pass() ? "PASS" : "FAIL")).c_str() : " reported",
				r.evaluated ? "" : " (not every query evaluated)");
	}
	const bool ok = passed == gated && standin == 0;
	std::printf("E18 (a): %s (%zu/%zu gated rows, %zu stand-in rows; expected 0)\n", ok ? "PASS" : "FAIL", passed, gated, standin);
	return ok ? 0 : 1;
}

// ---- E13 rewind rows ----------------------------------------------------------------------------------------------

struct PerfResult {
	std::string name;
	uint32_t ticks = 0;
	double p99_ms = 0, mean_ms = 0, max_ms = 0, world_ms_mean = 0;
	double queries = 0, candidates = 0, cache_hits = 0, culled = 0, answered = 0; // per tick
	uint64_t live_peak = 0, shots = 0, hits = 0;
	host::LagCompStats stats;
};

constexpr uint32_t kShooters = 64;
constexpr uint32_t kUpTicks = 8; // command one-way latency
constexpr int64_t kShotInterval = 4'500'000; // 13.3 rounds/s (M16A3 full auto, 800 rpm)

struct ClientNet {
	uint16_t next_seq = 0;
	uint32_t down = 9;
	struct Entry {
		uint16_t seq;
		uint32_t tick;
		bool main_lost, others_lost, others_sent;
	};
	std::deque<Entry> sent;
};

core::Result<PerfResult> run_perf(bool p_long, const sw::WeaponTable &p_table, uint16_t p_def, const gp::CapsulePoseTable &p_capsules,
		const std::array<float, sim::kPoseCount> &p_eyes, uint32_t p_warm, uint32_t p_ticks) {
	PerfResult res;
	res.name = p_long ? "long" : "pairs";
	const std::vector<sim::StaticTriangle> tris = sw::range_wall_triangles({}, 1.6f, 3'500.0f, 150.0f);
	core::Result<std::shared_ptr<const sim::StaticWorldShapes>> shapes = sim::build_static_world(std::span<const sim::StaticTriangle>(tris));
	if (!shapes) {
		return shapes.error();
	}
	core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(*shapes);
	if (!world) {
		return world.error();
	}
	host::LagCompConfig cfg;
	cfg.pose_eye_heights = p_eyes;
	host::LagHistory history(cfg, kShooters);
	host::RewindProvider provider(history, p_capsules, cfg);
	auto table = std::make_shared<sw::WeaponTable>(p_table);
	sw::ProjectileWorld pw(table);
	// Layout: pairs, 32 lanes 12 m apart, opponents 20..40 m apart along z, facing each other; long, one line along x,
	// every shooter firing 2 deg up along +z (nothing in front).
	std::array<double, kShooters> base_x{}, base_z{};
	std::array<uint8_t, kShooters> team{};
	std::array<uint16_t, kShooters> yaw{};
	for (uint32_t s = 0; s < kShooters; ++s) {
		const uint32_t lane = s / 2;
		base_x[s] = 200.0 + 12.0 * lane + (p_long ? 6.0 * (s % 2) : 0.0);
		const double d = 20.0 + 20.0 * double(lane) / 31.0;
		base_z[s] = p_long ? 500.0 : (s % 2 == 0 ? 1500.0 : 1500.0 + d);
		team[s] = uint8_t(s % 2);
		yaw[s] = p_long || s % 2 == 0 ? 0 : 32768;
	}
	std::array<ClientNet, kShooters> net{};
	for (uint32_t c = 0; c < kShooters; ++c) {
		net[c].next_seq = uint16_t(65'000u + c * 997u); // different seqs per client, some wrap during the run
		net[c].down = 8 + c % 3;
	}
	std::array<sim::SoldierState, kShooters> st{};
	std::array<SimTime, kShooters> next_shot{};
	std::array<uint32_t, kShooters> shot_index{};
	const uint32_t total = p_warm + p_ticks;
	const uint32_t first_tick = 100; // history from tick 100; shots once the history spans the rewind
	for (uint32_t c = 0; c < kShooters; ++c) {
		next_shot[c] = sw::tick_start(first_tick + 60) + SimTime(c) * 70'000;
	}
	const int32_t view_back_q8 = int32_t(cfg.max_rewind_ticks + 1) * 256; // lag exactly max_rewind_ticks (§6.4)
	std::vector<double> times;
	double world_s = 0;
	host::LagCompStats at_warm{};
	sw::ProjectileStepOutput out;
	std::vector<net::OtherSoldier> records;
	for (uint32_t T = first_tick; T < first_tick + total; ++T) {
		const double t = double(T) / double(sim::kSimHz);
		for (uint32_t s = 0; s < kShooters; ++s) {
			const double w = 1.2 + 0.03 * s, ph = 0.7 * s, a = 2.0;
			sim::SoldierState &x = st[s];
			x.tick = T;
			x.life = 1;
			x.mode = sim::MoveMode::OnGround;
			x.position = { float(base_x[s] + a * std::sin(w * t + ph)), 0.0f, float(base_z[s]) };
			x.velocity = { float(a * w * std::cos(w * t + ph)), 0.0f, 0.0f };
			x.yaw = yaw[s];
		}
		// 1. Commands arrive: acks and the shot views of this tick's command (§4.1 step 1, §6.3).
		std::array<host::ShotView, kShooters> views{};
		std::array<bool, kShooters> view_ok{};
		for (uint32_t c = 0; c < kShooters; ++c) {
			ClientNet &n = net[c];
			const ClientNet::Entry *newest = nullptr;
			for (const ClientNet::Entry &e : n.sent) {
				if (e.tick + n.down + kUpTicks <= T && !e.main_lost) {
					newest = &e;
				}
			}
			if (newest == nullptr) {
				continue;
			}
			uint32_t main_bits = 0, others_bits = 0;
			for (const ClientNet::Entry &e : n.sent) {
				if (e.tick + n.down + kUpTicks > T) {
					continue;
				}
				const uint16_t back = uint16_t(newest->seq - e.seq);
				if (back >= 1 && back <= 32 && !e.main_lost) {
					main_bits |= 1u << (back - 1);
				}
				if (back <= 31 && e.others_sent && !e.others_lost) {
					others_bits |= 1u << back;
				}
			}
			history.ack(uint8_t(c), newest->seq, main_bits, others_bits);
			const host::AckState at = history.ack_state(uint8_t(c));
			view_ok[c] = history.shot_view(uint8_t(c), at, uint8_t(at.newest & 0xFF), view_back_q8, uint8_t(sim::ViewQuality::Interpolated), views[c]);
		}
		// 2. The provider's present set, then the shots of this tick and the projectile step.
		provider.begin_tick(T, st, team);
		for (uint32_t c = 0; c < kShooters; ++c) {
			while (next_shot[c] < sw::tick_end(T)) {
				sw::ShotEvent e;
				e.time = next_shot[c];
				e.spawn_time = e.time + table->defs[p_def].spawn_delay;
				e.seed = mix64(uint64_t(c) << 32 | shot_index[c]);
				e.player_slot = c;
				e.shot_index = shot_index[c];
				e.def = p_def;
				e.pellets = 1;
				e.origin = { st[c].position[0], 1.6f, st[c].position[2] };
				sim::Vec3 dir{ 0, 0, 1 };
				if (p_long) {
					dir = { 0.0f, float(std::sin(2.0 * kDeg)), float(std::cos(2.0 * kDeg)) };
				} else {
					const uint32_t o = c ^ 1u; // the opponent, aimed at its chest with a seeded error
					const double ex = (unit_hash(c, shot_index[c]) - 0.5) * 0.8;
					const double ey = (unit_hash(c + 1000, shot_index[c]) - 0.5) * 0.6;
					const double dx = double(st[o].position[0]) + ex - e.origin[0];
					const double dy = 1.3 + ey - e.origin[1];
					const double dz = double(st[o].position[2]) - e.origin[2];
					const double l = std::sqrt(dx * dx + dy * dy + dz * dz);
					dir = { float(dx / l), float(dy / l), float(dz / l) };
				}
				e.aim = dir;
				e.direction = dir;
				if (view_ok[c]) {
					provider.note_shot(uint8_t(c), e.shot_index, views[c]);
				} else {
					provider.note_refused(uint8_t(c), e.shot_index);
				}
				if (pw.spawn(e).ok()) {
					++res.shots;
				}
				++shot_index[c];
				next_shot[c] += kShotInterval;
			}
		}
		out.clear();
		const auto w0 = std::chrono::steady_clock::now();
		pw.step(T, **world, provider, out);
		const double ws = seconds_since(w0);
		provider.end_tick();
		res.live_peak = std::max<uint64_t>(res.live_peak, pw.live().size());
		// 3. The snapshot of this tick (30 Hz): shared records, then each client's seq and split.
		if (T % 2 == 0) {
			records.clear();
			for (uint32_t s = 0; s < kShooters; ++s) {
				records.push_back(net::quantize_other(uint8_t(s), st[s]));
			}
			history.record_tick(T, records);
			for (uint32_t c = 0; c < kShooters; ++c) {
				uint64_t main_mask = 0, others_mask = 0;
				for (uint32_t s = 0; s < kShooters; ++s) {
					if (s == c) {
						continue;
					}
					const uint32_t d = std::min((s + kShooters - c) % kShooters, (c + kShooters - s) % kShooters);
					(d <= 12 ? main_mask : others_mask) |= uint64_t(1) << s;
				}
				ClientNet &n = net[c];
				const uint16_t seq = n.next_seq++;
				history.sent(uint8_t(c), seq, T, main_mask, others_mask);
				ClientNet::Entry e{ seq, T, unit_hash(c * 3 + 1, seq) < 0.03, unit_hash(c * 3 + 2, seq) < 0.03, others_mask != 0 };
				n.sent.push_back(e);
				while (n.sent.size() > 48) {
					n.sent.pop_front();
				}
			}
		}
		if (T == first_tick + p_warm - 1) {
			at_warm = provider.stats();
		}
		if (T >= first_tick + p_warm) {
			times.push_back(provider.last_tick_ms());
			world_s += ws;
			res.hits += out.damage.size();
		}
	}
	res.ticks = uint32_t(times.size());
	res.stats = provider.stats();
	std::vector<double> sorted = times;
	std::sort(sorted.begin(), sorted.end());
	if (!sorted.empty()) {
		res.p99_ms = sorted[std::min(sorted.size() - 1, size_t(std::ceil(0.99 * double(sorted.size()))) - 1)];
		res.max_ms = sorted.back();
		double sum = 0;
		for (const double v : sorted) {
			sum += v;
		}
		res.mean_ms = sum / double(sorted.size());
		res.world_ms_mean = world_s * 1000.0 / double(sorted.size());
	}
	const double nt = std::max(1.0, double(res.ticks));
	res.queries = double(res.stats.queries - at_warm.queries) / nt;
	res.candidates = double(res.stats.candidates - at_warm.candidates) / nt;
	res.cache_hits = double(res.stats.pose_cache_hits - at_warm.pose_cache_hits) / nt;
	res.culled = double(res.stats.culled - at_warm.culled) / nt;
	res.answered = double(res.stats.answered - at_warm.answered) / nt;
	return res;
}

int run_perf_rows(const TableSet &p_t, const std::vector<std::string> &p_rows, uint32_t p_warm, uint32_t p_ticks) {
	const std::string_view variants[] = { kM16A3 };
	core::Result<std::shared_ptr<const sw::WeaponTable>> wt = sw::install_weapon_table(p_t.weapons, variants, sw::install_table_options(p_t.weapons, {}, true));
	if (!wt) {
		std::fprintf(stderr, "lagcomp --perf: %s\n", wt.error().message.c_str());
		return 1;
	}
	std::array<float, sim::kPoseCount> eyes{ 1.6f, 1.0f, 0.3f };
	for (size_t k = 0; k < sim::kPoseCount; ++k) {
		if (const gp::PoseDef *p = p_t.weapons.soldier.pose(gp::SoldierPose(k)); p != nullptr && p->eye_height > 0) {
			eyes[k] = p->eye_height;
		}
	}
	std::printf("lagcomp.perf weapon %s muzzle %.1f m/s ttl %.3f s; 64 human shooters at the maximum lag (%u ticks), 13.3 rounds/s each; warm %u, measured %u "
				"ticks (wall clock on a shared machine: noisy, run under the quiet lock)\n",
			std::string(kM16A3).c_str(), double((*wt)->defs[0].projectile.muzzle_speed), sw::units_to_seconds((*wt)->defs[0].projectile.time_to_live),
			host::kDefaultMaxRewindTicks, p_warm, p_ticks);
	bool all = true;
	for (const std::string &row : p_rows) {
		core::Result<PerfResult> r = run_perf(row == "long", **wt, 0, p_t.table, eyes, p_warm, p_ticks);
		if (!r) {
			std::fprintf(stderr, "lagcomp --perf %s: %s\n", row.c_str(), r.error().message.c_str());
			return 1;
		}
		const bool pass = r->p99_ms <= kRewindGateMs;
		all = all && pass;
		const host::LagCompStats &s = r->stats;
		std::printf("lagcomp.perf %-5s provider p99 %.4f ms mean %.4f ms max %.4f ms per tick; per tick: queries %.1f candidates %.2f cache hits %.2f culled %.1f "
					"answered %.2f; live peak %" PRIu64 ", shots %" PRIu64 ", damage events %" PRIu64 "; projectile step mean %.3f ms; verdicts present %" PRIu64
					" rewound %" PRIu64 " clamped %" PRIu64 " refused %" PRIu64 "; generation skips %" PRIu64 ", history misses %" PRIu64 ", acks ignored %" PRIu64
					", cache full %" PRIu64 " -- gate p99 <= %.1f ms %s\n",
				r->name.c_str(), r->p99_ms, r->mean_ms, r->max_ms, r->queries, r->candidates, r->cache_hits, r->culled, r->answered, r->live_peak, r->shots, r->hits,
				r->world_ms_mean, s.present, s.rewound, s.clamped, s.refused, s.generation_skips, s.history_misses, s.acks_ignored, s.pose_cache_full, kRewindGateMs,
				pass ? "PASS" : "FAIL");
	}
	std::printf("E13 rewind: %s\n", all ? "PASS" : "FAIL (wall clock: repeat once under the quiet lock before reporting, m2a.md §1 noise policy)");
	return all ? 0 : 1;
}

} // namespace

const char *lagcomp_usage() {
	return "  lagcomp <LEVEL> <MODE> [--deviation] [--perf] [--rows pairs,long] [--warm N] [--ticks N]\n"
		   "      M2a lc: the capsule pose table from the install; --deviation E18 (a) rows; --perf E13's rewind rows\n";
}

int cmd_lagcomp(const EbxContext &p_ctx, const std::vector<std::string> &p_args) {
	if (p_args.size() < 2) {
		std::fprintf(stderr, "%s", lagcomp_usage());
		return 2;
	}
	const std::string level = p_args[0];
	const std::string mode = p_args[1];
	bool deviation = false, perf = false;
	uint32_t warm = 120, ticks = 1200;
	std::vector<std::string> rows{ "pairs", "long" };
	for (size_t i = 2; i < p_args.size(); ++i) {
		const std::string &a = p_args[i];
		if (a == "--deviation") {
			deviation = true;
		} else if (a == "--perf") {
			perf = true;
		} else if ((a == "--ticks" || a == "--warm") && i + 1 < p_args.size()) {
			uint32_t v = 0;
			if (!parse_u32(p_args[++i], v) || v == 0 || v > 100'000) {
				std::fprintf(stderr, "lagcomp: bad %s\n", a.c_str());
				return 2;
			}
			(a == "--ticks" ? ticks : warm) = v;
		} else if (a == "--rows" && i + 1 < p_args.size()) {
			rows.clear();
			std::string s = p_args[++i];
			size_t at = 0;
			while (at <= s.size()) {
				const size_t comma = std::min(s.find(',', at), s.size());
				const std::string r = s.substr(at, comma - at);
				if (r != "pairs" && r != "long") {
					std::fprintf(stderr, "lagcomp: unknown row %s (pairs, long)\n", r.c_str());
					return 2;
				}
				rows.push_back(r);
				at = comma + 1;
			}
		} else {
			std::fprintf(stderr, "lagcomp: unknown argument %s\n%s", a.c_str(), lagcomp_usage());
			return 2;
		}
	}
	if (p_ctx.install == nullptr) {
		std::fprintf(stderr, "lagcomp: no install\n");
		return 2;
	}
	const core::InstallSnapshot before = p_ctx.reader != nullptr ? p_ctx.reader->snapshot() : core::InstallSnapshot();
	std::printf("lagcomp %s %s (the mode only labels the run: the table and the bench rows do not depend on it)\n", level.c_str(), mode.c_str());
	core::Result<std::unique_ptr<TableSet>> t = open_table(p_ctx, level);
	if (!t) {
		std::fprintf(stderr, "lagcomp: %s\n", t.error().message.c_str());
		install_unchanged(p_ctx, before);
		return 1;
	}
	print_table(**t);
	int rc = 0;
	if (deviation) {
		rc |= run_deviation(**t);
	}
	if (perf) {
		rc |= run_perf_rows(**t, rows, warm, ticks);
	}
	if (!install_unchanged(p_ctx, before)) {
		return 1;
	}
	return rc;
}

} // namespace bf3dump
