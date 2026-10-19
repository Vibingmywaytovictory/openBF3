// Bf3GameCrowd: the other soldiers as 3p soldiers in the game (M2a tp; docs/design/m2a.md §9, R5; as built:
// docs/design/m2a-tp.md). See game_crowd.h.
//
// Soldier index: crowd team x kSlotsPerTeam + wire slot (net::kMaxSlots = 128 per team, 256 = kMaxCrowdSoldiers), so
// S3's RS uploader, whose instances carry one team mesh each from build time, never rebuilds when a slot changes team;
// the index of the other team is hidden (crowd_flags::kHidden: no evaluation, no upload). Per display frame (step):
//   1. events (oldest first) wait in a pending list until the render clock passes their time (Render timeline; the
//      Immediate ones apply at once): RemoteShot (slot = shooter) bumps the soldier's fire_seq, Death (aux = victim)
//      starts its death, Spawn (slot) ends it;
//   2. present_others -> CrowdSoldierInput per present soldier: position, velocity, yaw, pitch, pose, target_pose,
//      transition, camera distance; team = crowd_team_of_wire(other_weapons[i].team); weapon = crowd_weapon_index of
//      the def (-1: item 0, counted; kNoWeapon keeps the last); a reload_seq change = kReloadStart; ads; a new
//      generation = alive again. Without any RemoteShot event this session, a `firing` sample with a new sample_tick
//      bumps fire_seq (the fallback). A dead soldier the snapshots no longer list stays where it was last presented
//      (dead = true) until the evaluator hides it (kCrowdDeathHideSeconds), then kHidden;
//   3. S3's order: wait_and_publish -> upload (roots of this frame's inputs, changed palettes) -> the weapon instances
//      (rebased when weapon_of changes, fed from weapon_palette) -> the muzzles (muzzle_model x root, fx2's effect
//      frame) -> set_inputs -> kick.
// Godot space is the sim's (S3's roots: Transform3D(Basis(Y, yaw), position)), so every transform here is sim
// coordinates, Frostbite axes.
#include "game_crowd.h"

#include "game_crowd_items.h"
#include "godot_util.h"
#include "render_bridge.h"
#include "skin_upload.h"

#include "bf3/anim/server_pose.h"
#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/install.h"
#include "bf3/core/worker_group.h"
#include "bf3/net/protocol.h"
#include "bf3/render/soldier_assets.h"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <optional>
#include <thread>
#include <vector>

namespace bf3::godot_side {

using namespace godot;

namespace {

constexpr uint32_t kSlotsPerTeam = uint32_t(net::kMaxSlots);
constexpr uint32_t kCrowdSoldiers = kSlotsPerTeam * anim::kMaxCrowdTeams;
static_assert(kCrowdSoldiers <= anim::kMaxCrowdSoldiers);
constexpr size_t kMaxPending = 1024; // events waiting for the render clock (beyond: the oldest is applied early, counted)
constexpr float kCorpseExtraSeconds = 0.5f; // a corpse input outlives the evaluator's hide by this much, then kHidden
constexpr uint32_t kCamLaneOffset = uint32_t(net::kMaxPlayerRows); // M2b cam [corpse]: a slot's second crowd index
static_assert(kCamLaneOffset * 2 == kSlotsPerTeam); // M2b cam [corpse]

void log(const std::string &p_line) {
	UtilityFunctions::print(to_godot("[crowd] " + p_line));
}

anim::Quat yaw_quat(float p_yaw) {
	return anim::Quat{ 0.0f, std::sin(0.5f * p_yaw), 0.0f, std::cos(0.5f * p_yaw) };
}

fx::Vec3 fv(const anim::Vec3 &p_v) {
	return fx::Vec3{ p_v.x, p_v.y, p_v.z };
}

anim::Vec3 rotate(const anim::Quat &p_q, const anim::Vec3 &p_v) {
	return anim::xform_mul(anim::Xform{ p_q, anim::Vec3{} }, anim::Xform{ anim::Quat{}, p_v }).t;
}

fx::Transform fx_transform(const anim::Xform &p_x) {
	fx::Transform t;
	t.right = fv(rotate(p_x.q, anim::Vec3{ 1, 0, 0 }));
	t.up = fv(rotate(p_x.q, anim::Vec3{ 0, 1, 0 }));
	t.forward = fv(rotate(p_x.q, anim::Vec3{ 0, 0, 1 }));
	t.trans = fv(p_x.t);
	return t;
}

struct WeaponMesh {
	Ref<ArrayMesh> mesh;
	AABB aabb;
	int32_t binds = 0;
};

} // namespace

struct Bf3GameCrowd::Impl {
	// Loaded (loader thread; read-only on the main thread after loaded is set).
	std::unique_ptr<render::SoldierAssets> assets;
	std::unique_ptr<render::CrowdWeaponSet> weapons;
	anim::ServerPoseClips pose;
	bool have_pose = false;
	anim::CrowdMoveClips moves;
	bool have_moves = false;
	anim::CrowdMoveGroupSet move_groups; // PT1 run [load]: BF3's move groups (docs/design/pt1-run.md)
	bool have_move_groups = false; // PT1 run [load]
	anim::CrowdConfig config;
	std::array<std::vector<anim::CrowdWeapon>, 2> views;
	std::vector<anim::CrowdTeam> teams;
	std::atomic<bool> loaded{ false };
	// M2a w3 integration (E16): load_body's result until load() takes it (one thread at a time: the session joins
	// its crowd thread before load()).
	std::unique_ptr<render::SoldierAssets> body;
	std::vector<std::string> body_notes;
	std::array<double, 2> body_seconds{}; // soldier assets; stance and move clips and the aim measure
	// Built (main thread).
	bool built = false;
	std::unique_ptr<core::WorkerGroup> lanes;
	std::unique_ptr<anim::CrowdEvaluator> eval;
	std::unique_ptr<SkinUploader> uploader;
	// False under the headless display (the dummy RenderingServer refuses instances and skeletons): the crowd poses,
	// counts and publishes its muzzles (FX / audio) but draws nothing.
	bool draw = true;
	RID scenario;
	std::array<std::vector<WeaponMesh>, 2> weapon_meshes;
	struct WeaponDraw {
		RID instance;
		RID skeleton;
		uint8_t item = anim::kNoCrowdWeapon; // the base of the instance
		int32_t binds = 0;
		bool visible = false;
	};
	std::vector<WeaponDraw> weapon_draws;
	std::vector<bool> soldier_visible;
	// Per wire slot.
	struct Slot {
		bool known = false;
		uint8_t team = 0; // crowd team
		uint8_t generation = 0;
		bool present = false; // in present_others this frame
		bool dead = false;
		double dead_seconds = 0.0; // since the death was applied
		anim::CrowdSoldierInput input; // the last presented input (a corpse keeps it)
		uint8_t fire_seq = 0;
		uint8_t reload_seq = 0;
		bool reload_known = false;
		uint32_t sample_tick = 0;
		uint8_t weapon = 0;
		uint8_t lane = 0; // M2b cam [corpse]: this life's crowd index is team x 128 + slot + lane x kCamLaneOffset
		bool revived = false; // M2b cam [revive]: this life began as a revive (CrowdSoldierInput::revived)
		double revive_wait = -1.0; // M2b cam [revive]: seconds since a Revive event named the slot (< 0: none)
	};
	std::array<Slot, kSlotsPerTeam> slots{};
	// M2b cam [corpse]: the M2b corpse state (set_corpse_seconds; the // M2b cam block at the end of this file).
	struct CamCorpse { // M2b cam [corpse]: a corpse drawn on its own crowd index while its slot lives again
		bool active = false; // M2b cam [corpse]
		uint32_t index = 0; // M2b cam [corpse]
		double seconds = 0.0; // M2b cam [corpse]: since the death
		anim::CrowdSoldierInput input; // M2b cam [corpse]
	}; // M2b cam [corpse]
	std::array<std::array<CamCorpse, 2>, kSlotsPerTeam> cam_corpses{}; // M2b cam [corpse]: [slot][lane]
	float corpse_seconds = anim::kCrowdDeathHideSeconds; // M2b cam [corpse]
	bool cam_m2b = false; // M2b cam [corpse]: set_corpse_seconds ran (two indices per slot, the own corpse)
	uint64_t cam_counts[5] = {}; // M2b cam [corpse]: lane flips, corpses cut short, skipped slots, stale revives, own corpses
	bool cam_event(const host::PresentEvent &p_e, const M2aFrame &p_frame); // M2b cam [revive]
	void cam_new_life(uint32_t p_slot, Slot &r_s); // M2b cam [corpse] [revive]
	void cam_draw_corpses(double p_dt, const anim::Vec3 &p_camera); // M2b cam [corpse]
	void cam_log() const; // M2b cam [corpse]
	struct Pending {
		host::PresentKind kind;
		host::PresentTimeline timeline;
		uint8_t slot;
		int64_t time;
	};
	std::deque<Pending> pending;
	bool remote_shot_seen = false;
	uint64_t pending_dropped = 0;
	std::vector<anim::CrowdSoldierInput> inputs;
	std::vector<Transform3D> roots;
	std::vector<anim::Xform> root_x;
	std::vector<fx::MuzzleSample> muzzles;
	GameCrowdStats stats;
	SkinUploadStats upload_stats;
	bool shut_down = false;

	void apply(const Pending &p_e);
	void build_inputs(const M2aFrame &p_frame);
	void draw_weapons(const M2aFrame &p_frame);
	void clear_draw();
};

void Bf3GameCrowd::Impl::apply(const Pending &p_e) {
	if (p_e.slot >= kSlotsPerTeam) {
		return;
	}
	Slot &s = slots[p_e.slot];
	switch (p_e.kind) {
		case host::PresentKind::RemoteShot:
			s.fire_seq = uint8_t(s.fire_seq + 1);
			break;
		case host::PresentKind::Death:
			if (!s.dead) {
				s.dead = true;
				s.dead_seconds = 0.0;
			}
			break;
		case host::PresentKind::Spawn:
			s.dead = false;
			s.reload_known = false;
			break;
		default:
			break;
	}
}

void Bf3GameCrowd::Impl::build_inputs(const M2aFrame &p_frame) {
	// 1. Events.
	for (const host::PresentEvent &e : p_frame.events) {
		if (cam_event(e, p_frame)) { // M2b cam [revive]: Revive events and the own death, applied at once
			continue; // M2b cam [revive]
		} // M2b cam [revive]
		Pending p{ e.kind, e.timeline, e.slot, e.time };
		if (e.kind == host::PresentKind::Death) {
			p.slot = e.aux; // the victim
		} else if (e.kind == host::PresentKind::RemoteShot) {
			remote_shot_seen = true;
		} else if (e.kind != host::PresentKind::Spawn) {
			continue;
		}
		if (pending.size() >= kMaxPending) {
			apply(pending.front());
			pending.pop_front();
			++pending_dropped;
		}
		pending.push_back(p);
	}
	for (auto it = pending.begin(); it != pending.end();) {
		const bool due = it->timeline == host::PresentTimeline::Immediate || !p_frame.clock.valid(it->timeline) || p_frame.clock.now(it->timeline) >= it->time;
		if (due) {
			apply(*it);
			it = pending.erase(it);
		} else {
			++it;
		}
	}
	// 2. The present soldiers.
	for (Slot &s : slots) {
		s.present = false;
	}
	const anim::Vec3 cam{ p_frame.camera.trans.x, p_frame.camera.trans.y, p_frame.camera.trans.z };
	stats.unknown_weapons = 0;
	for (size_t i = 0; i < p_frame.others.size(); ++i) {
		const host::PresentedSoldier &o = p_frame.others[i];
		if (o.slot >= kSlotsPerTeam) {
			continue;
		}
		Slot &s = slots[o.slot];
		const host::PresentedOtherWeapon *w = i < p_frame.other_weapons.size() && p_frame.other_weapons[i].valid ? &p_frame.other_weapons[i] : nullptr;
		const uint8_t team = w != nullptr && w->team != 0 ? anim::crowd_team_of_wire(w->team) : (s.known ? s.team : uint8_t(0));
		if (!s.known || s.generation != o.generation) {
			cam_new_life(o.slot, s); // M2b cam [corpse] [revive]: the corpse moves to its own index, or stands up
			s.dead = false; // a new life
			s.reload_known = false;
			s.fire_seq = 0;
		}
		s.known = true;
		s.team = team;
		s.generation = o.generation;
		s.present = true;
		anim::CrowdSoldierInput in;
		in.position = anim::Vec3{ o.position[0], o.position[1], o.position[2] };
		in.velocity = anim::Vec3{ o.velocity[0], o.velocity[1], o.velocity[2] };
		in.yaw = o.yaw;
		in.pitch = o.pitch;
		in.camera_distance = std::sqrt((in.position.x - cam.x) * (in.position.x - cam.x) + (in.position.y - cam.y) * (in.position.y - cam.y) +
				(in.position.z - cam.z) * (in.position.z - cam.z));
		in.team = team;
		in.pose = uint8_t(o.pose);
		in.target_pose = uint8_t(o.target_pose);
		in.transition = o.transition;
		if (w != nullptr) {
			if (w->def != sim::weapons::kNoWeapon) {
				const int32_t idx = render::crowd_weapon_index(*weapons, team, w->def);
				if (idx < 0) {
					++stats.unknown_weapons;
					s.weapon = 0;
				} else {
					s.weapon = uint8_t(idx);
				}
			}
			if (s.reload_known && w->reload_seq != s.reload_seq) {
				in.flags |= anim::crowd_flags::kReloadStart;
			}
			s.reload_seq = w->reload_seq;
			s.reload_known = true;
			if (!remote_shot_seen && w->firing && w->sample_tick != s.sample_tick) {
				s.fire_seq = uint8_t(s.fire_seq + 1);
			}
			s.sample_tick = w->sample_tick;
			in.ads = w->ads;
		}
		in.weapon = s.weapon;
		in.fire_seq = s.fire_seq;
		s.input = in;
	}
	// The inputs of every index: present soldiers, corpses, the rest hidden.
	for (anim::CrowdSoldierInput &in : inputs) {
		in = anim::CrowdSoldierInput{};
		in.flags = anim::crowd_flags::kHidden;
	}
	const double dt = std::clamp(p_frame.dt, 0.0, 0.25);
	for (uint32_t slot = 0; slot < kSlotsPerTeam; ++slot) {
		Slot &s = slots[slot];
		if (!s.known) {
			continue;
		}
		if (cam_m2b && slot >= kCamLaneOffset) { // M2b cam [corpse]: the index belongs to slot - 64's second lane
			++cam_counts[2]; // M2b cam [corpse]
			continue; // M2b cam [corpse]
		} // M2b cam [corpse]
		if (s.dead) {
			s.dead_seconds += dt;
		}
		const bool corpse = s.dead && s.dead_seconds < double(corpse_seconds + kCorpseExtraSeconds); // M2b cam [corpse]
		if (!s.present && !corpse) {
			continue;
		}
		const uint32_t index = uint32_t(s.team) * kSlotsPerTeam + slot + uint32_t(s.lane) * kCamLaneOffset; // M2b cam [corpse]
		anim::CrowdSoldierInput in = s.input;
		in.dead = s.dead;
		in.revived = s.revived; // M2b cam [revive]
		if (!s.present) {
			in.velocity = anim::Vec3{};
			in.flags = 0; // a corpse: no reload start
		}
		inputs[index] = in;
		const anim::Xform root{ yaw_quat(in.yaw), in.position };
		root_x[index] = root;
		roots[index] = Transform3D(Basis(Vector3(0, 1, 0), in.yaw), Vector3(in.position.x, in.position.y, in.position.z));
	}
	cam_draw_corpses(dt, cam); // M2b cam [corpse]
}

void Bf3GameCrowd::Impl::draw_weapons(const M2aFrame &p_frame) {
	(void)p_frame;
	RenderingServer *rs = RenderingServer::get_singleton();
	muzzles.clear();
	stats.weapon_instances = 0;
	stats.weapon_rebases = 0;
	stats.soldiers = 0;
	for (uint32_t i = 0; i < kCrowdSoldiers; ++i) {
		const bool drawn = eval->drawn(i) && (inputs[i].flags & anim::crowd_flags::kHidden) == 0;
		if (draw && drawn != soldier_visible[i]) {
			uploader->set_soldier_visible(i, drawn);
		}
		soldier_visible[i] = drawn;
		WeaponDraw &d = weapon_draws[i];
		const uint32_t team = i / kSlotsPerTeam;
		const uint8_t item = drawn ? eval->weapon_of(i) : anim::kNoCrowdWeapon;
		const bool has_item = item != anim::kNoCrowdWeapon && team < 2 && item < weapon_meshes[team].size() && weapon_meshes[team][item].binds > 0;
		const bool has_mesh = has_item && (!draw || weapon_meshes[team][item].mesh.is_valid());
		auto hide_weapon = [&] {
			if (d.visible) {
				if (draw) {
					rs->instance_set_visible(d.instance, false);
				}
				d.visible = false;
			}
		};
		if (!drawn) {
			hide_weapon();
			continue;
		}
		++stats.soldiers;
		if (!has_mesh) {
			hide_weapon();
		} else {
			const WeaponMesh &wm = weapon_meshes[team][item];
			bool rebased = false;
			if (draw && !d.instance.is_valid()) {
				d.instance = rs->instance_create();
				d.skeleton = rs->skeleton_create();
				rs->instance_set_scenario(d.instance, scenario);
			}
			if (d.item != item) {
				if (draw) {
					rs->instance_set_base(d.instance, wm.mesh->get_rid());
					rs->skeleton_allocate_data(d.skeleton, wm.binds, false);
					rs->instance_attach_skeleton(d.instance, d.skeleton);
					rs->instance_set_custom_aabb(d.instance, wm.aabb);
				}
				d.item = item;
				d.binds = wm.binds;
				rebased = true;
				++stats.weapon_rebases;
			}
			if (!d.visible) {
				if (draw) {
					rs->instance_set_visible(d.instance, true);
				}
				d.visible = true;
			}
			if (draw) {
				rs->instance_set_transform(d.instance, roots[i]);
				if (rebased || eval->changed(i)) {
					const std::span<const anim::Mat34> palette = eval->weapon_palette(i);
					const int32_t n = std::min<int32_t>(d.binds, int32_t(palette.size()));
					for (int32_t b = 0; b < n; ++b) {
						rs->skeleton_bone_set_transform(d.skeleton, b, to_godot(palette[size_t(b)]));
					}
				}
			}
			++stats.weapon_instances;
		}
		anim::Xform mz;
		if (eval->muzzle_model(i, mz)) {
			fx::MuzzleSample m;
			m.slot = uint8_t(i % kSlotsPerTeam);
			m.valid = true;
			m.first_person = false;
			m.transform = fx_transform(anim::xform_mul(root_x[i], mz));
			m.velocity = fv(inputs[i].velocity);
			muzzles.push_back(m);
		}
	}
	stats.muzzles = uint32_t(muzzles.size());
}

void Bf3GameCrowd::Impl::clear_draw() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr) {
		for (WeaponDraw &d : weapon_draws) {
			if (d.instance.is_valid()) {
				rs->free_rid(d.instance);
			}
			if (d.skeleton.is_valid()) {
				rs->free_rid(d.skeleton);
			}
		}
	}
	weapon_draws.clear();
	weapon_meshes = {};
	if (uploader) {
		uploader->clear();
		uploader.reset();
	}
}

Bf3GameCrowd::Bf3GameCrowd() :
		impl_(std::make_unique<Impl>()) {}

Bf3GameCrowd::~Bf3GameCrowd() {
	shutdown();
}

// M2a w3 integration (E16): the half of the load that needs no SharedLevel (the soldier assets, lc's stance clips, tp's
// moves, the aim measure). The level session runs it on its own thread beside load_shared_level (m2a_load_options);
// load() runs it itself when nobody did.
core::Status Bf3GameCrowd::load_body(const core::InstallReader &p_reader, core::Cache *p_cache, const std::atomic<bool> *p_stop) {
	Impl &m = *impl_;
	if (m.loaded.load(std::memory_order_acquire) || m.body != nullptr) {
		return core::make_error("crowd: already loaded");
	}
	auto stopped = [&] { return p_stop != nullptr && p_stop->load(std::memory_order_relaxed); };
	const auto clock_s = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
	const double t_start = clock_s();
	render::SoldierSpec spec;
	// The clip package's Ant source (about 1.1 s warm on Metro) opens on a second thread while the soldier assets load;
	// it is joined before any return.
	std::optional<core::Result<std::shared_ptr<core::Install>>> install;
	std::optional<core::Result<std::unique_ptr<core::ant::AntInstallSource>>> source;
	std::thread ant_opener([&] {
		install.emplace(core::Install::open(p_reader));
		if (*install) {
			source.emplace(core::ant::AntInstallSource::open(**install));
		}
	});
	core::Result<std::unique_ptr<render::SoldierAssets>> assets = render::load_soldier_assets(p_reader, spec, nullptr, p_cache);
	ant_opener.join();
	if (!assets) {
		return core::error_at("crowd soldier assets", assets.error());
	}
	if (stopped()) {
		return core::make_error("crowd: stopped");
	}
	const double t_assets = clock_s();
	m.body_notes.clear();
	// lc's stance clips and tp's moves, from the clip set's package (a failure is a note: the stand clips stand in).
	{
		if (install && *install) {
			if (source && *source) {
				core::Result<anim::ServerPoseClips> pose = anim::load_server_pose_clips(***source, *(*assets)->library);
				if (pose) {
					m.pose = std::move(*pose);
					m.have_pose = true;
				} else {
					m.body_notes.push_back("stance clips: " + pose.error().message + " (every stance drawn standing)");
				}
			}
		}
		core::Result<anim::CrowdMoveClips> moves = anim::load_crowd_move_clips((*assets)->clips, *(*assets)->library);
		if (moves) {
			m.moves = std::move(*moves);
			m.have_moves = true;
			for (const std::string &n : m.moves.notes) {
				m.body_notes.push_back("move clips: " + n);
			}
		} else {
			m.body_notes.push_back("move clips: " + moves.error().message);
		}
		core::Result<anim::CrowdMoveGroupSet> groups = anim::load_crowd_move_groups((*assets)->clips, *(*assets)->library); // PT1 run [load]
		if (groups) { // PT1 run [load]
			m.move_groups = std::move(*groups); // PT1 run [load]
			m.have_move_groups = true; // PT1 run [load]
			for (const std::string &n : m.move_groups.notes) { // PT1 run [load]
				m.body_notes.push_back("move groups: " + n); // PT1 run [load]
			} // PT1 run [load]
		} else { // PT1 run [load]
			m.body_notes.push_back("move groups: " + groups.error().message + " (S3's and tp's cycles)"); // PT1 run [load]
		} // PT1 run [load]
	}
	m.config = anim::CrowdConfig{};
	m.config.lod = (*assets)->lod;
	core::Result<anim::AimMeasure> aim = anim::measure_aim((*assets)->clips, m.config.aim_order);
	if (aim && aim->qualifies()) {
		m.config.aim_up_full_pitch_deg = aim->up_deg;
		m.config.aim_down_full_pitch_deg = -aim->down_deg;
	}
	m.body_seconds[0] = t_assets - t_start;
	m.body_seconds[1] = clock_s() - t_assets;
	m.body = std::move(*assets);
	return core::Status::success();
}

core::Status Bf3GameCrowd::load(const M2aLoadInputs &p_in) {
	Impl &m = *impl_;
	if (m.loaded.load(std::memory_order_acquire)) {
		return core::make_error("crowd: already loaded");
	}
	if (p_in.db == nullptr || !p_in.db->install) {
		return core::make_error("crowd: no install");
	}
	auto stopped = [&] { return p_in.stop != nullptr && p_in.stop->load(std::memory_order_relaxed); };
	const core::InstallReader &reader = p_in.db->install->reader();
	const bool body_before = m.body != nullptr;
	if (!body_before) {
		if (const core::Status s = load_body(reader, p_in.cache, p_in.stop); !s.ok()) {
			return s;
		}
	}
	if (stopped()) {
		return core::make_error("crowd: stopped");
	}
	const auto clock_s = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
	const double t_start = clock_s();
	render::SoldierSpec spec;
	const host::GunplayLevel *gunplay = p_in.shared ? p_in.shared->gunplay.get() : nullptr;
	const CrowdItemLists lists = crowd_item_specs(gunplay, spec);
	core::Result<std::unique_ptr<render::CrowdWeaponSet>> weapons =
			render::load_crowd_weapons(reader, spec, *m.body, { std::span<const render::CrowdWeaponSpec>(lists.teams[0]), std::span<const render::CrowdWeaponSpec>(lists.teams[1]) },
					p_in.cache);
	if (!weapons) {
		return core::error_at("crowd weapons", weapons.error());
	}
	for (const std::string &n : lists.notes) {
		(*weapons)->notes.push_back(n);
	}
	for (const std::string &n : m.body_notes) {
		(*weapons)->notes.push_back(n);
	}
	m.body_notes.clear();
	if (stopped()) {
		return core::make_error("crowd: stopped");
	}
	{
		char line[224];
		std::snprintf(line, sizeof(line), "load: soldier assets %.2f s, stance and move clips and aim %.2f s (%s), weapons %.2f s", m.body_seconds[0], m.body_seconds[1],
				body_before ? "on the session's crowd thread" : "here", clock_s() - t_start);
		(*weapons)->notes.push_back(line);
	}
	// The body meshes without the default weapon part (the items draw the weapons).
	render::strip_team_weapon_parts(*m.body);
	m.assets = std::move(m.body);
	m.weapons = std::move(*weapons);
	m.teams.assign(2, anim::CrowdTeam{});
	for (uint32_t t = 0; t < 2; ++t) {
		const render::SoldierTeam &team = m.assets->teams[t];
		m.views[t] = render::crowd_weapons_of(*m.weapons, t);
		m.teams[t].binds = &team.binds;
		m.teams[t].reload = &team.reload;
		m.teams[t].weapon_joints = &team.weapon_joints;
		m.teams[t].weapon_local = team.weapon_local;
		m.teams[t].weapons = m.views[t];
	}
	m.loaded.store(true, std::memory_order_release);
	return core::Status::success();
}

bool Bf3GameCrowd::loaded() const {
	return impl_->loaded.load(std::memory_order_acquire);
}

bool Bf3GameCrowd::build(Node3D *p_parent, std::string *r_error) {
	Impl &m = *impl_;
	auto fail = [&](const std::string &p_why) {
		if (r_error != nullptr) {
			*r_error = "crowd: " + p_why;
		}
		return false;
	};
	if (m.built) {
		return true;
	}
	if (!loaded()) {
		return fail("not loaded");
	}
	if (p_parent == nullptr || p_parent->get_world_3d().is_null()) {
		return fail("no parent node in a 3D world");
	}
	for (const std::string &n : m.assets->notes) {
		log(n);
	}
	for (const std::string &n : m.weapons->notes) {
		log(n);
	}
	m.scenario = p_parent->get_world_3d()->get_scenario();
	const unsigned hc = std::thread::hardware_concurrency();
	const uint32_t lanes = anim::crowd_lanes(hc, true);
	m.lanes = std::make_unique<core::WorkerGroup>(lanes, "crowd pose");
	m.stats.lanes = lanes;
	anim::CrowdStanceSet stance;
	stance.pose = m.have_pose ? &m.pose : nullptr;
	stance.moves = m.have_moves ? &m.moves : nullptr;
	stance.corpse_seconds = m.corpse_seconds; // M2b cam [corpse]
	core::Result<std::unique_ptr<anim::CrowdEvaluator>> e = anim::CrowdEvaluator::make(m.assets->clips, stance, m.teams, kCrowdSoldiers, m.config);
	if (!e) {
		return fail(e.error().message);
	}
	m.eval = std::move(*e);
	std::vector<uint8_t> team_of(kCrowdSoldiers);
	for (uint32_t i = 0; i < kCrowdSoldiers; ++i) {
		team_of[i] = uint8_t(i / kSlotsPerTeam);
	}
	m.uploader = make_skin_uploader(SkinBackend::RsSkeleton, p_parent);
	if (!m.uploader) {
		return fail("no RS uploader");
	}
	std::string err;
	if (!m.uploader->build(*m.assets, *m.eval, team_of, m.scenario, &err)) {
		m.uploader->clear();
		m.uploader.reset();
		if (DisplayServer::get_singleton()->get_name() != String("headless")) {
			m.clear_draw();
			return fail(err);
		}
		m.draw = false;
		log("headless display: " + err + "; posing without drawing (muzzles still published)");
	}
	m.soldier_visible.assign(kCrowdSoldiers, false);
	for (uint32_t i = 0; m.draw && i < kCrowdSoldiers; ++i) {
		m.uploader->set_soldier_visible(i, false);
	}
	// The items' meshes: one RS mesh per (team, item), textures from the soldier set or the weapon set.
	std::map<std::string, Ref<Texture2D>> textures;
	auto find_texture = [&](const std::string &p_name) -> Ref<Texture2D> {
		const auto it = textures.find(p_name);
		if (it != textures.end()) {
			return it->second;
		}
		const render::PreparedTexture *t = m.assets->texture(p_name);
		if (t == nullptr) {
			t = m.weapons->texture(p_name);
		}
		Ref<Texture2D> tex;
		if (t != nullptr) {
			tex = make_texture(*t);
		}
		textures[p_name] = tex;
		return tex;
	};
	for (uint32_t t = 0; t < 2; ++t) {
		for (const render::CrowdWeaponPart &part : m.weapons->teams[t]) {
			WeaponMesh wm;
			std::vector<Ref<Material>> materials;
			bool have = false;
			Vector3 lo, hi;
			for (const render::PreparedSurface &s : part.prepared.surfaces) {
				Ref<Material> mat;
				if (m.draw && s.material_index < part.materials.slots.size()) {
					mat = make_standard_material(part.materials.slots[s.material_index], !s.tangents.empty(), find_texture, MaterialOptions{});
				}
				materials.push_back(mat);
				const Vector3 a(s.aabb_min[0], s.aabb_min[1], s.aabb_min[2]), b(s.aabb_max[0], s.aabb_max[1], s.aabb_max[2]);
				lo = have ? lo.min(a) : a;
				hi = have ? hi.max(b) : b;
				have = true;
			}
			if (have && !m.draw) {
				wm.binds = int32_t(part.binds.size()); // posed and counted, not drawn
			} else if (have) {
				const render::PreparedSurface *first = part.prepared.surfaces.data();
				wm.mesh = make_array_mesh(part.prepared, [&](const render::PreparedSurface &p_s) -> Ref<Material> {
					const size_t index = size_t(&p_s - first);
					return index < materials.size() ? materials[index] : Ref<Material>();
				});
				// The bind-pose bounds grown by 1.5 m: the weapon is posed on the soldier (Wep_Root hangs under Spine2)
				// and a skinned instance's bounds are not recomputed (s3.md §7.1).
				const Vector3 grow(1.5f, 2.0f, 1.5f);
				wm.aabb = AABB(lo - grow, (hi - lo) + grow * 2.0f);
				wm.binds = int32_t(part.binds.size());
			}
			if (m.draw && wm.mesh.is_null()) {
				log("item " + part.spec.weapon_def + ": no mesh created (not drawn)");
			}
			m.weapon_meshes[t].push_back(wm);
		}
	}
	m.weapon_draws.assign(kCrowdSoldiers, Impl::WeaponDraw{});
	m.inputs.assign(kCrowdSoldiers, anim::CrowdSoldierInput{});
	m.roots.assign(kCrowdSoldiers, Transform3D());
	m.root_x.assign(kCrowdSoldiers, anim::xform_identity());
	char line[256];
	std::snprintf(line, sizeof(line), "built: %u soldier indices (%u slots x 2 teams), %u pose lanes, items %zu + %zu, stance clips %s, moves %s", kCrowdSoldiers,
			kSlotsPerTeam, lanes, m.weapon_meshes[0].size(), m.weapon_meshes[1].size(), m.have_pose ? "yes" : "no", m.have_moves ? "yes" : "no");
	log(line);
	m.built = true;
	return true;
}

bool Bf3GameCrowd::built() const {
	return impl_->built;
}

void Bf3GameCrowd::step(const M2aFrame &p_frame) {
	Impl &m = *impl_;
	if (!m.built || m.shut_down) {
		return;
	}
	m.build_inputs(p_frame);
	m.eval->wait_and_publish(*m.lanes);
	if (m.uploader) {
		m.uploader->upload(*m.eval, m.roots, m.upload_stats);
	}
	m.draw_weapons(p_frame);
	m.eval->set_inputs(m.inputs, std::clamp(p_frame.dt, 0.0, 0.25));
	if (!m.eval->kick(*m.lanes)) {
		++m.stats.frozen_frames;
	}
	++m.stats.frames;
}

std::span<const fx::MuzzleSample> Bf3GameCrowd::muzzles() const {
	return impl_->muzzles;
}

GameCrowdStats Bf3GameCrowd::stats() const {
	return impl_->stats;
}

void Bf3GameCrowd::shutdown() {
	Impl &m = *impl_;
	if (m.shut_down) {
		return;
	}
	m.shut_down = true;
	m.cam_log(); // M2b cam [corpse]
	if (m.eval && m.lanes) {
		m.eval->wait_and_publish(*m.lanes);
	}
	m.clear_draw();
	m.eval.reset();
	m.lanes.reset();
	m.muzzles.clear();
	m.built = false;
}

} // namespace bf3::godot_side

// ---- M2b cam (docs/design/m2b.md §4, §2.6 #17; as built: docs/design/m2b-cam.md) ---------------------------------
// The 3p corpse and the revive stand-up in the M2b scene (set_corpse_seconds before build() turns it on; M2a never
// calls it, so its crowd is unchanged):
//   * a corpse stays drawn for corpse_seconds (anim::kCrowdCorpseSeconds, 15 s) through CrowdStanceSet::corpse_seconds;
//   * every slot below 64 owns two crowd indices (team x 128 + slot, and + 64: the lanes; TDM's slots are below
//     net::kMaxPlayerRows, slots >= 64 are spectators and are not drawn here, counted). A slot that lives again while
//     its corpse is still drawn (a bot respawns after 5-8 s) keeps the corpse in a CamCorpse record on the lane it died
//     on and draws the new life on the other lane, so the corpse keeps its pose, place and time. An older corpse on
//     the lane the new life takes (two deaths within the corpse time) is cut short (counted);
//   * a Revive event (aux = the revived slot, applied when drained, before the render clock shows the new life) marks
//     the slot's next life as a revive: that life stays on the corpse's lane with CrowdSoldierInput::revived, and the
//     evaluator stands the corpse up over kCrowdReviveBlendSeconds at the same place (crowd_eval.cpp [revive]). A
//     Revive whose life does not show within kCamReviveWaitSeconds is dropped (counted);
//   * the own soldier is never in present_others, so the own Death event (aux = the own slot) draws the own body as a
//     corpse from the frame's own state (position, yaw, stance, team, held weapon) for the death camera to look at;
//     the own Spawn event (a deploy or a revive: the 1p view takes over) removes it.
namespace bf3::godot_side {

namespace {

constexpr double kCamReviveWaitSeconds = 2.0; // [inferred]: the revived life shows in the snapshots well within this

} // namespace

void Bf3GameCrowd::set_corpse_seconds(float p_seconds) {
	Impl &m = *impl_;
	if (m.built) {
		log("set_corpse_seconds after build(): ignored (the evaluator was made with " + std::to_string(m.corpse_seconds) + " s)");
		return;
	}
	if (!std::isfinite(p_seconds) || p_seconds < 0.0f || p_seconds > anim::kCrowdMaxCorpseSeconds) {
		log("set_corpse_seconds(" + std::to_string(p_seconds) + "): outside [0, " + std::to_string(anim::kCrowdMaxCorpseSeconds) + "], using " +
				std::to_string(anim::kCrowdCorpseSeconds));
		p_seconds = anim::kCrowdCorpseSeconds;
	}
	m.corpse_seconds = p_seconds;
	m.cam_m2b = true;
}

bool Bf3GameCrowd::Impl::cam_event(const host::PresentEvent &p_e, const M2aFrame &p_frame) {
	if (p_e.kind == host::PresentKind::Revive) {
		if (p_e.aux < kSlotsPerTeam && p_e.aux != p_frame.own_slot) {
			slots[p_e.aux].revive_wait = 0.0;
		}
		return true; // the pending list has no use for it
	}
	if (!cam_m2b || p_frame.own_slot >= kCamLaneOffset) {
		return false;
	}
	const uint8_t own = p_frame.own_slot;
	if (p_e.kind == host::PresentKind::Spawn && p_e.slot == own) {
		for (CamCorpse &c : cam_corpses[own]) {
			c.active = false; // the 1p view takes over (a deploy or the own revive)
		}
		return false;
	}
	if (p_e.kind != host::PresentKind::Death || p_e.aux != own) {
		return false;
	}
	const host::PresentationFrame *f = p_frame.frame;
	if (f == nullptr || !f->valid) {
		return true;
	}
	const sim::SoldierState &st = f->own_curr;
	const anim::Vec3 pos{ st.position[0] + f->visual_offset[0], st.position[1] + f->visual_offset[1], st.position[2] + f->visual_offset[2] };
	if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) {
		return true;
	}
	const uint8_t wire_team = f->own_team != 0 ? f->own_team : p_frame.own_team;
	const uint8_t team = anim::crowd_team_of_wire(wire_team);
	CamCorpse &c = cam_corpses[own][0];
	c.input = anim::CrowdSoldierInput{};
	c.input.position = pos;
	c.input.yaw = sim::yaw_radians(st.yaw);
	c.input.pose = uint8_t(std::min<uint32_t>(uint32_t(st.pose), 2));
	c.input.target_pose = uint8_t(std::min<uint32_t>(uint32_t(st.target_pose), 2));
	c.input.team = team;
	c.input.dead = true;
	c.input.weapon = 0;
	if (f->own_weapons_valid && f->own_weapons_curr.active < f->own_weapons_curr.slots.size()) {
		const uint16_t def = f->own_weapons_curr.slots[f->own_weapons_curr.active].def;
		const int32_t idx = def != sim::weapons::kNoWeapon ? render::crowd_weapon_index(*weapons, team, def) : -1;
		c.input.weapon = idx >= 0 ? uint8_t(idx) : uint8_t(0);
	}
	c.index = uint32_t(team) * kSlotsPerTeam + own;
	c.seconds = 0.0;
	c.active = true;
	cam_corpses[own][1].active = false;
	++cam_counts[4];
	return true;
}

void Bf3GameCrowd::Impl::cam_new_life(uint32_t p_slot, Slot &r_s) {
	if (!r_s.known) {
		return; // the slot's first life
	}
	if (r_s.revive_wait >= 0.0) {
		// The revived life: the corpse's lane; the evaluator stands the corpse up.
		r_s.revive_wait = -1.0;
		r_s.revived = true;
		return;
	}
	r_s.revived = false;
	if (!cam_m2b || p_slot >= kCamLaneOffset) {
		return;
	}
	const bool corpse = r_s.dead && r_s.dead_seconds < double(corpse_seconds + kCorpseExtraSeconds);
	if (!corpse) {
		return;
	}
	CamCorpse &c = cam_corpses[p_slot][r_s.lane];
	c.active = true;
	c.index = uint32_t(r_s.team) * kSlotsPerTeam + p_slot + uint32_t(r_s.lane) * kCamLaneOffset;
	c.seconds = r_s.dead_seconds;
	c.input = r_s.input;
	c.input.dead = true;
	c.input.revived = false;
	c.input.velocity = anim::Vec3{};
	c.input.flags = 0;
	r_s.lane = uint8_t(r_s.lane ^ 1u);
	++cam_counts[0];
	// The lane the new life takes: the life's team is only known after this call, so an older corpse there goes.
	CamCorpse &other = cam_corpses[p_slot][r_s.lane];
	if (other.active) {
		other.active = false;
		++cam_counts[1];
	}
}

void Bf3GameCrowd::Impl::cam_draw_corpses(double p_dt, const anim::Vec3 &p_camera) {
	for (Slot &s : slots) {
		if (s.revive_wait >= 0.0) {
			s.revive_wait += p_dt;
			if (s.revive_wait > kCamReviveWaitSeconds) {
				s.revive_wait = -1.0;
				++cam_counts[3];
			}
		}
	}
	if (!cam_m2b) {
		return;
	}
	for (uint32_t slot = 0; slot < kCamLaneOffset; ++slot) {
		for (CamCorpse &c : cam_corpses[slot]) {
			if (!c.active) {
				continue;
			}
			c.seconds += p_dt;
			if (c.seconds >= double(corpse_seconds + kCorpseExtraSeconds) || c.index >= inputs.size()) {
				c.active = false;
				continue;
			}
			anim::CrowdSoldierInput in = c.input;
			in.camera_distance = std::sqrt((in.position.x - p_camera.x) * (in.position.x - p_camera.x) + (in.position.y - p_camera.y) * (in.position.y - p_camera.y) +
					(in.position.z - p_camera.z) * (in.position.z - p_camera.z));
			inputs[c.index] = in;
			root_x[c.index] = anim::Xform{ yaw_quat(in.yaw), in.position };
			roots[c.index] = godot::Transform3D(godot::Basis(godot::Vector3(0, 1, 0), in.yaw), godot::Vector3(in.position.x, in.position.y, in.position.z));
		}
	}
}

void Bf3GameCrowd::Impl::cam_log() const {
	if (!cam_m2b) {
		return;
	}
	char line[320];
	std::snprintf(line, sizeof(line),
			"M2b corpses (%.1f s): %llu kept on the second lane, %llu cut short, %llu own corpses, %llu slot-frames >= 64 skipped, %llu revives without a new life; "
			"evaluator revives %llu",
			double(corpse_seconds), (unsigned long long)cam_counts[0], (unsigned long long)cam_counts[1], (unsigned long long)cam_counts[4],
			(unsigned long long)cam_counts[2], (unsigned long long)cam_counts[3], (unsigned long long)(eval ? eval->m2a_stats().revives : 0));
	log(line);
}

} // namespace bf3::godot_side
