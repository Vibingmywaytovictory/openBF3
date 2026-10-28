#include "catalog_weapons.h"

#include "bf3/core/job_queue.h"
#include "bf3/core/util/elapsed.h"

#include <chrono>
#include <mutex>
#include <set>
#include <span>
#include <utility>

namespace bf3::godot_side {

struct CatalogWeapons::Impl {
	std::unique_ptr<render::ViewmodelSources> sources;
	CatalogCacheSource cache;
	mutable std::mutex mutex;
	std::set<std::pair<uint8_t, uint16_t>> viewmodels_requested;
	std::vector<CatalogViewmodel> viewmodels_done;
	CatalogWeaponsStats stats;
	core::JobQueue jobs{ "catalog weapons" };

	void load_viewmodel(uint8_t p_team, uint16_t p_def, const std::string &p_unlock) {
		const auto t0 = std::chrono::steady_clock::now();
		CatalogViewmodel out;
		out.team = p_team;
		out.def = p_def;
		out.unlock = p_unlock;
		const std::shared_ptr<core::Cache> c = cache ? cache() : nullptr;
		render::ViewmodelSetSpec spec =
				render::team_viewmodel_items(p_team, std::span<const std::string>(&p_unlock, 1));
		spec.superbundle = sources->superbundle;
		core::Result<std::unique_ptr<render::ViewmodelSet>> set =
				render::load_viewmodel_set(*sources, spec, nullptr, c.get());
		if (set) {
			out.set = std::move(*set);
		} else {
			out.error = set.error().message;
		}
		out.seconds = core::seconds_since(t0);
		std::lock_guard lock(mutex);
		++(out.set ? stats.viewmodel_loaded : stats.viewmodel_failed);
		viewmodels_done.push_back(std::move(out));
	}
};

CatalogWeapons::CatalogWeapons(std::unique_ptr<render::ViewmodelSources> p_sources, CatalogCacheSource p_cache) :
		impl_(std::make_unique<Impl>()) {
	impl_->sources = std::move(p_sources);
	impl_->cache = std::move(p_cache);
}

CatalogWeapons::~CatalogWeapons() {
	impl_->jobs.stop();
}

bool CatalogWeapons::request_viewmodel(uint8_t p_team, uint16_t p_def, const std::string &p_unlock) {
	Impl &m = *impl_;
	if (m.sources == nullptr || (p_team != 1 && p_team != 2) || p_unlock.empty()) {
		return false;
	}
	{
		std::lock_guard lock(m.mutex);
		if (!m.viewmodels_requested.insert({ p_team, p_def }).second) {
			return false;
		}
		++m.stats.viewmodel_requests;
	}
	return m.jobs.post([&m, p_team, p_def, p_unlock] { m.load_viewmodel(p_team, p_def, p_unlock); });
}

bool CatalogWeapons::viewmodel_requested(uint8_t p_team, uint16_t p_def) const {
	std::lock_guard lock(impl_->mutex);
	return impl_->viewmodels_requested.count({ p_team, p_def }) != 0;
}

std::vector<CatalogViewmodel> CatalogWeapons::take_viewmodels() {
	std::lock_guard lock(impl_->mutex);
	return std::exchange(impl_->viewmodels_done, {});
}

CatalogWeaponsStats CatalogWeapons::stats() const {
	const core::JobQueueStats jobs = impl_->jobs.stats();
	std::lock_guard lock(impl_->mutex);
	CatalogWeaponsStats out = impl_->stats;
	out.pending = jobs.pending;
	out.busy_seconds = jobs.busy_seconds;
	return out;
}

}
