#include "logic_commands.h"

#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/gameplay/logic_world.h"
#include "bf3/core/level/level.h"
#include "bf3/core/path_guard.h"
#include "bf3/sim/logic/logic_harness.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <optional>
#include <cstdio>
#include <map>
#include <set>
#include <string>

using namespace bf3::core;
using namespace bf3::core::gameplay;

namespace bf3dump {

namespace {

double seconds_since(std::chrono::steady_clock::time_point p_start) {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - p_start).count();
}

std::string guid8(const Guid &p_guid) {
	std::string s = p_guid.to_string();
	if (!s.empty() && s[0] == '{') {
		s = s.substr(1);
	}
	return s.substr(0, 8);
}

const char *const kPortNames[] = { "In", "Out", "In0", "In1", "In2", "In3", "In4", "In5", "In6", "In7", "In8", "In9", "Value", "SetTrue", "SetFalse",
	"Toggle", "OnTrue", "OnFalse", "Open", "Close", "Enable", "Disable", "Enabled", "Trigger", "OnTrigger", "Reset", "Start", "Stop", "Restart",
	"OnFinished", "OnStopped", "OnStarted", "Pause", "PlayFirstFrame", "Destroy", "OnDestroyed", "Spawn", "Unspawn", "OnSpawned", "OnCaptured", "OnLost",
	"OnPlayerEnter", "OnPlayerLeave", "OnRoundOver", "RoundStarted", "StartRound", "TeamWins", "StopTime", "OnTriggeredByMatchingTeam",
	"OnTriggerOnlyForSpecifiedTeam", "OnTriggerOnlyForPlayer", "OnTriggeredByHost", "OnTriggerOnlyForHost", "Delay", "Impulse", "OnImpulse", "A<B", "A=B",
	"A>B", "A!=B", "A<=B", "A>=B", "A", "B", "Bool", "Out0", "Out1", "Out2", "Out3", "Out4", "Out5", "Out6", "Out7", "Out8", "Out9", "NextOut",
	"PreviousOut", "Increment", "Decrement", "ResetCount", "SubtractCount", "OnReachedZero", "BoolIn", "BoolOut", "TransformIn", "TransformOut", "FloatIn",
	"FloatOut", "IntIn", "IntOut", "IntIn0", "IntIn1", "FloatIn0", "FloatIn1", "InputSelect", "OnCommand", "Print", "CastToBool", "CastToInt",
	"CastToFloat", "BoolValue", "IntValue", "FloatValue", "Transform", "CurrentOwnerTeam", "FlagLocation", "FlagDirection", "Tickets", "Kills",
	"KillLimit", "Activate", "Deactivate", "DamageArea", "ClearArea", "ShowObjective", "HideObjective", "Success", "OnSuccess", "OnInteraction",
	"OnEnter", "OnLeave", "OnInsideArea", "SendMessage", "DisableCapture", "EnableCapture", "SpawnDelay", "InitialSpawnDelay", "MaxCount", "Team",
	"InitialOwnerTeam", "BlueprintTransform", "LocationNameSid", "LocationTextSid", "DecreaseLife", "IncreaseReinforce", "DestructBase", "OnDisabled",
	"UnSpawnAllSoldiers", "OnPlayerSpawned", "OnPlayerDied", "OnSpectatorEnter", "ExternalTime", "TriggerAmount", "SetVariation1", "SetVariation2",
	"EnablePlayerMovement", "PlayerEnter", "DisableAllowedToSpawnOn", "EnableAllowedToSpawnOn", "RestartLevel", "AreaValue", "CaptureRadius" };

std::string port_name(int32_t p_port) {
	static const std::map<int32_t, const char *> names = [] {
		std::map<int32_t, const char *> m;
		for (const char *n : kPortNames) {
			m.emplace(logic_port_id(n), n);
		}
		return m;
	}();
	const auto it = names.find(p_port);
	return it != names.end() ? std::string(it->second) : "#" + std::to_string(p_port);
}

const char *realm_name(LogicRealm p_realm) {
	switch (p_realm) {
		case LogicRealm::Client:
			return "C";
		case LogicRealm::Server:
			return "S";
		case LogicRealm::ClientAndServer:
			return "CS";
		case LogicRealm::None:
			return "N";
		case LogicRealm::Pipeline:
			return "P";
		case LogicRealm::Unset:
			return "-";
	}
	return "?";
}

const char *class_name(LogicTypeClass p_class) {
	static const char *const kNames[] = { "unknown", "primitive", "native", "passive", "data", "structural" };
	return kNames[static_cast<size_t>(p_class)];
}

std::string value_text(const LogicGraph &p_graph, const LogicValue &p_value) {
	char b[160];
	switch (p_value.kind) {
		case LogicValue::Kind::None:
			return "none";
		case LogicValue::Kind::Bool:
			return p_value.b ? "true" : "false";
		case LogicValue::Kind::Int:
			return std::to_string(p_value.i);
		case LogicValue::Kind::Team:
			return "team" + std::to_string(p_value.i);
		case LogicValue::Kind::Float:
			std::snprintf(b, sizeof(b), "%g", p_value.f);
			return b;
		case LogicValue::Kind::Text:
			return "\"" + std::string(p_graph.text(static_cast<uint32_t>(p_value.i))) + "\"";
		case LogicValue::Kind::Vec3:
			std::snprintf(b, sizeof(b), "(%g,%g,%g)", p_value.m[0], p_value.m[1], p_value.m[2]);
			return b;
		case LogicValue::Kind::Transform:
			std::snprintf(b, sizeof(b), "T(%g,%g,%g)", p_value.m[9], p_value.m[10], p_value.m[11]);
			return b;
	}
	return "?";
}

std::string field_name(LogicTypeId p_type, int32_t p_field) {
	for (const LogicParamField &f : logic_param_fields(p_type)) {
		if (logic_port_id(f.name) == p_field) {
			return std::string(f.name);
		}
		for (const LogicParamMember &m : f.members) {
			if (!m.name.empty() && logic_port_id(m.name) == p_field) {
				return std::string(f.name) + "." + std::string(m.name);
			}
		}
	}
	return "#" + std::to_string(p_field);
}

std::string scope_chain(const LogicGraph &p_graph, uint32_t p_scope) {
	std::string out;
	for (uint32_t s = p_scope, n = 0; s != kNoLogicIndex && s < p_graph.scopes().size() && n < 64; s = p_graph.scopes()[s].parent, ++n) {
		const LogicScopeInfo &info = p_graph.scopes()[s];
		out += info.reference.is_zero() ? std::string("root") : guid8(info.reference) + "<";
	}
	return out;
}

struct Loaded {
	std::unique_ptr<level::LoadedLevel> level;
	std::shared_ptr<const LogicGraph> graph;
	double load_s = 0, build_s = 0;
};

Result<Loaded> load_graph(const std::shared_ptr<Install> &p_install, const std::string &p_level, const std::string &p_mode, LogicSide p_side) {
	Loaded out;
	const auto t0 = std::chrono::steady_clock::now();
	Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(p_install, p_level, p_mode);
	if (!loaded) {
		return error_at(p_level + " " + p_mode, loaded.error());
	}
	out.level = std::move(*loaded);
	out.load_s = seconds_since(t0);
	const auto t1 = std::chrono::steady_clock::now();
	LogicGraphOptions options;
	options.mode = p_mode;
	options.side = p_side;
	Result<LogicGraph> graph = build_logic_graph(out.level->db(), out.level->root(), options);
	if (!graph) {
		return error_at(p_level + " " + p_mode, graph.error());
	}
	out.graph = std::make_shared<const LogicGraph>(std::move(*graph));
	out.build_s = seconds_since(t1);
	return out;
}

void print_stats(const LogicGraph &p_graph) {
	const LogicGraphStats &s = p_graph.stats();
	std::printf("scopes\t%zu\nobjects\t%zu\nnodes\t%zu\n", s.scopes, s.objects, p_graph.nodes().size());
	for (size_t c = 0; c < 6; ++c) {
		std::printf("nodes.%s\t%zu\n", class_name(static_cast<LogicTypeClass>(c)), s.nodes_by_class[c]);
	}
	std::printf("unknown_nodes\t%zu\nunimplemented_nodes\t%zu\nunknown_unconnected\t%zu\nunimplemented_unconnected\t%zu\n", s.unknown_nodes,
			s.unimplemented_nodes, s.unknown_unconnected, s.unimplemented_unconnected);
	std::printf("event_edges\t%zu\nproperty_edges\t%zu\nlink_edges\t%zu\nnetworked_only_edges\t%zu\nduplicate_paths\t%zu\n", s.event_edges,
			s.property_edges, s.link_edges, s.networked_only_edges, s.duplicate_paths);
	std::printf("connections_scanned\t%zu\ninterface_hops\t%zu\ndropped_realm\t%zu\ndropped_target_type\t%zu\ndropped_unbound\t%zu\n"
				"dropped_unbound_events\t%zu\ndropped_unresolved\t%zu\n",
			s.connections_scanned, s.interface_hops, s.dropped_realm, s.dropped_target_type, s.dropped_unbound, s.dropped_unbound_events,
			s.dropped_unresolved);
	std::printf("synced_bools\t%zu\nwork\t%zu\ndigest\t%016llx\n", s.synced_bools, s.work, static_cast<unsigned long long>(p_graph.digest()));
	for (const auto &[name, count] : s.unknown_types) {
		std::printf("unknown_type\t%s\t%zu\n", name.c_str(), count);
	}
	for (const auto &[name, count] : s.unimplemented_types) {
		std::printf("unimplemented_type\t%s\t%zu\n", name.c_str(), count);
	}
}

int cmd_instantiate(const LogicGraph &p_graph, bool p_edges) {
	std::printf("# node\tindex\ttype\tclass\trealm\tconnected\tguid\tkey\tscope\tchain\tparams\tinputs\n");
	for (uint32_t i = 0; i < p_graph.nodes().size(); ++i) {
		const LogicNode &n = p_graph.nodes()[i];
		std::string params;
		for (const LogicParam &p : p_graph.params_of(i)) {
			params += field_name(n.type, p.field) + (p.element ? "[" + std::to_string(p.element) + "]" : "") + "=" + value_text(p_graph, p.value) + " ";
		}
		std::string inputs;
		for (uint32_t k = 0; k < n.input_count; ++k) {
			const LogicInput &in = p_graph.initial_inputs()[n.input_begin + k];
			inputs += port_name(in.port) + "=" + value_text(p_graph, in.value) + " ";
		}
		std::printf("node\t%u\t%s\t%s\t%s\t%d\t%s\t%016llx\t%u\t%s\t%s\t%s\n", i, std::string(p_graph.text(n.type_name)).c_str(),
				class_name(logic_type_info(n.type).cls), realm_name(n.realm), (n.flags & kLogicNodeConnected) ? 1 : 0, guid8(n.guid).c_str(),
				static_cast<unsigned long long>(n.key), n.scope, scope_chain(p_graph, n.scope).c_str(), params.c_str(), inputs.c_str());
	}
	if (!p_edges) {
		return 0;
	}
	const auto label = [&](uint32_t p_node) -> std::string {
		if (p_node == kNoLogicIndex) {
			return "-";
		}
		const LogicNode &n = p_graph.nodes()[p_node];
		std::string t(p_graph.text(n.type_name));
		if (t.size() > 10 && t.substr(t.size() - 10) == "EntityData") {
			t = t.substr(0, t.size() - 10);
		}
		return std::to_string(p_node) + ":" + t + ":" + guid8(n.guid);
	};
	static const char *const kTargets[] = { "Invalid", "ClientAndServer", "Client", "Server", "NetworkedClient", "NetworkedClientAndServer" };
	std::printf("# edge\tkind\tsource\tport\ttarget\tport\ttarget_type\tconnection\n");
	const auto edges = [&](const char *p_kind, std::span<const LogicEdge> p_list) {
		for (const LogicEdge &e : p_list) {
			std::printf("edge\t%s\t%s\t%s\t%s\t%s\t%s\t%016llx\n", p_kind, label(e.source).c_str(), port_name(e.source_port).c_str(), label(e.target).c_str(),
					port_name(e.target_port).c_str(), kTargets[static_cast<size_t>(e.target_type) % 6], static_cast<unsigned long long>(e.connection));
		}
	};
	edges("E", p_graph.event_edges());
	edges("P", p_graph.property_edges());
	edges("L", p_graph.link_edges());
	edges("N", p_graph.networked_edges());
	return 0;
}

int cmd_ports(const LogicGraph &p_graph) {
	std::map<std::string, std::map<std::string, size_t>> usage;
	const auto type_of = [&](uint32_t p_node) { return std::string(p_graph.text(p_graph.nodes()[p_node].type_name)); };
	const auto count = [&](std::span<const LogicEdge> p_list, const char *p_in, const char *p_out) {
		for (const LogicEdge &e : p_list) {
			++usage[type_of(e.source)][std::string(p_out) + " " + port_name(e.source_port)];
			if (e.target != kNoLogicIndex) {
				++usage[type_of(e.target)][std::string(p_in) + " " + port_name(e.target_port)];
			}
		}
	};
	count(p_graph.event_edges(), "E<", "E>");
	count(p_graph.property_edges(), "P<", "P>");
	count(p_graph.link_edges(), "L<", "L>");
	count(p_graph.networked_edges(), "N<", "N>");
	for (const auto &[type, ports] : usage) {
		const LogicTypeInfo &info = logic_type_info(logic_type_of(type));
		std::printf("%s\t%s%s\n", type.c_str(), class_name(info.cls), info.s4 ? " s4" : "");
		for (const auto &[port, n] : ports) {
			std::printf("\t%s\t%zu\n", port.c_str(), n);
		}
	}
	return 0;
}

int cmd_fields(const level::LoadedLevel &p_level, const std::string &p_mode, const std::vector<std::string> &p_types) {
	LogicWorldOptions o;
	o.mode = p_mode;
	o.expand_root_objects = true;
	Result<LogicWorld> world = LogicWorld::build(p_level.db(), p_level.root(), o);
	if (!world) {
		std::fprintf(stderr, "logic fields: %s\n", world.error().message.c_str());
		return 1;
	}
	std::set<std::string> done;
	for (const LogicObject &ob : world->objects()) {
		const std::string type(ob.entity.type_name());
		bool want = false;
		for (const std::string &t : p_types) {
			want |= type == t;
		}
		if (!want || !done.insert(type).second) {
			continue;
		}
		std::printf("%s %s\n", type.c_str(), guid8(ob.entity.guid()).c_str());
		for (const EbxField &f : ob.entity.fields()) {
			std::string v;
			if (const auto b = f.value.as_bool(); b && f.value.kind() == EbxValue::Kind::Bool) {
				v = *b ? "true" : "false";
			} else if (const auto e = f.value.enum_name()) {
				v = std::string(*e);
			} else if (const auto s = f.value.as_str()) {
				v = "\"" + std::string(*s) + "\"";
			} else if (const auto d = f.value.as_f64()) {
				v = std::to_string(*d);
			} else if (const auto i = f.value.as_i64()) {
				v = std::to_string(*i);
			} else if (const OptionalEbxArray a = f.value.as_array()) {
				v = "[" + std::to_string(a->size()) + "]";
				size_t k = 0;
				for (const EbxValue el : *a) {
					if (k++ >= 3) {
						break;
					}
					if (const auto st = el.as_struct()) {
						v += " {";
						for (const EbxField &m : st->fields()) {
							v += std::string(m.name) + "=";
							if (const auto mi = m.value.as_i64()) {
								v += std::to_string(*mi);
							} else if (const auto md = m.value.as_f64()) {
								v += std::to_string(*md);
							} else if (const auto ms = m.value.as_str()) {
								v += std::string(*ms);
							}
							v += " ";
						}
						v += "}";
					} else if (const auto r = el.as_ref()) {
						v += r->is_null() ? " null" : " ref";
						if (const std::optional<EbxInstance> t = p_level.db().resolve(ob.entity.partition(), *r)) {
							v += ":" + std::string(t->type_name()) + "{";
							for (const EbxField &m : t->fields()) {
								v += std::string(m.name) + "=";
								if (const auto mi = m.value.as_i64()) {
									v += std::to_string(*mi);
								} else if (const auto md = m.value.as_f64()) {
									v += std::to_string(*md);
								}
								v += " ";
							}
							v += "}";
						}
					} else if (const auto ei = el.as_i64()) {
						v += " " + std::to_string(*ei);
					} else if (const auto ed = el.as_f64()) {
						v += " " + std::to_string(*ed);
					}
				}
			} else {
				v = "(" + std::to_string(static_cast<int>(f.value.kind())) + ")";
			}
			std::printf("  %s = %s\n", std::string(f.name).c_str(), v.c_str());
		}
	}
	return 0;
}

bool parse_u64(const std::string &p_text, uint64_t &r_value) {
	const auto res = std::from_chars(p_text.data(), p_text.data() + p_text.size(), r_value);
	return res.ec == std::errc() && res.ptr == p_text.data() + p_text.size();
}

bool parse_force(const LogicGraph &p_graph, const std::string &p_spec, bf3::sim::logic::LogicScriptOptions::Force &r_force) {
	const size_t colon = p_spec.find(':');
	const size_t at = p_spec.rfind('@');
	if (colon == std::string::npos || at == std::string::npos || at < colon) {
		return false;
	}
	const std::string node = p_spec.substr(0, colon), port = p_spec.substr(colon + 1, at - colon - 1), tick = p_spec.substr(at + 1);
	uint64_t v = 0;
	if (parse_u64(node, v) && v < p_graph.nodes().size()) {
		r_force.node = static_cast<uint32_t>(v);
	} else {
		std::vector<uint32_t> found;
		for (uint32_t i = 0; i < p_graph.nodes().size(); ++i) {
			if (guid8(p_graph.nodes()[i].guid).rfind(node, 0) == 0 || p_graph.nodes()[i].guid.to_string().find(node) == 1) {
				found.push_back(i);
			}
		}
		if (found.size() != 1) {
			std::fprintf(stderr, "--force %s: node matches %zu nodes\n", p_spec.c_str(), found.size());
			return false;
		}
		r_force.node = found[0];
	}
	int64_t port_id = 0;
	const auto res = std::from_chars(port.data(), port.data() + port.size(), port_id);
	r_force.port = (res.ec == std::errc() && res.ptr == port.data() + port.size()) ? static_cast<int32_t>(port_id) : logic_port_id(port);
	if (!parse_u64(tick, v) || v == 0 || v > 0xffffffffull) {
		return false;
	}
	r_force.tick = static_cast<bf3::sim::Tick>(v);
	return true;
}

int cmd_run(const LogicGraph &p_graph_ref, const std::vector<std::string> &p_args, bool p_trace) {
	namespace lg = bf3::sim::logic;
	std::string script = "idle";
	std::optional<uint64_t> ticks, seed;
	std::vector<lg::LogicScriptOptions::Force> forces;
	for (size_t i = 0; i < p_args.size(); ++i) {
		const std::string &a = p_args[i];
		const bool has_value = i + 1 < p_args.size();
		uint64_t v = 0;
		if (a == "--script" && has_value) {
			script = p_args[++i];
		} else if (a == "--ticks" && has_value && parse_u64(p_args[i + 1], v)) {
			if (v > lg::kMaxLogicScriptTicks) {
				std::fprintf(stderr, "logic run: --ticks %llu is more than %u\n", static_cast<unsigned long long>(v), lg::kMaxLogicScriptTicks);
				return 2;
			}
			ticks = v;
			++i;
		} else if (a == "--seed" && has_value && parse_u64(p_args[i + 1], v)) {
			seed = v;
			++i;
		} else if (a == "--force" && has_value) {
			lg::LogicScriptOptions::Force f;
			if (!parse_force(p_graph_ref, p_args[++i], f)) {
				std::fprintf(stderr, "bad --force %s (expected <node>:<port>@<tick>)\n", p_args[i].c_str());
				return 2;
			}
			forces.push_back(f);
		} else {
			std::fprintf(stderr, "logic run: unknown argument %s\n", a.c_str());
			return 2;
		}
	}
	lg::LogicScriptOptions options;
	if (script == "rush") {
		options = lg::rush_mcoms_script();
	} else if (script == "conquest") {
		options = lg::caspian_conquest_script();
	} else if (script == "idle") {
		options.kind = lg::LogicScriptKind::Idle;
		options.ticks = 3600;
	} else {
		std::fprintf(stderr, "logic run: unknown script %s (idle|rush|conquest)\n", script.c_str());
		return 2;
	}
	if (ticks) {
		options.ticks = static_cast<bf3::sim::Tick>(std::min<uint64_t>(*ticks, 0xffffffffull));
	}
	if (seed) {
		options.runtime.root_seed = *seed;
	}
	options.forces = forces;
	options.runtime.keep_trace_records = p_trace;

	const std::shared_ptr<const LogicGraph> graph(std::shared_ptr<const LogicGraph>(), &p_graph_ref);
	const auto t0 = std::chrono::steady_clock::now();
	Result<lg::LogicRunReport> report = lg::run_logic_script(graph, options);
	const double run_s = seconds_since(t0);
	if (!report) {
		std::fprintf(stderr, "logic run: %s\n", report.error().message.c_str());
		return 1;
	}
	const lg::LogicRunReport &r = *report;
	if (p_trace) {
		std::printf("# trace\ttick\tseq\tkind\tsource\tsource_port\ttarget\ttarget_port\tvalue_hash\tplayer\n");
		static const char *const kKinds[] = { "event", "property", "timer", "forced", "effect", "carried", "fault" };
		for (const lg::LogicTraceRecord &t : r.trace) {
			std::printf("trace\t%u\t%u\t%s\t%u\t%s\t%u\t%s\t%016llx\t%d\n", t.tick, t.seq, kKinds[static_cast<size_t>(t.kind) % 7], t.source,
					port_name(t.source_port).c_str(), t.target, port_name(t.target_port).c_str(), static_cast<unsigned long long>(t.value_hash), t.player);
		}
	}
	static const char *const kWhat[] = { "enabled", "destroyed", "owner", "spawned", "round", "input" };
	for (const lg::NativeTimelineEntry &e : r.timeline) {
		if (e.what == lg::NativeTimelineEntry::What::Input) {
			continue;
		}
		const LogicNode &n = p_graph_ref.nodes()[e.node];
		std::printf("timeline\t%u\t%u\t%s\t%s\t%s\t%lld\n", e.tick, e.node, std::string(p_graph_ref.text(n.type_name)).c_str(), guid8(n.guid).c_str(),
				kWhat[static_cast<size_t>(e.what) % 6], static_cast<long long>(e.value));
	}
	for (const lg::LogicRunReport::Action &a : r.actions) {
		static const char *const kActions[] = { "player_enter", "destroy", "capture", "round_start" };
		std::printf("action\t%u\t%u\t%s\t%s\tteam%u\n", a.tick, a.node, guid8(p_graph_ref.nodes()[a.node].guid).c_str(),
				kActions[static_cast<size_t>(a.what) % 4], a.team);
	}
	for (const lg::LogicUnhandledPort &u : r.unhandled) {
		std::printf("unhandled\t%s\t%s\t%s\t%llu\n", std::string(logic_type_info(u.type).ebx_type).c_str(), port_name(u.port).c_str(),
				u.event ? "event" : "property", static_cast<unsigned long long>(u.count));
	}
	std::printf("ticks\t%u\ndeliveries\t%zu\nmax_deliveries_tick\t%zu\ncarried_over\t%zu\neffects\t%zu\nnetworked\t%zu\n", r.ticks, r.deliveries,
			r.max_deliveries_tick, r.carried_over, r.effects, r.networked);
	std::printf("tick0\tdeliveries %zu timers %zu carried %zu queue_peak %zu effects %zu drain_ns %llu\n", r.tick0.deliveries, r.tick0.timers_fired,
			r.tick0.carried_over, r.tick0.queue_peak, r.tick0.effects, static_cast<unsigned long long>(r.tick0.drain_ns));
	std::printf("drain_ns\tp50 %llu p99 %llu busy_p99 %llu max %llu busy_ticks %zu (wall clock, shared machine)\n",
			static_cast<unsigned long long>(r.drain_ns_p50), static_cast<unsigned long long>(r.drain_ns_p99),
			static_cast<unsigned long long>(r.drain_ns_busy_p99), static_cast<unsigned long long>(r.drain_ns_max), r.busy_ticks);
	std::printf("trace_hash\t%016llx\ntrace_records\t%zu%s\n", static_cast<unsigned long long>(r.trace_hash), r.trace_records,
			r.trace_truncated ? " (records truncated)" : "");
	if (r.faulted) {
		std::printf("fault\t%s\n", r.fault.c_str());
	}
	int rc = r.faulted ? 1 : 0;
	if (options.kind == lg::LogicScriptKind::RushMcoms) {
		const lg::LogicGateVerdict g2 = lg::judge_rush_spawner_order(p_graph_ref, r);
		const lg::LogicGateVerdict g3 = lg::judge_park_delay(p_graph_ref, r);
		std::printf("%s%s", g2.detail.c_str(), g3.detail.c_str());
		rc = (g2.pass && g3.pass) ? rc : 1;
	} else if (options.kind == lg::LogicScriptKind::ConquestCaptures) {
		const lg::LogicGateVerdict g6 = lg::judge_conquest_timeline(p_graph_ref, r, lg::caspian_conquest_expected());
		std::printf("%s", g6.detail.c_str());
		rc = g6.pass ? rc : 1;
	}
	std::fprintf(stderr, "logic run: %u ticks in %.3f s\n", r.ticks, run_s);
	return rc;
}

int cmd_census(const InstallReader &p_reader, const std::shared_ptr<Install> &p_install, const std::vector<std::string> &p_args) {
	namespace lg = bf3::sim::logic;
	std::vector<std::string> levels;
	std::string out_path;
	for (size_t i = 0; i < p_args.size(); ++i) {
		if (p_args[i] == "--out" && i + 1 < p_args.size()) {
			out_path = p_args[++i];
		} else {
			levels.push_back(p_args[i]);
		}
	}
	if (levels.empty()) {
		for (const Level &l : p_install->levels()) {
			levels.push_back(l.id);
		}
	}
	FILE *out = stdout;
	if (!out_path.empty()) {
		const std::filesystem::path path = std::filesystem::path(std::u8string(out_path.begin(), out_path.end()));
		if (p_reader.write_guard().check_write_target(path) != PathGuard::Verdict::Allowed) {
			std::fprintf(stderr, "logic census: refusing to write %s\n", out_path.c_str());
			return 2;
		}
		if (_wfopen_s(&out, to_extended_length(path).c_str(), L"wb") != 0 || out == nullptr) {
			std::fprintf(stderr, "logic census: cannot open %s\n", out_path.c_str());
			return 1;
		}
	}
	std::fprintf(out, "%s\n", lg::logic_census_header().c_str());
	size_t rows = 0, failed = 0;
	const auto start = std::chrono::steady_clock::now();
	for (const std::string &level_id : levels) {
		Result<level::LevelInfo> info = level::level_info(*p_install, level_id);
		if (!info) {
			std::fprintf(stderr, "logic census: %s: %s\n", level_id.c_str(), info.error().message.c_str());
			++failed;
			continue;
		}
		for (const std::string &mode : info->modes) {
			Result<Loaded> loaded = load_graph(p_install, level_id, mode, LogicSide::Server);
			lg::LogicCensusRow row;
			if (!loaded) {
				row.level = level_id;
				row.mode = mode;
				row.error = loaded.error().message;
			} else {
				row = lg::logic_census_row(level_id, mode, loaded->graph, loaded->build_s * 1000.0);
			}
			failed += row.ok ? 0 : 1;
			++rows;
			std::fprintf(out, "%s\n", lg::logic_census_line(row).c_str());
			std::fflush(out);
		}
	}
	if (out != stdout) {
		std::fclose(out);
	}
	std::fprintf(stderr, "logic census: %zu rows, %zu failed, %.1f s\n", rows, failed, seconds_since(start));
	return failed == 0 ? 0 : 1;
}

}

const char *logic_usage() {
	return "  logic instantiate <LEVEL> <MODE> [--edges] [--client]\n"
		   "  logic stats <LEVEL> <MODE> [--client]\n"
		   "  logic ports <LEVEL> <MODE>\n"
		   "  logic guids <LEVEL> <MODE> <guid-prefix>...\n"
		   "  logic fields <LEVEL> <MODE> <EbxType>...\n"
		   "  logic run|trace <LEVEL> <MODE> [--script idle|rush|conquest] [--ticks N] [--seed S] [--force <node>:<port>@<tick>]...\n"
		   "  logic census [<LEVEL>...] [--out <file>]\n";
}

int cmd_logic(const InstallReader &p_reader, const std::shared_ptr<Install> &p_install, const std::vector<std::string> &p_args) {
	if (!p_args.empty() && p_args[0] == "census") {
		return cmd_census(p_reader, p_install, std::vector<std::string>(p_args.begin() + 1, p_args.end()));
	}
	if (p_args.size() < 3) {
		std::fprintf(stderr, "%s", logic_usage());
		return 2;
	}
	const std::string &sub = p_args[0];
	const std::string &level_id = p_args[1];
	const std::string &mode = p_args[2];
	bool client = false, with_edges = false;
	std::vector<std::string> rest;
	for (size_t i = 3; i < p_args.size(); ++i) {
		if (p_args[i] == "--client") {
			client = true;
		} else if (p_args[i] == "--edges") {
			with_edges = true;
		} else {
			rest.push_back(p_args[i]);
		}
	}
	if (sub == "fields") {
		Result<std::unique_ptr<level::LoadedLevel>> loaded = level::load_level(p_install, level_id, mode);
		if (!loaded) {
			std::fprintf(stderr, "logic fields: %s\n", loaded.error().message.c_str());
			return 1;
		}
		return cmd_fields(**loaded, mode, rest);
	}
	Result<Loaded> loaded = load_graph(p_install, level_id, mode, client ? LogicSide::Client : LogicSide::Server);
	if (!loaded) {
		std::fprintf(stderr, "logic %s: %s\n", sub.c_str(), loaded.error().message.c_str());
		return 1;
	}
	std::fprintf(stderr, "logic %s %s: load %.3f s, build %.3f s\n", level_id.c_str(), mode.c_str(), loaded->load_s, loaded->build_s);
	if (sub == "stats") {
		print_stats(*loaded->graph);
		return 0;
	}
	if (sub == "instantiate") {
		return cmd_instantiate(*loaded->graph, with_edges);
	}
	if (sub == "ports") {
		return cmd_ports(*loaded->graph);
	}
	if (sub == "guids") {

		const LogicGraph &g = *loaded->graph;
		for (const std::string &prefix : rest) {
			for (uint32_t i = 0; i < g.nodes().size(); ++i) {
				if (guid8(g.nodes()[i].guid).rfind(prefix, 0) == 0) {
					std::printf("node\t%u\t%s\t%s\t%s\n", i, g.nodes()[i].guid.to_string().c_str(), std::string(g.text(g.nodes()[i].type_name)).c_str(),
							scope_chain(g, g.nodes()[i].scope).c_str());
				}
			}
			for (uint32_t s = 0; s < g.scopes().size(); ++s) {
				if (!g.scopes()[s].reference.is_zero() && guid8(g.scopes()[s].reference).rfind(prefix, 0) == 0) {
					std::printf("scope\t%u\t%s\t%s\n", s, g.scopes()[s].reference.to_string().c_str(), std::string(g.text(g.scopes()[s].name)).c_str());
				}
			}
		}
		return 0;
	}
	if (sub == "run" || sub == "trace") {
		return cmd_run(*loaded->graph, rest, sub == "trace");
	}
	std::fprintf(stderr, "%s", logic_usage());
	return 2;
}

}
