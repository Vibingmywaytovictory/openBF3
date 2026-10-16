// S4 level logic: native port bindings and the native stubs (docs/design/s4.md §8). See logic_native.h.
//
// After interface collapse a capture point's links all leave its own CapturePoint port #-320184006 (the flag and HQ
// prefabs forward their interface link #-275573010 to it), to CharacterSpawn and VehicleSpawn targets alike. The
// stub's link rule therefore keys on the target type: linked CharacterSpawns follow the owner team; linked
// VehicleSpawns are left to the graph's own Enabled wiring (s4.md §8, open question 6).
#include "bf3/sim/logic/logic_native.h"

#include <algorithm>
#include <string>

namespace bf3::sim::logic {

namespace gp = core::gameplay;
using gp::LogicTypeClass;
using T = LogicTypeId;
using D = NativePortDir;
using M = NativePortMeaning;

namespace {

constexpr int32_t id(std::string_view p_name) {
	return gp::logic_port_id(p_name);
}

// The bindings S4 interprets (verified port names and ids, s4.md §8 and the ports census); every other input of a
// native type is Recorded. Sorted by (type, port) at first use.
struct Row {
	T type;
	D dir;
	int32_t port;
	M meaning;
	std::string_view name;
};

constexpr Row kSpawnerRows[] = {
	{ T::Unknown, D::PropertyIn, id("Enabled"), M::Enabled, "Enabled" },
	{ T::Unknown, D::EventIn, id("Enable"), M::Enable, "Enable" },
	{ T::Unknown, D::EventIn, id("Disable"), M::Disable, "Disable" },
	{ T::Unknown, D::EventIn, id("Spawn"), M::Spawn, "Spawn" },
	{ T::Unknown, D::EventIn, id("Destroy"), M::Destroy, "Destroy" },
	{ T::Unknown, D::EventOut, id("OnSpawned"), M::OnSpawned, "OnSpawned" },
	{ T::Unknown, D::EventOut, id("OnDestroyed"), M::OnDestroyed, "OnDestroyed" },
	{ T::Unknown, D::EventOut, id("OnPlayerEnter"), M::OnPlayerEnter, "OnPlayerEnter" },
	{ T::Unknown, D::PropertyOut, native_id::kSpawnerUnknownOut, M::Recorded, "" },
};

std::vector<NativePortBinding> build_bindings() {
	std::vector<NativePortBinding> out;
	const auto add = [&](T p_type, D p_dir, int32_t p_port, M p_meaning, std::string_view p_name) { out.push_back({ p_type, p_dir, p_port, p_meaning, p_name }); };
	for (const T spawner : { T::CharacterSpawn, T::VehicleSpawn, T::AlternateSpawn }) {
		for (const Row &r : kSpawnerRows) {
			add(spawner, r.dir, r.port, r.meaning, r.name);
		}
	}
	add(T::CapturePoint, D::PropertyIn, id("Enabled"), M::Enabled, "Enabled");
	add(T::CapturePoint, D::EventIn, id("Enable"), M::Enable, "Enable");
	add(T::CapturePoint, D::EventOut, id("OnCaptured"), M::OnCaptured, "OnCaptured");
	add(T::CapturePoint, D::EventOut, id("OnLost"), M::OnLost, "OnLost");
	add(T::CapturePoint, D::PropertyOut, id("CurrentOwnerTeam"), M::OwnerTeam, "CurrentOwnerTeam");
	add(T::CapturePoint, D::LinkOut, native_id::kCaptureCharacterSpawnLink, M::LinkedSpawner, "");
	add(T::CapturePoint, D::LinkOut, native_id::kCaptureVehicleSpawnLink, M::Recorded, "");
	for (const T model : { T::StaticModel, T::BreakableModel }) {
		add(model, D::PropertyIn, id("Enabled"), M::Enabled, "Enabled");
		add(model, D::EventIn, id("Enable"), M::Enable, "Enable");
		add(model, D::EventIn, id("Disable"), M::Disable, "Disable");
		add(model, D::EventIn, id("Destroy"), M::Destroy, "Destroy");
		add(model, D::EventOut, id("OnDestroyed"), M::OnDestroyed, "OnDestroyed");
	}
	add(T::HumanPlayer, D::EventOut, id("OnPlayerEnter"), M::OnPlayerEnter, "OnPlayerEnter");
	add(T::PreRound, D::EventOut, id("StartRound"), M::RoundStarted, "StartRound");
	add(T::RoundOver, D::EventIn, id("RoundStarted"), M::RoundStarted, "RoundStarted");
	add(T::RoundOver, D::EventOut, id("OnRoundOver"), M::OnRoundOver, "OnRoundOver");
	add(T::GameInteraction, D::EventIn, id("Enable"), M::Enable, "Enable");
	add(T::GameInteraction, D::EventIn, id("Disable"), M::Disable, "Disable");
	add(T::GameInteraction, D::EventIn, -686199110, M::Activate, ""); // unnamed; the Rush crate interaction's arm input
	// The remaining S4 natives: enable state only.
	for (const T t : { T::CombatAreaTrigger, T::SurroundingGeometry, T::TicketCounter, T::LifeCounter, T::KillCounter, T::AutoTeam, T::Objective,
				 T::DamageAreaTrigger, T::ClearAreaTrigger, T::GeometryTrigger, T::UnderFireTrigger, T::FriendZone, T::Predestruction, T::LevelControl,
				 T::InputRestriction }) {
		add(t, D::PropertyIn, id("Enabled"), M::Enabled, "Enabled");
		add(t, D::EventIn, id("Enable"), M::Enable, "Enable");
		add(t, D::EventIn, id("Disable"), M::Disable, "Disable");
	}
	std::sort(out.begin(), out.end(), [](const NativePortBinding &a, const NativePortBinding &b) {
		return a.type != b.type ? a.type < b.type : (a.port != b.port ? a.port < b.port : a.dir < b.dir);
	});
	return out;
}

const std::vector<NativePortBinding> &bindings() {
	static const std::vector<NativePortBinding> table = build_bindings();
	return table;
}

bool is_native(LogicTypeId p_type) {
	return gp::logic_type_info(p_type).cls == LogicTypeClass::Native;
}

} // namespace

std::span<const NativePortBinding> native_ports(LogicTypeId p_type) {
	const std::vector<NativePortBinding> &all = bindings();
	const auto lo = std::lower_bound(all.begin(), all.end(), p_type, [](const NativePortBinding &b, LogicTypeId t) { return b.type < t; });
	auto hi = lo;
	while (hi != all.end() && hi->type == p_type) {
		++hi;
	}
	return std::span<const NativePortBinding>(lo, hi);
}

std::optional<NativePortBinding> native_port(LogicTypeId p_type, NativePortDir p_dir, int32_t p_port) {
	for (const NativePortBinding &b : native_ports(p_type)) {
		if (b.port == p_port && b.dir == p_dir) {
			return b;
		}
	}
	return std::nullopt;
}

struct LogicNativeStubs::State {
	std::shared_ptr<const gp::LogicGraph> graph;
	std::vector<std::optional<NativeStubState>> stubs; // per node; set for Native nodes
	std::vector<NativeTimelineEntry> timeline;
	bool timeline_full = false; // a note() was refused at kMaxNativeTimelineEntries; apply() then fails
	Tick tick = 0; // the last tick applied
	struct Pending {
		Tick tick = 0;
		uint32_t node = 0;
		bool enabled = false;
	};
	std::vector<Pending> pending; // native-side changes for a later apply (the capture point link rule)

	void note(uint32_t p_node, NativeTimelineEntry::What p_what, int64_t p_value) {
		NativeTimelineEntry e;
		e.tick = tick;
		e.node = p_node;
		e.what = p_what;
		e.value = p_value;
		if (timeline.size() >= kMaxNativeTimelineEntries) {
			timeline_full = true;
			return;
		}
		timeline.push_back(e);
	}

	void set_enabled(uint32_t p_node, bool p_enabled) {
		NativeStubState &s = *stubs[p_node];
		if (s.enabled != p_enabled) {
			s.enabled = p_enabled;
			note(p_node, NativeTimelineEntry::What::Enabled, p_enabled ? 1 : 0);
		}
	}
};

LogicNativeStubs::LogicNativeStubs(std::unique_ptr<State> p_state) :
		state_(std::move(p_state)) {}
LogicNativeStubs::LogicNativeStubs(LogicNativeStubs &&) noexcept = default;
LogicNativeStubs &LogicNativeStubs::operator=(LogicNativeStubs &&) noexcept = default;
LogicNativeStubs::~LogicNativeStubs() = default;

core::Result<LogicNativeStubs> LogicNativeStubs::create(std::shared_ptr<const gp::LogicGraph> p_graph) {
	if (!p_graph) {
		return core::make_error("logic natives: no graph");
	}
	auto state = std::make_unique<State>();
	state->graph = std::move(p_graph);
	const gp::LogicGraph &g = *state->graph;
	state->stubs.resize(g.nodes().size());
	const int32_t enabled = id("Enabled");
	for (uint32_t i = 0; i < g.nodes().size(); ++i) {
		const LogicTypeId type = g.nodes()[i].type;
		if (!is_native(type)) {
			continue;
		}
		NativeStubState s;
		const std::optional<LogicValue> e = g.param(i, enabled);
		s.enabled = e ? (e->kind == LogicValue::Kind::Bool ? e->b : e->i != 0) : true;
		if (type == T::CapturePoint) {
			if (const std::optional<LogicValue> owner = g.param(i, id("InitialOwnerTeam"))) {
				s.owner = static_cast<uint8_t>(std::clamp<int64_t>(owner->i, 0, 255));
			}
		} else if (type == T::TicketCounter) {
			if (const std::optional<LogicValue> t = g.param(i, id("InitialTicketCount"))) {
				s.tickets = t->i;
			}
		} else if (type == T::LifeCounter) {
			if (const std::optional<LogicValue> t = g.param(i, id("StartingLifeCount"))) {
				s.tickets = t->i;
			}
		}
		state->stubs[i] = s;
	}
	return LogicNativeStubs(std::move(state));
}

core::Status LogicNativeStubs::apply(Tick p_tick, std::span<const LogicEffect> p_effects, LogicRuntime &r_runtime) {
	State &s = *state_;
	s.tick = p_tick;
	// Native-side changes scheduled for this tick first (in the order they were scheduled).
	std::vector<State::Pending> later;
	for (const State::Pending &p : s.pending) {
		if (p.tick <= p_tick) {
			s.set_enabled(p.node, p.enabled);
		} else {
			later.push_back(p);
		}
	}
	s.pending.swap(later);
	const gp::LogicGraph &g = *s.graph;
	for (const LogicEffect &e : p_effects) {
		if (e.apply_tick != p_tick) {
			continue;
		}
		if (e.node >= s.stubs.size() || !s.stubs[e.node]) {
			return core::make_error("logic natives: effect into node " + std::to_string(e.node) + ", which is not native");
		}
		const LogicTypeId type = g.nodes()[e.node].type;
		const D dir = e.kind == LogicEffectKind::Property ? D::PropertyIn : D::EventIn;
		const std::optional<NativePortBinding> b = native_port(type, dir, e.port);
		const M meaning = b ? b->meaning : M::Recorded;
		NativeStubState &st = *s.stubs[e.node];
		switch (meaning) {
			case M::Enabled: {
				const LogicValue &v = e.value;
				const bool on = v.kind == LogicValue::Kind::Bool ? v.b : (v.kind == LogicValue::Kind::Float ? v.f != 0.0 : v.i != 0);
				s.set_enabled(e.node, on);
				break;
			}
			case M::Enable:
				s.set_enabled(e.node, true);
				break;
			case M::Disable:
				s.set_enabled(e.node, false);
				break;
			case M::Spawn:
				s.note(e.node, NativeTimelineEntry::What::Spawned, 1);
				BF3_TRY(r_runtime.force_event(e.node, id("OnSpawned"), e.context));
				break;
			case M::Destroy:
				if (!st.destroyed) {
					st.destroyed = true;
					s.note(e.node, NativeTimelineEntry::What::Destroyed, 1);
				}
				break;
			case M::RoundStarted:
				s.note(e.node, NativeTimelineEntry::What::RoundOver, 0);
				break;
			default:
				s.note(e.node, NativeTimelineEntry::What::Input, e.port);
				break;
		}
	}
	if (s.timeline_full) {
		return core::make_error("logic natives: the timeline holds more than " + std::to_string(kMaxNativeTimelineEntries) + " entries");
	}
	return core::Status::success();
}

core::Status LogicNativeStubs::player_enter(uint32_t p_human_player, LogicContext p_player, LogicRuntime &r_runtime) {
	State &s = *state_;
	if (p_human_player >= s.stubs.size() || s.graph->nodes()[p_human_player].type != T::HumanPlayer) {
		return core::make_error("logic natives: player_enter on node " + std::to_string(p_human_player) + ", which is not a HumanPlayer");
	}
	return r_runtime.force_event(p_human_player, gp::logic_port::kOnPlayerEnter, p_player);
}

core::Status LogicNativeStubs::destroy(uint32_t p_node, LogicRuntime &r_runtime) {
	State &s = *state_;
	if (p_node >= s.stubs.size() || !s.stubs[p_node]) {
		return core::make_error("logic natives: destroy on node " + std::to_string(p_node) + ", which is not native");
	}
	NativeStubState &st = *s.stubs[p_node];
	if (!st.destroyed) {
		st.destroyed = true;
		s.note(p_node, NativeTimelineEntry::What::Destroyed, 1);
	}
	return r_runtime.force_event(p_node, gp::logic_port::kOnDestroyed, LogicContext{});
}

core::Status LogicNativeStubs::capture(uint32_t p_capture_point, uint8_t p_team, LogicRuntime &r_runtime) {
	State &s = *state_;
	const gp::LogicGraph &g = *s.graph;
	if (p_capture_point >= s.stubs.size() || g.nodes()[p_capture_point].type != T::CapturePoint) {
		return core::make_error("logic natives: capture on node " + std::to_string(p_capture_point) + ", which is not a CapturePoint");
	}
	NativeStubState &st = *s.stubs[p_capture_point];
	if (st.owner == p_team) {
		return core::Status::success();
	}
	const uint8_t previous = st.owner;
	st.owner = p_team;
	s.note(p_capture_point, NativeTimelineEntry::What::Owner, p_team);
	if (previous != 0) {
		LogicContext lost;
		lost.team = previous;
		BF3_TRY(r_runtime.force_event(p_capture_point, gp::logic_port::kOnLost, lost));
	}
	LogicContext captured;
	captured.team = p_team;
	BF3_TRY(r_runtime.force_event(p_capture_point, gp::logic_port::kOnCaptured, captured));
	// The capture point's own character spawners follow the owner from the next tick [inferred, s4.md §8].
	const int32_t team_field = id("Team");
	for (const gp::LogicEdge &e : g.links_from(p_capture_point)) {
		if (e.source_port != native_id::kCaptureCharacterSpawnLink || g.nodes()[e.target].type != T::CharacterSpawn || !s.stubs[e.target]) {
			continue;
		}
		const std::optional<LogicValue> team = g.param(e.target, team_field);
		const bool own = team && team->i == p_team;
		s.pending.push_back({ s.tick + 1, e.target, own });
	}
	return core::Status::success();
}

std::optional<NativeStubState> LogicNativeStubs::state(uint32_t p_node) const {
	if (p_node >= state_->stubs.size()) {
		return std::nullopt;
	}
	return state_->stubs[p_node];
}

std::span<const NativeTimelineEntry> LogicNativeStubs::timeline() const {
	return state_->timeline;
}

} // namespace bf3::sim::logic
