#include "bf3/host/client_session.h"

#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace bf3::host {

namespace {

bool names(const net::NextLevel &p_next, std::string_view p_level, std::string_view p_mode) {
	return core::iequals_ascii(p_next.level, p_level) && core::iequals_ascii(p_next.mode, p_mode);
}

}

struct ClientPrepare::Run {
	std::thread thread;
	std::atomic<uint16_t> permille{ 0 };
	std::atomic<bool> cold{ false };
	std::atomic<bool> stop{ false };
	std::atomic<bool> done{ false };
	LoadProgress progress;
	mutable std::mutex mutex;
	PrepareStatus status;

	~Run() {
		stop.store(true, std::memory_order_release);
		if (thread.joinable()) {
			thread.join();
		}
	}

	PrepareStatus snapshot() const {
		std::lock_guard lock(mutex);
		PrepareStatus s = status;
		s.running = !done.load(std::memory_order_acquire);
		if (s.running) {
			s.permille =
					uint16_t(std::max<uint32_t>(permille.load(std::memory_order_acquire), progress.permille() / 2));
			s.cold = cold.load(std::memory_order_acquire) ||
					progress.warmth.load(std::memory_order_acquire) == uint8_t(CacheWarmth::Cold);
		}
		return s;
	}
};

ClientPrepare::ClientPrepare(PrepareJob p_job, unsigned p_threads) :
		job_(std::move(p_job)), threads_(std::clamp(p_threads, 1u, kPrepareMaxThreads)) {}

ClientPrepare::~ClientPrepare() = default;

void ClientPrepare::start(const ClientRoundOver &p_round_over) {
	run_.reset();
	auto run = std::make_shared<Run>();
	run->status.serial = p_round_over.serial;
	run->status.next = p_round_over.info.next;
	run->status.round_id = p_round_over.round_id;
	if (names(p_round_over.info.next, p_round_over.level, p_round_over.mode) || !job_) {
		run->status.state = net::PrepareState::Ready;
		run->status.permille = 1000;
		run->status.skipped = true;
		run->done.store(true, std::memory_order_release);
		run_ = std::move(run);
		return;
	}
	run->status.state = net::PrepareState::Preparing;
	Run *r = run.get();
	run->thread = std::thread([r, job = job_, threads = threads_] {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		PrepareTask task;
		{
			std::lock_guard lock(r->mutex);
			task.next = r->status.next;
		}
		task.threads = threads;
		task.permille = &r->permille;
		task.cold = &r->cold;
		task.stop = &r->stop;
		task.progress = &r->progress;
		const auto t0 = std::chrono::steady_clock::now();
		core::Result<PrepareOutcome> outcome = job(task);
		std::lock_guard lock(r->mutex);
		r->status.seconds = core::seconds_since(t0);
		r->status.cold = r->cold.load(std::memory_order_acquire) || (outcome && outcome->cold) ||
				r->progress.warmth.load(std::memory_order_acquire) == uint8_t(CacheWarmth::Cold);
		if (!outcome) {
			r->status.state = net::PrepareState::None;
			r->status.permille = 0;
			r->status.error = outcome.error().message;
		} else if (task.next.source_digest && outcome->source_digest &&
				!(*task.next.source_digest == *outcome->source_digest)) {
			r->status.state = net::PrepareState::Mismatch;
			r->status.permille = 1000;
		} else {
			r->status.state = net::PrepareState::Ready;
			r->status.permille = 1000;
		}
		r->done.store(true, std::memory_order_release);
	});
	run_ = std::move(run);
}

void ClientPrepare::poll(ClientSession &r_client) {
	ClientRoundOver round_over;
	if (r_client.round_over(round_over) && round_over.serial != seen_serial_) {
		seen_serial_ = round_over.serial;
		queued_ = std::move(round_over);
	}
	if (queued_) {
		if (run_ != nullptr && !run_->done.load(std::memory_order_acquire)) {
			run_->stop.store(true, std::memory_order_release);
		} else {
			start(*queued_);
			queued_.reset();
		}
	}
	report(r_client);
}

void ClientPrepare::report(ClientSession &r_client) const {
	if (run_ == nullptr) {
		return;
	}
	const PrepareStatus s = run_->snapshot();
	net::Prepare p;
	p.round_id = s.round_id;
	p.next_source_digest = s.next.source_digest;
	p.state = s.state;
	p.permille = std::min<uint16_t>(s.permille, 1000);
	r_client.set_prepare_progress(p);
	net::LevelChange change;
	if (s.running && r_client.level_change(change) && holds(change)) {
		net::Loading l;
		l.round_id = change.round_id;
		l.stage = uint8_t(LoadStage::Mount);
		l.permille = p.permille;
		l.warmth = s.cold ? net::LoadWarmth::Cold : net::LoadWarmth::Warm;
		r_client.set_load_progress(l);
	}
}

bool ClientPrepare::holds(const net::LevelChange &p_change) const {
	return run_ != nullptr && !run_->done.load(std::memory_order_acquire) &&
			names(run_->snapshot().next, p_change.level, p_change.mode);
}

PrepareStatus ClientPrepare::status() const {
	return run_ != nullptr ? run_->snapshot() : PrepareStatus{};
}

}
