#pragma once

#include "bf3/core/ebx_db.h"
#include "bf3/core/result.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::core::ui {

inline constexpr std::string_view kUiActionsPartition = "ui/flow/action/veniceuiactions";
inline constexpr size_t kUiGraphMaxNodes = 1024;
inline constexpr size_t kUiGraphMaxPortsPerNode = 256;
inline constexpr size_t kUiGraphMaxConnections = 4096;
inline constexpr size_t kUiGraphMaxParams = 64;
inline constexpr size_t kUiGraphMaxSteps = 64;
inline constexpr size_t kUiActionMaxKeys = 4096;

enum class UiGraphNodeKind : uint8_t {
	InstanceInput,
	InstanceOutput,
	State,
	Dialog,
	Widget,
	Action,
	DataSet,
	DataGet,
	DataToggle,
	DataIncrement,
	DataStep,
	BinaryLogic,
	ComparisonLogic,
	OperandLogic,
	Splitter,
	Unknown,
};

const char *ui_graph_node_kind_name(UiGraphNodeKind p_kind);

enum class UiGraphPortRole : uint8_t {
	In,
	Out,
	Show,
	Hide,
	True,
	False,
	Input,
	Output,
};

struct UiGraphPort {
	UiGraphPortRole role = UiGraphPortRole::In;
	std::string name;
	std::string instance;
	std::string query;
	std::string input_event;
	int32_t input_action = -1;
};

struct UiDataRef {
	std::string category;
	std::string name;
	int32_t key = 0;

	bool defined() const { return key != 0 || !name.empty(); }
	bool operator==(const UiDataRef &) const = default;
};

std::string ui_data_category_leaf(std::string_view p_category);

struct UiDialogButton {
	std::string label;
	int32_t input_concept = -1;
	bool operator==(const UiDialogButton &) const = default;
};

struct UiGraphNode {
	UiGraphNodeKind kind = UiGraphNodeKind::Unknown;
	std::string type;
	std::string name;
	std::string instance;
	bool root = false;
	std::vector<UiGraphPort> ports;
	std::string screen;
	int32_t action_key = 0;
	std::vector<std::string> params;
	bool append_params = false;
	std::vector<UiDataRef> data_inputs;
	UiDataRef data;
	UiDataRef right;
	std::string param;
	bool force_update = false;
	bool set_empty = false;
	bool skip_fractionals = false;
	std::string op;
	double literal = 0.0;
	UiDataRef current_step;
	double increment = 0.0;
	double minimum = 0.0;
	double maximum = 0.0;
	std::vector<double> steps;
	int32_t output_id = 0;
	bool destroy_graph = false;
	std::string dialog_title;
	std::string dialog_text;
	std::vector<UiDialogButton> dialog_buttons;

	std::optional<uint32_t> port(UiGraphPortRole p_role, std::string_view p_name = {}) const;
};

struct UiGraphConnection {
	uint32_t source_node = 0;
	uint32_t source_port = 0;
	uint32_t target_node = 0;
	uint32_t target_port = 0;
	int32_t pop = 0;
	bool operator==(const UiGraphConnection &) const = default;
};

struct UiGraphAsset {
	std::string partition;
	std::string type;
	std::string name;
	bool modal = false;
	bool protect_screens = false;
	std::vector<UiGraphNode> nodes;
	std::vector<UiGraphConnection> connections;
	uint32_t unlisted_port_connections = 0;
	uint32_t named_port_connections = 0;
	uint32_t duplicate_connections = 0;

	std::optional<uint32_t> find(UiGraphNodeKind p_kind, std::string_view p_name) const;
	bool is_screen() const { return type == "UIScreenAsset"; }
};

Result<UiGraphAsset> read_ui_graph_asset(const EbxResolver &p_db, std::string_view p_partition);

int32_t ui_action_key(std::string_view p_name);

class UiActionTable {
public:
	static Result<UiActionTable> read(const EbxResolver &p_db, std::string_view p_partition = kUiActionsPartition);
	static UiActionTable of(std::vector<std::string> p_names);

	std::optional<std::string_view> name_of(int32_t p_key) const;
	size_t size() const { return names_.size(); }
	const std::vector<std::string> &names() const { return names_; }

private:
	std::vector<std::string> names_;
	std::vector<std::pair<int32_t, uint32_t>> by_key_;
};

struct UiFlowAction {
	std::string graph;
	std::string node;
	int32_t key = 0;
	std::string name;
	std::vector<std::string> params;
};

struct UiFlowScreen {
	uint64_t id = 0;
	std::string partition;
	std::string graph;
	std::string state;
	uint32_t state_node = 0;
	bool dialog = false;
	bool shown = true;
	bool modal = false;
	bool focusable = false;
	bool initialized = false;
	bool exiting = false;
	uint64_t shown_count = 0;
	std::string dialog_title;
	std::string dialog_text;
	std::vector<UiDialogButton> dialog_buttons;
};

class UiFlowHost {
public:
	virtual ~UiFlowHost() = default;
	virtual void action(const UiFlowAction &p_action) = 0;
	virtual std::optional<std::string> data_get(const UiDataRef &p_ref) = 0;
	virtual void data_set(const UiDataRef &p_ref, std::string_view p_value, bool p_force) = 0;
	virtual void widget_event(const UiFlowScreen &p_screen, std::string_view p_instance, std::string_view p_query,
			const std::optional<std::string> &p_value) = 0;
	virtual void graph_output(std::string_view p_graph, std::string_view p_output, int32_t p_id, bool p_destroy) = 0;
};

using UiGraphProvider = std::function<std::shared_ptr<const UiGraphAsset>(std::string_view)>;

inline constexpr size_t kUiFlowMaxStepsPerCall = 4096;
inline constexpr size_t kUiFlowMaxLog = 1u << 16;
inline constexpr size_t kUiFlowMaxScreens = 128;
inline constexpr std::string_view kUiScreenInitializedPort = "Initialized";
inline constexpr std::string_view kUiScreenShownPort = "Shown";
inline constexpr std::string_view kUiScreenHiddenPort = "Hidden";
inline constexpr std::string_view kUiEnterFocusedInput = "EnterFocused";

class UiFlow {
public:
	UiFlow(UiFlowHost &r_host, UiGraphProvider p_graphs, const UiActionTable *p_actions = nullptr);
	~UiFlow();
	UiFlow(const UiFlow &) = delete;
	UiFlow &operator=(const UiFlow &) = delete;

	bool fire_input(
			std::string_view p_graph, std::string_view p_input, std::optional<std::string> p_value = std::nullopt);
	bool widget_fired(uint64_t p_screen, std::string_view p_instance, std::string_view p_query,
			std::optional<std::string> p_value = std::nullopt);
	bool input_action(int32_t p_action);
	void screen_initialized(uint64_t p_screen);
	void screen_exited(uint64_t p_screen);
	bool close_graph(std::string_view p_graph);

	const std::vector<UiFlowScreen> &screens() const { return screens_; }
	const UiFlowScreen *screen(uint64_t p_id) const;
	const UiFlowScreen *find_screen(std::string_view p_partition) const;
	std::optional<uint64_t> focused() const;
	bool graph_live(std::string_view p_graph) const;
	std::vector<uint64_t> take_opened();
	std::vector<uint64_t> take_closing();

	const std::vector<std::string> &log() const { return log_; }
	void clear_log() { log_.clear(); }
	size_t steps() const { return steps_; }
	size_t step_overflows() const { return overflows_; }
	const std::vector<std::string> &errors() const { return errors_; }

private:
	struct Instance;
	Instance *instance_of(std::string_view p_graph, bool p_create);
	Instance *screen_instance(uint64_t p_screen);
	void begin_call();
	void end_call();
	void enter(Instance &r_inst, uint32_t p_node, uint32_t p_port, const std::optional<std::string> &p_value);
	void follow(Instance &r_inst, uint32_t p_node, uint32_t p_port, const std::optional<std::string> &p_value);
	UiFlowScreen *ensure_screen(Instance &r_inst, uint32_t p_node, bool p_shown, bool p_focus);
	void pop(int32_t p_count);
	void begin_close(size_t p_index);
	void refocus();
	bool state_output(uint64_t p_screen, std::string_view p_port_name, const std::optional<std::string> &p_value);
	void note(std::string p_line);
	void error(std::string p_line);
	std::optional<std::string> read(const UiDataRef &p_ref, const std::optional<std::string> &p_incoming);

	UiFlowHost &host_;
	UiGraphProvider graphs_;
	const UiActionTable *actions_ = nullptr;
	std::vector<std::unique_ptr<Instance>> instances_;
	std::vector<UiFlowScreen> screens_;
	std::vector<uint64_t> opened_;
	std::vector<uint64_t> closing_;
	std::optional<uint64_t> focus_;
	uint64_t next_screen_ = 1;
	size_t depth_ = 0;
	size_t calls_ = 0;
	size_t budget_ = 0;
	size_t steps_ = 0;
	size_t overflows_ = 0;
	std::vector<std::string> log_;
	std::vector<std::string> errors_;
};

std::string ui_flow_value_text(const std::optional<std::string> &p_value);
bool ui_flow_truthy(std::string_view p_value);

}
