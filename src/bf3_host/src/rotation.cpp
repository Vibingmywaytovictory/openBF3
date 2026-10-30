#include "bf3/host/rotation.h"

#include "bf3/core/gameplay/eor_timeline.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace bf3::host {

bool same_level(const LevelEntry &p_a, const LevelEntry &p_b) {
	return core::iequals_ascii(p_a.level, p_b.level) && core::iequals_ascii(p_a.mode, p_b.mode);
}

Rotation::Rotation(RotationConfig p_config) :
		config_(std::move(p_config)), cursor_(config_.maps), current_config_(config_.first) {
	round_id_ = current_config_ != nullptr ? current_config_->round_id : 0u;
	stats_.current = cursor_.current();
	stats_.round_id = round_id_;
}

Rotation::~Rotation() {
	join_loader();
}

void Rotation::join_loader() {
	if (loader_.joinable()) {
		loader_.join();
	}
}

void Rotation::start_load(const LevelEntry &p_entry) {
	{
		std::lock_guard lock(mutex_);
		if (pending_ != nullptr && same_level(pending_->entry, p_entry)) {
			return;
		}
	}
	join_loader();
	auto pending = std::make_shared<Pending>();
	pending->entry = p_entry;
	{
		std::lock_guard lock(mutex_);
		pending_ = pending;
		stats_.loading = true;
	}
	const auto run = [this, pending] {
		const auto t0 = std::chrono::steady_clock::now();
		core::Result<std::shared_ptr<const GameServerConfig>> r =
				config_.load ? config_.load(pending->entry) : core::make_error("rotation: no level loader");
		const double seconds = core::seconds_since(t0);
		std::lock_guard lock(mutex_);
		stats_.last_load_s = seconds;
		pending->result = std::move(r);
		pending->done = true;
		stats_.loading = false;
	};
	if (!config_.background) {
		run();
		return;
	}
	loader_ = std::thread([run] {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		run();
	});
}

std::optional<core::Result<std::shared_ptr<const GameServerConfig>>> Rotation::take_load(const LevelEntry &p_entry) {
	std::lock_guard lock(mutex_);
	if (pending_ == nullptr || !pending_->done || !same_level(pending_->entry, p_entry)) {
		return std::nullopt;
	}
	core::Result<std::shared_ptr<const GameServerConfig>> r = std::move(pending_->result);
	pending_.reset();
	return r;
}

net::NextLevel Rotation::next_identity(const LevelEntry &p_entry) const {
	net::NextLevel n;
	n.level = p_entry.level;
	n.mode = p_entry.mode;
	std::lock_guard lock(mutex_);
	if (pending_ != nullptr && pending_->done && pending_->result && same_level(pending_->entry, p_entry) &&
			(*pending_->result)->level != nullptr) {
		const SharedLevel &l = *(*pending_->result)->level;
		n.source_digest = l.source_digest;
		n.logic_digest = l.logic_digest;
	} else if (current_config_ != nullptr && current_config_->level != nullptr &&
			same_level(cursor_.current(), p_entry)) {
		n.source_digest = current_config_->level->source_digest;
		n.logic_digest = current_config_->level->logic_digest;
	}
	return n;
}

uint16_t Rotation::intermission_s() const {
	if (config_.intermission_override_s >= 0.0f) {
		return uint16_t(std::min(std::lround(config_.intermission_override_s), 65535l));
	}
	if (current_config_ != nullptr && current_config_->level != nullptr && current_config_->level->eor != nullptr) {
		const float s = current_config_->level->eor->restart_delay_s;
		if (std::isfinite(s) && s >= 0.0f) {
			return uint16_t(std::min(std::lround(s), 65535l));
		}
	}
	return config_.intermission_s;
}

bool Rotation::level_over(const GameServer &p_server) const {
	if (config_.intermission_override_s < 0.0f) {
		return p_server.level_ended();
	}
	return announced_ &&
			p_server.current_tick() - round_over_tick_ >= sim::seconds_to_ticks(config_.intermission_override_s);
}

std::unique_ptr<GameServer> Rotation::after_tick(GameServer &r_server) {
	const TdmRules *rules = r_server.rules();
	if (rules == nullptr) {
		return nullptr;
	}
	if (!announced_ && !changing_ && rules->phase() == net::MatchPhase::RoundOver) {
		announced_ = true;
		round_over_tick_ = r_server.current_tick();
		const LevelEntry next = cursor_.next_level();
		if (!same_level(next, cursor_.current())) {
			start_load(next);
		}
		net::RoundOverInfo info;
		info.winner = uint8_t(std::min<int>(rules->match_state().winner, 2));
		info.next = next_identity(next);
		info.intermission_s = intermission_s();
		r_server.announce_round_over(info);
		std::lock_guard lock(mutex_);
		++stats_.round_overs;
	}
	if (!changing_ && level_over(r_server)) {
		announced_ = false;
		const bool changes = cursor_.round_over();
		if (!changes) {
			r_server.restart_round();
			std::lock_guard lock(mutex_);
			++stats_.in_place;
			stats_.index = cursor_.index();
			stats_.current = cursor_.current();
			return nullptr;
		}
		begin_change(r_server);
		return nullptr;
	}
	if (!changing_) {
		return nullptr;
	}
	std::optional<core::Result<std::shared_ptr<const GameServerConfig>>> loaded = take_load(cursor_.current());
	if (!loaded) {
		return nullptr;
	}
	std::string error;
	std::unique_ptr<GameServer> next;
	if (*loaded) {
		GameServerConfig c = ***loaded;
		c.round_id = round_id_;
		c.rules.hold_level_end = true;
		c.carried_seats = r_server.seats_to_carry();
		next = std::make_unique<GameServer>();
		const core::Status st = next->init(c);
		if (st.ok()) {
			r_server.release_seats();
			current_config_ = std::make_shared<const GameServerConfig>(std::move(c));
		} else {
			error = st.error().message;
			next.reset();
		}
	} else {
		error = loaded->error().message;
	}
	if (next == nullptr) {
		{
			std::lock_guard lock(mutex_);
			++stats_.load_failures;
			stats_.last_error = cursor_.current().level + " " + cursor_.current().mode + ": " + error;
		}
		cursor_.round_over();
		begin_change(r_server);
		return nullptr;
	}
	changing_ = false;
	std::lock_guard lock(mutex_);
	stats_.changing = false;
	++stats_.level_changes;
	return next;
}

void Rotation::begin_change(GameServer &r_server) {
	changing_ = true;
	++round_id_;
	start_load(cursor_.current());
	net::LevelChange change;
	change.level = cursor_.current().level;
	change.mode = cursor_.current().mode;
	change.round_id = round_id_;
	const LoadDeadlines deadlines = current_config_ != nullptr ? current_config_->load : LoadDeadlines{};
	change.load_deadline_s = uint16_t(std::clamp(std::lround(deadlines.warm_s), 0l, 65535l));
	r_server.begin_level_change(change, next_identity(cursor_.current()));
	std::lock_guard lock(mutex_);
	stats_.changing = true;
	stats_.round_id = round_id_;
	stats_.index = cursor_.index();
	stats_.current = cursor_.current();
}

RotationStats Rotation::stats() const {
	std::lock_guard lock(mutex_);
	return stats_;
}

}
