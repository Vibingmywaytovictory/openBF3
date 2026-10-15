// Headless walker bots on the GameServer (bot_fill.h; docs/design/s1.md §5.5, §8): load_bot_level on the loader
// thread, and the BotDirector (bot_director.h) that runs the brains on the server thread.
#include "bf3/host/bot_fill.h"

#include "bot_director.h"

#include "bf3/core/level/level.h"
#include "bf3/core/level/mode_areas.h"
#include "bf3/host/level_load.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/spawn.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace bf3::host {

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point p_start) {
	return std::chrono::duration<double>(Clock::now() - p_start).count();
}

double ms_since(Clock::time_point p_start) {
	return std::chrono::duration<double, std::milli>(Clock::now() - p_start).count();
}

// docs §5.5: the on-mesh radius and the search box of each kind.
float kind_radius(BotPointRow::Kind p_kind) {
	switch (p_kind) {
		case BotPointRow::Kind::Spawn:
			return 1.0f;
		case BotPointRow::Kind::Mcom:
			return 2.0f;
		default:
			return 5.0f;
	}
}

sim::bots::BotGoal::Kind goal_kind(BotPointRow::Kind p_kind) {
	switch (p_kind) {
		case BotPointRow::Kind::Flag:
			return sim::bots::BotGoal::Kind::Flag;
		case BotPointRow::Kind::Hq:
			return sim::bots::BotGoal::Kind::Hq;
		case BotPointRow::Kind::Mcom:
			return sim::bots::BotGoal::Kind::Mcom;
		default:
			return sim::bots::BotGoal::Kind::Spawn;
	}
}

class BotSource final : public CommandSource {
public:
	BotSource(BotDirector *p_director, uint32_t p_index) :
			director_(p_director), index_(p_index) {}
	sim::Command command(sim::Tick p_tick, const sim::SoldierState &p_state) override {
		return director_->command(index_, p_tick, p_state);
	}

private:
	BotDirector *director_ = nullptr;
	uint32_t index_ = 0;
};

} // namespace

core::Result<std::shared_ptr<const BotLevel>> load_bot_level(const LevelDatabase &p_db, const SharedLevel &p_shared, const BotLoadOptions &p_options,
		LoadProgress *p_progress) {
	(void)p_progress;
	if (p_db.level == nullptr || p_db.world == nullptr || p_shared.static_world == nullptr) {
		return core::make_error("load_bot_level: no level database or static world");
	}
	auto out = std::make_shared<BotLevel>();
	out->level = p_shared.level;
	out->mode = p_shared.mode;
	const sim::NavParams params = p_options.params_from_motor ? sim::nav_params_from_motor(p_shared.motor) : p_options.params;

	// The region: the combat-area tiles of every MP mode (+ margin); the static world must have that cover.
	Clock::time_point clock = Clock::now();
	core::Result<core::level::LevelCombatTiles> combat = core::level::level_combat_tiles(p_db.install, p_shared.level, params.region_margin);
	if (!combat) {
		return core::error_at("load_bot_level: combat areas", combat.error());
	}
	out->seconds_areas = seconds_since(clock);
	if (combat->tiles.empty()) {
		return core::make_error("load_bot_level: " + p_shared.level + " has no combat-area tile");
	}
	if (combat->tiles != p_shared.terrain_cover) {
		return core::make_error("load_bot_level: the static world's terrain cover (" + std::to_string(p_shared.terrain_cover.size()) +
				" tiles) is not the level's combat-area tiles (" + std::to_string(combat->tiles.size()) +
				"): load_shared_level must get SharedLoadOptions::terrain_cover = level_combat_tiles");
	}
	out->region.assign(combat->tiles.begin(), combat->tiles.end());

	clock = Clock::now();
	sim::NavLoadOptions nav_options;
	nav_options.bake.params = params;
	nav_options.bake.threads = p_options.threads;
	nav_options.bake.link_shapes = p_shared.static_world;
	nav_options.world.build.threads = p_options.threads;
	nav_options.world.soup_threads = p_options.threads;
	nav_options.world.terrain_cover = p_shared.terrain_cover;
	nav_options.region = out->region;
	core::Result<std::shared_ptr<const sim::NavWorld>> nav =
			sim::load_nav_world_cached(p_options.cache, *p_db.level, *p_db.world, nullptr, *p_shared.static_world, nav_options);
	if (!nav) {
		return core::error_at("load_bot_level: nav", nav.error());
	}
	out->nav = std::move(*nav);
	out->seconds_nav = seconds_since(clock);

	clock = Clock::now();
	out->components = sim::nav_components(*out->nav);
	out->main_component = out->components.largest;
	out->seconds_components = seconds_since(clock);

	// The rows: the mode's flags, HQs and MCOMs, and every spawn point at its settled feet.
	core::Result<core::level::ModeAreas> areas = core::level::read_mode_areas(*p_db.world);
	if (!areas) {
		return core::error_at("load_bot_level: mode areas", areas.error());
	}
	for (size_t i = 0; i < areas->objectives.size(); ++i) {
		const core::level::ModeObjectiveDef &o = areas->objectives[i];
		BotPointRow row;
		row.kind = o.kind == core::level::ModeObjectiveDef::Kind::Hq ? BotPointRow::Kind::Hq
				: o.kind == core::level::ModeObjectiveDef::Kind::Mcom	 ? BotPointRow::Kind::Mcom
																		 : BotPointRow::Kind::Flag;
		row.index = uint32_t(i);
		row.point = { float(o.position[0]), float(o.position[1]), float(o.position[2]) };
		out->points.push_back(row);
	}
	{
		core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(p_shared.static_world);
		if (!world) {
			return core::error_at("load_bot_level: settle world", world.error());
		}
		sim::SoldierMotor motor;
		BF3_TRY(motor.init(**world, p_shared.motor));
		for (size_t i = 0; i < p_shared.spawns.size(); ++i) {
			BotPointRow row;
			row.kind = BotPointRow::Kind::Spawn;
			row.index = uint32_t(i);
			const sim::SettleResult r = sim::settle_spawn(motor, **world, p_shared.spawns[i], 1, 0);
			row.point = r.ok ? r.state.position : p_shared.spawns[i].position;
			out->points.push_back(row);
		}
	}
	for (BotPointRow &row : out->points) {
		const float radius = kind_radius(row.kind);
		const float vertical = row.kind == BotPointRow::Kind::Spawn ? 1.0f : 3.0f;
		row.check = sim::nav_check_point(*out->nav, out->components, row.point, { radius + 1.0f, vertical + 1.0f, radius + 1.0f });
		row.on_mesh = row.check.poly != 0 && row.check.horizontal <= radius && std::fabs(row.check.vertical) <= vertical;
		row.in_main = row.on_mesh && row.check.component == out->main_component;
		if (row.in_main) {
			sim::bots::BotGoal g;
			g.kind = goal_kind(row.kind);
			g.position = row.check.nearest;
			g.poly = row.check.poly;
			g.index = row.index;
			out->objectives.push_back(g);
		}
	}
	return std::shared_ptr<const BotLevel>(std::move(out));
}

// ---- BotDirector --------------------------------------------------------------------------------------------------

core::Result<std::unique_ptr<BotDirector>> BotDirector::create(std::shared_ptr<const BotFillConfig> p_config, uint64_t p_seed) {
	if (p_config == nullptr || p_config->level == nullptr || p_config->level->nav == nullptr) {
		return core::make_error("bots: no bot level");
	}
	std::unique_ptr<BotDirector> d(new BotDirector());
	d->config_ = std::move(p_config);
	core::Result<std::unique_ptr<sim::NavQuery>> query = sim::NavQuery::create(d->config_->level->nav, d->config_->query);
	if (!query) {
		return core::error_at("bots", query.error());
	}
	d->query_ = std::move(*query);
	d->paths_ = std::make_unique<sim::bots::PathService>(*d->query_, d->config_->paths);
	d->brains_.reserve(d->config_->count);
	for (uint32_t i = 0; i < d->config_->count; ++i) {
		d->brains_.emplace_back(i, p_seed, d->config_->walker);
	}
	return d;
}

BotDirector::~BotDirector() = default;

uint32_t BotDirector::count() const {
	return uint32_t(brains_.size());
}

std::unique_ptr<CommandSource> BotDirector::source(uint32_t p_index) {
	return std::make_unique<BotSource>(this, p_index);
}

sim::Command BotDirector::command(uint32_t p_index, sim::Tick p_tick, const sim::SoldierState &p_state) {
	const Clock::time_point start = Clock::now();
	if (p_tick != run_tick_) {
		run_tick_ = p_tick;
		brains_ms_ = 0.0;
		thinks_tick_ = 0;
		paths_->run();
		paths_ms_ = ms_since(start);
	}
	if (p_index >= brains_.size()) {
		sim::Command c;
		c.tick = p_tick;
		return c;
	}
	sim::bots::WalkerBrain &brain = brains_[p_index];
	const BotLevel &level = *config_->level;
	sim::bots::WalkerView view;
	view.tick = p_tick;
	view.slot = p_index;
	view.self = &p_state;
	view.objectives = level.objectives;
	view.components = &level.components;
	view.main_component = level.main_component;
	view.query = query_.get();
	view.paths = paths_.get();
	const uint64_t thinks = brain.stats().thinks;
	const sim::Command c = brain.command(view);
	thinks_tick_ += uint32_t(brain.stats().thinks - thinks);
	brains_ms_ += ms_since(start);
	return c;
}

BotFillStats BotDirector::stats() const {
	BotFillStats s;
	s.bots = uint32_t(brains_.size());
	s.thinks_last_tick = thinks_tick_;
	s.path_iterations_last_tick = paths_->stats().iterations_last;
	s.paths_pending = uint32_t(paths_->pending());
	s.last_brains_ms = brains_ms_;
	s.last_paths_ms = paths_ms_;
	for (const sim::bots::WalkerBrain &b : brains_) {
		s.walkers.add(b.stats());
	}
	s.paths = paths_->stats();
	s.query = query_->stats();
	return s;
}

const sim::bots::WalkerBrain &BotDirector::brain(uint32_t p_index) const {
	return brains_[p_index];
}

} // namespace bf3::host
