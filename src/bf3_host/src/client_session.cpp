// ClientSession: the M1 client on its own thread (docs/design/m1.md §7.5, §9; client_session.h has the per-tick
// order). Godot-free: ENet client, netsim both ways, the command clock, prediction and reconciliation through
// sim::catch_up, the others' interpolation buffer, and the double-buffered PresentationFrame.
//
// The visual error smoothing follows IW4L's predicted_error.rs (ERROR_DECAY_MS 100, snap below 0.1 IW units),
// the user's own Apache-2.0 code; the clock reset idea follows IW4L's frame_clock.rs (docs §7.5, §9.3).
#include "bf3/host/client_session.h"

#include "instance_registry.h"

#include "bf3/net/enet_host.h"
#include "bf3/net/loopback_ping.h"
#include "bf3/sim/collision_world.h"
#include "bf3/sim/soldier_motor.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace bf3::host {

namespace {

// H hygiene: pins instances against their destructor while stop_all is inside them (instance_registry.h).
detail::InstanceRegistry<ClientSession> g_registry;

constexpr double kNsPerTick = 1e9 / double(sim::kSimHz);
constexpr double kMsPerTick = 1000.0 / double(sim::kSimHz);
// A client thread that fell more than this many ticks behind its clock (a stall) jumps to the clock instead of
// predicting every missed tick; the base rule then fills the gap through catch_up (docs §9.2).
constexpr uint32_t kMaxBurstTicks = 8;
constexpr size_t kTrackSamples = 8; // snapshot samples kept per other soldier
constexpr size_t kSentTimes = 1024; // send times kept for the RTT echo
constexpr uint64_t kTrackTimeoutNs = 1'000'000'000ull;

float length3(const sim::Vec3 &p_v) {
	return std::sqrt(p_v[0] * p_v[0] + p_v[1] * p_v[1] + p_v[2] * p_v[2]);
}

sim::Vec3 sub3(const sim::Vec3 &p_a, const sim::Vec3 &p_b) {
	return { p_a[0] - p_b[0], p_a[1] - p_b[1], p_a[2] - p_b[2] };
}

void set_error(ClientStats &r_stats, const std::string &p_text) {
	const size_t n = std::min(p_text.size(), r_stats.last_error.size() - 1);
	std::memcpy(r_stats.last_error.data(), p_text.data(), n);
	r_stats.last_error[n] = '\0';
}

// Equal within the reconcile epsilons (docs §9.2): positions and velocities within epsilon, every discrete field
// equal, the floats that are not positions within 1e-3.
bool states_match(const sim::SoldierState &p_a, const sim::SoldierState &p_b, float p_pos_eps, float p_vel_eps) {
	if (p_a.tick != p_b.tick || p_a.life != p_b.life || p_a.yaw != p_b.yaw || p_a.pitch != p_b.pitch || p_a.pose != p_b.pose ||
			p_a.target_pose != p_b.target_pose || p_a.transition_ticks != p_b.transition_ticks || p_a.transition_total != p_b.transition_total ||
			p_a.mode != p_b.mode || p_a.flags != p_b.flags || p_a.events != p_b.events || p_a.jump_penalty_ticks != p_b.jump_penalty_ticks ||
			p_a.air_ticks != p_b.air_ticks || p_a.protect_ticks != p_b.protect_ticks) {
		return false;
	}
	if (!(length3(sub3(p_a.position, p_b.position)) <= p_pos_eps) || !(length3(sub3(p_a.velocity, p_b.velocity)) <= p_vel_eps)) {
		return false;
	}
	return std::fabs(p_a.health - p_b.health) <= 1e-3f && std::fabs(p_a.impact_speed - p_b.impact_speed) <= 1e-3f &&
			std::fabs(p_a.damage - p_b.damage) <= 1e-3f;
}

float lerp(float p_a, float p_b, float p_t) {
	return p_a + (p_b - p_a) * p_t;
}

// Cubic Hermite between p0 (tangent m0) and p1 (tangent m1), u in [0, 1].
float hermite(float p_p0, float p_m0, float p_p1, float p_m1, float p_u) {
	const float u2 = p_u * p_u, u3 = u2 * p_u;
	return (2 * u3 - 3 * u2 + 1) * p_p0 + (u3 - 2 * u2 + p_u) * p_m0 + (-2 * u3 + 3 * u2) * p_p1 + (u3 - u2) * p_m1;
}

float eye_of(const PresentationFrame &p_frame, const net::OtherSoldier &p_o) {
	const size_t from = std::min<size_t>(uint8_t(p_o.pose), sim::kPoseCount - 1);
	const size_t to = std::min<size_t>(uint8_t(p_o.target_pose), sim::kPoseCount - 1);
	return lerp(p_frame.pose_eye_heights[from], p_frame.pose_eye_heights[to], float(p_o.transition) / 255.0f);
}

PresentedSoldier present_sample(const PresentationFrame &p_frame, const net::OtherSoldier &p_o) {
	PresentedSoldier s;
	s.slot = p_o.slot;
	s.generation = p_o.generation;
	s.position = p_o.position;
	s.velocity = p_o.velocity;
	s.yaw = sim::yaw_radians(p_o.yaw);
	s.pitch = sim::pitch_radians(p_o.pitch);
	s.pose = p_o.pose;
	s.eye_height = eye_of(p_frame, p_o);
	s.mode = p_o.mode;
	s.flags = p_o.flags;
	return s;
}

} // namespace

size_t present_others(const PresentationFrame &p_frame, uint64_t p_now_ns, std::span<PresentedSoldier> r_out, uint64_t *r_extrapolated) {
	const double dt_ns = p_now_ns >= p_frame.publish_ns ? double(p_now_ns - p_frame.publish_ns) : -double(p_frame.publish_ns - p_now_ns);
	const double render = p_frame.render_ticks_at_publish + dt_ns * p_frame.ticks_per_ns;
	const double max_extra_ticks = std::max(0.0, double(p_frame.extrapolate_max_ms)) / kMsPerTick;
	size_t n = 0;
	const uint32_t count = std::min<uint32_t>(p_frame.other_count, uint32_t(p_frame.others.size()));
	for (uint32_t i = 0; i < count && n < r_out.size(); ++i) {
		const OtherTrack &t = p_frame.others[i];
		const uint32_t used = std::min<uint32_t>(t.count, uint32_t(kOtherSamples));
		if (used == 0) {
			continue;
		}
		// The bracketing pair: the newest sample at or before the render time and the next one.
		uint32_t a = 0;
		while (a + 1 < used && double(t.samples[a + 1].server_tick) <= render) {
			++a;
		}
		const OtherSample &sa = t.samples[a];
		PresentedSoldier out;
		if (render <= double(sa.server_tick)) {
			out = present_sample(p_frame, sa.soldier); // before the oldest sample: hold it
		} else if (a + 1 < used) {
			const OtherSample &sb = t.samples[a + 1];
			const double span = double(sb.server_tick - sa.server_tick);
			const float u = float(std::clamp((render - double(sa.server_tick)) / span, 0.0, 1.0));
			const float dt = float(span) * sim::kTickSeconds;
			out = present_sample(p_frame, sa.soldier);
			for (size_t k = 0; k < 3; ++k) {
				out.position[k] = hermite(sa.soldier.position[k], sa.soldier.velocity[k] * dt, sb.soldier.position[k], sb.soldier.velocity[k] * dt, u);
				out.velocity[k] = lerp(sa.soldier.velocity[k], sb.soldier.velocity[k], u);
			}
			const int16_t dyaw = int16_t(uint16_t(sb.soldier.yaw - sa.soldier.yaw)); // shortest arc
			out.yaw = sim::yaw_radians(uint16_t(sa.soldier.yaw + int32_t(std::lround(float(dyaw) * u))));
			out.pitch = lerp(sim::pitch_radians(sa.soldier.pitch), sim::pitch_radians(sb.soldier.pitch), u);
			const net::OtherSoldier &discrete = u >= 1.0f ? sb.soldier : sa.soldier; // pose and mode switch at the newer sample
			out.pose = discrete.pose;
			out.mode = discrete.mode;
			out.flags = discrete.flags;
			out.generation = discrete.generation;
			out.eye_height = lerp(eye_of(p_frame, sa.soldier), eye_of(p_frame, sb.soldier), u);
		} else {
			// Past the newest sample: extrapolate along its velocity for at most extrapolate_max_ms, then hold.
			const double ahead = std::min(render - double(sa.server_tick), max_extra_ticks);
			const float secs = float(ahead) * sim::kTickSeconds;
			out = present_sample(p_frame, sa.soldier);
			for (size_t k = 0; k < 3; ++k) {
				out.position[k] = sa.soldier.position[k] + sa.soldier.velocity[k] * secs;
			}
			out.extrapolated = true;
			if (r_extrapolated != nullptr) {
				++*r_extrapolated;
			}
		}
		r_out[n++] = out;
	}
	return n;
}

double present_own_fraction(const PresentationFrame &p_frame, uint64_t p_now_ns) {
	const double dt_ns = p_now_ns >= p_frame.publish_ns ? double(p_now_ns - p_frame.publish_ns) : 0.0;
	return std::clamp(p_frame.tick_fraction + dt_ns * p_frame.own_ticks_per_ns, 0.0, 1.0);
}

struct ClientSession::Impl {
	struct Track {
		bool used = false;
		uint8_t generation = 0;
		uint64_t last_seen_ns = 0;
		std::deque<OtherSample> samples; // by server tick, oldest first
	};
	struct SentTime {
		uint32_t sequence = 0;
		uint64_t ns = 0;
	};
	struct RttSample {
		uint64_t ns = 0;
		double ms = 0;
	};

	ClientSessionConfig config;
	std::shared_ptr<const SharedLevel> level;
	std::unique_ptr<sim::CollisionWorld> world;
	sim::SoldierMotor motor;
	std::unique_ptr<sim::PredictionRing> ring;
	uint64_t motor_hash = 0;
	std::unique_ptr<net::Host> host;
	uint32_t peer = 0;
	net::NetSim out;
	net::NetSim in;

	std::thread thread;
	std::mutex lifecycle_mutex;
	bool started = false; // one connection per ClientSession (the client-thread state below is not reset)
	std::atomic<bool> stop_requested{ false };
	std::atomic<bool> running{ false };

	// Handoffs with other threads.
	mutable std::mutex input_mutex;
	InputSample input;
	mutable std::mutex frame_mutex;
	std::unique_ptr<PresentationFrame> shared_frame = std::make_unique<PresentationFrame>();
	std::mutex debug_mutex;
	std::deque<net::DebugRequest> debug_out;
	std::deque<net::DebugReport> reports;
	std::atomic<int32_t> shift_request{ 0 };
	std::atomic<uint32_t> stall_request_ms{ 0 };

	// ---- client thread only ----
	std::unique_ptr<PresentationFrame> build = std::make_unique<PresentationFrame>();
	ClientStats stats;
	uint64_t serial = 0;
	bool connected = true;
	bool have_match = false;
	uint8_t slot = 0;
	uint8_t divisor = 2;
	uint64_t hello_ns = 0;
	double hello_rtt_ms = 0;
	// The command clock: the tick being commanded at time t is base_ticks + (t - base_ns) * rate.
	bool clock_running = false;
	double clock_base_ticks = 0;
	uint64_t clock_base_ns = 0;
	double rate_scale = 1.0;
	sim::Tick next_tick = 0; // C: the tick about to be predicted
	bool holdoff = true;
	uint32_t holdoff_sequence = 1;
	std::deque<int32_t> reports64;
	std::deque<int32_t> reports8;
	uint32_t target = 2;
	uint32_t candidate = 2;
	uint64_t candidate_since_ns = 0;
	std::deque<RttSample> rtt_window;
	bool rtt_valid = false;
	// The newest authoritative tick and when it arrived (the clock estimate).
	sim::Tick anchor_tick = 0;
	uint64_t anchor_ns = 0;
	// Commands
	uint32_t next_sequence = 1;
	std::deque<sim::Command> history; // the last kCommandsPerPacket commands made and sent, newest last
	std::array<SentTime, kSentTimes> sent_times{};
	sim::Command last_made;
	bool jump_seen = false;
	uint32_t last_jump_presses = 0;
	sim::Pose keep_pose = sim::Pose::Stand;
	// Own soldier
	bool have_own = false;
	uint32_t own_life = 0;
	sim::SoldierState spawn_state;
	bool have_auth = false;
	sim::SoldierState last_auth;
	sim::Vec3 visual_offset{};
	uint32_t events_acc = 0;
	uint16_t server_events_acc = 0;
	// Snapshots
	bool have_snapshot = false;
	uint16_t newest_seq = 0;
	uint32_t ack_bits = 0;
	bool pending_snapshot_valid = false;
	net::Snapshot pending_snapshot;
	std::vector<sim::SoldierState> pending_spawned;
	// Others: the received server time and the interpolation delay (docs §9.4)
	std::array<Track, net::kMaxSlots> tracks;
	bool recv_valid = false;
	double recv_offset = 0; // received server ticks - local ticks (filtered)
	double recv_rate = 0; // the filter's slew this tick (-0.05..0.05)
	std::deque<std::pair<uint64_t, double>> offsets; // (arrival ns, server tick - local ticks) over the last second
	uint64_t last_arrival_ns = 0;
	double interval_mean_ms = 0;
	double interval_mad_ms = 0;
	uint64_t last_tick_ns = 0;

	static double local_ticks(uint64_t p_ns) { return double(p_ns) / kNsPerTick; }

	double clock_ticks(uint64_t p_now) const {
		const double dt = p_now >= clock_base_ns ? double(p_now - clock_base_ns) : -double(clock_base_ns - p_now);
		return clock_base_ticks + dt / kNsPerTick * rate_scale;
	}
	void set_clock(double p_ticks, uint64_t p_now) {
		clock_base_ticks = p_ticks;
		clock_base_ns = p_now;
	}
	void set_rate(double p_scale, uint64_t p_now) {
		clock_base_ticks = clock_ticks(p_now);
		clock_base_ns = p_now;
		rate_scale = p_scale;
	}
	uint64_t tick_deadline_ns(sim::Tick p_tick) const {
		const double dt_ticks = double(p_tick) - clock_base_ticks;
		const double ns = double(clock_base_ns) + dt_ticks * kNsPerTick / rate_scale;
		return ns <= 0.0 ? 0u : uint64_t(ns);
	}
	double rtt_for_estimate_ms() const { return rtt_valid ? stats.rtt_min_ms : hello_rtt_ms; }
	// The clock estimate (docs §7.5): the newest server tick + the time since it arrived + RTT + the target buffer.
	double estimate(uint64_t p_now) const {
		const double since = p_now >= anchor_ns ? double(p_now - anchor_ns) / kNsPerTick : 0.0;
		return double(anchor_tick) + since + rtt_for_estimate_ms() / kMsPerTick + double(target);
	}

	void begin_holdoff() {
		holdoff = true;
		holdoff_sequence = next_sequence; // the first command sent after the reset
		reports8.clear();
		reports64.clear();
	}

	void reset_clock(uint64_t p_now) {
		const double e = estimate(p_now);
		const sim::Tick c = sim::Tick(std::max(0.0, std::floor(e)));
		if (c < next_tick) {
			ring->discard_after(c - 1u); // a backward reset re-predicts the ticks it sends again
		}
		rate_scale = 1.0;
		set_clock(e, p_now);
		next_tick = c;
		++stats.clock_resets;
		begin_holdoff();
	}

	// ---- sending ------------------------------------------------------------------------------------------------

	void submit(uint8_t p_channel, std::vector<uint8_t> p_bytes, uint64_t p_now) {
		if (p_bytes.empty()) {
			return;
		}
		net::NetSim::Datagram d;
		d.peer = peer;
		d.channel = p_channel;
		d.reliable = p_channel != net::kChannelUnreliable;
		d.bytes = std::move(p_bytes);
		out.submit(std::move(d), p_now);
	}

	void release_out(uint64_t p_now) {
		bool sent = false;
		net::NetSim::Datagram d;
		while (out.poll(p_now, d)) {
			if (host != nullptr && connected) {
				host->send(d.peer, d.channel, d.bytes.data(), d.bytes.size(), d.reliable ? net::Host::SendMode::Reliable : net::Host::SendMode::Unsequenced);
				sent = true;
			}
		}
		if (sent) {
			host->flush();
		}
	}

	// ---- receiving ----------------------------------------------------------------------------------------------

	void rtt_sample(const net::Snapshot &p_snap, uint64_t p_now) {
		if (next_sequence <= 1) {
			return;
		}
		const uint32_t newest = next_sequence - 1u;
		uint32_t seq = (newest & ~0xFFFFu) | p_snap.sequence_echo;
		if (seq > newest) {
			if (seq < 0x10000u) {
				return;
			}
			seq -= 0x10000u;
		}
		const SentTime &t = sent_times[seq % kSentTimes];
		if (t.sequence != seq || t.ns == 0 || seq == 0) {
			return;
		}
		const uint64_t hold_ns = uint64_t(p_snap.echo_hold_100us) * 100000u;
		const double ms = p_now > t.ns + hold_ns ? double(p_now - t.ns - hold_ns) / 1e6 : 0.0;
		stats.rtt_ms = rtt_valid ? stats.rtt_ms + (ms - stats.rtt_ms) * 0.1 : ms;
		rtt_window.push_back({ p_now, ms });
		while (!rtt_window.empty() && p_now - rtt_window.front().ns > 2'000'000'000ull) {
			rtt_window.pop_front();
		}
		double lo = ms;
		for (const RttSample &s : rtt_window) {
			lo = std::min(lo, s.ms);
		}
		stats.rtt_min_ms = lo;
		rtt_valid = true;
	}

	void received_time_sample(sim::Tick p_tick, uint64_t p_now) {
		const double o = double(p_tick) - local_ticks(p_now);
		offsets.push_back({ p_now, o });
		while (!offsets.empty() && p_now - offsets.front().first > 1'000'000'000ull) {
			offsets.pop_front();
		}
		if (!recv_valid) {
			recv_offset = o;
			recv_valid = true;
		}
		if (last_arrival_ns != 0) {
			const double interval = double(p_now - last_arrival_ns) / 1e6;
			if (interval_mean_ms == 0.0) {
				interval_mean_ms = interval;
			}
			interval_mean_ms += (interval - interval_mean_ms) / 16.0;
			interval_mad_ms += (std::fabs(interval - interval_mean_ms) - interval_mad_ms) / 16.0;
		}
		last_arrival_ns = p_now;
	}

	void add_others(const net::Snapshot &p_snap, uint64_t p_now) {
		for (const net::OtherSoldier &o : p_snap.others) {
			if (o.slot >= tracks.size() || o.slot == slot) {
				continue;
			}
			Track &t = tracks[o.slot];
			if (!t.used || t.generation != o.generation) {
				t = Track();
				t.used = true;
				t.generation = o.generation;
			}
			t.last_seen_ns = p_now;
			OtherSample s;
			s.server_tick = p_snap.server_tick;
			s.soldier = o;
			auto at = std::lower_bound(t.samples.begin(), t.samples.end(), s.server_tick,
					[](const OtherSample &a, sim::Tick b) { return a.server_tick < b; });
			if (at != t.samples.end() && at->server_tick == s.server_tick) {
				continue; // a duplicate
			}
			t.samples.insert(at, s);
			while (t.samples.size() > kTrackSamples) {
				t.samples.pop_front();
			}
		}
	}

	void handle_datagram(const net::NetSim::Datagram &p_d, uint64_t p_now) {
		const std::optional<net::MessageType> type = net::peek_type(p_d.bytes);
		if (!type) {
			return;
		}
		switch (*type) {
			case net::MessageType::MatchInfo: {
				core::Result<net::MatchInfo> m = net::decode_match_info(p_d.bytes);
				if (!m || have_match) {
					return;
				}
				have_match = true;
				slot = m->slot;
				stats.slot = m->slot;
				divisor = std::max<uint8_t>(m->snapshot_divisor, 1);
				if (m->motor_hash != motor_hash) {
					++stats.motor_hash_mismatch;
				}
				hello_rtt_ms = hello_ns != 0 && p_now > hello_ns ? double(p_now - hello_ns) / 1e6 : 0.0;
				anchor_tick = m->server_tick;
				anchor_ns = p_now;
				set_clock(estimate(p_now) + double(config.initial_clock_offset_ticks), p_now);
				next_tick = sim::Tick(std::max(0.0, std::floor(clock_ticks(p_now))));
				clock_running = true;
				last_tick_ns = p_now;
				begin_holdoff();
				return;
			}
			case net::MessageType::Spawned: {
				core::Result<net::Spawned> s = net::decode_spawned(p_d.bytes);
				if (s && pending_spawned.size() < 16) {
					if (s->state.tick > anchor_tick) {
						anchor_tick = s->state.tick;
						anchor_ns = p_now;
					}
					pending_spawned.push_back(s->state);
				}
				return;
			}
			case net::MessageType::Snapshot: {
				core::Result<net::Snapshot> s = net::decode_snapshot(p_d.bytes);
				if (!s) {
					return;
				}
				++stats.snapshots_received;
				server_events_acc = uint16_t(server_events_acc | s->own_events);
				rtt_sample(*s, p_now);
				if (have_snapshot && !net::seq_newer(s->seq, newest_seq)) {
					if (s->seq != newest_seq) {
						++stats.snapshots_late;
						const uint16_t back = uint16_t(newest_seq - s->seq);
						if (back >= 1 && back <= 32) {
							ack_bits |= 1u << (back - 1u);
						}
						add_others(*s, p_now);
					}
					return;
				}
				if (have_snapshot) {
					const uint16_t gap = uint16_t(s->seq - newest_seq);
					stats.snapshots_lost += gap > 1 ? uint64_t(gap - 1u) : 0u;
					ack_bits = gap >= 32 ? 0u : (ack_bits << gap) | (1u << (gap - 1u));
				}
				have_snapshot = true;
				newest_seq = s->seq;
				if (s->server_tick >= anchor_tick) {
					anchor_tick = s->server_tick; // the clock estimate's anchor: the newest server tick and its arrival
					anchor_ns = p_now;
				}
				received_time_sample(s->server_tick, p_now);
				add_others(*s, p_now);
				pending_snapshot = std::move(*s);
				pending_snapshot_valid = true;
				return;
			}
			case net::MessageType::DebugReport: {
				core::Result<net::DebugReport> r = net::decode_debug_report(p_d.bytes);
				if (r) {
					std::lock_guard lock(debug_mutex);
					reports.push_back(std::move(*r));
					while (reports.size() > 64) {
						reports.pop_front(); // nobody is taking them
					}
				}
				return;
			}
			case net::MessageType::Kick: {
				core::Result<net::Kick> k = net::decode_kick(p_d.bytes);
				set_error(stats, "kicked: " + (k ? k->reason : std::string("(bad kick)")));
				connected = false;
				return;
			}
			case net::MessageType::Bye:
				set_error(stats, "the server said bye");
				connected = false;
				return;
			default:
				return;
		}
	}

	// ---- the clock (docs §7.5) --------------------------------------------------------------------------------

	void buffer_report(const net::Snapshot &p_snap, uint64_t p_now) {
		stats.command_buffer_ticks = p_snap.command_buffer_ticks;
		if (holdoff) {
			// Reports for commands sent before the last reset still describe the old clock.
			if (net::seq_newer(uint16_t(holdoff_sequence & 0xFFFFu), p_snap.sequence_echo) || next_sequence <= holdoff_sequence) {
				return;
			}
			holdoff = false;
		}
		const int32_t report = p_snap.command_buffer_ticks;
		reports64.push_back(report);
		while (reports64.size() > 64) {
			reports64.pop_front();
		}
		reports8.push_back(report);
		while (reports8.size() > 8) {
			reports8.pop_front();
		}
		// Target: target_buffer_ticks + max(0, ceil(2 MAD - 1)), adopted after target_hold_s.
		double mean = 0;
		for (int32_t r : reports64) {
			mean += r;
		}
		mean /= double(reports64.size());
		double mad = 0;
		for (int32_t r : reports64) {
			mad += std::fabs(double(r) - mean);
		}
		mad /= double(reports64.size());
		const double extra = std::max(0.0, std::ceil(2.0 * mad - 1.0));
		const uint32_t want = std::min<uint32_t>(config.target_buffer_ticks + uint32_t(extra), std::max(config.max_buffer_ticks, config.target_buffer_ticks));
		if (want != candidate) {
			candidate = want;
			candidate_since_ns = p_now;
		} else if (want != target && double(p_now - candidate_since_ns) >= double(config.target_hold_s) * 1e9) {
			target = want;
		}
		// Reset beyond clock_reset_ms.
		if (std::fabs(double(report) - double(target)) * kMsPerTick > double(config.clock_reset_ms)) {
			reset_clock(p_now);
			return;
		}
		// Proportional slew on the smoothed error, outside the deadband.
		double err = 0;
		for (int32_t r : reports8) {
			err += r;
		}
		err = err / double(reports8.size()) - double(target);
		double adjust = 0;
		if (std::fabs(err) > double(config.clock_deadband_ticks)) {
			adjust = -err / (double(sim::kSimHz) * std::max(0.01, double(config.clock_time_constant_s)));
			adjust = std::clamp(adjust, -double(config.max_clock_adjust), double(config.max_clock_adjust));
		}
		set_rate(1.0 + adjust, p_now);
	}

	// ---- reconciliation (docs §9.2) -----------------------------------------------------------------------------

	void run_catch_up(const sim::SoldierState &p_base) {
		const sim::CatchUpStats c = sim::catch_up(motor, p_base, *ring, next_tick);
		stats.replayed_ticks += c.steps;
	}

	void authoritative(const sim::SoldierState &p_s, uint64_t p_now) {
		if (have_own && p_s.life < own_life) {
			return; // an older life
		}
		const bool spawn = !have_own || p_s.life > own_life;
		if (!spawn && have_auth && p_s.tick <= last_auth.tick) {
			return; // not newer than what we have
		}
		// The prediction never runs at or behind an authoritative tick (startup, a stall, Spawned early).
		if (p_s.tick >= next_tick) {
			const double e = estimate(p_now);
			const sim::Tick c = std::max(sim::Tick(std::max(0.0, std::floor(e))), p_s.tick + 1u);
			rate_scale = 1.0;
			set_clock(std::max(e, double(c)), p_now);
			next_tick = c;
			++stats.forced_forward;
			++stats.clock_resets;
			begin_holdoff();
		}
		last_auth = p_s;
		have_auth = true;
		stats.last_server_tick = std::max(stats.last_server_tick, p_s.tick);
		if (spawn) {
			own_life = p_s.life;
			have_own = true;
			spawn_state = p_s;
			keep_pose = p_s.target_pose;
			// The server's last command at the spawn tick is the hold command (whatever the ring held for that
			// tick belonged to the old life), so the catch-up repeats it for ticks the client never sent.
			ring->put({ sim::hold_command(p_s), p_s, false });
			run_catch_up(p_s);
			++stats.spawns;
			stats.spawned = true;
			visual_offset = {};
			return;
		}
		const sim::PredictionEntry *e = ring->find(p_s.tick);
		if (e == nullptr) {
			++stats.ring_misses;
			run_catch_up(p_s);
			return;
		}
		if (states_match(e->state, p_s, config.position_epsilon_m, config.velocity_epsilon)) {
			if (config.debug_hash_compare && sim::soldier_state_hash(e->state) != sim::soldier_state_hash(p_s)) {
				++stats.hash_mismatches;
			}
			return;
		}
		++stats.reconciliations;
		stats.last_reconcile_tick = p_s.tick;
		stats.last_reconcile_ns = p_now;
		const sim::PredictionEntry *before = next_tick > 0 ? ring->find(next_tick - 1u) : nullptr;
		const sim::Vec3 old_pos = before != nullptr ? before->state.position : p_s.position;
		run_catch_up(p_s);
		const sim::PredictionEntry *after = next_tick > 0 ? ring->find(next_tick - 1u) : nullptr;
		const sim::Vec3 new_pos = after != nullptr ? after->state.position : p_s.position;
		const sim::Vec3 corr = sub3(old_pos, new_pos);
		const float m = length3(corr);
		if (m > 1e-3f) {
			++stats.corrections_over_1mm;
		}
		stats.max_correction_m = std::max(stats.max_correction_m, m);
		if (m > config.teleport_above_m) {
			++stats.teleports;
			visual_offset = {};
		} else {
			for (size_t k = 0; k < 3; ++k) {
				visual_offset[k] += corr[k];
			}
		}
	}

	// ---- one client tick (client_session.h steps 2-9) -----------------------------------------------------------

	sim::Command sample_command(sim::Tick p_tick) {
		InputSample s;
		{
			std::lock_guard lock(input_mutex);
			s = input;
		}
		sim::Command c;
		c.tick = p_tick;
		c.sequence = next_sequence++;
		const bool jumped = jump_seen && s.jump_presses != last_jump_presses;
		jump_seen = true;
		last_jump_presses = s.jump_presses;
		if (s.life < own_life) {
			// The mapper has not adopted this life's spawn view: command the hold command's content (docs §8.2).
			c.yaw = spawn_state.yaw;
			c.pitch = spawn_state.pitch;
			c.pose = spawn_state.target_pose;
			keep_pose = spawn_state.target_pose;
			return c;
		}
		if (s.free_camera) {
			c.yaw = last_made.yaw;
			c.pitch = last_made.pitch;
			c.pose = keep_pose;
			return c;
		}
		c.move_forward = sim::quantize_axis(s.move_forward);
		c.move_right = sim::quantize_axis(s.move_right);
		c.yaw = sim::quantize_yaw(s.yaw);
		c.pitch = sim::quantize_pitch(s.pitch);
		c.buttons = s.held_buttons & sim::buttons::kWireMask & ~sim::buttons::kEdgeMask;
		if (jumped) {
			c.buttons |= sim::buttons::kJump;
		}
		if (s.pose_request == PoseRequest::Set && uint8_t(s.pose) < sim::kPoseCount) {
			keep_pose = s.pose;
		}
		c.pose = keep_pose;
		return c;
	}

	void client_tick(uint64_t p_now) {
		// Visual offset decay (docs §9.3).
		const double dt_ms = last_tick_ns != 0 && p_now > last_tick_ns ? double(p_now - last_tick_ns) / 1e6 : kMsPerTick;
		last_tick_ns = p_now;
		const float decay = float(std::exp(-std::min(dt_ms, 1000.0) / std::max(1e-3, double(config.error_decay_ms))));
		for (float &v : visual_offset) {
			v *= decay;
		}
		if (length3(visual_offset) < config.snap_below_m) {
			visual_offset = {};
		}
		// 2-3. the newest snapshot: buffer report (slew / reset), then the own states in arrival order.
		if (pending_snapshot_valid && pending_snapshot.own.has_value() && have_own) {
			buffer_report(pending_snapshot, p_now);
		}
		for (const sim::SoldierState &s : pending_spawned) {
			authoritative(s, p_now);
		}
		pending_spawned.clear();
		if (pending_snapshot_valid) {
			if (pending_snapshot.own.has_value()) {
				authoritative(*pending_snapshot.own, p_now);
			}
			pending_snapshot_valid = false;
		}
		const sim::Tick c = next_tick;
		const sim::PredictionEntry *made = nullptr;
		sim::Command cmd;
		if (have_own && c > 0) {
			// 5. the base rule.
			if (ring->find(c - 1u) == nullptr) {
				const sim::PredictionEntry *b = ring->newest_at_or_below(c - 1u);
				const sim::SoldierState *base = (b != nullptr && b->state.life == own_life) ? &b->state : nullptr;
				if (have_auth && last_auth.life == own_life && last_auth.tick < c && (base == nullptr || last_auth.tick > base->tick)) {
					base = &last_auth;
				}
				if (base != nullptr) {
					const sim::SoldierState copy = *base;
					run_catch_up(copy);
					++stats.base_fills;
				}
			}
			// 6-7. sample and predict from ring[C - 1].
			const sim::PredictionEntry *base = ring->find(c - 1u);
			cmd = sample_command(c);
			if (base == nullptr || cmd.tick != base->state.tick + 1u) {
				++stats.step_precondition_failures;
				assert(false && "ClientSession: prediction base missing");
			} else {
				sim::SoldierState state = base->state;
				motor.step(state, cmd);
				events_acc |= state.events;
				ring->put({ cmd, state, true });
				made = ring->find(c);
				last_made = cmd;
				// 8. send the last kCommandsPerPacket commands of the send history.
				history.push_back(cmd);
				while (history.size() > net::kCommandsPerPacket) {
					history.pop_front();
				}
				sent_times[cmd.sequence % kSentTimes] = { cmd.sequence, p_now };
				net::CommandPacket p;
				p.newest_snapshot = newest_seq;
				p.snapshot_ack_bits = ack_bits;
				p.count = uint8_t(history.size());
				for (size_t i = 0; i < history.size(); ++i) {
					p.commands[i] = history[history.size() - 1 - i];
				}
				submit(net::kChannelUnreliable, net::encode_command_packet(p), p_now);
				++stats.commands_sent;
				release_out(p_now);
			}
		}
		next_tick = c + 1u;
		publish(p_now, made, cmd);
	}

	// ---- presentation (docs §9.4, §9.5) -------------------------------------------------------------------------

	void publish(uint64_t p_now, const sim::PredictionEntry *p_made, const sim::Command &p_cmd) {
		PresentationFrame &f = *build;
		f.serial = ++serial;
		f.publish_ns = p_now;
		f.own_ticks_per_ns = rate_scale / kNsPerTick;
		const sim::PredictionEntry *prev = p_made != nullptr && p_made->state.tick > 0 ? ring->find(p_made->state.tick - 1u) : nullptr;
		f.valid = p_made != nullptr && prev != nullptr;
		if (f.valid) {
			f.tick = p_made->state.tick;
			f.tick_fraction = std::clamp(clock_ticks(p_now) - double(f.tick), 0.0, 1.0);
			f.own_prev = prev->state;
			f.own_curr = p_made->state;
			f.own_command = p_cmd;
			f.eye_height_prev = motor.eye_height(prev->state);
			f.eye_height_curr = motor.eye_height(p_made->state);
		}
		f.visual_offset = visual_offset;
		f.server_own_valid = have_auth;
		f.server_own = last_auth;
		f.server_events = server_events_acc;
		server_events_acc = 0;
		f.events = events_acc;
		events_acc = 0;
		// The others' clock: received server time (filtered arrival offsets) minus the interpolation delay.
		recv_rate = 0;
		if (recv_valid && !offsets.empty()) {
			double lo = offsets.front().second;
			for (const auto &o : offsets) {
				lo = std::min(lo, o.second);
			}
			const double step = 0.05; // <= 5 % of a tick per tick
			const double diff = lo - recv_offset;
			const double moved = std::clamp(diff, -step, step);
			recv_offset += moved;
			recv_rate = moved / 1.0;
		}
		const double snapshot_interval_ms = double(divisor) * kMsPerTick;
		const double delay_ms =
				std::clamp(2.0 * snapshot_interval_ms + interval_mad_ms, double(config.interp_min_ms), double(std::max(config.interp_min_ms, config.interp_max_ms)));
		stats.interp_delay_ms = delay_ms;
		f.render_ticks_at_publish = local_ticks(p_now) + recv_offset - delay_ms / kMsPerTick;
		f.ticks_per_ns = (1.0 + std::clamp(recv_rate, -0.05, 0.05)) / kNsPerTick;
		f.extrapolate_max_ms = config.extrapolate_max_ms;
		for (size_t k = 0; k < sim::kPoseCount; ++k) {
			f.pose_eye_heights[k] = level->motor.poses[k].eye_height;
		}
		uint32_t n = 0;
		for (size_t i = 0; i < tracks.size(); ++i) {
			Track &t = tracks[i];
			if (!t.used) {
				continue;
			}
			if (p_now > t.last_seen_ns + kTrackTimeoutNs || t.samples.empty()) {
				t = Track();
				continue;
			}
			size_t first = 0;
			while (first + 1 < t.samples.size() && double(t.samples[first + 1].server_tick) <= f.render_ticks_at_publish) {
				++first;
			}
			OtherTrack &o = f.others[n++];
			o.slot = uint8_t(i);
			o.count = 0;
			for (size_t k = first; k < t.samples.size() && o.count < kOtherSamples; ++k) {
				o.samples[o.count++] = t.samples[k];
			}
		}
		f.other_count = n;
		stats.connected = connected;
		stats.client_tick = next_tick;
		stats.target_buffer_ticks = target;
		stats.clock_adjust = float(rate_scale - 1.0);
		stats.enet_rtt_ms = host != nullptr ? double(host->peer_rtt_ms(peer)) : 0.0;
		stats.netsim_out = out.stats();
		stats.netsim_in = in.stats();
		f.stats = stats;
		std::lock_guard lock(frame_mutex);
		copy_frame(*shared_frame, f);
	}

	// Copies the used part of p_from (every field, the first other_count tracks).
	static void copy_frame(PresentationFrame &r_to, const PresentationFrame &p_from) {
		constexpr size_t kOthersAt = offsetof(PresentationFrame, others);
		constexpr size_t kOthersEnd = kOthersAt + sizeof(PresentationFrame::others);
		static_assert(kOthersEnd <= sizeof(PresentationFrame));
		const uint32_t n = std::min<uint32_t>(p_from.other_count, uint32_t(p_from.others.size()));
		auto *to = reinterpret_cast<unsigned char *>(&r_to);
		const auto *from = reinterpret_cast<const unsigned char *>(&p_from);
		std::memcpy(to, from, kOthersAt);
		std::memcpy(to + kOthersAt, from + kOthersAt, size_t(n) * sizeof(OtherTrack));
		std::memcpy(to + kOthersEnd, from + kOthersEnd, sizeof(PresentationFrame) - kOthersEnd);
	}

	// ---- the thread ---------------------------------------------------------------------------------------------

	void run() {
		while (!stop_requested.load(std::memory_order_acquire)) {
			if (const uint32_t stall = stall_request_ms.exchange(0); stall > 0) {
				std::this_thread::sleep_for(std::chrono::milliseconds(stall));
			}
			if (const int32_t shift = shift_request.exchange(0); shift != 0 && clock_running) {
				// A disturbance, not a reset: the clock rules must notice and reset once (host.session.unit).
				const uint64_t now = net::monotonic_ns();
				set_clock(clock_ticks(now) + double(shift), now);
				next_tick = sim::Tick(int64_t(next_tick) + shift);
			}
			uint64_t now = net::monotonic_ns();
			uint64_t deadline = now + 50'000'000ull; // at most 50 ms between loops
			if (clock_running && connected) {
				deadline = std::min(deadline, tick_deadline_ns(next_tick));
			}
			if (const uint64_t r = in.next_release_ns(); r != 0) {
				deadline = std::min(deadline, r);
			}
			if (const uint64_t r = out.next_release_ns(); r != 0) {
				deadline = std::min(deadline, r);
			}
			const uint32_t wait_ms = deadline > now ? uint32_t(std::min<uint64_t>((deadline - now) / 1'000'000ull, 50)) : 0u;
			// Service ENet: wait for the first event up to the deadline, then drain.
			net::Host::Event ev;
			int got = host->service(wait_ms, ev);
			while (got > 0) {
				const uint64_t at = net::monotonic_ns();
				if (ev.type == net::Host::EventType::Receive) {
					net::NetSim::Datagram d;
					d.peer = ev.peer;
					d.channel = ev.channel;
					d.reliable = ev.channel != net::kChannelUnreliable;
					d.bytes = std::move(ev.data);
					in.submit(std::move(d), at);
				} else if (ev.type == net::Host::EventType::Disconnect) {
					connected = false;
					set_error(stats, "disconnected by the server");
				}
				got = host->service(0, ev);
			}
			if (got < 0) {
				connected = false;
				set_error(stats, "ENet service failed");
			}
			now = net::monotonic_ns();
			net::NetSim::Datagram d;
			while (in.poll(now, d)) {
				handle_datagram(d, now);
			}
			{
				std::deque<net::DebugRequest> requests;
				{
					std::lock_guard lock(debug_mutex);
					requests.swap(debug_out);
				}
				for (const net::DebugRequest &q : requests) {
					submit(net::kChannelReliable, net::encode_debug(q), now);
				}
			}
			release_out(now);
			if (clock_running && connected) {
				const double clock = clock_ticks(now);
				if (clock >= double(next_tick)) {
					if (clock - double(next_tick) > double(kMaxBurstTicks)) {
						next_tick = sim::Tick(std::floor(clock)); // a stall: jump, the base rule fills the gap
					}
					client_tick(now);
				}
			}
		}
		if (host != nullptr) {
			if (connected) {
				const std::vector<uint8_t> bye = net::encode_bye();
				host->send(peer, net::kChannelReliable, bye.data(), bye.size(), net::Host::SendMode::Reliable);
				host->flush();
			}
			host->disconnect_all();
		}
	}
};

ClientSession::ClientSession() :
		impl_(std::make_unique<Impl>()) {}

ClientSession::~ClientSession() {
	g_registry.retire(this); // first: a concurrent stop_all is done with this instance once it returns
	stop();
}

core::Status ClientSession::start(const ClientSessionConfig &p_config, std::shared_ptr<const SharedLevel> p_level) {
	std::lock_guard lock(impl_->lifecycle_mutex);
	if (impl_->thread.joinable() || impl_->started) {
		return core::make_error("ClientSession: already started (one connection per session)");
	}
	if (p_level == nullptr || p_level->static_world == nullptr) {
		return core::make_error("ClientSession: no level");
	}
	if (p_config.port == 0) {
		return core::make_error("ClientSession: no server port");
	}
	// A fresh state for this run (the handoff mutexes and the lifecycle mutex stay).
	Impl &m = *impl_;
	m.config = p_config;
	m.level = p_level;
	m.config.prediction_ring = std::clamp<uint32_t>(p_config.prediction_ring, 16u, 4096u);
	m.config.max_buffer_ticks = std::max(p_config.max_buffer_ticks, p_config.target_buffer_ticks);
	core::Result<std::unique_ptr<sim::CollisionWorld>> world = sim::CollisionWorld::create(p_level->static_world);
	if (!world) {
		return core::error_at("ClientSession", world.error());
	}
	m.world = std::move(*world);
	BF3_TRY(m.motor.init(*m.world, p_level->motor));
	m.motor_hash = sim::motor_params_hash(p_level->motor);
	m.ring = std::make_unique<sim::PredictionRing>(m.config.prediction_ring);
	m.out = net::NetSim(p_config.netsim);
	net::NetSimConfig in_config = p_config.netsim;
	in_config.seed = p_config.netsim.seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull; // an independent stream
	m.in = net::NetSim(in_config);
	std::string err;
	m.host = net::Host::create_client(net::kChannelCount, &err);
	if (m.host == nullptr) {
		return core::make_error("ClientSession: " + err);
	}
	const int peer = m.host->connect(p_config.host.c_str(), p_config.port);
	if (peer < 0) {
		m.host.reset();
		return core::make_error("ClientSession: cannot connect to " + p_config.host);
	}
	m.peer = uint32_t(peer);
	const uint64_t t0 = net::monotonic_ns();
	bool connected = false;
	while (!connected && net::monotonic_ns() - t0 < uint64_t(p_config.connect_timeout_ms) * 1'000'000ull) {
		net::Host::Event ev;
		const int r = m.host->service(10, ev);
		if (r < 0) {
			break;
		}
		connected = r > 0 && ev.type == net::Host::EventType::Connect;
	}
	if (!connected) {
		m.host.reset();
		return core::make_error("ClientSession: no connection to " + p_config.host + ":" + std::to_string(p_config.port) + " within " +
				std::to_string(p_config.connect_timeout_ms) + " ms");
	}
	m.stats = ClientStats();
	m.stats.connected = true;
	m.connected = true;
	m.stop_requested = false;
	const uint64_t now = net::monotonic_ns();
	net::Hello hello;
	hello.name = p_config.name;
	m.hello_ns = now;
	m.submit(net::kChannelReliable, net::encode_hello(hello), now);
	net::Ready ready;
	ready.source_digest = p_level->source_digest;
	m.submit(net::kChannelReliable, net::encode_ready(ready), now);
	m.release_out(now);
	m.running = true;
	m.started = true;
	m.thread = std::thread([this] { impl_->run(); });
	g_registry.add(this);
	return core::Status::success();
}

void ClientSession::stop() {
	std::lock_guard lock(impl_->lifecycle_mutex);
	if (!impl_->thread.joinable()) {
		return;
	}
	impl_->stop_requested = true;
	impl_->thread.join();
	impl_->host.reset();
	impl_->running = false;
	g_registry.remove(this);
}

bool ClientSession::running() const {
	return impl_->running.load(std::memory_order_acquire);
}

void ClientSession::set_input(const InputSample &p_input) {
	std::lock_guard lock(impl_->input_mutex);
	impl_->input = p_input;
}

bool ClientSession::read_presentation(PresentationFrame &r_frame) const {
	std::lock_guard lock(impl_->frame_mutex);
	if (impl_->shared_frame->serial == r_frame.serial) {
		return false;
	}
	Impl::copy_frame(r_frame, *impl_->shared_frame);
	return true;
}

ClientStats ClientSession::stats() const {
	std::lock_guard lock(impl_->frame_mutex);
	return impl_->shared_frame->stats;
}

void ClientSession::send_debug(const net::DebugRequest &p_request) {
	std::lock_guard lock(impl_->debug_mutex);
	impl_->debug_out.push_back(p_request);
}

bool ClientSession::take_debug_report(net::DebugReport &r_report) {
	std::lock_guard lock(impl_->debug_mutex);
	if (impl_->reports.empty()) {
		return false;
	}
	r_report = std::move(impl_->reports.front());
	impl_->reports.pop_front();
	return true;
}

void ClientSession::debug_shift_clock(int32_t p_ticks) {
	impl_->shift_request.fetch_add(p_ticks, std::memory_order_acq_rel);
}

void ClientSession::debug_stall(uint32_t p_ms) {
	impl_->stall_request_ms.store(p_ms, std::memory_order_release);
}

size_t ClientSession::stop_all() {
	return g_registry.for_each_pinned([](ClientSession *p_session) { p_session->stop(); });
}

} // namespace bf3::host
