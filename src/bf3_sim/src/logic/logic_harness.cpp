// S4 level logic: scripted runs and the gate judges (docs/design/s4.md §9). See logic_harness.h. Selections are made
// from the collapsed graph's structure; GUIDs appear only as test data (the conquest flags and expectation rows) and
// as printed cross-checks.
#include "bf3/sim/logic/logic_harness.h"

#include "bf3/core/gameplay/logic_types.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>
#include <string>

namespace bf3::sim::logic {

namespace gp = core::gameplay;
using gp::LogicEdge;
using gp::LogicNodeRef;
using gp::LogicTypeId;
using gp::kNoLogicIndex;
using T = LogicTypeId;
using core::Guid;

namespace {

Guid guid_of(const char *p_text) {
	return core::Guid::parse(p_text).value_or(core::Guid{});
}

std::string guid8(const core::Guid &p_guid) {
	std::string s = p_guid.to_string();
	if (!s.empty() && s[0] == '{') {
		s = s.substr(1);
	}
	return s.substr(0, 8);
}

std::string node_label(const LogicGraph &p_graph, uint32_t p_node) {
	if (p_node >= p_graph.nodes().size()) {
		return "-";
	}
	return std::to_string(p_node) + ":" + guid8(p_graph.nodes()[p_node].guid);
}

int64_t param_int(const LogicGraph &p_graph, uint32_t p_node, std::string_view p_field, int64_t p_default) {
	const std::optional<LogicValue> v = p_graph.param(p_node, gp::logic_port_id(p_field));
	if (!v) {
		return p_default;
	}
	switch (v->kind) {
		case LogicValue::Kind::Bool:
			return v->b ? 1 : 0;
		case LogicValue::Kind::Float:
			return static_cast<int64_t>(v->f);
		default:
			return v->i;
	}
}

double param_float(const LogicGraph &p_graph, uint32_t p_node, std::string_view p_field, double p_default) {
	const std::optional<LogicValue> v = p_graph.param(p_node, gp::logic_port_id(p_field));
	if (!v) {
		return p_default;
	}
	return v->kind == LogicValue::Kind::Float ? v->f : static_cast<double>(v->i);
}

// Incoming edges per node (scripts and judges only; never on the tick path).
struct Incoming {
	std::vector<std::vector<uint32_t>> events, properties;
	explicit Incoming(const LogicGraph &p_graph) :
			events(p_graph.nodes().size()), properties(p_graph.nodes().size()) {
		for (uint32_t i = 0; i < p_graph.event_edges().size(); ++i) {
			events[p_graph.event_edges()[i].target].push_back(i);
		}
		for (uint32_t i = 0; i < p_graph.property_edges().size(); ++i) {
			properties[p_graph.property_edges()[i].target].push_back(i);
		}
	}
};

struct Mcom {
	uint32_t model = 0; // StaticModel
	uint32_t activation = 0; // the first DelayTrigger on the path (its Enable input is the activation)
	uint32_t base = 0; // MultipleTrigger, TriggerAmount 2
};

struct RushSelection {
	std::vector<Mcom> mcoms; // in StaticModel node order
	std::vector<uint32_t> bases; // distinct, in node order
	std::vector<uint32_t> spawners; // CharacterSpawn nodes fed by a SyncedBool, in node order
	std::vector<std::pair<uint32_t, uint32_t>> park; // (base, park SyncedBool) candidates, in base then node order
};

// MCOMs (s4.md §9.1): StaticModel OnDestroyed -> ... -> MultipleTrigger(TriggerAmount 2) within kMcomSearchHops event
// hops, breadth first in edge order; the first DelayTrigger on the found path is the activation.
std::optional<Mcom> find_mcom(const LogicGraph &p_graph, uint32_t p_model) {
	struct Visit {
		uint32_t node;
		uint32_t parent; // index into visits, or none
		uint32_t depth;
	};
	std::vector<Visit> visits;
	std::set<uint32_t> seen;
	for (const LogicEdge &e : p_graph.events_from(p_model)) {
		if (e.source_port == gp::logic_port::kOnDestroyed && seen.insert(e.target).second) {
			visits.push_back({ e.target, kNoLogicIndex, 1 });
		}
	}
	for (size_t at = 0; at < visits.size(); ++at) {
		const Visit v = visits[at];
		const gp::LogicNode &node = p_graph.nodes()[v.node];
		if (node.type == T::MultipleTrigger && param_int(p_graph, v.node, "TriggerAmount", 0) == 2) {
			// Walk back for the first DelayTrigger after the model.
			std::vector<uint32_t> path;
			for (uint32_t k = static_cast<uint32_t>(at); k != kNoLogicIndex; k = visits[k].parent) {
				path.push_back(visits[k].node);
			}
			std::reverse(path.begin(), path.end());
			for (const uint32_t n : path) {
				if (p_graph.nodes()[n].type == T::DelayTrigger) {
					return Mcom{ p_model, n, v.node };
				}
			}
			return std::nullopt;
		}
		if (v.depth >= kMcomSearchHops) {
			continue;
		}
		for (const LogicEdge &e : p_graph.events_from(v.node)) {
			if (seen.insert(e.target).second) {
				visits.push_back({ e.target, static_cast<uint32_t>(at), v.depth + 1 });
			}
		}
	}
	return std::nullopt;
}

// Park candidates of one base: SyncedBools reached from the base's OnTrigger within kParkSearchHops event hops by a
// SetTrue, through exactly one Delay, whose Delay is 10 s (s4.md §9.1). Depth first over paths, distinct targets.
void find_park(const LogicGraph &p_graph, uint32_t p_base, std::vector<uint32_t> &r_out) {
	struct Frame {
		uint32_t node;
		int32_t port; // the port the walk leaves through (OnTrigger at the base; any later)
		uint32_t depth;
		uint32_t delays;
		bool ten_seconds;
	};
	std::vector<Frame> stack{ { p_base, gp::logic_port::kOnTrigger, 0, 0, true } };
	size_t steps = 0;
	while (!stack.empty() && steps < 100000) {
		++steps;
		const Frame f = stack.back();
		stack.pop_back();
		if (f.depth >= kParkSearchHops) {
			continue;
		}
		for (const LogicEdge &e : p_graph.events_from(f.node)) {
			if (f.depth == 0 && e.source_port != f.port) {
				continue;
			}
			const gp::LogicNode &t = p_graph.nodes()[e.target];
			if (t.type == T::SyncedBool && e.target_port == gp::logic_port::kSetTrue && f.delays == 1 && f.ten_seconds) {
				if (std::find(r_out.begin(), r_out.end(), e.target) == r_out.end()) {
					r_out.push_back(e.target);
				}
				continue;
			}
			Frame next{ e.target, 0, f.depth + 1, f.delays, f.ten_seconds };
			if (t.type == T::Delay) {
				++next.delays;
				next.ten_seconds = next.ten_seconds && logic_delay_ticks(param_float(p_graph, e.target, "Delay", 0.0)) == logic_delay_ticks(10.0);
			}
			if (next.delays <= 1) {
				stack.push_back(next);
			}
		}
	}
}

RushSelection select_rush(const LogicGraph &p_graph) {
	RushSelection out;
	for (const uint32_t m : p_graph.nodes_of_type(T::StaticModel)) {
		if (const std::optional<Mcom> mcom = find_mcom(p_graph, m)) {
			out.mcoms.push_back(*mcom);
			if (std::find(out.bases.begin(), out.bases.end(), mcom->base) == out.bases.end()) {
				out.bases.push_back(mcom->base);
			}
		}
	}
	std::sort(out.bases.begin(), out.bases.end());
	const Incoming in(p_graph);
	for (const uint32_t s : p_graph.nodes_of_type(T::CharacterSpawn)) {
		for (const uint32_t ei : in.properties[s]) {
			const LogicEdge &e = p_graph.property_edges()[ei];
			if (e.target_port == gp::logic_port::kEnabled && p_graph.nodes()[e.source].type == T::SyncedBool) {
				out.spawners.push_back(s);
				break;
			}
		}
	}
	for (const uint32_t b : out.bases) {
		std::vector<uint32_t> park;
		find_park(p_graph, b, park);
		std::sort(park.begin(), park.end());
		for (const uint32_t p : park) {
			out.park.emplace_back(b, p);
		}
	}
	return out;
}

// The state of a native node's Enabled after the timeline entries up to and including tick p_tick.
bool enabled_at(const std::vector<NativeTimelineEntry> &p_timeline, uint32_t p_node, bool p_initial, Tick p_tick) {
	bool on = p_initial;
	for (const NativeTimelineEntry &e : p_timeline) {
		if (e.node == p_node && e.what == NativeTimelineEntry::What::Enabled && e.tick <= p_tick) {
			on = e.value != 0;
		}
	}
	return on;
}

bool initial_enabled(const LogicGraph &p_graph, uint32_t p_node) {
	return param_int(p_graph, p_node, "Enabled", 1) != 0;
}

uint64_t percentile(std::vector<uint64_t> p_values, double p_q) {
	if (p_values.empty()) {
		return 0;
	}
	std::sort(p_values.begin(), p_values.end());
	// Nearest rank: the smallest value with at least p_q of the values at or below it.
	size_t at = static_cast<size_t>(std::ceil(p_q * static_cast<double>(p_values.size())));
	at = at > 0 ? at - 1 : 0;
	return p_values[std::min(at, p_values.size() - 1)];
}

void line(std::string &r_out, const char *p_format, ...) {
	char buffer[512];
	va_list args;
	va_start(args, p_format);
	std::vsnprintf(buffer, sizeof(buffer), p_format, args);
	va_end(args);
	r_out += buffer;
	r_out += '\n';
}

} // namespace

LogicScriptOptions rush_mcoms_script() {
	LogicScriptOptions o;
	o.kind = LogicScriptKind::RushMcoms;
	o.ticks = 36000;
	o.rush_destroy_after = 3600;
	return o;
}

LogicScriptOptions caspian_conquest_script() {
	LogicScriptOptions o;
	o.kind = LogicScriptKind::ConquestCaptures;
	o.ticks = 3000;
	const Guid f3042a9e = guid_of("f3042a9e-de9a-463b-a94b-292223b3f744");
	const Guid e4c5d622 = guid_of("e4c5d622-e2aa-4f48-9e79-fafa27236b2f");
	o.conquest_order = {
		{ 600, LogicNodeRef{ f3042a9e, Guid{} }, 1 },
		{ 1200, LogicNodeRef{ e4c5d622, Guid{} }, 2 },
		{ 1800, LogicNodeRef{ f3042a9e, Guid{} }, 2 },
		{ 2400, LogicNodeRef{ e4c5d622, Guid{} }, 1 },
	};
	return o;
}

core::Result<LogicRunReport> run_logic_script(std::shared_ptr<const LogicGraph> p_graph, const LogicScriptOptions &p_options) {
	if (!p_graph) {
		return core::make_error("logic script: no graph");
	}
	const LogicGraph &g = *p_graph;
	core::Result<LogicRuntime> created = LogicRuntime::create(p_graph, p_options.runtime);
	if (!created) {
		return created.error();
	}
	LogicRuntime rt = std::move(*created);
	core::Result<LogicNativeStubs> stubs_made = LogicNativeStubs::create(p_graph);
	if (!stubs_made) {
		return stubs_made.error();
	}
	LogicNativeStubs stubs = std::move(*stubs_made);
	LogicRunReport report;

	RushSelection rush;
	std::vector<uint32_t> humans;
	struct Capture {
		Tick tick;
		uint32_t node;
		uint8_t team;
	};
	std::vector<Capture> captures;
	if (p_options.kind == LogicScriptKind::RushMcoms) {
		rush = select_rush(g);
		for (const Mcom &m : rush.mcoms) {
			BF3_TRY(rt.watch(m.activation, gp::logic_port::kEnable, LogicWatchKind::EventIn));
		}
		for (const uint32_t b : rush.bases) {
			BF3_TRY(rt.watch(b, gp::logic_port::kOnTrigger, LogicWatchKind::EventOut));
		}
		for (const auto &[base, park] : rush.park) {
			BF3_TRY(rt.watch(park, gp::logic_port::kSetTrue, LogicWatchKind::EventIn));
		}
		for (const uint32_t h : g.nodes_of_type(T::HumanPlayer)) {
			humans.push_back(h);
		}
	} else if (p_options.kind == LogicScriptKind::ConquestCaptures) {
		for (const LogicScriptOptions::Capture &c : p_options.conquest_order) {
			const std::vector<uint32_t> found = g.find_nodes(c.flag, T::CapturePoint);
			if (found.size() != 1) {
				return core::make_error("logic script: capture flag " + guid8(c.flag.scope_reference) + "/" + guid8(c.flag.entity) + " resolves to " +
						std::to_string(found.size()) + " CapturePoint nodes (expected 1)");
			}
			captures.push_back({ c.tick, found[0], c.team });
		}
	}

	std::vector<uint64_t> drains;
	drains.reserve(p_options.ticks);
	std::vector<uint64_t> busy;
	const auto absorb = [&](const LogicTickStats &p_stats) {
		report.deliveries += p_stats.deliveries;
		report.max_deliveries_tick = std::max(report.max_deliveries_tick, p_stats.deliveries);
		report.carried_over += p_stats.carried_over;
		report.effects += p_stats.effects;
		report.networked += p_stats.networked;
		report.drain_ns_max = std::max(report.drain_ns_max, p_stats.drain_ns);
		for (const LogicWatchHit &h : rt.watched()) {
			report.watch_hits.push_back(h);
		}
	};

	core::Result<LogicTickStats> tick0 = rt.start();
	if (!tick0) {
		report.faulted = true;
		report.fault = tick0.error().message;
	} else {
		report.tick0 = *tick0;
		absorb(*tick0);
	}
	std::vector<LogicEffect> effects(rt.effects().begin(), rt.effects().end());
	// Destroy schedule: MCOM index -> tick (once per MCOM, from its first watched activation).
	std::vector<Tick> destroy_at(rush.mcoms.size(), 0);
	std::vector<bool> scheduled(rush.mcoms.size(), false);
	const auto schedule_from_hits = [&](std::span<const LogicWatchHit> p_hits) {
		for (const LogicWatchHit &h : p_hits) {
			if (h.kind != LogicWatchKind::EventIn || h.port != gp::logic_port::kEnable) {
				continue;
			}
			for (size_t k = 0; k < rush.mcoms.size(); ++k) {
				if (rush.mcoms[k].activation == h.node && !scheduled[k]) {
					scheduled[k] = true;
					destroy_at[k] = h.tick + p_options.rush_destroy_after;
				}
			}
		}
	};
	if (!report.faulted) {
		schedule_from_hits(rt.watched());
	}
	Tick ran = 0;
	for (Tick t = 1; t <= p_options.ticks && !report.faulted; ++t) {
		BF3_TRY(stubs.apply(t, effects, rt));
		if (p_options.kind == LogicScriptKind::RushMcoms) {
			if (t == 1) {
				for (const uint32_t h : humans) {
					for (int player = 0; player < 2; ++player) {
						LogicContext ctx;
						ctx.player = player;
						ctx.team = static_cast<uint8_t>(player + 1);
						BF3_TRY(stubs.player_enter(h, ctx, rt));
						report.actions.push_back({ t, h, LogicRunReport::Action::What::PlayerEnter, ctx.team });
					}
				}
			}
			for (size_t k = 0; k < rush.mcoms.size(); ++k) {
				if (scheduled[k] && destroy_at[k] == t) {
					BF3_TRY(stubs.destroy(rush.mcoms[k].model, rt));
					report.actions.push_back({ t, rush.mcoms[k].model, LogicRunReport::Action::What::Destroy, 0 });
				}
			}
		} else if (p_options.kind == LogicScriptKind::ConquestCaptures) {
			for (const Capture &c : captures) {
				if (c.tick == t) {
					BF3_TRY(stubs.capture(c.node, c.team, rt));
					report.actions.push_back({ t, c.node, LogicRunReport::Action::What::Capture, c.team });
				}
			}
		}
		if (p_options.kind != LogicScriptKind::Idle && p_options.round_start_tick == t) {
			for (const uint32_t pre : g.nodes_of_type(T::PreRound)) {
				BF3_TRY(rt.force_event(pre, gp::logic_port_id("StartRound"), LogicContext{}));
				report.actions.push_back({ t, pre, LogicRunReport::Action::What::RoundStart, 0 });
			}
		}
		for (const LogicScriptOptions::Force &f : p_options.forces) {
			if (f.tick == t) {
				BF3_TRY(rt.force_event(f.node, f.port, LogicContext{}));
			}
		}
		core::Result<LogicTickStats> stats = rt.step();
		if (!stats) {
			report.faulted = true;
			report.fault = stats.error().message;
			break;
		}
		ran = t;
		absorb(*stats);
		drains.push_back(stats->drain_ns);
		if (stats->deliveries > 0) {
			busy.push_back(stats->drain_ns);
		}
		if (p_options.kind == LogicScriptKind::RushMcoms) {
			schedule_from_hits(rt.watched());
		}
		effects.assign(rt.effects().begin(), rt.effects().end());
	}
	report.ticks = ran;
	report.trace_hash = rt.trace().hash;
	report.trace_records = rt.trace().records_total;
	report.trace_truncated = rt.trace().truncated;
	report.drain_ns_p50 = percentile(drains, 0.50);
	report.drain_ns_p99 = percentile(drains, 0.99);
	report.drain_ns_busy_p99 = percentile(busy, 0.99);
	report.busy_ticks = busy.size();
	report.timeline.assign(stubs.timeline().begin(), stubs.timeline().end());
	if (p_options.runtime.keep_trace_records) {
		report.trace = rt.trace().records;
	}
	for (const LogicUnhandledPort &u : rt.unhandled()) {
		report.unhandled.push_back(u);
	}
	return report;
}

LogicGateVerdict judge_rush_spawner_order(const LogicGraph &p_graph, const LogicRunReport &p_report) {
	LogicGateVerdict v;
	std::string &d = v.detail;
	const RushSelection rush = select_rush(p_graph);
	bool ok = !p_report.faulted;
	if (p_report.faulted) {
		line(d, "run faulted: %s", p_report.fault.c_str());
	}
	line(d, "mcoms %zu, bases %zu, rush spawners %zu (expected 8)", rush.mcoms.size(), rush.bases.size(), rush.spawners.size());
	ok = ok && rush.spawners.size() == 8;
	// Base falls in the order they fire (first OnTrigger of each base).
	std::vector<std::pair<Tick, uint32_t>> falls;
	for (const LogicWatchHit &h : p_report.watch_hits) {
		if (h.kind == LogicWatchKind::EventOut && h.port == gp::logic_port::kOnTrigger &&
				std::find(rush.bases.begin(), rush.bases.end(), h.node) != rush.bases.end() &&
				std::none_of(falls.begin(), falls.end(), [&](const auto &f) { return f.second == h.node; })) {
			falls.emplace_back(h.tick, h.node);
		}
	}
	for (size_t b = 0; b < falls.size(); ++b) {
		size_t activated = 0, destroyed = 0;
		for (size_t k = 0; k < rush.mcoms.size(); ++k) {
			if (rush.mcoms[k].base != falls[b].second) {
				continue;
			}
			activated += std::any_of(p_report.watch_hits.begin(), p_report.watch_hits.end(), [&](const LogicWatchHit &h) {
				return h.kind == LogicWatchKind::EventIn && h.node == rush.mcoms[k].activation && h.port == gp::logic_port::kEnable;
			});
			destroyed += std::any_of(p_report.actions.begin(), p_report.actions.end(), [&](const LogicRunReport::Action &a) {
				return a.what == LogicRunReport::Action::What::Destroy && a.node == rush.mcoms[k].model;
			});
		}
		line(d, "base %zu falls at tick %u (MultipleTrigger %s): mcoms activated %zu, destroyed %zu (expected 2 and 2)", b + 1, falls[b].first,
				node_label(p_graph, falls[b].second).c_str(), activated, destroyed);
	}
	line(d, "bases fallen %zu (expected 4)", falls.size());
	ok = ok && falls.size() >= 4;
	struct Opened {
		uint32_t node;
		Tick first;
		int64_t team;
	};
	std::vector<Opened> opened;
	for (const uint32_t s : rush.spawners) {
		const bool initial = initial_enabled(p_graph, s);
		if (initial) {
			line(d, "spawner %s starts enabled (expected disabled)", node_label(p_graph, s).c_str());
			ok = false;
		}
		Tick first = 0;
		for (const NativeTimelineEntry &e : p_report.timeline) {
			if (e.node == s && e.what == NativeTimelineEntry::What::Enabled && e.value != 0) {
				first = e.tick;
				break;
			}
		}
		const int64_t team = param_int(p_graph, s, "Team", 0);
		if (first == 0) {
			line(d, "spawner %s (team %lld) never opens", node_label(p_graph, s).c_str(), static_cast<long long>(team));
			ok = false;
			continue;
		}
		opened.push_back({ s, first, team });
	}
	std::stable_sort(opened.begin(), opened.end(), [](const Opened &a, const Opened &b) { return a.first < b.first; });
	if (opened.size() == 8 && falls.size() >= 4) {
		for (size_t g = 0; g < 4; ++g) {
			const Opened &a = opened[2 * g];
			const Opened &b = opened[2 * g + 1];
			const Tick after = g == 0 ? 0 : falls[g - 1].first;
			const Tick by = falls[g].first;
			const Tick limit = by + logic_delay_ticks(30.0) + 2;
			const bool teams = (a.team == 1 && b.team == 2) || (a.team == 2 && b.team == 1);
			const bool window = a.first > after && b.first > after && a.first <= by && b.first <= by;
			bool closed = true;
			for (const Opened *o : { &a, &b }) {
				for (const NativeTimelineEntry &e : p_report.timeline) {
					if (e.node == o->node && e.what == NativeTimelineEntry::What::Enabled && e.value != 0 && e.tick > limit) {
						closed = false;
					}
				}
				closed = closed && !enabled_at(p_report.timeline, o->node, false, limit + 1);
				if (p_report.ticks < limit + 1) {
					closed = false; // the run is too short to show the close
				}
			}
			line(d, "group %zu: %s team %lld opens %u, %s team %lld opens %u; window (%u, %u]; closed by %u: %s%s%s", g + 1,
					node_label(p_graph, a.node).c_str(), static_cast<long long>(a.team), a.first, node_label(p_graph, b.node).c_str(),
					static_cast<long long>(b.team), b.first, after, by, limit, teams ? "" : "TEAMS ", window ? "" : "WINDOW ", closed ? "ok" : "NOT CLOSED");
			ok = ok && teams && window && closed;
		}
	} else {
		ok = false;
	}
	v.pass = ok;
	line(d, "G2 %s", ok ? "PASS" : "FAIL");
	return v;
}

LogicGateVerdict judge_park_delay(const LogicGraph &p_graph, const LogicRunReport &p_report) {
	LogicGateVerdict v;
	std::string &d = v.detail;
	const RushSelection rush = select_rush(p_graph);
	std::optional<std::pair<Tick, uint32_t>> first_fall;
	for (const LogicWatchHit &h : p_report.watch_hits) {
		if (h.kind == LogicWatchKind::EventOut && h.port == gp::logic_port::kOnTrigger &&
				std::find(rush.bases.begin(), rush.bases.end(), h.node) != rush.bases.end()) {
			first_fall = std::make_pair(h.tick, h.node);
			break;
		}
	}
	if (!first_fall) {
		line(d, "no base fell");
		line(d, "G3 FAIL");
		return v;
	}
	std::vector<uint32_t> park;
	for (const auto &[base, p] : rush.park) {
		if (base == first_fall->second) {
			park.push_back(p);
		}
	}
	line(d, "base 1 (%s) falls at tick %u; park SyncedBools selected: %zu (expected 1)", node_label(p_graph, first_fall->second).c_str(), first_fall->first,
			park.size());
	if (park.size() != 1) {
		line(d, "G3 FAIL");
		return v;
	}
	std::optional<Tick> set_true;
	for (const LogicWatchHit &h : p_report.watch_hits) {
		if (h.kind == LogicWatchKind::EventIn && h.node == park[0] && h.port == gp::logic_port::kSetTrue) {
			set_true = h.tick;
			break;
		}
	}
	const Tick expect = first_fall->first + logic_delay_ticks(10.0);
	line(d, "park SyncedBool %s (cross-check bcc06e18) SetTrue at tick %s, expected %u (fall + %u)", node_label(p_graph, park[0]).c_str(),
			set_true ? std::to_string(*set_true).c_str() : "never", expect, logic_delay_ticks(10.0));
	v.pass = !p_report.faulted && set_true && *set_true == expect;
	line(d, "G3 %s", v.pass ? "PASS" : "FAIL");
	return v;
}

ConquestSelection select_conquest_spawners(const LogicGraph &p_graph) {
	ConquestSelection out;
	const Incoming in(p_graph);
	std::set<uint32_t> flags;
	// The gate of a Bool / SyncedBool: SetTrue or SetFalse from a TeamFilter whose In is fed by a CapturePoint's
	// OnCaptured; returns the CapturePoints.
	const auto gate_flags = [&](uint32_t p_node, std::set<uint32_t> &r_flags) {
		bool gated = false;
		for (const uint32_t ei : in.events[p_node]) {
			const LogicEdge &e = p_graph.event_edges()[ei];
			if ((e.target_port != gp::logic_port::kSetTrue && e.target_port != gp::logic_port::kSetFalse) || p_graph.nodes()[e.source].type != T::TeamFilter) {
				continue;
			}
			for (const uint32_t fi : in.events[e.source]) {
				const LogicEdge &f = p_graph.event_edges()[fi];
				if (f.target_port == gp::logic_port::kIn && f.source_port == gp::logic_port::kOnCaptured && p_graph.nodes()[f.source].type == T::CapturePoint) {
					r_flags.insert(f.source);
					gated = true;
				}
			}
		}
		return gated;
	};
	const auto is_logic = [](T p_type) { return p_type == T::Bool || p_type == T::SyncedBool || p_type == T::Not || p_type == T::And || p_type == T::Or; };
	for (const uint32_t v : p_graph.nodes_of_type(T::VehicleSpawn)) {
		std::vector<uint32_t> frontier;
		for (const uint32_t ei : in.properties[v]) {
			const LogicEdge &e = p_graph.property_edges()[ei];
			if (e.target_port == gp::logic_port::kEnabled) {
				frontier.push_back(e.source);
			}
		}
		if (frontier.empty()) {
			continue;
		}
		std::set<uint32_t> found;
		bool gated = false;
		std::vector<uint32_t> level = frontier;
		std::set<uint32_t> seen(level.begin(), level.end());
		for (uint32_t hop = 1; hop <= kConquestGateHops && !level.empty(); ++hop) {
			std::vector<uint32_t> next;
			for (const uint32_t n : level) {
				const T type = p_graph.nodes()[n].type;
				if (!is_logic(type)) {
					continue;
				}
				if ((type == T::Bool || type == T::SyncedBool) && gate_flags(n, found)) {
					gated = true;
				}
				for (const uint32_t pi : in.properties[n]) {
					const uint32_t s = p_graph.property_edges()[pi].source;
					if (seen.insert(s).second) {
						next.push_back(s);
					}
				}
			}
			level.swap(next);
		}
		if (gated) {
			out.gated.push_back(v);
			flags.insert(found.begin(), found.end());
			continue;
		}
		if (frontier.size() == 1 && p_graph.nodes()[frontier[0]].type == T::SyncedBool && in.events[frontier[0]].empty() &&
				in.properties[frontier[0]].empty()) {
			out.constant.push_back(v);
		}
	}
	out.flags.assign(flags.begin(), flags.end());
	return out;
}

std::span<const ConquestExpectation> caspian_conquest_expected() {
	static const ConquestExpectation kRows[] = {
		{ { Guid{}, guid_of("1ceb4ab1-7db7-4d5f-ab29-fef752103e74") }, true, "M1A2 1ceb4ab1 (team 1)", { false, true, true, false, false } },
		{ { Guid{}, guid_of("1fc335ff-3275-47d4-84a7-5e892639ab0a") }, true, "T-90 1fc335ff (team 2)", { false, false, true, true, false } },
		{ { Guid{}, guid_of("c8bd75c7-91e7-4431-a092-f4e4a1167355") }, true, "Growler c8bd75c7 (team 1)", { false, true, true, false, false } },
		{ { Guid{}, guid_of("e3e937bb-ad32-4ee4-87d4-1a416a6f8b59") }, true, "VDV e3e937bb (team 2)", { false, false, true, true, false } },
		{ { Guid{}, guid_of("4b82be80-91d1-4971-ae95-290fa68fcfe1") }, true, "complement 4b82be80 (team 1)", { true, false, false, true, true } },
		{ { Guid{}, guid_of("a747b2ee-dba6-490d-ac49-a41b5fdf0377") }, true, "complement a747b2ee (team 2)", { true, true, false, false, true } },
		{ { guid_of("5b7bb147-ac4b-4408-ba71-821fe8a427b2"), Guid{} }, false, "F-18 5b7bb147 (team 1)", { true, true, true, true, true } },
		{ { guid_of("2abca96b-e41a-435b-b62b-a978e42c720e"), Guid{} }, false, "F-18 2abca96b (team 1)", { true, true, true, true, true } },
		{ { guid_of("d94f16e5-6780-4aae-bb03-9550681bc91a"), Guid{} }, false, "Su-35 d94f16e5 (team 2)", { true, true, true, true, true } },
		{ { guid_of("42feab7b-2f0e-4264-8c5a-3de15d66546a"), Guid{} }, false, "Su-35 42feab7b (team 2)", { true, true, true, true, true } },
	};
	return kRows;
}

LogicGateVerdict judge_conquest_timeline(const LogicGraph &p_graph, const LogicRunReport &p_report, std::span<const ConquestExpectation> p_expected) {
	LogicGateVerdict v;
	std::string &d = v.detail;
	bool ok = !p_report.faulted;
	if (p_report.faulted) {
		line(d, "run faulted: %s", p_report.fault.c_str());
	}
	const ConquestSelection sel = select_conquest_spawners(p_graph);
	std::vector<uint32_t> want_gated, want_constant;
	std::vector<uint32_t> nodes;
	for (const ConquestExpectation &row : p_expected) {
		const std::vector<uint32_t> found = p_graph.find_nodes(row.spawner, T::VehicleSpawn);
		if (found.size() != 1) {
			line(d, "%s: resolves to %zu VehicleSpawn nodes (expected 1)", row.label, found.size());
			ok = false;
			nodes.push_back(kNoLogicIndex);
			continue;
		}
		nodes.push_back(found[0]);
		(row.gated ? want_gated : want_constant).push_back(found[0]);
	}
	std::sort(want_gated.begin(), want_gated.end());
	std::sort(want_constant.begin(), want_constant.end());
	std::string flags;
	for (const uint32_t f : sel.flags) {
		const uint32_t scope = p_graph.nodes()[f].scope;
		const uint32_t parent = p_graph.scopes()[scope].parent;
		flags += " " + node_label(p_graph, f) + "<" + guid8(parent != kNoLogicIndex ? p_graph.scopes()[parent].reference : core::Guid{});
	}
	line(d, "selection: gated %zu (expected %zu), constant %zu (expected %zu), flags%s", sel.gated.size(), want_gated.size(), sel.constant.size(),
			want_constant.size(), flags.c_str());
	if (sel.gated != want_gated || sel.constant != want_constant) {
		line(d, "selection differs from the expectation rows");
		ok = false;
	}
	for (size_t r = 0; r < p_expected.size(); ++r) {
		if (nodes[r] == kNoLogicIndex) {
			continue;
		}
		const uint32_t n = nodes[r];
		const bool initial = initial_enabled(p_graph, n);
		std::string got;
		bool row_ok = true;
		for (size_t k = 0; k < 5; ++k) {
			const bool on = enabled_at(p_report.timeline, n, initial, kConquestCheckTicks[k]);
			got += on ? "1" : "0";
			row_ok = row_ok && on == p_expected[r].enabled[k];
		}
		// No Enabled change in ticks 2..3,000 other than at the check ticks where the expectation changes.
		size_t stray = 0;
		for (const NativeTimelineEntry &e : p_report.timeline) {
			if (e.node != n || e.what != NativeTimelineEntry::What::Enabled || e.tick < 2 || e.tick > 3000) {
				continue;
			}
			bool allowed = false;
			for (size_t k = 1; k < 5; ++k) {
				allowed = allowed || (e.tick == kConquestCheckTicks[k] && p_expected[r].enabled[k] != p_expected[r].enabled[k - 1]);
			}
			stray += !allowed;
		}
		std::string want;
		for (size_t k = 0; k < 5; ++k) {
			want += p_expected[r].enabled[k] ? "1" : "0";
		}
		line(d, "%-30s node %-14s enabled at 1/601/1201/1801/2401: %s (expected %s), stray changes %zu", p_expected[r].label, node_label(p_graph, n).c_str(),
				got.c_str(), want.c_str(), stray);
		ok = ok && row_ok && stray == 0;
	}
	v.pass = ok;
	line(d, "G6 %s", ok ? "PASS" : "FAIL");
	return v;
}

LogicCensusRow logic_census_row(std::string p_level, std::string p_mode, std::shared_ptr<const LogicGraph> p_graph, double p_build_ms) {
	LogicCensusRow row;
	row.level = std::move(p_level);
	row.mode = std::move(p_mode);
	row.build_ms = p_build_ms;
	if (!p_graph) {
		row.error = "no graph";
		return row;
	}
	row.stats = p_graph->stats();
	row.nodes = p_graph->nodes().size();
	if (core::Result<LogicGhostLayout> layout = LogicGhostLayout::of(*p_graph)) {
		row.ghost_bools = layout->bools.size();
		row.ghost_transforms = layout->transforms.size();
		row.ghost_sequences = layout->sequences.size();
	}
	LogicScriptOptions idle;
	idle.kind = LogicScriptKind::Idle;
	idle.ticks = kCensusIdleTicks;
	core::Result<LogicRunReport> run = run_logic_script(p_graph, idle);
	if (!run) {
		row.error = run.error().message;
		return row;
	}
	if (run->faulted) {
		row.error = run->fault;
		return row;
	}
	row.ok = true;
	row.tick0 = run->tick0;
	row.idle_deliveries = run->deliveries - run->tick0.deliveries;
	row.idle_p99_ns = run->drain_ns_p99;
	row.unhandled = run->unhandled.size();
	return row;
}

std::string logic_census_header() {
	return "level\tmode\tok\tscopes\tobjects\tnodes\tprimitive\tnative\tpassive\tdata\tstructural\tnodes_unknown_class\tunknown\tunimplemented\t"
		   "unknown_unconnected\tunimplemented_unconnected\tevent_edges\tproperty_edges\tlink_edges\tnetworked_only\tduplicate_paths\t"
		   "dropped_realm\tdropped_target_type\tdropped_unbound\tdropped_unresolved\tsynced_bools\tghost_bools\tghost_transforms\t"
		   "ghost_sequences\twork\tbuild_ms\ttick0_deliveries\ttick0_carried\tidle600_deliveries\tidle_p99_ns\tunhandled_ports\t"
		   "unknown_types\tunimplemented_types\terror";
}

std::string logic_census_line(const LogicCensusRow &p_row) {
	const gp::LogicGraphStats &s = p_row.stats;
	const auto types = [](const std::vector<std::pair<std::string, size_t>> &p_list) {
		std::string out;
		for (const auto &[name, count] : p_list) {
			out += (out.empty() ? "" : ",") + name + ":" + std::to_string(count);
		}
		return out.empty() ? std::string("-") : out;
	};
	char b[1024];
	std::snprintf(b, sizeof(b),
			"%s\t%s\t%d\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t"
			"%zu\t%zu\t%zu\t%.1f\t%zu\t%zu\t%zu\t%llu\t%zu\t",
			p_row.level.c_str(), p_row.mode.c_str(), p_row.ok ? 1 : 0, s.scopes, s.objects, p_row.nodes, s.nodes_by_class[1], s.nodes_by_class[2],
			s.nodes_by_class[3], s.nodes_by_class[4], s.nodes_by_class[5], s.nodes_by_class[0], s.unknown_nodes, s.unimplemented_nodes,
			s.unknown_unconnected, s.unimplemented_unconnected, s.event_edges, s.property_edges, s.link_edges, s.networked_only_edges, s.duplicate_paths,
			s.dropped_realm, s.dropped_target_type, s.dropped_unbound, s.dropped_unresolved, s.synced_bools, p_row.ghost_bools, p_row.ghost_transforms,
			p_row.ghost_sequences, s.work, p_row.build_ms, p_row.tick0.deliveries, p_row.tick0.carried_over, p_row.idle_deliveries,
			static_cast<unsigned long long>(p_row.idle_p99_ns), p_row.unhandled);
	std::string out = b;
	out += types(s.unknown_types) + "\t" + types(s.unimplemented_types) + "\t" + (p_row.error.empty() ? std::string("-") : p_row.error);
	return out;
}

} // namespace bf3::sim::logic
