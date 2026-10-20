#include "bf3/fx/effect_library.h"

#include "bf3/core/thread_pool.h"
#include "fx_ebx.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace bf3::fx {

struct EffectLibrary::Impl {
	struct Entry {
		std::once_flag once;
		std::shared_ptr<const EffectDef> def;
		std::string error;
		std::vector<AtlasBinding> bindings;
	};
	Sources sources;
	std::mutex mutex;
	std::unordered_map<std::string, std::shared_ptr<Entry>> by_name;
	std::unordered_map<const EffectDef *, std::shared_ptr<Entry>> by_def;
	core::ConvertTotals totals;
};

EffectLibrary::EffectLibrary(const Sources &p_sources) :
		impl_(std::make_unique<Impl>()) {
	impl_->sources = p_sources;
}

EffectLibrary::~EffectLibrary() = default;

core::Result<std::shared_ptr<const EffectDef>> EffectLibrary::get(std::string_view p_blueprint) {
	const std::string name = detail::canonical_name(p_blueprint);
	std::shared_ptr<Impl::Entry> entry;
	{
		const std::lock_guard<std::mutex> lock(impl_->mutex);
		std::shared_ptr<Impl::Entry> &slot = impl_->by_name[name];
		if (!slot) {
			slot = std::make_shared<Impl::Entry>();
		}
		entry = slot;
	}

	std::call_once(entry->once, [&]() {
		core::Result<CachedEffect> c = compile_effect_cached(impl_->sources.cache, impl_->sources.fx, name, &impl_->totals);
		if (!c) {
			entry->error = c.error().message;
			return;
		}
		auto def = std::make_shared<const EffectDef>(std::move(c->def));
		entry->bindings.resize(def->emitters.size());
		if (const EmitterAtlas *atlas = impl_->sources.atlas) {
			for (size_t i = 0; i < def->emitters.size(); ++i) {
				const TextureRef &t = def->emitters[i].texture;
				entry->bindings[i].texture = t.texture.empty() ? nullptr : atlas->resolve(t.texture);
				entry->bindings[i].normal_map = t.normal_map.empty() ? nullptr : atlas->resolve(t.normal_map);
			}
		}
		entry->def = std::move(def);
		const std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->by_def[entry->def.get()] = entry;
	});
	if (!entry->def) {
		return core::make_error(entry->error);
	}
	return entry->def;
}

size_t EffectLibrary::preload(const std::vector<std::string> &p_blueprints, unsigned p_threads) {
	std::atomic<size_t> failed{ 0 };
	core::parallel_for(
			p_blueprints.size(),
			[&](size_t i) {
				if (!get(p_blueprints[i]).ok()) {
					++failed;
				}
			},
			p_threads);
	return failed.load();
}

std::span<const AtlasBinding> EffectLibrary::bindings(const EffectDef &p_def) const {
	const std::lock_guard<std::mutex> lock(impl_->mutex);
	const auto it = impl_->by_def.find(&p_def);
	if (it == impl_->by_def.end()) {
		return {};
	}
	return it->second->bindings;
}

const ImpactTable *EffectLibrary::impacts() const {
	return impl_->sources.impacts;
}

core::ConvertTotals &EffectLibrary::totals() {
	return impl_->totals;
}

}
