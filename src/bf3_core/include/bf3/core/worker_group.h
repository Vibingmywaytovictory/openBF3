#pragma once
// Persistent worker lanes for per-frame jobs (S3 crowd; docs/design/s3.md §3; plan §2.3 "Animation pose
// evaluation ... core thread_pool").
//
// parallel_for (thread_pool.h) starts its threads on every call and makes the caller a lane, so the caller
// blocks until the job ends. That suits loaders. A display frame needs the opposite: the main thread starts
// the job and returns at once, does other work, and collects the result one frame later. Godot's
// WorkerThreadPool does not fit either: the project caps it at 4 threads, and the level upload (M1 C1) and
// Godot's own tasks share it.
//
//   WorkerGroup group(6, "s3 pose");
//   group.kick(64, [&](size_t i) { evaluator.evaluate(i); });   // returns at once
//   ...                                                          // the frame renders
//   const double blocked = group.wait();                         // 0 when the job already finished
//
// The lanes are std::threads created once and joined in the destructor. One job runs at a time. Items are
// pulled from an atomic counter, so an item runs exactly once, on some lane, in no fixed order.
// kick, wait and the destructor are called from ONE owning thread (the main thread in S3). The work
// function must not call back into the group.

#include <cstddef>
#include <functional>
#include <string_view>

namespace bf3::core {

class WorkerGroup {
public:
	static constexpr unsigned kMaxLanes = 32;

	// Starts p_lanes threads (clamped to [1, kMaxLanes]). They wait on a condition variable while idle.
	// p_name labels them in debuggers ("<name> <i>").
	explicit WorkerGroup(unsigned p_lanes, std::string_view p_name = "worker");
	// Waits for a running job, then stops and joins the lanes.
	~WorkerGroup();
	WorkerGroup(const WorkerGroup &) = delete;
	WorkerGroup &operator=(const WorkerGroup &) = delete;

	unsigned lanes() const;
	// Starts p_work(i) for every i in [0, p_count) and returns without waiting. Returns false, and starts
	// nothing, while the previous job is still running (call wait() first). p_count == 0 starts nothing and
	// returns true. p_work and everything it captures must stay valid until wait() returns.
	bool kick(size_t p_count, std::function<void(size_t)> p_work);
	// Blocks until the current job is done. Returns the seconds spent blocked (0 when no job was running or it
	// had already finished).
	double wait();
	// True between kick and the end of the job (the job may already be done when wait() has not been called).
	bool busy() const;

private:
	struct Impl;
	Impl *impl_ = nullptr;
};

} // namespace bf3::core
