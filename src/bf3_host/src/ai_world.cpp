#include "ai_world.h"

#include "tdm_bot_shared.h"

#include "bf3/host/server_view.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/command.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string>

namespace bf3::host {

namespace {

constexpr size_t kMaxAiSlots = 64;
constexpr float kProbeAbove = 200.0f;
constexpr float kProbeBelow = 400.0f;
constexpr uint16_t kUnperceivedYaw = 0;
const sim::Vec3 kPolygonExtents{ 2.0f, 4.0f, 2.0f };

}

float area_heat(std::span<const float> p_heat, int p_cells, const sim::Vec3 &p_origin, float p_cell_m,
		const sim::Vec3 &p_centre, float p_radius) {
	if (p_cells <= 0 || p_heat.size() < size_t(p_cells) * size_t(p_cells) || !(p_cell_m > 0.0f)) {
		return 0.0f;
	}
	const float reach = p_radius + 0.5f * p_cell_m;
	const int own_x = int(std::floor((p_centre.x - p_origin.x) / p_cell_m));
	const int own_z = int(std::floor((p_centre.z - p_origin.z) / p_cell_m));
	float sum = 0.0f;
	for (int z = 0; z < p_cells; ++z) {
		for (int x = 0; x < p_cells; ++x) {
			const float cx = p_origin.x + (float(x) + 0.5f) * p_cell_m - p_centre.x;
			const float cz = p_origin.z + (float(z) + 0.5f) * p_cell_m - p_centre.z;
			if ((x == own_x && z == own_z) || cx * cx + cz * cz <= reach * reach) {
				sum += p_heat[size_t(z) * size_t(p_cells) + size_t(x)];
			}
		}
	}
	return sum;
}

float tdm_area_temperature(TdmShared &r_shared, const BotStrategicArea &p_area, sim::Tick p_tick) {
	for (int cell = 0; cell < tdm_bot::kHeatCells * tdm_bot::kHeatCells; ++cell) {
		r_shared.heat_at(cell, p_tick);
	}
	return area_heat(r_shared.heat, tdm_bot::kHeatCells, r_shared.host.heat_origin,
			std::max(r_shared.config->heat_cell_m, 1.0f), p_area.centre, p_area.radius);
}

namespace {

class SlotObject final : public ai::AiObject {
public:
	void set_slot(uint8_t p_slot, bool p_perceived) {
		slot_ = p_slot;
		name_ = (p_perceived ? "perceived_" : "soldier_") + std::to_string(p_slot);
	}
	void place(const sim::Vec3 &p_position, uint16_t p_yaw, sim::Pose p_pose) {
		position_ = p_position;
		const float yaw = sim::yaw_radians(p_yaw);
		transform_.forward = { std::sin(yaw), 0.0f, std::cos(yaw) };
		transform_.right = { std::cos(yaw), 0.0f, -std::sin(yaw) };
		transform_.trans = p_position;
		pose_ = p_pose == sim::Pose::Crouch	 ? ai::SoldierPose::Crouching
				: p_pose == sim::Pose::Prone ? ai::SoldierPose::Lying
											 : ai::SoldierPose::Standing;
	}
	void set_side(int p_side) { side_ = p_side; }
	void set_kit(int p_kit) { kit_ = p_kit; }
	uint32_t &handle(int p_side) { return handles_[size_t(std::clamp(p_side, 1, 2) - 1)]; }

	const std::string &name() const override { return name_; }
	const std::string &ai_name() const override { return name_; }
	const std::string &object_type_name() const override { return type_name_; }
	ai::AiObjectArmament *armament() const override { return nullptr; }
	ai::AiObjectCover *cover() const override { return nullptr; }
	ai::AiObjectMobile *mobile() const override { return nullptr; }
	ai::AiObjectPhysical *physical() const override { return nullptr; }
	ai::AiObjectUnit *unit() const override { return nullptr; }
	uint32_t information(int p_side) const override {
		return p_side == 1 || p_side == 2 ? handles_[size_t(p_side - 1)] : ai::kNoHandle;
	}
	const ai::Vec3 &position() const override { return position_; }
	const ai::Transform &transformation() const override { return transform_; }
	uint32_t object_id() const override { return slot_; }
	ai::SoldierPose soldier_pose() const override { return pose_; }
	int kit_type() const override { return kit_; }
	bool is_primary() const override { return true; }
	ai::AiObject *primary_object() const override { return const_cast<SlotObject *>(this); }
	ai::AiObject *secondary_object(int &r_iterator) const override {
		r_iterator = -1;
		return nullptr;
	}
	ai::AiObject *first_secondary_object(int &r_iterator) const override {
		r_iterator = -1;
		return nullptr;
	}
	int first_secondary_object_iterator() const override { return -1; }
	int side() const override { return side_; }
	bool object_spawner_holding() const override { return false; }
	ai::AiObject *object_spawner() const override { return nullptr; }
	bool is_root_object() const override { return true; }
	int player_id() const override { return slot_; }

private:
	uint8_t slot_ = 0;
	std::string name_;
	std::string type_name_ = "Soldier";
	ai::Vec3 position_{};
	ai::Transform transform_{};
	ai::SoldierPose pose_ = ai::SoldierPose::Standing;
	int side_ = 0;
	int kit_ = 0;
	std::array<uint32_t, 2> handles_{ ai::kNoHandle, ai::kNoHandle };
};

}

struct HostAiWorld::Impl {
	AiWorldConfig config;
	std::unique_ptr<sim::CollisionWorld> collision;
	std::unique_ptr<sim::NavQuery> query;
	ai::InformationManager information;
	std::unique_ptr<ai::InformationFactory> factory;
	std::array<std::unique_ptr<ai::AIInformationGrid>, 2> grids;
	std::array<SlotObject, kMaxAiSlots> soldiers;
	std::array<std::array<SlotObject, kMaxAiSlots>, 2> perceived;
	std::array<uint8_t, kMaxAiSlots> alive{};
	std::array<uint8_t, kMaxAiSlots> teams{};
	std::vector<uint8_t> bots;
	std::array<std::unique_ptr<sim::bots::Perception>, kMaxAiSlots> perceptions;
	std::unique_ptr<ai::AIStrategicObjectManager> strategic;
	std::vector<ai::AIStrategicArea *> areas;
	std::vector<int> owners;
	std::vector<ai::AiShot> shots;
	sim::Tick tick = 0;
	uint32_t perception_ticks = 0;
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

	void forget(int p_side, SlotObject &r_object) {
		uint32_t &h = r_object.handle(p_side);
		if (h != ai::kNoHandle) {
			grids[size_t(p_side - 1)]->remove_info(h);
			information.free_information(h);
			h = ai::kNoHandle;
		}
	}

	void know(int p_side, SlotObject &r_object) {
		uint32_t &h = r_object.handle(p_side);
		if (h == ai::kNoHandle) {
			h = factory->request_information_real(ai::kNoHandle, &r_object, 0, 1.0f, tick, 0);
			if (h != ai::kNoHandle) {
				grids[size_t(p_side - 1)]->add_info(h);
			}
			return;
		}
		grids[size_t(p_side - 1)]->update_position(h);
	}
};

HostAiWorld::HostAiWorld(std::unique_ptr<Impl> p_impl) : impl_(std::move(p_impl)) {}

HostAiWorld::~HostAiWorld() = default;

core::Result<std::unique_ptr<HostAiWorld>> HostAiWorld::create(const AiWorldConfig &p_config) {
	if (p_config.level == nullptr || p_config.level->nav == nullptr || p_config.level->clusters == nullptr) {
		return core::make_error("ai world: no bot level with nav and clusters");
	}
	if (p_config.static_world == nullptr) {
		return core::make_error("ai world: no static world");
	}
	auto impl = std::make_unique<Impl>();
	impl->config = p_config;
	core::Result<std::unique_ptr<sim::CollisionWorld>> collision = sim::CollisionWorld::create(p_config.static_world);
	if (!collision) {
		return core::error_at("ai world: collision", collision.error());
	}
	impl->collision = std::move(*collision);
	core::Result<std::unique_ptr<sim::NavQuery>> query = sim::NavQuery::create(p_config.level->nav);
	if (!query) {
		return core::error_at("ai world: nav query", query.error());
	}
	impl->query = std::move(*query);
	std::unique_ptr<HostAiWorld> world(new HostAiWorld(std::move(impl)));
	Impl &m = *world->impl_;
	ai::KnowledgeContext context;
	context.information = &m.information;
	context.timer = world.get();
	context.altitude = world.get();
	m.factory = std::make_unique<ai::InformationFactory>(context);
	const sim::Vec3 lo = p_config.level->nav->min();
	const sim::Vec3 hi = p_config.level->nav->max();
	const int dim = int(std::max<uint32_t>(p_config.grid_dimension, 1));
	for (int side = 1; side <= 2; ++side) {
		m.grids[size_t(side - 1)] = std::make_unique<ai::AIInformationGrid>(
				m.information, side, dim, dim, ai::Vec2{ lo.x, lo.z }, ai::Vec2{ hi.x, hi.z });
	}
	for (uint8_t s = 0; s < kMaxAiSlots; ++s) {
		m.soldiers[s].set_slot(s, false);
		m.perceived[0][s].set_slot(s, true);
		m.perceived[1][s].set_slot(s, true);
	}
	ai::StrategicServices services;
	services.game = world.get();
	services.information = &m.information;
	services.grids = { m.grids[0].get(), m.grids[1].get() };
	m.strategic = std::make_unique<ai::AIStrategicObjectManager>(services);
	const std::vector<BotStrategicArea> &areas = p_config.level->areas;
	m.owners.resize(areas.size(), 0);
	for (size_t i = 0; i < areas.size(); ++i) {
		const BotStrategicArea &a = areas[i];
		m.owners[i] = a.team;
		if (a.kind == BotStrategicArea::Kind::TdmCluster) {
			const ai::Vec2 centre{ a.centre.x, a.centre.z };
			m.strategic->create(a.name, ai::Vec2{ centre.x - a.radius, centre.y - a.radius }, centre, a.centre.y,
					a.radius, std::string());
		} else {
			m.strategic->create_from_control_point(a.name, uint32_t(i), a.radius, std::string());
		}
		m.areas.push_back(m.strategic->strategic_area(a.name));
	}
	for (size_t i = 0; i < m.areas.size(); ++i) {
		std::vector<std::pair<float, size_t>> near;
		for (size_t j = 0; j < m.areas.size(); ++j) {
			if (j != i) {
				near.push_back({ ai::distance_sq(areas[i].centre, areas[j].centre), j });
			}
		}
		std::sort(near.begin(), near.end());
		for (size_t k = 0; k < std::min<size_t>(2, near.size()); ++k) {
			const size_t j = near[k].second;
			const auto &list = m.areas[i]->neighbours();
			if (std::find(list.begin(), list.end(), m.areas[j]) == list.end()) {
				m.strategic->add_two_way_neighbour(areas[i].name, areas[j].name);
			}
		}
	}
	m.strategic->post_load_init_areas();
	return world;
}

ai::AiWorld HostAiWorld::world() {
	ai::AiWorld w;
	w.timer = this;
	w.physics = this;
	w.terrain = this;
	w.objects = this;
	w.collisions = this;
	w.graphs = this;
	w.strategic = this;
	return w;
}

void HostAiWorld::add_bot(uint8_t p_slot) {
	Impl &m = *impl_;
	if (p_slot >= kMaxAiSlots || m.perceptions[p_slot] != nullptr) {
		return;
	}
	m.perceptions[p_slot] = std::make_unique<sim::bots::Perception>(m.config.perception);
	m.bots.push_back(p_slot);
	std::sort(m.bots.begin(), m.bots.end());
}

void HostAiWorld::step(const ServerView &p_view, std::span<const sim::bots::HeardShot> p_shots,
		std::span<const uint8_t> p_firing, sim::Tick p_tick) {
	Impl &m = *impl_;
	m.tick = p_tick;
	const std::span<const sim::SoldierState> states = p_view.states();
	const size_t n = std::min(states.size(), kMaxAiSlots);
	for (size_t s = 0; s < n; ++s) {
		const uint8_t slot = uint8_t(s);
		m.alive[s] = p_view.alive(slot) && std::isfinite(states[s].position.x) ? 1 : 0;
		const uint8_t team = p_view.team(slot);
		if (team != m.teams[s]) {
			for (int side = 1; side <= 2; ++side) {
				m.forget(side, m.soldiers[s]);
			}
			m.teams[s] = team;
		}
		SlotObject &own = m.soldiers[s];
		own.set_side(team);
		own.set_kit(p_view.kit(slot));
		own.place(states[s].position, states[s].yaw, states[s].pose);
		for (int side = 1; side <= 2; ++side) {
			if (m.alive[s] != 0 && team == side) {
				m.know(side, own);
			} else {
				m.forget(side, own);
			}
		}
	}
	m.shots.clear();
	for (const sim::bots::HeardShot &shot : p_shots) {
		m.shots.push_back({ int(shot.shooter), shot.origin, shot.tick });
	}
	for (const uint8_t bot : m.bots) {
		if (bot >= n || m.alive[bot] == 0 ||
				!sim::bots::perception_tick(bot, p_tick, m.config.perception.update_ticks)) {
			continue;
		}
		const sim::SoldierState &self = states[bot];
		sim::bots::PerceptionInput in;
		in.self = bot;
		in.team = m.teams[bot];
		in.tick = p_tick;
		const size_t pose = std::min<size_t>(size_t(self.pose), 2);
		in.eye = { self.position.x, self.position.y + m.config.eye_heights[pose], self.position.z };
		in.yaw = self.yaw;
		in.pitch = self.pitch;
		in.states = states.first(n);
		in.teams = std::span<const uint8_t>(m.teams.data(), n);
		in.alive = std::span<const uint8_t>(m.alive.data(), n);
		in.firing = p_firing.size() >= n ? p_firing.first(n) : std::span<const uint8_t>();
		in.shots = p_shots;
		in.world = &p_view.world();
		in.seed = m.config.seed ^ (uint64_t(bot) << 32) ^ p_tick;
		for (size_t i = 0; i < 3; ++i) {
			in.eye_heights[i] = m.config.eye_heights[i];
		}
		m.perceptions[bot]->update(in);
		++m.perception_ticks;
	}
	for (int side = 1; side <= 2; ++side) {
		std::array<const sim::bots::PerceivedEnemy *, kMaxAiSlots> freshest{};
		for (const uint8_t bot : m.bots) {
			if (m.teams[bot] != side || m.perceptions[bot] == nullptr) {
				continue;
			}
			for (const sim::bots::PerceivedEnemy &e : m.perceptions[bot]->enemies()) {
				if (!e.detected || e.slot >= kMaxAiSlots) {
					continue;
				}
				if (freshest[e.slot] == nullptr || e.last_seen > freshest[e.slot]->last_seen) {
					freshest[e.slot] = &e;
				}
			}
		}
		for (size_t s = 0; s < kMaxAiSlots; ++s) {
			SlotObject &proxy = m.perceived[size_t(side - 1)][s];
			if (freshest[s] == nullptr || s >= n || m.teams[s] == side) {
				m.forget(side, proxy);
				continue;
			}
			proxy.set_side(m.teams[s]);
			proxy.place(freshest[s]->last_position, kUnperceivedYaw, sim::Pose::Stand);
			m.know(side, proxy);
		}
	}
}

void HostAiWorld::set_area_temperature(size_t p_area, float p_temperature) {
	if (p_area < impl_->areas.size() && impl_->areas[p_area] != nullptr) {
		impl_->areas[p_area]->set_temperature(1, p_temperature);
		impl_->areas[p_area]->set_temperature(2, p_temperature);
	}
}

const ai::InformationManager &HostAiWorld::information() const {
	return impl_->information;
}

const ai::AIInformationGrid &HostAiWorld::grid(int p_side) const {
	return *impl_->grids[size_t(std::clamp(p_side, 1, 2) - 1)];
}

uint32_t HostAiWorld::information_of(int p_side, uint8_t p_slot) const {
	if (p_slot >= kMaxAiSlots || (p_side != 1 && p_side != 2)) {
		return ai::kNoHandle;
	}
	const uint32_t own = impl_->soldiers[p_slot].information(p_side);
	return own != ai::kNoHandle ? own : impl_->perceived[size_t(p_side - 1)][p_slot].information(p_side);
}

const sim::bots::Perception *HostAiWorld::perception(uint8_t p_slot) const {
	return p_slot < kMaxAiSlots ? impl_->perceptions[p_slot].get() : nullptr;
}

std::span<const uint8_t> HostAiWorld::bots() const {
	return impl_->bots;
}

ai::AIStrategicObjectManager &HostAiWorld::strategic() {
	return *impl_->strategic;
}

const ai::AIStrategicObjectManager &HostAiWorld::strategic() const {
	return *impl_->strategic;
}

ai::AIStrategicArea *HostAiWorld::area(size_t p_index) const {
	return p_index < impl_->areas.size() ? impl_->areas[p_index] : nullptr;
}

uint32_t HostAiWorld::perception_ticks() const {
	return impl_->perception_ticks;
}

sim::Tick HostAiWorld::game_tick() const {
	return impl_->tick;
}

double HostAiWorld::tick_seconds() const {
	return double(sim::kTickSeconds);
}

double HostAiWorld::update_time() const {
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - impl_->start).count();
}

float HostAiWorld::gravity() const {
	return impl_->config.gravity;
}

float HostAiWorld::altitude(const ai::Vec3 &p_position) const {
	sim::StaticHit hit;
	if (impl_->collision->ray(p_position, { 0.0f, -kProbeBelow, 0.0f }, sim::query_mask::kMovement, &hit)) {
		return p_position.y - hit.position.y;
	}
	return p_position.y - terrain_altitude(p_position.x, p_position.z);
}

float HostAiWorld::height(float p_x, float p_z) const {
	return terrain_altitude(p_x, p_z);
}

float HostAiWorld::terrain_altitude(float p_x, float p_z) const {
	const float top = impl_->config.level->nav->max().y + kProbeAbove;
	sim::StaticHit hit;
	if (impl_->collision->ray({ p_x, top, p_z },
				{ 0.0f, -(top - impl_->config.level->nav->min().y + kProbeBelow), 0.0f }, sim::query_mask::kMovement,
				&hit)) {
		return hit.position.y;
	}
	return impl_->config.level->nav->min().y;
}

ai::Vec2 HostAiWorld::world_min() const {
	const sim::Vec3 lo = impl_->config.level->nav->min();
	return { lo.x, lo.z };
}

ai::Vec2 HostAiWorld::world_max() const {
	const sim::Vec3 hi = impl_->config.level->nav->max();
	return { hi.x, hi.z };
}

bool HostAiWorld::inside_combat_area(const ai::Vec3 &p_position) const {
	uint32_t poly = 0;
	uint32_t island = 0;
	ai::Vec3 on{};
	return polygon(p_position, 0, poly, island, on) && island == impl_->config.level->main_component;
}

void HostAiWorld::world_player_ids(std::vector<int> &r_out) const {
	r_out.clear();
	for (size_t s = 0; s < kMaxAiSlots; ++s) {
		if (impl_->alive[s] != 0) {
			r_out.push_back(int(s));
		}
	}
}

ai::AiObject *HostAiWorld::player_object(int p_player_id) const {
	if (p_player_id < 0 || size_t(p_player_id) >= kMaxAiSlots || impl_->alive[size_t(p_player_id)] == 0) {
		return nullptr;
	}
	return &impl_->soldiers[size_t(p_player_id)];
}

bool HostAiWorld::collide_line_with_world(const ai::Vec3 &p_from, const ai::Vec3 &p_to, ai::Vec3 *r_hit) const {
	sim::StaticHit hit;
	const sim::Vec3 delta{ p_to.x - p_from.x, p_to.y - p_from.y, p_to.z - p_from.z };
	if (!impl_->collision->ray(p_from, delta, sim::query_mask::kProjectile, &hit)) {
		return false;
	}
	if (r_hit != nullptr) {
		*r_hit = hit.position;
	}
	return true;
}

std::span<const ai::AiShot> HostAiWorld::recent_shots() const {
	return impl_->shots;
}

const ai::NavClusterMap *HostAiWorld::create_graph() const {
	return impl_->config.level->clusters.get();
}

bool HostAiWorld::control_point_info(uint32_t p_id, ai::Vec3 &r_position, float &r_radius, bool &r_active) const {
	const std::vector<BotStrategicArea> &areas = impl_->config.level->areas;
	if (p_id >= areas.size() || areas[p_id].kind == BotStrategicArea::Kind::TdmCluster) {
		return false;
	}
	r_position = areas[p_id].centre;
	r_radius = areas[p_id].radius;
	r_active = true;
	return true;
}

int HostAiWorld::owner_of_control_point(uint32_t p_id) const {
	return p_id < impl_->owners.size() ? impl_->owners[p_id] : 0;
}

bool HostAiWorld::control_point_takeable(uint32_t p_id) const {
	const std::vector<BotStrategicArea> &areas = impl_->config.level->areas;
	return p_id < areas.size() && areas[p_id].takeable;
}

float HostAiWorld::control_point_take_over_counter(uint32_t p_id) const {
	(void)p_id;
	return 1.0f;
}

int HostAiWorld::number_of_maps() const {
	return 1;
}

bool HostAiWorld::polygon(
		const ai::Vec3 &p_position, int p_map, uint32_t &r_polygon, uint32_t &r_island, ai::Vec3 &r_on) const {
	if (p_map != 0) {
		return false;
	}
	const BotLevel &level = *impl_->config.level;
	sim::NavPolyRef ref = 0;
	sim::Vec3 on{};
	if (!impl_->query->nearest(p_position, kPolygonExtents, &ref, &on) || ref == 0) {
		return false;
	}
	r_polygon = sim::nav_poly_index(*level.nav, ref);
	r_island = sim::nav_poly_component(*level.nav, level.components, ref);
	r_on = on;
	return true;
}

int HostAiWorld::number_of_control_points(int p_side) const {
	int count = 0;
	for (size_t i = 0; i < impl_->owners.size(); ++i) {
		count += impl_->config.level->areas[i].kind != BotStrategicArea::Kind::TdmCluster && impl_->owners[i] == p_side
				? 1
				: 0;
	}
	return count;
}

namespace {

bool squad_slot(const TdmRules &p_rules, int p_squad, const ai::SquadPlayer &p_player, uint8_t &r_slot) {
	const int id = p_player.id();
	if (id < 0 || id >= int(net::kMaxPlayerRows) || p_squad < 1 || p_squad > int(net::kMaxSquads)) {
		return false;
	}
	r_slot = uint8_t(id);
	return p_player.side() == int(p_rules.team_of(r_slot));
}

bool lands_in_created(const SquadTable &p_squads, uint8_t p_slot, uint8_t p_team, uint8_t p_squad) {
	if (!p_squads.members(p_team, p_squad).empty()) {
		return false;
	}
	for (uint8_t q = 1; q < p_squad; ++q) {
		const std::span<const uint8_t> members = p_squads.members(p_team, q);
		if (members.empty() || (members.size() == 1 && members.front() == p_slot)) {
			return false;
		}
	}
	return true;
}

}

bool RulesSquadService::join_squad(int p_squad, const ai::SquadPlayer &p_player) {
	uint8_t slot = 0;
	if (!squad_slot(rules_, p_squad, p_player, slot)) {
		return false;
	}
	const SquadTable &squads = rules_.squads();
	const uint8_t squad = uint8_t(p_squad);
	if (squads.squad_of(slot) == squad) {
		return true;
	}
	net::SquadRequest request;
	request.squad = squad;
	if (squads.members(squads.team_of(slot), squad).empty()) {
		if (!lands_in_created(squads, slot, squads.team_of(slot), squad)) {
			return false;
		}
		request.op = net::SquadOp::Create;
	}
	return rules_.request_squad(slot, request) == SquadRefusal::None && squads.squad_of(slot) == squad;
}

bool RulesSquadService::exit_squad(int p_squad, const ai::SquadPlayer &p_player) {
	uint8_t slot = 0;
	if (!squad_slot(rules_, p_squad, p_player, slot) || rules_.squads().squad_of(slot) != uint8_t(p_squad)) {
		return false;
	}
	net::SquadRequest request;
	request.op = net::SquadOp::Leave;
	return rules_.request_squad(slot, request) == SquadRefusal::None;
}

bool RulesSquadService::is_squad_leader(int p_squad, const ai::SquadPlayer &p_player) const {
	uint8_t slot = 0;
	return squad_slot(rules_, p_squad, p_player, slot) && rules_.squads().squad_of(slot) == uint8_t(p_squad) &&
			rules_.squads().leader(slot);
}

void RulesSquadService::request_new_commander_order(int p_squad, const ai::SquadPlayer &p_player) {
	(void)p_squad;
	(void)p_player;
}

}
