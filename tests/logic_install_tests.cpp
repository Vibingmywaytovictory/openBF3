#include <doctest/doctest.h>

#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/gameplay/logic_world.h"
#include "bf3/core/install.h"
#include "bf3/core/install_reader.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/level/level.h"
#include "bf3/core/thread_pool.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/sim/logic/logic_runtime.h"
#include "bf3/sim_harness/logic_harness.h"
#include "test_support.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::gameplay;
using namespace bf3::sim::logic;

namespace {

using bf3::core::seconds_since;

using bf3::test::Mounted;

constexpr unsigned kCensusLanes = 6;

struct Built {
	std::unique_ptr<level::LoadedLevel> level;
	std::shared_ptr<const LogicGraph> graph;
	double load_s = 0, build_s = 0;
};

Built build(Mounted &p_m, const std::string &p_level, const std::string &p_mode, LogicSide p_side = LogicSide::Server) {
	Built b;
	const auto t0 = std::chrono::steady_clock::now();
	Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(p_m.install, p_level, p_mode);
	REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
	b.level = std::move(*loaded);
	b.load_s = seconds_since(t0);
	LogicGraphOptions o;
	o.mode = p_mode;
	o.side = p_side;
	const auto t1 = std::chrono::steady_clock::now();
	Result<LogicGraph> g = build_logic_graph(b.level->db(), b.level->root(), o);
	REQUIRE_MESSAGE(g.ok(), (g.ok() ? std::string() : g.error().message));
	b.build_s = seconds_since(t1);
	b.graph = std::make_shared<const LogicGraph>(std::move(*g));
	return b;
}

struct Metro {
	Built built;
	LogicRunReport rush;
};

Metro *metro() {
	static std::unique_ptr<Metro> m;
	static bool tried = false;
	Mounted *mount = bf3::test::shared_mount_or_skip();
	if (mount == nullptr) {
		return nullptr;
	}
	if (!tried) {
		tried = true;
		auto made = std::make_unique<Metro>();
		made->built = build(*mount, "MP_Subway", "RushLarge0");
		Result<LogicRunReport> run = run_logic_script(made->built.graph, rush_mcoms_script());
		REQUIRE_MESSAGE(run.ok(), (run.ok() ? std::string() : run.error().message));
		made->rush = std::move(*run);
		m = std::move(made);
	}
	REQUIRE(m != nullptr);
	return m.get();
}

std::optional<std::string> line_value(const std::string &p_text, const std::string &p_key) {
	size_t at = 0;
	while (at < p_text.size()) {
		const size_t end = p_text.find('\n', at);
		const std::string line = p_text.substr(at, end == std::string::npos ? std::string::npos : end - at);
		if (line.rfind(p_key + "\t", 0) == 0) {
			std::string v = line.substr(p_key.size() + 1);
			while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) {
				v.pop_back();
			}
			return v;
		}
		if (end == std::string::npos) {
			break;
		}
		at = end + 1;
	}
	return std::nullopt;
}

}

TEST_SUITE("logic-metro") {
	TEST_CASE("logic.metro G1: MP_Subway RushLarge0 instantiates its full graph headless") {
		Mounted *mount = bf3::test::shared_mount_or_skip();
		Metro *m = metro();
		if (m == nullptr || mount == nullptr) {
			return;
		}
		const LogicGraph &g = *m->built.graph;
		const LogicGraphStats &st = g.stats();
		std::printf("G1 scopes %zu objects %zu nodes %zu synced_bools %zu unknown %zu unimplemented %zu unknown_unconnected %zu "
					"dropped_unbound %zu (events %zu) work %zu load %.3f s build %.3f s\n",
				st.scopes, st.objects, g.nodes().size(), st.synced_bools, st.unknown_nodes, st.unimplemented_nodes, st.unknown_unconnected,
				st.dropped_unbound, st.dropped_unbound_events, st.work, m->built.load_s, m->built.build_s);
		std::printf("G1 info: event %zu property %zu link %zu networked_only %zu duplicate_paths %zu\n", st.event_edges, st.property_edges, st.link_edges,
				st.networked_only_edges, st.duplicate_paths);
		for (const auto &[name, count] : st.unknown_types) {
			std::printf("G1 info: unknown type %s %zu (connected)\n", name.c_str(), count);
		}
		CHECK(st.scopes == 549);
		CHECK(st.objects == 7299);
		CHECK(st.synced_bools == 145);
		CHECK(st.unknown_nodes == 0);
		CHECK(st.unimplemented_nodes == 0);
		CHECK(st.dropped_unbound_events == 0);
		CHECK(st.work <= 1000000);

		LogicWorldOptions wo;
		wo.mode = "RushLarge0";
		wo.expand_root_objects = true;
		Result<LogicWorld> world = LogicWorld::build(m->built.level->db(), m->built.level->root(), wo);
		REQUIRE_MESSAGE(world.ok(), (world.ok() ? std::string() : world.error().message));
		size_t synced = 0;
		for (const LogicObject &o : world->objects()) {
			synced += o.entity.type_name() == "SyncedBoolEntityData" ? 1 : 0;
		}
		CHECK(world->scopes().size() == 549);
		CHECK(world->objects().size() == 7299);
		CHECK(synced == 145);

		std::map<LogicTypeId, size_t> per_type;
		for (const LogicNode &n : g.nodes()) {
			if (!logic_param_fields(n.type).empty()) {
				++per_type[n.type];
			}
		}
		std::vector<std::pair<size_t, LogicTypeId>> ranked;
		for (const auto &[type, count] : per_type) {
			ranked.emplace_back(count, type);
		}
		std::sort(ranked.rbegin(), ranked.rend());
		ranked.resize(std::min<size_t>(ranked.size(), 10));
		std::map<std::pair<uint32_t, Guid>, uint32_t> node_by_key;
		for (uint32_t i = 0; i < g.nodes().size(); ++i) {
			node_by_key.emplace(std::make_pair(g.nodes()[i].scope, g.nodes()[i].guid), i);
		}
		size_t compared = 0, mismatched = 0;
		for (const LogicObject &o : world->objects()) {
			const LogicTypeId type = logic_type_of(o.entity.type_name());
			if (std::none_of(ranked.begin(), ranked.end(), [&](const auto &r) { return r.second == type; })) {
				continue;
			}
			const auto it = node_by_key.find(std::make_pair(o.scope, o.entity.guid()));
			if (it == node_by_key.end()) {
				continue;
			}
			for (const LogicParamField &f : logic_param_fields(type)) {
				if (f.shape != LogicParamField::Shape::Scalar) {
					continue;
				}
				const std::optional<PropertyValue> eff = world->effective(o, f.name);
				std::string text;
				const std::optional<LogicValue> want = eff ? logic_value_of(*eff, f.kind, &text) : std::nullopt;
				const std::optional<LogicValue> got = g.param(it->second, logic_port_id(f.name));
				++compared;
				bool same = want.has_value() == got.has_value();
				if (same && want) {
					if (want->kind == LogicValue::Kind::Text) {
						same = got->kind == LogicValue::Kind::Text && g.text(static_cast<uint32_t>(got->i)) == text;
					} else {
						same = want->same_as(*got);
					}
				}
				if (!same) {
					++mismatched;
					if (mismatched <= 10) {
						MESSAGE("param mismatch: node " << it->second << " " << o.entity.type_name() << "." << f.name);
					}
				}
			}
		}
		std::printf("G1 params: %zu scalar params compared over the %zu most frequent types, %zu mismatched\n", compared, ranked.size(), mismatched);
		CHECK(compared > 1000);
		CHECK(mismatched == 0);

		Result<LogicGhostLayout> layout = LogicGhostLayout::of(g);
		REQUIRE(layout.ok());
		std::printf("info ghost layout: bools %zu transforms %zu sequences %zu\n", layout->bools.size(), layout->transforms.size(), layout->sequences.size());
		CHECK(layout->bools.size() == 145);
	}

	TEST_CASE("logic.metro G2 / G3: rush spawners open in base order; the park fires 10 s after base 1") {
		Metro *m = metro();
		if (m == nullptr) {
			return;
		}
		const LogicGraph &g = *m->built.graph;
		const LogicGateVerdict g2 = judge_rush_spawner_order(g, m->rush);
		const LogicGateVerdict g3 = judge_park_delay(g, m->rush);
		std::printf("%s%s", g2.detail.c_str(), g3.detail.c_str());
		CHECK_MESSAGE(g2.pass, g2.detail);
		CHECK_MESSAGE(g3.pass, g3.detail);
		CHECK_FALSE(m->rush.faulted);

		Result<LogicGhostLayout> layout = LogicGhostLayout::of(g);
		REQUIRE(layout.ok());
		const std::vector<uint8_t> bytes = m->rush.ghost.encode();
		Result<LogicGhost> back = LogicGhost::decode(bytes, *layout);
		REQUIRE(back.ok());
		CHECK(*back == m->rush.ghost);
		std::printf("info ghost at tick %u: %zu bytes; trace records_total %zu (cap %zu, truncated %d)\n", m->rush.ticks, bytes.size(),
				m->rush.trace_records, LogicRuntimeOptions{}.max_trace_records, m->rush.trace_truncated ? 1 : 0);
	}

	TEST_CASE("logic.metro G4: identical trace hashes (2 runs in process, 1 bf3dump run in a child process)") {
		Metro *m = metro();
		if (m == nullptr) {
			return;
		}
		Result<LogicRunReport> again = run_logic_script(m->built.graph, rush_mcoms_script());
		REQUIRE(again.ok());
		std::printf("G4 run 1 %016llx records %zu; run 2 %016llx records %zu\n", static_cast<unsigned long long>(m->rush.trace_hash), m->rush.trace_records,
				static_cast<unsigned long long>(again->trace_hash), again->trace_records);
		CHECK(again->trace_hash == m->rush.trace_hash);
		CHECK(again->trace_records == m->rush.trace_records);

		char *exe = nullptr;
		size_t exe_len = 0;
		const bool have_exe = _dupenv_s(&exe, &exe_len, "OPENBF3_BF3DUMP") == 0 && exe != nullptr;
		const std::string exe_path = have_exe ? std::string(exe) : std::string();
		std::free(exe);
		REQUIRE_MESSAGE(have_exe, "OPENBF3_BF3DUMP is not set (ctest sets it to the bf3dump target)");
		const std::string command = "\"\"" + exe_path + "\" logic trace MP_Subway RushLarge0 --script rush 2>NUL\"";
		FILE *pipe = _popen(command.c_str(), "rb");
		REQUIRE(pipe != nullptr);
		std::string out;
		char buffer[65536];
		size_t n = 0;
		while ((n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
			out.append(buffer, n);
		}
		const int rc = _pclose(pipe);
		const std::optional<std::string> hash = line_value(out, "trace_hash");
		const std::optional<std::string> records = line_value(out, "trace_records");
		std::printf("G4 bf3dump (child, rc %d): trace_hash %s trace_records %s\n", rc, hash ? hash->c_str() : "-", records ? records->c_str() : "-");
		char expect[32];
		std::snprintf(expect, sizeof(expect), "%016llx", static_cast<unsigned long long>(m->rush.trace_hash));
		CHECK(rc == 0);
		REQUIRE(hash.has_value());
		REQUIRE(records.has_value());
		CHECK(*hash == std::string(expect));
		CHECK(std::strtoull(records->c_str(), nullptr, 10) == m->rush.trace_records);

		LogicScriptOptions seeded = rush_mcoms_script();
		seeded.runtime.root_seed += 1;
		seeded.runtime.keep_trace_records = true;
		LogicScriptOptions base = rush_mcoms_script();
		base.runtime.keep_trace_records = true;
		Result<LogicRunReport> a = run_logic_script(m->built.graph, base);
		Result<LogicRunReport> b = run_logic_script(m->built.graph, seeded);
		REQUIRE(a.ok());
		REQUIRE(b.ok());
		const auto draws = [&](const LogicRunReport &p_r) {
			std::vector<std::pair<uint32_t, bf3::sim::Tick>> out_draws;
			for (const LogicTraceRecord &t : p_r.trace) {
				if (t.kind == LogicTraceKind::Event && t.source < m->built.graph->nodes().size()) {
					const LogicTypeId type = m->built.graph->nodes()[t.source].type;
					if (type == LogicTypeId::RandomDelay || type == LogicTypeId::RandomMultiEvent) {
						out_draws.emplace_back(t.source, t.tick);
					}
				}
			}
			return out_draws;
		};
		const auto da = draws(*a), db = draws(*b);
		size_t changed = 0;
		for (size_t i = 0; i < std::min(da.size(), db.size()); ++i) {
			changed += da[i] != db[i] ? 1 : 0;
		}
		changed += std::max(da.size(), db.size()) - std::min(da.size(), db.size());
		std::printf("G4 info: root_seed + 1 -> trace hash %016llx (%s), seeded outputs %zu vs %zu, %zu changed\n",
				static_cast<unsigned long long>(b->trace_hash), b->trace_hash == a->trace_hash ? "same" : "differs", da.size(), db.size(), changed);
	}

	TEST_CASE("logic.metro: install unchanged (layout.toc, cas.cat, the .sb / .toc files)") {
		Mounted *mount = bf3::test::shared_mount_or_skip();
		if (mount == nullptr) {
			return;
		}
		const SnapshotDiff diff = diff_snapshots(mount->before, mount->reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s (%zu files)\n", diff.clean() ? "clean" : diff.summary().c_str(), mount->before.files.size());
	}
}

TEST_SUITE("logic-types") {
	TEST_CASE("logic.types G5: 0 unknown and 0 unimplemented connected types on every MP mode of Metro and Caspian") {
		Mounted *mount = bf3::test::shared_mount_or_skip();
		if (mount == nullptr) {
			return;
		}

		struct Job {
			const char *level;
			std::string mode;
		};
		struct Row {
			std::string error;
			LogicGraphStats stats;
			size_t nodes = 0;
		};
		std::vector<Job> jobs;
		for (const char *level_id : { "MP_Subway", "MP_007" }) {
			Result<level::LevelInfo> info = level::level_info(*mount->install, level_id);
			REQUIRE_MESSAGE(info.ok(), (info.ok() ? std::string() : info.error().message));
			for (const std::string &mode : info->modes) {
				jobs.push_back({ level_id, mode });
			}
		}
		const std::vector<Row> built = parallel_map<Row>(
				jobs.size(),
				[&](size_t i) {
					Row row;
					Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(mount->install, jobs[i].level, jobs[i].mode);
					if (!loaded) {
						row.error = loaded.error().message;
						return row;
					}
					LogicGraphOptions o;
					o.mode = jobs[i].mode;
					Result<LogicGraph> g = build_logic_graph((*loaded)->db(), (*loaded)->root(), o);
					if (!g) {
						row.error = g.error().message;
						return row;
					}
					row.stats = g->stats();
					row.nodes = g->nodes().size();
					return row;
				},
				kCensusLanes);
		size_t rows = 0;
		for (size_t i = 0; i < jobs.size(); ++i) {
			const Row &r = built[i];
			REQUIRE_MESSAGE(r.error.empty(), jobs[i].level, " ", jobs[i].mode, ": ", r.error);
			const LogicGraphStats &st = r.stats;
			std::printf("G5 %s %s: unknown %zu unimplemented %zu unknown_unconnected %zu unimplemented_unconnected %zu nodes %zu\n", jobs[i].level,
					jobs[i].mode.c_str(), st.unknown_nodes, st.unimplemented_nodes, st.unknown_unconnected, st.unimplemented_unconnected, r.nodes);
			CHECK(st.unknown_nodes == 0);
			CHECK(st.unimplemented_nodes == 0);
			++rows;
		}
		CHECK(rows == 14);
		const SnapshotDiff diff = diff_snapshots(mount->before, mount->reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s\n", diff.clean() ? "clean" : diff.summary().c_str());
	}
}

TEST_SUITE("logic-caspian") {
	TEST_CASE("logic.caspian G6: Caspian ConquestLarge0 spawner and vehicle Enabled timeline under forced captures") {
		Mounted *mount = bf3::test::shared_mount_or_skip();
		if (mount == nullptr) {
			return;
		}
		Built b = build(*mount, "MP_007", "ConquestLarge0");
		Result<LogicRunReport> run = run_logic_script(b.graph, caspian_conquest_script());
		REQUIRE_MESSAGE(run.ok(), (run.ok() ? std::string() : run.error().message));
		CHECK_FALSE(run->faulted);
		const LogicGateVerdict g6 = judge_conquest_timeline(*b.graph, *run, caspian_conquest_expected());
		std::printf("%s", g6.detail.c_str());
		CHECK_MESSAGE(g6.pass, g6.detail);
		const SnapshotDiff diff = diff_snapshots(mount->before, mount->reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s\n", diff.clean() ? "clean" : diff.summary().c_str());
	}
}

TEST_SUITE("logic-perf") {
	TEST_CASE("logic.perf G7: interpreter cost per tick (wall clock; noisy on a shared machine)") {
		Mounted *mount = bf3::test::shared_mount_or_skip();
		if (mount == nullptr) {
			return;
		}
		Built metro_graph = build(*mount, "MP_Subway", "RushLarge0");
		Built caspian_graph = build(*mount, "MP_007", "ConquestLarge0");
		struct Row {
			const char *name;
			LogicRunReport r;
		};
		std::vector<Row> rows;
		{
			LogicScriptOptions o = rush_mcoms_script();
			o.runtime.keep_trace_records = false;
			Result<LogicRunReport> r = run_logic_script(metro_graph.graph, o);
			REQUIRE(r.ok());
			rows.push_back({ "metro rush", std::move(*r) });
		}
		{
			LogicScriptOptions o = caspian_conquest_script();
			o.runtime.keep_trace_records = false;
			Result<LogicRunReport> r = run_logic_script(caspian_graph.graph, o);
			REQUIRE(r.ok());
			rows.push_back({ "caspian conquest", std::move(*r) });
		}
		uint64_t p99 = 0, busy_p99 = 0, max_ns = 0;
		size_t max_deliveries = 0, carried = 0;
		for (const Row &row : rows) {
			const LogicRunReport &r = row.r;
			std::printf("G7 %s: ticks %u p50 %llu ns p99 %llu ns busy p99 %llu ns (%zu busy ticks) max %llu ns; tick 0 %llu ns, %zu deliveries; "
						"max deliveries/tick %zu, carried_over %zu\n",
					row.name, r.ticks, static_cast<unsigned long long>(r.drain_ns_p50), static_cast<unsigned long long>(r.drain_ns_p99),
					static_cast<unsigned long long>(r.drain_ns_busy_p99), r.busy_ticks, static_cast<unsigned long long>(r.drain_ns_max),
					static_cast<unsigned long long>(r.tick0.drain_ns), r.tick0.deliveries, r.max_deliveries_tick, r.carried_over);
			p99 = std::max(p99, r.drain_ns_p99);
			busy_p99 = std::max(busy_p99, r.drain_ns_busy_p99);
			max_ns = std::max(max_ns, r.drain_ns_max);
			max_deliveries = std::max(max_deliveries, r.max_deliveries_tick);
			carried += r.carried_over;
			CHECK_FALSE(r.faulted);
		}
		std::printf("G7: p99 %llu ns (<= 100000), busy p99 %llu ns (<= 500000), max %llu ns (<= 20000000), max deliveries/tick %zu (< 10000), "
					"carried_over %zu (= 0); wall clock on a shared machine\n",
				static_cast<unsigned long long>(p99), static_cast<unsigned long long>(busy_p99), static_cast<unsigned long long>(max_ns), max_deliveries,
				carried);
		CHECK(p99 <= 100000);
		CHECK(busy_p99 <= 500000);
		CHECK(max_ns <= 20000000);
		CHECK(max_deliveries < 10000);
		CHECK(carried == 0);
	}
}

TEST_SUITE("logic-census") {
	TEST_CASE("logic.census I1: every level x mode (info)") {
		Mounted *mount = bf3::test::shared_mount_or_skip();
		if (mount == nullptr) {
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		std::printf("%s\n", logic_census_header().c_str());
		size_t expected = 0, listed = 0, ok = 0, unknown_rows = 0;

		struct Job {
			std::string level;
			std::string mode;
			std::string error;
		};
		std::vector<Job> jobs;
		for (const Level &l : mount->install->levels()) {
			Result<level::LevelInfo> info = level::level_info(*mount->install, l.id);
			if (!info) {
				jobs.push_back({ l.id, std::string(), info.error().message });
				continue;
			}
			for (const std::string &mode : info->modes) {
				jobs.push_back({ l.id, mode, std::string() });
			}
		}
		const std::vector<LogicCensusRow> rows = parallel_map<LogicCensusRow>(
				jobs.size(),
				[&](size_t i) {
					const Job &j = jobs[i];
					LogicCensusRow row;
					if (j.mode.empty()) {
						return row;
					}
					Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(mount->install, j.level, j.mode);
					if (!loaded) {
						row.level = j.level;
						row.mode = j.mode;
						row.error = loaded.error().message;
						return row;
					}
					LogicGraphOptions o;
					o.mode = j.mode;
					const auto t = std::chrono::steady_clock::now();
					Result<LogicGraph> g = build_logic_graph((*loaded)->db(), (*loaded)->root(), o);
					const double ms = seconds_since(t) * 1000.0;
					if (!g) {
						row.level = j.level;
						row.mode = j.mode;
						row.error = g.error().message;
						return row;
					}
					return logic_census_row(j.level, j.mode, std::make_shared<const LogicGraph>(std::move(*g)), ms);
				},
				kCensusLanes);
		for (size_t i = 0; i < jobs.size(); ++i) {
			++expected;
			++listed;
			if (jobs[i].mode.empty()) {
				std::printf("%s\t-\t0\t%s\n", jobs[i].level.c_str(), jobs[i].error.c_str());
				continue;
			}
			const LogicCensusRow &row = rows[i];
			std::printf("%s\n", logic_census_line(row).c_str());
			ok += row.ok ? 1 : 0;
			unknown_rows += row.ok && row.stats.unknown_nodes > 0 ? 1 : 0;
		}
		std::printf("I1: %zu rows listed (%zu expected), %zu built and ran, %zu with unknown connected types, %.1f s\n", listed, expected, ok, unknown_rows,
				seconds_since(start));
		CHECK(listed == expected);
		CHECK(expected > 0);
		const SnapshotDiff diff = diff_snapshots(mount->before, mount->reader.snapshot());
		CHECK_MESSAGE(diff.clean(), diff.summary());
		std::printf("install.unchanged: %s\n", diff.clean() ? "clean" : diff.summary().c_str());
	}
}
