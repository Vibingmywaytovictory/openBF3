#pragma once

#include "bf3/core/ui/key_bindings.h"
#include "bf3/core/ui/ui_graph.h"
#include "bf3/core/ui/ui_input.h"
#include "bf3/host/client_session.h"
#include "bf3/host/ui_flow_session.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace bf3::core {
class EbxResolver;
class Install;
namespace level {
class LoadedLevel;
}
}

namespace bf3::godot_side {

inline constexpr uint32_t kNativeFlowMaxCatchUpTicks = 30;
inline constexpr size_t kNativeFlowMaxActions = 4096;
inline constexpr std::string_view kUiFlowSpawnAction = "Spawn";

class NativeFlow {
public:
	NativeFlow();
	~NativeFlow();
	NativeFlow(const NativeFlow &) = delete;
	NativeFlow &operator=(const NativeFlow &) = delete;

	void start(std::shared_ptr<const bf3::core::Install> p_install, std::string p_level, std::string p_mode);
	bool poll();
	bool started() const { return started_; }
	bool ready() const { return session_ != nullptr; }
	bool failed() const { return !error_.empty(); }
	const std::string &error() const { return error_; }
	double load_seconds() const { return load_seconds_; }

	bf3::host::UiFlowSession *session() { return session_.get(); }
	const bf3::core::EbxResolver *resolver() const;
	const bf3::core::ui::UiInputRouter &router() const { return router_; }
	bool apply_key_bindings(const bf3::core::ui::KeyBindingTable &p_table, uint64_t p_revision);

	bf3::core::Status tick(const bf3::host::UiGameState &p_state, double p_seconds);
	bf3::core::Status receive(bf3::host::ClientLogicFeed &r_feed);
	std::vector<bf3::core::ui::UiFlowAction> take_actions();
	std::optional<std::string> take_journal_overflow();
	void set_keep_log(bool p_keep) { keep_log_ = p_keep; }
	uint64_t ticks() const { return ticks_; }
	uint64_t actions() const { return actions_; }

private:
	struct Loaded;
	void join();

	bool started_ = false;
	std::thread loader_;
	std::atomic<bool> done_{ false };
	std::unique_ptr<Loaded> loaded_;
	std::unique_ptr<bf3::core::level::LoadedLevel> level_;
	std::unique_ptr<bf3::host::UiFlowSession> session_;
	bf3::core::ui::UiInputRouter router_;
	bf3::core::ui::UiInputBindings base_bindings_;
	uint64_t bindings_revision_ = ~0ull;
	std::string error_;
	double load_seconds_ = 0.0;
	double clock_ = 0.0;
	bool keep_log_ = false;
	bool journal_overflow_reported_ = false;
	uint64_t ticks_ = 0;
	uint64_t actions_ = 0;
	std::vector<bf3::core::ui::UiFlowAction> pending_;
	std::optional<bf3::sim::logic::LogicGhost> ghost_;
	bool ghost_pending_ = false;
};

}
