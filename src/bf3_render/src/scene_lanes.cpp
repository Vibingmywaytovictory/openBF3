// Phase B of the level load: the scene lanes (docs/design/m1.md §12.1, §12.2; level_scene.h SceneLanes). Godot-free.
//
// Scheduling: one queue per kind, taken under one mutex. A lane takes, in this order, the next terrain, a texture
// while fewer than texture_lanes lanes decode textures, a mesh request (its blob once, then every plan mesh of it
// on the same lane), and otherwise waits for a texture slot or ends. Before handing an item to the sink a lane
// waits while the in-flight bytes plus the item's would cross max_inflight_bytes (an item is admitted alone when
// nothing is in flight, so one item larger than the cap cannot stall the load); release_bytes and cancel wake it.
#include "bf3/render/level_scene.h"

#include "level_scene_internal.h"

#include "bf3/core/superbundle_mount.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace bf3::render {

namespace {

using Clock = std::chrono::steady_clock;

double seconds(Clock::duration p_d) {
	return std::chrono::duration<double>(p_d).count();
}

} // namespace

struct SceneLanes::Impl {
	const LevelScenePlan *plan = nullptr;
	const core::LevelMount *mount = nullptr;
	core::Cache *cache = nullptr;
	SceneSink *sink = nullptr;
	SceneLaneOptions options;

	std::vector<uint32_t> requests; // requests with at least one plan mesh, in plan order
	std::vector<std::vector<uint32_t>> request_meshes; // per request: its plan meshes

	mutable std::mutex mutex;
	std::condition_variable wake;
	size_t next_terrain = 0, next_texture = 0, next_request = 0;
	unsigned textures_running = 0;
	uint64_t inflight = 0;
	SceneLaneStats stats; // under mutex
	Clock::time_point start;
	Clock::time_point last_handed;
	bool any_handed = false;

	std::atomic<bool> cancelled{ false };
	std::atomic<unsigned> running{ 0 };
	std::vector<std::thread> threads;
	bool started = false;
	bool joined = false;

	enum class Kind { None, Terrain, Texture, Mesh };
	struct Item {
		Kind kind = Kind::None;
		size_t index = 0;
	};

	Item take() {
		std::unique_lock<std::mutex> lock(mutex);
		for (;;) {
			if (cancelled) {
				return {};
			}
			if (next_terrain < plan->terrains.size()) {
				return { Kind::Terrain, next_terrain++ };
			}
			if (next_texture < plan->textures.size() && textures_running < options.texture_lanes) {
				++textures_running;
				return { Kind::Texture, next_texture++ };
			}
			if (next_request < requests.size()) {
				return { Kind::Mesh, requests[next_request++] };
			}
			if (next_texture < plan->textures.size() && options.texture_lanes > 0) {
				wake.wait(lock); // a texture lane ends, or cancel
				continue;
			}
			return {};
		}
	}

	// Waits until the item fits under the cap; false when cancelled.
	bool admit(uint64_t p_bytes) {
		std::unique_lock<std::mutex> lock(mutex);
		wake.wait(lock, [&] { return cancelled || inflight == 0 || inflight + p_bytes <= options.max_inflight_bytes; });
		if (cancelled) {
			return false;
		}
		inflight += p_bytes;
		stats.peak_inflight_bytes = std::max(stats.peak_inflight_bytes, inflight);
		return true;
	}

	void handed() {
		std::lock_guard<std::mutex> lock(mutex);
		last_handed = Clock::now();
		any_handed = true;
	}

	void fail(const std::string &p_item, const std::string &p_error) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			++stats.failures;
		}
		sink->failure(p_item, p_error);
	}

	void run_terrain(size_t p_index) {
		const Clock::time_point t0 = Clock::now();
		core::Result<PreparedMesh> mesh = prepare_scene_terrain(*mount, *plan, uint32_t(p_index));
		const double work = seconds(Clock::now() - t0);
		{
			std::lock_guard<std::mutex> lock(mutex);
			stats.terrain_seconds += work;
		}
		if (!mesh) {
			fail(plan->terrains[p_index].asset, mesh.error().message);
			return;
		}
		const uint64_t bytes = scene_internal::prepared_bytes(*mesh);
		if (!admit(bytes)) {
			return;
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			++stats.terrain_items;
			stats.terrain_bytes += bytes;
		}
		sink->terrain(uint32_t(p_index), std::move(*mesh), bytes);
		handed();
	}

	void run_texture(size_t p_index) {
		const Clock::time_point t0 = Clock::now();
		core::Result<PreparedTexture> texture = prepare_scene_texture(*mount, *plan, uint32_t(p_index));
		const double work = seconds(Clock::now() - t0);
		{
			std::lock_guard<std::mutex> lock(mutex);
			stats.texture_seconds += work;
			--textures_running;
		}
		wake.notify_all();
		if (!texture) {
			fail(plan->textures[p_index].name, texture.error().message);
			return;
		}
		const uint64_t bytes = scene_internal::prepared_bytes(*texture);
		if (!admit(bytes)) {
			return;
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			++stats.texture_items;
			stats.texture_bytes += bytes;
		}
		sink->texture(uint32_t(p_index), std::move(*texture), bytes);
		handed();
	}

	void run_request(size_t p_request) {
		Clock::time_point t0 = Clock::now();
		core::Result<CachedMesh> blob = load_scene_request(cache, *mount, *plan, uint32_t(p_request));
		double work = seconds(Clock::now() - t0);
		if (!blob) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				stats.mesh_seconds += work;
			}
			fail(plan->requests[p_request].res, blob.error().message);
			return;
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			stats.mesh_seconds += work;
			if (blob->item.outcome == core::CacheOutcome::Hit) {
				++stats.cache_hits;
			} else if (blob->item.outcome == core::CacheOutcome::Converted || blob->item.outcome == core::CacheOutcome::Reused) {
				++stats.cache_converted;
			}
		}
		for (uint32_t mesh : request_meshes[p_request]) {
			if (cancelled) {
				return;
			}
			t0 = Clock::now();
			core::Result<PreparedSceneMesh> prepared = prepare_scene_mesh(cache, *mount, *plan, mesh, blob->mesh);
			work = seconds(Clock::now() - t0);
			{
				std::lock_guard<std::mutex> lock(mutex);
				stats.mesh_seconds += work;
			}
			if (!prepared) {
				fail(plan->requests[p_request].res + " (scene mesh " + std::to_string(mesh) + ")", prepared.error().message);
				continue;
			}
			prepared->item = blob->item;
			const uint64_t bytes = scene_internal::prepared_bytes(prepared->lod0);
			if (!admit(bytes)) {
				return;
			}
			{
				std::lock_guard<std::mutex> lock(mutex);
				++stats.mesh_items;
				stats.mesh_bytes += bytes;
			}
			sink->mesh(std::move(*prepared), bytes);
			handed();
		}
	}

	void lane() {
		for (;;) {
			const Item item = take();
			if (item.kind == Kind::None) {
				break;
			}
			switch (item.kind) {
				case Kind::Terrain:
					run_terrain(item.index);
					break;
				case Kind::Texture:
					run_texture(item.index);
					break;
				case Kind::Mesh:
					run_request(item.index);
					break;
				case Kind::None:
					break;
			}
		}
		running.fetch_sub(1);
		wake.notify_all();
	}

	void join() {
		for (std::thread &t : threads) {
			if (t.joinable()) {
				t.join();
			}
		}
		threads.clear();
		if (!joined) {
			joined = true;
			std::lock_guard<std::mutex> lock(mutex);
			stats.wall_seconds = any_handed ? seconds(last_handed - start) : seconds(Clock::now() - start);
		}
	}
};

SceneLanes::SceneLanes() :
		impl_(std::make_unique<Impl>()) {}

SceneLanes::~SceneLanes() {
	cancel();
	impl_->join();
}

core::Status SceneLanes::start(const LevelScenePlan &p_plan, const core::LevelMount &p_mount, core::Cache *p_cache, const SceneLaneOptions &p_options,
		SceneSink &p_sink) {
	Impl &d = *impl_;
	if (d.started) {
		return core::make_error("scene lanes: already started");
	}
	if (p_options.lanes == 0 || p_options.lanes > 256) {
		return core::make_error("scene lanes: lanes must be 1..256");
	}
	d.plan = &p_plan;
	d.mount = &p_mount;
	d.cache = p_cache;
	d.sink = &p_sink;
	d.options = p_options;
	d.options.texture_lanes = std::min(std::max(p_options.texture_lanes, 1u), p_options.lanes);
	d.request_meshes.assign(p_plan.requests.size(), {});
	for (uint32_t m = 0; m < p_plan.meshes.size(); ++m) {
		const uint32_t r = p_plan.meshes[m].request;
		if (r >= p_plan.requests.size()) {
			return core::make_error("scene lanes: scene mesh " + std::to_string(m) + " names request " + std::to_string(r) + " out of range");
		}
		d.request_meshes[r].push_back(m);
	}
	for (uint32_t r = 0; r < p_plan.requests.size(); ++r) {
		if (!d.request_meshes[r].empty()) {
			d.requests.push_back(r);
		}
	}
	d.started = true;
	d.start = Clock::now();
	d.running = p_options.lanes;
	d.threads.reserve(p_options.lanes);
	for (unsigned i = 0; i < p_options.lanes; ++i) {
		d.threads.emplace_back([&d] { d.lane(); });
	}
	return core::Status::success();
}

void SceneLanes::release_bytes(uint64_t p_bytes) {
	Impl &d = *impl_;
	{
		std::lock_guard<std::mutex> lock(d.mutex);
		d.inflight = p_bytes > d.inflight ? 0 : d.inflight - p_bytes;
	}
	d.wake.notify_all();
}

void SceneLanes::cancel() {
	Impl &d = *impl_;
	{
		std::lock_guard<std::mutex> lock(d.mutex);
		d.cancelled = true;
	}
	d.wake.notify_all();
}

bool SceneLanes::done() const {
	return impl_->started && impl_->running.load() == 0;
}

core::Result<SceneLaneStats> SceneLanes::wait() {
	Impl &d = *impl_;
	if (!d.started) {
		return core::make_error("scene lanes: not started");
	}
	d.join();
	std::lock_guard<std::mutex> lock(d.mutex);
	return d.stats;
}

} // namespace bf3::render
