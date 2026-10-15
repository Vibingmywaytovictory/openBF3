// PathService (bots/walker_brain.h; docs/design/s1.md §6.1, §7): every bot's sliced A* on the server thread's one
// NavQuery, first come first served, at most iterations_per_tick A* iterations per tick across all requests.
#include "bf3/sim/bots/walker_brain.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <optional>
#include <unordered_map>

namespace bf3::sim::bots {

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point p_start) {
	return std::chrono::duration<double, std::milli>(Clock::now() - p_start).count();
}

} // namespace

void WalkerStats::add(const WalkerStats &p_other) {
	ticks += p_other.ticks;
	thinks += p_other.thinks;
	goals_started += p_other.goals_started;
	goals_reached += p_other.goals_reached;
	goals_abandoned += p_other.goals_abandoned;
	path_requests += p_other.path_requests;
	path_partial += p_other.path_partial;
	path_failed += p_other.path_failed;
	repaths += p_other.repaths;
	stuck_events += p_other.stuck_events;
	off_mesh_ticks += p_other.off_mesh_ticks;
	sprint_ticks += p_other.sprint_ticks;
	crouch_ticks += p_other.crouch_ticks;
	metres += p_other.metres;
}

struct PathService::Impl {
	struct Request {
		uint32_t slot = 0;
		NavPolyRef start = 0;
		Vec3 start_point{};
		NavPolyRef goal = 0;
		Vec3 goal_point{};
	};
	struct Answer {
		NavPathStatus status = NavPathStatus::Idle;
		std::vector<NavPolyRef> polys;
	};
	NavQuery *query = nullptr;
	Options options;
	std::deque<Request> queue;
	std::optional<Request> active;
	std::unordered_map<uint32_t, Answer> answers;
	PathServiceStats stats;
	std::vector<NavPolyRef> scratch;

	void answer(uint32_t p_slot, NavPathStatus p_status, std::vector<NavPolyRef> p_polys) {
		Answer &a = answers[p_slot];
		a.status = p_status;
		a.polys = std::move(p_polys);
		++stats.answers;
	}
};

PathService::PathService(NavQuery &p_query, const Options &p_options) :
		impl_(std::make_unique<Impl>()) {
	impl_->query = &p_query;
	impl_->options = p_options;
	impl_->options.iterations_per_tick = std::max<uint32_t>(p_options.iterations_per_tick, 1);
}

PathService::~PathService() = default;

void PathService::request(uint32_t p_slot, NavPolyRef p_start, const Vec3 &p_start_point, NavPolyRef p_goal, const Vec3 &p_goal_point) {
	Impl &m = *impl_;
	++m.stats.requests;
	m.answers.erase(p_slot);
	Impl::Request r{ p_slot, p_start, p_start_point, p_goal, p_goal_point };
	if (m.active && m.active->slot == p_slot) {
		m.query->cancel_path();
		m.active.reset();
		m.queue.push_front(r);
		return;
	}
	for (Impl::Request &q : m.queue) {
		if (q.slot == p_slot) {
			q = r;
			return;
		}
	}
	m.queue.push_back(r);
	m.stats.pending_max = std::max<uint32_t>(m.stats.pending_max, uint32_t(m.queue.size() + (m.active ? 1 : 0)));
}

void PathService::run() {
	Impl &m = *impl_;
	const Clock::time_point start = Clock::now();
	++m.stats.runs;
	uint32_t budget = m.options.iterations_per_tick;
	uint32_t spent = 0;
	// Each request costs at least its begin (no iterations when Detour refuses it at once); the queue holds at most
	// one request per slot, so a run is bounded by the slots plus the iteration budget.
	size_t begins = 0;
	while (budget > 0) {
		if (!m.active) {
			if (m.queue.empty() || begins > m.queue.size() + 64) {
				break;
			}
			m.active = m.queue.front();
			m.queue.pop_front();
			++begins;
			const NavPathStatus st = m.query->begin_path(m.active->start, m.active->start_point, m.active->goal, m.active->goal_point);
			if (st == NavPathStatus::Failed) {
				m.answer(m.active->slot, NavPathStatus::Failed, {});
				m.active.reset();
				continue;
			}
		}
		uint32_t done = 0;
		NavPathStatus st = m.query->continue_path(budget, &done);
		budget -= std::min(budget, done);
		spent += done;
		if (st == NavPathStatus::InProgress) {
			if (done == 0) {
				break; // no progress possible this tick
			}
			continue;
		}
		if (st != NavPathStatus::Failed) {
			st = m.query->finish_path(m.scratch);
		}
		m.answer(m.active->slot, st == NavPathStatus::Idle ? NavPathStatus::Failed : st, st == NavPathStatus::Failed ? std::vector<NavPolyRef>() : m.scratch);
		m.active.reset();
	}
	if (budget == 0 && (m.active || !m.queue.empty())) {
		++m.stats.ticks_at_budget;
	}
	m.stats.iterations += spent;
	m.stats.iterations_last = spent;
	const double ms = ms_since(start);
	m.stats.ms_run += ms;
	m.stats.ms_run_max = std::max(m.stats.ms_run_max, ms);
}

NavPathStatus PathService::take(uint32_t p_slot, std::vector<NavPolyRef> &r_polys) {
	Impl &m = *impl_;
	r_polys.clear();
	const auto it = m.answers.find(p_slot);
	if (it == m.answers.end()) {
		return NavPathStatus::Idle;
	}
	const NavPathStatus st = it->second.status;
	r_polys = std::move(it->second.polys);
	m.answers.erase(it);
	return st;
}

size_t PathService::pending() const {
	return impl_->queue.size() + (impl_->active ? 1u : 0u);
}

const PathServiceStats &PathService::stats() const {
	return impl_->stats;
}

} // namespace bf3::sim::bots
