#define OPENBF3_GUNPLAY_CLIENT_TESTS 1
#include "gunplay_fixture.h"
#include "weapons_support.h"

#include "bf3/core/gameplay/capsule_pose_table.h"
#include "bf3/core/util/text.h"
#include "bf3/host/client_projectiles.h"
#include "bf3/host/client_session.h"
#include "bf3/host/game_server.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/kits.h"
#include "bf3/host/lag_comp.h"
#include "bf3/host/presentation_events.h"
#include "bf3/host/presentation_weapon.h"
#include "bf3/host/server_host.h"
#include "bf3/net/enet_host.h"
#include "bf3/net/other_sample.h"
#include "bf3/net/protocol.h"
#include "bf3/net/weapons_wire.h"
#include "bf3/sim/prediction.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/spawn.h"
#include "bf3/sim/weapons/ballistics.h"
#include "bf3/sim/weapons/posed_target.h"
#include "bf3/sim/weapons/surface_materials.h"
#include "bf3/sim/weapons/weapon_rng.h"
#include "bf3/sim/weapons/weapon_rules.h"
#include "test_support.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace bf3;
namespace gp = bf3::core::gameplay;
namespace sw = bf3::sim::weapons;
namespace tw = bf3::test::weapons;
namespace tg = bf3::test::gunplay;

namespace {

gp::CapsuleLocal cap(float p_ax, float p_ay, float p_az, float p_bx, float p_by, float p_bz, float p_r, sw::HitZone p_zone, uint8_t p_index) {
	gp::CapsuleLocal c{};
	c.a[0] = p_ax;
	c.a[1] = p_ay;
	c.a[2] = p_az;
	c.b[0] = p_bx;
	c.b[1] = p_by;
	c.b[2] = p_bz;
	c.radius = p_r;
	c.zone = uint8_t(p_zone);
	c.capsule = p_index;
	c.property = uint8_t(10 + p_index);
	c.flags = p_zone == sw::HitZone::Legs ? sw::zone_capsule_flags::kDeactivateIfBehindWall : uint8_t(0);
	return c;
}

void stance_entry(size_t p_stance, size_t p_pitch, double p_extra, gp::CapsuleLocal *r_out) {
	const float s = float(p_stance);
	const float lean = 0.004f * float(p_pitch);
	r_out[0] = cap(0.0f, 1.65f - 0.6f * s, 0.05f + lean, 0.0f, 1.65f - 0.6f * s, 0.05f + lean, 0.15f, sw::HitZone::Head, 0);
	r_out[1] = cap(0.0f, 1.3f - 0.5f * s, 0.0f, 0.0f, 1.3f - 0.5f * s, 0.0f, 0.25f, sw::HitZone::Chest, 1);
	r_out[2] = cap(0.1f, 0.1f, 0.0f, 0.1f, 0.8f - 0.3f * s, 0.01f * float(p_pitch), 0.1f, sw::HitZone::Legs, 2);
	for (int c = 0; c < 3; ++c) {
		r_out[c].a[1] += float(p_extra);
		r_out[c].b[1] += float(p_extra);
	}
}

gp::CapsulePoseTable capsule_table() {
	gp::CapsulePoseTable t;
	t.version = gp::kCapsuleTableVersion;
	t.capsule_count = 3;
	for (size_t i = 0; i < gp::kCapsulePitchBuckets; ++i) {
		t.pitch_deg[i] = i <= 14 ? float(-60.7 + 60.7 * double(i) / 14.0) : float(78.1 * double(i - 14) / 18.0);
	}
	t.pitch_deg[14] = 0.0f;
	t.steady.resize(gp::kCapsuleStances * gp::kCapsulePitchBuckets * 3);
	t.transition.resize(gp::kCapsuleTransitions * gp::kCapsuleTransitionFractions * gp::kCapsulePitchBuckets * 3);
	for (size_t s = 0; s < gp::kCapsuleStances; ++s) {
		for (size_t p = 0; p < gp::kCapsulePitchBuckets; ++p) {
			stance_entry(s, p, 0, &t.steady[t.steady_index(s, p)]);
		}
	}
	const size_t from[6] = { 0, 0, 1, 1, 2, 2 }, to[6] = { 1, 2, 0, 2, 0, 1 };
	for (size_t r = 0; r < gp::kCapsuleTransitions; ++r) {
		for (size_t f = 0; f < gp::kCapsuleTransitionFractions; ++f) {
			const double w = double(f) / double(gp::kCapsuleTransitionFractions - 1);
			for (size_t p = 0; p < gp::kCapsulePitchBuckets; ++p) {
				gp::CapsuleLocal a[3], b[3];
				stance_entry(from[r], p, 0.05 * std::sin(3.14159265358979 * w), a);
				stance_entry(to[r], p, 0, b);
				gp::CapsuleLocal *o = &t.transition[t.transition_index(r, f, p)];
				for (int c = 0; c < 3; ++c) {
					o[c] = a[c];
					for (int k = 0; k < 3; ++k) {
						o[c].a[k] = float(a[c].a[k] + (b[c].a[k] - a[c].a[k]) * w);
						o[c].b[k] = float(a[c].b[k] + (b[c].b[k] - a[c].b[k]) * w);
					}
				}
			}
		}
	}
	double bound = 0;
	for (const auto *v : { &t.steady, &t.transition }) {
		for (const gp::CapsuleLocal &e : *v) {
			for (const float *p : { e.a, e.b }) {
				bound = std::max(bound, std::sqrt(double(p[0]) * p[0] + double(p[1]) * p[1] + double(p[2]) * p[2]) + double(e.radius));
			}
		}
	}
	t.bound_radius = float(bound) + 1e-3f;
	t.hash = gp::capsule_pose_table_hash(t);
	return t;
}

constexpr const char *kRifle = "weapons/m16a4/u_m16a4";
constexpr const char *kPistol = "weapons/m9/u_m9";

std::shared_ptr<const host::SharedLevel> gunplay_floor() {
	static std::shared_ptr<const host::SharedLevel> s_level;
	if (s_level != nullptr) {
		return s_level;
	}
	std::shared_ptr<const host::SharedLevel> floor = tg::synthetic_floor();
	REQUIRE(floor != nullptr);
	auto g = std::make_shared<host::GunplayLevel>();
	for (const tw::Synthetic &w : tw::synthetic_gate_weapons()) {
		g->defs.push_back(w.def);
	}
	g->materials = std::make_shared<const sw::SurfaceMaterialTable>();
	g->soldier = tg::synthetic_soldier();
	g->capsule_table = capsule_table();
	g->max_health = 100.0f;
	g->level_hash = host::gunplay_level_hash("synthetic_floor", "gunplay");
	g->notes.push_back("cl test: the synthetic gate weapons, lc's synthetic capsule pose table, M16A3 + M9 kits");
	uint16_t rifle = sw::kNoWeapon, pistol = sw::kNoWeapon;
	for (size_t i = 0; i < g->defs.size(); ++i) {
		rifle = g->defs[i].variant == kRifle ? uint16_t(i) : rifle;
		pistol = g->defs[i].variant == kPistol ? uint16_t(i) : pistol;
	}
	REQUIRE(rifle != sw::kNoWeapon);
	REQUIRE(pistol != sw::kNoWeapon);
	for (size_t team = 0; team < host::kKitTeams; ++team) {
		for (size_t kit = 0; kit < host::kKitCount; ++kit) {
			host::KitLoadout &l = g->kits.loadouts[team][kit];
			l.kit = uint8_t(kit);
			l.team = uint8_t(team + 1);
			l.defs.fill(sw::kNoWeapon);
			l.defs[0] = rifle;
			l.defs[1] = pistol;
			l.active = 0;
		}
	}
	g->kits.hash = host::kit_table_hash(g->kits);
	auto level = std::make_shared<host::SharedLevel>(*floor);
	level->gunplay = std::move(g);
	s_level = std::move(level);
	return s_level;
}

struct Tables {
	std::shared_ptr<const sw::WeaponTable> table;
	std::shared_ptr<const gp::CapsulePoseTable> capsules;
	host::KitTable kits;
	net::ShotQuantization frame;
	uint16_t rifle = 0;
	uint16_t pistol = 0;
};

const Tables &tables() {
	static Tables s;
	if (s.table != nullptr) {
		return s;
	}
	core::Result<std::shared_ptr<const host::ClientGunplayInputs>> in = host::client_gunplay_inputs(*gunplay_floor());
	REQUIRE_MESSAGE(in.ok(), (in.ok() ? std::string() : in.error().message));
	s.table = (*in)->table;
	s.capsules = (*in)->capsules;
	s.kits = (*in)->kits;
	s.frame = (*in)->shot_frame;
	s.rifle = tw::id_of(*s.table, kRifle);
	s.pistol = tw::id_of(*s.table, kPistol);
	return s;
}

std::string mechanics_cfg_path() {
	return OPENBF3_MECHANICS_CFG;
}

std::filesystem::path scratch_dir() {
	static std::filesystem::path s_dir;
	if (s_dir.empty()) {
		s_dir = std::filesystem::temp_directory_path() / ("openbf3-cl-" + std::to_string(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())));
		std::error_code ec;
		std::filesystem::create_directories(s_dir, ec);
	}
	return s_dir;
}

std::string cfg_with(const char *p_file, const std::string &p_key, const std::string &p_value) {
	const std::string base = tg::read_text(mechanics_cfg_path());
	REQUIRE_MESSAGE(!base.empty(), mechanics_cfg_path());
	std::string out;
	bool replaced = false;
	size_t at = 0;
	while (at < base.size()) {
		size_t end = base.find('\n', at);
		if (end == std::string::npos) {
			end = base.size();
		}
		std::string line = base.substr(at, end - at);
		if (line.rfind(p_key + " ", 0) == 0 || line.rfind(p_key + "=", 0) == 0) {
			line = p_key + " = " + p_value;
			replaced = true;
		}
		out += line + "\n";
		at = end + 1;
	}
	REQUIRE_MESSAGE(replaced, p_key);
	const std::filesystem::path path = scratch_dir() / p_file;
	std::ofstream f(path, std::ios::binary);
	f << out;
	f.close();
	return path.string();
}

std::string no_damage_cfg() {
	static const std::string s_path = cfg_with("no_damage.cfg", "bullet_damage_modifier", "0");
	return s_path;
}

struct ServerShot {
	sim::Tick tick = 0;
	sw::ShotEvent shot;
};
struct ServerTimed {
	sim::Tick tick = 0;
	uint8_t slot = 0;
	sw::TimedWeaponEvent event;
};
struct ServerMagazine {
	sim::Tick tick = 0;
	uint8_t slot = 0;
	int32_t magazine = 0;
};
struct ServerImpact {
	sim::Tick tick = 0;
	sw::ImpactEvent impact;
};

class ServerLog final : public host::GunplayObserver {
public:
	static constexpr size_t kMaxRecords = size_t(1) << 20;
	explicit ServerLog(bool p_rewind_rows) :
			rewind_rows_on_(p_rewind_rows) {}

	void weapon_tick(uint8_t p_slot, const sim::Command &p_command, const sw::SoldierWeaponsState &p_after, const sw::WeaponTickOutput &p_out) override {
		std::lock_guard lock(mutex_);
		for (size_t k = 0; k < std::min<size_t>(p_out.shot_count, p_out.shots.size()); ++k) {
			push(shots_, { p_command.tick, p_out.shots[k] });
		}
		for (size_t k = 0; k < std::min<size_t>(p_out.timed_count, p_out.timed.size()); ++k) {
			push(timed_, { p_command.tick, p_slot, p_out.timed[k] });
		}
		const int32_t magazine = int32_t(p_after.current().magazine);
		if (p_slot < last_magazine_.size() && last_magazine_[p_slot] != magazine) {
			last_magazine_[p_slot] = magazine;
			push(magazines_, { p_command.tick, p_slot, magazine });
		}
	}
	void projectiles(uint32_t p_tick, const sw::ProjectileStepOutput &p_out, std::span<const sw::Projectile> p_live) override {
		(void)p_live;
		std::lock_guard lock(mutex_);
		for (const sw::ImpactEvent &e : p_out.impacts) {
			if (e.kind == sw::ImpactKind::Target) {
				push(impacts_, { p_tick, e });
			}
		}
	}
	void damage(uint32_t p_tick, const sw::DamageEvent &p_event, bool p_applied, float p_health_after) override {
		(void)p_tick, (void)p_event, (void)p_health_after;
		std::lock_guard lock(mutex_);
		damage_applied_ += p_applied ? 1u : 0u;
		damage_refused_ += p_applied ? 0u : 1u;
	}
	bool wants_rewind_rows() const override { return rewind_rows_on_; }
	void rewind_rows(uint32_t p_tick, std::span<const host::RewindTraceRow> p_rows) override {
		(void)p_tick;
		std::lock_guard lock(mutex_);
		for (const host::RewindTraceRow &r : p_rows) {
			push(trace_, r);
		}
	}

	std::vector<ServerShot> shots() const {
		std::lock_guard lock(mutex_);
		return shots_;
	}
	std::vector<ServerTimed> timed() const {
		std::lock_guard lock(mutex_);
		return timed_;
	}
	std::vector<ServerMagazine> magazines() const {
		std::lock_guard lock(mutex_);
		return magazines_;
	}
	std::vector<ServerImpact> impacts() const {
		std::lock_guard lock(mutex_);
		return impacts_;
	}
	std::vector<host::RewindTraceRow> trace() const {
		std::lock_guard lock(mutex_);
		return trace_;
	}
	uint64_t damage_applied() const {
		std::lock_guard lock(mutex_);
		return damage_applied_;
	}
	uint64_t dropped() const {
		std::lock_guard lock(mutex_);
		return dropped_;
	}

private:
	template <typename T>
	void push(std::vector<T> &r_v, const T &p_value) {
		if (r_v.size() >= kMaxRecords) {
			++dropped_;
			return;
		}
		r_v.push_back(p_value);
	}

	mutable std::mutex mutex_;
	bool rewind_rows_on_ = false;
	std::vector<ServerShot> shots_;
	std::vector<ServerTimed> timed_;
	std::vector<ServerMagazine> magazines_;
	std::array<int32_t, net::kMaxSlots> last_magazine_ = [] {
		std::array<int32_t, net::kMaxSlots> a{};
		a.fill(-1);
		return a;
	}();
	std::vector<ServerImpact> impacts_;
	std::vector<host::RewindTraceRow> trace_;
	uint64_t damage_applied_ = 0, damage_refused_ = 0, dropped_ = 0;
};

struct ServerOptions {
	std::shared_ptr<const host::SharedLevel> level;
	std::string mechanics_cfg;
	bool exact_aim = false;
	bool infinite_magazine = false;
	uint32_t max_rewind_ticks = host::kDefaultMaxRewindTicks;
	uint64_t match_seed = 0x5EED0000C1u;
	uint32_t target_bots = 0;
	uint64_t seed = 1;
	bool rewind_rows = false;
};

class Server {
public:
	~Server() { stop(); }

	bool start(const ServerOptions &p_options, std::string &r_error) {
		level_ = p_options.level != nullptr ? p_options.level : gunplay_floor();
		log_ = std::make_shared<ServerLog>(p_options.rewind_rows);
		auto game = std::make_shared<host::GameServerConfig>();
		game->level = level_;
		game->max_slots = 64;
		game->dev_commands = true;
		game->seed = p_options.seed;
		host::GunplayConfig &g = game->gunplay;
		g.mechanics_cfg = p_options.mechanics_cfg.empty() ? mechanics_cfg_path() : p_options.mechanics_cfg;
		g.match_seed = p_options.match_seed;
		g.exact_aim = p_options.exact_aim;
		g.infinite_magazine = p_options.infinite_magazine;
		g.lag.max_rewind_ticks = p_options.max_rewind_ticks;
		g.target_bots = p_options.target_bots;
		g.target_team = 2;
		g.observer = log_;
		host::ServerConfig sc;
		sc.game = game;
		std::string err;
		if (!host_.start(sc, &err)) {
			r_error = err.empty() ? std::string("ServerHost::start failed") : err;
			return false;
		}
		return true;
	}
	void stop() { host_.stop(); }
	uint16_t port() const { return host_.port(); }
	host::GameServerStats stats() const { return host_.game_stats(); }
	sim::SoldierState state_of(uint32_t p_slot) const {
		const host::GameServerStats s = stats();
		return p_slot < s.slots.size() ? s.slots[p_slot].state : sim::SoldierState();
	}

	size_t spawned_clients() const {
		const host::GameServerStats s = stats();
		return size_t(std::count_if(s.slots.begin(), s.slots.end(), [](const host::SlotStats &p_s) { return p_s.used && p_s.remote && p_s.life > 0; }));
	}
	const ServerLog &log() const { return *log_; }

private:
	host::ServerHost host_;
	std::shared_ptr<ServerLog> log_;
	std::shared_ptr<const host::SharedLevel> level_;
};

bool same_bits(const void *p_a, const void *p_b, size_t p_n) {
	return std::memcmp(p_a, p_b, p_n) == 0;
}

template <typename T>
bool same_value(const T &p_a, const T &p_b) {
	return same_bits(&p_a, &p_b, sizeof(T));
}

bool same_shot(const sw::ShotEvent &p_a, const sw::ShotEvent &p_b, std::string *r_why = nullptr) {
	auto fail = [&](const char *p_field) {
		if (r_why != nullptr) {
			*r_why = p_field;
		}
		return false;
	};
	if (p_a.time != p_b.time) {
		return fail("time");
	}
	if (p_a.spawn_time != p_b.spawn_time) {
		return fail("spawn_time");
	}
	if (p_a.seed != p_b.seed) {
		return fail("seed");
	}
	if (p_a.player_slot != p_b.player_slot || p_a.shot_index != p_b.shot_index || p_a.def != p_b.def || p_a.weapon_slot != p_b.weapon_slot ||
			p_a.pellets != p_b.pellets) {
		return fail("ids");
	}
	if (!same_value(p_a.origin, p_b.origin)) {
		return fail("origin");
	}
	if (!same_value(p_a.aim, p_b.aim)) {
		return fail("aim");
	}
	if (!same_value(p_a.direction, p_b.direction)) {
		return fail("direction");
	}
	if (!same_value(p_a.spread, p_b.spread) || !same_value(p_a.pellet_cone, p_b.pellet_cone)) {
		return fail("spread");
	}
	if (!same_value(p_a.recoil.pitch, p_b.recoil.pitch) || !same_value(p_a.recoil.yaw, p_b.recoil.yaw)) {
		return fail("recoil");
	}
	if (p_a.tracer != p_b.tracer || !same_value(p_a.power, p_b.power)) {
		return fail("tracer/power");
	}
	return true;
}

std::optional<std::string> env_value(const char *p_name) {
	char *v = nullptr;
	size_t n = 0;
	if (_dupenv_s(&v, &n, p_name) != 0 || v == nullptr) {
		return std::nullopt;
	}
	std::string out(v);
	std::free(v);
	return out;
}

bool timing_enabled(const char *p_case) {
	const std::optional<std::string> e = env_value("OPENBF3_CL_TIMING");
	if (!e.has_value() || *e != "1") {
		const std::string why = std::string(p_case) + ": a timing case (set OPENBF3_CL_TIMING=1 and run under the quiet lock)";
		bf3::test::skip_install_case(why.c_str());
		return false;
	}
	return true;
}

double env_seconds(const char *p_name, double p_default) {
	const std::optional<std::string> e = env_value(p_name);
	return e.has_value() ? std::max(1.0, std::atof(e->c_str())) : p_default;
}

struct Player {
	host::ClientSession client;
	host::InputSample input;
	std::unique_ptr<host::PresentationFrame> frame = std::make_unique<host::PresentationFrame>();
	std::vector<host::PresentEvent> events;
	std::vector<host::PresentEvent> scratch = std::vector<host::PresentEvent>(host::kPresentationEventCapacity);
	float adopt_yaw = NAN;
	bool keep_events = true;

	~Player() { client.stop(); }

	bool start(uint16_t p_port, const std::string &p_netsim, uint64_t p_seed, const std::function<void(host::ClientSessionConfig &)> &p_configure = {},
			uint8_t p_team = 1, std::shared_ptr<const host::SharedLevel> p_level = nullptr) {
		host::ClientSessionConfig cc;
		cc.port = p_port;
		core::Result<net::NetSimConfig> ns = net::parse_netsim(p_netsim);
		if (!ns.ok()) {
			return false;
		}
		cc.netsim = *ns;
		cc.netsim.seed = p_seed * 7919u + 1u;
		cc.request_dev_commands = true;
		cc.mechanics_cfg = mechanics_cfg_path();
		cc.record_predicted = true;
		cc.send_kit_select = true;
		cc.kit_select.kit = 0;
		cc.kit_select.team = p_team;
		if (p_configure) {
			p_configure(cc);
		}
		const core::Status st = client.start(cc, p_level != nullptr ? p_level : gunplay_floor());
		if (!st.ok()) {
			std::printf("Player::start: %s\n", st.error().message.c_str());
		}
		return st.ok();
	}

	void poll() {
		client.read_presentation(*frame);
		const host::PresentationFrame &f = *frame;
		if (f.valid && f.own_curr.life > input.life) {
			input.life = f.own_curr.life;
			input.yaw = std::isnan(adopt_yaw) ? sim::yaw_radians(f.own_curr.yaw) : adopt_yaw;
			input.pitch = sim::pitch_radians(f.own_curr.pitch);
		}
		host::set_view_sample(input, f, net::monotonic_ns());
		client.set_input(input);
		const size_t n = client.events().drain(scratch);
		if (keep_events) {
			events.insert(events.end(), scratch.begin(), scratch.begin() + std::ptrdiff_t(n));
		}
	}

	void run_for(double p_seconds, const std::function<void(double)> &p_each = {}) {
		const auto start = std::chrono::steady_clock::now();
		for (;;) {
			const double t = tg::seconds_since(start);
			if (t >= p_seconds) {
				break;
			}
			if (p_each) {
				p_each(t);
			}
			poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
	}

	bool wait_spawned(double p_timeout_s = 8.0) {
		const auto start = std::chrono::steady_clock::now();
		while (tg::seconds_since(start) < p_timeout_s) {
			poll();
			if (frame->valid && frame->own_curr.life > 0 && input.life == frame->own_curr.life) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
		return false;
	}

	std::optional<net::DebugReport> teleport(const sim::Vec3 &p_position, uint16_t p_yaw) {
		net::DebugRequest q;
		q.kind = net::DebugKind::Teleport;
		q.position = p_position;
		q.yaw = p_yaw;
		q.id = 77;
		client.send_debug(q);
		const auto start = std::chrono::steady_clock::now();
		while (tg::seconds_since(start) < 10.0) {
			poll();
			net::DebugReport r;
			while (client.take_debug_report(r)) {
				if (r.id == q.id) {
					return r;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
		return std::nullopt;
	}

	size_t count(host::PresentKind p_kind) const {
		return size_t(std::count_if(events.begin(), events.end(), [&](const host::PresentEvent &e) { return e.kind == p_kind; }));
	}
};

using Script = std::function<void(Player &, double)>;

class Bot {
public:
	~Bot() { stop(); }

	bool start(uint16_t p_port, uint8_t p_team, uint64_t p_seed, Script p_script, const std::string &p_mechanics_cfg = {},
			std::shared_ptr<const host::SharedLevel> p_level = nullptr) {
		player_.keep_events = false;
		const std::string cfg = p_mechanics_cfg;
		if (!player_.start(
					p_port, "off", p_seed,
					[&](host::ClientSessionConfig &r_c) {
						r_c.name = "bot";
						r_c.record_predicted = false;
						if (!cfg.empty()) {
							r_c.mechanics_cfg = cfg;
						}
					},
					p_team, std::move(p_level))) {
			return false;
		}
		script_ = std::move(p_script);
		stop_ = false;
		thread_ = std::thread([this] { run(); });
		return true;
	}
	void stop() {
		if (thread_.joinable()) {
			stop_ = true;
			thread_.join();
		}
		player_.client.stop();
	}
	host::ClientStats stats() { return player_.client.stats(); }

	int slot() {
		const host::ClientStats s = stats();
		return s.spawned ? int(s.slot) : -1;
	}
	bool wait_spawned(double p_timeout_s = 10.0) {
		const auto start = std::chrono::steady_clock::now();
		while (tg::seconds_since(start) < p_timeout_s) {
			if (slot() >= 0 && spawned_.load()) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return false;
	}

	std::optional<net::DebugReport> teleport(const sim::Vec3 &p_position, uint16_t p_yaw, double p_timeout_s = 10.0) {
		uint32_t id = 0;
		{
			std::lock_guard lock(debug_mutex_);
			net::DebugRequest q;
			q.kind = net::DebugKind::Teleport;
			q.position = p_position;
			q.yaw = p_yaw;
			q.id = id = next_debug_id_++;
			pending_ = q;
			report_.reset();
		}
		const auto start = std::chrono::steady_clock::now();
		while (tg::seconds_since(start) < p_timeout_s) {
			{
				std::lock_guard lock(debug_mutex_);
				if (report_.has_value() && report_->id == id) {
					return report_;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return std::nullopt;
	}

private:
	void run() {
		const auto start = std::chrono::steady_clock::now();
		while (!stop_.load()) {
			{
				std::lock_guard lock(debug_mutex_);
				if (pending_.has_value()) {
					player_.client.send_debug(*pending_);
					pending_.reset();
				}
				net::DebugReport r;
				while (player_.client.take_debug_report(r)) {
					report_ = r;
				}
			}
			const bool live = player_.frame->valid && player_.input.life > 0 && player_.input.life == player_.frame->own_curr.life;
			spawned_ = live;
			if (live && script_) {
				script_(player_, tg::seconds_since(start));
			}
			player_.poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
	}

	Player player_;
	Script script_;
	std::thread thread_;
	std::atomic<bool> stop_{ false };
	std::atomic<bool> spawned_{ false };
	std::mutex debug_mutex_;
	std::optional<net::DebugRequest> pending_;
	std::optional<net::DebugReport> report_;
	uint32_t next_debug_id_ = 900;
};

Script firing_script(uint32_t p_phase) {
	return [p_phase](Player &r_p, double p_t) {
		r_p.input.yaw = float(std::fmod(p_t * 1.7, 6.283185307179586));
		r_p.input.pitch = -0.3f;
		r_p.input.move_forward = 0.0f;
		r_p.input.held_buttons = std::fmod(p_t * 60.0 + double(p_phase), 60.0) < 10.0 ? sim::buttons::kFire : 0u;
	};
}

struct StrafeSpec {
	std::mutex mutex;
	uint64_t generation = 0;
	sim::Vec3 origin{};
	float yaw0 = 0;

	void set(const sim::Vec3 &p_origin, float p_yaw0) {
		std::lock_guard lock(mutex);
		origin = p_origin;
		yaw0 = p_yaw0;
		++generation;
	}
};

Script strafing_script(std::shared_ptr<StrafeSpec> p_spec = nullptr) {
	struct State {
		bool init = false;
		uint32_t life = 0;
		uint64_t generation = 0;
		sim::Vec3 origin{};
		float yaw0 = 0;
		int dir = 1;
		double leg_start = 0;
	};
	auto st = std::make_shared<State>();
	return [st, p_spec](Player &r_p, double p_t) {
		const host::PresentationFrame &f = *r_p.frame;
		uint64_t generation = 0;
		sim::Vec3 origin{};
		float yaw0 = 0;
		if (p_spec != nullptr) {
			std::lock_guard lock(p_spec->mutex);
			generation = p_spec->generation;
			origin = p_spec->origin;
			yaw0 = p_spec->yaw0;
		}
		if (!st->init || st->life != f.own_curr.life || st->generation != generation) {
			st->init = true;
			st->life = f.own_curr.life;
			st->generation = generation;
			st->origin = generation != 0 ? origin : f.own_curr.position;
			st->yaw0 = generation != 0 ? yaw0 : sim::yaw_radians(f.own_curr.yaw);
			st->dir = 1;
			st->leg_start = p_t;
		}
		const float side = st->yaw0 + 1.5707963f;
		const float sx = std::sin(side), sz = std::cos(side);
		const float off = (f.own_curr.position[0] - st->origin[0]) * sx + (f.own_curr.position[2] - st->origin[2]) * sz;
		const int before = st->dir;
		if (off > 4.0f) {
			st->dir = -1;
		} else if (off < -4.0f) {
			st->dir = 1;
		} else if (p_t - st->leg_start > 3.0) {
			st->dir = -st->dir;
		}
		if (st->dir != before) {
			st->leg_start = p_t;
		}
		r_p.input.yaw = st->dir > 0 ? side : st->yaw0 - 1.5707963f;
		r_p.input.pitch = 0.0f;
		r_p.input.move_forward = 1.0f;
		r_p.input.held_buttons = 0;
	};
}

void print_client(const char *p_label, const host::ClientStats &p_s) {
	std::printf("%s: reconciliations %" PRIu64 ", weapon_reconciliations %" PRIu64 ", weapon_ring_misses %" PRIu64 ", health_rebases %" PRIu64
				", ring_misses %" PRIu64 ", predicted_shots %" PRIu64 ", predicted_timed %" PRIu64 ", no_view_shots %" PRIu64 ", remote_shots %" PRIu64
				", hits %" PRIu64 ", snapshot_others %" PRIu64 ", late_sample %" PRIu64 ", generation_reorder %" PRIu64 ", others_ahead %" PRIu64
				", track_timeout %" PRIu64 ", events_overflow %" PRIu64 ", step_precondition_failures %" PRIu64 ", lane gaps %" PRIu64 ", rtt %.1f ms\n",
			p_label, p_s.reconciliations, p_s.weapon_reconciliations, p_s.weapon_ring_misses, p_s.health_rebases, p_s.ring_misses, p_s.predicted_shots,
			p_s.predicted_timed, p_s.no_view_shots, p_s.remote_shots, p_s.combat_hits, p_s.snapshot_others_received, p_s.late_sample,
			p_s.generation_reorder, p_s.others_ahead, p_s.track_timeout, p_s.events_overflow, p_s.step_precondition_failures, p_s.lane.gaps, p_s.rtt_ms);
}

void print_server(const char *p_label, const host::GameServerStats &p_s) {
	const host::GunplayStats &g = p_s.gunplay;
	std::printf("%s (server): gunplay %d, shots %" PRIu64 ", hits %" PRIu64 ", deaths %" PRIu64 ", weapon_precondition_failures %" PRIu64
				", step_precondition_failures %" PRIu64 ", view_refused %" PRIu64 ", no_ack_state %" PRIu64 ", others_dropped %" PRIu64
				", lag present %" PRIu64 " rewound %" PRIu64 " clamped %" PRIu64 " refused %" PRIu64 " acks_ignored %" PRIu64 ", weapon_hash 0x%016" PRIx64 "\n",
			p_label, int(p_s.gunplay_active), g.shots, g.hits, g.deaths, g.weapon_precondition_failures, p_s.step_precondition_failures, g.view_refused,
			g.no_ack_state, g.others_dropped, g.lag.present, g.lag.rewound, g.lag.clamped, g.lag.refused, g.lag.acks_ignored, p_s.weapon_hash);
}

net::RemoteShot hostile_shot(sim::Tick p_tick, const sim::Vec3 &p_eye) {
	net::RemoteShot r;
	r.slot = 63;
	r.shot_index = 1000u + p_tick;
	r.def = uint16_t(net::kMaxWireDefs - 1);
	r.time = sw::tick_start(p_tick);
	r.origin = p_eye;
	r.direction = { 0.0f, 0.0f, 1.0f };
	return r;
}

class HostileRelay {
public:
	~HostileRelay() { stop(); }

	bool start(uint16_t p_server_port, const net::ShotQuantization &p_frame, std::string &r_error) {
		server_port_ = p_server_port;
		frame_ = p_frame;
		std::string err;
		front_ = net::Host::create_server("127.0.0.1", 0, 4, net::kChannelCount, &err);
		if (front_ == nullptr) {
			r_error = err;
			return false;
		}
		port_ = front_->bound_port();
		stop_ = false;
		thread_ = std::thread([this] { run(); });
		return true;
	}
	void stop() {
		if (!thread_.joinable()) {
			return;
		}
		stop_ = true;
		thread_.join();
		for (net::Host *h : { back_.get(), front_.get() }) {
			if (h != nullptr) {
				h->disconnect_all();
				h->flush();
			}
		}
		back_.reset();
		front_.reset();
	}
	uint16_t port() const { return port_; }
	uint64_t hostile_sent() const { return hostile_sent_.load(); }
	uint64_t forwarded() const { return forwarded_.load(); }

private:
	static net::Host::SendMode mode_of(uint8_t p_channel) {
		return p_channel == net::kChannelUnreliable ? net::Host::SendMode::Unsequenced : net::Host::SendMode::Reliable;
	}
	void to_client(uint8_t p_channel, const std::vector<uint8_t> &p_bytes) {
		if (have_front_ && !p_bytes.empty()) {
			front_->send(front_peer_, p_channel, p_bytes.data(), p_bytes.size(), mode_of(p_channel));
		}
	}
	void mutated_copies(const std::vector<uint8_t> &p_bytes) {
		if (p_bytes.size() < 2) {
			return;
		}
		for (int k = 0; k < 2; ++k) {
			std::vector<uint8_t> b = p_bytes;
			switch (rng_() % 3) {
				case 0: {
					const int flips = 1 + int(rng_() % 8);
					for (int f = 0; f < flips; ++f) {
						const size_t at = 1 + size_t(rng_() % (b.size() - 1));
						b[at] ^= uint8_t(1u << (rng_() % 8));
					}
					break;
				}
				case 1:
					b.resize(1 + size_t(rng_() % (b.size() - 1)));
					break;
				default:
					for (int t = 0, n = 1 + int(rng_() % 16); t < n; ++t) {
						b.push_back(uint8_t(rng_()));
					}
					break;
			}
			to_client(net::kChannelUnreliable, b);
			++hostile_sent_;
		}
	}
	void hostile_values(sim::Tick p_tick) {
		const sim::Vec3 eye{ 0.0f, 1.6f, 0.0f };
		net::ShotEvents bad;
		bad.server_tick = p_tick;
		bad.shots.push_back(hostile_shot(p_tick, eye));
		to_client(net::kChannelUnreliable, net::encode_shot_events(bad, frame_, eye));
		net::CombatEvents combat;
		combat.server_tick = p_tick;
		net::HitEvent hit;
		hit.victim = 250;
		hit.shooter = 251;
		hit.damage = 1e30f;
		hit.health_after = -1e30f;
		combat.hits.push_back(hit);
		to_client(net::kChannelReliable, net::encode_combat_events(combat, frame_));
		hostile_sent_ += 2;
	}
	void from_server(uint8_t p_channel, const std::vector<uint8_t> &p_bytes) {
		++forwarded_;
		const std::optional<net::MessageType> type = net::peek_type(p_bytes);
		if (type == net::MessageType::Snapshot) {
			if (++snapshots_ % 20u == 0) {
				core::Result<net::Snapshot> s = net::decode_snapshot(p_bytes);
				if (s && s->own_weapons.has_value()) {
					s->own_weapons->slots[s->own_weapons->active % s->own_weapons->slots.size()].def = 0xFFF0;
					to_client(p_channel, net::encode_snapshot(*s));
					++hostile_sent_;
				} else {
					to_client(p_channel, p_bytes);
				}
				hostile_values(s ? s->server_tick : 0u);
				return;
			}
			to_client(p_channel, p_bytes);
			return;
		}
		to_client(p_channel, p_bytes);
		if (type == net::MessageType::SnapshotOthers || type == net::MessageType::ShotEvents || type == net::MessageType::CombatEvents) {
			mutated_copies(p_bytes);
		}
	}
	void run() {
		std::vector<std::pair<uint8_t, std::vector<uint8_t>>> pending;
		while (!stop_.load()) {
			net::Host::Event ev;
			while (front_->service(0, ev) > 0) {
				if (ev.type == net::Host::EventType::Connect && !have_front_) {
					have_front_ = true;
					front_peer_ = ev.peer;
					std::string err;
					back_ = net::Host::create_client(net::kChannelCount, &err);
					back_peer_ = back_ != nullptr ? back_->connect("127.0.0.1", server_port_) : -1;
					back_up_ = false;
				} else if (ev.type == net::Host::EventType::Disconnect && have_front_ && ev.peer == front_peer_) {
					have_front_ = false;
					if (back_ != nullptr) {
						back_->disconnect_all();
						back_->flush();
					}
				} else if (ev.type == net::Host::EventType::Receive) {
					if (back_up_) {
						back_->send(uint32_t(back_peer_), ev.channel, ev.data.data(), ev.data.size(), mode_of(ev.channel));
					} else {
						pending.push_back({ ev.channel, ev.data });
					}
				}
			}
			if (back_ != nullptr && back_peer_ >= 0) {
				while (back_->service(0, ev) > 0) {
					if (ev.type == net::Host::EventType::Connect) {
						back_up_ = true;
						for (const auto &[channel, data] : pending) {
							back_->send(uint32_t(back_peer_), channel, data.data(), data.size(), mode_of(channel));
						}
						pending.clear();
					} else if (ev.type == net::Host::EventType::Disconnect) {
						back_up_ = false;
						if (have_front_) {
							front_->disconnect_all();
						}
					} else if (ev.type == net::Host::EventType::Receive) {
						from_server(ev.channel, ev.data);
					}
				}
				back_->flush();
			}
			front_->flush();
			std::this_thread::sleep_for(std::chrono::microseconds(500));
		}
	}

	uint16_t server_port_ = 0;
	net::ShotQuantization frame_;
	std::unique_ptr<net::Host> front_, back_;
	bool have_front_ = false, back_up_ = false;
	uint32_t front_peer_ = 0;
	int back_peer_ = -1;
	uint64_t snapshots_ = 0;
	std::mt19937_64 rng_{ 0xB0B5EED };
	uint16_t port_ = 0;
	std::thread thread_;
	std::atomic<bool> stop_{ false };
	std::atomic<uint64_t> hostile_sent_{ 0 }, forwarded_{ 0 };
};

struct SyntheticRecords final : host::ReceivedRecordSource {
	std::map<uint16_t, std::pair<uint32_t, std::vector<net::OtherSoldier>>> seqs;
	size_t candidates(const host::ShotView &p_view, uint8_t p_target, std::span<net::TimedOther> r_out) const override {
		size_t n = 0;
		for (uint32_t i = 0; i < 32 && n < r_out.size(); ++i) {
			if (((p_view.main_bits >> i) & 1u) == 0) {
				continue;
			}
			auto it = seqs.find(uint16_t(p_view.view_seq - i));
			if (it == seqs.end()) {
				continue;
			}
			for (const net::OtherSoldier &o : it->second.second) {
				if (o.slot == p_target && n < r_out.size()) {
					r_out[n++] = { it->second.first, o };
				}
			}
		}
		return n;
	}
	bool view_tick(uint16_t p_seq, uint32_t &r_tick) const override {
		auto it = seqs.find(p_seq);
		if (it == seqs.end()) {
			return false;
		}
		r_tick = it->second.first;
		return true;
	}
	void slots_in(const host::ShotView &p_view, std::array<uint64_t, 2> &r_slots) const override {
		r_slots = {};
		for (uint32_t i = 0; i < 32; ++i) {
			if (((p_view.main_bits >> i) & 1u) == 0) {
				continue;
			}
			auto it = seqs.find(uint16_t(p_view.view_seq - i));
			if (it == seqs.end()) {
				continue;
			}
			for (const net::OtherSoldier &o : it->second.second) {
				r_slots[o.slot >> 6] |= uint64_t(1) << (o.slot & 63);
			}
		}
	}
};

net::OtherSoldier record_at(uint8_t p_slot, const sim::Vec3 &p_feet, const sim::Vec3 &p_velocity) {
	sim::SoldierState s;
	s.life = 1;
	s.position = p_feet;
	s.velocity = p_velocity;
	s.flags = sim::soldier_flags::kAlive;
	return net::quantize_other(p_slot, s);
}

sw::ShotEvent own_shot(const sw::WeaponTable &p_table, uint16_t p_def, uint8_t p_slot, uint32_t p_index, sim::Tick p_tick, const sim::Vec3 &p_origin,
		const sim::Vec3 &p_dir) {
	sw::ShotEvent e;
	e.time = sw::tick_start(p_tick);
	e.spawn_time = e.time + p_table.defs[p_def].spawn_delay;
	e.seed = sw::shot_seed(1, p_slot, p_index);
	e.player_slot = p_slot;
	e.shot_index = p_index;
	e.def = p_def;
	e.origin = p_origin;
	e.aim = p_dir;
	e.direction = p_dir;
	return e;
}

std::shared_ptr<const host::SharedLevel> floor_level() {
	return gunplay_floor();
}

}

TEST_SUITE("host-gunplay-client-unit") {
	TEST_CASE("host.gunplay.client: PresentationEvents keeps order across threads and counts a full ring") {
		auto ring = std::make_unique<host::PresentationEvents>();
		std::vector<host::PresentEvent> out(host::kPresentationEventCapacity + 10);
		for (uint32_t i = 0; i < host::kPresentationEventCapacity + 5; ++i) {
			host::PresentEvent e;
			e.id = i;
			const bool ok = ring->push(e);
			CHECK(ok == (i < host::kPresentationEventCapacity));
		}
		CHECK(ring->overflows() == 5);
		CHECK(ring->drain(out) == host::kPresentationEventCapacity);
		CHECK(out[0].id == 0);
		CHECK(out[host::kPresentationEventCapacity - 1].id == host::kPresentationEventCapacity - 1);

		constexpr uint32_t kCount = 1'000'000;
		std::atomic<bool> done{ false };
		std::thread producer([&] {
			for (uint32_t i = 0; i < kCount;) {
				host::PresentEvent e;
				e.id = i;
				e.seed = uint64_t(i) * 0x9E3779B97F4A7C15ull;
				if (ring->push(e)) {
					++i;
				} else {
					std::this_thread::yield();
				}
			}
			done = true;
		});
		uint32_t expect = 0;
		bool order = true;
		std::vector<host::PresentEvent> batch(512);
		while (expect < kCount) {
			const size_t n = ring->drain(batch);
			for (size_t k = 0; k < n; ++k) {
				order = order && batch[k].id == expect && batch[k].seed == uint64_t(expect) * 0x9E3779B97F4A7C15ull;
				++expect;
			}
			if (n == 0) {
				std::this_thread::yield();
			}
		}
		producer.join();
		CHECK(order);
		std::printf("events ring: %u events in order across threads, overflows %" PRIu64 " (the producer retried on full)\n", kCount, ring->overflows());
	}

	TEST_CASE("host.gunplay.client: present_weapon from two predicted ticks (phase, ammo, spread and recoil lerp, zoom, no lerp across items)") {
		const Tables &t = tables();
		sw::SoldierWeaponsState w;
		w.player_slot = 3;
		const std::array<uint16_t, 2> loadout{ t.rifle, t.pistol };
		REQUIRE(sw::spawn_soldier_weapons(w, *t.table, loadout, 0, 100).ok());
		sw::WeaponContext ctx;
		ctx.eye_start = { 0, 1.6f, 0 };
		ctx.eye_end = { 0, 1.6f, 0 };
		std::vector<sw::SoldierWeaponsState> states{ w };
		for (sim::Tick tick = 101; tick <= 200; ++tick) {
			sim::Command c;
			c.tick = tick;
			if (tick >= 160 && tick < 170) {
				c.buttons = sim::buttons::kFire;
			}
			if (tick >= 180) {
				c.buttons = sim::buttons::kZoom;
			}
			ctx.yaw = c.yaw;
			ctx.pitch = c.pitch;
			sw::WeaponTickOutput out;
			sw::step_weapons_m2a(*t.table, w, c, ctx, 1, 100, out);
			states.push_back(w);
		}
		auto view = [&](sim::Tick p_tick) { return net::weapons_wire_view(states[p_tick - 100], p_tick); };

		const host::PresentedWeapon d = host::present_weapon(view(110), view(111), 111, 0.5, *t.table);
		REQUIRE(d.valid);
		CHECK(d.def == t.rifle);
		CHECK(d.magazine == 31);
		CHECK(d.magazine_capacity == 31);
		CHECK(d.phase_progress > 0.0f);
		CHECK(d.phase_progress < 1.0f);
		CHECK(std::fabs(d.time_s - (sw::units_to_seconds(sw::tick_end(110)) + 0.5 / 60.0)) < 1e-9);

		const host::PresentedWeapon f0 = host::present_weapon(view(164), view(165), 165, 0.0, *t.table);
		const host::PresentedWeapon f1 = host::present_weapon(view(164), view(165), 165, 1.0, *t.table);
		const host::PresentedWeapon fh = host::present_weapon(view(164), view(165), 165, 0.5, *t.table);
		CHECK(f0.magazine < 31);
		CHECK(f0.spread_deg == doctest::Approx(view(164).timers[0].spread));
		CHECK(f1.spread_deg == doctest::Approx(view(165).timers[0].spread));
		CHECK(fh.spread_deg == doctest::Approx(0.5f * (f0.spread_deg + f1.spread_deg)));
		CHECK(fh.recoil.pitch == doctest::Approx(0.5f * (f0.recoil.pitch + f1.recoil.pitch)));

		const host::PresentedWeapon z = host::present_weapon(view(190), view(191), 191, 0.3, *t.table);
		CHECK(z.zoomed);

		net::WeaponsWire other = view(164);
		other.slots[other.active].def = t.pistol;
		const host::PresentedWeapon s = host::present_weapon(other, view(165), 165, 0.5, *t.table);
		CHECK(s.spread_deg == doctest::Approx(view(165).timers[0].spread));

		net::WeaponsWire bad = view(165);
		bad.slots[bad.active].def = uint16_t(t.table->defs.size() + 3);
		CHECK(!host::present_weapon(view(164), bad, 165, 0.5, *t.table).valid);
		bad = view(165);
		bad.active = 200;
		CHECK(!host::present_weapon(view(164), bad, 165, 0.5, *t.table).valid);
	}

	TEST_CASE("host.gunplay.client: the weapon lane replays under catch_up without emitting twice; a dead soldier steps no weapon") {
		const Tables &t = tables();
		std::shared_ptr<const host::SharedLevel> level = floor_level();
		REQUIRE(level != nullptr);
		core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(level->static_world);
		REQUIRE(world.ok());
		sim::SoldierMotor motor;
		REQUIRE(motor.init(**world, level->motor).ok());
		const sim::SettleResult spawn = sim::settle_spawn(motor, **world, level->spawns[0], 1, 50);
		REQUIRE(spawn.ok);
		struct Sink final : sim::WeaponEmitSink {
			std::vector<std::pair<sim::Tick, uint32_t>> shots;
			uint64_t ticks = 0;
			void emitted(const sim::Command &p_c, const sw::WeaponTickOutput &p_o, const sim::WeaponPredictionEntry &) override {
				++ticks;
				for (size_t i = 0; i < p_o.shot_count; ++i) {
					shots.push_back({ p_c.tick, p_o.shots[i].shot_index });
				}
			}
		} sink;
		sim::PredictionRing ring(128);
		sim::WeaponLane lane(128, t.table, level->motor, 0x1234);
		lane.set_sink(&sink);
		ring.attach_weapons(&lane);
		sim::WeaponPredictionEntry w0;
		w0.tick = 50;
		w0.spawn_tick = 50;
		w0.state.player_slot = 2;
		const std::array<uint16_t, 2> loadout{ t.rifle, t.pistol };
		REQUIRE(sw::spawn_soldier_weapons(w0.state, *t.table, loadout, 0, 50).ok());
		lane.put(w0);
		lane.set_emitted_through(50);
		ring.put({ sim::hold_command(spawn.state), spawn.state, false });
		std::map<sim::Tick, uint64_t> first;
		sim::SoldierState state = spawn.state;
		for (sim::Tick tick = 51; tick <= 170; ++tick) {
			sim::Command c;
			c.tick = tick;
			c.sequence = tick;
			c.yaw = uint16_t(tick * 37u);
			c.move_forward = tick % 40u < 20u ? int8_t(127) : int8_t(0);
			if (tick >= 100 && tick < 130) {
				c.buttons |= sim::buttons::kFire;
			}
			const sim::SoldierState before = state;
			motor.step(state, c);
			const sim::WeaponPredictionEntry e = lane.step(*lane.find(tick - 1u), c, before, state);
			lane.put(e);
			ring.put({ c, state, true });
			first[tick] = sw::soldier_weapons_hash(e.state);
		}
		const size_t shots = sink.shots.size();
		CHECK(shots >= 6);
		CHECK(sink.ticks == 120);

		const sim::SoldierState base = ring.find(90)->state;
		const sim::CatchUpStats c = sim::catch_up(motor, base, ring, 171);
		CHECK(c.steps == 80);
		bool equal = true;
		for (sim::Tick tick = 90; tick <= 170; ++tick) {
			const sim::WeaponPredictionEntry *e = lane.find(tick);
			equal = equal && e != nullptr && sw::soldier_weapons_hash(e->state) == first[tick];
		}
		CHECK(equal);
		CHECK(sink.shots.size() == shots);
		CHECK(lane.stats().replayed_ticks == 80);
		CHECK(lane.stats().gaps == 0);

		sim::WeaponPredictionEntry rebased = *lane.find(95);
		rebased.state.slots[0].magazine = 3;
		lane.put(rebased);
		(void)sim::catch_up(motor, ring.find(95)->state, ring, 171);
		CHECK(lane.find(170)->state.slots[0].magazine <= 3);

		sim::SoldierState dead = state;
		dead.mode = sim::MoveMode::Dead;
		sim::Command c2;
		c2.tick = 171;
		c2.buttons = sim::buttons::kFire;
		const sim::WeaponPredictionEntry carried = lane.step(*lane.find(170), c2, dead, dead);
		CHECK(carried.state.tick == 171);
		CHECK(carried.state.events == 0);
		CHECK(carried.state.next_shot_index == lane.find(170)->state.next_shot_index);
		CHECK(lane.stats().dead_ticks == 1);
		std::printf("weapon lane: %zu shots emitted once over 120 ticks, 80-tick replay equal by soldier_weapons_hash, dead tick carried\n", shots);
	}

	TEST_CASE("host.gunplay.client: ClientProjectiles: own world on the rewound records (E12 log), remote world, impacts, seeds, refusals") {
		const Tables &t = tables();
		std::shared_ptr<const host::SharedLevel> level = floor_level();
		core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(level->static_world);
		REQUIRE(world.ok());
		host::ClientProjectilesConfig pc;
		pc.own_slot = 2;
		core::Result<std::unique_ptr<host::ClientProjectiles>> made = host::ClientProjectiles::create(t.table, t.capsules, 1, pc);
		REQUIRE(made.ok());
		host::ClientProjectiles &p = **made;
		CHECK(!host::ClientProjectiles::create(nullptr, t.capsules, 1).ok());
		auto ring = std::make_unique<host::PresentationEvents>();

		SyntheticRecords records;
		records.seqs[9] = { 98, { record_at(5, { 0.0f, 0.0f, 20.0f }, { 0, 0, 0 }) } };
		records.seqs[10] = { 100, { record_at(5, { 0.0f, 0.0f, 20.0f }, { 0, 0, 0 }) } };
		host::ShotView view;
		view.quality = uint8_t(sim::ViewQuality::Interpolated);
		view.view_seq = 10;
		view.main_bits = 0b11;
		const sim::Tick tick = 110;
		view.view_back_q8 = int32_t((tick << 8) - (100u << 8));
		const sim::Vec3 origin{ 0.0f, 1.3f, 0.0f };
		const sw::ShotEvent shot = own_shot(*t.table, t.rifle, 2, 7, tick, origin, { 0.0f, 0.0f, 1.0f });
		p.spawn_own(shot, view);
		for (sim::Tick k = tick; k < tick + 10; ++k) {
			p.step_own(k, **world, records, *ring);
		}
		std::vector<host::PresentEvent> ev(256);
		size_t n = ring->drain(ev);
		size_t contacts = 0;
		for (size_t i = 0; i < n; ++i) {
			if (ev[i].kind == host::PresentKind::SoldierContact) {
				++contacts;
				CHECK(ev[i].aux == 5);
				CHECK(ev[i].id == 7);
				CHECK(ev[i].timeline == host::PresentTimeline::Predicted);
				CHECK(uint8_t(ev[i].value) == uint8_t(sw::HitZone::Chest));
			}
		}
		CHECK(contacts == 1);
		std::vector<host::OwnTestSample> samples(64);
		const size_t got = p.take_samples(samples);
		CHECK(got >= 1);
		if (got >= 1) {
			CHECK(samples[0].target == 5);
			CHECK(samples[0].shot_index == 7);
			CHECK(samples[0].time_q8 == net::rewind_time_q8(sw::tick_start(tick), view.view_back_q8, host::kDefaultMaxRewindTicks));
		}

		const sw::ShotEvent stray = own_shot(*t.table, t.rifle, 2, 8, tick + 20, origin, { 0.0f, 0.0f, 1.0f });
		host::ShotView none;
		none.view_seq = 99;
		none.quality = uint8_t(sim::ViewQuality::Interpolated);
		p.spawn_own(stray, none);
		for (sim::Tick k = tick + 20; k < tick + 30; ++k) {
			p.step_own(k, **world, records, *ring);
		}
		n = ring->drain(ev);
		CHECK(std::none_of(ev.begin(), ev.begin() + std::ptrdiff_t(n), [](const host::PresentEvent &e) { return e.kind == host::PresentKind::SoldierContact; }));
		CHECK(p.stats().own_no_view > 0);

		net::RemoteShot r;
		r.slot = 9;
		r.shot_index = 41;
		r.def = t.rifle;
		r.time = sw::tick_start(300);
		r.origin = { 5.0f, 1.6f, 5.0f };
		r.direction = { 0.0f, -0.7071068f, 0.7071068f };
		p.queue_remote(r);
		net::RemoteShot bad = r;
		bad.def = uint16_t(t.table->defs.size() + 1);
		p.queue_remote(bad);
		CHECK(p.stats().refused == 1);
		const std::vector<net::OtherPose> others;
		p.step_remote(295.0, **world, others, *ring);
		CHECK(p.stats().remote_spawned == 0);
		for (double rt = 296.0; rt <= 320.0; rt += 1.0) {
			p.step_remote(rt, **world, others, *ring);
		}
		CHECK(p.stats().remote_spawned == 1);
		n = ring->drain(ev);
		size_t impacts = 0;
		for (size_t i = 0; i < n; ++i) {
			if (ev[i].kind == host::PresentKind::Impact && ev[i].slot == 9) {
				++impacts;
				CHECK(ev[i].timeline == host::PresentTimeline::Render);
				CHECK(ev[i].seed == host::impact_seed(host::effect_seed(1, 9, 41), 0, ev[i].extra));
				CHECK(std::fabs(ev[i].pos[1]) < 0.01f);
			}
		}
		CHECK(impacts == 1);

		CHECK(host::effect_seed(1, 9, 41) == sw::splitmix64(1 ^ (uint64_t(9) << 32 | 41u)));
		sw::ShotEvent derived;
		REQUIRE(host::shot_event_of(r, *t.table, 1, derived));
		CHECK(derived.seed == sw::shot_seed(1, 9, 41));
		CHECK(derived.spawn_time == r.time + t.table->defs[t.rifle].spawn_delay);
		CHECK(!host::shot_event_of(bad, *t.table, 1, derived));
		std::printf("client projectiles: own contact %zu (chest of slot 5 at its rewound record), %" PRIu64 " E12 samples, remote impact %zu, refused %" PRIu64 "\n",
				contacts, p.stats().samples_logged, impacts, p.stats().refused);
	}

	TEST_CASE("host.gunplay.client: the view sample, present_view_quality and window_extrapolated") {
		auto f = std::make_unique<host::PresentationFrame>();
		CHECK(host::present_view_quality(*f, 100.0) == sim::ViewQuality::None);
		f->serial = 5;
		f->publish_ns = 1'000'000'000ull;
		f->render_ticks_at_publish = 100.0;
		f->ticks_per_ns = 60e-9;
		f->newest_seq = 321;
		f->other_count = 1;
		host::OtherTrack &t = f->others[0];
		t.slot = 4;
		t.count = 2;
		t.samples[0] = { 98, record_at(4, { 0, 0, 0 }, { 1, 0, 0 }) };
		t.samples[1] = { 100, record_at(4, { 0.0333f, 0, 0 }, { 1, 0, 0 }) };
		t.track_newest_tick = 100;
		CHECK(host::present_view_quality(*f, 99.0) == sim::ViewQuality::Interpolated);
		CHECK(host::present_view_quality(*f, 101.0) == sim::ViewQuality::Extrapolated);
		CHECK(host::present_view_quality(*f, 100.0 + 15.5) == sim::ViewQuality::Held);
		host::InputSample in;
		host::set_view_sample(in, *f, f->publish_ns + 8'333'333ull);
		CHECK(in.view_valid);
		CHECK(in.view_render_ticks == doctest::Approx(100.5));
		CHECK(in.view_quality == uint8_t(sim::ViewQuality::Extrapolated));
		CHECK(in.view_seq == 321);
		std::array<host::PresentedSoldier, 4> out;
		host::PresentOthersCounts counts;
		CHECK(host::present_others(*f, f->publish_ns + 16'666'667ull, out, counts) == 1);
		CHECK(counts.extrapolated == 1);
		CHECK(counts.window_extrapolated == 0);
		t.track_newest_tick = 102;
		host::PresentOthersCounts counts2;
		host::present_others(*f, f->publish_ns + 16'666'667ull, out, counts2);
		CHECK(counts2.window_extrapolated == 1);

		uint64_t extra = 0;
		std::array<host::PresentedSoldier, 4> out1;
		CHECK(host::present_others(*f, f->publish_ns + 16'666'667ull, out1, &extra) == 1);
		CHECK(extra == 1);
		CHECK(same_value(out1[0].position, out[0].position));
		CHECK(out1[0].target_pose == out[0].target_pose);
	}

	TEST_CASE("host.gunplay.client: E6: a client whose weapon table differs is refused on MatchInfo, never sends Ready, never predicts") {
		const std::string variant = cfg_with("variant.cfg", "disc", "uniform_radius");
		const std::string missing = (scratch_dir() / "missing.cfg").string();
		core::Result<std::shared_ptr<const host::ClientGunplayInputs>> own = host::client_gunplay_inputs(*gunplay_floor());
		REQUIRE(own.ok());
		auto other_kits = std::make_shared<host::ClientGunplayInputs>(**own);
		other_kits->kits.hash ^= 0xBADull;
		struct Row {
			const char *label;
			std::string cfg;
			bool kits_differ;
			bool no_inputs;
			const char *reason;
		};
		const Row rows[] = {
			{ "mechanics differ (variant.cfg: disc = uniform_radius)", variant, false, false, "weapon table mismatch" },
			{ "cfg unreadable (missing.cfg)", missing, false, false, "weapon table mismatch" },
			{ "no weapon table (gunplay_from_level off, no inputs)", mechanics_cfg_path(), false, true, "weapon table mismatch" },
			{ "kit table differs", mechanics_cfg_path(), true, false, "kit table mismatch" },
		};
		for (const Row &row : rows) {
			Server server;
			std::string err;
			REQUIRE_MESSAGE(server.start(ServerOptions(), err), err);
			Player p;
			REQUIRE(p.start(server.port(), "off", 3, [&](host::ClientSessionConfig &r_c) {
				r_c.mechanics_cfg = row.cfg;
				if (row.no_inputs) {
					r_c.gunplay = nullptr;
					r_c.gunplay_from_level = false;
				} else if (row.kits_differ) {
					r_c.gunplay = other_kits;
				}
			}));
			p.run_for(1.5);
			const host::ClientStats s = p.client.stats();
			const size_t spawned = server.spawned_clients();
			const std::string error(s.last_error.data());
			CHECK_MESSAGE(s.refused, row.label);
			CHECK_MESSAGE(error.find(row.reason) != std::string::npos, row.label << ": " << error);
			CHECK_MESSAGE((row.kits_differ ? s.kit_hash_mismatch : s.weapon_hash_mismatch) == 1, row.label);
			CHECK_MESSAGE(spawned == 0, row.label);
			CHECK_MESSAGE(s.commands_sent == 0, row.label);
			CHECK_MESSAGE(!p.frame->valid, row.label);
			CHECK_MESSAGE(!s.connected, row.label);
			std::printf("E6 %s: refused \"%s\", weapon_hash_mismatch %" PRIu64 ", kit_hash_mismatch %" PRIu64 ", spawned on the server %zu, commands %" PRIu64 "\n",
					row.label, error.c_str(), s.weapon_hash_mismatch, s.kit_hash_mismatch, spawned, s.commands_sent);
		}

		for (const bool gunplay : { true, false }) {
			Server server;
			std::string err;
			ServerOptions so;
			so.level = gunplay ? gunplay_floor() : tg::synthetic_floor();
			REQUIRE_MESSAGE(server.start(so, err), err);
			Player p;
			REQUIRE(p.start(server.port(), "off", 4, {}, 1, so.level));
			CHECK(p.wait_spawned());
			p.run_for(0.5);
			const host::ClientStats s = p.client.stats();
			const host::GameServerStats gs = server.stats();
			CHECK(!s.refused);
			CHECK(server.spawned_clients() == 1);
			CHECK(s.gunplay == gunplay);
			CHECK(gs.gunplay_active == gunplay);
			CHECK(s.weapon_hash_mismatch == 0);
			CHECK(p.frame->own_weapons_valid == gunplay);
			std::printf("E6 matched (%s): spawned on the server %zu, gunplay %d, weapon_hash 0x%016" PRIx64 ", kit_hash 0x%016" PRIx64 "\n",
					gunplay ? "gunplay server" : "M1 server", server.spawned_clients(), int(s.gunplay), gs.weapon_hash, gs.kit_hash);
		}
	}

	TEST_CASE("host.gunplay.client: R2: a server hit (health, the tick's damage, kDamaged) is a health rebase, not a reconciliation") {

		Server server;
		std::string err;
		ServerOptions so;
		so.exact_aim = true;
		REQUIRE_MESSAGE(server.start(so, err), err);
		Player p;
		REQUIRE(p.start(server.port(), "off", 5));
		REQUIRE(p.wait_spawned());
		const uint16_t face = sim::quantize_yaw(0.0f);
		p.adopt_yaw = 0.0f;
		p.input.yaw = 0.0f;
		REQUIRE(p.teleport({ 0.0f, 0.05f, 0.0f }, face).has_value());
		const uint8_t victim = p.client.stats().slot;
		struct Shooter {
			std::atomic<int> placed{ 0 };
			std::atomic<uint32_t> wanted{ 0 }, fired{ 0 };
			double last = -10.0;
		};
		auto sh = std::make_shared<Shooter>();
		Bot shooter;
		REQUIRE(shooter.start(server.port(), 2, 6, [sh, victim](Player &r_p, double p_t) {
			if (sh->placed.load() == 0) {

				sh->placed = r_p.teleport({ 0.0f, 0.05f, 5.0f }, sim::quantize_yaw(3.14159265f)).has_value() ? 1 : -1;
				r_p.input.yaw = 3.14159265f;
				return;
			}
			std::array<host::PresentedSoldier, 8> drawn;
			host::PresentOthersCounts counts;
			const size_t n = host::present_others(*r_p.frame, net::monotonic_ns(), drawn, counts);
			for (size_t i = 0; i < n; ++i) {
				if (drawn[i].slot != victim) {
					continue;
				}
				const sim::Vec3 eye = tg::own_eye(*r_p.frame);
				const float dx = drawn[i].position[0] - eye[0], dy = drawn[i].position[1] + 1.3f - eye[1], dz = drawn[i].position[2] - eye[2];
				r_p.input.yaw = std::atan2(dx, dz);
				r_p.input.pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
				if (sh->fired.load() < sh->wanted.load() && p_t - sh->last > 0.3) {
					++r_p.input.fire_presses;
					++sh->fired;
					sh->last = p_t;
				}
			}
		}));
		REQUIRE(shooter.wait_spawned());

		p.run_for(3.0);
		REQUIRE(sh->placed.load() == 1);

		const uint64_t reconciliations_before = p.client.stats().reconciliations;
		for (uint32_t k = 1; k <= 3; ++k) {
			sh->wanted = k;
			p.run_for(1.0);
		}
		for (const auto t0 = std::chrono::steady_clock::now(); tg::seconds_since(t0) < 6.0;) {
			if (p.client.stats().health_rebases >= 3) {
				break;
			}
			p.run_for(0.25);
		}
		p.run_for(0.5);
		const host::ClientStats s = p.client.stats();
		const host::GameServerStats gs = server.stats();
		shooter.stop();
		print_client("R2 victim", s);
		print_server("R2", gs);
		CHECK(gs.gunplay.hits >= 3);
		CHECK(gs.gunplay.deaths == 0);
		CHECK(s.health_rebases >= 3);
		CHECK(s.reconciliations == reconciliations_before);
		CHECK(s.weapon_reconciliations == 0);
		CHECK(s.combat_hits >= 3);
		CHECK(p.frame->own_curr.health < 100.0f);
		CHECK(p.count(host::PresentKind::Hit) >= 3);
		CHECK(std::all_of(p.events.begin(), p.events.end(), [](const host::PresentEvent &e) {
			return e.kind != host::PresentKind::Hit || (e.timeline == host::PresentTimeline::Immediate && (e.flags & host::present_flags::kOwnVictim) != 0);
		}));
		CHECK(s.remote_shots >= 3);
		CHECK(p.count(host::PresentKind::RemoteShot) == s.remote_shots);
		std::printf("R2: %u taps, server hits %" PRIu64 " (damage applied %" PRIu64 "), health_rebases %" PRIu64 ", health %.1f, reconciliations %" PRIu64
					" after the teleport (weapon %" PRIu64 "), Hit events %zu, remote shots %" PRIu64 "\n",
				sh->fired.load(), gs.gunplay.hits, server.log().damage_applied(), s.health_rebases, double(p.frame->own_curr.health), s.reconciliations - reconciliations_before,
				s.weapon_reconciliations, p.count(host::PresentKind::Hit), s.remote_shots);
	}

	TEST_CASE("host.gunplay.client: the SnapshotOthers merge and its acks (60 target bots: the others do not fit one Snapshot)") {
		Server server;
		std::string err;
		ServerOptions so;
		so.target_bots = 60;
		REQUIRE_MESSAGE(server.start(so, err), err);
		Bot firing;
		REQUIRE(firing.start(server.port(), 1, 8, firing_script(7)));
		Player p;
		REQUIRE(p.start(server.port(), "off", 7));
		REQUIRE(p.wait_spawned());
		REQUIRE(firing.wait_spawned());
		p.run_for(3.0);
		for (const auto t0 = std::chrono::steady_clock::now(); tg::seconds_since(t0) < 10.0;) {
			const host::ClientStats now = p.client.stats();
			if (now.snapshot_others_received > 50 && now.remote_shots > 0) {
				break;
			}
			p.run_for(0.25);
		}
		const host::ClientStats s = p.client.stats();
		const host::GameServerStats gs = server.stats();
		firing.stop();
		print_client("SnapshotOthers", s);
		print_server("SnapshotOthers", gs);
		CHECK(s.snapshot_others_received > 50);
		CHECK(p.frame->other_count == 61);
		CHECK(gs.gunplay.lag.acks_ignored == 0);
		CHECK(s.reconciliations == 0);
		CHECK(s.weapon_reconciliations == 0);
		CHECK(s.remote_shots > 0);
		CHECK(p.count(host::PresentKind::RemoteShot) == s.remote_shots);
		CHECK(s.projectiles.remote_spawned > 0);
		std::printf("SnapshotOthers: %" PRIu64 " received, others drawn %u (60 targets + 1 client), server others_dropped %" PRIu64 ", acks_ignored %" PRIu64
					", remote shots %" PRIu64 ", remote impacts %" PRIu64 "\n",
				s.snapshot_others_received, p.frame->other_count, gs.gunplay.others_dropped, gs.gunplay.lag.acks_ignored, s.remote_shots,
				s.projectiles.remote_impacts);
	}
}

TEST_SUITE("host-gunplay-client-unit") {
	TEST_CASE("host.gunplay.client: hostile gunplay datagrams are refused and counted; the session keeps predicting") {
		Server server;
		std::string err;
		ServerOptions so;
		so.target_bots = 60;
		REQUIRE_MESSAGE(server.start(so, err), err);
		Bot b1, b2;
		REQUIRE(b1.start(server.port(), 1, 10, firing_script(5)));
		REQUIRE(b2.start(server.port(), 1, 11, firing_script(35)));
		HostileRelay relay;
		REQUIRE_MESSAGE(relay.start(server.port(), tables().frame, err), err);
		Player p;
		REQUIRE(p.start(relay.port(), "off", 9));
		REQUIRE(p.wait_spawned());
		p.run_for(5.0, [&](double p_t) {
			p.input.held_buttons = std::fmod(p_t, 1.0) < 0.3 ? sim::buttons::kFire : 0u;
			p.input.yaw = float(p_t * 0.5);
		});
		const host::ClientStats s = p.client.stats();
		b1.stop();
		b2.stop();
		print_client("hostile", s);
		CHECK(s.connected);
		CHECK(!s.refused);
		CHECK(p.frame->valid);
		CHECK(relay.hostile_sent() > 100);
		CHECK(s.bad_gunplay_datagrams > 0);

		net::ShotEvents probe;
		probe.server_tick = 600;
		probe.shots.push_back(hostile_shot(600, { 0.0f, 1.6f, 0.0f }));
		const sim::Vec3 eye0{ 0.0f, 1.6f, 0.0f };
		const core::Result<net::ShotEvents> decoded = net::decode_shot_events(net::encode_shot_events(probe, tables().frame, eye0), tables().frame);
		REQUIRE(decoded.ok());
		REQUIRE(decoded->shots.size() == 1);
		CHECK(decoded->shots[0].def >= tables().table->defs.size());
		std::printf("hostile probe: def %u decodes (table %zu defs), slot %u\n", unsigned(decoded->shots[0].def), tables().table->defs.size(),
				unsigned(decoded->shots[0].slot));
		CHECK(s.bad_gunplay_events > 0);
		CHECK(std::none_of(p.events.begin(), p.events.end(), [&](const host::PresentEvent &e) {
			return e.slot >= net::kMaxSlots || e.aux >= net::kMaxSlots || (e.kind == host::PresentKind::RemoteShot && e.def >= tables().table->defs.size());
		}));
		CHECK(s.bad_weapon_wires > 0);
		CHECK(s.step_precondition_failures == 0);
		CHECK(s.lane.gaps == 0);
		CHECK(s.predicted_shots > 0);
		std::printf("hostile: %" PRIu64 " hostile datagrams sent by the relay (%" PRIu64 " forwarded); refused to decode %" PRIu64 ", out-of-range events dropped %" PRIu64
					", own weapon wires refused %" PRIu64 ", still predicting (%" PRIu64 " own shots, %" PRIu64 " weapon reconciliations)\n",
				relay.hostile_sent(), relay.forwarded(), s.bad_gunplay_datagrams, s.bad_gunplay_events, s.bad_weapon_wires, s.predicted_shots,
				s.weapon_reconciliations);
	}
}

namespace {

struct Arena {
	const char *name = "";
	std::shared_ptr<const host::SharedLevel> level;
};

Arena floor_arena() {
	return { "synthetic floor", gunplay_floor() };
}

std::optional<Arena> metro_arena() {
	tg::Metro *m = tg::metro_gunplay();
	if (m == nullptr) {
		return std::nullopt;
	}
	return Arena{ "Metro", m->level };
}

using bf3::core::to_lower_ascii;

void run_fire_reload(const Arena &p_arena) {
	Server server;
	std::string err;
	ServerOptions so;
	so.level = p_arena.level;
	REQUIRE_MESSAGE(server.start(so, err), err);
	Player p;
	REQUIRE(p.start(server.port(), "off", 21, {}, 1, p_arena.level));
	REQUIRE(p.wait_spawned());
	p.run_for(1.5);

	p.input.held_buttons = sim::buttons::kFire;
	p.run_for(3.0);
	p.input.held_buttons = 0;
	p.run_for(0.5);

	p.input.held_buttons = sim::buttons::kReload;
	p.run_for(0.1);
	p.input.held_buttons = 0;
	p.run_for(3.0);
	p.input.held_buttons = sim::buttons::kFire;
	p.run_for(0.36);
	p.input.held_buttons = 0;
	p.run_for(0.3);
	p.input.held_buttons = sim::buttons::kReload;
	p.run_for(0.1);
	p.input.held_buttons = 0;
	p.run_for(2.5);
	const host::ClientStats s = p.client.stats();
	std::vector<host::PredictedShot> shots;
	std::vector<host::PredictedTimed> timed;
	p.client.take_predicted(shots, timed);
	const host::GameServerStats gs = server.stats();
	p.client.stop();
	server.stop();
	const std::string label = std::string("E1/E5 ") + p_arena.name;
	print_client(label.c_str(), s);
	print_server(label.c_str(), gs);
	std::vector<ServerShot> server_shots;
	for (const ServerShot &x : server.log().shots()) {
		if (x.shot.player_slot == s.slot) {
			server_shots.push_back(x);
		}
	}

	REQUIRE(shots.size() >= 31);
	REQUIRE(server_shots.size() >= 31);
	core::Result<std::shared_ptr<const host::ClientGunplayInputs>> in = host::client_gunplay_inputs(*p_arena.level);
	REQUIRE(in.ok());
	const std::string variant = shots[0].shot.def < (*in)->table->defs.size()
			? to_lower_ascii((*in)->table->defs[shots[0].shot.def].variant)
			: std::string("?");
	const bool m16 = variant.size() >= 7 && variant.compare(variant.size() - 7, 7, "u_m16a4") == 0;
	CHECK_MESSAGE(m16, variant);
	size_t equal = 0;
	std::string why;
	for (size_t i = 0; i < 31; ++i) {
		std::string w;
		if (same_shot(shots[i].shot, server_shots[i].shot, &w) && shots[i].tick == server_shots[i].tick) {
			++equal;
		} else if (why.empty()) {
			why = "shot " + std::to_string(i) + ": " + (w.empty() ? std::string("tick") : w);
		}
	}
	CHECK_MESSAGE(equal == 31, why);
	size_t gaps = 0;
	for (size_t i = 1; i < 31; ++i) {
		gaps += shots[i].shot.time - shots[i - 1].shot.time == 4'500'000 ? 1u : 0u;
	}
	CHECK(gaps == 30);
	CHECK(shots[0].shot.time == sw::tick_start(shots[0].tick));
	const size_t muzzle = p.count(host::PresentKind::OwnShot);
	CHECK(muzzle == shots.size());
	CHECK(shots.size() == server_shots.size());
	std::printf("E1 %s (%s): predicted %zu / server %zu shots; the first 31 equal field for field: %zu / 31; gaps of 4,500,000: %zu / 30; first at the press "
				"tick's start: %d; OwnShot events %zu\n",
			p_arena.name, variant.c_str(), shots.size(), server_shots.size(), equal, gaps, int(shots[0].shot.time == sw::tick_start(shots[0].tick)), muzzle);

	std::vector<ServerTimed> server_timed;
	for (const ServerTimed &x : server.log().timed()) {
		if (x.slot == s.slot) {
			server_timed.push_back(x);
		}
	}
	size_t same_timed = 0;
	for (size_t i = 0; i < std::min(timed.size(), server_timed.size()); ++i) {
		const sw::TimedWeaponEvent &a = timed[i].event, &b = server_timed[i].event;
		same_timed += a.time == b.time && a.event == b.event && a.weapon_slot == b.weapon_slot && timed[i].tick == server_timed[i].tick ? 1u : 0u;
	}
	CHECK(timed.size() == server_timed.size());
	CHECK(same_timed == timed.size());
	std::vector<sw::SimTime> starts, commits, finishes;
	for (const host::PredictedTimed &x : timed) {
		if (x.event.event == sw::weapon_events::kReloadStarted) {
			starts.push_back(x.event.time);
		} else if (x.event.event == sw::weapon_events::kReloadCommitted) {
			commits.push_back(x.event.time);
		} else if (x.event.event == sw::weapon_events::kReloadFinished) {
			finishes.push_back(x.event.time);
		}
	}
	REQUIRE(starts.size() >= 2);
	REQUIRE(finishes.size() >= 2);
	REQUIRE(commits.size() >= 2);
	CHECK(finishes[0] - starts[0] == 142'200'000);
	CHECK(finishes[1] - starts[1] == 108'000'000);
	CHECK(commits[1] - starts[1] == 91'800'000);

	int32_t after_empty = -1, after_tactical = -1;
	for (const ServerMagazine &m : server.log().magazines()) {
		if (m.slot != s.slot) {
			continue;
		}
		const sw::SimTime begin = sw::tick_start(m.tick);
		after_empty = begin <= finishes[0] ? m.magazine : after_empty;
		after_tactical = begin <= finishes[1] ? m.magazine : after_tactical;
	}
	CHECK(after_empty == 30);
	CHECK(after_tactical == 31);
	CHECK(s.weapon_reconciliations == 0);
	CHECK(s.reconciliations == 0);
	CHECK(gs.gunplay.weapon_precondition_failures == 0);
	std::printf("E5 %s: timed events predicted %zu / server %zu, equal %zu; empty reload %" PRId64 " -> %d rounds, tactical %" PRId64 " (commit +%" PRId64
				") -> %d rounds\n",
			p_arena.name, timed.size(), server_timed.size(), same_timed, int64_t(finishes[0] - starts[0]), after_empty, int64_t(finishes[1] - starts[1]),
			int64_t(commits[1] - starts[1]), after_tactical);
}

struct LagcompSite {
	sim::Vec3 shooter{};
	uint16_t shooter_yaw = 0;
	sim::Vec3 target{};
	uint16_t target_yaw = 0;
	uint32_t spawn_index = 0;
	float distance = 0;
	uint32_t candidates = 0;
};

std::optional<LagcompSite> find_lagcomp_site(const host::SharedLevel &p_level, float p_distance, float p_half) {
	core::Result<std::unique_ptr<sim::CollisionWorld>> made = sim::CollisionWorld::create(p_level.static_world);
	REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
	if (!made.ok()) {
		return std::nullopt;
	}
	const sim::CollisionWorld &w = **made;
	const float eye = p_level.gunplay != nullptr && !p_level.gunplay->soldier.poses.empty() ? p_level.gunplay->soldier.poses[0].eye_height : 1.6f;
	constexpr float kRadius = 0.35f, kHeight = 1.8f, kLift = 0.15f;
	auto add = [](const sim::Vec3 &p_a, const sim::Vec3 &p_b, float p_s) {
		return sim::Vec3{ p_a[0] + p_b[0] * p_s, p_a[1] + p_b[1] * p_s, p_a[2] + p_b[2] * p_s };
	};
	const sim::Vec3 up{ 0.0f, 1.0f, 0.0f };
	LagcompSite out;
	for (const sim::SpawnPoint &sp : p_level.spawns) {
		sim::StaticHit g;
		if (!w.ground_below(add(sp.position, up, 1.0f), 3.0f, sim::query_mask::kMovement, &g)) {
			continue;
		}
		const sim::Vec3 feet = g.position;
		if (w.overlap_capsule(add(feet, up, kLift), kRadius, kHeight - kLift, sim::query_mask::kMovement, 0.01f)) {
			continue;
		}
		const sim::Vec3 eye_at = add(feet, up, eye);
		for (uint32_t h = 0; h < 64; ++h) {
			++out.candidates;
			const uint16_t yaw = uint16_t(h * 1024u);
			const float yr = sim::yaw_radians(yaw);
			const sim::Vec3 dir{ std::sin(yr), 0.0f, std::cos(yr) };
			const sim::Vec3 across{ std::cos(yr), 0.0f, -std::sin(yr) };
			if (w.ray(eye_at, add({}, dir, p_distance + 3.0f), sim::query_mask::kProjectile)) {
				continue;
			}
			sim::StaticHit gc;
			if (!w.ground_below(add(add(feet, dir, p_distance), up, 1.5f), 4.0f, sim::query_mask::kMovement, &gc)) {
				continue;
			}
			const sim::Vec3 centre = gc.position;
			bool ok = std::fabs(centre[1] - feet[1]) <= 0.5f;
			for (float o = -p_half; ok && o <= p_half + 1e-3f; o += 0.5f) {
				const sim::Vec3 at = add(centre, across, o);
				sim::StaticHit gp;
				ok = w.ground_below(add(at, up, 1.0f), 2.0f, sim::query_mask::kMovement, &gp) && std::fabs(gp.position[1] - centre[1]) <= 0.1f;
				for (float hgt : { 0.3f, 1.0f, 1.6f }) {
					const sim::Vec3 p = add(at, up, hgt);
					ok = ok && !w.ray(eye_at, { p[0] - eye_at[0], p[1] - eye_at[1], p[2] - eye_at[2] }, sim::query_mask::kProjectile);
				}
			}
			ok = ok && !w.sweep_capsule(add(add(centre, across, -p_half), up, kLift), kRadius, kHeight - kLift, add({}, across, 2.0f * p_half), sim::query_mask::kMovement);
			if (!ok) {
				continue;
			}
			out.shooter = feet;
			out.shooter_yaw = yaw;
			out.target = centre;
			out.target_yaw = uint16_t(yaw + 32768u);
			out.spawn_index = sp.index;
			out.distance = p_distance;
			return out;
		}
	}
	return std::nullopt;
}

std::optional<LagcompSite> lagcomp_site(const Arena &p_arena) {
	for (float d : { 20.0f, 16.0f, 12.0f }) {
		const std::optional<LagcompSite> s = find_lagcomp_site(*p_arena.level, d, 5.0f);
		if (s.has_value()) {
			std::printf("E12 site on %s: spawn %u, shooter feet (%.2f, %.2f, %.2f) yaw %u, strafe centre (%.2f, %.2f, %.2f) %.0f m ahead, +-5 m clear and in view\n",
					p_arena.name, s->spawn_index, double(s->shooter[0]), double(s->shooter[1]), double(s->shooter[2]), unsigned(s->shooter_yaw),
					double(s->target[0]), double(s->target[1]), double(s->target[2]), double(d));
			return s;
		}
		std::printf("E12 site on %s: no %.0f m site among %zu spawns x 64 headings\n", p_arena.name, double(d), p_arena.level->spawns.size());
	}
	CHECK_MESSAGE(false, "no E12 site on " << p_arena.name);
	return std::nullopt;
}

struct LagcompResult {
	size_t shots = 0;
	size_t client_hits = 0;
	size_t both = 0;
	size_t same_zone = 0;
	size_t compared = 0, equal = 0, mismatched = 0, client_only = 0;
	size_t server_rows = 0, server_only = 0;
	size_t server_hits = 0, server_hits_uncompared = 0;
	host::ClientStats stats;
	host::LagCompStats lag;
	uint64_t refused_views = 0;
	double strafe_speed = 0;
	uint64_t draws = 0, window_extrapolated = 0, drawn_extrapolated = 0, drawn_held = 0;
};

LagcompResult run_lagcomp(const Arena &p_arena, const LagcompSite &p_site, const char *p_label, const std::string &p_netsim, uint32_t p_server_rewind,
		size_t p_shots) {
	LagcompResult r;
	const std::string cfg = no_damage_cfg();
	Server server;
	std::string err;
	ServerOptions so;
	so.level = p_arena.level;
	so.mechanics_cfg = cfg;
	so.exact_aim = true;
	so.infinite_magazine = true;
	so.max_rewind_ticks = p_server_rewind;
	so.rewind_rows = true;
	REQUIRE_MESSAGE(server.start(so, err), err);

	auto strafe = std::make_shared<StrafeSpec>();
	Bot target;
	REQUIRE(target.start(server.port(), 2, 12, strafing_script(strafe), cfg, p_arena.level));
	REQUIRE(target.wait_spawned());
	const uint32_t bot = uint32_t(target.slot());
	const sim::Vec3 target_at{ p_site.target[0], p_site.target[1] + 0.05f, p_site.target[2] };
	strafe->set(target_at, sim::yaw_radians(p_site.target_yaw));
	REQUIRE(target.teleport(target_at, p_site.target_yaw).has_value());
	Player p;
	REQUIRE(p.start(
			server.port(), p_netsim, 11,
			[&](host::ClientSessionConfig &r_c) {
				r_c.own_test_max_rewind_ticks = int32_t(host::kDefaultMaxRewindTicks);
				r_c.mechanics_cfg = cfg;
			},
			1, p_arena.level));
	REQUIRE(p.wait_spawned());
	p.adopt_yaw = sim::yaw_radians(p_site.shooter_yaw);
	p.input.yaw = sim::yaw_radians(p_site.shooter_yaw);
	REQUIRE(p.teleport({ p_site.shooter[0], p_site.shooter[1] + 0.05f, p_site.shooter[2] }, p_site.shooter_yaw).has_value());
	sim::SoldierState b0;
	for (const auto t0 = std::chrono::steady_clock::now(); tg::seconds_since(t0) < 5.0;) {
		b0 = server.state_of(bot);
		const float dx = b0.position[0] - p_site.target[0], dz = b0.position[2] - p_site.target[2];
		if (std::sqrt(b0.velocity[0] * b0.velocity[0] + b0.velocity[2] * b0.velocity[2]) > 1.0f && std::sqrt(dx * dx + dz * dz) < 6.0f) {
			break;
		}
		p.run_for(0.05);
	}
	const float vl = std::sqrt(b0.velocity[0] * b0.velocity[0] + b0.velocity[2] * b0.velocity[2]);
	REQUIRE(vl > 1.0f);
	p.run_for(1.5);
	std::set<uint32_t> fired;
	std::map<uint32_t, uint8_t> client_hit;
	sim::Tick last_press = 0;
	double speed_sum = 0;
	size_t speed_n = 0;
	p.input.held_buttons = sim::buttons::kZoom;
	const auto start = std::chrono::steady_clock::now();
	while (fired.size() < p_shots && tg::seconds_since(start) < double(p_shots) / 4.0 + 60.0) {
		p.poll();
		const host::PresentationFrame &f = *p.frame;
		std::array<host::PresentedSoldier, 8> drawn;
		host::PresentOthersCounts counts;
		const size_t n = host::present_others(f, net::monotonic_ns(), drawn, counts);
		r.window_extrapolated += counts.window_extrapolated;
		r.drawn_extrapolated += counts.extrapolated;
		r.drawn_held += counts.held;
		++r.draws;
		for (size_t i = 0; i < n; ++i) {
			if (drawn[i].slot != bot || !f.valid) {
				continue;
			}
			const sim::Vec3 eye = tg::own_eye(f);
			const float dx = drawn[i].position[0] - eye[0], dy = drawn[i].position[1] + 1.3f - eye[1], dz = drawn[i].position[2] - eye[2];
			p.input.yaw = std::atan2(dx, dz);
			p.input.pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
			speed_sum += std::sqrt(drawn[i].velocity[0] * drawn[i].velocity[0] + drawn[i].velocity[2] * drawn[i].velocity[2]);
			++speed_n;
			if (f.tick >= last_press + 12u) {
				++p.input.fire_presses;
				last_press = f.tick;
			}
		}
		p.client.set_input(p.input);
		for (const host::PresentEvent &e : p.events) {
			if (e.kind == host::PresentKind::OwnShot) {
				fired.insert(e.id);
			} else if (e.kind == host::PresentKind::SoldierContact && e.timeline == host::PresentTimeline::Predicted && e.aux == bot) {
				client_hit.emplace(e.id, uint8_t(e.value));
			}
		}
		p.events.clear();
		std::this_thread::sleep_for(std::chrono::milliseconds(3));
	}
	p.run_for(2.0);
	for (const host::PresentEvent &e : p.events) {
		if (e.kind == host::PresentKind::SoldierContact && e.timeline == host::PresentTimeline::Predicted && e.aux == bot) {
			client_hit.emplace(e.id, uint8_t(e.value));
		}
	}
	r.stats = p.client.stats();
	std::vector<host::OwnTestSample> samples;
	p.client.take_own_test_samples(samples);
	const host::GameServerStats gs = server.stats();
	p.client.stop();
	target.stop();
	server.stop();
	r.lag = gs.gunplay.lag;
	r.refused_views = gs.gunplay.view_refused;
	const uint8_t own = r.stats.slot;
	std::map<uint32_t, uint8_t> server_hit;
	for (const ServerImpact &x : server.log().impacts()) {
		if (x.impact.player_slot == own && x.impact.target == bot) {
			server_hit.emplace(x.impact.shot_index, uint8_t(x.impact.zone));
		}
	}
	r.shots = fired.size();
	r.client_hits = client_hit.size();
	r.server_hits = server_hit.size();
	for (const auto &[shot, zone] : client_hit) {
		auto it = server_hit.find(shot);
		if (it != server_hit.end()) {
			++r.both;
			r.same_zone += it->second == zone ? 1u : 0u;
		}
	}

	std::map<std::tuple<uint32_t, uint8_t, int64_t>, net::OtherPose> server_rows;
	for (const host::RewindTraceRow &row : server.log().trace()) {
		if (row.shooter == own) {
			server_rows.emplace(std::make_tuple(row.shot_index, row.target, row.time_q8), row.pose);
		}
	}
	r.server_rows = server_rows.size();
	std::set<std::tuple<uint32_t, uint8_t, int64_t>> seen, matched_rows;
	std::set<uint32_t> compared_shots;
	for (const host::OwnTestSample &s : samples) {
		const auto key = std::make_tuple(s.shot_index, s.target, s.time_q8);
		if (!seen.insert(key).second) {
			continue;
		}
		auto it = server_rows.find(key);
		if (it == server_rows.end()) {
			++r.client_only;
			continue;
		}
		++r.compared;
		matched_rows.insert(key);
		if (s.target == bot) {
			compared_shots.insert(s.shot_index);
		}
		if (net::same_bits(it->second, s.pose)) {
			++r.equal;
		} else {
			++r.mismatched;
		}
	}
	r.server_only = r.server_rows - matched_rows.size();
	for (const auto &[shot, zone] : server_hit) {
		(void)zone;
		r.server_hits_uncompared += compared_shots.count(shot) == 0 ? 1u : 0u;
	}
	r.strafe_speed = speed_n > 0 ? speed_sum / double(speed_n) : 0;
	std::printf("E12 %s on %s (%s, server max_rewind %u): %zu shots, client test hit %zu, server hit %zu of them (%.2f %%), same zone %zu (%.2f %%); "
				"(b) samples compared %zu, equal %zu, mismatched %zu (late_sample %" PRIu64 "), client-only %zu (the server's broad phase culled or no row), "
				"server rows %zu of which unmatched %zu, server-hit shots %zu of which without a compared sample %zu; "
				"server verdicts present %" PRIu64 " rewound %" PRIu64 " clamped %" PRIu64 " refused %" PRIu64 "; drawn strafe speed %.2f m/s\n",
			p_label, p_arena.name, p_netsim.c_str(), p_server_rewind, r.shots, r.client_hits, r.both,
			r.client_hits ? 100.0 * double(r.both) / double(r.client_hits) : 0.0, r.same_zone, r.both ? 100.0 * double(r.same_zone) / double(r.both) : 0.0,
			r.compared, r.equal, r.mismatched, r.stats.late_sample, r.client_only, r.server_rows, r.server_only, r.server_hits, r.server_hits_uncompared,
			r.lag.present, r.lag.rewound, r.lag.clamped, r.lag.refused, r.strafe_speed);
	print_client(p_label, r.stats);
	print_server(p_label, gs);
	std::printf("E12 %s residuals: generation_reorder %" PRIu64 ", others_ahead %" PRIu64 ", track_timeout %" PRIu64 ", window_extrapolated %" PRIu64
				" of %" PRIu64 " draws (extrapolated %" PRIu64 ", held %" PRIu64 "), refused views %" PRIu64 ", observer dropped %" PRIu64 "\n",
			p_label, r.stats.generation_reorder, r.stats.others_ahead, r.stats.track_timeout, r.window_extrapolated, r.draws, r.drawn_extrapolated,
			r.drawn_held, r.refused_views, server.log().dropped());
	return r;
}

void check_lagcomp_gate(const LagcompResult &p_gate, size_t p_shots, size_t p_min_hits) {
	CHECK(p_gate.shots >= p_shots);
	CHECK(p_gate.client_hits >= p_min_hits);
	CHECK(double(p_gate.both) >= 0.99 * double(p_gate.client_hits));
	CHECK(double(p_gate.same_zone) >= 0.99 * double(p_gate.both));
	CHECK(p_gate.compared > 0);
	CHECK(p_gate.mismatched <= p_gate.stats.late_sample);
	CHECK(p_gate.server_hits_uncompared <= p_gate.lag.clamped);
}

void run_loopback(const Arena &p_arena, double p_seconds) {
	Server server;
	std::string err;
	ServerOptions so;
	so.level = p_arena.level;
	REQUIRE_MESSAGE(server.start(so, err), err);
	Bot strafer, firing;
	REQUIRE(strafer.start(server.port(), 1, 32, strafing_script(), {}, p_arena.level));
	REQUIRE(firing.start(server.port(), 1, 33, firing_script(13), {}, p_arena.level));
	Player p;
	REQUIRE(p.start(server.port(), "off", 31, {}, 1, p_arena.level));
	REQUIRE(p.wait_spawned());
	sim::Vec3 lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
	p.run_for(p_seconds, [&](double p_t) {
		const double t = std::fmod(p_t, 12.0);
		uint32_t b = 0;
		p.input.move_forward = t < 6.0 ? 1.0f : (t < 8.0 ? -0.5f : 0.0f);
		p.input.move_right = std::sin(p_t * 0.7) > 0.3 ? 1.0f : 0.0f;
		p.input.yaw = float(std::fmod(p_t * 0.4, 6.283185307179586));
		p.input.pitch = float(0.2 * std::sin(p_t));
		if (t > 1.0 && t < 2.0) {
			b |= sim::buttons::kFire;
		}
		if (t > 2.5 && t < 3.5) {
			b |= sim::buttons::kZoom;
			if (std::fmod(t, 0.25) < 0.02) {
				++p.input.fire_presses;
			}
		}
		if (t > 4.0 && t < 4.1) {
			b |= sim::buttons::kReload;
		}
		if (t > 7.0 && t < 7.05) {
			b |= uint32_t(2) << sw::weapon_buttons::kSlotShift;
		}
		if (t > 8.0 && t < 8.6) {
			b |= sim::buttons::kFire;
		}
		if (t > 9.5 && t < 9.55) {
			b |= uint32_t(1) << sw::weapon_buttons::kSlotShift;
		}
		if (t > 10.0 && t < 10.05) {
			++p.input.jump_presses;
		}
		if (t > 10.5 && t < 11.0) {
			b |= sim::buttons::kSprint;
		}
		p.input.pose_request = host::PoseRequest::Set;
		p.input.pose = t > 5.0 && t < 6.0 ? sim::Pose::Crouch : sim::Pose::Stand;
		p.input.held_buttons = b;
		if (p.frame->valid) {
			for (size_t k = 0; k < 3; ++k) {
				lo[k] = std::min(lo[k], p.frame->own_curr.position[k]);
				hi[k] = std::max(hi[k], p.frame->own_curr.position[k]);
			}
		}
	});
	const host::ClientStats s = p.client.stats();
	const host::GameServerStats gs = server.stats();
	p.client.stop();
	strafer.stop();
	firing.stop();
	server.stop();
	const std::string label = std::string("E15 loopback ") + p_arena.name;
	print_client(label.c_str(), s);
	print_server(label.c_str(), gs);
	const double min_shots = p_seconds / 6.0;
	CHECK(s.weapon_reconciliations == 0);
	CHECK(s.reconciliations == 0);
	CHECK(s.weapon_ring_misses == 0);
	CHECK(s.step_precondition_failures == 0);
	CHECK(s.lane.gaps == 0);
	CHECK(s.events_overflow == 0);
	CHECK(double(s.predicted_shots) > min_shots);
	CHECK(double(s.remote_shots) > min_shots);
	CHECK(gs.gunplay.weapon_precondition_failures == 0);
	CHECK(gs.step_precondition_failures == 0);
	std::printf("E15 loopback %s: %.0f s, predicted shots %" PRIu64 ", server shots %" PRIu64 " (all shooters), remote shots seen %" PRIu64
				", motor reconciliations %" PRIu64 ", weapon reconciliations %" PRIu64 ", health rebases %" PRIu64 ", lane steps %" PRIu64 " replayed %" PRIu64
				"; the player's feet ranged x %.1f .. %.1f, y %.1f .. %.1f, z %.1f .. %.1f\n",
			p_arena.name, p_seconds, s.predicted_shots, gs.gunplay.shots, s.remote_shots, s.reconciliations, s.weapon_reconciliations, s.health_rebases,
			s.lane.steps, s.lane.replayed_ticks, double(lo[0]), double(hi[0]), double(lo[1]), double(hi[1]), double(lo[2]), double(hi[2]));
}

void run_two_clients(const Arena &p_arena, double p_before_s, double p_after_s, size_t p_min_remote) {
	Server server;
	std::string err;
	ServerOptions so;
	so.level = p_arena.level;
	REQUIRE_MESSAGE(server.start(so, err), err);
	Bot b1, b2;
	REQUIRE(b1.start(server.port(), 1, 43, firing_script(3), {}, p_arena.level));
	REQUIRE(b2.start(server.port(), 1, 44, firing_script(29), {}, p_arena.level));
	Player a, b;
	REQUIRE(a.start(server.port(), "off", 41, {}, 1, p_arena.level));
	REQUIRE(a.wait_spawned());
	REQUIRE(b1.wait_spawned());
	REQUIRE(b2.wait_spawned());
	const std::array<int, 2> bot_slots{ b1.slot(), b2.slot() };
	const auto t0 = std::chrono::steady_clock::now();
	auto script = [](Player &r_p, double p_t) {
		r_p.input.yaw = float(std::fmod(p_t * 0.3, 6.283185307179586));
		r_p.input.pitch = -0.2f;
		r_p.input.held_buttons = std::fmod(p_t, 3.0) < 0.5 ? sim::buttons::kFire : 0u;
	};
	a.run_for(p_before_s, [&](double p_t) { script(a, p_t); });
	std::set<uint8_t> fired_before;
	for (const host::PresentEvent &e : a.events) {
		if (e.kind == host::PresentKind::RemoteShot) {
			fired_before.insert(e.slot);
		}
	}
	for (int slot : bot_slots) {
		CHECK_MESSAGE(fired_before.count(uint8_t(slot)) == 1, "bot slot " << slot << " fired before the join");
	}
	REQUIRE(b.start(server.port(), "off", 42, {}, 1, p_arena.level));
	const auto join = std::chrono::steady_clock::now();
	for (;;) {
		const double t = tg::seconds_since(join);
		if (t > p_after_s) {
			break;
		}
		script(a, tg::seconds_since(t0));
		script(b, t + 1.5);
		a.poll();
		b.poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}

	a.input.held_buttons = 0;
	b.input.held_buttons = 0;
	b1.stop();
	b2.stop();
	for (const auto tail = std::chrono::steady_clock::now(); tg::seconds_since(tail) < 1.5;) {
		a.poll();
		b.poll();
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}
	const host::ClientStats sa = a.client.stats(), sb = b.client.stats();
	a.client.stop();
	b.client.stop();
	server.stop();
	const std::string la = std::string("E15 client A ") + p_arena.name, lb = std::string("E15 client B ") + p_arena.name;
	print_client(la.c_str(), sa);
	print_client(lb.c_str(), sb);

	std::map<std::pair<uint8_t, uint32_t>, const host::PresentEvent *> at_a;
	for (const host::PresentEvent &e : a.events) {
		if (e.kind == host::PresentKind::RemoteShot || e.kind == host::PresentKind::OwnShot) {
			at_a[{ e.slot, e.id }] = &e;
		}
	}
	size_t remote_b = 0, matched = 0, same_seed = 0, near = 0;
	for (const host::PresentEvent &e : b.events) {
		if (e.kind != host::PresentKind::RemoteShot) {
			continue;
		}
		++remote_b;
		auto it = at_a.find({ e.slot, e.id });
		if (it == at_a.end()) {
			continue;
		}
		++matched;
		same_seed += it->second->seed == e.seed ? 1u : 0u;
		const float dx = it->second->pos[0] - e.pos[0], dy = it->second->pos[1] - e.pos[1], dz = it->second->pos[2] - e.pos[2];
		near += std::sqrt(dx * dx + dy * dy + dz * dz) < 0.01f ? 1u : 0u;
	}
	CHECK(remote_b > p_min_remote);
	CHECK(matched == remote_b);
	CHECK(same_seed == matched);
	std::printf("E15 two clients %s: B saw %zu remote shots after its join, A saw %zu of them, same effect seed %zu, origin within 1 cm %zu; bots fired before "
				"the join: %zu / %zu\n",
			p_arena.name, remote_b, matched, same_seed, near, fired_before.size(), bot_slots.size());
}

}

TEST_SUITE("host-gunplay-client-unit") {
	TEST_CASE("host.gunplay.client: synthetic floor variant: E1 + E5 (31 predicted shots = the server's; reload phase events server = predicted)") {
		run_fire_reload(floor_arena());
	}

	TEST_CASE("host.gunplay.client: synthetic floor variant: E12 lagcomp gate run (150/2, 60 shots against a strafing target)") {
		const Arena a = floor_arena();
		const std::optional<LagcompSite> site = lagcomp_site(a);
		if (!site) {
			return;
		}
		const LagcompResult gate = run_lagcomp(a, *site, "floor gate", "150/2", host::kDefaultMaxRewindTicks, 60);
		check_lagcomp_gate(gate, 60, 12);
	}

	TEST_CASE("host.gunplay.client: synthetic floor variant: E15 loopback (24 s) and two clients (5 s + 10 s)") {
		run_loopback(floor_arena(), 24.0);
		run_two_clients(floor_arena(), 5.0, 10.0, 20);
	}
}

TEST_SUITE("host-gunplay-client") {
	TEST_CASE("host.gunplay.client: E1 + E5 on Metro: 31 predicted shots equal the server's field for field; reload phase events server = predicted") {
		if (!timing_enabled("E1/E5")) {
			return;
		}
		const std::optional<Arena> metro = metro_arena();
		if (!metro) {
			return;
		}
		run_fire_reload(*metro);
	}

	TEST_CASE("host.gunplay.client: E12 lagcomp on Metro: 150/2 against a strafing target, control (server max_rewind 0) and stress (lat=150-165,loss=2)") {
		if (!timing_enabled("E12 lagcomp")) {
			return;
		}
		const std::optional<Arena> metro = metro_arena();
		if (!metro) {
			return;
		}
		const std::optional<LagcompSite> found = lagcomp_site(*metro);
		if (!found) {
			return;
		}
		const LagcompSite &site = *found;
		const LagcompResult gate = run_lagcomp(*metro, site, "gate", "150/2", host::kDefaultMaxRewindTicks, 500);
		check_lagcomp_gate(gate, 500, 100);
		const LagcompResult control = run_lagcomp(*metro, site, "control", "150/2", 0, 500);
		CHECK(double(control.both) <= 0.5 * double(control.client_hits));
		const LagcompResult stress = run_lagcomp(*metro, site, "stress", "lat=150-165,loss=2", host::kDefaultMaxRewindTicks, 500);
		std::printf("E12 summary (Metro): gate %.2f %% (zone %.2f %%), control %.2f %%, stress %.2f %% (reported); (b) gate %zu / %zu equal, %zu mismatched, "
					"late_sample %" PRIu64 ", server-hit shots without a compared sample %zu\n",
				gate.client_hits ? 100.0 * double(gate.both) / double(gate.client_hits) : 0.0, gate.both ? 100.0 * double(gate.same_zone) / double(gate.both) : 0.0,
				control.client_hits ? 100.0 * double(control.both) / double(control.client_hits) : 0.0,
				stress.client_hits ? 100.0 * double(stress.both) / double(stress.client_hits) : 0.0, gate.equal, gate.compared, gate.mismatched,
				gate.stats.late_sample, gate.server_hits_uncompared);
	}

	TEST_CASE("host.gunplay.client: E15 loopback on Metro: scripted play without netsim, 0 weapon and 0 motor reconciliations") {
		if (!timing_enabled("E15 loopback")) {
			return;
		}
		const std::optional<Arena> metro = metro_arena();
		if (!metro) {
			return;
		}
		run_loopback(*metro, env_seconds("OPENBF3_CL_LOOPBACK_S", 600.0));
	}

	TEST_CASE("host.gunplay.client: E15 two clients on Metro: the second joins after 30 s; both derive the same effect seed for every remote shot") {
		if (!timing_enabled("E15 two clients")) {
			return;
		}
		const std::optional<Arena> metro = metro_arena();
		if (!metro) {
			return;
		}
		run_two_clients(*metro, 30.0, 30.0, 100);
	}

	TEST_CASE("host.gunplay.client: install unchanged") {
		if (!timing_enabled("install unchanged")) {
			return;
		}
		tg::Metro *m = tg::metro_gunplay();
		if (m == nullptr) {
			return;
		}
		tg::check_install_unchanged(*m);
	}
}

#include "bf3/host/target_bots.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/weapons/spread_recoil.h"

namespace {

class GateImpactLog final : public host::GunplayObserver {
public:
	void projectiles(uint32_t p_tick, const sw::ProjectileStepOutput &p_out, std::span<const sw::Projectile> p_live) override {
		(void)p_tick, (void)p_live;
		std::lock_guard lock(mutex_);
		for (const sw::ImpactEvent &e : p_out.impacts) {
			if (impacts_.size() < 65536) {
				impacts_.push_back(e);
			}
		}
	}
	std::vector<sw::ImpactEvent> impacts() const {
		std::lock_guard lock(mutex_);
		return impacts_;
	}

private:
	mutable std::mutex mutex_;
	std::vector<sw::ImpactEvent> impacts_;
};

void run_e2_client_flight(const Arena &p_arena) {
	auto log = std::make_shared<GateImpactLog>();
	auto game = std::make_shared<host::GameServerConfig>();
	game->level = p_arena.level;
	game->max_slots = 64;
	game->dev_commands = true;
	game->seed = 1;
	game->gunplay.mechanics_cfg = mechanics_cfg_path();
	game->gunplay.observer = log;
	host::ServerConfig sc;
	sc.game = game;
	host::ServerHost server;
	std::string err;
	REQUIRE_MESSAGE(server.start(sc, &err), err);
	core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(p_arena.level->static_world);
	REQUIRE(world.ok());
	core::Result<host::SkyLane> lane = host::find_sky_lane(**world, 975.0f, sw::BallisticsLimits{}.world_margin);
	REQUIRE_MESSAGE(lane.ok(), (lane.ok() ? std::string() : lane.error().message));

	int16_t best_pitch = 0;
	float best_distance = -1.0f;
	for (int k = 4; k <= 240; ++k) {
		const int16_t q = sim::quantize_pitch(-float(k) * 0.25f * 3.14159265f / 180.0f);
		const sim::Vec3 d = sw::aim_direction(lane->yaw, q, {});
		const sim::Vec3 delta{ d[0] * 600.0f, d[1] * 600.0f, d[2] * 600.0f };
		sim::StaticHit hit;
		if (!(*world)->ray(lane->eye, delta, sim::query_mask::kProjectile, &hit)) {
			continue;
		}
		const float h = std::hypot(hit.position[0] - lane->eye[0], hit.position[2] - lane->eye[2]);
		if (best_distance < 0.0f || std::fabs(h - 340.0f) < std::fabs(best_distance - 340.0f)) {
			best_distance = h;
			best_pitch = q;
		}
	}
	std::printf("E2 client flight: sky lane eye (%.1f, %.1f, %.1f) yaw %u; aim pitch %.2f deg meets the world %.1f m out (ray)\n", double(lane->eye[0]),
			double(lane->eye[1]), double(lane->eye[2]), unsigned(lane->yaw), double(sim::pitch_radians(best_pitch)) * 180.0 / 3.14159265358979, double(best_distance));
	REQUIRE(best_distance >= 300.0f);
	REQUIRE(best_distance <= 400.0f);
	Player p;
	REQUIRE(p.start(server.port(), "off", 41, {}, 1, p_arena.level));
	REQUIRE(p.wait_spawned());
	const uint8_t own = uint8_t(p.client.stats().slot);
	constexpr int kShots = 8;
	for (int k = 0; k < kShots; ++k) {

		const auto start = std::chrono::steady_clock::now();
		while (tg::seconds_since(start) < 15.0 && !(p.frame->valid && p.frame->own_curr.mode != sim::MoveMode::Dead && p.input.life == p.frame->own_curr.life)) {
			p.poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
		p.run_for(1.2);
		REQUIRE(p.teleport(lane->feet, lane->yaw).has_value());
		p.input.yaw = sim::yaw_radians(lane->yaw);
		p.input.pitch = sim::pitch_radians(best_pitch);
		p.run_for(0.05);
		p.input.held_buttons = sim::buttons::kFire;
		p.run_for(0.04);
		p.input.held_buttons = 0;
		p.run_for(2.0);
	}
	std::vector<host::PredictedShot> shots;
	std::vector<host::PredictedTimed> timed;
	p.client.take_predicted(shots, timed);
	p.client.stop();
	server.stop();
	const std::vector<sw::ImpactEvent> server_impacts = log->impacts();
	size_t compared = 0, within = 0, past_300 = 0;
	double worst = 0.0;
	for (const host::PredictedShot &s : shots) {
		const host::PresentEvent *c = nullptr;
		for (const host::PresentEvent &e : p.events) {
			if (e.kind == host::PresentKind::Impact && e.timeline == host::PresentTimeline::Predicted && e.slot == own && e.id == s.shot.shot_index && e.pellet == 0) {
				c = &e;
				break;
			}
		}
		const sw::ImpactEvent *v = nullptr;
		for (const sw::ImpactEvent &e : server_impacts) {
			if (e.player_slot == own && e.shot_index == s.shot.shot_index && e.pellet == 0 && e.kind == sw::ImpactKind::Static) {
				v = &e;
				break;
			}
		}
		if (c == nullptr || v == nullptr) {
			std::printf("E2 client flight: shot %u: client impact %s, server impact %s (origin %.1f %.1f %.1f, direction pitch %.2f deg)\n", unsigned(s.shot.shot_index),
					c ? "yes" : "no", v ? "yes" : "no", double(s.shot.origin[0]), double(s.shot.origin[1]), double(s.shot.origin[2]),
					std::asin(double(s.shot.direction[1])) * 180.0 / 3.14159265358979);
			continue;
		}
		const double dx = double(c->pos[0]) - double(v->position[0]), dy = double(c->pos[1]) - double(v->position[1]), dz = double(c->pos[2]) - double(v->position[2]);
		const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
		const double h = std::hypot(double(v->position[0]) - double(s.shot.origin[0]), double(v->position[2]) - double(s.shot.origin[2]));
		++compared;
		within += d <= 1e-3 ? 1u : 0u;
		worst = std::max(worst, d);
		std::printf("E2 client flight: shot %u: origin (%.1f, %.1f, %.1f), direction pitch %.2f deg, impact %.2f m out (horizontal), client - server %.4f mm\n",
				unsigned(s.shot.shot_index), double(s.shot.origin[0]), double(s.shot.origin[1]), double(s.shot.origin[2]),
				std::asin(double(s.shot.direction[1])) * 180.0 / 3.14159265358979, h, d * 1000.0);
		past_300 += h >= 300.0 ? 1u : 0u;
	}
	std::printf("E2 client flight (%s): %zu predicted shots, %zu with both impacts compared (%zu of them 300 m or more out), %zu within 1 mm, worst %.4f mm\n",
			p_arena.name, shots.size(), compared, past_300, within, worst * 1000.0);
	CHECK(past_300 >= 3);
	CHECK(within == compared);
}

}

TEST_SUITE("host-gunplay-client") {
	TEST_CASE("host.gunplay.client: E2 client flight on Metro: the cosmetic flight of an own shot ends within 1 mm of the server's about 300 m out") {
		if (!timing_enabled("E2 client flight")) {
			return;
		}
		const std::optional<Arena> metro = metro_arena();
		if (!metro) {
			return;
		}
		run_e2_client_flight(*metro);
	}
}
