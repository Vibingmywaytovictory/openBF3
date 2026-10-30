#include "bf3/host/ui_flow_session.h"

#include "bf3/core/gameplay/logic_types.h"
#include "bf3/core/gameplay/logic_world.h"
#include "bf3/core/level/level.h"
#include "bf3/core/util/finite.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace bf3::host {

namespace gp = core::gameplay;
namespace lg = sim::logic;
namespace ui = core::ui;

namespace {

constexpr int32_t kFadeDone = gp::logic_port_id("OnDone");
constexpr int32_t kOnRoundOver = gp::logic_port_id("OnRoundOver");
constexpr int32_t kTeamField = gp::logic_port_id("Team");
constexpr int32_t kEndOfRoundStart = 298914295;
constexpr int32_t kEndOfRoundPreEor = 1464479596;
constexpr int32_t kEndOfRoundPreEorElapsed = 1904769966;
constexpr int32_t kEndOfRoundRequest = -1283826768;
constexpr int32_t kEndOfRoundAnswer = -1342095349;
constexpr size_t kMaxWatches = 1u << 14;
constexpr size_t kMaxTimers = 4096;

std::string leaf_key(std::string_view p_category, int32_t p_key) {
	return ui::ui_data_category_leaf(p_category) + "#" + std::to_string(p_key);
}
}

int32_t ui_preround_event_id(UiPreRoundEvent p_event) {
	switch (p_event) {
		case UiPreRoundEvent::Waiting:
			return -1338735519;
		case UiPreRoundEvent::Countdown:
			return -72940873;
		case UiPreRoundEvent::RoundStarted:
			return -1598714430;
		case UiPreRoundEvent::Restarting:
			return 989192448;
	}
	return 0;
}

core::Result<std::unique_ptr<UiFlowSession>> UiFlowSession::create(
		const core::level::LoadedLevel &p_level, const UiFlowSessionOptions &p_options) {
	std::unique_ptr<UiFlowSession> s(new UiFlowSession());
	BF3_TRY(s->bind(p_level, p_options));
	return s;
}

UiFlowSession::~UiFlowSession() = default;

core::Result<ClientLogicWorld> build_client_logic(const core::level::LoadedLevel &p_level) {
	gp::LogicGraphOptions go;
	go.mode = p_level.mode;
	go.side = gp::LogicSide::Client;
	gp::LogicWorldOptions wo;
	wo.mode = go.mode;
	wo.expand_root_objects = true;
	wo.max_depth = go.max_depth;
	wo.max_scopes = go.max_scopes;
	wo.max_objects = go.max_objects;
	wo.max_walk_steps = go.max_walk_steps;
	wo.max_query_steps = go.max_query_steps;
	wo.max_work = go.max_work;
	core::Result<gp::LogicWorld> world = gp::LogicWorld::build(p_level.db(), p_level.root(), wo);
	if (!world) {
		return core::error_at("ui flow session: logic walk", world.error());
	}
	core::Result<gp::LogicGraph> graph = gp::build_logic_graph(*world, go);
	if (!graph) {
		return core::error_at("ui flow session: client logic graph", graph.error());
	}
	return ClientLogicWorld{ std::move(*world), std::make_shared<const gp::LogicGraph>(std::move(*graph)) };
}

core::Status UiFlowSession::bind(const core::level::LoadedLevel &p_level, const UiFlowSessionOptions &p_options) {
	options_ = p_options;
	const core::EbxResolver &db = p_level.db();
	core::Result<ClientLogicWorld> logic = build_client_logic(p_level);
	if (!logic) {
		return logic.error();
	}
	graph_ = logic->graph;
	if (core::Result<lg::SoundAreaNatives> areas = lg::SoundAreaNatives::create(logic->world, graph_)) {
		sound_areas_ = std::make_unique<lg::SoundAreaNatives>(std::move(*areas));
	} else {
		errors_.push_back("ui flow session: " + areas.error().message);
	}
	const std::optional<uint32_t> scope = graph_->scope_named(options_.scope);
	if (!scope) {
		return core::make_error(
				"ui flow session: no scope " + options_.scope + " in the client logic of " + p_level.mode);
	}
	scope_ = *scope;

	core::Result<ui::UiActionTable> table = ui::UiActionTable::read(db);
	if (table) {
		actions_table_ = std::move(*table);
	}
	provider_ = [this, &db](std::string_view p_name) -> std::shared_ptr<const ui::UiGraphAsset> {
		if (const auto it = graphs_.find(p_name); it != graphs_.end()) {
			return it->second;
		}
		core::Result<ui::UiGraphAsset> g = ui::read_ui_graph_asset(db, p_name);
		std::shared_ptr<const ui::UiGraphAsset> made;
		if (g) {
			made = std::make_shared<const ui::UiGraphAsset>(std::move(*g));
		} else {
			errors_.push_back(g.error().message);
		}
		graphs_.emplace(std::string(p_name), made);
		return made;
	};
	for (const uint32_t n : graph_->nodes_of_type(gp::LogicTypeId::UIGraph)) {
		const std::optional<core::EbxInstance> inst = logic_node_instance(db, *graph_, n);
		const std::optional<core::EbxInstance> asset = inst ? db.follow(inst->data(), "GraphAsset") : std::nullopt;
		const std::optional<std::string_view> name = asset ? db.name_of(asset->partition().guid()) : std::nullopt;
		if (!name) {
			continue;
		}
		ui_graph_of_[n] = std::string(*name);
		GraphEntity entity;
		entity.graph = std::string(*name);
		const std::optional<core::EbxValue> pop = inst->get("PopPreviousGraph");
		entity.pop_previous = pop && pop->as_bool().value_or(false);
		const std::optional<core::EbxValue> priority = inst->get("GraphPriority");
		const std::optional<std::string_view> priority_name = priority ? priority->enum_name() : std::nullopt;
		entity.priority = std::string(priority_name.value_or(""));
		graph_entities_[n] = std::move(entity);
		provider_(*name);
	}
	core::Result<LogicScreenEffects> effects = LogicScreenEffects::bind(db, *graph_);
	if (!effects) {
		return core::error_at("ui flow session", effects.error());
	}
	effects_ = std::make_unique<LogicScreenEffects>(std::move(*effects));
	for (const uint32_t n : graph_->nodes_of_type(gp::LogicTypeId::UIEndOfRound)) {
		const std::optional<core::EbxInstance> inst = logic_node_instance(db, *graph_, n);
		const std::optional<double> seconds = inst ? logic_node_float(*inst, "PreEorTime") : std::nullopt;
		end_of_round_nodes_.push_back(n);
		pre_eor_seconds_[n] = float(seconds.value_or(0.0));
	}
	for (const uint32_t n : graph_->nodes_of_type(gp::LogicTypeId::PreRound)) {
		pre_round_nodes_.push_back(n);
	}
	for (const uint32_t n : graph_->nodes_of_type(gp::LogicTypeId::RoundOver)) {
		const std::optional<gp::LogicValue> team = graph_->param(n, kTeamField);
		round_over_nodes_.emplace_back(n, team ? team->i : 0);
	}

	const auto watched = [&](uint32_t p_node) {
		return ui_graph_of_.count(p_node) != 0 ||
				std::find(end_of_round_nodes_.begin(), end_of_round_nodes_.end(), p_node) != end_of_round_nodes_.end();
	};
	std::set<std::pair<uint32_t, int32_t>> ports;
	for (const std::span<const gp::LogicEdge> list : { graph_->event_edges(), graph_->networked_edges() }) {
		for (const gp::LogicEdge &e : list) {
			if (e.target != gp::kNoLogicIndex && watched(e.target)) {
				ports.emplace(e.target, e.target_port);
			}
		}
	}
	for (const gp::LogicEntry &e : graph_->entries()) {
		if (watched(e.target)) {
			ports.emplace(e.target, e.target_port);
		}
	}
	for (const std::pair<uint32_t, int32_t> &p : effects_->watch_ports()) {
		ports.insert(p);
	}
	watch_ports_.assign(ports.begin(), ports.end());
	for (const gp::LogicEdge &e : graph_->networked_edges()) {
		if (e.target == gp::kNoLogicIndex) {
			continue;
		}
		std::vector<std::pair<uint32_t, int32_t>> &targets = networked_targets_[e.connection];
		if (std::find(targets.begin(), targets.end(), std::make_pair(e.target, e.target_port)) == targets.end()) {
			targets.emplace_back(e.target, e.target_port);
		}
	}
	stats_.ui_graph_nodes = ui_graph_of_.size();
	stats_.graphs = graphs_.size();
	stats_.watches = watch_ports_.size();
	stats_.fades = effects_->stats().fades;
	stats_.end_of_round_nodes = end_of_round_nodes_.size();
	stats_.pre_round_nodes = pre_round_nodes_.size();
	stats_.round_over_nodes = round_over_nodes_.size();
	return start_logic();
}

core::Status UiFlowSession::start_logic() {
	timers_.clear();
	effects_->reset();
	graph_owner_.clear();
	entered_.clear();
	flow_.reset();
	runtime_.reset();
	lg::LogicRuntimeOptions ro;
	ro.platform = options_.platform;
	ro.max_watches = kMaxWatches;
	ro.immediate_properties = true;
	core::Result<lg::LogicRuntime> rt = lg::LogicRuntime::create(graph_, ro);
	if (!rt) {
		return core::error_at("ui flow session: runtime", rt.error());
	}
	runtime_ = std::make_unique<lg::LogicRuntime>(std::move(*rt));
	if (sound_areas_ != nullptr) {
		sound_areas_->reset();
	}
	for (const auto &[node, port] : watch_ports_) {
		BF3_TRY(runtime_->watch(node, port, lg::LogicWatchKind::EventIn));
	}
	flow_ = std::make_unique<ui::UiFlow>(*this, provider_, actions_table_ ? &*actions_table_ : nullptr);
	core::Result<lg::LogicTickStats> started = runtime_->start();
	if (!started) {
		return core::error_at("ui flow session: runtime start", started.error());
	}
	if (ghost_) {
		if (const core::Status applied = apply_ghost(*ghost_); !applied.ok()) {
			errors_.push_back(applied.error().message);
		}
	}
	return core::Status::success();
}

void UiFlowSession::set_listener(const core::Vec3 &p_position) {
	if (core::finite(p_position)) {
		listener_ = p_position;
	}
}

void UiFlowSession::receive_networked(std::span<const lg::LogicNetworkedEvent> p_events) {
	for (const lg::LogicNetworkedEvent &e : p_events) {
		++stats_.networked_received;
		if (received_.size() >= kUiFlowSessionMaxReceived) {
			++stats_.networked_dropped;
			continue;
		}
		received_.push_back(e);
	}
}

core::Status UiFlowSession::apply_ghost(const lg::LogicGhost &p_ghost) {
	ghost_ = p_ghost;
	const core::Status s = runtime_->apply_ghost(p_ghost);
	if (!s.ok()) {
		++stats_.ghosts_refused;
		return core::error_at("ui flow session: server ghost", s.error());
	}
	++stats_.ghosts_applied;
	return core::Status::success();
}

void UiFlowSession::deliver_networked() {
	const std::vector<lg::LogicNetworkedEvent> received = std::exchange(received_, {});
	for (const lg::LogicNetworkedEvent &ev : received) {
		const auto targets = networked_targets_.find(ev.connection);
		if (targets == networked_targets_.end()) {
			++stats_.networked_unmatched;
			continue;
		}
		for (const auto &[node, port] : targets->second) {
			const core::Status s = runtime_->deliver_event(node, port, ev.context);
			if (!s.ok()) {
				errors_.push_back(s.error().message);
			}
			++stats_.networked_delivered;
		}
	}
	for (const lg::LogicNetworkedEvent &ev : runtime_->networked()) {
		++stats_.networked_local;
		if (!options_.echo_networked) {
			continue;
		}
		for (const gp::LogicEdge &e : graph_->networked_edges()) {
			if (e.source == ev.source && e.source_port == ev.source_port && e.connection == ev.connection &&
					e.target != gp::kNoLogicIndex) {
				deliver(e.target, e.target_port);
				++stats_.networked_echoed;
			}
		}
	}
}

void UiFlowSession::note(std::string p_line) {
	if (log_.size() < kUiFlowSessionMaxLog) {
		log_.push_back(std::move(p_line));
	}
}

void UiFlowSession::raise(uint32_t p_node, int32_t p_port) {
	const core::Status s = runtime_->force_event(p_node, p_port);
	if (!s.ok()) {
		errors_.push_back(s.error().message);
	}
}

void UiFlowSession::deliver(uint32_t p_node, int32_t p_port) {
	const core::Status s = runtime_->deliver_event(p_node, p_port);
	if (!s.ok()) {
		errors_.push_back(s.error().message);
	}
}

void UiFlowSession::enter_scope_input(std::string_view p_port_name) {
	const int32_t port = gp::logic_port_id(p_port_name);
	note("native " + std::string(p_port_name));
	for (const gp::LogicEntry &e : graph_->entries_of(scope_, port)) {
		deliver(e.target, e.target_port);
	}
}

void UiFlowSession::pre_round(UiPreRoundEvent p_event) {
	static constexpr const char *kNames[] = { "Waiting", "Countdown", "RoundStarted", "Restarting" };
	note(std::string("native PreRound.") + kNames[size_t(p_event)]);
	for (const uint32_t n : pre_round_nodes_) {
		raise(n, ui_preround_event_id(p_event));
	}
}

core::Status UiFlowSession::update(const UiGameState &p_state) {
	if ((last_.phase == net::MatchPhase::RoundOver || last_.phase == net::MatchPhase::Restarting) &&
			(p_state.phase == net::MatchPhase::PreRound || p_state.phase == net::MatchPhase::InRound)) {
		note("native Reload");
		BF3_TRY(start_logic());
		widget_events_.clear();
		++stats_.reloads;
		round_over_ = false;
		last_ = UiGameState{};
		last_.own_deaths = p_state.own_deaths;
		last_.own_spawns = p_state.own_spawns;
		last_.end_of_round = p_state.end_of_round;
	}
	const bool joining = p_state.joined && !last_.joined;
	if (joining) {
		enter_scope_input("Spawn");
	}
	if (p_state.phase != last_.phase) {
		if (p_state.phase == net::MatchPhase::PreRound) {
			pre_round(p_state.counting_down ? UiPreRoundEvent::Countdown : UiPreRoundEvent::Waiting);
		} else if (p_state.phase == net::MatchPhase::InRound && last_.phase == net::MatchPhase::PreRound) {
			pre_round(UiPreRoundEvent::RoundStarted);
		} else if (p_state.phase == net::MatchPhase::Restarting) {
			pre_round(UiPreRoundEvent::Restarting);
		} else if (p_state.phase == net::MatchPhase::RoundOver) {
			round_over_ = true;
			note("native RoundOver.OnRoundOver " + std::to_string(p_state.winner));
			for (const auto &[node, team] : round_over_nodes_) {
				if (team == int64_t(p_state.winner)) {
					raise(node, kOnRoundOver);
				}
			}
		}
	} else if (p_state.phase == net::MatchPhase::PreRound && p_state.counting_down && !last_.counting_down) {
		pre_round(UiPreRoundEvent::Countdown);
	}
	if (p_state.own_deaths > last_.own_deaths) {
		enter_scope_input("Died");
	}
	if (p_state.own_spawns > last_.own_spawns && !round_over_ && !joining) {
		enter_scope_input("Playing");
	}
	if (p_state.end_of_round && !last_.end_of_round) {
		enter_scope_input("EndOfRound");
		const sim::Tick at = runtime_->tick() + 1;
		for (const uint32_t n : end_of_round_nodes_) {
			if (timers_.size() < kMaxTimers) {
				timers_.push_back(Timer{ at + std::max<uint32_t>(lg::logic_delay_ticks(pre_eor_seconds_[n]), 1), n,
						kEndOfRoundPreEorElapsed });
			}
		}
	}
	const uint32_t spawns_seen = last_.own_spawns;
	last_ = p_state;
	if (joining) {
		last_.own_spawns = spawns_seen;
	}
	const core::Status stepped = step();
	dispatch_data_writes();
	return stepped;
}

core::Status UiFlowSession::step() {
	const sim::Tick next = runtime_->tick() + 1;
	std::vector<Timer> due;
	timers_.erase(std::remove_if(timers_.begin(), timers_.end(),
						  [&](const Timer &t) {
							  if (t.due <= next) {
								  due.push_back(t);
								  return true;
							  }
							  return false;
						  }),
			timers_.end());
	for (const Timer &t : due) {
		raise(t.node, t.port);
	}
	const bool areas = sound_areas_ != nullptr && listener_.has_value();
	if (areas) {
		BF3_TRY(sound_areas_->listen(*listener_, *runtime_));
	}
	core::Result<lg::LogicTickStats> stepped = runtime_->step();
	if (!stepped) {
		return stepped.error();
	}
	if (areas) {
		BF3_TRY(sound_areas_->settle(*runtime_));
	}
	const sim::Tick now = runtime_->tick();
	const auto later = [&](uint32_t p_node, int32_t p_port, uint32_t p_ticks) {
		if (timers_.size() < kMaxTimers) {
			timers_.push_back(Timer{ now + std::max<uint32_t>(p_ticks, 1), p_node, p_port });
		}
	};
	for (const lg::LogicWatchHit &h : runtime_->watched()) {
		if (h.kind != lg::LogicWatchKind::EventIn) {
			continue;
		}
		if (const std::optional<uint32_t> done = effects_->event(h.node, h.port, h.tick)) {
			later(h.node, kFadeDone, *done);
		}
		if (ui_graph_of_.count(h.node) != 0) {
			ui_graph_input(h.node, h.port);
		} else if (std::find(end_of_round_nodes_.begin(), end_of_round_nodes_.end(), h.node) !=
				end_of_round_nodes_.end()) {
			if (h.port == kEndOfRoundStart) {
				raise(h.node, kEndOfRoundPreEor);
			} else if (h.port == kEndOfRoundRequest) {
				raise(h.node, kEndOfRoundAnswer);
			}
		}
	}
	deliver_networked();
	initialize_opened();
	return core::Status::success();
}

void UiFlowSession::initialize_opened() {
	if (!options_.initialize_screens) {
		return;
	}
	for (size_t guard = 0; guard < 64; ++guard) {
		const std::vector<uint64_t> opened = flow_->take_opened();
		const std::vector<uint64_t> closing = flow_->take_closing();
		if (opened.empty() && closing.empty()) {
			break;
		}
		for (const uint64_t id : opened) {
			flow_->screen_initialized(id);
		}
		for (const uint64_t id : closing) {
			flow_->screen_exited(id);
		}
		for (const std::string &l : flow_->log()) {
			note(l);
		}
		flow_->clear_log();
	}
}

void UiFlowSession::ui_graph_input(uint32_t p_node, int32_t p_port) {
	const std::string &graph = ui_graph_of_[p_node];
	const std::shared_ptr<const ui::UiGraphAsset> &asset = graphs_[graph];
	const std::string leaf = ui::ui_data_category_leaf(graph);
	if (p_port == gp::logic_port_id(kUiGraphExitPort)) {
		note("logic " + leaf + " " + std::string(kUiGraphExitPort));
		flow_->close_graph(graph);
	} else if (p_port == gp::logic_port_id(kUiGraphEnterPort)) {
		graph_owner_[graph] = p_node;
		note("logic " + leaf + " " + std::string(kUiGraphEnterPort));
		push_graph(p_node);
		if (asset) {
			for (const ui::UiGraphNode &n : asset->nodes) {
				if (n.kind == ui::UiGraphNodeKind::InstanceInput && n.root) {
					flow_->fire_input(graph, n.name);
				}
			}
		}
	} else {
		const ui::UiGraphNode *input = nullptr;
		if (asset) {
			for (const ui::UiGraphNode &n : asset->nodes) {
				if (n.kind == ui::UiGraphNodeKind::InstanceInput && ui::ui_action_key(n.name) == p_port) {
					input = &n;
					break;
				}
			}
		}
		if (input == nullptr) {
			++stats_.unknown_inputs;
			note("logic " + leaf + " #" + std::to_string(p_port));
		} else {
			++stats_.inputs_fired;
			graph_owner_[graph] = p_node;
			note("logic " + leaf + "." + input->name);
			push_graph(p_node);
			flow_->fire_input(graph, input->name);
		}
	}
	for (const std::string &l : flow_->log()) {
		note(l);
	}
	flow_->clear_log();
}

void UiFlowSession::push_graph(uint32_t p_node) {
	const auto entity = graph_entities_.find(p_node);
	if (entity == graph_entities_.end()) {
		return;
	}
	const GraphEntity &e = entity->second;
	std::erase_if(entered_, [&](const GraphEntity &g) { return !flow_->graph_live(g.graph); });
	const auto top = std::find_if(
			entered_.rbegin(), entered_.rend(), [&](const GraphEntity &g) { return g.priority == e.priority; });
	if (top != entered_.rend() && top->graph == e.graph) {
		return;
	}
	if (e.pop_previous && top != entered_.rend()) {
		const std::string previous = top->graph;
		const auto asset = graphs_.find(previous);
		if (asset == graphs_.end() || asset->second == nullptr || !asset->second->protect_screens) {
			note("pop graph " + ui::ui_data_category_leaf(previous) + " for " + ui::ui_data_category_leaf(e.graph));
			++stats_.graphs_popped;
			flow_->close_graph(previous);
		}
		std::erase_if(entered_, [&](const GraphEntity &g) { return g.graph == previous; });
	}
	std::erase_if(entered_, [&](const GraphEntity &g) { return g.graph == e.graph; });
	entered_.push_back(e);
}

bool UiFlowSession::input_action(int32_t p_action) {
	const bool handled = flow_->input_action(p_action);
	drain_flow_log();
	initialize_opened();
	return handled;
}

void UiFlowSession::drain_flow_log() {
	for (const std::string &l : flow_->log()) {
		note(l);
	}
	flow_->clear_log();
}

void UiFlowSession::screen_initialized(uint64_t p_screen) {
	flow_->screen_initialized(p_screen);
	drain_flow_log();
	initialize_opened();
}

void UiFlowSession::screen_exited(uint64_t p_screen) {
	flow_->screen_exited(p_screen);
	drain_flow_log();
	initialize_opened();
}

bool UiFlowSession::widget_fired(std::string_view p_partition, std::string_view p_instance, std::string_view p_query,
		std::optional<std::string> p_value) {
	const ui::UiFlowScreen *screen = flow_->find_screen(p_partition);
	if (screen == nullptr) {
		return false;
	}
	const bool handled = flow_->widget_fired(screen->id, p_instance, p_query, std::move(p_value));
	for (const std::string &l : flow_->log()) {
		note(l);
	}
	flow_->clear_log();
	initialize_opened();
	return handled;
}

void UiFlowSession::set_data(std::string_view p_category, int32_t p_key, std::string p_value) {
	data_[leaf_key(p_category, p_key)] = std::move(p_value);
}

void UiFlowSession::erase_data(std::string_view p_category, int32_t p_key) {
	data_.erase(leaf_key(p_category, p_key));
}

std::optional<std::string> UiFlowSession::data(std::string_view p_category, int32_t p_key) const {
	const auto it = data_.find(leaf_key(p_category, p_key));
	if (it == data_.end()) {
		return std::nullopt;
	}
	return it->second;
}

std::vector<ui::UiFlowAction> UiFlowSession::take_actions() {
	std::vector<ui::UiFlowAction> out;
	out.swap(actions_);
	return out;
}

void UiFlowSession::action(const ui::UiFlowAction &p_action) {
	if (actions_.size() < kUiFlowSessionMaxActions) {
		actions_.push_back(p_action);
	}
}

std::optional<std::string> UiFlowSession::data_get(const ui::UiDataRef &p_ref) {
	return data(p_ref.category, p_ref.key);
}

void UiFlowSession::data_set(const ui::UiDataRef &p_ref, std::string_view p_value, bool) {
	data_[leaf_key(p_ref.category, p_ref.key)] = std::string(p_value);
	if (data_writes_.size() >= kUiFlowSessionMaxDataWrites) {
		++stats_.data_writes_dropped;
		return;
	}
	data_writes_.push_back(
			UiFlowDataWrite{ ui::ui_data_category_leaf(p_ref.category), p_ref.key, std::string(p_value) });
	stats_.data_writes_peak = std::max(stats_.data_writes_peak, data_writes_.size());
}

uint32_t UiFlowSession::add_data_sink(std::string p_category_leaf, UiFlowDataSink p_sink) {
	const uint32_t id = next_data_sink_++;
	data_sinks_.push_back(DataSink{ id, std::move(p_category_leaf), std::move(p_sink) });
	return id;
}

void UiFlowSession::remove_data_sink(uint32_t p_id) {
	std::erase_if(data_sinks_, [&](const DataSink &p_s) { return p_s.id == p_id; });
}

size_t UiFlowSession::dispatch_data_writes() {
	const std::vector<UiFlowDataWrite> writes = std::exchange(data_writes_, {});
	for (const UiFlowDataWrite &w : writes) {
		for (const DataSink &s : data_sinks_) {
			if (s.category == w.category) {
				s.sink(w);
			}
		}
	}
	stats_.data_writes_dispatched += writes.size();
	return writes.size();
}

void UiFlowSession::widget_event(const ui::UiFlowScreen &p_screen, std::string_view p_instance,
		std::string_view p_query, const std::optional<std::string> &p_value) {
	if (widget_events_.size() < kUiFlowSessionMaxWidgetEvents) {
		widget_events_.push_back(UiFlowWidgetEvent{
				p_screen.id, p_screen.partition, std::string(p_instance), std::string(p_query), p_value });
	}
}

std::vector<UiFlowWidgetEvent> UiFlowSession::take_widget_events() {
	std::vector<UiFlowWidgetEvent> out;
	out.swap(widget_events_);
	return out;
}

void UiFlowSession::graph_output(std::string_view p_graph, std::string_view p_output, int32_t p_id, bool) {
	++stats_.outputs_raised;
	note("logic out " + ui::ui_data_category_leaf(p_graph) + "." + std::string(p_output));
	if (const auto owner = graph_owner_.find(p_graph); owner != graph_owner_.end()) {
		raise(owner->second, p_id);
		return;
	}
	for (const auto &[node, graph] : ui_graph_of_) {
		if (graph == p_graph) {
			raise(node, p_id);
		}
	}
}

void ui_flow_tdm_pc_defaults(UiFlowSession &r_session, std::string_view p_mode) {
	const std::string_view family = p_mode.substr(0, p_mode.find_last_not_of("0123456789") + 1);
	r_session.set_data("ui/uicomponents/uisettingscomp", -668821714, "win32");
	r_session.set_data("ui/uicomponents/uisettingscomp", 1733092213, std::string(p_mode));
	r_session.set_data("ui/uicomponents/uisettingscomp", 1870459195, std::string(family));
	r_session.set_data("ui/uicomponents/uispawnpointcomp", -990983020, "True");
	r_session.set_data("ui/uicomponents/uispawnpointcomp", -225037516, "True");
	r_session.set_data("ui/uicomponents/uispectatorcomp", -366910738, "False");
	r_session.set_data("ui/uicomponents/uiendofroundcomp", -1857162541, "False");
	r_session.set_data("ui/uicomponents/uiendofroundcomp", 1450248605, "False");
	r_session.set_data("ui/uicomponents/uisoldiercomp", 347110003, "True");
	r_session.set_data("ui/uicomponents/uikitcomp", 1985842909, "0");
	r_session.set_data("ui/uicomponents/uicustomizationcomp", -493868347, "True");
	r_session.set_data("ui/uicomponents/uicustomizationcomp", 197143691, "True");
	r_session.set_data("ui/uicomponents/uicustomizationcomp", 975152979, "True");
	r_session.set_data("ui/uicomponents/uicustomizationcomp", 1324149331, "True");
	r_session.set_data("ui/uicomponents/uispawnlogiccomp", -1005581981, "0");
}
}
