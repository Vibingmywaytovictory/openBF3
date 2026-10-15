// The bf3_fx measurement harness (fx_harness.h; docs/design/m2a-vfx1.md §7.7, §9).
#include "bf3/fx/fx_harness.h"

#include "bf3/fx/effect_compiler.h"
#include "fx_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace bf3::fx {


namespace {

std::string fmt(const char *p_fmt, double p_v) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), p_fmt, p_v);
	return buf;
}
std::string num(double p_v) {
	return fmt("%.9g", p_v);
}

bool ends_with(std::string_view p_s, std::string_view p_suffix) {
	return p_s.size() >= p_suffix.size() && p_s.compare(p_s.size() - p_suffix.size(), p_suffix.size(), p_suffix) == 0;
}

uint64_t fnv(uint64_t p_h, const void *p_bytes, size_t p_n) {
	const uint8_t *b = static_cast<const uint8_t *>(p_bytes);
	for (size_t i = 0; i < p_n; ++i) {
		p_h = (p_h ^ b[i]) * 0x100000001B3ull;
	}
	return p_h;
}
template <class T>
uint64_t fnv_value(uint64_t p_h, const T &p_v) {
	static_assert(std::is_trivially_copyable_v<T>);
	return fnv(p_h, &p_v, sizeof(T));
}

void add_stats(FxStats &r_sum, const FxStats &p_s) {
	r_sum.live_effects = std::max(r_sum.live_effects, p_s.live_effects);
	r_sum.live_emitters = std::max(r_sum.live_emitters, p_s.live_emitters);
	r_sum.live_particles = std::max(r_sum.live_particles, p_s.live_particles);
	r_sum.live_lights = std::max(r_sum.live_lights, p_s.live_lights);
	r_sum.steps += p_s.steps;
	r_sum.substeps += p_s.substeps;
	r_sum.dt_clamped += p_s.dt_clamped;
	r_sum.spawned += p_s.spawned;
	r_sum.died += p_s.died;
	r_sum.spawns_capped += p_s.spawns_capped;
	r_sum.spawns_dropped += p_s.spawns_dropped;
	r_sum.emitter_starts_dropped += p_s.emitter_starts_dropped;
	r_sum.nonfinite_kills += p_s.nonfinite_kills;
	r_sum.stolen_instances += p_s.stolen_instances;
	r_sum.refused_starts += p_s.refused_starts;
}

// The live_* fields of a report are maxima over every frame (the end-of-run values are usually 0).
void note_peak(FxStats &r_sum, const FxStats &p_s) {
	r_sum.live_effects = std::max(r_sum.live_effects, p_s.live_effects);
	r_sum.live_emitters = std::max(r_sum.live_emitters, p_s.live_emitters);
	r_sum.live_particles = std::max(r_sum.live_particles, p_s.live_particles);
	r_sum.live_lights = std::max(r_sum.live_lights, p_s.live_lights);
}

void stats_lines(const FxStats &p_s, std::vector<std::string> &r_lines) {
	r_lines.push_back("stats\tspawned\t" + std::to_string(p_s.spawned) + "\tdied\t" + std::to_string(p_s.died) + "\tspawns_capped\t" +
			std::to_string(p_s.spawns_capped) + "\tspawns_dropped\t" + std::to_string(p_s.spawns_dropped) + "\temitter_starts_dropped\t" +
			std::to_string(p_s.emitter_starts_dropped) + "\tnonfinite_kills\t" + std::to_string(p_s.nonfinite_kills) + "\tstolen_instances\t" +
			std::to_string(p_s.stolen_instances) + "\trefused_starts\t" + std::to_string(p_s.refused_starts) + "\tdt_clamped\t" +
			std::to_string(p_s.dt_clamped) + "\tframes\t" + std::to_string(p_s.steps) + "\tsubsteps\t" + std::to_string(p_s.substeps));
	r_lines.push_back("stats\tmax_live_particles\t" + std::to_string(p_s.live_particles) + "\tmax_live_lights\t" + std::to_string(p_s.live_lights) +
			"\tmax_live_emitters\t" + std::to_string(p_s.live_emitters));
}

uint64_t frames_of(const HarnessRun &p_run) {
	if (p_run.variable) {
		return UINT64_MAX;
	}
	const double d = double(p_run.dt) > 0 ? double(p_run.seconds) / double(p_run.dt) : 0.0;
	return uint64_t(std::max(1.0, std::ceil(d - 1e-6)));
}

bool vertex_finite(const ParticleVertex &p_v) {
	const float *f = reinterpret_cast<const float *>(&p_v);
	for (size_t i = 0; i < 19; ++i) { // every float field (the last 4 bytes are the serial)
		if (!std::isfinite(f[i])) {
			return false;
		}
	}
	return true;
}
bool light_finite(const ParticleLight &p_l) {
	const float *f = reinterpret_cast<const float *>(&p_l);
	for (size_t i = 0; i < 8; ++i) {
		if (!std::isfinite(f[i])) {
			return false;
		}
	}
	return true;
}
static_assert(sizeof(ParticleVertex) == 19 * 4 + 4);
static_assert(sizeof(ParticleLight) == 8 * 4);

// Lanes for item-parallel work: a worker per lane pulling item indices.
void for_lanes(size_t p_items, unsigned p_lanes, const std::function<void(unsigned, size_t)> &p_work) {
	const unsigned lanes = std::max(1u, std::min<unsigned>(p_lanes == 0 ? std::max(1u, std::thread::hardware_concurrency()) : p_lanes,
										 unsigned(std::max<size_t>(1, p_items))));
	std::atomic<size_t> next{ 0 };
	auto lane = [&](unsigned p_lane) {
		for (;;) {
			const size_t i = next.fetch_add(1);
			if (i >= p_items) {
				return;
			}
			p_work(p_lane, i);
		}
	};
	std::vector<std::thread> threads;
	for (unsigned l = 1; l < lanes; ++l) {
		threads.emplace_back(lane, l);
	}
	lane(0);
	for (std::thread &t : threads) {
		t.join();
	}
}

} // namespace

float fx_variable_dt(uint64_t p_seed, uint64_t p_frame) {
	const float u = fx_unit(fx_key(fx_mix64(p_seed ^ 0xD7D7D7D7ull), uint32_t(p_frame), uint32_t(p_frame >> 32), 0xD7u));
	return kVariableDtMin + (kVariableDtMax - kVariableDtMin) * u;
}

// ---- lane pool ---------------------------------------------------------------------------------------------

struct LanePool::Impl {
	unsigned lanes = 1;
	std::vector<std::thread> threads;
	std::mutex mutex;
	std::condition_variable wake;
	std::condition_variable done;
	uint64_t job = 0;
	bool quit = false;
	FxWorld *world = nullptr;
	size_t items = 0;
	size_t chunk = 1;
	std::atomic<size_t> next{ 0 };
	unsigned busy = 0;

	void work() {
		const size_t chunks = (items + chunk - 1) / chunk;
		for (;;) {
			const size_t c = next.fetch_add(1);
			if (c >= chunks) {
				return;
			}
			world->run_items(c * chunk, std::min(chunk, items - c * chunk));
		}
	}
	void loop() {
		uint64_t seen = 0;
		for (;;) {
			{
				std::unique_lock<std::mutex> lock(mutex);
				wake.wait(lock, [&]() { return quit || job != seen; });
				if (quit) {
					return;
				}
				seen = job;
			}
			work();
			{
				const std::lock_guard<std::mutex> lock(mutex);
				if (--busy == 0) {
					done.notify_all();
				}
			}
		}
	}
};

LanePool::LanePool(unsigned p_lanes) :
		impl_(std::make_unique<Impl>()) {
	impl_->lanes = std::max(1u, p_lanes);
	for (unsigned l = 1; l < impl_->lanes; ++l) {
		impl_->threads.emplace_back([this]() { impl_->loop(); });
	}
}

LanePool::~LanePool() {
	{
		const std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->quit = true;
	}
	impl_->wake.notify_all();
	for (std::thread &t : impl_->threads) {
		t.join();
	}
}

unsigned LanePool::lanes() const {
	return impl_->lanes;
}

void LanePool::run(FxWorld &r_world, size_t p_items, size_t p_chunk) {
	Impl &p = *impl_;
	if (p_items == 0) {
		return;
	}
	{
		const std::lock_guard<std::mutex> lock(p.mutex);
		p.world = &r_world;
		p.items = p_items;
		p.chunk = std::max<size_t>(1, p_chunk);
		p.next = 0;
		p.busy = unsigned(p.threads.size());
		++p.job;
	}
	p.wake.notify_all();
	p.work();
	std::unique_lock<std::mutex> lock(p.mutex);
	p.done.wait(lock, [&]() { return p.busy == 0; });
}

void fx_step_frame(FxWorld &r_world, float p_dt, LanePool *p_pool, size_t p_chunk) {
	const uint32_t k = r_world.begin_frame(p_dt, FxView{});
	for (uint32_t s = 0; s < k; ++s) {
		const size_t n = r_world.begin_step();
		if (p_pool != nullptr) {
			p_pool->run(r_world, n, p_chunk);
		} else {
			r_world.run_items(0, n);
		}
		r_world.end_step();
	}
}

uint64_t fx_frame_digest(const FxWorld &p_world) {
	uint64_t h = 0xCBF29CE484222325ull;
	const std::span<const ParticleVertex> p = p_world.particles();
	const std::span<const ParticleLight> l = p_world.lights();
	h = fnv_value(h, p.size());
	h = fnv(h, p.data(), p.size_bytes());
	h = fnv_value(h, l.size());
	h = fnv(h, l.data(), l.size_bytes());
	for (const EmitterOutput &o : p_world.outputs()) {
		// Not the handle generation: it keeps counting across clear(), so it tells only how many starts the world saw.
		h = fnv_value(h, o.effect.slot);
		h = fnv_value(h, reinterpret_cast<uintptr_t>(o.def));
		h = fnv_value(h, o.emitter);
		h = fnv_value(h, o.component);
		h = fnv_value(h, o.first_person);
		h = fnv_value(h, o.first_particle);
		h = fnv_value(h, o.particle_count);
		h = fnv_value(h, o.first_light);
		h = fnv_value(h, o.light_count);
		h = fnv_value(h, o.emitter_transform);
	}
	const FxStats &s = p_world.stats();
	const uint64_t fields[] = { s.live_effects, s.live_emitters, s.live_particles, s.live_lights, s.steps, s.substeps, s.dt_clamped, s.spawned, s.died,
		s.spawns_capped, s.spawns_dropped, s.emitter_starts_dropped, s.nonfinite_kills, s.stolen_instances, s.refused_starts };
	return fnv(h, fields, sizeof(fields));
}

core::Result<EffectHandle> fx_run_start(FxWorld &r_world, const std::shared_ptr<const EffectDef> &p_def, uint64_t p_index, const HarnessRun &p_run,
		const FrameHook &p_frame, LanePool *p_pool, size_t p_chunk) {
	r_world.clear();
	EffectStart s;
	s.seed = fx_harness_seed(p_run.seed, p_index);
	s.tier = p_run.tier;
	core::Result<EffectHandle> h = r_world.start(p_def, s);
	if (!h) {
		return h;
	}
	const uint64_t frames = frames_of(p_run);
	double time = 0;
	for (uint64_t k = 0; k < frames; ++k) {
		const float dt = p_run.variable ? fx_variable_dt(p_run.seed, k) : p_run.dt;
		fx_step_frame(r_world, dt, p_pool, p_chunk);
		time += double(dt);
		if (p_frame) {
			p_frame(r_world, *h, k, time, dt);
		}
		if (p_run.variable && time >= double(p_run.seconds)) {
			break;
		}
	}
	return h;
}

// ---- simulate ----------------------------------------------------------------------------------------------

namespace {

const char *kind_name(ComponentKind p_kind) {
	switch (p_kind) {
		case ComponentKind::Emitter:
			return "emitter";
		case ComponentKind::Sound:
			return "sound";
		case ComponentKind::VisualEnvironment:
			return "visual_environment";
	}
	return "?";
}

std::string component_label(const EffectDef &p_def, uint32_t p_c) {
	const EffectComponentDef &c = p_def.components[p_c];
	return c.kind == ComponentKind::Emitter ? p_def.emitters[c.emitter].name : c.asset;
}

} // namespace

HarnessReport fx_simulate_starts(const std::shared_ptr<const EffectDef> &p_def, uint32_t p_starts, const HarnessRun &p_run) {
	HarnessReport rep;
	FxWorld world;
	const size_t nc = p_def->components.size();
	std::vector<uint32_t> selected(nc, 0), observed(nc, 0);
	uint32_t still_alive = 0;
	std::vector<uint32_t> live_series; // start 0
	for (uint32_t i = 0; i < p_starts; ++i) {
		const uint64_t seed = fx_harness_seed(p_run.seed, i);
		std::vector<uint8_t> seen(nc, 0);
		const core::Result<EffectHandle> h = fx_run_start(world, p_def, i, p_run, [&](const FxWorld &p_w, EffectHandle, uint64_t, double, float) {
			note_peak(rep.stats, p_w.stats());
			for (const EmitterOutput &o : p_w.outputs()) {
				if (o.particle_count > 0 && o.component < nc) {
					seen[o.component] = 1;
				}
			}
			if (i == 0) {
				live_series.push_back(p_w.stats().live_particles);
			}
		});
		if (!h) {
			rep.failures.push_back("start " + std::to_string(i) + ": " + h.error().message);
			break;
		}
		for (uint32_t c = 0; c < nc; ++c) {
			selected[c] += fx_component_selected(*p_def, c, seed, p_run.tier) ? 1 : 0;
			observed[c] += seen[c];
		}
		still_alive += world.alive(*h) ? 1 : 0;
		add_stats(rep.stats, world.stats());
	}
	rep.lines.push_back("effect\t" + p_def->name + "\tstarts\t" + std::to_string(p_starts) + "\tseed\t" + std::to_string(p_run.seed) + "\tseconds\t" +
			num(p_run.seconds) + "\tdt\t" + (p_run.variable ? std::string("variable") : num(p_run.dt)));
	for (uint32_t c = 0; c < nc; ++c) {
		rep.lines.push_back("component\t" + std::to_string(c) + "\t" + kind_name(p_def->components[c].kind) + "\t" + component_label(*p_def, c) + "\tp\t" +
				num(p_def->components[c].spawn_probability) + "\tselected\t" + num(double(selected[c]) / std::max(1u, p_starts)) + "\tobserved\t" +
				num(double(observed[c]) / std::max(1u, p_starts)));
	}
	std::string series = "live_start0";
	for (size_t k = 0; k < live_series.size(); ++k) {
		series += (k == 0 ? "\t" : " ") + std::to_string(live_series[k]);
	}
	rep.lines.push_back(series);
	rep.lines.push_back("alive_at_end\t" + std::to_string(still_alive));
	stats_lines(rep.stats, rep.lines);
	rep.pass = rep.failures.empty();
	return rep;
}

// ---- G3: the 1p muzzle golden --------------------------------------------------------------------------------

HarnessReport fx_golden_muzzle1p(const std::shared_ptr<const EffectDef> &p_def, uint32_t p_starts, const HarnessRun &p_run) {
	HarnessReport rep;
	const auto fail = [&](const std::string &p_why) {
		if (rep.failures.size() < 64) {
			rep.failures.push_back(p_why);
		}
	};
	// Golden data (docs §5.1), compared against the def first so a changed install shows as a data difference.
	const PolynomialDef size_poly{ { 0.44260055f, -0.88148373f, 0.6358111f, 0.7980724f }, 0.0f, 1.0f, 1.0f };
	struct LightGolden {
		const char *suffix;
		float radius;
		Vec3 color;
		Vec4 coefficients;
	};
	const LightGolden light_golden[2] = { { "_1p_light_01", 0.2f, { 1000.0f, 395.072f, 65.754f }, { -1.7119899f, 3.966709f, -3.2640686f, 0.97097605f } },
		{ "_1p_light_02", 2.5f, { 5.0f, 1.875f, 0.255f }, { 0.07549087f, -0.72858727f, -0.3657508f, 1.0118451f } } };
	enum Role : uint8_t { kOther, kFlash, kLight1, kLight2, kRapid, kNozzle };
	const size_t nc = p_def->components.size();
	std::vector<Role> role(nc, kOther);
	for (uint32_t c = 0; c < nc; ++c) {
		const EffectComponentDef &comp = p_def->components[c];
		if (comp.kind != ComponentKind::Emitter) {
			continue;
		}
		const std::string &n = p_def->emitters[comp.emitter].name;
		role[c] = ends_with(n, "_1p_flash") ? kFlash : ends_with(n, "_1p_light_01") ? kLight1 : ends_with(n, "_1p_light_02") ? kLight2 :
				ends_with(n, "rapidsmoke_ls_01") ? kRapid : ends_with(n, "nozzleflames_ls_01") ? kNozzle : kOther;
	}
	std::map<Role, uint32_t> component_of;
	for (uint32_t c = 0; c < nc; ++c) {
		if (role[c] != kOther) {
			component_of[role[c]] = c;
		}
	}
	for (const Role r : { kFlash, kLight1, kLight2, kRapid, kNozzle }) {
		if (!component_of.count(r)) {
			fail("component missing: role " + std::to_string(int(r)));
		}
	}
	if (!rep.failures.empty()) {
		rep.pass = false;
		return rep;
	}
	const EmitterDef &flash = p_def->emitters[p_def->components[component_of[kFlash]].emitter];
	{
		const ProcessorSlot &us = flash.slot(ProcessorType::UpdateSize);
		if (!us.present || us.evaluator == kNoEvaluator || flash.evaluators[size_t(us.evaluator)].type != EvaluatorType::Polynomial ||
				!(flash.evaluators[size_t(us.evaluator)].polynomial == size_poly)) {
			fail("flash UpdateSize polynomial differs from the golden {w 0.7980724, x 0.44260055, y -0.88148373, z 0.6358111}");
		}
	}
	for (int l = 0; l < 2; ++l) {
		const EmitterDef &e = p_def->emitters[p_def->components[component_of[l == 0 ? kLight1 : kLight2]].emitter];
		if (!(e.tmpl.light.intensity.coefficients == light_golden[l].coefficients) || e.tmpl.light.radius != light_golden[l].radius ||
				!(e.tmpl.light.color == light_golden[l].color) || (e.tmpl.flags & kEmitterActAsPointLight) == 0) {
			fail(std::string(light_golden[l].suffix) + ": light data differs from the golden");
		}
	}
	FxWorld world;
	std::vector<uint32_t> selected(nc, 0), observed(nc, 0);
	uint32_t flash_starts = 0, flash_bad_count = 0, still_alive = 0;
	double life_min = 1e9, life_max = -1e9, base_min = 1e9, base_max = -1e9, ratio_err = 0, frame_min = 1e9, frame_max = -1e9;
	uint64_t lights_checked = 0, light_bad = 0;
	double light_err = 0;
	uint32_t rapid_max = 0;
	for (uint32_t i = 0; i < p_starts; ++i) {
		const uint64_t seed = fx_harness_seed(p_run.seed, i);
		std::vector<uint8_t> seen(nc, 0);
		std::set<uint32_t> flash_serials;
		struct Obs {
			float t = 0, base = 0;
			bool have = false;
		};
		std::map<uint32_t, Obs> last; // flash serial -> previous observation
		const core::Result<EffectHandle> h = fx_run_start(world, p_def, i, p_run, [&](const FxWorld &p_w, EffectHandle, uint64_t, double, float p_dt) {
			note_peak(rep.stats, p_w.stats());
			const std::span<const ParticleVertex> parts = p_w.particles();
			const std::span<const ParticleLight> lights = p_w.lights();
			for (const EmitterOutput &o : p_w.outputs()) {
				if (o.component >= nc) {
					continue;
				}
				if (o.particle_count > 0) {
					seen[o.component] = 1;
				}
				const Role r = role[o.component];
				if (r == kRapid) {
					rapid_max = std::max(rapid_max, o.particle_count);
				}
				if (r == kFlash) {
					for (uint32_t k = 0; k < o.particle_count; ++k) {
						const ParticleVertex &v = parts[o.first_particle + k];
						flash_serials.insert(v.serial);
						const float p = evaluate_polynomial(size_poly, v.age);
						const double base = double(v.size.x) / double(p);
						Obs &ob = last[v.serial];
						if (!ob.have) {
							frame_min = std::min(frame_min, double(v.frame));
							frame_max = std::max(frame_max, double(v.frame));
							base_min = std::min(base_min, base);
							base_max = std::max(base_max, base);
						} else {
							const double life = double(p_dt) / (double(v.age) - double(ob.t));
							if (v.age < 1.0f) {
								life_min = std::min(life_min, life);
								life_max = std::max(life_max, life);
							}
							ratio_err = std::max(ratio_err, std::fabs(base / double(ob.base) - 1.0));
						}
						ob.t = v.age;
						ob.base = float(base);
						ob.have = true;
					}
				}
				if (r == kLight1 || r == kLight2) {
					const LightGolden &g = light_golden[r == kLight1 ? 0 : 1];
					const EmitterDef &e = p_def->emitters[o.emitter];
					if (o.light_count != o.particle_count) {
						++light_bad;
					}
					for (uint32_t k = 0; k < std::min(o.light_count, o.particle_count); ++k) {
						const ParticleLight &l = lights[o.first_light + k];
						const ParticleVertex &v = parts[o.first_particle + k];
						const PolynomialDef ip{ g.coefficients, e.tmpl.light.intensity.min_clamp, e.tmpl.light.intensity.max_clamp, 1.0f };
						const float intensity = evaluate_polynomial(ip, v.age);
						const Vec3 want{ g.color.x * intensity, g.color.y * intensity, g.color.z * intensity };
						const auto rel = [](float p_a, float p_b) {
							return std::fabs(double(p_a) - double(p_b)) / std::max(1e-12, std::fabs(double(p_b)));
						};
						const double err = std::max({ rel(l.color.x, want.x), rel(l.color.y, want.y), rel(l.color.z, want.z), rel(l.intensity, intensity) });
						light_err = std::max(light_err, err);
						++lights_checked;
						if (l.radius != g.radius || err > 1e-5) {
							++light_bad;
						}
					}
				}
			}
		});
		if (!h) {
			fail("start " + std::to_string(i) + ": " + h.error().message);
			break;
		}
		for (uint32_t c = 0; c < nc; ++c) {
			const bool sel = fx_component_selected(*p_def, c, seed, p_run.tier);
			selected[c] += sel ? 1 : 0;
			observed[c] += seen[c];
			if (sel != (seen[c] != 0)) {
				fail("start " + std::to_string(i) + ": component " + std::to_string(c) + " selected " + std::to_string(int(sel)) + " but observed " +
						std::to_string(int(seen[c])));
			}
		}
		if (seen[component_of[kFlash]] != 0) {
			++flash_starts;
			if (flash_serials.size() != 1) {
				++flash_bad_count;
			}
		}
		still_alive += world.alive(*h) ? 1 : 0;
		add_stats(rep.stats, world.stats());
	}
	const double n = std::max(1u, p_starts);
	const auto frac = [&](Role r) { return double(observed[component_of[r]]) / n; };
	rep.lines.push_back("golden\tmuzzle1p\t" + p_def->name + "\tstarts\t" + std::to_string(p_starts) + "\tseed\t" + std::to_string(p_run.seed) + "\tdt\t" +
			num(p_run.dt));
	for (uint32_t c = 0; c < nc; ++c) {
		rep.lines.push_back("component\t" + std::to_string(c) + "\t" + component_label(*p_def, c) + "\tselected\t" + num(double(selected[c]) / n) +
				"\tobserved\t" + num(double(observed[c]) / n));
	}
	rep.lines.push_back("flash\tstarts\t" + std::to_string(flash_starts) + "\tnot_one_particle\t" + std::to_string(flash_bad_count) + "\tlife\t" +
			num(life_min) + "\t" + num(life_max) + "\tbase_size\t" + num(base_min) + "\t" + num(base_max) + "\tsize_ratio_err\t" + num(ratio_err) +
			"\tstart_frame\t" + num(frame_min) + "\t" + num(frame_max));
	rep.lines.push_back("lights\tchecked\t" + std::to_string(lights_checked) + "\tbad\t" + std::to_string(light_bad) + "\tmax_rel_err\t" + num(light_err));
	rep.lines.push_back("rapidsmoke\tmax_live\t" + std::to_string(rapid_max));
	rep.lines.push_back("alive_at_end\t" + std::to_string(still_alive));
	stats_lines(rep.stats, rep.lines);
	// The thresholds of gate G3.
	if (rep.stats.stolen_instances != 0 || rep.stats.refused_starts != 0) {
		fail("stolen_instances / refused_starts not 0");
	}
	for (const Role r : { kFlash, kLight1, kLight2 }) {
		if (std::fabs(frac(r) - 0.75) > 0.02) {
			fail("component fraction " + num(frac(r)) + " not 0.75 +- 0.02");
		}
	}
	if (frac(kRapid) != 1.0) {
		fail("rapidsmoke fraction " + num(frac(kRapid)) + " not 1");
	}
	if (std::fabs(frac(kNozzle) - 0.25) > 0.02) {
		fail("nozzleflames fraction " + num(frac(kNozzle)) + " not 0.25 +- 0.02");
	}
	if (flash_bad_count != 0) {
		fail("flash: " + std::to_string(flash_bad_count) + " starts without exactly one particle");
	}
	if (!(life_min >= 0.05 - 1e-6 && life_max <= 0.05 + 1e-6)) {
		fail("flash life [" + num(life_min) + ", " + num(life_max) + "] not 0.05 +- 1e-6");
	}
	if (!(base_min >= 0.32 * (1 - 1e-6) && base_max <= 0.40 * (1 + 1e-6))) {
		fail("flash spawn size / P(0) [" + num(base_min) + ", " + num(base_max) + "] not in [0.32, 0.40]");
	}
	if (ratio_err > 1e-5) {
		fail("flash size(t)/size(0) differs from P(t)/P(0) by " + num(ratio_err));
	}
	if (!(frame_min >= 1.0 && frame_max < 16.0 && frame_min < 1.01 && frame_max > 15.99)) {
		fail("flash start frame range [" + num(frame_min) + ", " + num(frame_max) + "]");
	}
	if (light_bad != 0 || lights_checked == 0) {
		fail("lights: " + std::to_string(light_bad) + " of " + std::to_string(lights_checked) + " off the golden");
	}
	if (rapid_max > 6) {
		fail("rapidsmoke live " + std::to_string(rapid_max) + " > 6");
	}
	if (still_alive != 0) {
		fail(std::to_string(still_alive) + " starts still alive at the end of their run");
	}
	rep.pass = rep.failures.empty();
	return rep;
}

// ---- G4: the oilfire steady state ------------------------------------------------------------------------------

HarnessReport fx_golden_oilfire(const std::shared_ptr<const EffectDef> &p_def, const HarnessRun &p_run) {
	HarnessReport rep;
	FxWorld world;
	uint32_t steady_min = UINT32_MAX, steady_max = 0, max_live = 0;
	uint64_t frames = 0;
	const core::Result<EffectHandle> h = fx_run_start(world, p_def, 0, p_run, [&](const FxWorld &p_w, EffectHandle, uint64_t, double p_time, float) {
		note_peak(rep.stats, p_w.stats());
		const uint32_t live = p_w.stats().live_particles;
		max_live = std::max(max_live, live);
		if (p_time > 15.0) {
			steady_min = std::min(steady_min, live);
			steady_max = std::max(steady_max, live);
		}
		++frames;
	});
	if (!h) {
		rep.failures.push_back(h.error().message);
		rep.pass = false;
		return rep;
	}
	add_stats(rep.stats, world.stats());
	rep.lines.push_back("golden\toilfire\t" + p_def->name + "\tseed\t" + std::to_string(p_run.seed) + "\tdt\t" + (p_run.variable ? std::string("variable") : num(p_run.dt)) +
			"\tframes\t" + std::to_string(frames));
	rep.lines.push_back("oilfire\tsteady_min\t" + std::to_string(steady_min) + "\tsteady_max\t" + std::to_string(steady_max) + "\tmax_live\t" +
			std::to_string(max_live) + "\tspawns_capped\t" + std::to_string(rep.stats.spawns_capped) + "\tspawns_dropped\t" +
			std::to_string(rep.stats.spawns_dropped));
	stats_lines(rep.stats, rep.lines);
	if (steady_min < 79 || steady_max > 81) {
		rep.failures.push_back("live after the 15 s warm-up in [" + std::to_string(steady_min) + ", " + std::to_string(steady_max) + "], not within [79, 81]");
	}
	if (max_live > 81) {
		rep.failures.push_back("max live " + std::to_string(max_live) + " > 81");
	}
	if (rep.stats.spawns_dropped != 0) {
		rep.failures.push_back("spawns_dropped " + std::to_string(rep.stats.spawns_dropped));
	}
	rep.pass = rep.failures.empty();
	return rep;
}

// ---- G5: impacts -----------------------------------------------------------------------------------------------

namespace {

std::string leaf(std::string_view p_name) {
	const size_t slash = p_name.rfind('/');
	return std::string(slash == std::string_view::npos ? p_name : p_name.substr(slash + 1));
}

std::string joined_leaves(const std::vector<std::string> &p_names) {
	std::string out;
	for (const std::string &n : p_names) {
		out += (out.empty() ? "" : ",") + leaf(n);
	}
	return out.empty() ? "-" : out;
}

std::string lower(std::string_view p_s) {
	std::string out(p_s);
	for (char &c : out) {
		if (c >= 'A' && c <= 'Z') {
			c = char(c - 'A' + 'a');
		}
	}
	return out;
}

} // namespace

ImpactCounts fx_impact_counts(const ImpactTable &p_table, uint8_t p_projectile) {
	ImpactCounts c;
	c.rows = p_table.rows();
	c.effect_relations = p_table.effect_relations();
	c.decal_relations = p_table.decal_relations();
	const std::span<const int64_t> map = p_table.index_map();
	if (p_projectile >= map.size() || map[p_projectile] < 0 || uint64_t(map[p_projectile]) >= c.rows) {
		return c;
	}
	const size_t row = size_t(map[p_projectile]);
	const int64_t def = p_table.default_material_index();
	const int64_t default_row = def >= 0 && uint64_t(def) < map.size() ? map[size_t(def)] : -1;
	for (size_t i = 0; i < map.size(); ++i) {
		const bool valid = map[i] >= 0 && uint64_t(map[i]) < c.rows;
		const ImpactCell *cell = valid ? &p_table.cell(row, size_t(map[i])) : nullptr;
		if (map[i] == default_row) {
			++c.default_row_indices;
			if (cell == nullptr || cell->effects.size() != 1 || leaf(cell->effects[0]) != "fx_impact_generic_01_s" || !cell->decal.empty()) {
				c.default_cells_generic = false;
			}
		}
		if (cell != nullptr && cell->effects.size() == 1 && leaf(cell->effects[0]) == "fx_impact_generic_01_s") {
			++c.generic_effect_indices;
		}
		if (cell == nullptr || cell->effects.empty()) {
			++c.no_effect_indices;
		}
	}
	for (size_t col = 0; col < c.rows; ++col) {
		const ImpactCell &cell = p_table.cell(row, col);
		if (!cell.has_effect_relation) {
			++c.columns_without_effect_relation;
		}
		for (const std::string &e : cell.effects) {
			c.distinct_effects.insert(e);
		}
	}
	return c;
}

HarnessReport fx_golden_impacts(const ImpactTable &p_table, std::string_view p_level, uint8_t p_projectile) {
	HarnessReport rep;
	const std::string level = lower(p_level);
	const ImpactCounts c = fx_impact_counts(p_table, p_projectile);
	rep.lines.push_back("impacts\t" + level + "\tprojectile\t" + std::to_string(p_projectile));
	rep.lines.push_back("rows\t" + std::to_string(c.rows));
	rep.lines.push_back("effect_relations\t" + std::to_string(c.effect_relations));
	rep.lines.push_back("decal_relations\t" + std::to_string(c.decal_relations));
	rep.lines.push_back("default_row_indices\t" + std::to_string(c.default_row_indices) + "\tall_generic\t" + std::to_string(int(c.default_cells_generic)));
	rep.lines.push_back("generic_effect_indices\t" + std::to_string(c.generic_effect_indices));
	rep.lines.push_back("no_effect_indices\t" + std::to_string(c.no_effect_indices));
	rep.lines.push_back("columns_without_effect_relation\t" + std::to_string(c.columns_without_effect_relation));
	std::string distinct;
	for (const std::string &e : c.distinct_effects) {
		distinct += (distinct.empty() ? "" : ",") + leaf(e);
	}
	rep.lines.push_back("distinct_effects\t" + std::to_string(c.distinct_effects.size()) + "\t" + distinct);
	struct Named {
		const char *name;
		uint8_t index;
		const char *effect;
		const char *decal; // "" none
		float spread; // < 0: not gated
	};
	const bool metro = level == "mp_subway";
	const bool caspian = level == "mp_007";
	std::vector<Named> named;
	if (metro) {
		named = { { "Concrete", 8, "fx_impact_concrete_01_s_xp2", "decal_concrete_s_01", 30 }, { "Brick", 2, "fx_impact_concrete_01_s", "decal_brick_s_01", 30 },
			{ "Body", 66, "fx_impact_soldier_body_s", "", 90 }, { "Chest", 158, "fx_impact_soldier_body_s", "", 90 },
			{ "Head", 65, "fx_impact_soldier_body_s", "", 0 }, { "Water", 18, "fx_impact_water_s", "", -1 },
			{ "Glass", 10, "fx_impact_glass_01_s", "decal_glass_s_01", 5 }, { "Metal", 3, "fx_impact_metal_01_s", "decal_metalarmor_s_01", 45 } };
	} else if (caspian) {
		named = { { "Concrete", 8, "fx_impact_concrete_01_s_xp2", "decal_concrete_s_01", -1 }, { "Brick", 2, "fx_impact_generic_01_s", "", -1 },
			{ "Body", 66, "fx_impact_soldier_body_s", "", -1 }, { "Chest", 158, "fx_impact_soldier_body_s", "", -1 },
			{ "Head", 65, "fx_impact_soldier_body_s", "", -1 }, { "Water", 18, "fx_impact_water_s", "", -1 },
			{ "Glass", 10, "fx_impact_glass_01_s", "decal_glass_s_01", -1 }, { "Metal", 3, "fx_impact_metal_01_s", "decal_metalarmor_s_01", -1 } };
	} else {
		named = { { "Concrete", 8, "", "", -1 }, { "Brick", 2, "", "", -1 }, { "Body", 66, "", "", -1 }, { "Chest", 158, "", "", -1 },
			{ "Head", 65, "", "", -1 }, { "Water", 18, "", "", -1 }, { "Glass", 10, "", "", -1 }, { "Metal", 3, "", "", -1 } };
	}
	for (const Named &n : named) {
		const ImpactCell *cell = p_table.select(p_projectile, n.index);
		const std::string name(p_table.material_name(n.index));
		if (cell == nullptr) {
			rep.lines.push_back("cell\t" + std::string(n.name) + "\t" + std::to_string(n.index) + "\t" + name + "\t(none)");
			if (metro || caspian) {
				rep.failures.push_back(std::string(n.name) + ": no cell");
			}
			continue;
		}
		const std::string decal = cell->decal.empty() ? std::string() : leaf(cell->decal);
		rep.lines.push_back("cell\t" + std::string(n.name) + "\t" + std::to_string(n.index) + "\t" + name + "\teffects\t" + joined_leaves(cell->effects) +
				"\tdecal\t" + (decal.empty() ? std::string("-") : decal) + "\tspread\t" + num(cell->max_spread_degrees));
		if (metro || caspian) {
			if (cell->effects.size() != 1 || leaf(cell->effects[0]) != n.effect) {
				rep.failures.push_back(std::string(n.name) + ": effects " + joined_leaves(cell->effects) + ", expected " + n.effect);
			}
			if (decal != n.decal) {
				rep.failures.push_back(std::string(n.name) + ": decal " + (decal.empty() ? std::string("-") : decal) + ", expected " +
						(n.decal[0] != 0 ? n.decal : "-"));
			}
			if (n.spread >= 0 && cell->max_spread_degrees != n.spread) {
				rep.failures.push_back(std::string(n.name) + ": spread " + num(cell->max_spread_degrees) + ", expected " + num(n.spread));
			}
		}
	}
	const auto expect = [&](const char *p_what, size_t p_got, size_t p_want) {
		if (p_got != p_want) {
			rep.failures.push_back(std::string(p_what) + " " + std::to_string(p_got) + ", expected " + std::to_string(p_want));
		}
	};
	if (metro || caspian) {
		expect("rows", c.rows, metro ? 102 : 118);
		expect("effect_relations", c.effect_relations, metro ? 223 : 299);
		expect("decal_relations", c.decal_relations, metro ? 110 : 123);
		expect("default_row_indices", c.default_row_indices, metro ? 79 : 63);
		expect("generic_effect_indices", c.generic_effect_indices, metro ? 81 : 64);
		expect("no_effect_indices", c.no_effect_indices, metro ? 66 : 87);
		expect("columns_without_effect_relation", c.columns_without_effect_relation, metro ? 65 : 86);
		expect("distinct_effects", c.distinct_effects.size(), 16);
		if (!c.default_cells_generic) {
			rep.failures.push_back("a default-row cell is not exactly {fx_impact_generic_01_s, no decal}");
		}
	}
	rep.pass = rep.failures.empty();
	return rep;
}

// ---- G6: atlas -------------------------------------------------------------------------------------------------

HarnessReport fx_golden_atlas(const EmitterAtlas &p_atlas, std::string_view p_level, const std::set<std::string> &p_used_names, size_t p_documents) {
	HarnessReport rep;
	const std::string level = lower(p_level);
	const auto tex_line = [&](const char *p_kind, const AtlasTextureInfo &p_t) {
		rep.lines.push_back(std::string("texture\t") + p_kind + "\t" + p_t.partition + "\tformat\t" + std::to_string(int(p_t.format)) + "\tsrgb\t" +
				std::to_string(int(p_t.srgb)) + "\tsize\t" + std::to_string(p_t.width) + "x" + std::to_string(p_t.height) + "\tmips\t" +
				std::to_string(p_t.mips));
	};
	tex_line("base", p_atlas.base());
	tex_line("normal", p_atlas.normal());
	size_t base = 0, normal = 0, degenerate = 0, inexact = 0, unused = 0;
	for (const AtlasEntry &e : p_atlas.entries()) {
		base += e.kind == AtlasKind::Base ? 1 : 0;
		normal += e.kind == AtlasKind::Normal ? 1 : 0;
		degenerate += e.degenerate ? 1 : 0;
		inexact += !e.degenerate && !e.pixel_exact ? 1 : 0;
		const bool used = p_used_names.contains(e.name);
		unused += used ? 0 : 1;
		rep.lines.push_back("entry\t" + e.name + "\t" + (e.kind == AtlasKind::Base ? "base" : "normal") + "\tuv\t" + num(e.min_uv.x) + "," + num(e.min_uv.y) +
				"\t" + num(e.max_uv.x) + "," + num(e.max_uv.y) + "\tpx\t" + std::to_string(e.px_min_x) + "," + std::to_string(e.px_min_y) + "\t" +
				std::to_string(e.px_max_x) + "," + std::to_string(e.px_max_y) + (e.degenerate ? "\tdegenerate" : "") +
				(!e.degenerate && !e.pixel_exact ? "\tinexact" : "") + (used ? "" : "\tunused"));
	}
	size_t missing = 0;
	for (const std::string &n : p_used_names) {
		if (p_atlas.resolve(n) == nullptr) {
			++missing;
			rep.lines.push_back("missing\t" + n);
		}
	}
	rep.lines.push_back("atlas\t" + level + "\tentries\t" + std::to_string(p_atlas.entries().size()) + "\tbase\t" + std::to_string(base) + "\tnormal\t" +
			std::to_string(normal) + "\tdegenerate\t" + std::to_string(degenerate) + "\tinexact\t" + std::to_string(inexact));
	rep.lines.push_back("names\tdocuments\t" + std::to_string(p_documents) + "\tused\t" + std::to_string(p_used_names.size()) + "\tmissing\t" +
			std::to_string(missing) + "\tunused_entries\t" + std::to_string(unused));
	const auto expect = [&](const char *p_what, size_t p_got, size_t p_want) {
		if (p_got != p_want) {
			rep.failures.push_back(std::string(p_what) + " " + std::to_string(p_got) + ", expected " + std::to_string(p_want));
		}
	};
	expect("missing names", missing, 0);
	expect("inexact rects", inexact, 0);
	if (level == "mp_subway") {
		expect("entries", p_atlas.entries().size(), 52);
		expect("base entries", base, 42);
		expect("normal entries", normal, 10);
		expect("degenerate entries", degenerate, 3);
		expect("unused entries", unused, 0);
		expect("documents", p_documents, 1053);
		const AtlasTextureInfo &b = p_atlas.base();
		const AtlasTextureInfo &n = p_atlas.normal();
		if (b.format != core::res::TextureFormat::Dxt5 || !b.srgb || b.width != 4096 || b.height != 4096 || b.mips != 13) {
			rep.failures.push_back("base atlas texture differs from DXT5 sRGB 4096 x 4096, 13 mips");
		}
		if (n.format != core::res::TextureFormat::NormalDxt5Rga || n.srgb || n.width != 1024 || n.height != 2048 || n.mips != 12) {
			rep.failures.push_back("normal atlas texture differs from NormalDXT5RGA linear 1024 x 2048, 12 mips");
		}
	}
	rep.pass = rep.failures.empty();
	return rep;
}

// ---- G7 / G8: every effect ---------------------------------------------------------------------------------------

CheckReport fx_check_effects(const std::vector<std::shared_ptr<const EffectDef>> &p_defs, const CheckOptions &p_options) {
	const auto start_clock = std::chrono::steady_clock::now();
	CheckReport rep;
	rep.effects = p_defs.size();
	rep.results.resize(p_defs.size());
	std::vector<std::vector<uint64_t>> digests(p_defs.size());
	std::vector<uint64_t> frames(p_defs.size(), 0), particle_steps(p_defs.size(), 0);
	const unsigned lanes = p_options.threads == 0 ? std::max(1u, std::thread::hardware_concurrency()) : p_options.threads;
	std::vector<std::unique_ptr<FxWorld>> worlds(lanes);
	for_lanes(p_defs.size(), lanes, [&](unsigned p_lane, size_t p_i) {
		if (!worlds[p_lane]) {
			worlds[p_lane] = std::make_unique<FxWorld>();
		}
		FxWorld &world = *worlds[p_lane];
		const std::shared_ptr<const EffectDef> &def = p_defs[p_i];
		EffectCheck &ec = rep.results[p_i];
		ec.name = def->name;
		const EffectExtent extent = effect_extent(*def);
		ec.finite = extent.finite;
		const auto fail = [&](const std::string &p_why) {
			if (ec.failures.size() < 8) {
				ec.failures.push_back(p_why);
			}
		};
		for (uint32_t s = 0; s < 4; ++s) {
			HarnessRun run;
			run.seed = p_options.seed;
			run.seconds = p_options.seconds;
			run.dt = 1.0f / 60.0f;
			run.variable = s == 3;
			uint32_t prev_live = 0;
			bool alive_checked = false;
			float max_dt = 0;
			const core::Result<EffectHandle> h = fx_run_start(world, def, s, run, [&](const FxWorld &p_w, EffectHandle p_h, uint64_t p_frame, double p_time, float p_dt) {
				max_dt = std::max(max_dt, p_dt);
				const uint32_t live = p_w.stats().live_particles;
				ec.max_live = std::max(ec.max_live, live);
				++frames[p_i];
				particle_steps[p_i] += live;
				for (const ParticleVertex &v : p_w.particles()) {
					if (!vertex_finite(v)) {
						fail("start " + std::to_string(s) + " frame " + std::to_string(p_frame) + ": non-finite particle output");
						break;
					}
				}
				for (const ParticleLight &l : p_w.lights()) {
					if (!light_finite(l)) {
						fail("start " + std::to_string(s) + " frame " + std::to_string(p_frame) + ": non-finite light output");
						break;
					}
				}
				if (live > def->max_particles) {
					fail("start " + std::to_string(s) + ": live " + std::to_string(live) + " > max_particles " + std::to_string(def->max_particles));
				}
				if (extent.finite) {
					// Spawning can lag spawn_end by a sub-step per level (a delayed component starts at the first step
					// whose start time reaches its delay; a child starts at the end of its parent particle's step), so
					// the non-increasing rule starts three frames after spawn_end (docs §2 G7 (d)).
					const double slack = 3.0 * double(max_dt);
					if (p_time - double(p_dt) >= double(extent.spawn_end) + slack && live > prev_live) {
						fail("start " + std::to_string(s) + " t " + num(p_time) + ": live rose " + std::to_string(prev_live) + " -> " + std::to_string(live) +
								" after spawn_end " + num(extent.spawn_end));
					}
					if (!alive_checked && double(extent.end) + 1.0 <= double(p_options.seconds) && p_time >= double(extent.end) + 1.0) {
						alive_checked = true;
						if (p_w.alive(p_h)) {
							fail("start " + std::to_string(s) + ": still alive at end + 1 s = " + num(double(extent.end) + 1.0));
						}
					}
				}
				prev_live = live;
				if (s == 0 && p_options.determinism) {
					digests[p_i].push_back(fx_frame_digest(p_w));
				}
			});
			if (!h) {
				fail("start " + std::to_string(s) + ": " + h.error().message);
				break;
			}
			const FxStats &st = world.stats();
			if (st.nonfinite_kills > 0) {
				fail("start " + std::to_string(s) + ": nonfinite_kills " + std::to_string(st.nonfinite_kills));
			}
			if (st.spawns_dropped > 0) {
				fail("start " + std::to_string(s) + ": spawns_dropped " + std::to_string(st.spawns_dropped));
			}
			ec.emitter_starts_dropped += st.emitter_starts_dropped;
		}
		ec.unchecked_termination = extent.finite && double(extent.end) + 1.0 > double(p_options.seconds);
	});
	worlds.clear();
	if (p_options.determinism) {
		LanePool pool(p_options.determinism_lanes);
		FxWorld world;
		for (size_t i = 0; i < p_defs.size(); ++i) {
			if (digests[i].empty()) {
				continue;
			}
			++rep.determinism_compared;
			bool same = true;
			for (const size_t chunk : { size_t(1), size_t(7) }) {
				HarnessRun run;
				run.seed = p_options.seed;
				run.seconds = p_options.seconds;
				run.dt = 1.0f / 60.0f;
				size_t k = 0;
				const core::Result<EffectHandle> h = fx_run_start(world, p_defs[i], 0, run, [&](const FxWorld &p_w, EffectHandle, uint64_t, double, float) {
					if (k >= digests[i].size() || digests[i][k] != fx_frame_digest(p_w)) {
						same = false;
					}
					++k;
				}, &pool, chunk);
				if (!h || k != digests[i].size()) {
					same = false;
				}
			}
			if (!same) {
				++rep.determinism_mismatches;
				rep.results[i].failures.push_back("determinism: 1 lane differs from " + std::to_string(p_options.determinism_lanes) + " lanes");
			}
		}
	}
	for (size_t i = 0; i < p_defs.size(); ++i) {
		const EffectCheck &ec = rep.results[i];
		rep.failed_effects += ec.failures.empty() ? 0 : 1;
		rep.finite_effects += ec.finite ? 1 : 0;
		rep.unchecked_termination += ec.unchecked_termination ? 1 : 0;
		rep.emitter_starts_dropped += ec.emitter_starts_dropped;
		rep.frames += frames[i];
		rep.particle_steps += particle_steps[i];
		if (ec.max_live > rep.max_live) {
			rep.max_live = ec.max_live;
			rep.max_live_effect = ec.name;
		}
	}
	rep.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_clock).count();
	return rep;
}

// ---- G9: the bench ---------------------------------------------------------------------------------------------

BenchReport fx_bench(const EffectDef &p_oilfire, const EffectDef &p_muzzle, const EffectDef &p_impact, uint64_t p_seed, uint32_t p_runs,
		uint32_t p_warmup_frames, uint32_t p_timed_frames) {
	BenchReport rep;
	// Private copies with the pools raised so they never steal: the bench measures particle cost (docs §9).
	const auto raised = [](const EffectDef &p_def) {
		auto d = std::make_shared<EffectDef>(p_def);
		d->max_instance_count = uint16_t(kMaxInstanceCount);
		return std::shared_ptr<const EffectDef>(std::move(d));
	};
	const std::shared_ptr<const EffectDef> oil = raised(p_oilfire);
	const std::shared_ptr<const EffectDef> muzzle = raised(p_muzzle);
	const std::shared_ptr<const EffectDef> impact = raised(p_impact);
	FxWorld world;
	const float dt = 1.0f / 60.0f;
	for (uint32_t run = 0; run < p_runs; ++run) {
		world.clear();
		uint64_t serial = 0;
		const auto start_at = [&](const std::shared_ptr<const EffectDef> &p_def, const Vec3 &p_pos) {
			EffectStart s;
			s.seed = fx_harness_seed(p_seed, serial++);
			s.transform.trans = p_pos;
			(void)world.start(p_def, s);
		};
		for (uint32_t i = 0; i < 180; ++i) {
			start_at(oil, Vec3{ float(i % 15) * 20.0f, 0.0f, float(i / 15) * 20.0f });
		}
		BenchRun br;
		double live_sum = 0;
		double emitter_sum = 0;
		std::chrono::steady_clock::duration timed{};
		for (uint32_t f = 0; f < p_warmup_frames + p_timed_frames; ++f) {
			if (f % 6 == 0) { // 8 muzzles every 0.1 s
				for (uint32_t m = 0; m < 8; ++m) {
					start_at(muzzle, Vec3{ float(m) * 3.0f, 1.5f, -10.0f });
				}
			}
			if (f % 2 == 0) { // 30 impacts per second at seeded positions
				const uint64_t k = fx_mix64(p_seed ^ (uint64_t(f) << 20));
				start_at(impact, Vec3{ fx_unit(k) * 280.0f, 0.0f, fx_unit(fx_mix64(k)) * 220.0f });
			}
			const bool timed_frame = f >= p_warmup_frames;
			const auto t0 = std::chrono::steady_clock::now();
			const uint32_t k = world.begin_frame(dt, FxView{});
			for (uint32_t s = 0; s < k; ++s) {
				const uint64_t live_before = world.stats().live_particles;
				const uint64_t spawned_before = world.stats().spawned;
				const size_t n = world.begin_step();
				world.run_items(0, n);
				world.end_step();
				if (timed_frame) {
					br.admitted += live_before + (world.stats().spawned - spawned_before);
				}
			}
			if (timed_frame) {
				timed += std::chrono::steady_clock::now() - t0;
				live_sum += world.stats().live_particles;
				emitter_sum += world.stats().live_emitters;
			}
		}
		br.timed_seconds = std::chrono::duration<double>(timed).count();
		br.ns_per_particle = br.admitted > 0 ? br.timed_seconds * 1e9 / double(br.admitted) : 0.0;
		br.mean_live = p_timed_frames > 0 ? live_sum / double(p_timed_frames) : 0.0;
		br.mean_emitters = p_timed_frames > 0 ? emitter_sum / double(p_timed_frames) : 0.0;
		br.stats = world.stats();
		rep.runs.push_back(br);
	}
	std::vector<double> ns;
	rep.valid = !rep.runs.empty();
	for (const BenchRun &r : rep.runs) {
		ns.push_back(r.ns_per_particle);
		if (r.mean_live < 12000 || r.mean_live > 20000) {
			rep.valid = false;
			rep.invalid_reason = "mean live " + num(r.mean_live) + " outside [12000, 20000]";
		}
		if (r.stats.spawns_dropped != 0 || r.stats.refused_starts != 0) {
			rep.valid = false;
			rep.invalid_reason = "spawns_dropped " + std::to_string(r.stats.spawns_dropped) + ", refused_starts " + std::to_string(r.stats.refused_starts);
		}
	}
	if (!ns.empty()) {
		std::sort(ns.begin(), ns.end());
		rep.median_ns = ns[ns.size() / 2];
		rep.min_ns = ns.front();
		rep.max_ns = ns.back();
	}
	return rep;
}

} // namespace bf3::fx
