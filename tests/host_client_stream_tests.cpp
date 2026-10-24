#include <doctest/doctest.h>

#include "bf3/host/bot_fill.h"
#include "bf3/host/client_session.h"
#include "bf3/host/client_wiring.h"
#include "bf3/host/game_server.h"
#include "bf3/host/tdm_brain.h"
#include "bf3/host/tdm_rules.h"
#include "bf3/net/bit_stream.h"
#include "bf3/net/protocol.h"
#include "bf3/net/weapons_wire.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"
#include "client_loop.h"
#include "client_transport.h"

#include "tdm_fixture.h"
#include "test_support.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace bf3;
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

constexpr StreamHashes kAlpha{ 0x997be4eb46e873b7ull, 0xd32a137f772aa841ull, 0x56b2a90f6ce9e729ull, 0x6da40cd818d49832ull, 0x2ad4694744e40099ull, 0x1c0b49d107eae3b4ull };
constexpr StreamHashes kBravo{ 0x58da0a22862dd690ull, 0x25be809b43497154ull, 0x1593dee0ad1746a8ull, 0x14024ea242230040ull, 0x75b00bc5acc42981ull, 0xd2d56f3d67218236ull };
constexpr StreamHashes kCharlie{ 0xe725154fbc396869ull, 0x0d042d562bfe5336ull, 0x0d042d562bfe5336ull, 0x0d042d562bfe5336ull, 0x0d042d562bfe5336ull, 0x7018103a93ee23afull };
constexpr StreamHashes kDelta{ 0xda50c24242a75360ull, 0xba16cfbce5642c09ull, 0x1696f56b23947cb4ull, 0x0a8f15746ef4ed4dull, 0x6c2c0edf064c5098ull, 0xc899b236d3b4444cull };

constexpr uint32_t kTdmTicks = 1500;
constexpr uint32_t kDevTicks = 600;
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

	StreamClient &add(const char *p_name, uint32_t p_peer, uint32_t p_phase, uint8_t p_kit, host::ClientSessionConfig p_config) {
		auto c = std::make_unique<StreamClient>();
		c->name = p_name;
		c->peer = p_peer;
		c->phase = p_phase;
		c->kit = p_kit;
		p_config.port = 1;
		p_config.name = p_name;
		p_config.mechanics_cfg = OPENBF3_MECHANICS_CFG;
		const core::Status init = c->loop.init(p_config, level_);
		REQUIRE_MESSAGE(init.ok(), (init.ok() ? std::string() : init.error().message));
		server_.on_connect(p_peer);
		c->loop.begin(std::make_unique<ServerTransport>(server_, *c), clock_);
		clients_.push_back(std::move(c));
		return *clients_.back();
	}

	void play(uint32_t p_ticks, const std::function<void(sim::Tick)> &p_on_tick) {
		for (uint32_t step = 0; step < p_ticks * kStepsPerTick; ++step) {
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
		CHECK(cs.refused);
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
		cfg.dev_commands = true;
		cfg.seed = 5;
		cfg.walkers = 44;
		cfg.respawn_ticks = 45;
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

	TEST_CASE("host-client-stream: install unchanged") {
		tt::TdmLevel *level = tt::load_tdm_level_once();
		if (level != nullptr) {
			tt::check_install_unchanged(*level);
		}
	}
}
