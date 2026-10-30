#include "native_flow.h"

#include "bf3/core/install.h"
#include "bf3/core/level/level.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/sim/sim_types.h"

#include <chrono>
#include <cmath>

namespace bf3::godot_side {

struct NativeFlow::Loaded {
	std::unique_ptr<bf3::core::level::LoadedLevel> level;
	std::unique_ptr<bf3::host::UiFlowSession> session;
	bf3::core::ui::UiInputBindings bindings;
	std::string error;
	double seconds = 0.0;
};

NativeFlow::NativeFlow() = default;

NativeFlow::~NativeFlow() {
	join();
}

void NativeFlow::join() {
	if (loader_.joinable()) {
		loader_.join();
	}
}

void NativeFlow::start(std::shared_ptr<const bf3::core::Install> p_install, std::string p_level, std::string p_mode) {
	if (started_) {
		return;
	}
	started_ = true;
	done_.store(false, std::memory_order_relaxed);
	loader_ = std::thread([this, install = std::move(p_install), level = std::move(p_level), mode = std::move(p_mode)] {
		const auto t0 = std::chrono::steady_clock::now();
		auto out = std::make_unique<Loaded>();
		bf3::core::Result<std::unique_ptr<bf3::core::level::LoadedLevel>> l =
				bf3::core::level::load_level(install, level, mode);
		if (!l) {
			out->error = level + " " + mode + ": " + l.error().message;
		} else {
			out->level = std::move(*l);
			bf3::host::UiFlowSessionOptions o;
			o.initialize_screens = false;
			bf3::core::Result<std::unique_ptr<bf3::host::UiFlowSession>> s =
					bf3::host::UiFlowSession::create(*out->level, o);
			if (!s) {
				out->error = s.error().message;
			} else {
				out->session = std::move(*s);
				bf3::host::ui_flow_tdm_pc_defaults(*out->session, out->level->mode);
			}
			bf3::core::Result<bf3::core::ui::UiInputBindings> b =
					bf3::core::ui::read_ui_input_bindings(out->level->db());
			if (!b) {
				out->error += (out->error.empty() ? "" : "; ") + b.error().message;
			} else {
				out->bindings = std::move(*b);
			}
		}
		out->seconds = bf3::core::seconds_since(t0);
		loaded_ = std::move(out);
		done_.store(true, std::memory_order_release);
	});
}

bool NativeFlow::poll() {
	if (session_ != nullptr) {
		return true;
	}
	if (!started_ || !error_.empty() || !done_.load(std::memory_order_acquire)) {
		return false;
	}
	join();
	if (loaded_ == nullptr) {
		error_ = "no loader result";
		return false;
	}
	load_seconds_ = loaded_->seconds;
	if (!loaded_->error.empty()) {
		error_ = loaded_->error;
		loaded_.reset();
		return false;
	}
	level_ = std::move(loaded_->level);
	base_bindings_ = loaded_->bindings;
	router_.set_bindings(std::move(loaded_->bindings));
	session_ = std::move(loaded_->session);
	loaded_.reset();
	return true;
}

bool NativeFlow::apply_key_bindings(const bf3::core::ui::KeyBindingTable &p_table, uint64_t p_revision) {
	if (session_ == nullptr || p_revision == bindings_revision_) {
		return false;
	}
	bindings_revision_ = p_revision;
	router_.set_bindings(bf3::core::ui::ui_input_bindings_with(base_bindings_, p_table));
	return true;
}

const bf3::core::EbxResolver *NativeFlow::resolver() const {
	return level_ != nullptr ? &level_->db() : nullptr;
}

bf3::core::Status NativeFlow::tick(const bf3::host::UiGameState &p_state, double p_seconds) {
	if (session_ == nullptr) {
		return bf3::core::Status::success();
	}
	if (!keep_log_) {
		session_->clear_log();
	}
	clock_ += std::isfinite(p_seconds) && p_seconds > 0.0 ? p_seconds : 0.0;
	uint32_t due = 0;
	while (clock_ >= double(bf3::sim::kTickSeconds) && due < kNativeFlowMaxCatchUpTicks) {
		clock_ -= double(bf3::sim::kTickSeconds);
		++due;
	}
	if (due == kNativeFlowMaxCatchUpTicks) {
		clock_ = 0.0;
	}
	for (uint32_t i = 0; i < due; ++i) {
		BF3_TRY(session_->update(p_state));
		++ticks_;
	}
	for (bf3::core::ui::UiFlowAction &a : session_->take_actions()) {
		++actions_;
		if (pending_.size() < kNativeFlowMaxActions) {
			pending_.push_back(std::move(a));
		}
	}
	return bf3::core::Status::success();
}

bf3::core::Status NativeFlow::receive(bf3::host::ClientLogicFeed &r_feed) {
	if (r_feed.ghost) {
		ghost_ = std::move(*r_feed.ghost);
		r_feed.ghost.reset();
		ghost_pending_ = true;
	}
	if (session_ == nullptr) {
		r_feed.events.clear();
		return bf3::core::Status::success();
	}
	session_->receive_networked(r_feed.events);
	r_feed.events.clear();
	if (!ghost_pending_) {
		return bf3::core::Status::success();
	}
	ghost_pending_ = false;
	return session_->apply_ghost(*ghost_);
}

std::optional<std::string> NativeFlow::take_journal_overflow() {
	const uint64_t dropped = session_ != nullptr ? session_->stats().data_writes_dropped : 0;
	if (dropped == 0 || journal_overflow_reported_) {
		return std::nullopt;
	}
	journal_overflow_reported_ = true;
	return "the UI flow data-write journal dropped " + std::to_string(dropped) + " writes past its cap";
}

std::vector<bf3::core::ui::UiFlowAction> NativeFlow::take_actions() {
	if (session_ != nullptr) {
		for (bf3::core::ui::UiFlowAction &a : session_->take_actions()) {
			++actions_;
			if (pending_.size() < kNativeFlowMaxActions) {
				pending_.push_back(std::move(a));
			}
		}
	}
	return std::exchange(pending_, {});
}

}
