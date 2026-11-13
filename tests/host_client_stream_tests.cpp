#include <doctest/doctest.h>

#include "bf3/core/math.h"
#include "bf3/core/util/elapsed.h"
#include "bf3/core/util/percentile.h"
#include "bf3/host/bot_fill.h"
#include "bf3/host/client_session.h"
#include "bf3/host/client_wiring.h"
#include "bf3/host/duel.h"
#include "bf3/host/gadgets.h"
#include "bf3/host/game_server.h"
#include "bf3/host/gunplay.h"
#include "bf3/host/kits.h"
#include "bf3/host/level_vehicles.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/net/bit_stream.h"
#include "bf3/net/protocol.h"
#include "bf3/net/weapons_wire.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_motor.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/vehicle_world.h"
#include "bf3/sim/vehicles/vehicle_input.h"
#include "client_loop.h"
#include "client_transport.h"
#include "vehicles/client_vehicle_prediction.h"
#include "vehicles/server_vehicles.h"

#include "gunplay_fixture.h"
#include "tdm_fixture.h"
#include "test_support.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

using namespace bf3;
namespace tg = bf3::test::gunplay;
namespace tt = bf3::test::tdm;

namespace {

using Fnv = tt::detail::Fnv;

struct StreamHashes {
	uint64_t sent = 0;
	uint64_t frames = 0;
	uint64_t presented = 0;
	uint64_t events = 0;
	uint64_t gunplay = 0;
	uint64_t stats = 0;
};

constexpr StreamHashes kAlpha{ 0x21aecf2f023ed95bull, 0x238e7e65144a0de2ull, 0x3b1e751b77dc6e2eull,
	0xd9395164dc77a2eeull, 0x6279226fdb1eae88ull, 0xdd57c4f9576fabaaull };
constexpr StreamHashes kBravo{ 0x289f213a7d72fb31ull, 0xf80907c45d1ebc51ull, 0x1e457c5464ef5348ull,
	0xd8b18eb5fb459f28ull, 0x5f11829c247d996cull, 0x935e3bfa4c6f705full };
constexpr StreamHashes kCharlie{ 0x431ccb3188366fe6ull, 0x0d042d562bfe5336ull, 0x0d042d562bfe5336ull,
	0x0d042d562bfe5336ull, 0x0d042d562bfe5336ull, 0xd4bbb61a31c1220aull };
constexpr StreamHashes kDelta{ 0xb810487ee4eb8be5ull, 0x40ab0e0b2d925e99ull, 0xf272ee659e051d24ull,
	0x9d73df418acd8dacull, 0x1a19ab8ca4e37d79ull, 0xfeb81668a201e17aull };

constexpr uint32_t kTdmTicks = 1500;
constexpr uint32_t kDevTicks = 600;
constexpr uint64_t kStreamGameBuild = 0x0B3F0B3F0B3F0B3Full;
constexpr uint32_t kStepsPerTick = 8;
constexpr uint64_t kStepNs = 2'083'333;

void add3(Fnv &r_h, const sim::Vec3 &p_v) {
	r_h.add(p_v[0]);
	r_h.add(p_v[1]);
	r_h.add(p_v[2]);
}

void add_command(Fnv &r_h, const sim::Command &p_c) {
	r_h.add(p_c.tick);
	r_h.add(p_c.sequence);
	r_h.add(p_c.buttons);
	r_h.add(p_c.move_forward);
	r_h.add(p_c.move_right);
	r_h.add(p_c.yaw);
	r_h.add(p_c.pitch);
	r_h.add(p_c.pose);
	r_h.add(p_c.flags);
	r_h.add(p_c.view_back_q8);
	r_h.add(p_c.view_quality);
	r_h.add(p_c.view_seq8);
}

void add_other(Fnv &r_h, const net::OtherSoldier &p_o) {
	r_h.add(p_o.slot);
	r_h.add(p_o.generation);
	add3(r_h, p_o.position);
	add3(r_h, p_o.velocity);
	r_h.add(p_o.yaw);
	r_h.add(p_o.pitch);
	r_h.add(p_o.pose);
	r_h.add(p_o.target_pose);
	r_h.add(p_o.transition);
	r_h.add(p_o.mode);
	r_h.add(p_o.flags);
	r_h.add(p_o.weapon);
	r_h.add(p_o.weapon_phase);
	r_h.add(p_o.firing);
	r_h.add(p_o.ads);
	r_h.add(p_o.reload_seq);
	r_h.add(p_o.burst_seq);
	r_h.add(p_o.team);
}

void add_wire(Fnv &r_h, const net::WeaponsWire &p_wire) {
	net::BitWriter w(4096);
	net::write_weapons_wire(w, p_wire);
	r_h.add(uint64_t(w.bit_count()));
	r_h.bytes(w.bytes().data(), w.bytes().size());
}

void add_match(Fnv &r_h, const net::MatchState &p_m) {
	r_h.add(p_m.phase);
	r_h.add(p_m.round_start_tick);
	r_h.add(p_m.countdown_end_tick);
	r_h.add(p_m.kills[0]);
	r_h.add(p_m.kills[1]);
	r_h.add(p_m.kill_limit);
	r_h.add(p_m.winner);
	r_h.add(p_m.killer_camera);
	r_h.add(p_m.round_end_tick);
	r_h.add(p_m.unspawn_tick);
	r_h.add(p_m.restart_tick);
	r_h.add(p_m.restrict_until_tick);
	r_h.add(p_m.restrict_mask);
	r_h.add(p_m.players_needed);
}

void add_row(Fnv &r_h, const net::PlayerRow &p_r) {
	r_h.add(p_r.slot);
	r_h.add(p_r.team);
	r_h.add(p_r.kit);
	r_h.add(p_r.bot);
	r_h.add(p_r.alive);
	r_h.add(p_r.name_bytes);
	r_h.add(p_r.ping_ms);
	r_h.add(p_r.kills);
	r_h.add(p_r.deaths);
	r_h.add(p_r.score);
	r_h.bytes(p_r.name.data(), p_r.name.size());
	r_h.add(p_r.squad);
	r_h.add(p_r.leader);
}

void add_netsim(Fnv &r_h, const net::NetSimStats &p_s) {
	r_h.add(p_s.submitted);
	r_h.add(p_s.delivered);
	r_h.add(p_s.dropped);
	r_h.add(p_s.spike_dropped);
	r_h.add(p_s.duplicated);
	r_h.add(p_s.reordered);
	r_h.add(p_s.mean_delay_ms);
}

void add_stats(Fnv &r_h, const host::ClientStats &p_s) {
	r_h.add(p_s.connected);
	r_h.add(p_s.spawned);
	r_h.add(p_s.slot);
	r_h.add(p_s.client_tick);
	r_h.add(p_s.last_server_tick);
	r_h.add(p_s.rtt_ms);
	r_h.add(p_s.rtt_min_ms);
	r_h.add(p_s.enet_rtt_ms);
	r_h.add(p_s.command_buffer_ticks);
	r_h.add(p_s.target_buffer_ticks);
	r_h.add(p_s.clock_adjust);
	r_h.add(p_s.clock_resets);
	r_h.add(p_s.motor_hash_mismatch);
	r_h.add(p_s.snapshots_received);
	r_h.add(p_s.snapshots_lost);
	r_h.add(p_s.snapshots_late);
	r_h.add(p_s.commands_sent);
	r_h.add(p_s.reconciliations);
	r_h.add(p_s.last_reconcile_tick);
	r_h.add(p_s.last_reconcile_ns);
	r_h.add(p_s.ring_misses);
	r_h.add(p_s.spawns);
	r_h.add(p_s.replayed_ticks);
	r_h.add(p_s.base_fills);
	r_h.add(p_s.forced_forward);
	r_h.add(p_s.step_precondition_failures);
	r_h.add(p_s.corrections_over_1mm);
	r_h.add(p_s.max_correction_m);
	r_h.add(p_s.teleports);
	r_h.add(p_s.hash_mismatches);
	r_h.add(p_s.interp_delay_ms);
	add_netsim(r_h, p_s.netsim_out);
	add_netsim(r_h, p_s.netsim_in);
	r_h.bytes(p_s.last_error.data(), p_s.last_error.size());
	r_h.add(p_s.refused);
	r_h.add(p_s.gunplay);
	r_h.add(p_s.weapon_hash_mismatch);
	r_h.add(p_s.kit_hash_mismatch);
	r_h.add(p_s.weapon_reconciliations);
	r_h.add(p_s.weapon_ring_misses);
	r_h.add(p_s.health_rebases);
	r_h.add(p_s.predicted_shots);
	r_h.add(p_s.predicted_timed);
	r_h.add(p_s.no_view_shots);
	r_h.add(p_s.snapshot_others_received);
	r_h.add(p_s.remote_shots);
	r_h.add(p_s.combat_hits);
	r_h.add(p_s.combat_deaths);
	r_h.add(p_s.combat_detonations);
	r_h.add(p_s.bad_gunplay_datagrams);
	r_h.add(p_s.bad_weapon_wires);
	r_h.add(p_s.bad_gunplay_events);
	r_h.add(p_s.late_sample);
	r_h.add(p_s.generation_reorder);
	r_h.add(p_s.others_ahead);
	r_h.add(p_s.track_timeout);
	r_h.add(p_s.events_overflow);
	r_h.add(p_s.lane.steps);
	r_h.add(p_s.lane.emitted_ticks);
	r_h.add(p_s.lane.replayed_ticks);
	r_h.add(p_s.lane.dead_ticks);
	r_h.add(p_s.lane.gaps);
	const host::ClientProjectilesStats &p = p_s.projectiles;
	for (const uint64_t v : { p.own_spawned, p.remote_spawned, p.refused, p.own_soldier_contacts, p.own_impacts, p.remote_impacts, p.samples_logged,
				 p.samples_dropped, p.own_queries, p.own_no_view, p.remote_queries, p.remote_soldier_contacts, p.detonations_suppressed, p.remote_late,
				 p.events_refused }) {
		r_h.add(v);
	}
	r_h.add(p.live_own);
	r_h.add(p.live_remote);
	r_h.add(p_s.match_states);
	r_h.add(p_s.player_lists);
	r_h.add(p_s.revives);
	r_h.add(p_s.bad_tdm_datagrams);
	r_h.add(p_s.restricted_commands);
	r_h.add(p_s.unspawned_ticks);
	r_h.add(p_s.spawn_requests);
	r_h.add(p_s.own_shot_id_resets);
	r_h.add(p_s.rules_mismatch);
}

void add_frame(Fnv &r_h, const host::PresentationFrame &p_f) {
	r_h.add(p_f.serial);
	r_h.add(p_f.valid);
	r_h.add(p_f.tick);
	r_h.add(p_f.tick_fraction);
	r_h.add(p_f.publish_ns);
	r_h.add(p_f.own_ticks_per_ns);
	r_h.add(sim::soldier_state_hash(p_f.own_prev));
	r_h.add(sim::soldier_state_hash(p_f.own_curr));
	add_command(r_h, p_f.own_command);
	add3(r_h, p_f.visual_offset);
	r_h.add(p_f.eye_height_prev);
	r_h.add(p_f.eye_height_curr);
	r_h.add(p_f.server_own_valid);
	r_h.add(sim::soldier_state_hash(p_f.server_own));
	r_h.add(p_f.server_events);
	r_h.add(p_f.events);
	r_h.add(p_f.render_ticks_at_publish);
	r_h.add(p_f.ticks_per_ns);
	r_h.add(p_f.extrapolate_max_ms);
	for (const float e : p_f.pose_eye_heights) {
		r_h.add(e);
	}
	r_h.add(p_f.other_count);
	for (uint32_t i = 0; i < p_f.other_count && i < p_f.others.size(); ++i) {
		const host::OtherTrack &t = p_f.others[i];
		r_h.add(t.slot);
		r_h.add(t.count);
		for (size_t k = 0; k < t.count && k < t.samples.size(); ++k) {
			r_h.add(t.samples[k].server_tick);
			add_other(r_h, t.samples[k].soldier);
		}
		r_h.add(t.track_newest_tick);
	}
	add_stats(r_h, p_f.stats);
	r_h.add(p_f.newest_seq);
	r_h.add(p_f.own_weapons_valid);
	add_wire(r_h, p_f.own_weapons_prev);
	add_wire(r_h, p_f.own_weapons_curr);
	r_h.add(p_f.match_seed);
	r_h.add(p_f.newest_tick);
	r_h.add(p_f.own_kit);
	r_h.add(p_f.own_team);
	r_h.add(p_f.match_valid);
	add_match(r_h, p_f.match);
	r_h.add(p_f.player_count);
	for (uint32_t i = 0; i < p_f.player_count && i < p_f.players.size(); ++i) {
		add_row(r_h, p_f.players[i]);
	}
	r_h.add(p_f.own_respawn_tick);
	r_h.add(p_f.own_revive_until);
	r_h.add(p_f.own_unspawned);
}

void add_event(Fnv &r_h, const host::PresentEvent &p_e) {
	r_h.add(p_e.kind);
	r_h.add(p_e.timeline);
	r_h.add(p_e.slot);
	r_h.add(p_e.aux);
	r_h.add(p_e.def);
	r_h.add(p_e.flags);
	r_h.add(p_e.pellet);
	r_h.add(p_e.id);
	r_h.add(p_e.material);
	r_h.add(p_e.time);
	for (size_t k = 0; k < 3; ++k) {
		r_h.add(p_e.pos[k]);
		r_h.add(p_e.dir[k]);
	}
	r_h.add(p_e.value);
	r_h.add(p_e.extra);
	r_h.add(p_e.seed);
}

void add_shot(Fnv &r_h, const sim::weapons::ShotEvent &p_s) {
	r_h.add(p_s.time);
	r_h.add(p_s.spawn_time);
	r_h.add(p_s.seed);
	r_h.add(p_s.player_slot);
	r_h.add(p_s.shot_index);
	r_h.add(p_s.def);
	r_h.add(p_s.weapon_slot);
	r_h.add(p_s.pellets);
	add3(r_h, p_s.origin);
	add3(r_h, p_s.aim);
	add3(r_h, p_s.direction);
	r_h.add(p_s.spread);
	r_h.add(p_s.pellet_cone);
	r_h.add(p_s.recoil.pitch);
	r_h.add(p_s.recoil.yaw);
	r_h.add(p_s.tracer);
	r_h.add(p_s.power);
}

void add_pose(Fnv &r_h, const net::OtherPose &p_p) {
	r_h.add(p_p.slot);
	r_h.add(p_p.generation);
	r_h.add(p_p.pose);
	r_h.add(p_p.target_pose);
	r_h.add(p_p.transition);
	r_h.add(p_p.mode);
	r_h.add(p_p.flags);
	r_h.add(p_p.extrapolated);
	r_h.add(p_p.held);
	add3(r_h, p_p.position);
	add3(r_h, p_p.velocity);
	r_h.add(p_p.yaw);
	r_h.add(p_p.pitch);
	r_h.add(p_p.eye_height);
	r_h.add(p_p.tick_a);
	r_h.add(p_p.tick_b);
	r_h.add(p_p.kind);
}

void add_presented(Fnv &r_h, const host::PresentedSoldier &p_s) {
	r_h.add(p_s.slot);
	r_h.add(p_s.generation);
	add3(r_h, p_s.position);
	add3(r_h, p_s.velocity);
	r_h.add(p_s.yaw);
	r_h.add(p_s.pitch);
	r_h.add(p_s.pose);
	r_h.add(p_s.eye_height);
	r_h.add(p_s.mode);
	r_h.add(p_s.flags);
	r_h.add(p_s.extrapolated);
	r_h.add(p_s.target_pose);
	r_h.add(p_s.transition);
}

void add_other_weapon(Fnv &r_h, const host::PresentedOtherWeapon &p_w) {
	r_h.add(p_w.valid);
	r_h.add(p_w.slot);
	r_h.add(p_w.generation);
	r_h.add(p_w.def);
	r_h.add(p_w.phase);
	r_h.add(p_w.firing);
	r_h.add(p_w.ads);
	r_h.add(p_w.reload_seq);
	r_h.add(p_w.burst_seq);
	r_h.add(p_w.team);
	r_h.add(p_w.sample_tick);
}

struct StreamClient;

class ServerTransport final : public host::ClientTransport {
public:
	ServerTransport(host::GameServer &r_server, StreamClient &r_client) :
			server_(r_server), client_(r_client) {}

	int service(uint32_t p_wait_ms, net::Host::Event &r_event) override;
	void send(uint8_t p_channel, std::span<const uint8_t> p_bytes, net::Host::SendMode p_mode) override;
	void flush() override;
	void disconnect_all() override;
	uint32_t rtt_ms() const override { return 9; }

private:
	host::GameServer &server_;
	StreamClient &client_;
};

struct StreamClient {
	const char *name = "";
	uint32_t peer = 0;
	uint32_t phase = 0;
	uint8_t kit = 0;
	bool scripted = true;
	std::deque<net::Host::Event> inbox;
	bool disconnected = false;
	Fnv sent, frames, presented, events, gunplay, stats;
	uint64_t sends = 0, flushes = 0, frame_count = 0, event_count = 0, shot_count = 0, timed_count = 0, sample_count = 0, reports = 0;
	host::ClientLoop loop;
	host::InputSample input;
	float base_yaw = 0.0f;
	std::unique_ptr<host::PresentationFrame> frame = std::make_unique<host::PresentationFrame>();
	std::vector<host::PresentedSoldier> soldiers = std::vector<host::PresentedSoldier>(net::kMaxSlots);
	std::vector<host::PresentedOtherWeapon> weapons = std::vector<host::PresentedOtherWeapon>(net::kMaxSlots);
	std::vector<host::PredictedShot> shot_log;
	std::vector<host::PredictedTimed> timed_log;
	std::vector<host::OwnTestSample> sample_log;
	std::array<host::PresentEvent, 256> batch{};
	bool keep_events = false;
	std::vector<host::PresentEvent> kept;
	std::vector<host::PresentClock> kept_clocks;
	std::vector<host::PredictedShot> kept_shots;

	StreamHashes hashes() const { return { sent.h, frames.h, presented.h, events.h, gunplay.h, stats.h }; }
};

int ServerTransport::service(uint32_t, net::Host::Event &r_event) {
	if (client_.inbox.empty()) {
		return 0;
	}
	r_event = std::move(client_.inbox.front());
	client_.inbox.pop_front();
	return 1;
}

void ServerTransport::send(uint8_t p_channel, std::span<const uint8_t> p_bytes, net::Host::SendMode p_mode) {
	client_.sent.add(p_channel);
	client_.sent.add(p_mode);
	client_.sent.add(uint64_t(p_bytes.size()));
	client_.sent.bytes(p_bytes.data(), p_bytes.size());
	++client_.sends;
	server_.on_receive(client_.peer, p_channel, p_bytes);
}

void ServerTransport::flush() {
	client_.sent.add(uint8_t(0xF1));
	++client_.flushes;
}

void ServerTransport::disconnect_all() {
	client_.sent.add(uint8_t(0xD1));
	client_.disconnected = true;
	server_.on_disconnect(client_.peer);
}

void script_input(StreamClient &r_c, uint32_t p_step) {
	host::InputSample &in = r_c.input;
	const uint32_t t = p_step / kStepsPerTick + r_c.phase * 37u;
	in.move_forward = (t / 70u) % 3u == 0u ? 0.0f : 1.0f;
	in.move_right = (t / 45u) % 4u == 1u ? -0.6f : ((t / 45u) % 4u == 3u ? 0.6f : 0.0f);
	in.yaw = r_c.base_yaw + 0.0025f * float(p_step % 1200u) - 1.5f;
	in.pitch = 0.0005f * float(int32_t(p_step % 800u) - 400);
	in.held_buttons = 0;
	if ((t / 50u) % 5u == 2u) {
		in.held_buttons |= sim::buttons::kSprint;
	}
	if ((t / 40u) % 6u == 1u) {
		in.held_buttons |= sim::buttons::kZoom;
	}
	if (t % 45u < 12u) {
		in.held_buttons |= sim::buttons::kFire;
	}
	if (t % 300u >= 150u && t % 300u < 154u) {
		in.held_buttons |= sim::buttons::kReload;
	}
	in.jump_presses = t / 90u;
	in.fire_presses = t / 33u;
	in.pose_request = t % 240u < 2u ? host::PoseRequest::Set : host::PoseRequest::Keep;
	in.pose = sim::Pose((t / 240u) % 3u);
	in.free_camera = r_c.phase == 0 && t >= 600u && t < 640u;
}

void drive(StreamClient &r_c, uint32_t p_step, uint64_t p_now) {
	if (r_c.scripted) {
		script_input(r_c, p_step);
	}
	host::set_view_sample(r_c.input, *r_c.frame, p_now);
	r_c.loop.set_input(r_c.input);
	r_c.loop.iterate();
	if (r_c.loop.read_presentation(*r_c.frame)) {
		++r_c.frame_count;
		add_frame(r_c.frames, *r_c.frame);
		const host::PresentationFrame &f = *r_c.frame;
		if (f.valid && f.own_curr.life > r_c.input.life) {
			r_c.input.life = f.own_curr.life;
			r_c.base_yaw = sim::yaw_radians(f.own_curr.yaw);
		}
	}
	const host::PresentationFrame &f = *r_c.frame;
	if (f.serial != 0) {
		host::PresentOthersCounts counts;
		const size_t n = host::present_others(f, p_now, r_c.soldiers, counts);
		r_c.presented.add(uint64_t(n));
		for (size_t i = 0; i < n; ++i) {
			add_presented(r_c.presented, r_c.soldiers[i]);
		}
		r_c.presented.add(counts.extrapolated);
		r_c.presented.add(counts.window_extrapolated);
		r_c.presented.add(counts.held);
		const double render = host::present_render_ticks(f, p_now);
		r_c.presented.add(render);
		r_c.presented.add(host::present_view_quality(f, render));
		r_c.presented.add(host::present_own_fraction(f, p_now));
		const size_t w = host::present_other_weapons(f, render, r_c.weapons);
		r_c.presented.add(uint64_t(w));
		for (size_t i = 0; i < w; ++i) {
			add_other_weapon(r_c.presented, r_c.weapons[i]);
		}
		const host::PresentClock clock = host::present_clock(f, p_now);
		r_c.presented.add(clock.predicted_valid);
		r_c.presented.add(clock.render_valid);
		r_c.presented.add(clock.predicted);
		r_c.presented.add(clock.render);
	}
	size_t got = 0;
	while ((got = r_c.loop.events().drain(r_c.batch)) > 0) {
		for (size_t k = 0; k < got; ++k) {
			add_event(r_c.events, r_c.batch[k]);
			if (r_c.keep_events) {
				r_c.kept.push_back(r_c.batch[k]);
				r_c.kept_clocks.push_back(host::present_clock(*r_c.frame, p_now));
			}
		}
		r_c.event_count += got;
	}
	r_c.loop.take_predicted(r_c.shot_log, r_c.timed_log);
	for (const host::PredictedShot &s : r_c.shot_log) {
		r_c.gunplay.add(s.tick);
		add_shot(r_c.gunplay, s.shot);
	}
	for (const host::PredictedTimed &e : r_c.timed_log) {
		r_c.gunplay.add(e.tick);
		r_c.gunplay.add(e.event.time);
		r_c.gunplay.add(e.event.event);
		r_c.gunplay.add(e.event.weapon_slot);
	}
	if (r_c.keep_events) {
		r_c.kept_shots.insert(r_c.kept_shots.end(), r_c.shot_log.begin(), r_c.shot_log.end());
	}
	r_c.shot_count += r_c.shot_log.size();
	r_c.timed_count += r_c.timed_log.size();
	r_c.shot_log.clear();
	r_c.timed_log.clear();
	r_c.loop.take_own_test_samples(r_c.sample_log);
	for (const host::OwnTestSample &s : r_c.sample_log) {
		r_c.gunplay.add(s.shooter);
		r_c.gunplay.add(s.shot_index);
		r_c.gunplay.add(s.target);
		r_c.gunplay.add(s.time_q8);
		add_pose(r_c.gunplay, s.pose);
	}
	r_c.sample_count += r_c.sample_log.size();
	r_c.sample_log.clear();
	net::DebugReport report;
	while (r_c.loop.take_debug_report(report)) {
		r_c.gunplay.add(report.id);
		r_c.gunplay.add(report.ok);
		r_c.gunplay.add(uint64_t(report.trace.size()));
		r_c.gunplay.add(uint64_t(report.targets.size()));
		++r_c.reports;
	}
}

void deliver(std::span<const host::OutDatagram> p_out, std::span<const std::unique_ptr<StreamClient>> p_clients, std::span<const uint8_t> p_hold_mask,
		uint64_t &r_others) {
	for (const host::OutDatagram &d : p_out) {
		for (const std::unique_ptr<StreamClient> &c : p_clients) {
			if (c->peer != d.peer || c->disconnected) {
				continue;
			}
			net::Host::Event ev;
			ev.type = net::Host::EventType::Receive;
			ev.peer = d.peer;
			ev.channel = d.channel;
			ev.data = d.bytes;
			const std::optional<net::MessageType> type = net::peek_type(d.bytes);
			if (type == net::MessageType::Snapshot) {
				for (size_t i = 0; i < p_hold_mask.size() && i < ev.data.size(); ++i) {
					ev.data[i] = uint8_t(ev.data[i] & ~p_hold_mask[i]);
				}
			}
			if (type == net::MessageType::SnapshotOthers) {
				++r_others;
			}
			c->inbox.push_back(std::move(ev));
		}
	}
}

void print(const StreamClient &p_c) {
	const host::ClientStats s = p_c.loop.stats();
	std::printf("host.client.stream %s: %llu sends, %llu frames, %llu events, %llu predicted shots, %llu timed, %llu own samples, %llu reports; "
				"spawns %llu, reconciliations %llu, clock resets %llu, snapshots %llu lost %llu late %llu, remote shots %llu, hits %llu, deaths %llu, "
				"refused %d '%s'\n",
			p_c.name, (unsigned long long)p_c.sends, (unsigned long long)p_c.frame_count, (unsigned long long)p_c.event_count,
			(unsigned long long)p_c.shot_count, (unsigned long long)p_c.timed_count, (unsigned long long)p_c.sample_count,
			(unsigned long long)p_c.reports, (unsigned long long)s.spawns, (unsigned long long)s.reconciliations, (unsigned long long)s.clock_resets,
			(unsigned long long)s.snapshots_received, (unsigned long long)s.snapshots_lost, (unsigned long long)s.snapshots_late,
			(unsigned long long)s.remote_shots, (unsigned long long)s.combat_hits, (unsigned long long)s.combat_deaths, int(s.refused), s.last_error.data());
	const StreamHashes h = p_c.hashes();
	std::printf("host.client.stream %s: { 0x%016llxull, 0x%016llxull, 0x%016llxull, 0x%016llxull, 0x%016llxull, 0x%016llxull }\n", p_c.name,
			(unsigned long long)h.sent, (unsigned long long)h.frames, (unsigned long long)h.presented, (unsigned long long)h.events,
			(unsigned long long)h.gunplay, (unsigned long long)h.stats);
}

void check_hashes(const StreamClient &p_c, const StreamHashes &p_want) {
	const StreamHashes h = p_c.hashes();
	INFO(p_c.name);
	CHECK(h.sent == p_want.sent);
	CHECK(h.frames == p_want.frames);
	CHECK(h.presented == p_want.presented);
	CHECK(h.events == p_want.events);
	CHECK(h.gunplay == p_want.gunplay);
	CHECK(h.stats == p_want.stats);
}


class Stream {
public:
	Stream(host::GameServer &r_server, std::shared_ptr<const host::SharedLevel> p_level) :
			server_(r_server), level_(std::move(p_level)) {}

	uint64_t others = 0;

	std::span<const host::OutDatagram> out() const { return out_; }

	StreamClient &add(const char *p_name, uint32_t p_peer, uint32_t p_phase, uint8_t p_kit, host::ClientSessionConfig p_config) {
		auto c = std::make_unique<StreamClient>();
		c->name = p_name;
		c->peer = p_peer;
		c->phase = p_phase;
		c->kit = p_kit;
		p_config.port = 1;
		p_config.name = p_name;
		p_config.game_build = kStreamGameBuild;
		p_config.mechanics_cfg = OPENBF3_MECHANICS_CFG;
		const core::Status init = c->loop.init(p_config, level_);
		REQUIRE_MESSAGE(init.ok(), (init.ok() ? std::string() : init.error().message));
		server_.on_connect(p_peer);
		c->loop.begin(std::make_unique<ServerTransport>(server_, *c), clock_);
		clients_.push_back(std::move(c));
		return *clients_.back();
	}

	void play(uint32_t p_ticks, const std::function<void(sim::Tick)> &p_on_tick) {
		run(p_ticks, p_on_tick);
		finish();
	}

	void run(uint32_t p_ticks, const std::function<void(sim::Tick)> &p_on_tick) {
		for (const uint32_t end = step_ + p_ticks * kStepsPerTick; step_ < end; ++step_) {
			const uint32_t step = step_;
			now_ += kStepNs;
			for (const std::unique_ptr<StreamClient> &c : clients_) {
				drive(*c, step, now_);
			}
			if (step % kStepsPerTick != kStepsPerTick - 1u) {
				continue;
			}
			out_.clear();
			server_.tick(out_);
			deliver(out_, clients_, hold_mask_, others);
			const sim::Tick tick = server_.current_tick();
			for (const std::unique_ptr<StreamClient> &c : clients_) {
				for (Fnv *h : { &c->sent, &c->frames, &c->presented, &c->events, &c->gunplay }) {
					h->add(tick);
				}
				const host::PresentationFrame &f = *c->frame;
				const bool alive = f.valid && (f.own_curr.flags & sim::soldier_flags::kAlive) != 0;
				if (c->scripted && !alive && tick % 30u == 0u) {
					c->loop.send_spawn_request(c->kit);
				}
			}
			p_on_tick(tick);
		}
	}

	void finish() {
		for (const std::unique_ptr<StreamClient> &c : clients_) {
			add_stats(c->stats, c->loop.stats());
			c->loop.shutdown();
			c->loop.close();
			print(*c);
		}
		std::printf("host.client.stream: %llu SnapshotOthers delivered\n", (unsigned long long)others);
	}

private:
	host::GameServer &server_;
	std::shared_ptr<const host::SharedLevel> level_;
	uint64_t now_ = 1'000'000'000'000ull;
	host::MonotonicClock clock_ = [this] { return now_; };
	std::vector<std::unique_ptr<StreamClient>> clients_;
	std::vector<uint8_t> hold_mask_ = tt::detail::echo_hold_mask();
	std::vector<host::OutDatagram> out_;
	uint32_t step_ = 0;
};

}

TEST_SUITE("host-client-stream") {
	TEST_CASE("TDM: three client loops on a stepped clock against a 26-bot server, 1500 ticks: what each client sends and presents hashes to the recorded stream") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		host::GameServerConfig cfg;
		cfg.level = level->shared;
		cfg.max_slots = 40;
		cfg.game_build = kStreamGameBuild;
		cfg.dev_commands = true;
		cfg.seed = 33;
		cfg.gunplay.mechanics_cfg = OPENBF3_MECHANICS_CFG;
		cfg.rules.mode = host::MatchRulesConfig::Mode::Tdm;
		cfg.rules.deploy_screen = true;
		cfg.rules.revive = true;
		cfg.rules.preround_override_s = 0.0f;
		cfg.rules.max_players = 32;
		auto fill = std::make_shared<host::BotFillConfig>();
		fill->level = level->bots;
		fill->count = 0;
		cfg.bots = fill;
		auto bots = std::make_shared<host::TdmBotConfig>();
		bots->count = 26;
		bots->tuning_cfg = OPENBF3_BOT_TUNING_CFG;
		cfg.tdm_bots = bots;
		host::GameServer server;
		const core::Status st = server.init(cfg);
		REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));

		Stream stream(server, level->shared);
		host::ClientSessionConfig alpha;
		core::Result<net::NetSimConfig> lossy = net::parse_netsim("lat=20-45,loss=3,reorder=5,dup=2,spike=0.2,seed=11");
		REQUIRE_MESSAGE(lossy.ok(), (lossy.ok() ? std::string() : lossy.error().message));
		alpha.netsim = *lossy;
		alpha.record_predicted = true;
		alpha.debug_hash_compare = true;
		alpha.send_kit_select = true;
		alpha.kit_select.kit = 2;
		StreamClient &a = stream.add("alpha", 1u, 0u, 2u, alpha);
		host::ClientSessionConfig bravo;
		bravo.initial_clock_offset_ticks = 4;
		StreamClient &b = stream.add("bravo", 2u, 1u, 1u, bravo);
		host::ClientSessionConfig charlie;
		charlie.gunplay_from_level = false;
		StreamClient &c = stream.add("charlie", 3u, 2u, 0u, charlie);
		c.scripted = false;

		stream.play(kTdmTicks, [&](sim::Tick p_tick) {
			if (p_tick == 300u) {
				net::DebugRequest q;
				q.kind = net::DebugKind::TargetInfo;
				q.id = 300;
				q.slot = 0;
				a.loop.send_debug(q);
			}
			if (p_tick == 420u) {
				b.loop.shift_clock(6);
			}
			if (p_tick == 500u) {
				net::KitSelect k;
				k.kit = 3;
				b.loop.send_kit_select(k);
			}
			if (p_tick == 700u) {
				net::DebugRequest q;
				q.kind = net::DebugKind::Trace;
				q.id = 700;
				q.ticks = 20;
				a.loop.send_debug(q);
			}
			if (p_tick == 820u) {
				b.loop.shift_clock(-9);
			}
		});
		const host::ClientStats as = a.loop.stats();
		const host::ClientStats bs = b.loop.stats();
		const host::ClientStats cs = c.loop.stats();
		CHECK(as.gunplay);
		CHECK(bs.gunplay);
		CHECK(as.spawns > 0u);
		CHECK(bs.spawns > 0u);
		CHECK(as.snapshots_lost > 0u);
		CHECK(as.snapshots_late > 0u);
		CHECK(as.reconciliations > 0u);
		CHECK(as.remote_shots > 0u);
		CHECK(as.combat_hits > 0u);
		CHECK(bs.clock_resets > 0u);
		CHECK(a.shot_count > 0u);
		CHECK(a.sample_count > 0u);
		CHECK(a.reports >= 2u);
		CHECK_FALSE(cs.refused);
		CHECK(cs.kicked);
		CHECK(cs.kick_reason == net::KickReason::Weapon);
		CHECK(cs.mismatch_components == net::mismatch_bits::kWeapon);
		for (const host::ClientStats *st : { &as, &bs }) {
			CHECK(st->logic_full_syncs >= 1u);
			CHECK(st->logic_events > 0u);
			CHECK(st->logic_early_events == 0u);
			CHECK(st->bad_logic_datagrams == 0u);
		}
		const std::span<const net::PlayerRow> want = server.rules()->players();
		for (const StreamClient *sc : { &a, &b }) {
			const host::PresentationFrame &f = *sc->frame;
			const std::span<const net::PlayerRow> got(
					f.players.data(), std::min<size_t>(f.player_count, f.players.size()));
			size_t in_squads = 0, leaders = 0, differ = 0;
			for (const net::PlayerRow &w : want) {
				const auto at =
						std::find_if(got.begin(), got.end(), [&](const net::PlayerRow &r) { return r.slot == w.slot; });
				if (at == got.end() || at->team != w.team || at->squad != w.squad || at->leader != w.leader) {
					++differ;
					continue;
				}
				in_squads += at->squad != 0 ? 1u : 0u;
				leaders += at->leader ? 1u : 0u;
			}
			std::printf("host.client.stream %s: PlayerList rows %zu of %zu, %zu differ from the server's, %zu in a "
						"squad, %zu leaders\n",
					sc->name, got.size(), want.size(), differ, in_squads, leaders);
			CHECK(got.size() == want.size());
			CHECK(differ == 0u);
			CHECK(in_squads == want.size());
			CHECK(leaders >= 2u);
		}
		check_hashes(a, kAlpha);
		check_hashes(b, kBravo);
		check_hashes(c, kCharlie);
	}

	TEST_CASE("dev server: one client loop among 44 walkers and target bots, 600 ticks: the SnapshotOthers path hashes to the recorded stream") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		host::GameServerConfig cfg;
		cfg.level = level->shared;
		cfg.max_slots = 56;
		cfg.game_build = kStreamGameBuild;
		cfg.dev_commands = true;
		cfg.seed = 5;
		cfg.walkers = 44;
		cfg.respawn_ticks = 45;
		cfg.snapshot_budget_bytes = 3 * uint32_t(net::kMaxDatagramBytes);
		cfg.gunplay.mechanics_cfg = OPENBF3_MECHANICS_CFG;
		cfg.gunplay.target_bots = 2;
		auto fill = std::make_shared<host::BotFillConfig>();
		fill->level = level->bots;
		fill->count = 2;
		cfg.bots = fill;
		host::GameServer server;
		const core::Status st = server.init(cfg);
		REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));

		Stream stream(server, level->shared);
		host::ClientSessionConfig delta;
		delta.record_predicted = true;
		StreamClient &d = stream.add("delta", 1u, 3u, 0u, delta);
		stream.play(kDevTicks, [](sim::Tick) {});
		const host::ClientStats ds = d.loop.stats();
		CHECK(ds.gunplay);
		CHECK(ds.spawns > 0u);
		CHECK(ds.snapshot_others_received > 0u);
		CHECK(stream.others > 0u);
		check_hashes(d, kDelta);
	}

	TEST_CASE("late join: a client joining a 26-bot round at tick 300 is bound by its first snapshot, and every shot "
			  "and impact it shares with a client "
			  "there from the start carries the same seed") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		host::GameServerConfig cfg;
		cfg.level = level->shared;
		cfg.max_slots = 40;
		cfg.game_build = kStreamGameBuild;
		cfg.seed = 71;
		cfg.gunplay.mechanics_cfg = OPENBF3_MECHANICS_CFG;
		cfg.rules.mode = host::MatchRulesConfig::Mode::Tdm;
		cfg.rules.preround_override_s = 0.0f;
		cfg.rules.max_players = 32;
		auto fill = std::make_shared<host::BotFillConfig>();
		fill->level = level->bots;
		fill->count = 0;
		cfg.bots = fill;
		auto bots = std::make_shared<host::TdmBotConfig>();
		bots->count = 26;
		bots->tuning_cfg = OPENBF3_BOT_TUNING_CFG;
		cfg.tdm_bots = bots;
		host::GameServer server;
		const core::Status st = server.init(cfg);
		REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));

		constexpr sim::Tick kJoinTick = 300;
		Stream stream(server, level->shared);
		host::ClientSessionConfig early;
		core::Result<net::NetSimConfig> lag = net::parse_netsim("lat=50,seed=3");
		REQUIRE(lag.ok());
		early.netsim = *lag;
		StreamClient &a = stream.add("early", 1u, 0u, 2u, early);
		a.keep_events = true;
		StreamClient *b = nullptr;
		stream.play(1800, [&](sim::Tick p_tick) {
			if (p_tick == kJoinTick) {
				host::ClientSessionConfig late;
				late.netsim = *lag;
				b = &stream.add("late", 2u, 1u, 1u, late);
				b->keep_events = true;
			}
		});
		REQUIRE(b != nullptr);
		const sim::Tick a_first = a.frame->first_snapshot_tick, b_first = b->frame->first_snapshot_tick;
		std::printf("host.client.join: first snapshots at ticks %u (early) and %u (late, joined at %u)\n",
				unsigned(a_first), unsigned(b_first), unsigned(kJoinTick));
		CHECK(a_first > 0u);
		CHECK(a_first < kJoinTick);
		CHECK(b_first > kJoinTick);

		size_t waiting = 0, before = 0;
		for (size_t i = 0; i < b->kept.size(); ++i) {
			const host::PresentEvent &e = b->kept[i];
			const host::PresentClock &c = b->kept_clocks[i];
			waiting += c.join == present::PresentJoin::Waiting ? 1u : 0u;
			const bool timed = e.timeline == host::PresentTimeline::Render;
			const bool old = timed && e.time < sim::weapons::tick_start(b_first);
			before += old ? 1u : 0u;
			if (c.join == present::PresentJoin::Joined) {
				CHECK(c.first_snapshot == sim::weapons::tick_start(b_first));
				CHECK(c.before_join(e.timeline, e.time) == old);
			}
		}

		using Key = std::tuple<int, uint8_t, uint32_t, uint8_t, uint32_t>;
		const auto key_of = [](const host::PresentEvent &p_e) {
			return Key{ int(p_e.kind), p_e.slot, p_e.id, p_e.pellet,
				p_e.kind == host::PresentKind::Impact ? p_e.extra : 0u };
		};
		std::map<Key, const host::PresentEvent *> early_events;
		for (const host::PresentEvent &e : a.kept) {
			if (e.kind == host::PresentKind::RemoteShot || e.kind == host::PresentKind::Impact) {
				early_events.emplace(key_of(e), &e);
			}
		}
		const uint8_t a_slot = a.loop.stats().slot, b_slot = b->loop.stats().slot;
		size_t shared_shots = 0, shared_impacts = 0, differ = 0, impact_times = 0;
		for (const host::PresentEvent &e : b->kept) {
			if ((e.kind != host::PresentKind::RemoteShot && e.kind != host::PresentKind::Impact) || e.slot == a_slot ||
					e.slot == b_slot) {
				continue;
			}
			const auto it = early_events.find(key_of(e));
			if (it == early_events.end()) {
				continue;
			}
			const host::PresentEvent &o = *it->second;
			const bool impact = e.kind == host::PresentKind::Impact;
			const bool same = o.seed == e.seed && o.def == e.def && o.flags == e.flags && o.material == e.material &&
					(impact || o.time == e.time);
			differ += same ? 0u : 1u;
			impact_times += impact && o.time != e.time ? 1u : 0u;
			(impact ? shared_impacts : shared_shots) += 1u;
		}
		std::printf("host.client.join: late client %zu events (%zu while waiting for its first snapshot, %zu timed "
					"before it), %zu shots and %zu impacts shared with the early client, %zu differ in a choice, %zu "
					"impacts at another time\n",
				b->kept.size(), waiting, before, shared_shots, shared_impacts, differ, impact_times);
		CHECK(shared_shots > 20u);
		CHECK(shared_impacts > 20u);
		CHECK(differ == 0u);
	}

	host::GameServerConfig solo_tdm_config(const tt::TdmLevel &p_level, uint64_t p_seed) {
		host::GameServerConfig cfg;
		cfg.level = p_level.shared;
		cfg.max_slots = 8;
		cfg.game_build = kStreamGameBuild;
		cfg.seed = p_seed;
		cfg.gunplay.mechanics_cfg = OPENBF3_MECHANICS_CFG;
		cfg.rules.mode = host::MatchRulesConfig::Mode::Tdm;
		cfg.rules.deploy_screen = true;
		cfg.rules.preround_override_s = 0.0f;
		cfg.rules.min_players_override = 1;
		return cfg;
	}

	TEST_CASE("specializations: a client predicts the spawn its Spawned names, the Ammo reserve with no weapon "
			  "reconciliation") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const uint32_t ammo = tt::specialization_id(*level->shared->gunplay, "ammoboostl1");
		REQUIRE(ammo != 0);
		host::GameServer server;
		const core::Status st = server.init(solo_tdm_config(*level, 91));
		REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));

		const uint8_t support = uint8_t(host::KitRole::Support);
		Stream stream(server, level->shared);
		StreamClient &a = stream.add("ammo", 1u, 0u, support, host::ClientSessionConfig{});
		a.scripted = false;
		int32_t spawn_reserve = -1, server_reserve = -1, plain = 0, boosted = 0;
		uint32_t mask = 0;
		uint32_t spawned_lives = 0;
		stream.play(600, [&](sim::Tick p_tick) {
			const host::PresentationFrame &f = *a.frame;
			const bool alive = f.valid && (f.own_curr.flags & sim::soldier_flags::kAlive) != 0;
			if (p_tick == 20u) {
				net::LoadoutRequest q;
				q.kit = support;
				q.specialization = ammo;
				a.loop.send_loadout_request(q);
			}
			if (!alive && p_tick >= 40u && p_tick % 30u == 0u) {
				a.loop.send_spawn_request(support);
			}
			if (alive && f.own_curr.life > spawned_lives) {
				spawned_lives = f.own_curr.life;
				spawn_reserve = f.own_weapons_curr.slots[0].reserve;
				const host::Gunplay *g = server.gunplay();
				const uint8_t slot = a.loop.stats().slot;
				const sim::weapons::WeaponSlotState &primary = g->weapons(slot).slots[0];
				const sim::weapons::WeaponSimDef &d = g->table().def(primary.def, primary.variant);
				mask = g->spawn_specializations(slot);
				plain = d.magazines * d.magazine_capacity;
				boosted = int32_t(std::lround(d.magazines * 1.5)) * d.magazine_capacity;
				server_reserve = primary.reserve;
			}
		});
		const host::ClientStats s = a.loop.stats();
		std::printf(
				"specializations: Ammo client spawned %llu time(s), predicted reserve %d, the server's %d (plain %d, "
				"boosted %d), weapon reconciliations %llu, ring misses %llu\n",
				(unsigned long long)s.spawns, spawn_reserve, server_reserve, plain, boosted,
				(unsigned long long)s.weapon_reconciliations, (unsigned long long)s.weapon_ring_misses);
		REQUIRE(s.spawns >= 1);
		CHECK(mask != 0u);
		CHECK(boosted > plain);
		CHECK(server_reserve == boosted);
		CHECK(spawn_reserve == boosted);
		CHECK(s.weapon_reconciliations == 0);
	}

	TEST_CASE("specializations: Sprint raises the sprint speed by the body's SprintMultiplier, predicted with no "
			  "reconciliation") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level == nullptr) {
			return;
		}
		const uint32_t sprint = tt::specialization_id(*level->shared->gunplay, "sprintboostl1");
		REQUIRE(sprint != 0);
		host::GameServer server;
		const core::Status st = server.init(solo_tdm_config(*level, 92));
		REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
		const uint8_t assault = uint8_t(host::KitRole::Assault);
		Stream stream(server, level->shared);
		StreamClient &a = stream.add("sprint", 1u, 0u, assault, host::ClientSessionConfig{});
		a.scripted = false;
		float fastest = 0.0f;
		uint64_t reconciled_alive = 0;
		stream.play(600, [&](sim::Tick p_tick) {
			const host::PresentationFrame &f = *a.frame;
			const bool alive = f.valid && (f.own_curr.flags & sim::soldier_flags::kAlive) != 0;
			if (p_tick == 20u) {
				net::LoadoutRequest q;
				q.kit = assault;
				q.specialization = sprint;
				a.loop.send_loadout_request(q);
			}
			if (!alive && p_tick >= 40u && p_tick % 30u == 0u) {
				a.loop.send_spawn_request(assault);
			}
			a.input.move_forward = alive ? 1.0f : 0.0f;
			a.input.held_buttons = alive ? sim::buttons::kSprint : 0u;
			a.input.yaw = a.base_yaw;
			a.input.pitch = 0.0f;
			const host::ClientStats s = a.loop.stats();
			if (!alive) {
				reconciled_alive = s.reconciliations;
				return;
			}
			const sim::SoldierState *own = server.soldier(s.slot);
			if (own != nullptr && (own->flags & sim::soldier_flags::kSprinting) != 0) {
				fastest = std::max(fastest, std::hypot(own->velocity[0], own->velocity[2]));
			}
		});
		const host::ClientStats s = a.loop.stats();
		const host::Gunplay *g = server.gunplay();
		REQUIRE(g != nullptr);
		const sim::MotorParams &motor = level->shared->motor;
		const float plain = motor.poses[size_t(sim::Pose::Stand)].on_ground.velocity *
				motor.poses[size_t(sim::Pose::Stand)].on_ground.sprint_multiplier * motor.body_sprint_multiplier;
		const float boosted = plain / motor.body_sprint_multiplier * 1.1f;
		std::printf("specializations: Sprint client spawned %llu time(s), fastest sprint %.3f m/s (plain %.3f, boosted "
					"%.3f), reconciliations %llu (%llu after the spawn)\n",
				(unsigned long long)s.spawns, double(fastest), double(plain), double(boosted),
				(unsigned long long)s.reconciliations, (unsigned long long)(s.reconciliations - reconciled_alive));
		REQUIRE(s.spawns >= 1);
		CHECK(fastest > plain + 0.5f * (boosted - plain));
		CHECK(fastest <= boosted + 1e-3f);
		CHECK(s.reconciliations == reconciled_alive);
	}

	TEST_CASE("host-client-stream: install unchanged") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level != nullptr) {
			tt::check_install_unchanged(*level);
		}
	}
}

namespace {

uint32_t drive_predicted(tt::TdmLevel &r_level, const char *p_blueprint, const char *p_name, const char *p_row,
		const std::function<float(uint32_t)> &p_steer, float p_least_travel_m) {
	const host::LevelVehicles &lv = *r_level.shared->vehicles;
	host::GameServerConfig cfg;
	cfg.level = r_level.shared;
	cfg.max_slots = 8;
	cfg.game_build = kStreamGameBuild;
	cfg.dev_commands = true;
	cfg.seed = 65;
	cfg.gunplay.mechanics_cfg = OPENBF3_MECHANICS_CFG;
	cfg.rules.mode = host::MatchRulesConfig::Mode::Tdm;
	cfg.rules.deploy_screen = false;
	cfg.rules.revive = true;
	cfg.rules.preround_override_s = 0.0f;
	cfg.rules.min_players_override = 1;
	host::GameServer server;
	const core::Status st = server.init(cfg);
	REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
	core::Result<std::unique_ptr<sim::CollisionWorld>> world =
			sim::CollisionWorld::create(r_level.shared->static_world);
	REQUIRE(world.ok());
	{
		const auto start = std::chrono::steady_clock::now();
		host::ClientVehiclePrediction built;
		REQUIRE(built.init(*r_level.shared).ok());
		std::printf("%s: a client's vehicle world over MP_007's static shapes builds in %.1f ms\n", p_row,
				core::ms_since(start));
	}

	uint64_t now = 1'000'000'000'000ull;
	const host::MonotonicClock clock = [&] { return now; };
	std::vector<std::unique_ptr<StreamClient>> clients;
	const auto add = [&](const char *p_name, uint32_t p_peer, const char *p_netsim) -> StreamClient & {
		auto c = std::make_unique<StreamClient>();
		c->name = p_name;
		c->peer = p_peer;
		c->scripted = false;
		host::ClientSessionConfig cc;
		core::Result<net::NetSimConfig> ns = net::parse_netsim(p_netsim);
		REQUIRE(ns.ok());
		cc.netsim = *ns;
		cc.port = 1;
		cc.name = p_name;
		cc.game_build = kStreamGameBuild;
		cc.mechanics_cfg = OPENBF3_MECHANICS_CFG;
		cc.request_dev_commands = true;
		const core::Status init = c->loop.init(cc, r_level.shared);
		REQUIRE_MESSAGE(init.ok(), (init.ok() ? std::string() : init.error().message));
		server.on_connect(p_peer);
		c->loop.begin(std::make_unique<ServerTransport>(server, *c), clock);
		clients.push_back(std::move(c));
		return *clients.back();
	};
	StreamClient &driver = add("driver", 1u, "lat=40-60,loss=2,seed=3");
	StreamClient &watcher = add("watcher", 2u, "lat=20,seed=4");

	std::optional<uint16_t> spawner;
	std::map<sim::Tick, sim::Vec3> server_at;
	uint64_t others = 0, step = 0;
	uint32_t steered = 0;
	std::vector<host::OutDatagram> out;
	const auto tick = [&] {
		for (uint32_t k = 0; k < kStepsPerTick; ++k, ++step) {
			now += kStepNs;
			for (const std::unique_ptr<StreamClient> &c : clients) {
				drive(*c, uint32_t(step), now);
			}
		}
		out.clear();
		server.tick(out);
		deliver(out, clients, {}, others);
		for (const host::OutDatagram &d : out) {
			if (d.peer != driver.peer || net::peek_type(d.bytes) != net::MessageType::VehicleStates) {
				continue;
			}
			core::Result<net::VehicleStates> m = net::decode_vehicle_states(d.bytes);
			REQUIRE(m.ok());
			for (const net::VehicleWire &w : m->vehicles) {
				if (spawner && w.spawner == *spawner) {
					server_at[m->server_tick] = w.state.body.com_position;
					steered += w.state.driver.track_steer[0] != 0.0f || w.state.driver.track_steer[1] != 0.0f ? 1u : 0u;
				}
			}
		}
		for (const std::unique_ptr<StreamClient> &c : clients) {
			const host::PresentationFrame &f = *c->frame;
			if (!(f.valid && (f.own_curr.flags & sim::soldier_flags::kAlive) != 0) &&
					server.current_tick() % 30u == 0u) {
				c->loop.send_spawn_request(0);
			}
		}
	};
	const auto alive = [](const StreamClient &p_c) {
		return p_c.frame->valid && (p_c.frame->own_curr.flags & sim::soldier_flags::kAlive) != 0;
	};
	for (uint32_t t = 0; t < sim::seconds_to_ticks(tt::latest_initial_delay(lv)) + 300u; ++t) {
		tick();
	}
	REQUIRE(alive(driver));
	REQUIRE(alive(watcher));
	const host::PresentationFrame &df = *driver.frame;
	const host::PresentedVehicle *parked = nullptr;
	for (uint8_t i = 0; i < df.vehicle_count; ++i) {
		const host::PresentedVehicle &v = df.vehicles[i];
		if (v.spawner < lv.spawners.size() && lv.kinds[lv.spawners[v.spawner].kind].blueprint == p_blueprint &&
				tt::up_y_of(v.rotation) > 0.95f && (parked == nullptr || v.spawner < parked->spawner)) {
			parked = &v;
		}
	}
	REQUIRE(parked != nullptr);
	spawner = parked->spawner;
	const host::LevelVehicleKind &kind = lv.kinds[lv.spawners[*spawner].kind];
	const sim::Vec3 com = parked->com_position;
	std::optional<sim::Vec3> beside;
	for (int k = 0; k < 16 && !beside; ++k) {
		const float a = 0.3926991f * float(k);
		sim::StaticHit ground;
		if ((*world)->ground_below({ com[0] + 0.5f * kind.entry_radius * std::cos(a), com[1] + 5.0f,
										   com[2] + 0.5f * kind.entry_radius * std::sin(a) },
					12.0f, sim::query_mask::kMovement, &ground) &&
				!(*world)->overlap_capsule({ ground.position[0], ground.position[1] + 0.05f, ground.position[2] }, 0.4f,
						1.8f, sim::query_mask::kMovement)) {
			beside = ground.position;
		}
	}
	REQUIRE(beside.has_value());
	net::DebugRequest teleport;
	teleport.kind = net::DebugKind::Teleport;
	teleport.id = 1;
	teleport.position = *beside;
	driver.loop.send_debug(teleport);
	const uint8_t slot = df.stats.slot;
	const auto own_vehicle = [&](const host::PresentationFrame &p_f) -> const host::PresentedVehicle * {
		for (uint8_t i = 0; i < p_f.vehicle_count; ++i) {
			if (p_f.vehicles[i].spawner == *spawner) {
				return &p_f.vehicles[i];
			}
		}
		return nullptr;
	};
	for (uint32_t t = 0; t < 30; ++t) {
		tick();
	}
	const uint64_t entry_before = df.stats.reconciliations;
	uint32_t waited = 0;
	for (; waited < 240; ++waited) {
		driver.input.held_buttons = (waited / 8u) % 2u == 0u ? sim::buttons::kUse : 0u;
		tick();
		const host::PresentedVehicle *v = own_vehicle(df);
		if (v != nullptr && v->occupants[0] == slot) {
			break;
		}
	}
	driver.input.held_buttons = 0;
	REQUIRE(waited < 240);
	for (uint32_t t = 0; t < 30; ++t) {
		tick();
	}
	const uint64_t reconciled_before = df.stats.reconciliations;
	const uint64_t entry_reconciled = reconciled_before - entry_before;
	std::map<sim::Tick, sim::Vec3> drawn_at;
	const uint32_t drive_ticks = 480;
	for (uint32_t t = 0; t < drive_ticks; ++t) {
		driver.input.move_forward = 1.0f;
		driver.input.move_right = p_steer(t);
		tick();
		const host::PresentedVehicle *v = own_vehicle(df);
		REQUIRE(v != nullptr);
		if (df.valid) {
			drawn_at[df.tick] = host::vehicle_pose_at(*v, double(df.tick)).com_position;
		}
		const host::PresentedVehicle *seen = own_vehicle(*watcher.frame);
		REQUIRE(seen != nullptr);
		CHECK(seen->occupants[0] == slot);
	}
	driver.input.move_forward = 0.0f;
	driver.input.move_right = 0.0f;
	for (uint32_t t = 0; t < 30; ++t) {
		tick();
	}
	const uint64_t reconciled = df.stats.reconciliations - reconciled_before;
	std::vector<float> errors;
	float travelled = 0.0f;
	std::optional<sim::Vec3> first;
	for (const auto &[at, drawn] : drawn_at) {
		const auto s = server_at.find(at);
		if (s == server_at.end()) {
			continue;
		}
		if (!first) {
			first = s->second;
		}
		errors.push_back(core::vec::length(core::vec::sub(drawn, s->second)));
		travelled = std::max(travelled, core::vec::horizontal_distance(s->second, *first));
	}
	REQUIRE(errors.size() > 100u);
	float mean = 0.0f;
	for (const float e : errors) {
		mean += e / float(errors.size());
	}
	const float p95 = core::nearest_rank(errors, 0.95);
	const float largest = *std::max_element(errors.begin(), errors.end());
	const host::ClientStats &s = df.stats;
	std::printf("%s: the driver's %s (spawner %u) went %.1f m; at %zu predicted ticks the drawn own vehicle was "
				"%.3f m (mean), %.3f m (p95), %.3f m (max) from the server's state at that tick; %u of the server's "
				"states had a track steer term; %llu soldier reconciliations while entering, %llu while seated; "
				"vehicle prediction: %llu starts, %llu predicted ticks, %llu replayed, %llu matches, %llu "
				"corrections (largest %.3f m), %llu failures\n",
			p_row, p_name, unsigned(*spawner), double(travelled), errors.size(), double(mean), double(p95),
			double(largest), unsigned(steered), (unsigned long long)entry_reconciled, (unsigned long long)reconciled,
			(unsigned long long)s.vehicle_prediction_starts, (unsigned long long)s.vehicle_predicted_ticks,
			(unsigned long long)s.vehicle_replayed_ticks, (unsigned long long)s.vehicle_prediction_matches,
			(unsigned long long)s.vehicle_corrections, double(s.vehicle_max_correction_m),
			(unsigned long long)s.vehicle_prediction_failures);
	CHECK(travelled > p_least_travel_m);
	CHECK(mean < 0.05f);
	CHECK(p95 < 0.15f);
	CHECK(reconciled <= 4u);
	CHECK(s.vehicle_prediction_failures == 0u);
	const host::PresentedVehicle *seen = own_vehicle(*watcher.frame);
	REQUIRE(seen != nullptr);
	CHECK(seen->predicted == 0);
	for (const std::unique_ptr<StreamClient> &c : clients) {
		c->loop.shutdown();
		c->loop.close();
	}
	return steered;
}

}

TEST_SUITE("host-vehicles-predict") {
	TEST_CASE("vehicles: a client 100 ms from the server drives a Growler on MP_007 ConquestSmall0; the drawn own "
			  "vehicle at the predicted tick is where the server puts it, the seated soldier is not reconciled, and a "
			  "second client sees the vehicle unpredicted") {
		tt::TdmLevel *level = tt::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		drive_predicted(
				*level, "vehicles/growleritv/growleritv", "Growler", "host.vehicles.predict",
				[](uint32_t p_t) { return p_t < 60u ? 0.0f : ((p_t / 90u) % 2u == 0u ? 1.0f : -1.0f); }, 20.0f);
	}
}

TEST_SUITE("host-vehicles-predict-tank") {
	TEST_CASE("vehicles: a client 100 ms from the server drives an M1A2 on MP_007 ConquestSmall0 through left and "
			  "right turns; with each track side's steer term on the wire the drawn own tank at the predicted tick "
			  "is where the server puts it") {
		tt::TdmLevel *level = tt::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		CHECK(drive_predicted(
					  *level, "vehicles/m1a2/m1abrams", "M1A2", "host.vehicles.predict.tank",
					  [](uint32_t p_t) { return p_t < 240u ? ((p_t / 60u) % 2u == 0u ? 1.0f : -1.0f) : 0.0f; },
					  10.0f) > 0u);
	}
}

TEST_SUITE("host-vehicles-blend-unit") {
	TEST_CASE("vehicles: the own vehicle's correction fades over min(d, 0.25) s and snaps below 3 cm and above 3 m") {
		constexpr float kStep = 1.0f / 30.0f;
		host::VehicleBlend b;
		b.correct({ 0.10f, 0.0f, 0.0f });
		CHECK(b.shown()[0] == doctest::Approx(0.10f));
		for (const float left : { 2.0f / 3.0f, 1.0f / 3.0f, 0.0f, 0.0f }) {
			b.decay(kStep);
			CHECK(b.shown()[0] == doctest::Approx(0.10f * left));
		}
		b.correct({ 0.0f, 0.6f, 0.8f });
		b.decay(kStep);
		const float left = (0.25f - kStep) / 0.25f;
		CHECK(b.shown()[0] == doctest::Approx(0.0f));
		CHECK(b.shown()[1] == doctest::Approx(0.6f * left));
		CHECK(b.shown()[2] == doctest::Approx(0.8f * left));
		CHECK(core::vec::length(b.shown()) == doctest::Approx(0.8667f).epsilon(1e-4));
		const sim::Vec3 again = core::vec::add(b.shown(), { 0.05f, 0.0f, 0.0f });
		b.correct(again);
		CHECK(b.shown()[0] == doctest::Approx(again[0]));
		CHECK(b.shown()[1] == doctest::Approx(again[1]));
		CHECK(b.shown()[2] == doctest::Approx(again[2]));
		b.correct({ 4.0f, 0.0f, 0.0f });
		CHECK(core::vec::length(b.shown()) == 0.0f);
		b.correct({ 0.10f, 0.0f, 0.0f });
		b.correct({ 0.0f, 0.0f, 0.02f });
		CHECK(core::vec::length(b.shown()) == 0.0f);
	}
}

namespace {

std::optional<uint16_t> upright_spawner(const host::LevelVehicles &p_level, std::string_view p_blueprint) {
	for (size_t i = 0; i < p_level.spawners.size(); ++i) {
		if (p_level.kinds[p_level.spawners[i].kind].blueprint == p_blueprint &&
				tt::up_y_of(p_level.spawners[i].placement.rotation) > 0.95f) {
			return uint16_t(i);
		}
	}
	return std::nullopt;
}

std::unique_ptr<sim::VehicleWorld> server_vehicle_world(const tt::TdmLevel &p_level, uint16_t p_spawner) {
	const host::LevelVehicles &lv = *p_level.shared->vehicles;
	sim::VehicleWorldOptions options;
	options.max_vehicles = uint32_t(std::min<size_t>(lv.spawners.size(), sim::kMaxVehicleSlots));
	options.tuning = lv.tuning;
	core::Result<std::unique_ptr<sim::VehicleWorld>> server =
			sim::VehicleWorld::create(p_level.shared->static_world, options);
	REQUIRE(server.ok());
	REQUIRE((*server)
					->add_vehicle(sim::VehicleSlot(p_spawner), lv.kinds[lv.spawners[p_spawner].kind].model,
							lv.spawners[p_spawner].placement)
					.ok());
	return std::move(*server);
}

void receive_vehicle_state(host::ClientVehicles &r_seen, const host::LevelVehicles &p_level, uint16_t p_spawner,
		uint8_t p_driver, sim::Tick p_tick, const sim::VehicleState &p_state, uint8_t p_condition) {
	net::VehicleStates m;
	m.server_tick = p_tick;
	net::VehicleWire w;
	w.spawner = p_spawner;
	w.generation = 1;
	w.team = 1;
	w.seat_count = p_level.kinds[p_level.spawners[p_spawner].kind].seats;
	w.occupants[0] = p_driver;
	w.state = p_state;
	w.condition = p_condition;
	m.vehicles.push_back(w);
	r_seen.receive(net::encode_vehicle_states(m));
}

}

TEST_SUITE("host-vehicles-reconcile") {
	TEST_CASE("vehicles: a received Growler state shoved 0.5 m from the driver's prediction is replayed and faded "
			  "from where the vehicle was drawn over 0.25 s; one shoved 4 m snaps") {
		tt::TdmLevel *level = tt::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::LevelVehicles &lv = *level->shared->vehicles;
		const std::optional<uint16_t> spawner = upright_spawner(lv, "vehicles/growleritv/growleritv");
		REQUIRE(spawner.has_value());
		const std::unique_ptr<sim::VehicleWorld> server = server_vehicle_world(*level, *spawner);
		const sim::VehicleSlot slot(*spawner);
		host::ClientVehiclePrediction prediction;
		REQUIRE(prediction.init(*level->shared).ok());
		host::ClientVehicles seen;
		auto frame = std::make_unique<host::PresentationFrame>();

		constexpr uint8_t kOwnSlot = 3;
		constexpr sim::Tick kEntry = 120, kLag = 6, kShove = kEntry + 200, kSnap = kEntry + 320, kEnd = kEntry + 380;
		std::map<sim::Tick, sim::Command> sent;
		std::map<sim::Tick, sim::VehicleState> states;
		std::map<sim::Tick, sim::Vec3> drawn_at;
		float throttle_off = 0.0f, rising_throttle = 0.0f;
		uint32_t contact_off = 0;
		bool seated_before_entry = true, seated_at_entry = false;
		const host::CommandAt command_at = [&](sim::Tick p_tick) -> const sim::Command * {
			const auto c = sent.find(p_tick);
			return c != sent.end() ? &c->second : nullptr;
		};
		const auto shove = [&](float p_metres) {
			sim::VehicleState s = server->state(slot);
			s.body.com_position[0] += p_metres;
			REQUIRE(server->restore_vehicle_state(
								  slot, s, sim::vehicle_restore::kBody, sim::ContactCachePolicy::Invalidate)
							.ok());
		};
		for (sim::Tick c = 1; c <= kEnd; ++c) {
			sim::Command cmd;
			cmd.tick = c;
			cmd.sequence = c;
			cmd.move_forward = c > kEntry ? sim::kAxisMax : int8_t(0);
			sent[c] = cmd;
			const sim::VehicleCommand drive = c > kEntry ? sim::vehicles::driver_command(cmd) : sim::VehicleCommand();
			REQUIRE(server->step(std::span(&slot, 1), std::span(&drive, 1)).ok());
			if (c == kShove) {
				shove(0.5f);
			}
			if (c == kSnap) {
				shove(4.0f);
			}
			states[c] = server->state(slot);
			if (c > kLag && (c - kLag) % 2u == 0u) {
				const sim::Tick at = c - kLag;
				receive_vehicle_state(
						seen, lv, *spawner, at >= kEntry ? kOwnSlot : net::kNoVehicleOccupant, at, states[at], 0);
			}
			prediction.reconcile(seen, kOwnSlot, command_at, c);
			if (c == kEntry + kLag) {
				seated_before_entry = prediction.seat_at(kEntry - 1u).has_value();
				seated_at_entry = prediction.seat_at(kEntry).has_value();
			}
			prediction.step(cmd);
			frame->render_ticks_at_publish = double(c);
			seen.publish(*frame);
			prediction.publish(*frame);
			for (uint8_t i = 0; i < frame->vehicle_count; ++i) {
				const host::PresentedVehicle &v = frame->vehicles[i];
				if (v.spawner == *spawner && v.predicted == 1 && v.tick == c) {
					drawn_at[c] = v.com_position;
					if (c < kShove) {
						const sim::VehicleState &at = states.at(c);
						throttle_off = std::max(throttle_off, std::fabs(v.throttle - at.driver.throttle_filter));
						rising_throttle = std::max(rising_throttle,
								std::fabs(at.driver.throttle_filter - states.at(c - kLag).driver.throttle_filter));
						uint8_t contact = 0;
						for (size_t w = 0; w < at.wheel_count && w < sim::kMaxVehicleWheels; ++w) {
							contact |= uint8_t((at.wheels[w].in_contact != 0 ? 1u : 0u) << w);
						}
						contact_off += v.wheel_contact != contact ? 1u : 0u;
					}
				}
			}
		}
		host::ClientStats stats;
		prediction.stats(stats);
		REQUIRE(drawn_at.size() >= size_t(kEnd - kEntry - kLag));
		const auto drawn = [&](sim::Tick p_tick) {
			const auto d = drawn_at.find(p_tick);
			REQUIRE(d != drawn_at.end());
			return d->second;
		};
		const auto off = [&](sim::Tick p_tick) {
			return core::vec::length(core::vec::sub(drawn(p_tick), states.at(p_tick).body.com_position));
		};
		float before = 0.0f;
		for (sim::Tick t = kEntry + 2u * kLag; t < kShove; ++t) {
			before = std::max(before, off(t));
		}
		const sim::Tick corrected = kShove + kLag;
		const float jump = core::vec::length(core::vec::sub(drawn(corrected), drawn(corrected - 1u)));
		const float moved = core::vec::length(
				core::vec::sub(states.at(corrected).body.com_position, states.at(corrected - 1u).body.com_position));
		std::array<float, 16> fade{};
		for (size_t k = 0; k < fade.size(); ++k) {
			fade[k] = off(corrected + sim::Tick(k));
		}
		const float snapped = off(kSnap + kLag);
		std::printf("host.vehicles.reconcile: the drawn Growler was %.4f m from the server before the shove; the "
					"0.5 m shove seen %u ticks later moved the drawn vehicle %.3f m in a tick (the server's moved "
					"%.3f m), drawn %.3f, %.3f, %.3f, %.3f m from the server 0, 5, 10 and 15 ticks on; the 4 m "
					"shove drawn %.4f m from it; %llu corrections (largest %.3f m), %llu failures\n",
				double(before), unsigned(kLag), double(jump), double(moved), double(fade[0]), double(fade[5]),
				double(fade[10]), double(fade[15]), double(snapped), (unsigned long long)stats.vehicle_corrections,
				double(stats.vehicle_max_correction_m), (unsigned long long)stats.vehicle_prediction_failures);
		std::printf(
				"host.vehicles.reconcile: the drawn Growler's throttle filter was at most %.4f from the server's "
				"at its tick (it rose by up to %.4f over the %u-tick lag), its wheel contacts differed on %u ticks\n",
				double(throttle_off), double(rising_throttle), unsigned(kLag), unsigned(contact_off));
		CHECK_FALSE(seated_before_entry);
		CHECK(seated_at_entry);
		CHECK(rising_throttle > 0.1f);
		CHECK(throttle_off < 1e-6f);
		CHECK(contact_off == 0u);
		CHECK(before < 0.01f);
		CHECK(fade[0] > 0.4f);
		CHECK(fade[0] < 0.5f);
		CHECK(jump < moved + 0.1f);
		for (size_t k = 1; k < fade.size(); ++k) {
			CHECK(fade[k] <= fade[k - 1] + 1e-4f);
		}
		CHECK(fade[15] < 0.001f);
		CHECK(snapped < 0.001f);
		CHECK(stats.vehicle_max_correction_m > 3.5f);
		CHECK(stats.vehicle_prediction_starts == 1u);
		CHECK(stats.vehicle_prediction_failures == 0u);
	}

	TEST_CASE("vehicles: the driver's client of a disabled M1A2 predicts it through the data's disabled input "
			  "modifiers, as the server drives it") {
		tt::TdmLevel *level = tt::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::LevelVehicles &lv = *level->shared->vehicles;
		const std::optional<uint16_t> spawner = upright_spawner(lv, "vehicles/m1a2/m1abrams");
		REQUIRE(spawner.has_value());
		const host::LevelVehicleKind &kind = lv.kinds[lv.spawners[*spawner].kind];
		REQUIRE_FALSE(kind.disabled_input.empty());
		const std::unique_ptr<sim::VehicleWorld> server = server_vehicle_world(*level, *spawner);
		const sim::VehicleSlot slot(*spawner);
		host::ClientVehiclePrediction prediction;
		REQUIRE(prediction.init(*level->shared).ok());
		host::ClientVehicles seen;
		auto frame = std::make_unique<host::PresentationFrame>();

		constexpr uint8_t kOwnSlot = 3;
		constexpr uint8_t kDisabled = uint8_t(sim::vehicles::VehicleCondition::Disabled);
		constexpr sim::Tick kEntry = 120, kLag = 6, kEnd = kEntry + 300;
		std::map<sim::Tick, sim::Command> sent;
		std::map<sim::Tick, sim::VehicleState> states;
		float worst = 0.0f;
		const host::CommandAt command_at = [&](sim::Tick p_tick) -> const sim::Command * {
			const auto c = sent.find(p_tick);
			return c != sent.end() ? &c->second : nullptr;
		};
		for (sim::Tick c = 1; c <= kEnd; ++c) {
			sim::Command cmd;
			cmd.tick = c;
			cmd.sequence = c;
			cmd.move_forward = c > kEntry ? sim::kAxisMax : int8_t(0);
			cmd.move_right = c > kEntry + 60u ? sim::kAxisMax : int8_t(0);
			sent[c] = cmd;
			const sim::VehicleCommand drive = c > kEntry
					? host::disabled_command(sim::vehicles::driver_command(cmd), kind.disabled_input)
					: sim::VehicleCommand();
			REQUIRE(server->step(std::span(&slot, 1), std::span(&drive, 1)).ok());
			states[c] = server->state(slot);
			if (c > kLag && (c - kLag) % 2u == 0u) {
				const sim::Tick at = c - kLag;
				receive_vehicle_state(seen, lv, *spawner, at >= kEntry ? kOwnSlot : net::kNoVehicleOccupant, at,
						states[at], kDisabled);
			}
			prediction.reconcile(seen, kOwnSlot, command_at, c);
			prediction.step(cmd);
			frame->render_ticks_at_publish = double(c);
			seen.publish(*frame);
			prediction.publish(*frame);
			for (uint8_t i = 0; i < frame->vehicle_count; ++i) {
				const host::PresentedVehicle &v = frame->vehicles[i];
				if (v.spawner == *spawner && v.predicted == 1 && v.tick == c && c >= kEntry + 2u * kLag) {
					worst = std::max(
							worst, core::vec::length(core::vec::sub(v.com_position, states.at(c).body.com_position)));
				}
			}
		}
		host::ClientStats stats;
		prediction.stats(stats);
		const float travelled =
				core::vec::horizontal_distance(states.at(kEntry).body.com_position, states.at(kEnd).body.com_position);
		std::printf("host.vehicles.reconcile: a disabled M1A2 driven %.1f m through its %zu disabled input modifiers "
					"is drawn at most %.4f m from the server; %llu corrections (largest %.3f m), %llu failures\n",
				double(travelled), kind.disabled_input.size(), double(worst),
				(unsigned long long)stats.vehicle_corrections, double(stats.vehicle_max_correction_m),
				(unsigned long long)stats.vehicle_prediction_failures);
		CHECK(travelled > 1.0f);
		CHECK(worst < 0.001f);
		CHECK(stats.vehicle_corrections <= 4u);
		CHECK(stats.vehicle_max_correction_m < 0.03f);
		CHECK(stats.vehicle_prediction_starts == 1u);
		CHECK(stats.vehicle_prediction_failures == 0u);
	}
}

TEST_SUITE("host-gadgets-guided-client") {
	TEST_CASE("gadgets: a client holding the FGM-148 on MP_007 ConquestSmall0 strips fire until its OwnLock is "
			  "locked, then predicts the one shot the server fires with the tank as its lock target, and its own "
			  "missile copy and a remote copy of that shot home on the tank") {
		namespace sw = sim::weapons;
		tt::TdmLevel *level = tt::load_conquest_level_once();
		if (level == nullptr) {
			return;
		}
		const host::GunplayLevel &gl = *level->shared->gunplay;
		const host::LevelVehicles &lv = *level->shared->vehicles;
		host::GameServerConfig cfg;
		cfg.level = level->shared;
		cfg.max_slots = 8;
		cfg.game_build = kStreamGameBuild;
		cfg.dev_commands = true;
		cfg.seed = 131;
		cfg.gunplay.mechanics_cfg = OPENBF3_MECHANICS_CFG;
		cfg.rules.mode = host::MatchRulesConfig::Mode::Tdm;
		cfg.rules.deploy_screen = true;
		cfg.rules.revive = true;
		cfg.rules.preround_override_s = 0.0f;
		cfg.rules.min_players_override = 1;
		host::GameServer server;
		const core::Status st = server.init(cfg);
		REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
		core::Result<std::unique_ptr<sim::CollisionWorld>> world =
				sim::CollisionWorld::create(level->shared->static_world);
		REQUIRE(world.ok());
		sim::SoldierMotor motor;
		REQUIRE(motor.init(**world, level->shared->motor).ok());

		const uint8_t engineer = uint8_t(host::KitRole::Engineer);
		const uint8_t launcher_slot = uint8_t(core::gameplay::WeaponSlot::Gadget1);
		Stream stream(server, level->shared);
		host::ClientSessionConfig cc;
		core::Result<net::NetSimConfig> ns = net::parse_netsim("lat=40-60,seed=3");
		REQUIRE(ns.ok());
		cc.netsim = *ns;
		cc.request_dev_commands = true;
		cc.record_predicted = true;
		StreamClient &gunner = stream.add("gunner", 1u, 0, engineer, cc);
		gunner.scripted = false;
		gunner.keep_events = true;
		const host::PresentationFrame &f = *gunner.frame;
		tt::VehicleWatch vehicles;
		const auto on_tick = [&](sim::Tick) { vehicles.take(stream.out(), gunner.peer); };
		const auto until = [&](const std::function<bool()> &p_done, uint32_t p_ticks) {
			for (uint32_t t = 0; t < p_ticks; ++t) {
				stream.run(1, on_tick);
				if (p_done()) {
					return true;
				}
			}
			return false;
		};
		const auto alive = [&] { return f.valid && (f.own_curr.flags & sim::soldier_flags::kAlive) != 0; };

		REQUIRE(until([&] { return f.serial != 0 && server.rules()->phase() == net::MatchPhase::InRound; }, 600));
		const uint8_t slot = f.stats.slot;
		const uint8_t team = server.rules()->team_of(slot);
		const host::LoadoutKit *kit = gl.loadouts.kit(team, engineer);
		REQUIRE(kit != nullptr);
		const auto item = std::find_if(kit->slots[launcher_slot].begin(), kit->slots[launcher_slot].end(),
				[](const host::LoadoutItem &i) { return i.unlock == "weapons/fgm148/u_fgm148"; });
		REQUIRE(item != kit->slots[launcher_slot].end());
		REQUIRE(item->def < gl.defs.size());
		const uint16_t javelin = item->def;
		net::LoadoutRequest request;
		request.kit = engineer;
		request.items[launcher_slot] = item->identifier;
		gunner.loop.send_loadout_request(request);
		stream.run(2, on_tick);
		REQUIRE(until(
				[&] {
					if (!alive() && server.current_tick() % 30u == 0u) {
						gunner.loop.send_spawn_request(engineer);
					}
					return alive();
				},
				900));
		REQUIRE(server.gunplay()->weapons(slot).slots[launcher_slot].def == javelin);

		std::optional<tt::LockStand> found;
		REQUIRE(until(
				[&] {
					found = tt::mbt_lock_stand(*level->shared, **world, motor, vehicles.latest, team);
					return found.has_value();
				},
				sim::seconds_to_ticks(tt::latest_initial_delay(lv)) + 600u));
		const uint16_t tank = found->spawner;
		const uint32_t tank_id = sim::vehicles::kVehicleTargetIdBase + uint32_t(tank);
		net::DebugRequest teleport;
		teleport.kind = net::DebugKind::Teleport;
		teleport.id = 1;
		teleport.position = found->stand;
		gunner.loop.send_debug(teleport);
		stream.run(30, on_tick);
		REQUIRE(alive());

		bool zoom = false, fire = false;
		const auto aim = [&](sim::Tick p_tick) {
			on_tick(p_tick);
			const host::ViewAim a = host::aim_at(host::present_own_eye(f), found->aim);
			gunner.input.yaw = a.yaw;
			gunner.input.pitch = a.pitch;
			gunner.input.held_buttons = tg::select_slot_field(launcher_slot) | (zoom ? sim::buttons::kZoom : 0u) |
					(fire ? sim::buttons::kFire : 0u);
		};
		const auto aimed_until = [&](const std::function<bool()> &p_done, uint32_t p_ticks) {
			for (uint32_t t = 0; t < p_ticks; ++t) {
				stream.run(1, aim);
				if (p_done()) {
					return true;
				}
			}
			return false;
		};
		const auto never = [] { return false; };
		aimed_until(never, 150);
		const uint64_t server_shots = server.gunplay()->stats().shots;
		const uint64_t predicted = f.stats.predicted_shots;
		fire = true;
		aimed_until(never, 60);
		fire = false;
		aimed_until(never, 10);
		std::printf("javelin client: unlocked trigger held 60 ticks: %llu commands stripped of fire, %llu predicted "
					"shots, %llu server shots\n",
				(unsigned long long)f.stats.lock_stripped_fire,
				(unsigned long long)(f.stats.predicted_shots - predicted),
				(unsigned long long)(server.gunplay()->stats().shots - server_shots));
		CHECK(f.stats.lock_stripped_fire > 0u);
		CHECK(f.stats.predicted_shots == predicted);
		CHECK(server.gunplay()->stats().shots == server_shots);
		CHECK_FALSE(f.own_lock.locked);

		zoom = true;
		REQUIRE(aimed_until([&] { return f.own_lock.locked; }, 600));
		CHECK(f.own_lock.target == tank);
		CHECK(server.gunplay()->lock_state(slot).target() == tank_id);
		const uint64_t launches = server.gunplay()->gadget_stats().guided_launches;
		const uint64_t stripped = f.stats.lock_stripped_fire;
		gunner.kept_shots.clear();
		fire = true;
		REQUIRE(aimed_until([&] { return f.stats.predicted_shots > predicted; }, 120));
		fire = false;
		aimed_until(never, 30);
		std::printf("javelin client: locked on spawner %u; the trigger gave %llu predicted shot(s), %llu server "
					"shot(s), %llu guided launch(es)\n",
				unsigned(tank), (unsigned long long)(f.stats.predicted_shots - predicted),
				(unsigned long long)(server.gunplay()->stats().shots - server_shots),
				(unsigned long long)(server.gunplay()->gadget_stats().guided_launches - launches));
		CHECK(f.stats.predicted_shots == predicted + 1u);
		CHECK(server.gunplay()->stats().shots == server_shots + 1u);
		CHECK(server.gunplay()->gadget_stats().guided_launches == launches + 1u);
		CHECK(f.stats.lock_stripped_fire == stripped);
		REQUIRE(gunner.kept_shots.size() == 1u);
		const sw::ShotEvent shot = gunner.kept_shots.front().shot;
		CHECK(shot.def == javelin);
		CHECK(shot.lock_target == tank_id);

		REQUIRE(aimed_until([&] { return f.stats.projectiles.live_own == 0u; }, 15 * 60));
		std::printf("javelin client: the own missile copy flew %llu homing ticks\n",
				(unsigned long long)f.stats.projectiles.own_homing_ticks);
		CHECK(f.stats.projectiles.own_homing_ticks > 0u);

		core::Result<std::shared_ptr<const host::ClientGunplayInputs>> inputs =
				host::client_gunplay_inputs(*level->shared);
		REQUIRE(inputs.ok());
		core::Result<std::unique_ptr<host::ClientProjectiles>> remote =
				host::ClientProjectiles::create((*inputs)->table, (*inputs)->capsules, 131);
		REQUIRE(remote.ok());
		const net::VehicleWire *wire = vehicles.of(tank);
		REQUIRE(wire != nullptr);
		host::LockGuidance guidance;
		const std::array<sw::LockCandidate, 1> candidates{ host::lock_candidate_of(lv.kinds[lv.spawners[tank].kind],
				tank, wire->team, wire->state.body.com_position, wire->state.body.rotation, false) };
		guidance.set(candidates);
		(*remote)->set_guidance(&guidance);
		(*remote)->queue_remote(net::remote_shot_of(shot));
		auto ring = std::make_unique<host::PresentationEvents>();
		std::vector<host::PresentEvent> drained(256);
		const double from = sw::units_to_seconds(shot.spawn_time) * double(sim::kSimHz);
		for (uint32_t t = 1; t <= 15u * 60u; ++t) {
			(*remote)->step_remote(from + double(t), **world, {}, *ring);
			while (ring->drain(drained) > 0) {
			}
		}
		const host::ClientProjectilesStats &rs = (*remote)->stats();
		std::printf("javelin client: the remote copy spawned %llu, flew %llu homing ticks, %u live at the end\n",
				(unsigned long long)rs.remote_spawned, (unsigned long long)rs.remote_homing_ticks,
				unsigned(rs.live_remote));
		CHECK(rs.remote_spawned == 1u);
		CHECK(rs.remote_homing_ticks > 0u);
		CHECK(rs.live_remote == 0u);
		stream.finish();
	}
}
