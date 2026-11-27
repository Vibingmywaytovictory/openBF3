#include "game_crowd.h"

#include "game_crowd_items.h"
#include "godot_util.h"
#include "render_bridge.h"
#include "skin_upload.h"

#include "bf3/anim/death_anim.h"
#include "bf3/anim/server_pose.h"
#include "bf3/anim/viewmodel_anim.h"
#include "bf3/audio/footstep_driver.h"
#include "bf3/core/ant/ant_install_source.h"
#include "bf3/core/gameplay/death_body.h"
#include "bf3/core/install.h"
#include "bf3/core/job_queue.h"
#include "bf3/core/level/level.h"
#include "bf3/core/res/havok_ragdoll.h"
#include "bf3/core/worker_group.h"
#include "bf3/host/loadout.h"
#include "bf3/net/protocol.h"
#include "bf3/render/soldier_assets.h"
#include "bf3/sim/ragdoll_world.h"

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
#include <mutex>
#include <numbers>
#include <optional>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bf3::godot_side {

using namespace godot;

namespace {

constexpr uint32_t kSlotsPerTeam = uint32_t(net::kMaxSlots);
constexpr uint32_t kCrowdSoldiers = kSlotsPerTeam * anim::kMaxCrowdTeams;
static_assert(kCrowdSoldiers <= anim::kMaxCrowdSoldiers);
constexpr size_t kMaxPending = 1024;
// WHY: a corpse's input outlives the evaluator's own hide by this margin, so the evaluator ends the corpse, not the input.
constexpr float kCorpseExtraSeconds = 0.5f;
constexpr uint32_t kCamLaneOffset = uint32_t(net::kMaxPlayerRows);
static_assert(kCamLaneOffset * 2 == kSlotsPerTeam);

void log(const std::string &p_line) {
	UtilityFunctions::print(to_godot("[crowd] " + p_line));
}

anim::Quat yaw_quat(float p_yaw) {
	return anim::Quat{ 0.0f, std::sin(0.5f * p_yaw), 0.0f, std::cos(0.5f * p_yaw) };
}

anim::Vec3 rotate(const anim::Quat &p_q, const anim::Vec3 &p_v) {
	return anim::xform_mul(anim::Xform{ p_q, anim::Vec3{} }, anim::Xform{ anim::Quat{}, p_v }).t;
}

fx::Transform fx_transform(const anim::Xform &p_x) {
	fx::Transform t;
	t.right = rotate(p_x.q, anim::Vec3{ 1, 0, 0 });
	t.up = rotate(p_x.q, anim::Vec3{ 0, 1, 0 });
	t.forward = rotate(p_x.q, anim::Vec3{ 0, 0, 1 });
	t.trans = p_x.t;
	return t;
}

struct WeaponMesh {
	Ref<ArrayMesh> mesh;
	AABB aabb;
	int32_t binds = 0;
	size_t accessory_meshes = 0;
};

using GearPicks = std::array<uint32_t, host::kLoadoutAccessories>;

struct CatalogPart {
	uint8_t team = 0;
	uint16_t def = 0;
	GearPicks picks{};
	std::unique_ptr<render::CrowdWeaponSet> set;
	std::string error;
	double seconds = 0.0;
};
}

struct Bf3GameCrowd::Impl {

	std::unique_ptr<render::SoldierAssets> assets;
	std::unique_ptr<render::CrowdWeaponSet> weapons;
	anim::ServerPoseClips pose;
	bool have_pose = false;
	anim::CrowdMoveClips moves;
	bool have_moves = false;
	anim::CrowdMoveGroupSet move_groups;
	bool have_move_groups = false;
	anim::CrowdDeathClips death_clips;
	bool have_death_clips = false;
	std::shared_ptr<const core::res::RagdollDef> ragdoll_def;
	std::unique_ptr<sim::RagdollWorld> ragdoll_world;
	anim::RagdollRig ragdoll_rig;
	bool have_ragdoll = false;
	struct Corpse {
		bool active = false;
		bool in_world = false;
		bool copied = false;
		bool have_previous = false;
		bool done = false;
		core::gameplay::RagdollBlend blend;
		core::gameplay::RagdollSettle settle;
		uint8_t clip = anim::kNoCrowdDeathClip;
		double age = 0.0;
		float copy_dt = 0.0f;
		std::vector<anim::Xform> bodies, previous, joints;
		std::vector<sim::RagdollBodyPose> poses;
		GameCrowdCorpse seen;
		anim::Vec3 facing_position{};
		float facing_yaw = 0.0f;
	};
	std::vector<Corpse> corpses;
	uint64_t ragdolls_started = 0, ragdolls_frozen = 0, ragdoll_failures = 0, ragdoll_revives = 0;
	float ragdoll_hips_height = -1.0f;
	float ragdoll_weight = 0.0f;
	int32_t ragdoll_hips = -1;
	int32_t ragdoll_head = -1;
	void step_ragdolls(double p_dt);
	void begin_revive(Corpse &r_c, const anim::CrowdSoldierInput &p_in);
	void revive_roots();
	uint8_t death_clip_of(const core::gameplay::DeathRecord &p_record, uint8_t p_pose) const;
	anim::CrowdConfig config;
	std::array<std::vector<anim::CrowdWeapon>, 2> views;
	std::vector<anim::CrowdTeam> teams;
	std::atomic<bool> loaded{ false };

	std::unique_ptr<render::SoldierAssets> body;
	std::vector<std::string> body_notes;
	std::array<double, 2> body_seconds{};

	bool built = false;
	std::unique_ptr<core::WorkerGroup> lanes;
	std::unique_ptr<anim::CrowdEvaluator> eval;
	std::unique_ptr<SkinUploader> uploader;

	bool draw = true;
	RID scenario;
	std::array<std::vector<WeaponMesh>, 2> weapon_meshes;
	struct WeaponDraw {
		RID instance;
		RID skeleton;
		uint8_t item = anim::kNoCrowdWeapon;
		int32_t binds = 0;
		bool visible = false;
	};
	std::vector<WeaponDraw> weapon_draws;
	std::vector<bool> soldier_visible;

	struct Slot {
		bool known = false;
		uint8_t team = 0;
		uint8_t generation = 0;
		bool present = false;
		bool dead = false;
		double dead_seconds = 0.0;
		anim::CrowdSoldierInput input;
		uint8_t fire_seq = 0;
		uint8_t reload_seq = 0;
		bool reload_known = false;
		uint32_t sample_tick = 0;
		uint8_t weapon = 0;
		uint8_t lane = 0;
		bool revived = false;
		double revive_wait = -1.0;
		uint8_t kit = 0;
		bool vault_held = false;
		float vault_y = 0.0f;
		uint8_t death_clip = anim::kNoCrowdDeathClip;
	};
	std::array<Slot, kSlotsPerTeam> slots{};
	std::array<uint8_t, kSlotsPerTeam> plants{};
	void collect_plants();

	struct CamCorpse {
		bool active = false;
		uint32_t index = 0;
		double seconds = 0.0;
		anim::CrowdSoldierInput input;
	};
	std::array<std::array<CamCorpse, 2>, kSlotsPerTeam> cam_corpses{};
	float corpse_seconds = anim::kCrowdDeathHideSeconds;
	bool corpses_kept = false;
	uint64_t cam_counts[5] = {};
	bool cam_event(const host::PresentEvent &p_e, const GameFrame &p_frame);
	void cam_new_life(uint32_t p_slot, Slot &r_s);
	void cam_draw_corpses(double p_dt, const anim::Vec3 &p_camera);
	void cam_log() const;
	std::array<uint8_t, kCrowdSoldiers> kit_of{};
	static uint8_t row_kit(const host::PresentationFrame *p_frame, uint8_t p_slot, uint8_t p_fallback);
	struct Pending {
		host::PresentKind kind;
		host::PresentTimeline timeline;
		uint8_t slot;
		int64_t time;
		core::gameplay::DeathRecord record;
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
	void build_inputs(const GameFrame &p_frame);
	void draw_weapons(const GameFrame &p_frame);
	void clear_draw();

	std::shared_ptr<const core::Install> catalog_install;
	std::shared_ptr<const host::SharedLevel> catalog_level;
	render::SoldierSpec catalog_spec;
	std::function<std::shared_ptr<core::Cache>()> catalog_cache;
	std::unique_ptr<core::EbxLibrary> catalog_ebx;
	std::mutex catalog_mutex;
	std::vector<CatalogPart> catalog_done;
	std::set<std::pair<uint8_t, uint16_t>> catalog_requested;
	std::vector<std::unique_ptr<render::CrowdWeaponSet>> catalog_sets;
	std::array<std::unordered_map<uint16_t, uint8_t>, 2> weapon_index;
	std::array<std::map<std::pair<uint16_t, GearPicks>, uint8_t>, 2> gear_index;
	std::set<std::tuple<uint8_t, uint16_t, GearPicks>> gear_requested;
	std::map<std::string, Ref<Texture2D>> textures;
	uint32_t catalog_added = 0, catalog_failed = 0;
	std::unique_ptr<core::JobQueue> catalog_jobs;
	std::optional<std::array<std::vector<render::CrowdWeaponVaults>, 2>> vaults_done;
	std::vector<std::string> vault_notes;
	double vault_seconds = 0.0;

	int32_t index_of(uint8_t p_team, uint16_t p_def);
	int32_t gear_index_of(uint8_t p_team, const net::SoldierGear &p_gear);
	void want(uint8_t p_team, uint16_t p_def);
	void want_gear(uint8_t p_team, const net::SoldierGear &p_gear);
	void load_catalog(uint8_t p_team, const render::CrowdWeaponSpec &p_spec, const GearPicks &p_picks = {});
	void adopt_catalog();
	void load_vaults();
	void adopt_vaults();
	Ref<Texture2D> find_texture(const std::string &p_name);
	WeaponMesh make_weapon_mesh(const render::CrowdWeaponPart &p_part);
};

int32_t Bf3GameCrowd::Impl::index_of(uint8_t p_team, uint16_t p_def) {
	if (p_team >= weapon_index.size()) {
		return -1;
	}
	const auto it = weapon_index[p_team].find(p_def);
	if (it != weapon_index[p_team].end()) {
		return it->second;
	}
	want(p_team, p_def);
	return -1;
}

int32_t Bf3GameCrowd::Impl::gear_index_of(uint8_t p_team, const net::SoldierGear &p_gear) {
	if (p_team < gear_index.size()) {
		const auto it = gear_index[p_team].find({ p_gear.def, p_gear.accessories });
		if (it != gear_index[p_team].end()) {
			return it->second;
		}
		want_gear(p_team, p_gear);
	}
	return index_of(p_team, p_gear.def);
}

void Bf3GameCrowd::Impl::want_gear(uint8_t p_team, const net::SoldierGear &p_gear) {
	const host::GunplayLevel *gunplay = catalog_level != nullptr ? catalog_level->gunplay.get() : nullptr;
	if (catalog_jobs == nullptr || p_team >= gear_index.size() || gunplay == nullptr ||
			p_gear.def >= gunplay->defs.size() || p_gear.accessories == GearPicks{} ||
			!gear_requested.insert({ p_team, p_gear.def, p_gear.accessories }).second) {
		return;
	}
	const host::LoadoutItem *item = host::loadout_primary_item(gunplay->loadouts, uint8_t(p_team + 1), p_gear.def);
	if (item == nullptr) {
		return;
	}
	const host::LoadoutAccessorySet set = host::loadout_accessory_set(*item, p_gear.accessories);
	if (set.picked == GearPicks{}) {
		return;
	}
	render::CrowdWeaponSpec spec = catalog_crowd_spec(*gunplay, p_gear.def);
	spec.accessories = render::WeaponAccessories{ set.active, set.listed };
	const GearPicks picks = p_gear.accessories;
	catalog_jobs->post([this, p_team, spec, picks] { load_catalog(p_team, spec, picks); });
}

void Bf3GameCrowd::Impl::want(uint8_t p_team, uint16_t p_def) {
	if (catalog_jobs == nullptr || p_team >= weapon_index.size() || p_def == sim::weapons::kNoWeapon ||
			weapon_index[p_team].count(p_def) != 0) {
		return;
	}
	const host::GunplayLevel *gunplay = catalog_level != nullptr ? catalog_level->gunplay.get() : nullptr;
	if (gunplay == nullptr || p_def >= gunplay->defs.size() || !catalog_requested.insert({ p_team, p_def }).second) {
		return;
	}
	const render::CrowdWeaponSpec spec = catalog_crowd_spec(*gunplay, p_def);
	catalog_jobs->post([this, p_team, spec] { load_catalog(p_team, spec); });
}

void Bf3GameCrowd::Impl::load_catalog(uint8_t p_team, const render::CrowdWeaponSpec &p_spec, const GearPicks &p_picks) {
	const auto t0 = std::chrono::steady_clock::now();
	CatalogPart out;
	out.team = p_team;
	out.def = p_spec.def;
	out.picks = p_picks;
	if (catalog_ebx == nullptr) {
		core::Result<std::unique_ptr<core::EbxLibrary>> ebx =
				core::EbxLibrary::open(catalog_install, { catalog_spec.superbundle });
		if (ebx) {
			catalog_ebx = std::move(*ebx);
		} else {
			out.error = catalog_spec.superbundle + ": " + ebx.error().message;
		}
	}
	if (catalog_ebx != nullptr) {
		const std::shared_ptr<core::Cache> cache = catalog_cache ? catalog_cache() : nullptr;
		std::array<std::span<const render::CrowdWeaponSpec>, 2> items{};
		items[p_team] = std::span<const render::CrowdWeaponSpec>(&p_spec, 1);
		core::Result<std::unique_ptr<render::CrowdWeaponSet>> set =
				render::load_crowd_weapons(*catalog_ebx, catalog_spec, *assets, items, cache.get());
		if (!set) {
			out.error = set.error().message;
		} else if ((*set)->teams[p_team].size() != 1) {
			out.error = std::to_string((*set)->teams[p_team].size()) + " parts";
		} else {
			out.set = std::move(*set);
		}
	}
	out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::lock_guard lock(catalog_mutex);
	catalog_done.push_back(std::move(out));
}

void Bf3GameCrowd::Impl::load_vaults() {
	const auto t0 = std::chrono::steady_clock::now();
	std::vector<std::string> notes;
	std::array<std::vector<render::CrowdWeaponVaults>, 2> loaded =
			render::load_crowd_weapon_vaults(*assets, *weapons, notes);
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::lock_guard lock(catalog_mutex);
	vaults_done = std::move(loaded);
	vault_notes = std::move(notes);
	vault_seconds = seconds;
}

void Bf3GameCrowd::Impl::adopt_vaults() {
	std::optional<std::array<std::vector<render::CrowdWeaponVaults>, 2>> done;
	std::vector<std::string> notes;
	double seconds = 0.0;
	{
		std::lock_guard lock(catalog_mutex);
		done.swap(vaults_done);
		notes.swap(vault_notes);
		seconds = vault_seconds;
	}
	if (!done || eval == nullptr) {
		return;
	}
	uint32_t clips = 0, failures = 0;
	for (uint32_t t = 0; t < 2; ++t) {
		for (size_t i = 0; i < (*done)[t].size() && i < weapons->teams[t].size() && i < views[t].size(); ++i) {
			render::CrowdWeaponPart &part = weapons->teams[t][i];
			part.have_vault = (*done)[t][i].have_vault;
			part.vault = std::move((*done)[t][i].vault);
			for (size_t k = 0; k < anim::kVaultAnimKinds; ++k) {
				const anim::CrowdVaultClip *clip = part.have_vault[k] ? &part.vault[k] : nullptr;
				views[t][i].vault[k] = clip;
				if (clip == nullptr) {
					continue;
				}
				if (const core::Status st = eval->set_weapon_vault(t, uint8_t(i), k, clip); !st.ok()) {
					++failures;
					log("vault: " + st.error().message);
					continue;
				}
				++clips;
			}
		}
	}
	for (const std::string &n : notes) {
		log("vault: " + n);
	}
	char line[160];
	std::snprintf(line, sizeof(line),
			"vault clips: %u for %zu + %zu weapons in %.2f s on the catalog queue, %u refused", clips,
			(*done)[0].size(), (*done)[1].size(), seconds, failures);
	log(line);
}

void Bf3GameCrowd::Impl::adopt_catalog() {
	std::vector<CatalogPart> done;
	{
		std::lock_guard lock(catalog_mutex);
		done.swap(catalog_done);
	}
	for (CatalogPart &c : done) {
		const std::string what = "catalog weapon def " + std::to_string(c.def) + " team " + std::to_string(c.team);
		if (c.set == nullptr || eval == nullptr) {
			++catalog_failed;
			log(what + ": " + (c.error.empty() ? std::string("the crowd is not built") : c.error));
			continue;
		}
		const render::CrowdWeaponPart &part = c.set->teams[c.team][0];
		const std::vector<anim::CrowdWeapon> view = render::crowd_weapons_of(*c.set, c.team);
		core::Result<uint8_t> index = eval->add_weapon(c.team, view[0]);
		if (!index || *index != weapon_meshes[c.team].size()) {
			++catalog_failed;
			log(what + ": " +
					(index ? "index " + std::to_string(*index) + " is not the next mesh" : index.error().message));
			continue;
		}
		catalog_sets.push_back(std::move(c.set));
		weapon_meshes[c.team].push_back(make_weapon_mesh(part));
		if (c.picks != GearPicks{}) {
			gear_index[c.team].emplace(std::pair{ c.def, c.picks }, *index);
		} else {
			weapon_index[c.team].emplace(c.def, *index);
		}
		++catalog_added;
		char line[320];
		std::snprintf(line, sizeof(line),
				"%s (%s%s): item %u, %llu vertices, %zu accessory meshes, %zu binds, loaded in %.2f s", what.c_str(),
				part.spec.weapon_def.c_str(), c.picks != GearPicks{} ? ", picked accessories" : "", unsigned(*index),
				static_cast<unsigned long long>(part.prepared.vertices), part.accessory_meshes.size(),
				part.binds.size(), c.seconds);
		log(line);
	}
}

Ref<Texture2D> Bf3GameCrowd::Impl::find_texture(const std::string &p_name) {
	const auto it = textures.find(p_name);
	if (it != textures.end()) {
		return it->second;
	}
	const render::PreparedTexture *t = assets->texture(p_name);
	if (t != nullptr && t->data.empty()) {
		t = nullptr;
	}
	if (t == nullptr) {
		t = weapons->texture(p_name);
	}
	for (size_t i = catalog_sets.size(); t == nullptr && i-- > 0;) {
		t = catalog_sets[i]->texture(p_name);
	}
	Ref<Texture2D> tex;
	if (t != nullptr) {
		tex = make_texture(*t);
	}
	textures[p_name] = tex;
	return tex;
}

WeaponMesh Bf3GameCrowd::Impl::make_weapon_mesh(const render::CrowdWeaponPart &p_part) {
	WeaponMesh wm;
	wm.accessory_meshes = p_part.accessory_meshes.size();
	std::vector<Ref<Material>> materials;
	bool have = false;
	Vector3 lo, hi;
	const auto texture = [this](const std::string &p_name) { return find_texture(p_name); };
	for (const render::PreparedSurface &s : p_part.prepared.surfaces) {
		Ref<Material> mat;
		if (draw && s.material_index < p_part.materials.slots.size()) {
			mat = make_standard_material(
					p_part.materials.slots[s.material_index], !s.tangents.empty(), texture, MaterialOptions{});
		}
		materials.push_back(mat);
		const Vector3 a(s.aabb_min[0], s.aabb_min[1], s.aabb_min[2]), b(s.aabb_max[0], s.aabb_max[1], s.aabb_max[2]);
		lo = have ? lo.min(a) : a;
		hi = have ? hi.max(b) : b;
		have = true;
	}
	if (have && !draw) {
		wm.binds = int32_t(p_part.binds.size());
	} else if (have) {
		const render::PreparedSurface *first = p_part.prepared.surfaces.data();
		wm.mesh = make_array_mesh(p_part.prepared, [&](const render::PreparedSurface &p_s) -> Ref<Material> {
			const size_t index = size_t(&p_s - first);
			return index < materials.size() ? materials[index] : Ref<Material>();
		});

		const Vector3 grow(1.5f, 2.0f, 1.5f);
		wm.aabb = AABB(lo - grow, (hi - lo) + grow * 2.0f);
		wm.binds = int32_t(p_part.binds.size());
	}
	if (draw && wm.mesh.is_null()) {
		log("item " + p_part.spec.weapon_def + ": no mesh created (not drawn)");
	}
	return wm;
}

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
				s.revived = false;
				s.dead_seconds = 0.0;
				s.death_clip = death_clip_of(p_e.record, s.input.pose);
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

void Bf3GameCrowd::Impl::build_inputs(const GameFrame &p_frame) {

	for (const host::PresentEvent &e : p_frame.events) {
		if (cam_event(e, p_frame)) {
			continue;
		}
		Pending p{ e.kind, e.timeline, e.slot, e.time, {} };
		if (e.kind == host::PresentKind::Death) {
			p.slot = e.aux;
			p.record = present::death_record_of(e);
		} else if (e.kind == host::PresentKind::RemoteShot) {
			remote_shot_seen = true;
			if ((e.flags & (host::present_flags::kNoRound | host::present_flags::kVehicle)) != 0) {
				continue;
			}
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
		const bool same_life = s.known && s.generation == o.generation;
		const float last_y = s.input.position.y;
		if (!s.known || s.generation != o.generation) {
			cam_new_life(o.slot, s);
			s.dead = false;
			s.reload_known = false;
			s.fire_seq = 0;
		}
		s.known = true;
		s.team = team;
		s.generation = o.generation;
		s.present = true;
		anim::CrowdSoldierInput in;
		in.position = o.position;
		in.velocity = o.velocity;
		in.yaw = o.yaw;
		in.pitch = o.pitch;
		in.camera_distance = std::sqrt((in.position.x - cam.x) * (in.position.x - cam.x) + (in.position.y - cam.y) * (in.position.y - cam.y) +
				(in.position.z - cam.z) * (in.position.z - cam.z));
		in.team = team;
		in.pose = uint8_t(o.pose);
		in.target_pose = uint8_t(o.target_pose);
		in.transition = o.transition;
		in.sprinting = (o.flags & sim::soldier_flags::kSprinting) != 0;
		if (w != nullptr) {
			if (w->def != sim::weapons::kNoWeapon) {
				const net::SoldierGear *gear = p_frame.frame != nullptr && o.slot < p_frame.frame->gear.size() &&
								p_frame.frame->gear[o.slot].def == w->def
						? &p_frame.frame->gear[o.slot]
						: nullptr;
				const int32_t idx = gear != nullptr ? gear_index_of(team, *gear) : index_of(team, w->def);
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
			in.holding = w->phase == sim::weapons::WeaponPhase::Holding;
		}
		in.weapon = s.weapon;
		in.fire_seq = s.fire_seq;
		in.vault = o.vault == present::kNoPresentedVault ? anim::kNoVaultAnim : o.vault;
		in.vault_seconds = o.vault_seconds;
		if (in.vault != anim::kNoVaultAnim) {
			if (!s.vault_held) {
				s.vault_held = true;
				s.vault_y = same_life && !s.dead ? last_y : o.position.y;
			}
			in.position.y = s.vault_y;
		} else {
			s.vault_held = false;
		}
		s.input = in;
		if ((o.flags & sim::soldier_flags::kAlive) != 0 && !s.dead) {
			s.kit = row_kit(p_frame.frame, o.slot, s.kit);
		}
	}

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
		if (corpses_kept && slot >= kCamLaneOffset) {
			++cam_counts[2];
			continue;
		}
		if (s.dead) {
			s.dead_seconds += dt;
		}
		const bool corpse = s.dead && s.dead_seconds < double(corpse_seconds + kCorpseExtraSeconds);
		if (!s.present && !corpse) {
			continue;
		}
		const uint32_t index = uint32_t(s.team) * kSlotsPerTeam + slot + uint32_t(s.lane) * kCamLaneOffset;
		anim::CrowdSoldierInput in = s.input;
		in.dead = s.dead;
		in.revived = s.revived;
		in.death_clip = s.dead ? s.death_clip : anim::kNoCrowdDeathClip;
		if (!s.present) {
			in.velocity = anim::Vec3{};
			in.flags = 0;
		}
		inputs[index] = in;
		if (!s.dead) {
			kit_of[index] = s.kit;
		}
		const anim::Xform root{ yaw_quat(in.yaw), in.position };
		root_x[index] = root;
		roots[index] = Transform3D(Basis(Vector3(0, 1, 0), in.yaw), Vector3(in.position.x, in.position.y, in.position.z));
	}
	revive_roots();
	cam_draw_corpses(dt, cam);
}

void Bf3GameCrowd::Impl::draw_weapons(const GameFrame &p_frame) {
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
		if (draw && drawn) {
			uploader->set_soldier_kit(i, kit_of[i]);
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
			m.velocity = inputs[i].velocity;
			muzzles.push_back(m);
		}
	}
	stats.muzzles = uint32_t(muzzles.size());
	stats.throwing = eval->combat_stats().throwing;
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
	textures.clear();
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

core::Status Bf3GameCrowd::load_body(const core::InstallReader &p_reader, core::Cache *p_cache, const std::atomic<bool> *p_stop) {
	Impl &m = *impl_;
	if (m.loaded.load(std::memory_order_acquire) || m.body != nullptr) {
		return core::make_error("crowd: already loaded");
	}
	auto stopped = [&] { return p_stop != nullptr && p_stop->load(std::memory_order_relaxed); };
	const auto clock_s = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
	const double t_start = clock_s();
	render::SoldierSpec spec;
	spec.teams[0].team_data = "gameplay/teams/us";
	spec.teams[1].team_data = "gameplay/teams/ru";

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
		core::Result<anim::CrowdDeathClips> deaths =
				anim::load_crowd_death_clips((*assets)->clips, *(*assets)->library, anim::ChooserIndices{});
		if (deaths) {
			m.death_clips = std::move(*deaths);
			m.have_death_clips = true;
		} else {
			m.body_notes.push_back("death clips: " + deaths.error().message + " (deaths play no clip)");
		}
		core::Result<anim::CrowdMoveGroupSet> groups = anim::load_crowd_move_groups((*assets)->clips, *(*assets)->library);
		if (groups) {
			m.move_groups = std::move(*groups);
			m.have_move_groups = true;
			for (const std::string &n : m.move_groups.notes) {
				m.body_notes.push_back("move groups: " + n);
			}
		} else {
			m.body_notes.push_back("move groups: " + groups.error().message + " (S3's and tp's cycles)");
		}
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

core::Status Bf3GameCrowd::load(const GameLoadInputs &p_in) {
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
	core::Result<std::unique_ptr<render::CrowdWeaponSet>> weapons = render::load_crowd_weapons(reader, spec, *m.body,
			{ std::span<const render::CrowdWeaponSpec>(lists.teams[0]),
					std::span<const render::CrowdWeaponSpec>(lists.teams[1]) },
			p_in.cache, false);
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
		for (size_t i = 0; i < m.views[t].size(); ++i) {
			m.weapon_index[t].emplace(m.weapons->teams[t][i].spec.def, uint8_t(i));
		}
	}
	if (gunplay != nullptr && !gunplay->soldier.ragdoll_asset.empty()) {
		const std::string &name = gunplay->soldier.ragdoll_asset;
		std::optional<core::LevelMount> own;
		const core::LevelMount *mount = p_in.db->level ? &p_in.db->level->mount() : nullptr;
		if (mount == nullptr && p_in.shared != nullptr) {
			core::Result<core::LevelMount> described =
					core::level::mount_level_description(*p_in.db->install, p_in.shared->level);
			if (described) {
				own.emplace(std::move(*described));
				mount = &*own;
			}
		}
		auto res = mount != nullptr ? mount->read_res(name)
									: decltype(mount->read_res(name))(core::make_error("no level mount"));
		auto def = std::make_shared<core::res::RagdollDef>();
		const core::res::ResError parsed =
				res ? core::res::RagdollDef::parse(res->second, res->first.res_meta, *def) : core::res::ResError();
		if (!res) {
			m.weapons->notes.push_back("ragdoll " + name + ": " + res.error().message + " (no ragdolls)");
		} else if (!parsed.ok()) {
			m.weapons->notes.push_back("ragdoll " + name + ": " + parsed.message() + " (no ragdolls)");
		} else {
			m.ragdoll_def = std::move(def);
		}
	}
	m.catalog_install = p_in.db->install;
	m.catalog_level = p_in.shared;
	m.catalog_spec = spec;
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
	stance.move_groups = m.have_move_groups ? &m.move_groups : nullptr;
	if (has_user_arg("no-locomotion")) {
		stance.move_groups = nullptr;
	}
	stance.corpse_seconds = m.corpse_seconds;
	if (m.have_death_clips) {
		stance.death_clips = m.death_clips.clips;
	}
	core::Result<std::unique_ptr<anim::CrowdEvaluator>> e = anim::CrowdEvaluator::make(m.assets->clips, stance, m.teams, kCrowdSoldiers, m.config);
	if (!e) {
		return fail(e.error().message);
	}
	m.eval = std::move(*e);
	if (m.ragdoll_def != nullptr && m.catalog_level != nullptr && m.catalog_level->static_world != nullptr) {
		std::vector<std::string> bones;
		std::vector<anim::Vec3> pivots(m.ragdoll_def->bodies.size(), anim::Vec3{});
		for (const core::res::RagdollBodyDef &b : m.ragdoll_def->bodies) {
			bones.push_back(b.bone);
		}
		for (const core::res::RagdollJointDef &j : m.ragdoll_def->joints) {
			pivots[j.child] = j.child_frame.trans;
		}
		std::string why;
		core::Result<anim::RagdollRig> rig = anim::RagdollRig::make(m.assets->clips.rig, bones, pivots);
		if (!rig) {
			why = rig.error().message;
		} else if (core::Status joints = m.eval->set_ragdoll_joints(rig->joints); !joints.ok()) {
			why = joints.error().message;
		} else if (core::Result<std::unique_ptr<sim::RagdollWorld>> world = sim::RagdollWorld::create(
						   m.catalog_level->static_world, m.ragdoll_def, sim::RagdollWorldOptions{ kCrowdSoldiers });
				!world) {
			why = world.error().message;
		} else {
			m.ragdoll_rig = std::move(*rig);
			m.ragdoll_world = std::move(*world);
			m.have_ragdoll = true;
			m.ragdoll_hips = m.ragdoll_def->find_body("Hips");
			m.ragdoll_head = m.ragdoll_def->find_body("Head");
			m.corpses.assign(kCrowdSoldiers, Impl::Corpse{});
			log("ragdoll: " + std::to_string(m.ragdoll_def->bodies.size()) + " bodies, " +
					std::to_string(m.ragdoll_def->joints.size()) + " joints over the static world");
		}
		if (!why.empty()) {
			log("ragdoll: " + why + " (no ragdolls)");
		}
	}
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
	render::release_soldier_textures(*m.assets, *m.weapons);
	m.soldier_visible.assign(kCrowdSoldiers, false);
	for (uint32_t i = 0; m.draw && i < kCrowdSoldiers; ++i) {
		m.uploader->set_soldier_visible(i, false);
	}

	for (uint32_t t = 0; t < 2; ++t) {
		for (const render::CrowdWeaponPart &part : m.weapons->teams[t]) {
			m.weapon_meshes[t].push_back(m.make_weapon_mesh(part));
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
	if (m.uploader) {
		log("iff kits: " + std::to_string(m.uploader->kit_meshes()) + " distinct (team, kit) meshes");
	}
	m.built = true;
	return true;
}

bool Bf3GameCrowd::built() const {
	return impl_->built;
}

void Bf3GameCrowd::step(const GameFrame &p_frame) {
	Impl &m = *impl_;
	if (!m.built || m.shut_down) {
		return;
	}
	m.build_inputs(p_frame);
	m.eval->wait_and_publish(*m.lanes);
	m.step_ragdolls(std::clamp(p_frame.dt, 0.0, 0.25));
	m.collect_plants();
	m.adopt_catalog();
	m.adopt_vaults();
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

void Bf3GameCrowd::Impl::collect_plants() {
	for (uint32_t slot = 0; slot < kSlotsPerTeam; ++slot) {
		const Slot &s = slots[slot];
		const uint32_t index = uint32_t(s.team) * kSlotsPerTeam + slot + uint32_t(s.lane) * kCamLaneOffset;
		const uint8_t p = s.known && s.present && !s.dead && index < kCrowdSoldiers ? eval->foot_plants(index) : 0u;
		plants[slot] = uint8_t(((p & anim::kCrowdPlantLeft) != 0 ? audio::kFootPlantLeftBit : 0u) |
				((p & anim::kCrowdPlantRight) != 0 ? audio::kFootPlantRightBit : 0u) |
				((p & anim::kCrowdPlantsMeasured) != 0 ? audio::kFootPlantsAnimatedBit : 0u));
	}
}

std::span<const uint8_t> Bf3GameCrowd::foot_plants() const {
	return impl_->built ? std::span<const uint8_t>(impl_->plants) : std::span<const uint8_t>();
}

bool Bf3GameCrowd::vault_of(uint8_t p_slot, uint8_t &r_kind, float &r_seconds) const {
	const Impl &m = *impl_;
	r_kind = anim::kNoVaultAnim;
	r_seconds = 0.0f;
	if (!m.built || m.eval == nullptr || p_slot >= kSlotsPerTeam || !m.slots[p_slot].known) {
		return false;
	}
	return m.eval->vault_of(uint32_t(m.slots[p_slot].team) * kSlotsPerTeam + p_slot, r_kind, r_seconds);
}

GameCrowdStats Bf3GameCrowd::stats() const {
	GameCrowdStats s = impl_->stats;
	s.catalog_added = impl_->catalog_added;
	s.catalog_failed = impl_->catalog_failed;
	s.catalog_pending = impl_->catalog_jobs != nullptr ? impl_->catalog_jobs->stats().pending : 0;
	s.ragdolls = impl_->ragdoll_world != nullptr ? uint32_t(impl_->ragdoll_world->ragdoll_count()) : 0u;
	s.ragdolls_started = impl_->ragdolls_started;
	s.ragdolls_frozen = impl_->ragdolls_frozen;
	s.ragdoll_failures = impl_->ragdoll_failures;
	s.ragdoll_hips_height = impl_->ragdoll_hips_height;
	s.ragdoll_weight = impl_->ragdoll_weight;
	s.ragdoll_revives = impl_->ragdoll_revives;
	return s;
}

GameCrowdCorpse Bf3GameCrowd::corpse_of(uint8_t p_slot) const {
	const Impl &m = *impl_;
	if (p_slot >= kSlotsPerTeam || !m.slots[p_slot].known || m.ragdoll_world == nullptr) {
		return GameCrowdCorpse{};
	}
	const Impl::Slot &s = m.slots[p_slot];
	const uint32_t index = uint32_t(s.team) * kSlotsPerTeam + p_slot + uint32_t(s.lane) * kCamLaneOffset;
	if (index >= m.corpses.size()) {
		return GameCrowdCorpse{};
	}
	const Impl::Corpse &c = m.corpses[index];
	GameCrowdCorpse r = c.seen;
	r.active = c.active;
	r.in_world = c.in_world;
	r.frozen = c.in_world && m.ragdoll_world->frozen(index);
	r.clip = c.clip;
	r.age = c.active ? float(c.age) : -1.0f;
	r.weight = c.blend.weight();
	r.reviving = c.active && c.blend.blending_out();
	r.root_fraction = c.blend.root_fraction();
	if (r.reviving && index < m.root_x.size() && index < m.inputs.size()) {
		const anim::Vec3 d = anim::vec_sub(m.root_x[index].t, m.inputs[index].position);
		r.root_offset = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	}
	return r;
}

void Bf3GameCrowd::enable_catalog(std::function<std::shared_ptr<core::Cache>()> p_cache) {
	Impl &m = *impl_;
	if (m.catalog_jobs != nullptr || !loaded() || m.shut_down) {
		return;
	}
	m.catalog_cache = std::move(p_cache);
	m.catalog_jobs = std::make_unique<core::JobQueue>("crowd catalog");
	m.catalog_jobs->post([&m] { m.load_vaults(); });
}

void Bf3GameCrowd::want_weapon(uint8_t p_wire_team, uint16_t p_def) {
	impl_->want(anim::crowd_team_of_wire(p_wire_team), p_def);
}

void Bf3GameCrowd::want_gear(uint8_t p_wire_team, const net::SoldierGear &p_gear) {
	impl_->want_gear(anim::crowd_team_of_wire(p_wire_team), p_gear);
}

size_t Bf3GameCrowd::gear_accessory_meshes(uint8_t p_wire_team, const net::SoldierGear &p_gear) const {
	const Impl &m = *impl_;
	const uint8_t team = anim::crowd_team_of_wire(p_wire_team);
	const auto it = m.gear_index[team].find({ p_gear.def, p_gear.accessories });
	if (it == m.gear_index[team].end() || it->second >= m.weapon_meshes[team].size()) {
		return 0;
	}
	const WeaponMesh &wm = m.weapon_meshes[team][it->second];
	return wm.binds > 0 && (!m.draw || wm.mesh.is_valid()) ? wm.accessory_meshes : 0;
}

size_t Bf3GameCrowd::slot_accessory_meshes(uint8_t p_slot) const {
	const Impl &m = *impl_;
	if (p_slot >= kSlotsPerTeam || !m.slots[p_slot].known) {
		return 0;
	}
	const Impl::Slot &s = m.slots[p_slot];
	const size_t index = size_t(s.team) * kSlotsPerTeam + p_slot + size_t(s.lane) * kCamLaneOffset;
	if (index >= m.weapon_draws.size() || !m.weapon_draws[index].visible || s.team >= m.weapon_meshes.size() ||
			m.weapon_draws[index].item >= m.weapon_meshes[s.team].size()) {
		return 0;
	}
	return m.weapon_meshes[s.team][m.weapon_draws[index].item].accessory_meshes;
}
bool Bf3GameCrowd::has_weapon_mesh(uint8_t p_wire_team, uint16_t p_def) const {
	const Impl &m = *impl_;
	const uint8_t team = anim::crowd_team_of_wire(p_wire_team);
	const auto it = m.weapon_index[team].find(p_def);
	if (it == m.weapon_index[team].end() || it->second >= m.weapon_meshes[team].size()) {
		return false;
	}
	const WeaponMesh &wm = m.weapon_meshes[team][it->second];
	return wm.binds > 0 && (!m.draw || wm.mesh.is_valid());
}

void Bf3GameCrowd::shutdown() {
	Impl &m = *impl_;
	if (m.shut_down) {
		return;
	}
	m.shut_down = true;
	if (m.catalog_jobs != nullptr) {
		m.catalog_jobs->stop();
	}
	m.cam_log();
	if (m.eval && m.lanes) {
		m.eval->wait_and_publish(*m.lanes);
	}
	m.clear_draw();
	m.eval.reset();
	m.lanes.reset();
	m.muzzles.clear();
	m.built = false;
}

}

namespace bf3::godot_side {

namespace {

// STAND-IN: a Revive event matches a new life seen within 2 s. Settle: a note on when a revived ghost's new life arrives.
constexpr double kCamReviveWaitSeconds = 2.0;

}

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
	m.corpses_kept = true;
}

uint8_t Bf3GameCrowd::Impl::death_clip_of(const core::gameplay::DeathRecord &p_record, uint8_t p_pose) const {
	if (!have_death_clips || !p_record.valid) {
		return anim::kNoCrowdDeathClip;
	}
	anim::CrowdDeathChoice choice;
	choice.explosion = p_record.explosion;
	choice.crouch = p_pose == uint8_t(sim::Pose::Crouch);
	choice.prone = p_pose == uint8_t(sim::Pose::Prone);
	choice.random_value = p_record.random_value();
	return death_clips.choose(choice);
}

void Bf3GameCrowd::Impl::step_ragdolls(double p_dt) {
	if (!have_ragdoll || eval == nullptr) {
		return;
	}
	const float dt = float(p_dt);
	const size_t n = ragdoll_world->body_count();
	for (uint32_t i = 0; i < kCrowdSoldiers; ++i) {
		Corpse &c = corpses[i];
		const anim::CrowdSoldierInput &in = inputs[i];
		const bool shown = (in.flags & anim::crowd_flags::kHidden) == 0;
		const bool dead = in.dead && shown;
		if (!dead) {
			if (c.active && shown && in.revived && c.in_world && !c.done && !c.blend.blending_out()) {
				begin_revive(c, in);
			}
			if (c.active && shown && c.blend.blending_out()) {
				// INFERRED: the graph's RagdollBlend is 0 after a revive (no get-up clip), so the weight drops at once.
				// Settle: a note on the get-up graph's RagdollBlend after a revive.
				c.blend.update(dt, 0.0f);
				if (!c.blend.blend_out_done()) {
					continue;
				}
			}
			if (c.active) {
				ragdoll_world->remove(i);
				c = Corpse{};
				eval->set_ragdoll(i, {}, 0.0f);
			}
			continue;
		}
		if (c.active && c.blend.blending_out()) {
			ragdoll_world->remove(i);
			c = Corpse{};
			eval->set_ragdoll(i, {}, 0.0f);
		}
		if (!c.active) {
			c = Corpse{};
			c.active = true;
			c.clip = in.death_clip;
			c.blend.activate();
			c.bodies.resize(n);
			c.previous.resize(n);
			c.joints.resize(n);
			c.poses.resize(n);
		}
		c.age += p_dt;
		if (c.done || c.age >= double(corpse_seconds)) {
			if (c.in_world) {
				ragdoll_world->remove(i);
				c.in_world = false;
			}
			c.done = true;
			continue;
		}
		// SOURCE: notes R5-DT1 D and E (the weight follows the graph's RagdollBlend; 1 at once for a body not animating).
		// INFERRED: RagdollBlend read from the clip's tag, and a death with no clip as the notes' not-animating branch.
		// Settle: a note on the graph's RagdollBlend output and on deaths without a death clip.
		const float target =
				c.clip < death_clips.clips.size() ? death_clips.clips[c.clip].ragdoll_target(float(c.age)) : 1.0f;
		// SOURCE: notes R5-DT2 B1 (while w < 1 the bodies follow the animated pose and take its velocities).
		if (c.blend.copies_pose()) {
			const std::span<const anim::Xform> model = eval->model(i);
			if (!model.empty()) {
				c.have_previous = c.copied;
				std::swap(c.previous, c.bodies);
				ragdoll_rig.bodies(model, root_x[i], c.bodies);
				c.copied = true;
				c.copy_dt = dt;
				c.blend.pose_copied();
			}
		}
		c.blend.update(dt, target);
		if (target > 0.0f && c.seen.rise_age < 0.0f) {
			c.seen.rise_age = float(c.age);
		}
		if (c.blend.weight() > 0.0f && !c.in_world && c.copied) {
			std::vector<sim::Vec3> linear(n, sim::Vec3{}), angular(n, sim::Vec3{});
			for (size_t b = 0; b < n; ++b) {
				const anim::Xform &x = c.bodies[b];
				c.poses[b].position = x.t;
				c.poses[b].rotation = { x.q.x, x.q.y, x.q.z, x.q.w };
				// STAND-IN: the animated joints' velocities as the last two pose copies' difference over the frame time.
				// Settle: a note on how the engine takes the animated joint velocities.
				if (c.have_previous && c.copy_dt > 0.0f) {
					linear[b] = anim::vec_scale(anim::vec_sub(x.t, c.previous[b].t), 1.0f / c.copy_dt);
					anim::Quat d = anim::quat_mul(x.q, anim::quat_conj(c.previous[b].q));
					if (d.w < 0.0f) {
						d = anim::Quat{ -d.x, -d.y, -d.z, -d.w };
					}
					const float s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
					if (s > 1.0e-6f) {
						const float angle = 2.0f * std::atan2(s, d.w);
						angular[b] = anim::vec_scale(anim::Vec3{ d.x, d.y, d.z }, angle / (s * c.copy_dt));
					}
				}
			}
			if (ragdoll_world->add(i, c.poses, linear, angular).ok()) {
				c.in_world = true;
				++ragdolls_started;
				c.seen.start_age = float(c.age);
				c.seen.start_weight = c.blend.weight();
				c.seen.start_speed = 0.0f;
				for (const sim::Vec3 &v : linear) {
					c.seen.start_speed = std::max(c.seen.start_speed, std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
				}
			} else {
				++ragdoll_failures;
			}
		}
	}
	if (!ragdoll_world->step(dt).ok()) {
		++ragdoll_failures;
	}
	ragdoll_hips_height = -1.0f;
	ragdoll_weight = 0.0f;
	for (uint32_t i = 0; i < kCrowdSoldiers; ++i) {
		Corpse &c = corpses[i];
		if (!c.active) {
			continue;
		}
		if (!c.in_world) {
			eval->set_ragdoll(i, {}, 0.0f);
			continue;
		}
		if (!ragdoll_world->frozen(i) &&
				c.settle.update(dt, ragdoll_world->max_linear_speed(i), ragdoll_world->max_angular_speed(i))) {
			ragdoll_world->freeze(i);
			++ragdolls_frozen;
			c.seen.frozen_age = float(c.age);
		}
		if (!ragdoll_world->pose(i, c.poses)) {
			continue;
		}
		for (size_t b = 0; b < c.poses.size(); ++b) {
			const sim::RagdollBodyPose &p = c.poses[b];
			c.bodies[b] =
					anim::Xform{ anim::Quat{ p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3] }, p.position };
		}
		if (ragdoll_hips >= 0 && size_t(ragdoll_hips) < c.poses.size()) {
			c.seen.hips_height = c.poses[size_t(ragdoll_hips)].position.y - root_x[i].t.y;
			ragdoll_hips_height = std::max(ragdoll_hips_height, c.seen.hips_height);
		}
		ragdoll_rig.joints_model(c.bodies, root_x[i], c.joints);
		eval->set_ragdoll(i, c.joints, c.blend.weight());
		ragdoll_weight = std::max(ragdoll_weight, c.blend.weight());
	}
}

void Bf3GameCrowd::Impl::begin_revive(Corpse &r_c, const anim::CrowdSoldierInput &p_in) {
	r_c.blend.blend_out();
	++ragdoll_revives;
	r_c.facing_position = p_in.position;
	r_c.facing_yaw = p_in.yaw;
	if (ragdoll_hips < 0 || ragdoll_head < 0 || size_t(std::max(ragdoll_hips, ragdoll_head)) >= r_c.poses.size()) {
		return;
	}
	const sim::RagdollBodyPose &hips = r_c.poses[size_t(ragdoll_hips)];
	const sim::RagdollBodyPose &head = r_c.poses[size_t(ragdoll_head)];
	// SOURCE: notes R5-DT2 F3 (the facing transform from the hip and head bones, horizontal hip to head).
	// INFERRED: its position is the hip body's. Settle: a note on the facing transform's origin.
	r_c.facing_position = anim::Vec3{ hips.position.x, hips.position.y, hips.position.z };
	const float dx = head.position.x - hips.position.x;
	const float dz = head.position.z - hips.position.z;
	if (dx * dx + dz * dz > core::gameplay::kDeathDirectionMinLengthSq) {
		r_c.facing_yaw = std::atan2(dx, dz);
	}
}

void Bf3GameCrowd::Impl::revive_roots() {
	for (uint32_t i = 0; i < corpses.size() && i < inputs.size(); ++i) {
		const Corpse &c = corpses[i];
		const anim::CrowdSoldierInput &in = inputs[i];
		if (!c.active || !c.blend.blending_out() || in.dead || (in.flags & anim::crowd_flags::kHidden) != 0) {
			continue;
		}
		// SOURCE: notes R5-DT2 F3 (the root moves from the stored facing transform to the soldier's by f).
		// INFERRED: f = 1 is the stored ragdoll transform, as the note infers. Settle: a note on the blend-out's ends.
		const float f = c.blend.root_fraction();
		const float yaw = in.yaw + std::remainder(c.facing_yaw - in.yaw, 2.0f * float(std::numbers::pi)) * f;
		const anim::Vec3 p =
				anim::vec_add(in.position, anim::vec_scale(anim::vec_sub(c.facing_position, in.position), f));
		root_x[i] = anim::Xform{ yaw_quat(yaw), p };
		roots[i] = Transform3D(Basis(Vector3(0, 1, 0), yaw), Vector3(p.x, p.y, p.z));
	}
}

bool Bf3GameCrowd::Impl::cam_event(const host::PresentEvent &p_e, const GameFrame &p_frame) {
	if (p_e.kind == host::PresentKind::Revive) {
		if (p_e.aux < kSlotsPerTeam && p_e.aux != p_frame.own_slot) {
			slots[p_e.aux].revive_wait = 0.0;
		}
		return true;
	}
	if (!corpses_kept || p_frame.own_slot >= kCamLaneOffset) {
		return false;
	}
	const uint8_t own = p_frame.own_slot;
	if (p_e.kind == host::PresentKind::Spawn && p_e.slot == own) {
		for (CamCorpse &c : cam_corpses[own]) {
			c.active = false;
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
	c.input.death_clip = death_clip_of(present::death_record_of(p_e), c.input.pose);
	c.input.weapon = 0;
	if (f->own_weapons_valid && f->own_weapons_curr.active < f->own_weapons_curr.slots.size()) {
		const uint16_t def = f->own_weapons_curr.slots[f->own_weapons_curr.active].def;
		const int32_t idx = def != sim::weapons::kNoWeapon ? index_of(team, def) : -1;
		c.input.weapon = idx >= 0 ? uint8_t(idx) : uint8_t(0);
	}
	c.index = uint32_t(team) * kSlotsPerTeam + own;
	if (c.index < kit_of.size()) {
		kit_of[c.index] = f->own_kit < 4 ? f->own_kit : uint8_t(0);
	}
	c.seconds = 0.0;
	c.active = true;
	cam_corpses[own][1].active = false;
	++cam_counts[4];
	return true;
}

void Bf3GameCrowd::Impl::cam_new_life(uint32_t p_slot, Slot &r_s) {
	if (!r_s.known) {
		return;
	}
	if (r_s.revive_wait >= 0.0) {

		r_s.revive_wait = -1.0;
		r_s.revived = true;
		return;
	}
	r_s.revived = false;
	if (!corpses_kept || p_slot >= kCamLaneOffset) {
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
	c.input.death_clip = r_s.death_clip;
	c.input.revived = false;
	c.input.velocity = anim::Vec3{};
	c.input.flags = 0;
	r_s.lane = uint8_t(r_s.lane ^ 1u);
	++cam_counts[0];

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
	if (!corpses_kept) {
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
	if (!corpses_kept) {
		return;
	}
	char line[320];
	std::snprintf(line, sizeof(line),
			"corpses (%.1f s): %llu kept on the second lane, %llu cut short, %llu own corpses, %llu slot-frames >= 64 skipped, %llu revives without a new life; "
			"evaluator revives %llu",
			double(corpse_seconds), (unsigned long long)cam_counts[0], (unsigned long long)cam_counts[1], (unsigned long long)cam_counts[4],
			(unsigned long long)cam_counts[2], (unsigned long long)cam_counts[3], (unsigned long long)(eval ? eval->combat_stats().revives : 0));
	log(line);
}

}

namespace bf3::godot_side {

uint8_t Bf3GameCrowd::Impl::row_kit(const host::PresentationFrame *p_frame, uint8_t p_slot, uint8_t p_fallback) {
	if (p_frame == nullptr) {
		return p_fallback;
	}
	const size_t rows = std::min<size_t>(p_frame->player_count, p_frame->players.size());
	const auto end = p_frame->players.begin() + std::ptrdiff_t(rows);
	const auto it = std::lower_bound(p_frame->players.begin(), end, p_slot, [](const net::PlayerRow &p_row, uint8_t p_s) { return p_row.slot < p_s; });
	if (it == end || it->slot != p_slot || it->kit >= 4) {
		return p_fallback;
	}
	return it->kit;
}

}
