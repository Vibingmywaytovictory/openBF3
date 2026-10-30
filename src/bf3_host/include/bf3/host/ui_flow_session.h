#pragma once

#include "bf3/core/gameplay/logic_graph.h"
#include "bf3/core/result.h"
#include "bf3/core/ui/ui_graph.h"
#include "bf3/host/logic_screen_effects.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/logic/logic_runtime.h"
#include "bf3/sim/logic/sound_area_natives.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bf3::core::level {
class LoadedLevel;
}

namespace bf3::host {

inline constexpr std::string_view kUiGameLogicTdm = "ui/flow/logic/uigamelogictdm";
inline constexpr std::string_view kUiGraphEnterPort = "EnterUIGraph";
inline constexpr std::string_view kUiGraphExitPort = "ExitUIGraph";
inline constexpr size_t kUiFlowSessionMaxLog = 1u << 16;
inline constexpr size_t kUiFlowSessionMaxActions = 4096;
inline constexpr size_t kUiFlowSessionMaxWidgetEvents = 4096;
inline constexpr size_t kUiFlowSessionMaxDataWrites = 4096;
inline constexpr size_t kUiFlowSessionMaxReceived = 4096;

enum class UiPreRoundEvent : uint8_t { Waiting, Countdown, RoundStarted, Restarting };
int32_t ui_preround_event_id(UiPreRoundEvent p_event);

struct ClientLogicWorld {
	core::gameplay::LogicWorld world;
	std::shared_ptr<const core::gameplay::LogicGraph> graph;
};
core::Result<ClientLogicWorld> build_client_logic(const core::level::LoadedLevel &p_level);

struct UiGameState {
	bool joined = false;
	net::MatchPhase phase = net::MatchPhase::Loading;
	bool counting_down = false;
	uint32_t own_deaths = 0;
	uint32_t own_spawns = 0;
	uint8_t winner = 0;
	bool end_of_round = false;
};

struct UiFlowSessionOptions {
	std::string scope = std::string(kUiGameLogicTdm);
	sim::logic::LogicPlatform platform = sim::logic::LogicPlatform::Win32;
	bool initialize_screens = true;
	bool echo_networked = false;
};

struct UiFlowWidgetEvent {
	uint64_t screen = 0;
	std::string partition;
	std::string instance;
	std::string query;
	std::optional<std::string> value;
};

struct UiFlowDataWrite {
	std::string category;
	int32_t key = 0;
	std::string value;
};

using UiFlowDataSink = std::function<void(const UiFlowDataWrite &)>;

struct UiFlowSessionStats {
	size_t ui_graph_nodes = 0;
	size_t graphs = 0;
	size_t watches = 0;
	size_t fades = 0;
	size_t end_of_round_nodes = 0;
	size_t pre_round_nodes = 0;
	size_t round_over_nodes = 0;
	uint64_t inputs_fired = 0;
	uint64_t unknown_inputs = 0;
	uint64_t outputs_raised = 0;
	uint64_t networked_delivered = 0;
	uint64_t networked_received = 0;
	uint64_t networked_unmatched = 0;
	uint64_t networked_dropped = 0;
	uint64_t networked_local = 0;
	uint64_t networked_echoed = 0;
	uint64_t ghosts_applied = 0;
	uint64_t ghosts_refused = 0;
	uint64_t reloads = 0;
	uint64_t graphs_popped = 0;
	uint64_t data_writes_dispatched = 0;
	uint64_t data_writes_dropped = 0;
	size_t data_writes_peak = 0;
};

class UiFlowSession final : public core::ui::UiFlowHost {
public:
	static core::Result<std::unique_ptr<UiFlowSession>> create(
			const core::level::LoadedLevel &p_level, const UiFlowSessionOptions &p_options = {});
	~UiFlowSession() override;
	UiFlowSession(const UiFlowSession &) = delete;
	UiFlowSession &operator=(const UiFlowSession &) = delete;

	core::Status update(const UiGameState &p_state);
	core::Status step();
	bool input_action(int32_t p_action);
	bool widget_fired(std::string_view p_partition, std::string_view p_instance, std::string_view p_query,
			std::optional<std::string> p_value = std::nullopt);
	void set_data(std::string_view p_category, int32_t p_key, std::string p_value);
	void erase_data(std::string_view p_category, int32_t p_key);
	void screen_initialized(uint64_t p_screen);
	void screen_exited(uint64_t p_screen);
	void receive_networked(std::span<const sim::logic::LogicNetworkedEvent> p_events);
	core::Status apply_ghost(const sim::logic::LogicGhost &p_ghost);
	void set_listener(const core::Vec3 &p_position);
	const sim::logic::SoundAreaNatives *sound_areas() const { return sound_areas_.get(); }

	core::ui::UiFlow &flow() { return *flow_; }
	const core::ui::UiFlow &flow() const { return *flow_; }
	const sim::logic::LogicRuntime &runtime() const { return *runtime_; }
	const std::vector<std::string> &log() const { return log_; }
	void clear_log() { log_.clear(); }
	std::vector<core::ui::UiFlowAction> take_actions();
	std::vector<UiFlowWidgetEvent> take_widget_events();
	uint32_t add_data_sink(std::string p_category_leaf, UiFlowDataSink p_sink);
	void remove_data_sink(uint32_t p_id);
	size_t dispatch_data_writes();
	size_t pending_data_writes() const { return data_writes_.size(); }
	const UiFlowSessionStats &stats() const { return stats_; }
	LogicScreenEffectsFrame screen_effects() const { return effects_->frame(runtime_->tick(), runtime_.get()); }
	const LogicScreenEffects &effects() const { return *effects_; }
	const std::vector<std::string> &errors() const { return errors_; }
	std::optional<std::string> data(std::string_view p_category, int32_t p_key) const;

	void action(const core::ui::UiFlowAction &p_action) override;
	std::optional<std::string> data_get(const core::ui::UiDataRef &p_ref) override;
	void data_set(const core::ui::UiDataRef &p_ref, std::string_view p_value, bool p_force) override;
	void widget_event(const core::ui::UiFlowScreen &p_screen, std::string_view p_instance, std::string_view p_query,
			const std::optional<std::string> &p_value) override;
	void graph_output(std::string_view p_graph, std::string_view p_output, int32_t p_id, bool p_destroy) override;

private:
	struct GraphEntity {
		std::string graph;
		std::string priority;
		bool pop_previous = false;
	};
	struct Timer {
		sim::Tick due = 0;
		uint32_t node = 0;
		int32_t port = 0;
	};
	struct DataSink {
		uint32_t id = 0;
		std::string category;
		UiFlowDataSink sink;
	};

	UiFlowSession() = default;
	core::Status bind(const core::level::LoadedLevel &p_level, const UiFlowSessionOptions &p_options);
	core::Status start_logic();
	void enter_scope_input(std::string_view p_port_name);
	void raise(uint32_t p_node, int32_t p_port);
	void deliver(uint32_t p_node, int32_t p_port);
	void ui_graph_input(uint32_t p_node, int32_t p_port);
	void push_graph(uint32_t p_node);
	void pre_round(UiPreRoundEvent p_event);
	void note(std::string p_line);
	void drain_flow_log();
	void initialize_opened();
	void deliver_networked();

	UiFlowSessionOptions options_;
	std::shared_ptr<const core::gameplay::LogicGraph> graph_;
	std::unique_ptr<sim::logic::LogicRuntime> runtime_;
	std::unique_ptr<sim::logic::SoundAreaNatives> sound_areas_;
	std::optional<core::Vec3> listener_;
	std::optional<core::ui::UiActionTable> actions_table_;
	std::map<std::string, std::shared_ptr<const core::ui::UiGraphAsset>, std::less<>> graphs_;
	core::ui::UiGraphProvider provider_;
	std::vector<std::pair<uint32_t, int32_t>> watch_ports_;
	std::map<uint64_t, std::vector<std::pair<uint32_t, int32_t>>> networked_targets_;
	std::vector<sim::logic::LogicNetworkedEvent> received_;
	std::optional<sim::logic::LogicGhost> ghost_;
	std::unique_ptr<core::ui::UiFlow> flow_;
	uint32_t scope_ = 0;
	std::map<uint32_t, std::string> ui_graph_of_;
	std::map<std::string, uint32_t, std::less<>> graph_owner_;
	std::map<uint32_t, GraphEntity> graph_entities_;
	std::vector<GraphEntity> entered_;
	std::unique_ptr<LogicScreenEffects> effects_;
	std::vector<uint32_t> end_of_round_nodes_;
	std::vector<uint32_t> pre_round_nodes_;
	std::vector<std::pair<uint32_t, int64_t>> round_over_nodes_;
	std::map<uint32_t, float> pre_eor_seconds_;
	std::vector<Timer> timers_;
	std::map<std::string, std::string, std::less<>> data_;
	std::vector<core::ui::UiFlowAction> actions_;
	std::vector<UiFlowWidgetEvent> widget_events_;
	std::vector<UiFlowDataWrite> data_writes_;
	std::vector<DataSink> data_sinks_;
	uint32_t next_data_sink_ = 1;
	std::vector<std::string> log_;
	std::vector<std::string> errors_;
	UiGameState last_;
	bool round_over_ = false;
	UiFlowSessionStats stats_;
};

void ui_flow_tdm_pc_defaults(UiFlowSession &r_session, std::string_view p_mode);
}
