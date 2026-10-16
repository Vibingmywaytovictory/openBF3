// M2a weapons on the retail install (read-only; docs/design/m2a-weapons.md §1, §14). Each suite exits 77 = skipped
// without an install, and every case prints the install.unchanged row.
//   weapons        (ctest sim.weapons)       grid pairs and materials, every Metro soup material, the oracle rows on
//                                            defs resolved from MP_Subway (W1, W2, W3, W5, W6, W9), W4's table
//   weapons-fuzz   (ctest sim.weapons.fuzz)  2,000 mutated retail WeaponDef blobs (and synthetic ones) through
//                                            decode_weapon_def, make_weapon_sim_def and 600 random ticks (W8; asan)
//   weapons-perf   (ctest sim.weapons.perf)  W11: 64 shooters on Metro, run_range_perf (wall clock, noisy)
#include "weapons_install_support.h"

#include "bf3/core/gameplay/weapon_blob.h"
#include "bf3/sim/weapons/damage.h"
#include "bf3/sim/weapons/weapon_checks.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace bf3;
using namespace bf3::sim;
using namespace bf3::sim::weapons;
using namespace bf3::test::weapons;

namespace {

constexpr std::string_view kFuzzVariants[] = { "weapons/m16a4/u_m16a4", "weapons/remington870/u_870", "weapons/m67/u_m67", "weapons/rpg7/u_rpg7" };

// Seeded mutations of one blob: bit flips, byte overwrites, truncation, and a copied span.
std::vector<uint8_t> mutate(const std::vector<uint8_t> &p_blob, std::mt19937 &r_rng) {
	std::vector<uint8_t> b = p_blob;
	if (b.empty()) {
		return b;
	}
	const uint32_t kind = r_rng() % 8;
	const int edits = 1 + int(r_rng() % 12);
	for (int e = 0; e < edits; ++e) {
		const size_t at = r_rng() % b.size();
		switch (kind) {
			case 0:
			case 1:
			case 2:
				b[at] ^= uint8_t(1u << (r_rng() % 8));
				break;
			case 3:
				b[at] = uint8_t(r_rng());
				break;
			case 4:
				b[at] = (r_rng() & 1) ? 0xFF : 0x7F; // the high bytes of floats and counts
				break;
			case 5: {
				const size_t from = r_rng() % b.size();
				const size_t n = std::min<size_t>(1 + r_rng() % 16, std::min(b.size() - at, b.size() - from));
				std::copy_n(p_blob.begin() + std::ptrdiff_t(from), n, b.begin() + std::ptrdiff_t(at));
				break;
			}
			default:
				b[at] ^= uint8_t(r_rng());
				break;
		}
	}
	if (kind == 6 && b.size() > 1) {
		b.resize(r_rng() % b.size());
	}
	return b;
}

struct FuzzTally {
	size_t mutants = 0, decoded = 0, accepted = 0, ran = 0, violations = 0, refused_shots = 0, dropped = 0;
	std::string first_violation;
};

void fuzz_blob(const core::gameplay::WeaponDef &p_def, const SurfaceMaterialTable &p_materials, const std::shared_ptr<const SurfaceMaterialTable> &p_shared,
		size_t p_count, uint32_t p_seed, const CollisionWorld &p_world, const HitTargetProvider &p_targets, FuzzTally &r_tally) {
	const std::vector<uint8_t> blob = core::gameplay::encode_weapon_def(p_def);
	REQUIRE(core::gameplay::decode_weapon_def(blob).ok());
	std::mt19937 rng(p_seed);
	for (size_t i = 0; i < p_count; ++i) {
		const std::vector<uint8_t> m = mutate(blob, rng);
		++r_tally.mutants;
		core::Result<core::gameplay::WeaponDef> d = core::gameplay::decode_weapon_def(m);
		if (!d) {
			continue;
		}
		++r_tally.decoded;
		core::Result<WeaponSimDef> sd = make_weapon_sim_def(*d, p_materials);
		if (!sd) {
			continue;
		}
		++r_tally.accepted;
		auto table = std::make_shared<WeaponTable>();
		table->defs.push_back(std::move(*sd));
		table->materials = p_shared;
		table->hash = weapon_table_hash(*table);
		const RandomRun r = random_run(table, 0, 0, p_seed + uint32_t(i), p_world, p_targets);
		++r_tally.ran;
		r_tally.violations += r.violations;
		r_tally.refused_shots += r.refused;
		r_tally.dropped += r.dropped;
		if (r_tally.first_violation.empty() && !r.first_violation.empty()) {
			r_tally.first_violation = r.first_violation;
		}
	}
}

} // namespace

TEST_SUITE("weapons") {
	TEST_CASE("sim.weapons: MP_Subway grid pairs, surface materials, zone properties") {
		WeaponsInstall *w = weapons_install();
		if (w == nullptr) {
			return;
		}
		const InstallWeapons &iw = w->weapons;
		CHECK(iw.grid_pairs.size() == 32'400);
		size_t resolved = 0, index_ok = 0, penetrable = 0;
		for (size_t i = 0; i < iw.grid_pairs.size(); ++i) {
			const core::gameplay::MaterialPairRef &p = iw.grid_pairs[i];
			resolved += p.resolved ? 1 : 0;
			index_ok += (p.flags_and_index & core::level::kMaterialPairMask) == i ? 1 : 0;
			penetrable += p.penetrable() ? 1 : 0;
		}
		std::printf("grid pairs: %zu, resolved %zu, index = low 24 bits %zu, penetrable %zu\n", iw.grid_pairs.size(), resolved, index_ok, penetrable);
		CHECK(resolved == iw.grid_pairs.size());
		CHECK(index_ok == iw.grid_pairs.size());
		CHECK(penetrable == 1'800); // topics/01 §1.7, C9
		const SurfaceMaterialTable &mt = *iw.materials;
		CHECK(mt.has_grid());
		CHECK(mt.grid_pair_count() == 32'400);
		CHECK(mt.zone_property(HitZone::Head) == 65);
		CHECK(mt.zone_property(HitZone::Body) == 66);
		CHECK(mt.zone_property(HitZone::Chest) == 158);
		CHECK(mt.zone_property(HitZone::Legs) == 91);
		CHECK(iw.capsules.size() == 13);
		CHECK(iw.max_health == 100.0f);
		CHECK(iw.stand_eye_height == doctest::Approx(1.6f));
		CHECK(iw.sprint_recover_seconds == doctest::Approx(0.12f));
		// The gate weapons' own pairs and their zone cells (the core.gameplay.weapons body-multiplier rows, now as the
		// sim def carries them).
		const core::Result<WeaponCheckInputs> in = install_weapon_check_inputs(iw);
		REQUIRE_MESSAGE(in.ok(), (in.ok() ? std::string() : in.error().message));
		for (const WeaponSimDef &d : in->table->defs) {
			std::printf("  %-36s property %3u never_penetrate %d zones %.2f %.2f %.2f %.2f, explosion zones %.2f %.2f %.2f %.2f (%s)\n", d.variant.c_str(),
					unsigned(d.projectile.property), d.projectile.never_penetrate_any ? 1 : 0, double(d.projectile.zone[0].protection_multiplier),
					double(d.projectile.zone[1].protection_multiplier), double(d.projectile.zone[2].protection_multiplier),
					double(d.projectile.zone[3].protection_multiplier), double(d.projectile.explosion.zone_multiplier[0]),
					double(d.projectile.explosion.zone_multiplier[1]), double(d.projectile.explosion.zone_multiplier[2]),
					double(d.projectile.explosion.zone_multiplier[3]), d.projectile.explosion.zone_multiplier_from_data ? "data" : "default");
			for (const auto &z : d.projectile.zone) {
				CHECK(z.present);
			}
		}
		check_install_unchanged(*w);
	}

	TEST_CASE("sim.weapons: every Metro soup material value resolves to a grid pair") {
		WeaponsInstall *w = weapons_install();
		if (w == nullptr) {
			return;
		}
		const core::Result<core::level::LevelCollision> c = core::level::load_level_collision(w->install, "MP_Subway", "TeamDeathMatch0");
		REQUIRE_MESSAGE(c.ok(), (c.ok() ? std::string() : c.error().message));
		const SurfaceMaterialTable &mt = *w->weapons.materials;
		size_t triangles = 0, unresolved = 0, terrain = 0, terrain_unresolved = 0;
		std::set<uint32_t> values, bad;
		for (const core::level::SoupTile &tile : c->soup.tiles) {
			for (const core::level::SoupTriangle &t : tile.triangles) {
				++triangles;
				values.insert(t.material);
				const bool is_terrain = (t.flags & core::level::kSoupTerrain) != 0;
				terrain += is_terrain ? 1 : 0;
				if (!mt.static_pair(t.material).resolved) {
					++unresolved;
					terrain_unresolved += is_terrain ? 1 : 0;
					bad.insert(t.material);
				}
			}
		}
		std::printf("Metro soup: %zu triangles (%zu terrain), %zu distinct material values, %zu triangles unresolved (%zu terrain), %zu distinct unresolved\n",
				triangles, terrain, values.size(), unresolved, terrain_unresolved, bad.size());
		for (const uint32_t v : bad) {
			std::printf("  unresolved material value 0x%08x\n", v);
			if (std::distance(bad.begin(), bad.find(v)) >= 16) {
				break;
			}
		}
		CHECK(triangles == c->soup.triangles);
		CHECK(unresolved == 0);
		check_install_unchanged(*w);
	}

	TEST_CASE("sim.weapons: the oracle rows on defs resolved from MP_Subway (W1 W2 W3 W5 W6 W9)") {
		WeaponsInstall *w = weapons_install();
		if (w == nullptr) {
			return;
		}
		const core::Result<WeaponCheckInputs> in = install_weapon_check_inputs(w->weapons);
		REQUIRE_MESSAGE(in.ok(), (in.ok() ? std::string() : in.error().message));
		const std::vector<WeaponCheckRow> rows = run_weapon_checks(*in);
		std::printf("%s", format_weapon_checks(rows).c_str());
		size_t gated = 0;
		for (const WeaponCheckRow &r : rows) {
			CHECK_MESSAGE(r.pass, r.gate << " " << r.name << ": " << r.measured << " (" << r.threshold << ")");
			gated += r.reported ? 0 : 1;
		}
		CHECK(gated >= 30);
		check_install_unchanged(*w);
	}

	TEST_CASE("sim.weapons: W4 the generated table of every infantry weapon") {
		WeaponsInstall *w = weapons_install();
		if (w == nullptr) {
			return;
		}
		const W4Table t = build_w4_table(w->weapons);
		std::printf("W4: %zu variants, %zu qualifying, %zu rows, %zu errors, %.2f s\n", t.variants, t.qualifying, t.rows.size(), t.errors, t.seconds);
		for (const W4Row &r : t.rows) {
			if (!r.error.empty()) {
				std::printf("  error %s: %s\n", r.variant.c_str(), r.error.c_str());
			}
			if (r.variant == "weapons/m16a4/u_m16a4" || r.variant == "weapons/model98b/u_m98b" || r.variant == "weapons/remington870/u_870") {
				std::printf("  %s: %.1f RPM, stk chest 0 m %d, 50 m %d, 100 m %d; head %d / %d / %d\n", r.variant.c_str(), r.rpm, r.shots_to_kill[0][2],
						r.shots_to_kill[3][2], r.shots_to_kill[5][2], r.shots_to_kill[0][0], r.shots_to_kill[3][0], r.shots_to_kill[5][0]);
			}
		}
		CHECK(t.variants == 172);
		CHECK(t.qualifying == 119);
		CHECK(t.rows.size() == 119);
		CHECK(t.errors == 0);
		const std::string tsv = format_w4_tsv(t);
		CHECK(size_t(std::count(tsv.begin(), tsv.end(), '\n')) == t.rows.size() + 1);
		check_install_unchanged(*w);
	}
}

TEST_SUITE("weapons-fuzz") {
	TEST_CASE("sim.weapons.fuzz: mutated synthetic blobs (no data)") {
		const auto materials = std::make_shared<const SurfaceMaterialTable>();
		const std::unique_ptr<CollisionWorld> world = wall_world({ 40.0f, 300.0f });
		const StaticTargets targets(random_run_targets());
		FuzzTally tally;
		uint32_t seed = 7001;
		for (const Synthetic &s : synthetic_gate_weapons()) {
			fuzz_blob(s.def, *materials, materials, 60, seed++, *world, targets, tally);
		}
		std::printf("fuzz (synthetic): %zu mutants, %zu decoded, %zu accepted, %zu ran 600 ticks, %zu violations\n", tally.mutants, tally.decoded, tally.accepted,
				tally.ran, tally.violations);
		CHECK_MESSAGE(tally.violations == 0, tally.first_violation);
		CHECK(tally.refused_shots == 0);
		CHECK(tally.dropped == 0);
		CHECK(tally.ran > 0);
	}

	TEST_CASE("sim.weapons.fuzz: 2,000 mutated retail blobs (M16A3, 870, M67, RPG-7)") {
		WeaponsInstall *w = weapons_install();
		if (w == nullptr) {
			return;
		}
		const std::unique_ptr<CollisionWorld> world = wall_world({ 40.0f, 300.0f });
		const StaticTargets targets(random_run_targets());
		FuzzTally tally;
		uint32_t seed = 20261015;
		for (const std::string_view v : kFuzzVariants) {
			core::Result<core::gameplay::WeaponDef> d = w->weapons.catalog->resolve(v);
			REQUIRE_MESSAGE(d.ok(), (d.ok() ? std::string() : d.error().message));
			fuzz_blob(*d, *w->weapons.materials, w->weapons.materials, 500, seed, *world, targets, tally);
			seed += 1000;
		}
		std::printf("fuzz (retail): %zu mutants, %zu decoded, %zu accepted, %zu ran 600 ticks, %zu violations\n", tally.mutants, tally.decoded, tally.accepted,
				tally.ran, tally.violations);
		CHECK(tally.mutants == 2'000);
		CHECK_MESSAGE(tally.violations == 0, tally.first_violation);
		CHECK(tally.refused_shots == 0);
		CHECK(tally.dropped == 0);
		check_install_unchanged(*w);
	}
}

TEST_SUITE("weapons-perf") {
	TEST_CASE("sim.weapons.perf: W11 64 shooters full auto on Metro (wall clock, noisy)") {
#if defined(__SANITIZE_ADDRESS__)
		// W11 is a dev-preset measurement; an instrumented build is 100x slower and says nothing about the budget.
		std::printf("W11: not measured under AddressSanitizer (the gate is the dev preset)\n");
		bf3::test::skip_install_case("asan build");
		return;
#endif
		std::string lock;
		{
			char *v = nullptr;
			size_t n = 0;
			if (_dupenv_s(&v, &n, "OPENBF3_QUIET_LOCK") == 0 && v != nullptr) {
				lock = v;
				std::free(v);
			}
		}
		if (!lock.empty()) {
			std::error_code ec;
			if (std::filesystem::exists(std::filesystem::path(lock), ec)) {
				std::printf("quiet lock %s exists: not measuring on a busy machine\n", lock.c_str());
				bf3::test::skip_install_case("quiet lock present");
				return;
			}
		}
		WeaponsInstall *w = weapons_install();
		if (w == nullptr) {
			return;
		}
		MetroWorld *m = metro_world(*w);
		const std::string_view variant[1] = { "weapons/m16a4/u_m16a4" };
		core::Result<std::shared_ptr<const WeaponTable>> table = install_weapon_table(w->weapons, variant, install_table_options(w->weapons, {}, true));
		REQUIRE_MESSAGE(table.ok(), (table.ok() ? std::string() : table.error().message));
		core::Result<RangePerfResult> r = run_range_perf(**table, *m->world, m->points, w->weapons.stand_eye_height, 0, 64, 600, 1);
		REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
		std::printf("W11: %u shooters x 600 ticks, %zu shots, peak %zu live projectiles, mean lane %.1f m; per tick mean %.3f ms, p50 %.3f ms, p99 %.3f ms, "
					"max %.3f ms (threshold p99 <= 2.0 ms; wall clock, noisy)\n",
				r->shooters, r->shots, r->peak_live_projectiles, double(r->mean_lane_length), r->mean_ms, r->p50_ms, r->p99_ms, r->max_ms);
		CHECK(r->shooters == 64);
		CHECK(r->tick_ms.size() == 600);
		CHECK(r->p99_ms <= 2.0);
		check_install_unchanged(*w);
	}
}
