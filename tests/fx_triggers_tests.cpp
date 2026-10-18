// M2a fx3 (docs/design/m2a-fx3.md): VFX3's triggers (bf3/fx/triggers.h).
//   fx.triggers.unit  (suite fx-triggers-unit): install-free, synthetic EBX effects and material grid through the real
//                     EffectLibrary / ImpactTable / FxWorld; shots, shells, tracers, impacts, soldier cells, detonations,
//                     the two timelines, duplicates, hostile events, effect sounds and case "late join" (E15's variant
//                     row: two clients' event streams from cl's ClientProjectiles through two trigger instances).
//   fx.install.cells  (suite fx-install-cells): the retail install, read only; load_fire_effects over Metro's
//                     GunplayLevel defs, the kit and impact blueprints preloaded (E8 (1)), the 1p flash fraction over
//                     1,000 OwnShots (E8 (3)), in-game cell selection from Metro collision triangles (E8 (5), fx G5
//                     named cells) with the Caspian water fallback, the cues (E8 (7)); install unchanged last.
#include <doctest/doctest.h>

#include "fx_test_support.h"
#include "test_support.h"
#include "weapons_support.h"

#include "bf3/core/ebx_db.h"
#include "bf3/core/gameplay/material_grid.h"
#include "bf3/core/gameplay/weapon_catalog.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/collision_soup.h"
#include "bf3/core/res/res_loader.h"
#include "bf3/core/gameplay/weapon_blob.h"
#include "bf3/fx/effect_compiler.h"
#include "bf3/fx/fx_harness.h"
#include "bf3/fx/effect_library.h"
#include "bf3/fx/impact_table.h"
#include "bf3/fx/simulator.h"
#include "bf3/fx/triggers.h"
#include "bf3/host/client_projectiles.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/presentation_events.h"
#include "bf3/render/texture_prep.h"
#include "bf3/sim/weapons/sim_time.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace bf3;
using namespace bf3::fx;
using fxt::DocBuilder;
using fxt::EffectBuilder;
using fxt::f32;
using fxt::flag;
using fxt::i32;

namespace tw = bf3::test::weapons;
namespace sw = bf3::sim::weapons;
namespace hk = bf3::host;

// Until tp's crowd lands (m2a.md §15 fx3 row): the 3p muzzle of a presented soldier, at eye height plus the weapon
// offset along its view (fx::fallback_muzzle is the same rule; the fake stands for Bf3GameCrowd::muzzles()).
namespace m2a_fake {
MuzzleSample crowd_muzzle(const hk::PresentedSoldier &p_soldier) {
	return fallback_muzzle(p_soldier);
}
} // namespace m2a_fake

namespace {

constexpr uint32_t kNormTime = 1;
constexpr uint32_t kOne = 3;

// ---- the synthetic world --------------------------------------------------------------------------------------

constexpr const char *kMuzz1p = "fx/syn/fx_syn_muzz_1p";
constexpr const char *kMuzz3p = "fx/syn/fx_syn_muzz_3p";
constexpr const char *kShell = "fx/weapons/shells/fx_syn_shell";
constexpr const char *kTracer = "fx/syn/fx_syn_tracer";
constexpr const char *kImpactA = "fx/syn/fx_syn_impact_a";
constexpr const char *kImpactB = "fx/syn/fx_syn_impact_b";
constexpr const char *kImpactMetal = "fx/syn/fx_syn_impact_metal"; // CullDistance 50
constexpr const char *kSoldier = "fx/syn/fx_syn_soldier";
constexpr const char *kWater = "fx/syn/fx_syn_water";
constexpr const char *kFrag = "fx/weapons/grenades/frag/fx_grenade_frag_01";
constexpr const char *kSounding = "fx/syn/fx_syn_sounding";
constexpr const char *kDecalConcrete = "decals/syn/decal_concrete";
constexpr const char *kDecalMetal = "decals/syn/decal_metal";
constexpr const char *kDecalScuff = "decals/syn/decal_scuff";
constexpr const char *kSoundConcrete = "sound/syn/impact_concrete";
constexpr const char *kSoundMetal = "sound/syn/impact_metal";
constexpr const char *kSoundWater = "sound/syn/impact_water";
constexpr const char *kSoundPatch = "sound/syn/effect_patch";

// Surface PhysicsMaterialIndex values (the retail ones where they exist).
constexpr uint8_t kConcrete = 8, kMetal = 3, kWaterMat = 18, kScuff = 20, kEmptyMat = 200;

constexpr uint64_t kMatch = 0x5eed5eedull;

DocBuilder simple_doc(uint32_t p_seed, uint32_t p_max_count = 8, float p_rate = 10, float p_life = 1, float p_window = 0) {
	DocBuilder d(p_seed, p_max_count, p_window);
	d.proc("SpawnRateData", { { "SpawnRate", f32(p_rate) }, { "DistributeOverTime", flag(true) } }, kOne);
	d.proc("UpdateAgeData", { { "Lifetime", f32(p_life) } }, kNormTime);
	d.proc("UpdateLinearVelocityData");
	return d;
}

struct Syn {
	fxt::MemResolver db;
	std::optional<ImpactTable> impacts;
	std::unique_ptr<EffectLibrary> library;
	std::shared_ptr<FireEffectTable> fire;
	std::map<std::string, uint32_t> seeds; // partition name -> seed
};

// One effect partition with p_emitters emitter components (probabilities) on document seed 1.
void add_effect(Syn &r_s, const std::string &p_name, uint32_t p_seed, std::vector<float> p_probabilities, float p_cull = 0, bool p_sound = false,
		uint32_t p_instances = 64) {
	EffectBuilder e(p_seed, p_instances);
	fxt::set(e.entity(), "CullDistance", f32(p_cull));
	for (const float p : p_probabilities) {
		e.emitter(1, p);
	}
	if (p_sound) {
		e.sound(900);
	}
	REQUIRE(r_s.db.set(p_name, e.done()));
	r_s.seeds[p_name] = p_seed;
}

// The relation instances of one cell: effects (external EffectBlueprint refs), decal, sound; the reference words.
std::vector<uint32_t> cell_relations(fxt::Partition &r_p, Syn &r_s, std::vector<std::string> p_effects, float p_spread, bool p_mirror, const char *p_decal,
		const char *p_sound) {
	std::vector<uint32_t> out;
	fxt::Vals v{ { "ImpactEffectMaxSpreadAngle", f32(p_spread) }, { "MirrorImpactDirection", flag(p_mirror) } };
	fxt::Array a;
	for (const std::string &e : p_effects) {
		a.words.push_back(r_p.ext(r_s.seeds.at(e), 0).word);
	}
	fxt::set(v, "ImpactEffects", r_p.array(std::move(a)));
	out.push_back(r_p.ref(r_p.add({ "MaterialRelationEffectData", std::move(v) })).word);
	if (p_decal != nullptr) {
		out.push_back(r_p.ref(r_p.add({ "MaterialRelationDecalData", { { "Decal", r_p.ext(r_s.seeds.at(p_decal), 0) } } })).word);
	}
	if (p_sound != nullptr) {
		out.push_back(r_p.ref(r_p.add({ "MaterialRelationSoundData", { { "ImpactSound", r_p.ext(r_s.seeds.at(p_sound), 0) } } })).word);
	}
	return out;
}

WeaponFireEffects rifle_effects() {
	WeaponFireEffects w;
	FireEffect m1;
	m1.blueprint = kMuzz1p;
	m1.offset = fire_effect_offset({ 0, 0.028f, 0.99f }, {});
	m1.update_transform = true;
	FireEffect s1;
	s1.blueprint = kShell;
	s1.offset = fire_effect_offset({ -0.013f, 0.026f, 0.44f }, {});
	s1.shell = true;
	FireEffect m3;
	m3.blueprint = kMuzz3p;
	m3.offset = fire_effect_offset({ 0, 0.028f, 0.99f }, {});
	FireEffect s3 = s1;
	s3.offset = fire_effect_offset({ 0, 0.03f, 0.43f }, {});
	w.first_person = { m1, s1 };
	w.third_person = { m3, s3 };
	w.tracer = kTracer;
	w.trace_frequency = 1;
	w.bullet_material = 1;
	w.firearm = true;
	w.tracer_speed = 900;
	w.tracer_ttl = 1.5f;
	w.converge = 30;
	return w;
}

// Defs: 0 rifle, 1 grenade (detonation frag), 2 knife (nothing).
std::shared_ptr<FireEffectTable> small_fire_table() {
	auto t = std::make_shared<FireEffectTable>();
	t->defs.push_back(rifle_effects());
	WeaponFireEffects g;
	g.firearm = false;
	g.bullet_material = 1;
	g.detonation = kFrag;
	t->defs.push_back(g);
	WeaponFireEffects k;
	k.firearm = false;
	k.bullet_material = 1;
	t->defs.push_back(k);
	t->default_detonation = kFrag;
	return t;
}

std::unique_ptr<Syn> make_syn(std::shared_ptr<FireEffectTable> p_fire = small_fire_table()) {
	auto s = std::make_unique<Syn>();
	REQUIRE(s->db.set("fx/syn/doc", simple_doc(1).p));
	uint32_t seed = 10;
	add_effect(*s, kMuzz1p, seed++, { 1.0f, 0.75f });
	add_effect(*s, kMuzz3p, seed++, { 1.0f });
	add_effect(*s, kShell, seed++, { 1.0f });
	add_effect(*s, kTracer, seed++, { 1.0f }, 0, false, 1024);
	add_effect(*s, kImpactA, seed++, { 1.0f });
	add_effect(*s, kImpactB, seed++, { 1.0f });
	add_effect(*s, kImpactMetal, seed++, { 1.0f }, 50.0f);
	add_effect(*s, kSoldier, seed++, { 1.0f });
	add_effect(*s, kWater, seed++, { 1.0f });
	add_effect(*s, kFrag, seed++, { 1.0f });
	REQUIRE(s->db.set(kSoundPatch, fxt::target(900)));
	s->seeds[kSoundPatch] = 900;
	add_effect(*s, kSounding, seed++, { 1.0f }, 0, true);
	for (const char *t : { kDecalConcrete, kDecalMetal, kDecalScuff, kSoundConcrete, kSoundMetal, kSoundWater }) {
		REQUIRE(s->db.set(t, fxt::target(seed)));
		s->seeds[t] = seed++;
	}
	// The grid: rows 0 (default, empty), 1 the bullet, 2 concrete (and material 0: the wall world of "late join"),
	// 3 metal, 4 water, 5 body, 6 chest, 7 head, 8 a decal-only surface.
	constexpr size_t kRows = 9;
	std::array<uint32_t, 256> map{};
	map[1] = 1;
	map[0] = 2;
	map[kConcrete] = 2;
	map[kMetal] = 3;
	map[kWaterMat] = 4;
	map[kSoldierBodyMaterial] = 5;
	map[kSoldierChestMaterial] = 6;
	map[kSoldierHeadMaterial] = 7;
	map[kScuff] = 8;
	fxt::Partition p;
	p.seed = 500;
	p.add({ "MaterialGridData", {} });
	fxt::Array idx{ "*i", {}, {} };
	idx.words.assign(map.begin(), map.end());
	fxt::Array rows{ "MaterialInteractionGridRow", {}, {} };
	for (size_t r = 0; r < kRows; ++r) {
		fxt::Array items{ "MaterialRelationPropertyPair", {}, {} };
		for (size_t c = 0; c < kRows; ++c) {
			fxt::Array props;
			if (r == 1) {
				switch (c) {
					case 2:
						props.words = cell_relations(p, *s, { kImpactA, kImpactB }, 30, false, kDecalConcrete, kSoundConcrete);
						break;
					case 3:
						props.words = cell_relations(p, *s, { kImpactMetal }, 0, true, kDecalMetal, kSoundMetal);
						break;
					case 4:
						props.words = cell_relations(p, *s, { kWater }, 0, false, nullptr, kSoundWater);
						break;
					case 5:
					case 6:
						props.words = cell_relations(p, *s, { kSoldier }, 90, false, nullptr, nullptr);
						break;
					case 7:
						props.words = cell_relations(p, *s, { kSoldier }, 0, false, nullptr, nullptr);
						break;
					case 8:
						props.words = { p.ref(p.add({ "MaterialRelationDecalData", { { "Decal", p.ext(s->seeds.at(kDecalScuff), 0) } } })).word };
						break;
					default:
						break;
				}
			}
			items.structs.push_back({ { "PhysicsMaterialProperties", p.array(std::move(props)) } });
		}
		rows.structs.push_back({ { "Items", p.array(std::move(items)) } });
	}
	fxt::set(p.instances[0].values, "MaterialIndexMap", p.array(std::move(idx)));
	fxt::set(p.instances[0].values, "DefaultMaterialIndex", i32(0));
	fxt::set(p.instances[0].values, "InteractionGrid", p.array(std::move(rows)));
	REQUIRE(s->db.set("fx/syn/grid", p));
	core::gameplay::WorkBudget budget;
	core::Result<ImpactTable> table = ImpactTable::load(s->db, "fx/syn/grid", budget);
	REQUIRE_MESSAGE(table.ok(), (table.ok() ? std::string() : table.error().message));
	s->impacts.emplace(std::move(*table));
	EffectLibrary::Sources src;
	src.fx.db = &s->db;
	const fxt::MemResolver *db = &s->db;
	src.fx.digest = [db](std::string_view p_name) { return db->digest(p_name); };
	src.impacts = &*s->impacts;
	s->library = std::make_unique<EffectLibrary>(src);
	s->fire = std::move(p_fire);
	return s;
}

hk::PresentClock clock_at(int64_t p_time) {
	hk::PresentClock c;
	c.predicted_valid = c.render_valid = true;
	c.predicted = c.render = p_time;
	return c;
}

hk::PresentEvent ev(hk::PresentKind p_kind, uint8_t p_slot, uint32_t p_id, uint16_t p_def, const Vec3 &p_pos, const Vec3 &p_dir, int64_t p_time = 0) {
	hk::PresentEvent e;
	e.kind = p_kind;
	e.timeline = p_kind == hk::PresentKind::OwnShot ? hk::PresentTimeline::Predicted : hk::PresentTimeline::Render;
	e.slot = p_slot;
	e.id = p_id;
	e.def = p_def;
	e.time = p_time;
	e.pos[0] = p_pos.x, e.pos[1] = p_pos.y, e.pos[2] = p_pos.z;
	e.dir[0] = p_dir.x, e.dir[1] = p_dir.y, e.dir[2] = p_dir.z;
	if (p_kind == hk::PresentKind::OwnShot || p_kind == hk::PresentKind::RemoteShot) {
		e.seed = hk::effect_seed(kMatch, p_slot, p_id);
		e.extra = 1;
		e.value = 1;
	}
	return e;
}

hk::PresentEvent impact(uint8_t p_slot, uint32_t p_id, uint8_t p_material, const Vec3 &p_pos, const Vec3 &p_normal, uint32_t p_ordinal = 0, uint8_t p_pellet = 0) {
	hk::PresentEvent e = ev(hk::PresentKind::Impact, p_slot, p_id, 0, p_pos, p_normal);
	e.material = p_material;
	e.pellet = p_pellet;
	e.extra = p_ordinal;
	e.seed = hk::impact_seed(hk::effect_seed(kMatch, p_slot, p_id), p_pellet, p_ordinal);
	return e;
}

float len(const Vec3 &a) {
	return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}
float dot3(const Vec3 &a, const Vec3 &b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vec3 sub3(const Vec3 &a, const Vec3 &b) {
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}
Vec3 cross3(const Vec3 &a, const Vec3 &b) {
	return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
bool near3(const Vec3 &a, const Vec3 &b, float p_eps = 1e-4f) {
	return len(sub3(a, b)) <= p_eps;
}
void check_frame(const Transform &p_t) {
	CHECK(std::fabs(len(p_t.right) - 1) < 1e-4f);
	CHECK(std::fabs(len(p_t.up) - 1) < 1e-4f);
	CHECK(std::fabs(len(p_t.forward) - 1) < 1e-4f);
	CHECK(std::fabs(dot3(p_t.right, p_t.up)) < 1e-4f);
	CHECK(std::fabs(dot3(p_t.up, p_t.forward)) < 1e-4f);
	CHECK(dot3(p_t.right, cross3(p_t.up, p_t.forward)) > 0.999f); // a rotation, no mirror
}

std::vector<const TriggerStart *> starts_of(const FxTriggers &p_t, std::string_view p_def) {
	std::vector<const TriggerStart *> out;
	for (const TriggerStart &s : p_t.starts()) {
		if (s.def != nullptr && s.def->name == p_def) {
			out.push_back(&s);
		}
	}
	return out;
}

const FxView kNear{ { 0, 1.6f, 0 }, { 0, 0, 1 } };

} // namespace

TEST_SUITE("fx-triggers-unit") {

TEST_CASE("frames, offsets, aim and fallback muzzles, seeds") {
	const Transform f = effect_frame({ 1, 2, 3 }, { 0, 0, 5 }, { 0, 1, 0 });
	check_frame(f);
	CHECK(near3(f.up, { 0, 0, 1 }));
	CHECK(near3(f.forward, { 0, 1, 0 }));
	CHECK(near3(f.trans, { 1, 2, 3 }));
	// Degenerate axis and hint: still a rotation.
	check_frame(effect_frame({}, {}, {}));
	check_frame(effect_frame({}, { 0, 1, 0 }, { 0, 1, 0 }));
	check_frame(effect_frame({}, { 0, 0, 1 }, { 0, 0, 1 }));
	check_frame(effect_frame({}, { std::nanf(""), 0, 0 }, { 0, 0, 1 }));
	// Weapon frame <-> effect frame: up = barrel (weapon z), forward = weapon up.
	Transform w;
	w.trans = { 4, 5, 6 };
	const Transform e = effect_frame_of_weapon(w);
	check_frame(e);
	CHECK(near3(e.up, { 0, 0, 1 }));
	CHECK(near3(e.forward, { 0, 1, 0 }));
	const Transform back = weapon_of_effect_frame(e);
	CHECK(near3(back.right, w.right));
	CHECK(near3(back.up, w.up));
	CHECK(near3(back.forward, w.forward));
	// FireEffectData Rotation: 90 degrees about y turns the forward axis to +x.
	const Transform r = fire_effect_offset({ 0, 0, 1 }, { 0, 90, 0 });
	CHECK(near3(r.forward, { 1, 0, 0 }));
	CHECK(near3(r.trans, { 0, 0, 1 }));
	// The aim muzzle: ahead of, right of and below the eye, +Y along the aim.
	const MuzzleSample a = aim_muzzle(3, { 0, 1.6f, 0 }, { 0, 0, 2 }, {});
	REQUIRE(a.valid);
	check_frame(a.transform);
	CHECK(near3(a.transform.up, { 0, 0, 1 }));
	CHECK(a.transform.trans.z == doctest::Approx(kFallbackMuzzleForward));
	CHECK(a.transform.trans.y == doctest::Approx(1.6f + kFallbackMuzzleUp));
	CHECK(std::fabs(a.transform.trans.x) == doctest::Approx(kFallbackMuzzleRight));
	CHECK_FALSE(aim_muzzle(3, { std::nanf(""), 0, 0 }, { 0, 0, 1 }, {}).valid);
	hk::PresentedSoldier soldier;
	soldier.slot = 4;
	soldier.position = { 10, 0, 10 };
	soldier.eye_height = 1.6f;
	soldier.yaw = 0.0f;
	soldier.pitch = 0.0f;
	const MuzzleSample fb = m2a_fake::crowd_muzzle(soldier);
	REQUIRE(fb.valid);
	CHECK(fb.slot == 4);
	CHECK(near3(fb.transform.up, { 0, 0, 1 })); // yaw 0 looks along +z (sim aim_direction)
	CHECK(fb.transform.trans.y == doctest::Approx(1.6f + kFallbackMuzzleUp));
	// Seeds: every purpose and index its own; the soldier seed does not depend on which kind played it.
	std::set<uint64_t> seen;
	for (uint8_t p = 0; p < 8; ++p) {
		for (uint32_t i = 0; i < 4; ++i) {
			CHECK(seen.insert(trigger_seed(77, TriggerPurpose(p), i)).second);
		}
	}
	CHECK(soldier_seed(kMatch, 1, 2, 0) == hk::impact_seed(hk::effect_seed(kMatch, 1, 2), 0, kSoldierOrdinal));
	CHECK(soldier_seed(kMatch, 1, 2, 0) != soldier_seed(kMatch, 1, 2, 1));
	CHECK(detonation_seed(kMatch, 1, 2) != detonation_seed(kMatch, 1, 3));
}

TEST_CASE("decal textures and placements") {
	const std::vector<std::string> tex{ "decals/impacts/concrete/decal_concrete_s_01_d", "decals/impacts/concrete/decal_concrete_s_01_n",
		"decals/impacts/glass/decal_glass_01_d", "decals/impacts/metal/decal_metal_s_01_d", "decals/impacts/metal/decal_metalglow_01_d",
		"decals/impacts/scorch/d_scorch_01_n", "decals/impacts/scorch/decal_scorch_01_c", "decals/impacts/decal_s_01_d" };
	CHECK(pick_decal_texture("Decals/Impacts/Concrete/Decal_Concrete_S_01", tex) == "decals/impacts/concrete/decal_concrete_s_01_d");
	CHECK(pick_decal_texture("decals/impacts/glass/decal_glass_s_01", tex) == "decals/impacts/glass/decal_glass_01_d");
	CHECK(pick_decal_texture("decals/impacts/metal/decal_metalarmor_s_01", tex) == "decals/impacts/metal/decal_metal_s_01_d");
	CHECK(pick_decal_texture("decals/impacts/scorch/decal_scorchmark_m_01", tex) == "decals/impacts/scorch/decal_scorch_01_c");
	CHECK(pick_decal_texture("decals/impacts/cloth/decal_cloth_s_01", tex).empty());
	CHECK(pick_decal_texture("", tex).empty());
	DecalTemplate d;
	d.size = 0.2f;
	d.random_size = 0.16f;
	d.random_rotation = 180.0f;
	float lo = 1, hi = 0, rmin = 10, rmax = -10;
	for (uint64_t s = 0; s < 500; ++s) {
		const DecalPlacement p = decal_placement(d, fx_mix64(s));
		lo = std::min(lo, p.size);
		hi = std::max(hi, p.size);
		rmin = std::min(rmin, p.rotation);
		rmax = std::max(rmax, p.rotation);
	}
	CHECK(lo >= 0.2f);
	CHECK(hi <= 0.36f);
	CHECK(hi - lo > 0.12f);
	CHECK(rmin >= -3.1416f);
	CHECK(rmax <= 3.1416f);
	CHECK(rmax - rmin > 5.5f);
	// Templates from synthetic EBX are covered by fx.install.cells on the retail ones; a missing one is an error.
	fxt::MemResolver db;
	CHECK_FALSE(load_decal_template(db, "decals/none", tex).ok());
	REQUIRE(db.set("decals/syn/target", fxt::target(3)));
	CHECK_FALSE(load_decal_template(db, "decals/syn/target", tex).ok()); // not a DecalTemplateData
}

TEST_CASE("decal keys: BC1 decode, the magenta key, the RGBA mip chain, hostile sizes") {
	// One 4-colour block: c0 = magenta (31, 0, 31), c1 = black; indices 0 (magenta) in row 0, 1 (black) in row 1, 2 and 3
	// (the 2/3 and 1/3 mixes) in rows 2 and 3.
	const uint16_t c0 = uint16_t((31u << 11) | 31u), c1 = 0;
	const std::array<uint8_t, 8> block{ uint8_t(c0 & 0xFF), uint8_t(c0 >> 8), uint8_t(c1 & 0xFF), uint8_t(c1 >> 8), 0x00, 0x55, 0xAA, 0xFF };
	core::Result<std::vector<uint8_t>> rgba = decode_bc1(block, 4, 4);
	REQUIRE(rgba.ok());
	REQUIRE(rgba->size() == 64);
	auto texel = [&](uint32_t p_x, uint32_t p_y) {
		const uint8_t *t = rgba->data() + (size_t(p_y) * 4 + p_x) * 4;
		return std::array<int, 4>{ t[0], t[1], t[2], t[3] };
	};
	CHECK(texel(1, 0) == std::array<int, 4>{ 255, 0, 255, 255 });
	CHECK(texel(2, 1) == std::array<int, 4>{ 0, 0, 0, 255 });
	CHECK(texel(0, 2) == std::array<int, 4>{ 170, 0, 170, 255 });
	CHECK(texel(3, 3) == std::array<int, 4>{ 85, 0, 85, 255 });
	CHECK(magenta_share(*rgba) == doctest::Approx(0.25f));
	// The 3-colour mode (c0 <= c1): index 3 is transparent black.
	const std::array<uint8_t, 8> three{ 0, 0, uint8_t(c0 & 0xFF), uint8_t(c0 >> 8), 0xFF, 0xFF, 0xFF, 0xFF };
	core::Result<std::vector<uint8_t>> t3 = decode_bc1(three, 4, 4);
	REQUIRE(t3.ok());
	CHECK((*t3)[3] == 0);
	// Un-keying: magenta becomes transparent; a grey texel blended half over magenta comes back grey at alpha 0.5; a
	// texel with no magenta in it keeps its alpha and becomes its grey (G); a bluish texel over magenta (stone's cracks,
	// R < B) comes back grey, not blue.
	std::vector<uint8_t> px{ 255, 0, 255, 255, 191, 64, 191, 255, 100, 120, 90, 255, 128, 0, 255, 255 };
	unkey_magenta(px);
	CHECK(px[3] == 0);
	CHECK(px[7] == doctest::Approx(128).epsilon(0.02));
	CHECK(px[4] == doctest::Approx(127).epsilon(0.03));
	CHECK(px[5] == doctest::Approx(127).epsilon(0.03));
	CHECK(px[6] == doctest::Approx(127).epsilon(0.03));
	CHECK(std::vector<uint8_t>(px.begin() + 8, px.begin() + 12) == std::vector<uint8_t>{ 120, 120, 120, 255 });
	CHECK(px[12] == px[14]); // no blue left
	CHECK(px[12] == 0);
	CHECK(px[15] == doctest::Approx(127).epsilon(0.02));
	// The mip chain of a 4 x 2 level: 4x2, 2x1, 1x1; alpha-weighted (a transparent texel adds no colour).
	std::vector<uint8_t> level(4 * 2 * 4, 0);
	for (size_t i = 0; i < 8; ++i) {
		level[i * 4 + 0] = 200;
		level[i * 4 + 3] = (i % 2 == 0) ? 255 : 0;
	}
	uint32_t levels = 0;
	const std::vector<uint8_t> chain = rgba_mip_chain(level, 4, 2, &levels);
	CHECK(levels == 3);
	REQUIRE(chain.size() == (8 + 2 + 1) * 4);
	CHECK(chain[32] == 200); // level 1 texel 0: red from the opaque texels only
	CHECK(chain[35] == doctest::Approx(128).epsilon(0.02));
	// Hostile: too few bytes, a zero or oversized level.
	CHECK_FALSE(decode_bc1(std::span<const uint8_t>(block.data(), 7), 4, 4).ok());
	CHECK_FALSE(decode_bc1(block, 8, 4).ok());
	CHECK_FALSE(decode_bc1(block, 0, 4).ok());
	CHECK_FALSE(decode_bc1(block, 5000, 4).ok());
	CHECK(rgba_mip_chain(level, 4, 4, &levels).empty());
	CHECK(levels == 0);
	CHECK(rgba_mip_chain(level, 0, 2, nullptr).empty());
	CHECK(magenta_share({}) == 0.0f);
}

TEST_CASE("decal texture rule: BC3 decode, alpha, magenta key, opaque masks refused, hostile input") {
	// A BC3 block: alpha endpoints 255 / 0 (8-value mode), indices 0 (255) in texels 0..7, 1 (0) in texels 8..15;
	// colour c0 = white, c1 = black, all index 0 (white).
	std::array<uint8_t, 16> bc3{};
	bc3[0] = 255;
	bc3[1] = 0;
	// 3-bit indices: texels 8..15 index 1 -> bits 24..47 = 0b001 repeated.
	uint64_t abits = 0;
	for (int i = 8; i < 16; ++i) {
		abits |= uint64_t(1) << (3 * i);
	}
	for (int i = 0; i < 6; ++i) {
		bc3[size_t(2 + i)] = uint8_t(abits >> (8 * i));
	}
	bc3[8] = 0xFF;
	bc3[9] = 0xFF; // c0 = white
	bc3[10] = 0;
	bc3[11] = 0; // c1 = black; indices 0
	core::Result<std::vector<uint8_t>> d3 = decode_bc3(bc3, 4, 4);
	REQUIRE(d3.ok());
	CHECK((*d3)[3] == 255);
	CHECK((*d3)[8 * 4 + 3] == 0);
	CHECK((*d3)[0] == 255);
	CHECK(transparent_share(*d3) == doctest::Approx(0.5f));
	CHECK_FALSE(decode_bc3(std::span<const uint8_t>(bc3.data(), 15), 4, 4).ok());
	CHECK_FALSE(decode_bc3(bc3, 0, 4).ok());
	// The BC3 block has a real alpha: kept as prepared.
	core::Result<DecalTexturePrep> a = prepare_decal_texture(DecalPixelFormat::Bc3, bc3, 4, 4);
	REQUIRE(a.ok());
	CHECK(a->kind == DecalTextureKind::Alpha);
	CHECK(a->usable());
	CHECK(a->rgba_chain.empty());
	// A magenta-keyed BC1 block (a quarter key, the rest magenta blends and black): un-keyed to an RGBA8 chain.
	const uint16_t m0 = uint16_t((31u << 11) | 31u);
	const std::array<uint8_t, 8> keyed{ uint8_t(m0 & 0xFF), uint8_t(m0 >> 8), 0, 0, 0x00, 0x55, 0xAA, 0xFF };
	core::Result<DecalTexturePrep> k = prepare_decal_texture(DecalPixelFormat::Bc1, keyed, 4, 4);
	REQUIRE(k.ok());
	CHECK(k->kind == DecalTextureKind::MagentaKeyed);
	CHECK(k->magenta == doctest::Approx(0.25f));
	CHECK(k->transparent >= kMinDecalTransparentShare);
	CHECK(k->levels == 3);
	CHECK(k->rgba_chain.size() == (16 + 4 + 1) * 4);
	// decal_brick_s_01_d's shape: a red background around a yellow-green blob, opaque BC1, no key: refused.
	const uint16_t red = uint16_t(31u << 11), yellow = uint16_t((31u << 11) | (63u << 5));
	const std::array<uint8_t, 8> mask{ uint8_t(yellow & 0xFF), uint8_t(yellow >> 8), uint8_t(red & 0xFF), uint8_t(red >> 8), 0x05, 0x50, 0x05, 0x50 };
	core::Result<DecalTexturePrep> o = prepare_decal_texture(DecalPixelFormat::Bc1, mask, 4, 4);
	REQUIRE(o.ok());
	CHECK(o->kind == DecalTextureKind::Opaque);
	CHECK_FALSE(o->usable());
	CHECK(o->transparent == 0.0f);
	// An opaque RGBA8 level is refused too; one with a quarter see-through is kept.
	std::vector<uint8_t> rgba(4 * 4 * 4, 200);
	CHECK(prepare_decal_texture(DecalPixelFormat::Rgba8, rgba, 4, 4)->kind == DecalTextureKind::Opaque);
	for (size_t i = 0; i < 4; ++i) {
		rgba[i * 4 + 3] = 0;
	}
	CHECK(prepare_decal_texture(DecalPixelFormat::Rgba8, rgba, 4, 4)->kind == DecalTextureKind::Alpha);
	// No alpha to read (BC4/BC5/L8/RG8, or BC2): refused without decoding.
	CHECK(prepare_decal_texture(DecalPixelFormat::Other, rgba, 4, 4)->kind == DecalTextureKind::Unsupported);
	CHECK(std::string(decal_texture_kind_name(DecalTextureKind::Opaque)) == "opaque");
	// Hostile: short data, zero or oversized levels are errors, never a read past the end.
	CHECK_FALSE(prepare_decal_texture(DecalPixelFormat::Rgba8, std::span<const uint8_t>(rgba.data(), 63), 4, 4).ok());
	CHECK_FALSE(prepare_decal_texture(DecalPixelFormat::Bc1, std::span<const uint8_t>(keyed.data(), 4), 4, 4).ok());
	CHECK_FALSE(prepare_decal_texture(DecalPixelFormat::Bc3, bc3, 8, 8).ok());
	CHECK_FALSE(prepare_decal_texture(DecalPixelFormat::Bc1, keyed, 0, 4).ok());
	CHECK_FALSE(prepare_decal_texture(DecalPixelFormat::Rgba8, rgba, 5000, 4).ok());
	CHECK(transparent_share({}) == 0.0f);
}

TEST_CASE("name lists: fire table blueprints, impact sounds, effects, decals") {
	std::unique_ptr<Syn> s = make_syn();
	const std::vector<std::string> bp = s->fire->blueprints();
	CHECK(bp == std::vector<std::string>{ kMuzz1p, kMuzz3p, kTracer, kFrag, kShell });
	CHECK(impact_sound_names(*s->impacts) == std::vector<std::string>{ kSoundConcrete, kSoundMetal, kSoundWater });
	CHECK(impact_effect_names(*s->impacts) == std::vector<std::string>{ kImpactA, kImpactB, kImpactMetal, kSoldier, kWater });
	CHECK(impact_decal_names(*s->impacts) == std::vector<std::string>{ std::string(kScorchDecal), kDecalConcrete, kDecalMetal, kDecalScuff });
	CHECK(s->library->preload(bp, 2) == 0);
	CHECK(s->library->preload(impact_effect_names(*s->impacts), 2) == 0);
}

TEST_CASE("shots: 1p and 3p muzzles, shells per firearm shot, fallbacks, knife, the 1p flash fraction") {
	std::unique_ptr<Syn> s = make_syn();
	FxWorld world;
	FxTriggerConfig cfg;
	cfg.own_slot = 1;
	cfg.match_seed = kMatch;
	FxTriggers t(world, *s->library, s->fire, cfg);
	// The own 1p muzzle: weapon at (0, 1.5, 0.5) looking along +z.
	MuzzleSample own;
	own.slot = 1;
	own.valid = true;
	own.first_person = true;
	own.transform = effect_frame({ 0, 1.5f, 0.5f }, { 0, 0, 1 }, { 0, 1, 0 });
	t.on_muzzles(std::span<const MuzzleSample>(&own, 1));
	hk::PresentEvent shot = ev(hk::PresentKind::OwnShot, 1, 0, 0, { 0, 1.6f, 0 }, { 0, 0, 1 });
	t.on_events(std::span<const hk::PresentEvent>(&shot, 1), clock_at(0), kNear);
	{
		const auto m = starts_of(t, kMuzz1p);
		REQUIRE(m.size() == 1);
		CHECK(m[0]->first_person);
		CHECK(m[0]->started);
		CHECK(m[0]->purpose == TriggerPurpose::Muzzle);
		CHECK(m[0]->seed == trigger_seed(shot.seed, TriggerPurpose::Muzzle, 0));
		CHECK(m[0]->transform == own.transform);
		const auto sh = starts_of(t, kShell);
		REQUIRE(sh.size() == 1);
		CHECK(sh[0]->purpose == TriggerPurpose::Shell);
		CHECK(sh[0]->seed == trigger_seed(shot.seed, TriggerPurpose::Shell, 1));
		// The shell sits 0.55 m behind the muzzle along the barrel, 0.013 m left and 0.002 m lower.
		const Vec3 d = sub3(sh[0]->transform.trans, own.transform.trans);
		CHECK(d.z == doctest::Approx(-0.55f).epsilon(1e-3));
		CHECK(d.y == doctest::Approx(-0.002f).epsilon(1e-2));
		CHECK(std::fabs(d.x) == doctest::Approx(0.013f).epsilon(1e-2));
		check_frame(sh[0]->transform);
		CHECK(starts_of(t, kTracer).empty()); // no kTracer flag
	}
	CHECK(t.stats().muzzle_starts == 1);
	CHECK(t.stats().shells == 1);
	CHECK(t.stats().firearm_shots == 1);
	CHECK(t.stats().muzzle_fallbacks == 0);
	// The muzzle follows its sample (UpdateTransform).
	own.transform.trans.x += 0.1f;
	t.on_muzzles(std::span<const MuzzleSample>(&own, 1));
	CHECK(t.stats().attached_updates == 1);
	// A remote shot: with a crowd muzzle (the m2a_fake stand-in for tp), then without one (counted fallback).
	hk::PresentedSoldier other;
	other.slot = 5;
	other.position = { 3, 0, 3 };
	other.yaw = 0.5f;
	const MuzzleSample crowd = m2a_fake::crowd_muzzle(other);
	t.on_muzzles(std::span<const MuzzleSample>(&crowd, 1));
	hk::PresentEvent remote = ev(hk::PresentKind::RemoteShot, 5, 9, 0, { 3, 1.6f, 3 }, { std::sin(0.5f), 0, std::cos(0.5f) });
	t.on_events(std::span<const hk::PresentEvent>(&remote, 1), clock_at(0), kNear);
	{
		const auto m = starts_of(t, kMuzz3p);
		REQUIRE(m.size() == 1);
		CHECK_FALSE(m[0]->first_person);
		CHECK(m[0]->transform == crowd.transform);
		CHECK(starts_of(t, kMuzz1p).empty());
	}
	t.on_muzzles({});
	remote.id = 10;
	remote.seed = hk::effect_seed(kMatch, 5, 10);
	t.on_events(std::span<const hk::PresentEvent>(&remote, 1), clock_at(0), kNear);
	CHECK(t.stats().muzzle_fallbacks == 1);
	CHECK(starts_of(t, kMuzz3p).size() == 1);
	// The knife: no muzzle, no shell, not a firearm shot.
	hk::PresentEvent knife = ev(hk::PresentKind::OwnShot, 1, 1, 2, { 0, 1.6f, 0 }, { 0, 0, 1 });
	t.on_events(std::span<const hk::PresentEvent>(&knife, 1), clock_at(0), kNear);
	CHECK(t.starts().empty());
	CHECK(t.stats().firearm_shots == 3);
	CHECK(t.stats().shells == 3);
	// Shells off.
	FxTriggerConfig off = cfg;
	off.shells = false;
	t.set_config(off);
	hk::PresentEvent next = ev(hk::PresentKind::OwnShot, 1, 2, 0, { 0, 1.6f, 0 }, { 0, 0, 1 });
	t.on_events(std::span<const hk::PresentEvent>(&next, 1), clock_at(0), kNear);
	CHECK(starts_of(t, kShell).empty());
	CHECK(t.stats().shells == 3);
	t.set_config(cfg);
	// E8 (3)'s rule on the synthetic 1p muzzle (component 1, SpawnProbability 0.75): 1,000 OwnShots.
	t.on_muzzles(std::span<const MuzzleSample>(&own, 1));
	uint32_t flash = 0, shots = 0;
	for (uint32_t i = 100; i < 1100; ++i) {
		hk::PresentEvent e = ev(hk::PresentKind::OwnShot, 1, i, 0, { 0, 1.6f, 0 }, { 0, 0, 1 });
		t.on_events(std::span<const hk::PresentEvent>(&e, 1), clock_at(0), kNear);
		for (const TriggerStart *m : starts_of(t, kMuzz1p)) {
			++shots;
			flash += fx_component_selected(*m->def, 1, m->seed, kTierHigh) ? 1 : 0;
		}
		fx_step_frame(world, 1.0f / 60.0f);
	}
	CHECK(shots == 1000);
	CHECK(flash >= 710);
	CHECK(flash <= 790);
	MESSAGE("synthetic 1p flash component present in " << flash << " of 1000 shots");
	CHECK(t.stats().double_starts == 0);
}

TEST_CASE("tracers: from the muzzle onto the aim over the converge distance, ended by the impact or the time to live") {
	std::unique_ptr<Syn> s = make_syn();
	FxWorld world;
	FxTriggerConfig cfg;
	cfg.own_slot = 1;
	FxTriggers t(world, *s->library, s->fire, cfg);
	MuzzleSample own;
	own.slot = 1;
	own.valid = true;
	own.first_person = true;
	own.transform = effect_frame({ 0.15f, 1.48f, 0.55f }, { 0, 0, 1 }, { 0, 1, 0 });
	t.on_muzzles(std::span<const MuzzleSample>(&own, 1));
	const int64_t u = sw::kTimeUnitsPerSecond;
	hk::PresentEvent shot = ev(hk::PresentKind::OwnShot, 1, 0, 0, { 0, 1.6f, 0 }, { 0, 0, 1 }, 10 * u);
	shot.flags = hk::present_flags::kTracer;
	t.on_events(std::span<const hk::PresentEvent>(&shot, 1), clock_at(10 * u), kNear);
	{
		const auto tr = starts_of(t, kTracer);
		REQUIRE(tr.size() == 1);
		CHECK_FALSE(tr[0]->first_person);
		CHECK(near3(tr[0]->transform.trans, own.transform.trans)); // age 0: at the muzzle
		CHECK(near3(tr[0]->transform.up, { 0, 0, 1 }));
	}
	CHECK(t.live_tracers() == 1);
	CHECK(t.stats().tracer_shots == 1);
	CHECK(t.stats().tracers == 1);
	fx_step_frame(world, 1.0f / 60.0f);
	t.on_events({}, clock_at(10 * u + u / 10), kNear); // 90 m: still flying (TTL 1.5 s = 1,350 m)
	CHECK(t.live_tracers() == 1);
	// The shot's impact at 120 m ends it once it gets there.
	hk::PresentEvent hit = impact(1, 0, kConcrete, { 0, 1.6f, 120 }, { 0, 0, -1 });
	hit.timeline = hk::PresentTimeline::Predicted;
	hit.time = 10 * u + u / 8;
	t.on_events(std::span<const hk::PresentEvent>(&hit, 1), clock_at(10 * u + u / 8), kNear); // 112.5 m
	CHECK(t.live_tracers() == 1);
	t.on_events({}, clock_at(10 * u + u / 7), kNear); // 128.6 m
	CHECK(t.live_tracers() == 0);
	// A tracer started late (0.1 s old: 90 m) is already on the aim ray (past 30 m).
	hk::PresentEvent late = ev(hk::PresentKind::OwnShot, 1, 1, 0, { 0, 1.6f, 0 }, { 0, 0, 1 }, 20 * u);
	late.flags = hk::present_flags::kTracer;
	t.on_events(std::span<const hk::PresentEvent>(&late, 1), clock_at(20 * u + u / 10), kNear);
	{
		const auto tr = starts_of(t, kTracer);
		REQUIRE(tr.size() == 1);
		CHECK(near3(tr[0]->transform.trans, { 0, 1.6f, 90 }, 1e-3f));
		CHECK(tr[0]->start_age == doctest::Approx(0.1f));
	}
	t.on_events({}, clock_at(22 * u), kNear); // past the time to live
	CHECK(t.live_tracers() == 0);
	// Tracers off: counted as tracer shots, not started.
	FxTriggerConfig off = cfg;
	off.tracers = false;
	t.set_config(off);
	hk::PresentEvent quiet = ev(hk::PresentKind::OwnShot, 1, 2, 0, { 0, 1.6f, 0 }, { 0, 0, 1 }, 30 * u);
	quiet.flags = hk::present_flags::kTracer;
	t.on_events(std::span<const hk::PresentEvent>(&quiet, 1), clock_at(30 * u), kNear);
	CHECK(t.stats().tracer_shots == 3);
	CHECK(t.stats().tracers == 2);
	// More than kMaxTracers live at once: the oldest is stopped (counted).
	t.set_config(cfg);
	for (uint32_t i = 0; i < kMaxTracers + 5; ++i) {
		hk::PresentEvent e = ev(hk::PresentKind::OwnShot, 1, 100 + i, 0, { 0, 1.6f, 0 }, { 0, 0, 1 }, 40 * u);
		e.flags = hk::present_flags::kTracer;
		t.on_events(std::span<const hk::PresentEvent>(&e, 1), clock_at(40 * u), kNear);
	}
	CHECK(t.live_tracers() <= kMaxTracers);
	CHECK(t.stats().tracer_steals == 5);
}

TEST_CASE("impacts: cell choice by seed, spread cone, mirror, decal and sound cues, cull distance, empty cells") {
	std::unique_ptr<Syn> s = make_syn();
	FxWorld world;
	FxTriggerConfig cfg;
	cfg.own_slot = 1;
	cfg.match_seed = kMatch;
	FxTriggers t(world, *s->library, s->fire, cfg);
	const Vec3 n{ 0, 0, -1 };
	uint32_t a = 0, b = 0;
	float worst = 0;
	for (uint32_t i = 0; i < 200; ++i) {
		const hk::PresentEvent e = impact(3, i, kConcrete, { 0, 1, 10 }, n);
		t.on_events(std::span<const hk::PresentEvent>(&e, 1), clock_at(0), kNear);
		REQUIRE(t.starts().size() == 1);
		const TriggerStart &st = t.starts()[0];
		CHECK(st.seed == trigger_seed(e.seed, TriggerPurpose::ImpactEffect, 0));
		a += st.def->name == kImpactA ? 1 : 0;
		b += st.def->name == kImpactB ? 1 : 0;
		const size_t expect = std::min<size_t>(1, size_t(fx_unit(trigger_seed(e.seed, TriggerPurpose::ImpactChoice, 0)) * 2.0f));
		CHECK(st.index == expect);
		check_frame(st.transform);
		worst = std::max(worst, std::acos(std::clamp(dot3(st.transform.up, n), -1.0f, 1.0f)) * 180.0f / 3.14159265f);
		REQUIRE(t.decals().size() == 1);
		CHECK(t.decals()[0].decal == kDecalConcrete);
		CHECK(near3(t.decals()[0].transform.up, n));
		CHECK(t.decals()[0].seed == trigger_seed(e.seed, TriggerPurpose::Decal, 0));
		REQUIRE(t.sounds().size() == 1);
		CHECK(t.sounds()[0].patch == kSoundConcrete);
		CHECK(t.sounds()[0].seed == e.seed);
		CHECK(near3(t.sounds()[0].position, { 0, 1, 10 }));
	}
	CHECK(a + b == 200);
	CHECK(a >= 70);
	CHECK(b >= 70);
	CHECK(worst <= 30.01f);
	CHECK(worst >= 15.0f);
	MESSAGE("concrete cell: " << a << " / " << b << " choices over 200 seeds, widest spread " << worst << " degrees (limit 30)");
	// Mirror (metal): the effect's +Y is the shot direction reflected about the normal.
	hk::PresentEvent shot = ev(hk::PresentKind::RemoteShot, 3, 500, 0, { 0, 1, 0 }, { 0.6f, 0, 0.8f });
	t.on_events(std::span<const hk::PresentEvent>(&shot, 1), clock_at(0), kNear);
	const hk::PresentEvent metal = impact(3, 500, kMetal, { 6, 1, 8 }, { 0, 0, -1 });
	t.on_events(std::span<const hk::PresentEvent>(&metal, 1), clock_at(0), kNear);
	{
		const auto m = starts_of(t, kImpactMetal);
		REQUIRE(m.size() == 1);
		CHECK(near3(m[0]->transform.up, { 0.6f, 0, -0.8f }, 1e-3f));
		REQUIRE(t.decals().size() == 1);
		CHECK(t.decals()[0].decal == kDecalMetal);
		CHECK(t.sounds().size() == 1);
	}
	// Beyond the metal effect's CullDistance (50 m) from the camera: not started, no decal, no sound.
	const FxView far{ { 0, 1, 500 }, { 0, 0, 1 } };
	const hk::PresentEvent culled = impact(3, 501, kMetal, { 6, 1, 8 }, { 0, 0, -1 });
	const uint64_t before = t.stats().impacts_culled;
	t.on_events(std::span<const hk::PresentEvent>(&culled, 1), clock_at(0), far);
	CHECK(t.stats().impacts_culled == before + 1);
	CHECK(t.decals().empty());
	CHECK(t.sounds().empty());
	REQUIRE(t.starts().size() == 1);
	CHECK_FALSE(t.starts()[0].started);
	// Water: effect and sound, no decal. A decal-only cell: decal, no effect. Empty: counted.
	const hk::PresentEvent water = impact(3, 502, kWaterMat, { 0, 0, 5 }, { 0, 1, 0 });
	t.on_events(std::span<const hk::PresentEvent>(&water, 1), clock_at(0), kNear);
	CHECK(starts_of(t, kWater).size() == 1);
	CHECK(t.decals().empty());
	CHECK(t.sounds().size() == 1);
	const hk::PresentEvent scuff = impact(3, 503, kScuff, { 0, 0, 5 }, { 0, 1, 0 });
	t.on_events(std::span<const hk::PresentEvent>(&scuff, 1), clock_at(0), kNear);
	CHECK(t.starts().empty());
	REQUIRE(t.decals().size() == 1);
	CHECK(t.decals()[0].decal == kDecalScuff);
	const uint64_t empty_before = t.stats().impacts_empty;
	const hk::PresentEvent nothing = impact(3, 504, kEmptyMat, { 0, 0, 5 }, { 0, 1, 0 });
	t.on_events(std::span<const hk::PresentEvent>(&nothing, 1), clock_at(0), kNear);
	CHECK(t.stats().impacts_empty == empty_before + 1);
	CHECK(t.starts().empty());
	// Impacts off: nothing.
	FxTriggerConfig off = cfg;
	off.impacts = false;
	t.set_config(off);
	const hk::PresentEvent quiet = impact(3, 505, kConcrete, { 0, 0, 5 }, { 0, 1, 0 });
	t.on_events(std::span<const hk::PresentEvent>(&quiet, 1), clock_at(0), kNear);
	CHECK(t.starts().empty());
	CHECK(t.decals().empty());
	// Decals off: effect and sound, no decal.
	off = cfg;
	off.decals = false;
	t.set_config(off);
	const hk::PresentEvent no_decal = impact(3, 506, kConcrete, { 0, 0, 5 }, { 0, 1, 0 });
	t.on_events(std::span<const hk::PresentEvent>(&no_decal, 1), clock_at(0), kNear);
	CHECK(t.starts().size() == 1);
	CHECK(t.decals().empty());
	CHECK(t.sounds().size() == 1);
	CHECK(t.stats().double_starts == 0);
}

TEST_CASE("soldier cells: own contact, Hit confirmation, remote Hit, own victim, zones, seeds") {
	std::unique_ptr<Syn> s = make_syn();
	FxWorld world;
	FxTriggerConfig cfg;
	cfg.own_slot = 1;
	cfg.match_seed = kMatch;
	FxTriggers t(world, *s->library, s->fire, cfg);
	auto contact = [](uint8_t p_shooter, uint32_t p_id, uint8_t p_victim, sw::HitZone p_zone) {
		hk::PresentEvent e = ev(hk::PresentKind::SoldierContact, p_shooter, p_id, 0, { 0, 1.4f, 10 }, { 0, 0, -1 });
		e.timeline = hk::PresentTimeline::Predicted;
		e.aux = p_victim;
		e.value = float(uint8_t(p_zone));
		e.seed = 0; // cl leaves the contact's seed 0
		return e;
	};
	auto hit = [](uint8_t p_shooter, uint32_t p_id, uint8_t p_victim, sw::HitZone p_zone, uint8_t p_flags) {
		hk::PresentEvent e = ev(hk::PresentKind::Hit, p_shooter, p_id, 0xFFFF, { 0, 1.4f, 10 }, { 0, 0, -1 });
		e.timeline = (p_flags & (hk::present_flags::kOwnShooter | hk::present_flags::kOwnVictim)) != 0 ? hk::PresentTimeline::Immediate : hk::PresentTimeline::Render;
		e.aux = p_victim;
		e.material = uint32_t(p_zone);
		e.flags = p_flags;
		e.value = 25;
		e.seed = 0;
		return e;
	};
	const hk::PresentEvent own = contact(1, 7, 5, sw::HitZone::Head);
	t.on_events(std::span<const hk::PresentEvent>(&own, 1), clock_at(0), kNear);
	{
		const auto st = starts_of(t, kSoldier);
		REQUIRE(st.size() == 1);
		CHECK(st[0]->seed == trigger_seed(soldier_seed(kMatch, 1, 7, 0), TriggerPurpose::ImpactEffect, 0));
		CHECK(near3(st[0]->transform.up, { 0, 0, -1 })); // the head cell has no spread
		CHECK(t.decals().empty());
	}
	const hk::PresentEvent confirm = hit(1, 7, 5, sw::HitZone::Head, hk::present_flags::kOwnShooter);
	t.on_events(std::span<const hk::PresentEvent>(&confirm, 1), clock_at(0), kNear);
	CHECK(t.starts().empty());
	CHECK(t.stats().hits_confirmed == 1);
	// A remote shooter's predicted contact plays nothing; its Hit plays the cell with the same seed rule.
	const hk::PresentEvent remote_contact = contact(3, 8, 6, sw::HitZone::Chest);
	t.on_events(std::span<const hk::PresentEvent>(&remote_contact, 1), clock_at(0), kNear);
	CHECK(t.starts().empty());
	const hk::PresentEvent remote_hit = hit(3, 8, 6, sw::HitZone::Chest, 0);
	t.on_events(std::span<const hk::PresentEvent>(&remote_hit, 1), clock_at(0), kNear);
	{
		const auto st = starts_of(t, kSoldier);
		REQUIRE(st.size() == 1);
		CHECK(st[0]->seed == trigger_seed(soldier_seed(kMatch, 3, 8, 0), TriggerPurpose::ImpactEffect, 0));
		CHECK(dot3(st[0]->transform.up, { 0, 0, -1 }) >= std::cos(90.01f * 3.14159265f / 180.0f));
	}
	// The own soldier hit: nothing (the 1p camera is inside its capsule).
	const hk::PresentEvent me = hit(4, 9, 1, sw::HitZone::Body, hk::present_flags::kOwnVictim);
	t.on_events(std::span<const hk::PresentEvent>(&me, 1), clock_at(0), kNear);
	CHECK(t.starts().empty());
	// Legs map to the body column.
	const hk::PresentEvent legs = contact(1, 10, 5, sw::HitZone::Legs);
	t.on_events(std::span<const hk::PresentEvent>(&legs, 1), clock_at(0), kNear);
	CHECK(starts_of(t, kSoldier).size() == 1);
	CHECK(t.stats().soldier_hits == 3);
	CHECK(t.stats().double_starts == 0);
	// The same contact delivered twice is a double start.
	t.on_events(std::span<const hk::PresentEvent>(&legs, 1), clock_at(0), kNear);
	CHECK(t.stats().double_starts == 1);
}

TEST_CASE("detonations: the def's effect, the default, duds, the scorch decal") {
	std::unique_ptr<Syn> s = make_syn();
	FxWorld world;
	FxTriggerConfig cfg;
	cfg.match_seed = kMatch;
	FxTriggers t(world, *s->library, s->fire, cfg);
	hk::PresentEvent d = ev(hk::PresentKind::Detonation, 2, 4, 1, { 5, 0.1f, 5 }, { 0, 1, 0 });
	t.on_events(std::span<const hk::PresentEvent>(&d, 1), clock_at(0), kNear);
	{
		const auto st = starts_of(t, kFrag);
		REQUIRE(st.size() == 1);
		CHECK(st[0]->seed == detonation_seed(kMatch, 2, 4));
		CHECK(near3(st[0]->transform.up, { 0, 1, 0 }));
		REQUIRE(t.decals().size() == 1);
		CHECK(t.decals()[0].decal == kScorchDecal);
	}
	hk::PresentEvent dud = d;
	dud.id = 5;
	dud.flags = hk::present_flags::kDud;
	t.on_events(std::span<const hk::PresentEvent>(&dud, 1), clock_at(0), kNear);
	CHECK(t.starts().empty());
	CHECK(t.decals().empty());
	CHECK(t.stats().duds == 1);
	hk::PresentEvent other = d;
	other.id = 6;
	other.def = 2; // the knife: no detonation effect of its own -> the default
	t.on_events(std::span<const hk::PresentEvent>(&other, 1), clock_at(0), kNear);
	CHECK(starts_of(t, kFrag).size() == 1);
	CHECK(t.stats().detonations == 2);
}

TEST_CASE("timelines: pending until the clock passes, start ages, invalid clocks, Immediate, the pending bound") {
	std::unique_ptr<Syn> s = make_syn();
	FxWorld world;
	FxTriggerConfig cfg;
	cfg.own_slot = 1;
	FxTriggers t(world, *s->library, s->fire, cfg);
	const int64_t u = sw::kTimeUnitsPerSecond;
	// A predicted shot ahead of the drawn time waits.
	hk::PresentEvent ahead = ev(hk::PresentKind::OwnShot, 1, 0, 0, { 0, 1.6f, 0 }, { 0, 0, 1 }, 5 * u);
	t.on_events(std::span<const hk::PresentEvent>(&ahead, 1), clock_at(5 * u - sw::kTimeUnitsPerTick), kNear);
	CHECK(t.pending() == 1);
	CHECK(t.starts().empty());
	t.on_events({}, clock_at(5 * u + u / 2), kNear);
	CHECK(t.pending() == 0);
	REQUIRE(starts_of(t, kMuzz3p).size() == 1);
	CHECK(starts_of(t, kMuzz3p)[0]->start_age == doctest::Approx(0.5f));
	// An invalid render clock holds a remote shot; Immediate events never wait.
	hk::PresentClock invalid = clock_at(0);
	invalid.render_valid = false;
	hk::PresentEvent remote = ev(hk::PresentKind::RemoteShot, 4, 1, 0, { 0, 1.6f, 0 }, { 0, 0, 1 }, 0);
	t.on_events(std::span<const hk::PresentEvent>(&remote, 1), invalid, kNear);
	CHECK(t.pending() == 1);
	hk::PresentEvent now = impact(4, 77, kConcrete, { 0, 0, 5 }, { 0, 1, 0 });
	now.timeline = hk::PresentTimeline::Immediate;
	now.time = 99 * u;
	t.on_events(std::span<const hk::PresentEvent>(&now, 1), invalid, kNear);
	REQUIRE(t.starts().size() == 1);
	CHECK(t.starts()[0].start_age == 0.0f);
	t.on_events({}, clock_at(u), kNear);
	CHECK(t.pending() == 0);
	REQUIRE(starts_of(t, kMuzz3p).size() == 1);
	CHECK(starts_of(t, kMuzz3p)[0]->slot == 4);
	CHECK(starts_of(t, kMuzz3p)[0]->start_age == doctest::Approx(1.0f));
	// The pending list keeps the newest kMaxPendingStarts; the oldest are dropped and counted.
	std::vector<hk::PresentEvent> many;
	for (uint32_t i = 0; i < kMaxPendingStarts + 76; ++i) {
		many.push_back(ev(hk::PresentKind::OwnShot, 1, 1000 + i, 0, { 0, 1.6f, 0 }, { 0, 0, 1 }, 100 * u));
	}
	t.on_events(many, clock_at(2 * u), kNear);
	CHECK(t.pending() == kMaxPendingStarts);
	CHECK(t.stats().pending_dropped == 76);
	CHECK(t.stats().pending_now == kMaxPendingStarts);
	t.on_events({}, clock_at(101 * u), kNear);
	CHECK(t.pending() == 0);
	CHECK(starts_of(t, kMuzz3p).size() == kMaxPendingStarts);
	// A huge age clamps to kMaxStartAge.
	hk::PresentEvent old = ev(hk::PresentKind::OwnShot, 1, 5000, 0, { 0, 1.6f, 0 }, { 0, 0, 1 }, 0);
	t.on_events(std::span<const hk::PresentEvent>(&old, 1), clock_at(int64_t(1.0e7) * u), kNear);
	REQUIRE(starts_of(t, kMuzz3p).size() == 1);
	CHECK(starts_of(t, kMuzz3p)[0]->start_age == kMaxStartAge);
}

TEST_CASE("duplicates: a replayed stream starts nothing twice (E8 (7) double starts counted)") {
	std::unique_ptr<Syn> s = make_syn();
	FxWorld world;
	FxTriggerConfig cfg;
	cfg.own_slot = 1;
	cfg.match_seed = kMatch;
	FxTriggers t(world, *s->library, s->fire, cfg);
	std::vector<hk::PresentEvent> stream;
	for (uint32_t i = 0; i < 20; ++i) {
		hk::PresentEvent shot = ev(hk::PresentKind::OwnShot, 1, i, 0, { 0, 1.6f, 0 }, { 0, 0, 1 });
		shot.flags = (i % 2) == 0 ? hk::present_flags::kTracer : uint8_t(0);
		stream.push_back(shot);
		stream.push_back(impact(1, i, kConcrete, { 0, 1, 10 }, { 0, 0, -1 }));
	}
	t.on_events(stream, clock_at(0), kNear);
	const FxTriggerStats first = t.stats();
	CHECK(first.muzzle_starts == 20);
	CHECK(first.shells == 20);
	CHECK(first.tracer_shots == 10);
	CHECK(first.impacts == 20);
	CHECK(first.sounds == 20);
	CHECK(first.double_starts == 0);
	t.on_events(stream, clock_at(0), kNear);
	CHECK(t.starts().empty());
	CHECK(t.decals().empty());
	CHECK(t.sounds().empty());
	CHECK(t.stats().double_starts == 40);
	CHECK(t.stats().muzzle_starts == 20);
	// A second impact of the same shot (another ordinal) is not a duplicate.
	const hk::PresentEvent second = impact(1, 0, kConcrete, { 0, 1, 12 }, { 0, 0, -1 }, 1);
	t.on_events(std::span<const hk::PresentEvent>(&second, 1), clock_at(0), kNear);
	CHECK(t.starts().size() == 1);
}

TEST_CASE("hostile events and inputs: non-finite, unknown kinds and defs, no table, no library impacts") {
	std::unique_ptr<Syn> s = make_syn();
	FxWorld world;
	FxTriggers t(world, *s->library, s->fire, {});
	std::vector<hk::PresentEvent> bad;
	hk::PresentEvent nan = ev(hk::PresentKind::OwnShot, 1, 0, 0, { std::nanf(""), 0, 0 }, { 0, 0, 1 });
	bad.push_back(nan);
	hk::PresentEvent inf = impact(1, 0, kConcrete, { 0, 0, 0 }, { 0, std::numeric_limits<float>::infinity(), 0 });
	bad.push_back(inf);
	hk::PresentEvent huge = impact(1, 1, kConcrete, { 3.0e38f, 0, 0 }, { 0, 1, 0 });
	bad.push_back(huge);
	hk::PresentEvent kind = ev(hk::PresentKind::OwnShot, 1, 2, 0, { 0, 0, 0 }, { 0, 0, 1 });
	kind.kind = hk::PresentKind(200);
	bad.push_back(kind);
	hk::PresentEvent timeline = ev(hk::PresentKind::OwnShot, 1, 3, 0, { 0, 0, 0 }, { 0, 0, 1 });
	timeline.timeline = hk::PresentTimeline(9);
	bad.push_back(timeline);
	t.on_events(bad, clock_at(0), kNear);
	CHECK(t.stats().rejected == 5);
	CHECK(t.starts().empty());
	// Unknown defs, a zero direction, slot 255, material 255, pellet 255: counted or handled, no crash.
	std::vector<hk::PresentEvent> odd;
	odd.push_back(ev(hk::PresentKind::OwnShot, 255, 4, 0xFFFE, { 0, 0, 0 }, { 0, 0, 0 }));
	odd.push_back(ev(hk::PresentKind::RemoteShot, 255, 5, 0, { 0, 0, 0 }, { 0, 0, 0 }));
	odd.push_back(impact(255, 6, 255, { 0, 0, 0 }, { 0, 0, 0 }, 0xFFFFFFFFu, 255));
	hk::PresentEvent det = ev(hk::PresentKind::Detonation, 255, 7, 0xFFFE, { 0, 0, 0 }, { 0, 0, 0 });
	odd.push_back(det);
	hk::PresentEvent zone = ev(hk::PresentKind::Hit, 9, 8, 0xFFFF, { 0, 0, 0 }, { 0, 0, 0 });
	zone.material = 0xFFFFFFFFu;
	odd.push_back(zone);
	hk::PresentEvent value = ev(hk::PresentKind::SoldierContact, 0xFF, 9, 0, { 0, 0, 0 }, { 0, 0, 0 });
	value.value = std::nanf("");
	odd.push_back(value);
	t.on_events(odd, clock_at(0), kNear);
	CHECK(t.stats().unknown_defs == 2);
	CHECK(t.stats().impacts_empty >= 1);
	for (const TriggerStart &st : t.starts()) {
		check_frame(st.transform);
	}
	// Non-finite muzzles are ignored; a non-finite camera is treated as the origin.
	MuzzleSample m;
	m.valid = true;
	m.first_person = true;
	m.slot = 1;
	m.transform.trans = { std::nanf(""), 0, 0 };
	m.velocity = { std::nanf(""), 0, 0 };
	t.on_muzzles(std::span<const MuzzleSample>(&m, 1));
	const FxView nan_view{ { std::nanf(""), 0, 0 }, { 0, 0, 1 } };
	hk::PresentEvent shot = ev(hk::PresentKind::OwnShot, 1, 50, 0, { 0, 1.6f, 0 }, { 0, 0, 1 });
	t.on_events(std::span<const hk::PresentEvent>(&shot, 1), clock_at(0), nan_view);
	CHECK(t.stats().muzzle_fallbacks == 2);
	// No fire table and a library without an impact table: nothing starts, nothing crashes.
	EffectLibrary::Sources none;
	EffectLibrary empty(none);
	FxTriggers bare(world, empty, nullptr, {});
	std::vector<hk::PresentEvent> all = odd;
	all.push_back(ev(hk::PresentKind::OwnShot, 1, 60, 0, { 0, 1.6f, 0 }, { 0, 0, 1 }));
	all.push_back(impact(1, 60, kConcrete, { 0, 0, 5 }, { 0, 1, 0 }));
	bare.on_muzzles({});
	bare.on_events(all, clock_at(0), kNear);
	CHECK(bare.starts().empty());
	CHECK(bare.stats().impacts_empty >= 1);
	// A named blueprint that does not compile is counted once per start attempt.
	auto broken = std::make_shared<FireEffectTable>(*small_fire_table());
	broken->defs[0].first_person[0].blueprint = "fx/syn/missing";
	broken->defs[0].third_person[0].blueprint = "fx/syn/missing"; // no muzzle sample: the 3p list plays
	FxTriggers b(world, *s->library, broken, {});
	hk::PresentEvent own = ev(hk::PresentKind::OwnShot, 1, 61, 0, { 0, 1.6f, 0 }, { 0, 0, 1 });
	b.on_events(std::span<const hk::PresentEvent>(&own, 1), clock_at(0), kNear);
	CHECK(b.stats().compile_failures == 1);
	CHECK(b.stats().shells == 1);
	// load_fire_effects on a resolver without the weapons: empty lists and notes, never an error.
	fxt::MemResolver nothing;
	std::vector<core::gameplay::WeaponDef> defs(3);
	defs[1].firing_data = "weapons/none/none#00000000-0000-0000-0000-000000000000";
	defs[2].firing_data = "garbage";
	core::Result<FireEffectTable> loaded = load_fire_effects(nothing, defs);
	REQUIRE(loaded.ok());
	CHECK(loaded->defs.size() == 3);
	CHECK(loaded->notes.size() == 3);
	CHECK(loaded->blueprints().empty());
}

TEST_CASE("effect sounds: a sound component's FxEvent becomes an EffectSoundCue") {
	std::unique_ptr<Syn> s = make_syn();
	FxWorld world;
	core::Result<std::shared_ptr<const EffectDef>> def = s->library->get(kSounding);
	REQUIRE(def.ok());
	EffectStart st;
	st.transform.trans = { 1, 2, 3 };
	core::Result<EffectHandle> h = world.start(*def, st);
	REQUIRE(h.ok());
	fx_step_frame(world, 1.0f / 60.0f);
	std::vector<EffectSoundCue> cues;
	effect_sound_cues(world.events(), cues);
	REQUIRE(cues.size() == 1);
	CHECK(cues[0].patch == kSoundPatch);
	CHECK(cues[0].start);
	CHECK(cues[0].effect == ((uint64_t(h->slot) << 32) | h->generation));
	CHECK(near3(cues[0].position, { 1, 2, 3 }));
	world.stop(*h, StopMode::Kill);
	fx_step_frame(world, 1.0f / 60.0f);
	cues.clear();
	effect_sound_cues(world.events(), cues);
	CHECK(std::all_of(cues.begin(), cues.end(), [](const EffectSoundCue &c) { return !c.start; }));
}

TEST_CASE("late join: two clients' event streams through two trigger instances start the same variants (E15)") {
	// cl's real code makes both streams: two ClientProjectiles (clients of slots 1 and 2) get the same remote shots
	// of slot 7 against the same wall world; client B joins late (from shot 15) and steps its render time on another
	// schedule. Each client's RemoteShot events (built as client_session.cpp does) and the drained Impact events go
	// through its own FxTriggers; every (event, purpose, index) both started has the same effect and seed.
	const std::shared_ptr<const sw::WeaponTable> table = tw::synthetic_table();
	const uint16_t rifle = tw::id_of(*table, "weapons/m16a4/u_m16a4");
	auto fire = std::make_shared<FireEffectTable>();
	fire->defs.assign(table->defs.size(), rifle_effects());
	auto capsules = std::make_shared<core::gameplay::CapsulePoseTable>();
	capsules->version = core::gameplay::kCapsuleTableVersion;
	capsules->capsule_count = 1;
	for (size_t i = 0; i < core::gameplay::kCapsulePitchBuckets; ++i) {
		capsules->pitch_deg[i] = -64.0f + 4.0f * float(i);
	}
	core::gameplay::CapsuleLocal c{};
	c.b[1] = 1.0f;
	c.radius = 0.3f;
	capsules->steady.assign(core::gameplay::kCapsuleStances * core::gameplay::kCapsulePitchBuckets, c);
	capsules->transition.assign(core::gameplay::kCapsuleTransitions * core::gameplay::kCapsuleTransitionFractions * core::gameplay::kCapsulePitchBuckets, c);
	capsules->bound_radius = 2.0f;
	REQUIRE(core::gameplay::capsule_pose_table_valid(*capsules));
	const std::unique_ptr<sim::CollisionWorld> wall = tw::wall_world({ 25.0f });

	struct Client {
		std::unique_ptr<hk::ClientProjectiles> projectiles;
		std::unique_ptr<hk::PresentationEvents> ring = std::make_unique<hk::PresentationEvents>();
		std::unique_ptr<Syn> syn;
		std::unique_ptr<FxWorld> world = std::make_unique<FxWorld>();
		std::unique_ptr<FxTriggers> triggers;
		std::map<std::tuple<int, uint8_t, uint32_t, uint8_t, int, uint32_t>, std::pair<std::string, uint64_t>> log;
		std::map<std::tuple<int, uint8_t, uint32_t, uint8_t, int, uint32_t>, Vec3> where;
		uint32_t first_shot = 0;
		double step = 1.0;
	};
	Client clients[2];
	for (int k = 0; k < 2; ++k) {
		Client &cl = clients[k];
		hk::ClientProjectilesConfig pc;
		pc.own_slot = uint8_t(1 + k);
		core::Result<std::unique_ptr<hk::ClientProjectiles>> made = hk::ClientProjectiles::create(table, capsules, kMatch, pc);
		REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
		cl.projectiles = std::move(*made);
		cl.syn = make_syn(fire);
		FxTriggerConfig cfg;
		cfg.own_slot = pc.own_slot;
		cfg.match_seed = kMatch;
		cl.triggers = std::make_unique<FxTriggers>(*cl.world, *cl.syn->library, cl.syn->fire, cfg);
	}
	clients[1].first_shot = 15;
	clients[1].step = 0.37;
	constexpr uint32_t kShots = 40;
	constexpr sim::Tick kFirst = 300;
	auto shot_of = [&](uint32_t p_i) {
		net::RemoteShot r;
		r.slot = 7;
		r.shot_index = p_i;
		r.def = rifle;
		r.time = sw::tick_start(kFirst + 6 * p_i);
		r.origin = { 0.0f, 1.6f, 0.0f };
		const float yaw = 0.02f * float(int(p_i % 9) - 4);
		r.direction = { std::sin(yaw), 0.0f, std::cos(yaw) };
		r.tracer = (p_i % 2) == 0;
		r.aim = r.direction;
		return r;
	};
	for (Client &cl : clients) {
		std::vector<hk::PresentEvent> pending_shots;
		for (uint32_t i = cl.first_shot; i < kShots; ++i) {
			const net::RemoteShot r = shot_of(i);
			cl.projectiles->queue_remote(r);
			hk::PresentEvent e; // client_session.cpp's RemoteShot event
			e.kind = hk::PresentKind::RemoteShot;
			e.timeline = hk::PresentTimeline::Render;
			e.slot = r.slot;
			e.id = r.shot_index;
			e.def = r.def;
			e.pos[0] = r.origin[0], e.pos[1] = r.origin[1], e.pos[2] = r.origin[2];
			e.dir[0] = r.direction[0], e.dir[1] = r.direction[1], e.dir[2] = r.direction[2];
			e.flags = r.tracer ? hk::present_flags::kTracer : uint8_t(0);
			e.seed = hk::effect_seed(kMatch, r.slot, r.shot_index);
			e.value = r.power;
			e.extra = r.pellets;
			e.time = r.time;
			pending_shots.push_back(e);
		}
		const std::vector<net::OtherPose> others;
		std::vector<hk::PresentEvent> drained(4096);
		const double start = double(kFirst) - 2.0 + (cl.first_shot > 0 ? 6.0 * cl.first_shot : 0.0);
		for (double rt = start; rt <= double(kFirst + 6 * kShots + 60); rt += cl.step) {
			cl.projectiles->step_remote(rt, *wall, others, *cl.ring);
			std::vector<hk::PresentEvent> frame(pending_shots.begin(), pending_shots.end());
			pending_shots.clear();
			const size_t n = cl.ring->drain(drained);
			frame.insert(frame.end(), drained.begin(), drained.begin() + std::ptrdiff_t(n));
			hk::PresentClock clock;
			clock.render_valid = clock.predicted_valid = true;
			clock.render = clock.predicted = sw::SimTime(rt * double(sw::kTimeUnitsPerTick));
			cl.triggers->on_muzzles({});
			cl.triggers->on_events(frame, clock, kNear);
			for (const TriggerStart &s : cl.triggers->starts()) {
				const auto key = std::make_tuple(int(s.kind), s.slot, s.id, s.pellet, int(s.purpose), s.index);
				CHECK(cl.log.emplace(key, std::make_pair(s.def->name, s.seed)).second);
				cl.where[key] = s.transform.trans;
			}
			fx_step_frame(*cl.world, 1.0f / 60.0f);
		}
	}
	size_t common = 0, muzzles = 0, impacts = 0, tracers = 0;
	std::set<std::string> impact_variants;
	for (const auto &[key, value] : clients[1].log) {
		const auto it = clients[0].log.find(key);
		REQUIRE_MESSAGE(it != clients[0].log.end(), "late client started a variant the first one did not");
		CHECK(it->second == value);
		const TriggerPurpose p = TriggerPurpose(std::get<4>(key));
		if (p != TriggerPurpose::Tracer) { // a tracer starts where its age puts it, and the ages differ by design
			CHECK(near3(clients[0].where[key], clients[1].where[key], 1e-3f));
		}
		++common;
		muzzles += p == TriggerPurpose::Muzzle ? 1 : 0;
		tracers += p == TriggerPurpose::Tracer ? 1 : 0;
		if (p == TriggerPurpose::ImpactEffect) {
			++impacts;
			impact_variants.insert(value.first);
		}
	}
	const uint32_t late = kShots - clients[1].first_shot;
	CHECK(muzzles == late);
	CHECK(impacts == late);
	uint32_t flagged = 0;
	for (uint32_t i = clients[1].first_shot; i < kShots; ++i) {
		flagged += (i % 2) == 0 ? 1 : 0;
	}
	CHECK(tracers == flagged);
	CHECK(impact_variants.size() == 2); // both of the concrete cell's effects were chosen, on both clients alike
	CHECK(clients[0].triggers->stats().double_starts == 0);
	CHECK(clients[1].triggers->stats().double_starts == 0);
	CHECK(clients[0].triggers->stats().remote_shots == kShots);
	CHECK(clients[1].triggers->stats().remote_shots == late);
	MESSAGE("late join: " << common << " starts of " << late << " shots compared, " << impacts << " impacts, " << impact_variants.size() << " impact variants");
}

} // TEST_SUITE fx-triggers-unit

// ---- fx.install.cells -----------------------------------------------------------------------------------------

namespace {

std::optional<core::InstallSnapshot> g_before;

struct Retail {
	core::InstallReader reader;
	std::shared_ptr<core::Install> install;
};

bool open_retail(Retail &r_r) {
	if (!r_r.reader.locate({})) {
		bf3::test::skip_install_case(r_r.reader.describe().c_str());
		return false;
	}
	if (!g_before) {
		g_before = r_r.reader.snapshot();
	}
	core::Result<std::shared_ptr<core::Install>> install = core::Install::open(r_r.reader);
	REQUIRE_MESSAGE(install.ok(), (install.ok() ? std::string() : install.error().message));
	r_r.install = *install;
	return true;
}

std::string leaf(std::string_view p_name) {
	const size_t s = p_name.rfind('/');
	return std::string(s == std::string_view::npos ? p_name : p_name.substr(s + 1));
}

double since(std::chrono::steady_clock::time_point p_t0) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_t0).count();
}

// A level's fx sources as Bf3GameFx::load builds them (fx_demo's route): the level superbundle library, the impact
// table of its grid, an EffectLibrary over both.
struct LevelFx {
	std::unique_ptr<core::gameplay::WeaponSources> sources;
	std::optional<ImpactTable> impacts;
	std::unique_ptr<EffectLibrary> library;
};

std::unique_ptr<LevelFx> level_fx(const std::shared_ptr<core::Install> &p_install, std::string_view p_level, bool p_load_all) {
	const std::optional<core::Level> info = p_install->level(p_level);
	REQUIRE(info.has_value());
	auto out = std::make_unique<LevelFx>();
	core::Result<core::gameplay::WeaponSources> src = core::gameplay::open_weapon_sources(p_install, info->superbundle);
	REQUIRE_MESSAGE(src.ok(), (src.ok() ? std::string() : src.error().message));
	out->sources = std::make_unique<core::gameplay::WeaponSources>(std::move(*src));
	core::EbxLibrary &lib = *out->sources->level;
	if (p_load_all) {
		lib.load_all();
	}
	const std::optional<std::string> grid = core::gameplay::MaterialGrid::grid_of_level(lib, out->sources->level_partition);
	REQUIRE(grid.has_value());
	core::gameplay::WorkBudget budget{ kDefaultCompileWorkLimit };
	core::Result<ImpactTable> t = ImpactTable::load(lib, *grid, budget);
	REQUIRE_MESSAGE(t.ok(), (t.ok() ? std::string() : t.error().message));
	out->impacts.emplace(std::move(*t));
	EffectLibrary::Sources s;
	s.fx.db = &lib;
	s.fx.digest = core::gameplay::library_partition_digest(lib);
	s.impacts = &*out->impacts;
	out->library = std::make_unique<EffectLibrary>(s);
	return out;
}

} // namespace

TEST_SUITE("fx-install-cells") {

TEST_CASE("cells: Metro's fire effects, preload, the 1p flash fraction, in-game cells, cues; Caspian water") {
	Retail r;
	if (!open_retail(r)) {
		return;
	}
	const auto t0 = std::chrono::steady_clock::now();
	// The match's defs (sv's GunplayLevel) and the level's fx sources.
	core::Result<std::shared_ptr<const hk::GunplayLevel>> gl = hk::load_gunplay_level(r.install, "MP_Subway", "TeamDeathMatch0");
	REQUIRE_MESSAGE(gl.ok(), (gl.ok() ? std::string() : gl.error().message));
	const double s_gunplay = since(t0);
	const auto t1 = std::chrono::steady_clock::now();
	std::unique_ptr<LevelFx> metro = level_fx(r.install, "MP_Subway", true);
	const double s_fx = since(t1);
	const auto t2 = std::chrono::steady_clock::now();
	core::Result<FireEffectTable> fire = load_fire_effects(*metro->sources->level, (*gl)->defs);
	REQUIRE(fire.ok());
	const double s_fire = since(t2);
	for (const std::string &n : fire->notes) {
		MESSAGE("fire effects note: " << n);
	}
	CHECK(fire->defs.size() == (*gl)->defs.size());
	// The M16A3 (variant weapons/m16a4/u_m16a4), the M67 and the knife [verified: bf3dump, m2a-fx3.md §2].
	auto def_of = [&](std::string_view p_variant) -> const WeaponFireEffects * {
		for (size_t i = 0; i < (*gl)->defs.size(); ++i) {
			if ((*gl)->defs[i].variant == p_variant) {
				return &fire->defs[i];
			}
		}
		return nullptr;
	};
	const WeaponFireEffects *m16 = def_of("weapons/m16a4/u_m16a4");
	REQUIRE(m16 != nullptr);
	REQUIRE(m16->first_person.size() == 3);
	CHECK(m16->first_person[0].blueprint == "fx/weapons/muzzleflashes/generic/fx_muzz_generic_rifle_1p");
	CHECK(m16->first_person[0].offset.trans.z == doctest::Approx(0.99f));
	CHECK(m16->first_person[0].offset.trans.y == doctest::Approx(0.028f));
	CHECK(m16->first_person[0].update_transform);
	CHECK(m16->first_person[1].shell);
	CHECK(leaf(m16->first_person[1].blueprint) == "fx_shellejection_m");
	CHECK(m16->first_person[1].offset.trans.z == doctest::Approx(0.44f));
	CHECK(leaf(m16->third_person[0].blueprint) == "fx_muzz_generic_rifle_3p");
	CHECK(leaf(m16->tracer) == "fx_tracer_generic_parent");
	CHECK(m16->trace_frequency == 1);
	CHECK(m16->bullet_material == 1);
	CHECK(m16->firearm);
	CHECK(m16->converge == doctest::Approx(30.0f));
	CHECK(m16->tracer_speed > 500.0f);
	const WeaponFireEffects *m67 = def_of("weapons/m67/u_m67");
	REQUIRE(m67 != nullptr);
	CHECK_FALSE(m67->firearm);
	CHECK(m67->first_person.empty());
	CHECK(leaf(m67->detonation) == kGrenadeEffectName);
	CHECK(leaf(fire->default_detonation) == kGrenadeEffectName);
	const WeaponFireEffects *knife = def_of("weapons/knife/u_knife");
	REQUIRE(knife != nullptr);
	CHECK_FALSE(knife->firearm);
	CHECK(knife->tracer.empty());
	size_t firearms = 0, with_tracer = 0;
	for (const WeaponFireEffects &w : fire->defs) {
		firearms += w.firearm ? 1 : 0;
		with_tracer += w.tracer.empty() ? 0 : 1;
	}
	// E8 (1): every blueprint the kits and the impact table name compiles.
	const auto t3 = std::chrono::steady_clock::now();
	std::vector<std::string> names = fire->blueprints();
	const std::vector<std::string> impact_names = impact_effect_names(*metro->impacts);
	names.insert(names.end(), impact_names.begin(), impact_names.end());
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	const size_t failures = metro->library->preload(names, 0);
	const double s_preload = since(t3);
	CHECK(failures == 0);
	MESSAGE("Metro: " << (*gl)->defs.size() << " defs (" << firearms << " firearms, " << with_tracer << " with a tracer), " << fire->blueprints().size()
					  << " fire blueprints + " << impact_names.size() << " impact effects = " << names.size() << " preloaded, " << failures << " failures");
	CHECK(impact_sound_names(*metro->impacts).size() > 0);
	MESSAGE("Metro: " << impact_sound_names(*metro->impacts).size() << " impact sounds, " << impact_decal_names(*metro->impacts).size() << " decals");

	// E8 (3): over 1,000 predicted M16A3 shots the _1p_flash emitter is present in [0.71, 0.79] of them.
	std::shared_ptr<FireEffectTable> table = std::make_shared<FireEffectTable>(*fire);
	uint16_t m16_id = 0;
	for (size_t i = 0; i < (*gl)->defs.size(); ++i) {
		m16_id = (*gl)->defs[i].variant == "weapons/m16a4/u_m16a4" ? uint16_t(i) : m16_id;
	}
	FxWorld world;
	FxTriggerConfig cfg;
	cfg.own_slot = 1;
	cfg.match_seed = 0x1234;
	FxTriggers t(world, *metro->library, table, cfg);
	MuzzleSample own;
	own.slot = 1;
	own.valid = true;
	own.first_person = true;
	own.transform = effect_frame({ 0.1f, 1.5f, 0.7f }, { 0, 0, 1 }, { 0, 1, 0 });
	t.on_muzzles(std::span<const MuzzleSample>(&own, 1));
	core::Result<std::shared_ptr<const EffectDef>> muzzle = metro->library->get(m16->first_person[0].blueprint);
	REQUIRE(muzzle.ok());
	int32_t flash_component = -1;
	for (size_t c = 0; c < (*muzzle)->components.size(); ++c) {
		const EffectComponentDef &cd = (*muzzle)->components[c];
		if (cd.kind == ComponentKind::Emitter && cd.emitter < (*muzzle)->emitters.size() &&
				(*muzzle)->emitters[cd.emitter].name.find("_1p_flash") != std::string::npos) {
			flash_component = int32_t(c);
		}
	}
	REQUIRE(flash_component >= 0);
	uint32_t flash = 0, shots = 0, tracer_flags = 0;
	for (uint32_t i = 0; i < 1000; ++i) {
		hk::PresentEvent e = ev(hk::PresentKind::OwnShot, 1, i, m16_id, { 0, 1.6f, 0 }, { 0, 0, 1 }, int64_t(i) * sw::kTimeUnitsPerTick * 6);
		e.seed = hk::effect_seed(cfg.match_seed, 1, i);
		const bool tracer = m16->trace_frequency > 0 && (i % uint32_t(m16->trace_frequency)) == 0;
		e.flags = tracer ? hk::present_flags::kTracer : uint8_t(0);
		tracer_flags += tracer ? 1 : 0;
		t.on_events(std::span<const hk::PresentEvent>(&e, 1), clock_at(e.time), kNear);
		for (const TriggerStart *m : starts_of(t, m16->first_person[0].blueprint)) {
			++shots;
			flash += fx_component_selected(*m->def, uint32_t(flash_component), m->seed, kTierHigh) ? 1 : 0;
		}
		fx_step_frame(world, 0.1f);
	}
	CHECK(shots == 1000);
	CHECK(flash >= 710);
	CHECK(flash <= 790);
	// E8 (7): tracer starts = shots with tracer set, one shell per firearm shot.
	CHECK(t.stats().tracers == tracer_flags);
	CHECK(t.stats().shells == 1000);
	CHECK(t.stats().firearm_shots == 1000);
	CHECK(t.stats().double_starts == 0);
	MESSAGE("E8 (3): _1p_flash present in " << flash << " of " << shots << " M16A3 OwnShots (gate [710, 790]); tracers " << t.stats().tracers
											<< " / flagged " << tracer_flags << ", shells " << t.stats().shells);

	// E8 (5): in-game cells from Metro's collision triangles (the material value -> the sim's SurfaceMaterialTable ->
	// the PhysicsMaterialIndex that cl's Impact event carries) through the trigger code: fx's G5 named cells.
	const auto t4 = std::chrono::steady_clock::now();
	core::Result<core::level::LevelCollision> col = core::level::load_level_collision(r.install, "MP_Subway", "TeamDeathMatch0");
	REQUIRE_MESSAGE(col.ok(), (col.ok() ? std::string() : col.error().message));
	const double s_collision = since(t4);
	const sw::SurfaceMaterialTable &materials = *(*gl)->materials;
	struct Named {
		const char *name;
		uint8_t index;
		const char *effect;
		const char *decal;
	};
	const Named named[] = { { "Concrete", 8, "fx_impact_concrete_01_s_xp2", "decal_concrete_s_01" }, { "Brick", 2, "fx_impact_concrete_01_s", "decal_brick_s_01" },
		{ "Glass", 10, "fx_impact_glass_01_s", "decal_glass_s_01" }, { "Metal", 3, "fx_impact_metal_01_s", "decal_metalarmor_s_01" },
		{ "Water", 18, "fx_impact_water_s", "" } };
	std::map<uint8_t, const core::level::SoupTriangle *> by_material;
	std::map<uint8_t, size_t> triangles_of;
	for (const core::level::SoupTile &tile : col->soup.tiles) {
		for (const core::level::SoupTriangle &tri : tile.triangles) {
			const sw::SurfacePair pair = materials.static_pair(tri.material);
			if (!pair.resolved) {
				continue;
			}
			++triangles_of[pair.physics_material];
			by_material.emplace(pair.physics_material, &tri);
		}
	}
	{
		std::string census;
		for (const auto &[m, count] : triangles_of) {
			const ImpactCell *cell = metro->impacts->select(1, m);
			census += (census.empty() ? "" : ", ") + std::to_string(int(m)) + " " + std::string(metro->impacts->material_name(m)) + " " + std::to_string(count) +
					(cell != nullptr && !cell->effects.empty() ? " (" + leaf(cell->effects[0]) + ")" : " (no effect)");
		}
		MESSAGE("Metro collision triangles by PhysicsMaterialIndex: " << census);
	}
	uint32_t id = 5000;
	size_t checked = 0;
	bool metro_water = false;
	for (const Named &n : named) {
		const auto it = by_material.find(n.index);
		if (it == by_material.end()) {
			MESSAGE("Metro: no " << std::string(n.name) << " (" << int(n.index) << ") triangle");
			CHECK(std::string(n.name) == "Water"); // only water may be absent (E8 (5))
			continue;
		}
		metro_water = metro_water || n.index == 18;
		const core::level::SoupTriangle &tri = *it->second;
		const Vec3 a{ tri.v[0][0], tri.v[0][1], tri.v[0][2] }, b{ tri.v[1][0], tri.v[1][1], tri.v[1][2] }, c{ tri.v[2][0], tri.v[2][1], tri.v[2][2] };
		const Vec3 centre{ (a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, (a.z + b.z + c.z) / 3 };
		Vec3 normal = cross3(sub3(b, a), sub3(c, a));
		const float l = len(normal);
		normal = l > 0 ? Vec3{ normal.x / l, normal.y / l, normal.z / l } : Vec3{ 0, 1, 0 };
		hk::PresentEvent e = impact(9, id, n.index, centre, normal);
		e.def = m16_id;
		e.seed = hk::impact_seed(hk::effect_seed(cfg.match_seed, 9, id), 0, 0);
		++id;
		const FxView view{ { centre.x + normal.x * 5, centre.y + normal.y * 5, centre.z + normal.z * 5 }, { -normal.x, -normal.y, -normal.z } };
		t.on_events(std::span<const hk::PresentEvent>(&e, 1), clock_at(0), view);
		REQUIRE(t.starts().size() == 1);
		CHECK(t.starts()[0].started);
		CHECK(leaf(t.starts()[0].def->name) == n.effect);
		if (n.decal[0] != 0) {
			REQUIRE(t.decals().size() == 1);
			CHECK(leaf(t.decals()[0].decal) == n.decal);
		} else {
			CHECK(t.decals().empty());
		}
		CHECK(t.sounds().size() == 1);
		++checked;
		MESSAGE("Metro " << std::string(n.name) << " (" << int(n.index) << ", " << triangles_of[n.index] << " triangles): " << leaf(t.starts()[0].def->name) << ", decal "
						 << (t.decals().empty() ? std::string("-") : leaf(t.decals()[0].decal)) << ", sound "
						 << (t.sounds().empty() ? std::string("-") : leaf(t.sounds()[0].patch)));
	}
	CHECK(checked >= 4);
	// A soldier: the predicted contact on each zone (fx G5: fx_impact_soldier_body_s, no decal).
	for (const sw::HitZone z : { sw::HitZone::Head, sw::HitZone::Body, sw::HitZone::Chest, sw::HitZone::Legs }) {
		hk::PresentEvent e = ev(hk::PresentKind::SoldierContact, 1, id++, m16_id, { 0, 1.4f, 10 }, { 0, 0, -1 });
		e.timeline = hk::PresentTimeline::Predicted;
		e.aux = 4;
		e.value = float(uint8_t(z));
		e.seed = 0;
		t.on_events(std::span<const hk::PresentEvent>(&e, 1), clock_at(0), kNear);
		REQUIRE(t.starts().size() == 1);
		CHECK(leaf(t.starts()[0].def->name) == "fx_impact_soldier_body_s");
		CHECK(t.decals().empty());
	}
	// The M67's detonation.
	{
		hk::PresentEvent e = ev(hk::PresentKind::Detonation, 9, id++, 0xFFFF, { 0, 0.1f, 5 }, { 0, 1, 0 });
		t.on_events(std::span<const hk::PresentEvent>(&e, 1), clock_at(0), kNear);
		REQUIRE(t.starts().size() == 1);
		CHECK(leaf(t.starts()[0].def->name) == kGrenadeEffectName);
		REQUIRE(t.decals().size() == 1);
		CHECK(t.decals()[0].decal == kScorchDecal);
		CHECK(metro->sources->level->partition_by_name(kScorchDecal) != nullptr);
	}
	// Every decal the cells name and the scorch mark is a DecalTemplateData in the level with a diffuse texture.
	std::vector<std::string> textures;
	for (const core::EbxLibrary::Entry &e : metro->sources->level->entries()) {
		if (e.partition->primary().type_name() == "TextureAsset") {
			textures.emplace_back(e.name);
		}
	}
	size_t with_texture = 0;
	for (const std::string &d : impact_decal_names(*metro->impacts)) {
		core::Result<DecalTemplate> dt = load_decal_template(*metro->sources->level, d, textures);
		CHECK_MESSAGE(dt.ok(), (dt.ok() ? d : dt.error().message));
		if (!dt.ok()) {
			continue;
		}
		with_texture += dt->texture.empty() ? 0 : 1;
		const std::string l = leaf(d);
		if (l == "decal_concrete_s_01" || l == "decal_brick_s_01" || l == "decal_glass_s_01" || l == "decal_metalarmor_s_01" || l == "decal_scorchmark_m_01") {
			CHECK_MESSAGE(!dt->texture.empty(), d); // the named cells' decals and the scorch mark
			CHECK_MESSAGE(metro->sources->level->partition_by_name(dt->texture) != nullptr, dt->texture);
		}
		CHECK_MESSAGE(dt->tiles_x * dt->tiles_y >= 1, d);
		MESSAGE("decal " << leaf(d) << ": size " << dt->size << " + " << dt->random_size << ", rotation " << dt->rotation << " +- " << dt->random_rotation
						 << ", tiles " << dt->tiles_x << " x " << dt->tiles_y << ", texture " << (dt->texture.empty() ? std::string("-") : leaf(dt->texture)));
	}
	MESSAGE("decals: " << with_texture << " of " << impact_decal_names(*metro->impacts).size() << " with a diffuse texture");
	CHECK(with_texture + 2 >= impact_decal_names(*metro->impacts).size()); // cloth and plastic have none on Metro
	// The decal-texture rule (fx::prepare_decal_texture, the same call FxDecalAssets::load makes on Bf3GameFx's loader
	// thread): every texture a decal would draw has a real see-through part. Metro's brick decal texture is an opaque
	// channel mask (no key, no alpha) and is refused; the named cells' other decals and the scorch mark are drawn.
	{
		std::map<std::string, std::vector<std::string>> texture_of; // texture -> the decal leaves naming it
		for (const std::string &d : impact_decal_names(*metro->impacts)) {
			core::Result<DecalTemplate> dt = load_decal_template(*metro->sources->level, d, textures);
			if (dt.ok() && !dt->texture.empty()) {
				texture_of[dt->texture].push_back(leaf(d));
			}
		}
		const auto tt = std::chrono::steady_clock::now();
		std::map<std::string, DecalTextureKind> kind_of; // by decal leaf
		size_t usable = 0;
		for (const auto &[texture, decals] : texture_of) {
			core::Result<core::res::LoadedTexture> loaded = core::res::load_texture(metro->sources->level->mount(0), texture);
			REQUIRE_MESSAGE(loaded.ok(), texture);
			core::Result<render::PreparedTexture> prepared = render::prepare_texture(*loaded, render::TextureRole::Color);
			REQUIRE_MESSAGE(prepared.ok(), texture);
			DecalPixelFormat pf = DecalPixelFormat::Other;
			if (prepared->format == render::GpuFormat::Bc1) {
				pf = DecalPixelFormat::Bc1;
			} else if (prepared->format == render::GpuFormat::Bc3) {
				pf = DecalPixelFormat::Bc3;
			} else if (prepared->format == render::GpuFormat::Rgba8) {
				pf = DecalPixelFormat::Rgba8;
			}
			core::Result<DecalTexturePrep> rule = prepare_decal_texture(pf, prepared->data, prepared->width, prepared->height);
			REQUIRE_MESSAGE(rule.ok(), texture);
			for (const std::string &d : decals) {
				kind_of[d] = rule->kind;
			}
			if (rule->usable()) {
				++usable;
				CHECK_MESSAGE(rule->transparent >= kMinDecalTransparentShare, texture);
				CHECK_MESSAGE(rule->transparent < 0.999f, texture); // and something to draw
			}
			std::string names;
			for (const std::string &d : decals) {
				names += (names.empty() ? "" : ", ") + d;
			}
			MESSAGE("decal texture " << leaf(texture) << " (" << names << "): " << std::string(render::gpu_format_name(prepared->format)) << " " << prepared->width
									 << "x" << prepared->height << ", magenta " << rule->magenta << ", see-through " << rule->transparent << " -> "
									 << std::string(decal_texture_kind_name(rule->kind)));
		}
		CHECK(kind_of["decal_brick_s_01"] == DecalTextureKind::Opaque);
		for (const char *d : { "decal_concrete_s_01", "decal_glass_s_01", "decal_metalarmor_s_01", "decal_scorchmark_m_01" }) {
			CHECK_MESSAGE(kind_of.count(d) == 1, std::string(d));
			CHECK_MESSAGE((kind_of.count(d) == 1 && (kind_of[d] == DecalTextureKind::Alpha || kind_of[d] == DecalTextureKind::MagentaKeyed)), std::string(d));
		}
		MESSAGE("decal textures: " << usable << " of " << texture_of.size() << " drawn (" << kind_of.size() << " decals), the rest refused, " << since(tt) << " s");
	}
	MESSAGE("Metro water: " << std::string(metro_water ? "a Water (18) triangle exists" : "no water") << " (" << triangles_of[18] << " triangles)");
	// The Caspian water fallback: the same trigger code on Caspian's grid (MP_007, lazily read).
	const auto t5 = std::chrono::steady_clock::now();
	std::unique_ptr<LevelFx> caspian = level_fx(r.install, "MP_007", false);
	{
		auto empty = std::make_shared<FireEffectTable>();
		FxTriggers ct(world, *caspian->library, empty, cfg);
		hk::PresentEvent e = impact(9, id++, 18, { 0, 0, 5 }, { 0, 1, 0 });
		e.def = 0xFFFF; // the default bullet material
		ct.on_events(std::span<const hk::PresentEvent>(&e, 1), clock_at(0), kNear);
		REQUIRE(ct.starts().size() == 1);
		CHECK(ct.starts()[0].started);
		CHECK(leaf(ct.starts()[0].def->name) == "fx_impact_water_s");
		CHECK(ct.decals().empty());
		MESSAGE("Caspian water (18): " << leaf(ct.starts()[0].def->name) << ", sound "
									   << (ct.sounds().empty() ? std::string("-") : leaf(ct.sounds()[0].patch)));
	}
	const double s_caspian = since(t5);
	CHECK(t.stats().double_starts == 0);
	MESSAGE("seconds: gunplay level " << s_gunplay << ", Metro fx sources " << s_fx << ", fire effects " << s_fire << ", preload " << s_preload
									  << ", collision " << s_collision << ", Caspian " << s_caspian << ", total " << since(t0));
}

// Last case of the suite (doctest runs file order).
TEST_CASE("install unchanged") {
	Retail r;
	if (!open_retail(r)) {
		return;
	}
	REQUIRE(g_before.has_value());
	const core::SnapshotDiff diff = core::diff_snapshots(*g_before, r.reader.snapshot());
	CHECK_MESSAGE(diff.clean(), diff.summary());
}

} // TEST_SUITE fx-install-cells
