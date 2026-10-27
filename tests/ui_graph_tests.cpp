#include <doctest/doctest.h>

#include "bf3/core/ebx_db.h"
#include "bf3/core/install.h"
#include "bf3/core/install_snapshot.h"
#include "bf3/core/ui/hud_widgets.h"
#include "bf3/core/ui/ui_graph.h"
#include "bf3/core/ui/ui_input.h"
#include "test_support.h"

#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace bf3::core;
using namespace bf3::core::ui;

namespace {

struct GraphHost final : UiFlowHost {
	std::map<std::string, std::string> data;
	std::vector<std::string> outputs;
	std::vector<std::string> widgets;
	std::vector<UiFlowAction> actions;

	static std::string key_of(const UiDataRef &p_ref) {
		return ui_data_category_leaf(p_ref.category) + "#" + std::to_string(p_ref.key);
	}
	void action(const UiFlowAction &p_action) override { actions.push_back(p_action); }
	std::optional<std::string> data_get(const UiDataRef &p_ref) override {
		const auto it = data.find(key_of(p_ref));
		if (it == data.end()) {
			return std::nullopt;
		}
		return it->second;
	}
	void data_set(const UiDataRef &p_ref, std::string_view p_value, bool) override {
		data[key_of(p_ref)] = std::string(p_value);
	}
	void widget_event(const UiFlowScreen &p_screen, std::string_view p_instance, std::string_view p_query,
			const std::optional<std::string> &) override {
		widgets.push_back(
				ui_data_category_leaf(p_screen.partition) + "." + std::string(p_instance) + " " + std::string(p_query));
	}
	void graph_output(std::string_view p_graph, std::string_view p_output, int32_t, bool p_destroy) override {
		outputs.push_back(ui_data_category_leaf(p_graph) + "." + std::string(p_output) + (p_destroy ? " destroy" : ""));
	}
};

std::vector<std::string> flow_steps(const std::vector<std::string> &p_log) {
	std::vector<std::string> out;
	for (const std::string &l : p_log) {
		for (const std::string_view p :
				{ "push ", "show ", "hide ", "focus ", "close ", "action ", "output ", "concept " }) {
			if (l.starts_with(p)) {
				out.push_back(l);
				break;
			}
		}
	}
	return out;
}

std::string joined(const std::vector<std::string> &p_lines) {
	std::string s;
	for (const std::string &l : p_lines) {
		s += "  " + l + "\n";
	}
	return s;
}

UiGraphPort port_of(UiGraphPortRole p_role, std::string p_name) {
	UiGraphPort p;
	p.role = p_role;
	p.name = std::move(p_name);
	return p;
}

UiGraphNode node_of(UiGraphNodeKind p_kind, std::string p_name, std::vector<UiGraphPort> p_ports) {
	UiGraphNode n;
	n.kind = p_kind;
	n.name = std::move(p_name);
	n.ports = std::move(p_ports);
	return n;
}

struct SyntheticGraphs {
	std::map<std::string, std::shared_ptr<const UiGraphAsset>> by_name;
	UiGraphProvider provider() {
		return [this](std::string_view p_name) -> std::shared_ptr<const UiGraphAsset> {
			const auto it = by_name.find(std::string(p_name));
			return it == by_name.end() ? nullptr : it->second;
		};
	}
};

std::shared_ptr<const UiGraphAsset> synthetic_flow() {
	auto g = std::make_shared<UiGraphAsset>();
	g->partition = "test/flow";
	g->type = "UIGraphAsset";
	g->nodes.push_back(node_of(UiGraphNodeKind::InstanceInput, "Enter", { port_of(UiGraphPortRole::Out, "") }));
	UiGraphNode action = node_of(UiGraphNodeKind::Action, "GameInput",
			{ port_of(UiGraphPortRole::In, ""), port_of(UiGraphPortRole::Out, "") });
	action.action_key = ui_action_key("GameInput");
	action.params = { "False" };
	g->nodes.push_back(action);
	UiGraphNode cmp = node_of(UiGraphNodeKind::ComparisonLogic, "Mode",
			{ port_of(UiGraphPortRole::In, ""), port_of(UiGraphPortRole::Output, "default"),
					port_of(UiGraphPortRole::Output, "TeamDeathMatch") });
	cmp.data = { "ui/uicomponents/uisettingscomp", "", 7 };
	g->nodes.push_back(cmp);
	UiGraphNode state = node_of(UiGraphNodeKind::State, "Screen",
			{ port_of(UiGraphPortRole::In, ""), port_of(UiGraphPortRole::Show, ""), port_of(UiGraphPortRole::Hide, ""),
					port_of(UiGraphPortRole::Output, "Go") });
	state.screen = "test/screen";
	UiGraphPort concept_port = port_of(UiGraphPortRole::Output, "Deactivate [Input]");
	concept_port.input_event = "UIInputAction_Deactivate";
	concept_port.input_action = 7;
	state.ports.push_back(concept_port);
	g->nodes.push_back(state);
	UiGraphNode set = node_of(UiGraphNodeKind::DataSet, "IsInMenu",
			{ port_of(UiGraphPortRole::In, ""), port_of(UiGraphPortRole::Out, "") });
	set.data = { "ui/uicomponents/usercomp", "", 9 };
	set.param = "False";
	g->nodes.push_back(set);
	UiGraphNode out = node_of(UiGraphNodeKind::InstanceOutput, "Done", { port_of(UiGraphPortRole::In, "") });
	out.destroy_graph = true;
	g->nodes.push_back(out);
	g->nodes.push_back(node_of(UiGraphNodeKind::State, "Other", { port_of(UiGraphPortRole::In, "") }));
	g->nodes.back().screen = "test/other";
	g->connections = {
		{ 0, 0, 1, 0, 0 },
		{ 1, 1, 2, 0, 0 },
		{ 2, 2, 3, 0, 0 },
		{ 2, 1, 6, 0, 0 },
		{ 3, 4, 4, 0, 0 },
		{ 4, 1, 5, 0, 0 },
		{ 3, 3, 6, 0, 1 },
	};
	return g;
}

std::shared_ptr<const UiGraphAsset> synthetic_screen() {
	auto g = std::make_shared<UiGraphAsset>();
	g->partition = "test/screen";
	g->type = "UIScreenAsset";
	UiGraphNode w = node_of(UiGraphNodeKind::Widget, "Button", {});
	w.instance = "Button_01";
	UiGraphPort released = port_of(UiGraphPortRole::Output, "Released");
	released.query = "UIWidgetEventID_OnItemReleased";
	w.ports.push_back(released);
	UiGraphPort hide = port_of(UiGraphPortRole::Input, "Hide");
	hide.query = "UIWidgetEventID_Hide";
	w.ports.push_back(hide);
	g->nodes.push_back(w);
	g->nodes.push_back(node_of(UiGraphNodeKind::InstanceOutput, "Go", { port_of(UiGraphPortRole::In, "") }));
	g->connections = { { 0, 0, 0, 1, 0 }, { 0, 0, 1, 0, 0 } };
	return g;
}

TEST_SUITE("ui-graph-unit") {
	TEST_CASE("ui graph: action keys are the djb2-xor name hash") {
		CHECK(ui_action_key("GameInput") == -368863171);
		CHECK(ui_action_key("DefragMemory") == 788197175);
		const UiActionTable t = UiActionTable::of({ "GameInput", "DefragMemory" });
		CHECK(t.name_of(-368863171) == std::optional<std::string_view>("GameInput"));
		CHECK(t.name_of(788197175) == std::optional<std::string_view>("DefragMemory"));
		CHECK_FALSE(t.name_of(1).has_value());
	}

	TEST_CASE("ui graph: truthy values") {
		CHECK(ui_flow_truthy("True"));
		CHECK(ui_flow_truthy("1"));
		CHECK(ui_flow_truthy("0.35"));
		CHECK(ui_flow_truthy("win32"));
		CHECK_FALSE(ui_flow_truthy("False"));
		CHECK_FALSE(ui_flow_truthy("0"));
		CHECK_FALSE(ui_flow_truthy(""));
		CHECK(ui_data_category_leaf("ui/uicomponents/uisettingscomp") == "uisettingscomp");
	}

	TEST_CASE("ui graph: a synthetic flow pushes, focuses, routes concepts and pops") {
		SyntheticGraphs graphs;
		graphs.by_name["test/flow"] = synthetic_flow();
		graphs.by_name["test/screen"] = synthetic_screen();
		const UiActionTable actions = UiActionTable::of({ "GameInput" });
		GraphHost host;
		host.data["uisettingscomp#7"] = "TeamDeathMatch";
		UiFlow flow(host, graphs.provider(), &actions);
		REQUIRE(flow.fire_input("test/flow", "Enter"));
		CHECK(flow_steps(flow.log()) ==
				std::vector<std::string>{ "action GameInput(False)", "push screen", "show screen", "focus screen" });
		REQUIRE(flow.screens().size() == 1);
		const uint64_t id = flow.screens()[0].id;
		CHECK(flow.focused() == id);
		CHECK(flow.take_opened() == std::vector<uint64_t>{ id });
		flow.clear_log();
		REQUIRE(flow.widget_fired(id, "Button_01", "UIWidgetEventID_OnItemReleased"));
		CHECK(host.widgets == std::vector<std::string>{ "screen.Button_01 UIWidgetEventID_Hide" });
		CHECK(flow_steps(flow.log()) ==
				std::vector<std::string>{
						"output screen.Go", "close screen", "push other", "show other", "focus other" });
		CHECK(flow.take_closing() == std::vector<uint64_t>{ id });
		flow.screen_exited(id);
		CHECK(flow.screen(id) == nullptr);
		flow.clear_log();
		CHECK_FALSE(flow.input_action(7));
		host.data["uisettingscomp#7"] = "Rush";
		REQUIRE(flow.fire_input("test/flow", "Enter"));
		CHECK(flow_steps(flow.log()) == std::vector<std::string>{ "action GameInput(False)" });
		CHECK(flow.errors().empty());
	}

	TEST_CASE("ui graph: an ActionNode sends its Params, with the incoming value appended when AppendIncomingParams "
			  "is set or Params is empty") {
		auto g = std::make_shared<UiGraphAsset>();
		g->partition = "test/actions";
		g->type = "UIGraphAsset";
		g->nodes.push_back(node_of(UiGraphNodeKind::InstanceInput, "Send", { port_of(UiGraphPortRole::Out, "") }));
		for (const std::string_view name : { "GameInput", "PostProcessEffect", "SelectSpawnPoint" }) {
			UiGraphNode a = node_of(UiGraphNodeKind::Action, std::string(name),
					{ port_of(UiGraphPortRole::In, ""), port_of(UiGraphPortRole::Out, "") });
			a.action_key = ui_action_key(name);
			g->nodes.push_back(a);
		}
		g->nodes[1].params = { "All" };
		g->nodes[1].append_params = true;
		g->nodes[2].params = { "0.35" };
		g->connections = { { 0, 0, 1, 0, 0 }, { 1, 1, 2, 0, 0 }, { 2, 1, 3, 0, 0 } };
		SyntheticGraphs graphs;
		graphs.by_name["test/actions"] = g;
		const UiActionTable actions = UiActionTable::of({ "GameInput", "PostProcessEffect", "SelectSpawnPoint" });
		GraphHost host;
		UiFlow flow(host, graphs.provider(), &actions);
		const auto sent = [&host] {
			std::vector<std::vector<std::string>> out;
			for (const UiFlowAction &a : host.actions) {
				out.push_back(a.params);
			}
			host.actions.clear();
			return out;
		};
		REQUIRE(flow.fire_input("test/actions", "Send", "hello"));
		CHECK(sent() == std::vector<std::vector<std::string>>{ { "All", "hello" }, { "0.35" }, { "hello" } });
		REQUIRE(flow.fire_input("test/actions", "Send"));
		CHECK(sent() == std::vector<std::vector<std::string>>{ { "All" }, { "0.35" }, {} });
		CHECK(flow.errors().empty());
	}

	TEST_CASE("ui graph: a concept on the focused screen follows its input-event port") {
		SyntheticGraphs graphs;
		graphs.by_name["test/flow"] = synthetic_flow();
		graphs.by_name["test/screen"] = synthetic_screen();
		GraphHost host;
		host.data["uisettingscomp#7"] = "TeamDeathMatch";
		UiFlow flow(host, graphs.provider());
		REQUIRE(flow.fire_input("test/flow", "Enter"));
		flow.clear_log();
		CHECK_FALSE(flow.input_action(6));
		REQUIRE(flow.input_action(7));
		CHECK(flow_steps(flow.log()) ==
				std::vector<std::string>{
						"concept screen UIInputAction_Deactivate", "output flow.Done", "close screen" });
		CHECK(host.data["usercomp#9"] == "False");
		CHECK(host.outputs == std::vector<std::string>{ "flow.Done destroy" });
		CHECK_FALSE(flow.graph_live("test/flow"));
		CHECK_FALSE(flow.focused().has_value());
		CHECK(flow.errors().size() == 1);
	}

	TEST_CASE("ui graph: a cycle stops at the step budget") {
		auto g = std::make_shared<UiGraphAsset>();
		g->partition = "test/loop";
		g->nodes.push_back(node_of(UiGraphNodeKind::InstanceInput, "Enter", { port_of(UiGraphPortRole::Out, "") }));
		g->nodes.push_back(node_of(UiGraphNodeKind::Splitter, "Split",
				{ port_of(UiGraphPortRole::In, ""), port_of(UiGraphPortRole::Output, "0"),
						port_of(UiGraphPortRole::Output, "1") }));
		g->connections = { { 0, 0, 1, 0, 0 }, { 1, 1, 1, 0, 0 }, { 1, 2, 1, 0, 0 } };
		SyntheticGraphs graphs;
		graphs.by_name["test/loop"] = g;
		GraphHost host;
		UiFlow flow(host, graphs.provider());
		CHECK(flow.fire_input("test/loop", "Enter"));
		CHECK(flow.step_overflows() > 0);
		CHECK(flow.steps() <= kUiFlowMaxStepsPerCall);
	}
}

struct RetailGraphs {
	bf3::test::Mounted *mount = nullptr;
	std::unique_ptr<EbxLibrary> level;
	std::optional<UiActionTable> actions;
	std::map<std::string, std::shared_ptr<const UiGraphAsset>> cache;
	std::vector<std::string> read_errors;
	std::string error;

	UiGraphProvider provider() {
		return [this](std::string_view p_name) -> std::shared_ptr<const UiGraphAsset> {
			const std::string name(p_name);
			if (const auto it = cache.find(name); it != cache.end()) {
				return it->second;
			}
			Result<UiGraphAsset> g = read_ui_graph_asset(*level, name);
			std::shared_ptr<const UiGraphAsset> made;
			if (g) {
				made = std::make_shared<const UiGraphAsset>(std::move(*g));
			} else {
				read_errors.push_back(g.error().message);
			}
			cache[name] = made;
			return made;
		};
	}
};

RetailGraphs *retail_graphs_or_skip() {
	static RetailGraphs r;
	static bool tried = false;
	if (!tried) {
		tried = true;
		r.mount = bf3::test::shared_mount_or_skip();
		if (r.mount == nullptr) {
			return nullptr;
		}
		Result<std::unique_ptr<EbxLibrary>> level =
				EbxLibrary::open(r.mount->install, { "Win32/Levels/MP_Subway/MP_Subway" });
		if (!level) {
			r.error = level.error().message;
			return &r;
		}
		r.level = std::move(*level);
		Result<UiActionTable> actions = UiActionTable::read(*r.level);
		if (!actions) {
			r.error = actions.error().message;
			return &r;
		}
		r.actions = std::move(*actions);
	} else if (r.mount == nullptr) {
		bf3::test::skip_install_case("no install");
		return nullptr;
	}
	REQUIRE_MESSAGE(r.error.empty(), r.error);
	return &r;
}

constexpr std::string_view kKilledGraph = "ui/flow/graph/ingame/killedgraph";
constexpr std::string_view kSpawnGraphPc = "ui/flow/graph/spawn/spawngraphpc";
constexpr std::string_view kSpawnButtonGraph = "ui/flow/graph/spawn/spawnbuttongraph";
constexpr std::string_view kScoreboardGraph = "ui/flow/graph/ingame/mp/ingamescoreboardgraph";
constexpr std::string_view kPreRoundWaitingGraph = "ui/flow/graph/preround/preroundwaitinggraph";
constexpr std::string_view kPreEndOfRoundGraph = "ui/flow/graph/endofround/preendofroundgraph";
constexpr std::string_view kEndOfRoundGraph = "ui/flow/graph/endofround/endofroundgraph";
constexpr std::string_view kIngameMenuGraph = "ui/flow/graph/ingame/mp/ingamemenumpgraph";

constexpr std::string_view kRetailGraphs[] = { kKilledGraph, kSpawnGraphPc, kSpawnButtonGraph, kScoreboardGraph,
	kPreRoundWaitingGraph, kPreEndOfRoundGraph, kEndOfRoundGraph, kIngameMenuGraph };

void tdm_pc_data(GraphHost &r_host) {
	r_host.data["uisettingscomp#-668821714"] = "win32";
	r_host.data["uisettingscomp#1733092213"] = "TeamDeathMatch0";
	r_host.data["uisettingscomp#1870459195"] = "TeamDeathMatch";
	r_host.data["uispawnpointcomp#-990983020"] = "True";
	r_host.data["uispawnpointcomp#-225037516"] = "True";
	r_host.data["uispectatorcomp#-366910738"] = "False";
	r_host.data["uiendofroundcomp#-1857162541"] = "False";
	r_host.data["uiendofroundcomp#1450248605"] = "False";
	r_host.data["uiendofroundcomp#-603546634"] = "False";
	r_host.data["uisoldiercomp#347110003"] = "True";
	r_host.data["uikitcomp#1985842909"] = "0";
	r_host.data["uispawnlogiccomp#-1005581981"] = "0";
}

struct Run {
	GraphHost host;
	UiFlow flow;
	explicit Run(RetailGraphs &r_graphs) : flow(host, r_graphs.provider(), &*r_graphs.actions) { tdm_pc_data(host); }

	std::vector<std::string> take(const char *p_label) {
		const std::vector<std::string> steps = flow_steps(flow.log());
		std::printf("ui.graph %s\n%s", p_label, joined(flow.log()).c_str());
		flow.clear_log();
		return steps;
	}
	uint64_t screen_of(std::string_view p_partition) {
		const UiFlowScreen *s = flow.find_screen(p_partition);
		REQUIRE_MESSAGE(s != nullptr, std::string(p_partition));
		return s->id;
	}
	void initialize_opened() {
		for (const uint64_t id : flow.take_opened()) {
			flow.screen_initialized(id);
		}
	}
};

int32_t input_action_of(const UiGraphAsset &p_graph, std::string_view p_event) {
	for (const UiGraphNode &n : p_graph.nodes) {
		for (const UiGraphPort &p : n.ports) {
			if (p.input_event == p_event) {
				return p.input_action;
			}
		}
	}
	return -1;
}

using Lines = std::vector<std::string>;

TEST_SUITE("ui-graph") {
	TEST_CASE("ui graph: the eight in-round graphs read and every action key is in veniceuiactions") {
		RetailGraphs *r = retail_graphs_or_skip();
		if (r == nullptr) {
			return;
		}
		CHECK(r->actions->size() == 197);
		CHECK(r->actions->names()[4] == "GameInput");
		for (const std::string_view name : kRetailGraphs) {
			const std::shared_ptr<const UiGraphAsset> g = r->provider()(name);
			REQUIRE_MESSAGE(g != nullptr, std::string(name));
			size_t action_nodes = 0;
			for (const UiGraphNode &n : g->nodes) {
				CHECK_MESSAGE(n.kind != UiGraphNodeKind::Unknown, (std::string(name) + " " + n.type));
				if (n.kind == UiGraphNodeKind::Action) {
					++action_nodes;
					CHECK_MESSAGE(r->actions->name_of(n.action_key).has_value(), (std::string(name) + " " + n.name));
				}
				if (n.kind == UiGraphNodeKind::InstanceOutput) {
					CHECK_MESSAGE(n.output_id == ui_action_key(n.name), (std::string(name) + " " + n.name));
				}
			}
			std::printf("ui.graph read %s nodes %zu connections %zu actions %zu by-name %u duplicate %u unlisted %u\n",
					std::string(name).c_str(), g->nodes.size(), g->connections.size(), action_nodes,
					g->named_port_connections, g->duplicate_connections, g->unlisted_port_connections);
			CHECK(g->unlisted_port_connections == (name == kIngameMenuGraph ? 1u : 0u));
			CHECK(!g->nodes.empty());
			CHECK(!g->connections.empty());
		}
		CHECK(r->read_errors.empty());
	}

	TEST_CASE("ui graph: killedgraph Death") {
		RetailGraphs *r = retail_graphs_or_skip();
		if (r == nullptr) {
			return;
		}
		Run run(*r);
		REQUIRE(run.flow.fire_input(kKilledGraph, "Death"));
		CHECK(run.take("killed.Death") ==
				Lines{ "action GameInput(False)", "push killscreen", "show killscreen", "focus killscreen" });
		CHECK(run.flow.errors().empty());
	}

	TEST_CASE("ui graph: playingminimapinit MapZoom increments and steps the zoom") {
		RetailGraphs *r = retail_graphs_or_skip();
		if (r == nullptr) {
			return;
		}
		constexpr std::string_view graph = "ui/flow/graph/ingame/playingminimapinit";
		Run run(*r);
		REQUIRE(run.flow.fire_input(graph, "Initialized"));
		CHECK(run.host.data["uiminimapcomp#-1358262034"] == "True");
		CHECK(run.host.data["uiminimapcomp#1404427580"] == "0");
		std::vector<std::string> zoom;
		std::vector<std::string> level;
		for (int i = 0; i < 4; ++i) {
			REQUIRE(run.flow.fire_input(graph, "MapZoom"));
			zoom.push_back(run.host.data["uiminimapcomp#1404427580"]);
			level.push_back(run.host.data["uiminimapcomp#-1127323954"]);
		}
		CHECK(zoom == Lines{ "0.4", "1.2", "0", "0.4" });
		CHECK(level == Lines{ "1", "2", "0", "1" });
		CHECK(run.host.data["uiminimapcomp#-1095821268"].empty());
		run.take("playingminimapinit.MapZoom");
		CHECK(run.flow.errors().empty());
	}

	TEST_CASE("ui graph: spawngraphpc EnterSpawn and deploy") {
		RetailGraphs *r = retail_graphs_or_skip();
		if (r == nullptr) {
			return;
		}
		Run run(*r);
		REQUIRE(run.flow.fire_input(kSpawnGraphPc, "EnterSpawn"));
		CHECK(run.take("spawngraphpc.EnterSpawn") ==
				Lines{ "action SelectSpawnPoint(-1) <0", "action GameInput(False) <0", "action MouseInput(True) <0",
						"action OnFrontendCameraChanged(0.35) <0", "push spawnscreenpc", "show spawnscreenpc",
						"focus spawnscreenpc" });
		run.initialize_opened();
		run.take("spawngraphpc.initialized");
		const int32_t activate = input_action_of(*r->provider()(kSpawnGraphPc), "UIInputAction_Activate");
		const int32_t deactivate = input_action_of(*r->provider()(kSpawnGraphPc), "UIInputAction_Deactivate");
		std::printf("ui.graph UIInputAction_Activate %d UIInputAction_Deactivate %d\n", activate, deactivate);
		REQUIRE(run.flow.input_action(activate));
		CHECK(run.take("spawngraphpc.Activate") ==
				Lines{ "concept spawnscreenpc UIInputAction_Activate", "action OnFrontendCameraChanged(0.35) <0",
						"action Spawn() <0" });
		CHECK(run.flow.errors().empty());
	}

	TEST_CASE("ui graph: spawngraphpc's ReselectSpawn selects no spawn point and a List_01 highlight selects the "
			  "highlighted row's point") {
		RetailGraphs *r = retail_graphs_or_skip();
		if (r == nullptr) {
			return;
		}
		Run run(*r);
		const auto selections = [&run] {
			std::vector<std::optional<int32_t>> out;
			for (const UiFlowAction &a : run.host.actions) {
				if (a.key == kSelectSpawnPointActionKey) {
					out.push_back(hud_selected_spawn_point(a.params));
				}
			}
			run.host.actions.clear();
			return out;
		};
		REQUIRE(run.flow.fire_input(kSpawnGraphPc, "EnterSpawn"));
		run.take("spawngraphpc.EnterSpawn");
		CHECK(selections() == std::vector<std::optional<int32_t>>{ -1 });
		run.initialize_opened();
		run.take("spawngraphpc.initialized");
		run.host.actions.clear();
		REQUIRE(run.flow.widget_fired(
				run.screen_of("ui/flow/screen/spawnscreenpc"), "List_01", "UIWidgetEventID_OnItemHighlighted", "2"));
		CHECK(run.take("spawnscreenpc.List_01 highlighted") ==
				Lines{ "output spawnscreenpc.SpawnSelected", "action SelectSpawnPoint() <2" });
		CHECK(selections() == std::vector<std::optional<int32_t>>{ 2 });
		CHECK(run.flow.errors().empty());
	}

	TEST_CASE("ui graph: spawngraphpc Deactivate leaves for the ingame menu") {
		RetailGraphs *r = retail_graphs_or_skip();
		if (r == nullptr) {
			return;
		}
		Run run(*r);
		REQUIRE(run.flow.fire_input(kSpawnGraphPc, "EnterSpawn"));
		run.take("spawngraphpc.EnterSpawn");
		REQUIRE(run.flow.input_action(input_action_of(*r->provider()(kSpawnGraphPc), "UIInputAction_Deactivate")));
		CHECK(run.take("spawngraphpc.Deactivate") ==
				Lines{ "concept spawnscreenpc UIInputAction_Deactivate", "output spawngraphpc.EnterIngameMenu",
						"close spawnscreenpc" });
		CHECK(run.host.outputs == Lines{ "spawngraphpc.EnterIngameMenu destroy" });
		CHECK_FALSE(run.flow.graph_live(kSpawnGraphPc));
	}

	TEST_CASE("ui graph: spawnbuttongraph StartSpawnButtonScreen, deploy and Spawned") {
		RetailGraphs *r = retail_graphs_or_skip();
		if (r == nullptr) {
			return;
		}
		Run run(*r);
		REQUIRE(run.flow.fire_input(kSpawnButtonGraph, "StartSpawnButtonScreen"));
		CHECK(run.take("spawnbutton.StartSpawnButtonScreen") ==
				Lines{ "push spawnbuttonscreen", "show spawnbuttonscreen", "focus spawnbuttonscreen" });
		const uint64_t screen = run.screen_of("ui/flow/screen/spawnbuttonscreen");
		REQUIRE(run.flow.widget_fired(screen, "SpawnTimer_01", "UIWidgetEventID_OnItemReleased"));
		CHECK(run.take("spawnbutton.Deploy") ==
				Lines{ "output spawnbuttonscreen.Spawn", "action SetCustomization(1)",
						"action SetWeaponCustomization(1)", "action StorePrimaryWeaponAccessories(1)",
						"action StoreVehicleAccessories(1)", "action Spawn() <0" });
		REQUIRE(run.flow.fire_input(kSpawnButtonGraph, "Spawned"));
		CHECK(run.take("spawnbutton.Spawned") ==
				Lines{ "action UnSpawnCustomizationVehicle() <0", "action UnSpawnCustomization() <0",
						"action OnFrontendCameraChanged(0.35) <0", "output spawnbuttongraph.ExitCustomIzation",
						"close spawnbuttonscreen" });
		CHECK(run.host.outputs == Lines{ "spawnbuttongraph.ExitCustomIzation destroy" });
		CHECK(run.flow.errors().empty());
	}

	TEST_CASE("ui graph: ingamescoreboardgraph startup and the tab listener") {
		RetailGraphs *r = retail_graphs_or_skip();
		if (r == nullptr) {
			return;
		}
		Run run(*r);
		REQUIRE(run.flow.fire_input(kScoreboardGraph, "Startup and hide"));
		CHECK(run.take("scoreboard.Startup") == Lines{ "push scoreboardtwoteamshud32screen" });
		run.initialize_opened();
		CHECK(run.take("scoreboard.initialized") ==
				Lines{ "push tablistenerscreen", "show tablistenerscreen", "focus tablistenerscreen" });
		run.initialize_opened();
		run.take("scoreboard.initialized2");
		const uint64_t listener = run.screen_of("ui/flow/screen/tablistenerscreen");
		REQUIRE(run.flow.widget_fired(listener, "InputListener_01", "UIWidgetEventID_OnItemPressed"));
		CHECK(run.take("scoreboard.TabPressed") ==
				Lines{ "output tablistenerscreen.TabPressed", "show scoreboardtwoteamshud32screen" });
		REQUIRE(run.flow.widget_fired(listener, "InputListener_01", "UIWidgetEventID_OnItemReleased"));
		CHECK(run.take("scoreboard.TabReleased") ==
				Lines{ "output tablistenerscreen.TabReleased", "hide scoreboardtwoteamshud32screen" });
		CHECK(run.flow.errors().empty());
	}

	TEST_CASE("ui graph: preroundwaitinggraph Waiting, Countdown and Restarting") {
		RetailGraphs *r = retail_graphs_or_skip();
		if (r == nullptr) {
			return;
		}
		for (const std::string_view input : { "Waiting", "Countdown", "Restarting" }) {
			Run run(*r);
			REQUIRE(run.flow.fire_input(kPreRoundWaitingGraph, input));
			const std::string label = "preroundwaiting." + std::string(input);
			const Lines expected{ "push preroundwaitingscreen", "show preroundwaitingscreen",
				"focus preroundwaitingscreen" };
			CHECK_MESSAGE(run.take(label.c_str()) == expected, label);
			CHECK(run.host.widgets == Lines{ "preroundwaitingscreen.HudAlertMessage_03 UIWidgetEventID_Show" });
			CHECK(run.flow.errors().empty());
		}
	}

	TEST_CASE("ui graph: preendofroundgraph WinningTeam and Scoreboard") {
		RetailGraphs *r = retail_graphs_or_skip();
		if (r == nullptr) {
			return;
		}
		Run run(*r);
		REQUIRE(run.flow.fire_input(kPreEndOfRoundGraph, "WinningTeam"));
		CHECK(run.take("preendofround.WinningTeam") ==
				Lines{ "action OnFrontendCameraChanged(0.35) <1", "push eorwinningteamscreen",
						"show eorwinningteamscreen", "focus eorwinningteamscreen" });
		REQUIRE(run.flow.fire_input(kPreEndOfRoundGraph, "Scoreboard"));
		CHECK(run.take("preendofround.Scoreboard") ==
				Lines{ "action OnFrontendCameraChanged(0.2) <-1", "push scoreboardtwoteamshud64screen",
						"show scoreboardtwoteamshud64screen", "focus scoreboardtwoteamshud64screen" });
		CHECK(run.flow.errors().empty());
	}

	TEST_CASE("ui graph: endofroundgraph EndOfRound and quit") {
		RetailGraphs *r = retail_graphs_or_skip();
		if (r == nullptr) {
			return;
		}
		Run run(*r);
		REQUIRE(run.flow.fire_input(kEndOfRoundGraph, "EndOfRound"));
		CHECK(run.take("endofround.EndOfRound") ==
				Lines{ "action OnFrontendCameraChanged(0.35) <False", "action MouseInput(True) <False",
						"action GameInput(False) <False", "push eordetailstabsscreen", "show eordetailstabsscreen",
						"focus eordetailstabsscreen" });
		run.initialize_opened();
		CHECK(run.take("endofround.initialized") ==
				Lines{ "action OnFrontendCameraChanged(0.35) <True", "push eordetailsscoreboardscreen",
						"show eordetailsscoreboardscreen", "focus eordetailsscoreboardscreen" });
		run.initialize_opened();
		CHECK(run.take("endofround.initialized2") ==
				Lines{ "push scoreboardtwoteamsscreen", "show scoreboardtwoteamsscreen",
						"focus scoreboardtwoteamsscreen" });
		REQUIRE(run.flow.input_action(input_action_of(*r->provider()(kEndOfRoundGraph), "UIInputAction_Deactivate")));
		CHECK(run.take("endofround.Deactivate") ==
				Lines{ "concept eordetailstabsscreen UIInputAction_Deactivate", "push popupgeneric",
						"show popupgeneric", "focus popupgeneric" });
		CHECK(run.flow.errors().empty());
	}

	TEST_CASE("ui graph: ingamemenumpgraph EnterFromGame, resume and quit") {
		RetailGraphs *r = retail_graphs_or_skip();
		if (r == nullptr) {
			return;
		}
		Run run(*r);
		REQUIRE(run.flow.fire_input(kIngameMenuGraph, "EnterFromGame"));
		CHECK(run.take("ingamemenu.EnterFromGame") ==
				Lines{ "action MouseInput(False) <1", "action ActivateCommoRose(False) <1",
						"action GameInput(False) <1", "action MouseCursor(1) <1", "action MouseInput(True) <1",
						"push ingamemenump", "show ingamemenump", "focus ingamemenump", "action SetSelectedLevel()",
						"action RestrictMouse(False) <1" });
		run.initialize_opened();
		run.take("ingamemenu.initialized");
		const uint64_t menu = run.screen_of("ui/flow/screen/ingamemenump");
		REQUIRE(run.flow.widget_fired(menu, "List_01", "UIWidgetEventID_OnItemReleased", "ID_M_IGMMP_QUIT"));
		CHECK(run.take("ingamemenu.Quit") ==
				Lines{ "output ingamemenump.RowSelected", "push popupgeneric", "show popupgeneric",
						"focus popupgeneric" });
		const UiFlowScreen *popup = run.flow.find_screen("ui/flow/screen/popups/popupgeneric");
		REQUIRE(popup != nullptr);
		CHECK(popup->dialog);
		CHECK(popup->modal);
		CHECK(popup->dialog_title == "ID_M_POPUP_QUIT_TO_WINDOWS");
		CHECK(popup->dialog_text == "ID_M_POPUP_QUIT_TEXT");
		CHECK(popup->dialog_buttons ==
				std::vector<UiDialogButton>{ { "ID_M_POPUP_OK", int32_t(UiInputConcept::Activate) },
						{ "ID_M_POPUP_CANCEL", int32_t(UiInputConcept::Deactivate) } });
		run.flow.clear_log();
		Run resume(*r);
		REQUIRE(resume.flow.fire_input(kIngameMenuGraph, "EnterFromGame"));
		resume.take("ingamemenu.EnterFromGame");
		REQUIRE(resume.flow.widget_fired(resume.screen_of("ui/flow/screen/ingamemenump"), "List_01",
				"UIWidgetEventID_OnItemReleased", "ID_M_IGMMP_RESUME"));
		CHECK(resume.take("ingamemenu.Resume") ==
				Lines{ "output ingamemenump.RowSelected", "action MouseInput(False) <ID_M_IGMMP_RESUME",
						"action GameInput(True) <ID_M_IGMMP_RESUME",
						"action OnFrontendCameraChanged(0.35) <ID_M_IGMMP_RESUME",
						"output ingamemenumpgraph.exitIngameMenuMP", "close ingamemenump" });
		CHECK(resume.host.outputs == Lines{ "ingamemenumpgraph.exitIngameMenuMP destroy" });
		CHECK(resume.flow.errors().empty());
	}

	TEST_CASE("ui graph: install unchanged") {
		RetailGraphs *r = retail_graphs_or_skip();
		if (r == nullptr) {
			return;
		}
		const SnapshotDiff diff = diff_snapshots(r->mount->before, r->mount->reader.snapshot());
		std::printf("ui.graph install.unchanged %s\n", diff.clean() ? "yes" : diff.summary().c_str());
		CHECK(diff.clean());
	}
}

}
