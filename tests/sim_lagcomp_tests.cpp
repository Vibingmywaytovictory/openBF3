// M2a lc: lag compensation and hit capsules without data (docs/design/m2a.md §6, §13.3, E12 (a), R4; as built:
// docs/design/m2a-lc.md). Suite "sim-lagcomp-unit" (ctest sim.lagcomp.unit; also under the asan preset).
//
//   * the sampler and its selection rule (net::sample_other / select_sample), the time rule (rewind_time_q8 with
//     sim_time_q8 and view_lag_q8), the client's track rules (rebuild_track);
//   * LagHistory's ack conventions (§7.2: snapshot bits newest - 1 - i, others bits newest - i, unknown seqs and others
//     bits for seqs that sent no SnapshotOthers ignored and counted) and the shot view's refusal rules;
//   * E12 (a) sample-equality rows: two clients with different seqs (one wraps) and different Snapshot /
//     SnapshotOthers splits, both datagram kinds lost and reordered, a respawn; per fired command and target the server's
//     rebuild from LagHistory equals the client's rebuild from its ReceivedRecords (namespace m2a_fake: nt's wire types
//     are not merged, so the client's store holds the records as quantize_other gives them, which is what the decoder
//     yields) bit for bit, the list and the OtherPose; the provider's rewound answers with the pose cache on equal the
//     cache off; window (rebuild_track over the 32-seq received set) vs the client's incremental track (add_others'
//     rules), every difference classified;
//   * the capsule pose table: hash, structural checks, the capsules/ blob, hostile blobs (every truncation, byte
//     mutations, oversized counts); posed_target's blend, yaw and clamps;
//   * the provider's verdicts and the swept-bound broad phase (culling never removes a target the segment hits);
//   * R4: ProjectileWorld fills TargetQuery::from / to / radius / has_segment at its query sites.
#include <doctest/doctest.h>

#include "bf3/core/gameplay/capsule_pose_table.h"
#include "bf3/host/lag_comp.h"
#include "bf3/net/other_sample.h"
#include "bf3/net/protocol.h"
#include "bf3/sim/command.h"
#include "bf3/sim/soldier_state.h"
#include "bf3/sim/weapons/ballistics.h"
#include "bf3/sim/weapons/hit_target.h"
#include "bf3/sim/weapons/posed_target.h"
#include "test_support.h"
#include "weapons_support.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace bf3;
namespace gp = bf3::core::gameplay;
namespace sw = bf3::sim::weapons;
namespace tw = bf3::test::weapons;

namespace {

constexpr int64_t U = sw::kTimeUnitsPerTick;
const std::array<float, sim::kPoseCount> kEyes{ 1.6f, 1.0f, 0.3f };
constexpr double kMaxExtraTicks = 250.0 / (1000.0 / 60.0); // 15 ticks (ClientSessionConfig::extrapolate_max_ms)

bool same_f(float p_a, float p_b) {
	return std::memcmp(&p_a, &p_b, sizeof(float)) == 0;
}

bool same_target(const sw::HitTarget &p_a, const sw::HitTarget &p_b) {
	if (p_a.id != p_b.id || p_a.team != p_b.team || p_a.capsule_count != p_b.capsule_count || p_a.spawn_protected != p_b.spawn_protected ||
			!same_f(p_a.bound_radius, p_b.bound_radius)) {
		return false;
	}
	for (size_t k = 0; k < 3; ++k) {
		if (!same_f(p_a.bound_center[k], p_b.bound_center[k])) {
			return false;
		}
	}
	for (size_t c = 0; c < p_a.capsule_count; ++c) {
		const sw::ZoneCapsule &x = p_a.capsules[c], &y = p_b.capsules[c];
		for (size_t k = 0; k < 3; ++k) {
			if (!same_f(x.a[k], y.a[k]) || !same_f(x.b[k], y.b[k])) {
				return false;
			}
		}
		if (!same_f(x.radius, y.radius) || x.zone != y.zone || x.capsule != y.capsule || x.property != y.property || x.flags != y.flags) {
			return false;
		}
	}
	return true;
}

// ---- a synthetic capsule pose table (3 capsules) -------------------------------------------------------------------

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
	const float lean = 0.004f * float(p_pitch); // the head moves forward with the pitch bucket
	r_out[0] = cap(0.0f, 1.65f - 0.6f * s, 0.05f + lean, 0.0f, 1.65f - 0.6f * s, 0.05f + lean, 0.15f, sw::HitZone::Head, 0);
	r_out[1] = cap(0.0f, 1.3f - 0.5f * s, 0.0f, 0.0f, 1.3f - 0.5f * s, 0.0f, 0.25f, sw::HitZone::Chest, 1);
	r_out[2] = cap(0.1f, 0.1f, 0.0f, 0.1f, 0.8f - 0.3f * s, 0.01f * float(p_pitch), 0.1f, sw::HitZone::Legs, 2);
	for (int c = 0; c < 3; ++c) {
		r_out[c].a[1] += float(p_extra);
		r_out[c].b[1] += float(p_extra);
	}
}

gp::CapsulePoseTable synth_table() {
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
			const double w = double(f) / 7.0;
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

net::OtherSoldier rec(uint8_t p_slot, float p_x, float p_vx, uint16_t p_yaw = 0, uint8_t p_gen = 1) {
	net::OtherSoldier o;
	o.slot = p_slot;
	o.generation = p_gen;
	o.position = { p_x, 0.0f, 10.0f };
	o.velocity = { p_vx, 0.0f, 0.0f };
	o.yaw = p_yaw;
	return o;
}

} // namespace

// ---- m2a_fake: the client side of nt's v2 wire (not merged) ------------------------------------------------------

namespace m2a_fake {

// One datagram of a snapshot seq to one client: the Snapshot (main part, the others of main_mask) or the
// SnapshotOthers (others_mask). The records are quantize_other's, which is what nt's decoder yields (write_other
// encodes exactly the quantised fields; protocol.cpp).
struct Datagram {
	uint8_t client = 0;
	bool others = false;
	uint16_t seq = 0;
	uint32_t tick = 0;
	uint32_t arrive = 0;
	std::vector<net::OtherSoldier> records;
};

// The client's received records by seq (a 64 ring per part), its newest Snapshot and the §7.2 ack fields.
struct ReceivedRecords {
	struct Part {
		bool valid = false;
		uint16_t seq = 0;
		uint32_t tick = 0;
		std::vector<net::OtherSoldier> records;
	};
	std::array<Part, 64> main{}, others{};
	bool any_main = false;
	uint16_t newest = 0;
	uint32_t newest_tick = 0;

	void receive(const Datagram &p_d) {
		Part &p = (p_d.others ? others : main)[p_d.seq % 64];
		p.valid = true;
		p.seq = p_d.seq;
		p.tick = p_d.tick;
		p.records = p_d.records;
		if (!p_d.others && (!any_main || net::seq_newer(p_d.seq, newest))) {
			any_main = true;
			newest = p_d.seq;
			newest_tick = p_d.tick;
		}
	}
	const Part *part(bool p_others, uint16_t p_seq) const {
		const Part &p = (p_others ? others : main)[p_seq % 64];
		return p.valid && p.seq == p_seq ? &p : nullptr;
	}
	// CommandPacket's newest_snapshot / snapshot_ack_bits (bit i = newest - 1 - i) / others_ack_bits (bit i = newest - i).
	void acks(uint16_t &r_newest, uint32_t &r_snap, uint32_t &r_others) const {
		r_newest = newest;
		r_snap = 0;
		r_others = 0;
		for (uint32_t i = 0; i < 32; ++i) {
			if (part(false, uint16_t(newest - 1 - i)) != nullptr) {
				r_snap |= 1u << i;
			}
			if (part(true, uint16_t(newest - i)) != nullptr) {
				r_others |= 1u << i;
			}
		}
	}
	// The client's own rebuild: the target's records in the 32 seqs up to the view seq (both parts), rebuild_track.
	size_t rebuild(uint8_t p_target, std::span<net::TimedOther, net::kTrackSamples> r_list) const {
		std::vector<net::TimedOther> c;
		for (uint32_t i = 0; i < 32; ++i) {
			for (const bool o : { false, true }) {
				if (const Part *p = part(o, uint16_t(newest - i))) {
					for (const net::OtherSoldier &r : p->records) {
						if (r.slot == p_target) {
							c.push_back({ p->tick, r });
						}
					}
				}
			}
		}
		return net::rebuild_track(c, newest_tick, r_list);
	}
};

// The client's incremental track (client_session.cpp add_others and publish, M1): per other, reset on a generation
// change of any arriving record, sorted insert, a duplicate tick skipped, the 8 newest kept, dropped 1 s (60 ticks)
// after the last arrival.
struct ClientTracks {
	struct Track {
		bool used = false;
		uint8_t generation = 0;
		uint32_t last_seen = 0; // arrival tick
		std::deque<net::TimedOther> samples;
		bool reset_by_older = false; // a reordered older record of another generation reset it
	};
	std::array<Track, 16> tracks{};
	void receive(const Datagram &p_d, uint8_t p_self) {
		for (const net::OtherSoldier &o : p_d.records) {
			if (o.slot >= tracks.size() || o.slot == p_self) {
				continue;
			}
			Track &t = tracks[o.slot];
			if (!t.used || t.generation != o.generation) {
				const bool older = t.used && !t.samples.empty() && p_d.tick < t.samples.back().server_tick;
				t = Track();
				t.used = true;
				t.generation = o.generation;
				t.reset_by_older = older;
			}
			t.last_seen = p_d.arrive;
			auto at = std::lower_bound(t.samples.begin(), t.samples.end(), p_d.tick, [](const net::TimedOther &a, uint32_t b) { return a.server_tick < b; });
			if (at != t.samples.end() && at->server_tick == p_d.tick) {
				continue;
			}
			t.samples.insert(at, net::TimedOther{ p_d.tick, o });
			while (t.samples.size() > net::kTrackSamples) {
				t.samples.pop_front();
			}
		}
	}
};

} // namespace m2a_fake

namespace {

// ---- the E12 scenario ---------------------------------------------------------------------------------------------

constexpr uint32_t kPlayers = 6;
constexpr uint32_t kRespawnSlot = 3;
constexpr uint32_t kDeathTick = 200, kRespawnTick = 230;

sim::SoldierState state_of(uint32_t p_slot, uint32_t p_tick) {
	sim::SoldierState s;
	const double t = double(p_tick);
	s.tick = p_tick;
	s.life = 1;
	if (p_slot == kRespawnSlot && p_tick >= kRespawnTick) {
		s.life = 2;
	}
	s.mode = p_slot == kRespawnSlot && p_tick >= kDeathTick && p_tick < kRespawnTick ? sim::MoveMode::Dead : sim::MoveMode::OnGround;
	const double w = 0.05 + 0.01 * p_slot;
	s.position = { float(3.0 * p_slot + 2.0 * std::sin(w * t + p_slot)), float(0.1 * std::sin(0.02 * t)), float(10.0 + 1.5 * std::cos(0.07 * t + 2.0 * p_slot)) };
	s.velocity = { float(2.0 * w * 60.0 * std::cos(w * t + p_slot)), float(0.002 * 60.0 * std::cos(0.02 * t)), float(-1.5 * 0.07 * 60.0 * std::sin(0.07 * t + 2.0 * p_slot)) };
	s.yaw = uint16_t(uint32_t(p_tick * 700u + p_slot * 9000u) & 0xFFFFu); // turns through the wrap
	s.pitch = int16_t(1500.0 * std::sin(0.03 * t + p_slot));
	// Slot 2 crouches and stands up again every 90 ticks (20-tick transitions).
	if (p_slot == 2) {
		const uint32_t ph = p_tick % 90;
		const bool down = (p_tick / 90) % 2 == 0;
		s.pose = down ? sim::Pose::Stand : sim::Pose::Crouch;
		s.target_pose = down ? sim::Pose::Crouch : sim::Pose::Stand;
		if (ph < 20) {
			s.transition_total = 20;
			s.transition_ticks = uint16_t(19 - ph);
			if (s.transition_ticks == 0) {
				s.pose = s.target_pose;
			}
		} else {
			s.pose = s.target_pose;
		}
	}
	return s;
}

bool live(const sim::SoldierState &p_s) {
	return p_s.life != 0 && p_s.mode != sim::MoveMode::Dead;
}

uint64_t mix(uint64_t p_x) {
	p_x ^= p_x >> 33;
	p_x *= 0xFF51AFD7ED558CCDull;
	p_x ^= p_x >> 33;
	p_x *= 0xC4CEB9FE1A85EC53ull;
	p_x ^= p_x >> 33;
	return p_x;
}

struct ClientSetup {
	uint8_t slot = 0;
	uint16_t first_seq = 0;
	uint64_t main_mask = 0, others_mask = 0; // the split (before the live filter)
	uint32_t down = 3, up = 2, jitter = 3;
	uint32_t loss_pct = 12;
};

struct Fired {
	uint8_t client = 0;
	uint32_t cmd_tick = 0;
	uint32_t shot_index = 0;
	int32_t view_back_q8 = 0;
	int64_t render_q8 = 0;
	uint32_t view_tick = 0; // the client's newest Snapshot tick (the view seq's)
	uint8_t quality = 0;
	bool view_ok = false;
	host::ShotView view;
	// The client's side at fire time, per target slot: its rebuild and its sample.
	std::array<std::array<net::TimedOther, net::kTrackSamples>, kPlayers> list{};
	std::array<size_t, kPlayers> list_n{};
	std::array<net::OtherPose, kPlayers> pose{};
	// Window vs track: the incremental track at fire time.
	std::array<std::vector<net::TimedOther>, kPlayers> track{};
	std::array<bool, kPlayers> track_live{};
	std::array<bool, kPlayers> track_reset_by_older{};
};

struct E12Run {
	std::vector<Fired> fired;
	host::LagCompStats history_stats;
	uint64_t datagrams = 0, lost = 0, reordered = 0;
	std::array<std::vector<sim::SoldierState>, 1> unused;
};

// Runs the two-client scenario; p_on_command is called on the server at every command arrival (after the shot view),
// with the present states of that tick.
template <typename OnCommand>
E12Run run_e12(host::LagHistory &r_history, const std::array<ClientSetup, 2> &p_clients, uint32_t p_ticks, OnCommand &&p_on_command) {
	E12Run run;
	std::array<m2a_fake::ReceivedRecords, 2> received{};
	std::array<m2a_fake::ClientTracks, 2> tracks{};
	std::array<uint16_t, 2> next_seq{ p_clients[0].first_seq, p_clients[1].first_seq };
	std::vector<m2a_fake::Datagram> in_flight;
	struct Cmd {
		uint32_t arrive;
		Fired f;
		uint16_t newest;
		uint32_t snap_bits, others_bits;
		bool any;
	};
	std::vector<Cmd> commands;
	std::array<uint32_t, 2> shot_index{};
	std::array<uint32_t, 2> last_arrival{};
	for (uint32_t T = 1; T <= p_ticks; ++T) {
		std::vector<sim::SoldierState> states(kPlayers);
		for (uint32_t s = 0; s < kPlayers; ++s) {
			states[s] = state_of(s, T);
		}
		// Server: the commands arriving now (in order; command loss is not part of E12).
		for (Cmd &c : commands) {
			if (c.arrive != T) {
				continue;
			}
			if (c.any) {
				r_history.ack(c.f.client, c.newest, c.snap_bits, c.others_bits);
			}
			const host::AckState at = r_history.ack_state(c.f.client);
			c.f.view_ok = r_history.shot_view(c.f.client, at, uint8_t(c.newest & 0xFF), c.f.view_back_q8, c.f.quality, c.f.view);
			p_on_command(c.f, states);
			run.fired.push_back(c.f);
		}
		// Server: the snapshot of tick T (30 Hz) after the step.
		if (T % 2 == 0) {
			std::vector<net::OtherSoldier> recs;
			for (uint32_t s = 0; s < kPlayers; ++s) {
				if (live(states[s])) {
					recs.push_back(net::quantize_other(uint8_t(s), states[s]));
				}
			}
			r_history.record_tick(T, recs);
			for (uint8_t c = 0; c < 2; ++c) {
				const ClientSetup &cs = p_clients[c];
				uint64_t main_mask = 0, others_mask = 0;
				m2a_fake::Datagram dm, d_o;
				for (const net::OtherSoldier &r : recs) {
					if (r.slot == cs.slot) {
						continue;
					}
					if ((cs.main_mask >> r.slot) & 1u) {
						main_mask |= uint64_t(1) << r.slot;
						dm.records.push_back(r);
					} else if ((cs.others_mask >> r.slot) & 1u) {
						others_mask |= uint64_t(1) << r.slot;
						d_o.records.push_back(r);
					}
				}
				const uint16_t seq = next_seq[c]++;
				r_history.sent(c, seq, T, main_mask, others_mask);
				dm.client = d_o.client = c;
				dm.seq = d_o.seq = seq;
				dm.tick = d_o.tick = T;
				d_o.others = true;
				for (m2a_fake::Datagram *d : { &dm, &d_o }) {
					if (d->others && others_mask == 0) {
						continue; // no SnapshotOthers this seq
					}
					++run.datagrams;
					const uint64_t h = mix(uint64_t(c) << 40 | uint64_t(seq) << 8 | (d->others ? 1u : 0u));
					if (h % 100 < cs.loss_pct) {
						++run.lost;
						continue;
					}
					d->arrive = T + cs.down + uint32_t((h >> 8) % (cs.jitter + 1));
					in_flight.push_back(*d);
				}
			}
		}
		// Clients: deliver what arrives this tick (in arrival order; ties in send order).
		for (uint8_t c = 0; c < 2; ++c) {
			for (const m2a_fake::Datagram &d : in_flight) {
				if (d.client == c && d.arrive == T) {
					if (received[c].any_main && net::seq_newer(received[c].newest, d.seq)) {
						++run.reordered;
					}
					received[c].receive(d);
					tracks[c].receive(d, p_clients[c].slot);
					last_arrival[c] = T;
				}
			}
		}
		std::erase_if(in_flight, [&](const m2a_fake::Datagram &d) { return d.arrive <= T; });
		// Clients: draw and fire every tick: render time a few ticks behind the newest Snapshot, fractional, sometimes
		// past it (extrapolated); the command for tick T + up carries the view sample and the acks.
		for (uint8_t c = 0; c < 2; ++c) {
			const m2a_fake::ReceivedRecords &rr = received[c];
			if (!rr.any_main) {
				continue;
			}
			Cmd cmd;
			cmd.any = true;
			rr.acks(cmd.newest, cmd.snap_bits, cmd.others_bits);
			Fired &f = cmd.f;
			f.client = c;
			f.cmd_tick = T + p_clients[c].up;
			f.shot_index = shot_index[c]++;
			const int64_t delay_q8 = int64_t(mix(uint64_t(T) * 7 + c) % (5 * 256)) - 256; // -1 .. +4 ticks
			f.view_tick = rr.newest_tick;
			f.render_q8 = int64_t(rr.newest_tick) * 256 - delay_q8;
			f.quality = uint8_t(f.render_q8 <= int64_t(rr.newest_tick) * 256 ? sim::ViewQuality::Interpolated : sim::ViewQuality::Extrapolated);
			f.view_back_q8 = int32_t((int64_t(f.cmd_tick) << 8) - f.render_q8);
			for (uint8_t s = 0; s < kPlayers; ++s) {
				if (s == p_clients[c].slot) {
					continue;
				}
				f.list_n[s] = rr.rebuild(s, f.list[s]);
				f.pose[s] = net::sample_other(std::span<const net::TimedOther>(f.list[s].data(), f.list_n[s]), double(f.render_q8) / 256.0, kEyes, kMaxExtraTicks);
				const m2a_fake::ClientTracks::Track &tr = tracks[c].tracks[s];
				f.track_live[s] = tr.used && !tr.samples.empty() && T <= tr.last_seen + net::kTrackTimeoutTicks;
				f.track[s].assign(tr.samples.begin(), tr.samples.end());
				f.track_reset_by_older[s] = tr.reset_by_older;
			}
			cmd.arrive = f.cmd_tick;
			commands.push_back(cmd);
		}
		std::erase_if(commands, [&](const Cmd &c) { return c.arrive <= T; });
	}
	run.history_stats = r_history.stats();
	return run;
}

std::array<ClientSetup, 2> two_clients() {
	std::array<ClientSetup, 2> c{};
	c[0].slot = 0;
	c[0].first_seq = 65'500; // wraps during the run
	c[0].main_mask = (1u << 1) | (1u << 2) | (1u << 3);
	c[0].others_mask = (1u << 4) | (1u << 5);
	c[0].down = 3;
	c[0].up = 2;
	c[0].jitter = 3;
	c[1].slot = 1;
	c[1].first_seq = 100;
	c[1].main_mask = (1u << 0) | (1u << 4);
	c[1].others_mask = (1u << 2) | (1u << 3) | (1u << 5);
	c[1].down = 5;
	c[1].up = 3;
	c[1].jitter = 4;
	c[1].loss_pct = 15;
	return c;
}

} // namespace

TEST_SUITE("sim-lagcomp-unit") {
	TEST_CASE("sample_other: the selection rule, hold, extrapolation limit, shortest-arc yaw, discrete switch") {
		std::array<net::TimedOther, 3> l{ net::TimedOther{ 10, rec(1, 0.0f, 6.0f, 65000) }, net::TimedOther{ 12, rec(1, 0.2f, 6.0f, 500) },
			net::TimedOther{ 14, rec(1, 0.4f, 6.0f, 1000) } };
		l[2].soldier.pose = sim::Pose::Crouch;
		const std::span<const net::TimedOther> s(l);
		// Empty: Hold, held.
		const net::OtherPose e = net::sample_other({}, 5.0, kEyes, kMaxExtraTicks);
		CHECK(e.kind == uint8_t(net::SampleKind::Hold));
		CHECK(e.held);
		// Before the oldest: hold it.
		net::OtherPose p = net::sample_other(s, 9.0, kEyes, kMaxExtraTicks);
		CHECK(p.kind == uint8_t(net::SampleKind::Hold));
		CHECK(p.held);
		CHECK(p.tick_a == 10);
		CHECK(p.position[0] == l[0].soldier.position[0]);
		// Exactly at a sample: Hold at that sample, not held.
		p = net::sample_other(s, 12.0, kEyes, kMaxExtraTicks);
		CHECK(p.kind == uint8_t(net::SampleKind::Hold));
		CHECK_FALSE(p.held);
		CHECK(p.tick_a == 12);
		// Between: Interpolate, tick_a / tick_b the bracketing pair; the yaw takes the shortest arc through the wrap.
		p = net::sample_other(s, 11.0, kEyes, kMaxExtraTicks);
		CHECK(p.kind == uint8_t(net::SampleKind::Interpolate));
		CHECK(p.tick_a == 10);
		CHECK(p.tick_b == 12);
		CHECK(p.position[0] > 0.0f);
		CHECK(p.position[0] < 0.2f);
		const double y0 = sim::yaw_radians(65000), y1 = sim::yaw_radians(500) + 2 * sim::kPi;
		CHECK(std::fabs(std::remainder(double(p.yaw) - 0.5 * (y0 + y1), 2 * sim::kPi)) < 1e-3);
		// The discrete fields switch together at the newer sample only.
		p = net::sample_other(s, 13.9, kEyes, kMaxExtraTicks);
		CHECK(p.pose == uint8_t(sim::Pose::Stand));
		p = net::sample_other(s, 14.0, kEyes, kMaxExtraTicks);
		CHECK(p.pose == uint8_t(sim::Pose::Crouch));
		// Past the newest: extrapolate along the velocity up to 15 ticks, then hold (held, at the limit).
		p = net::sample_other(s, 20.0, kEyes, kMaxExtraTicks);
		CHECK(p.kind == uint8_t(net::SampleKind::Extrapolate));
		CHECK(p.extrapolated);
		CHECK(p.position[0] == doctest::Approx(0.4 + 6.0 * 6.0 / 60.0).epsilon(1e-4));
		const net::OtherPose limit = net::sample_other(s, 14.0 + kMaxExtraTicks + 3.0, kEyes, kMaxExtraTicks);
		CHECK(limit.held);
		CHECK(limit.position[0] == doctest::Approx(0.4 + 6.0 * 15.0 / 60.0).epsilon(1e-4));
		// select_sample is the rule sample_other uses (random lists and times).
		std::mt19937 rng(7);
		for (int i = 0; i < 2000; ++i) {
			std::vector<net::TimedOther> v;
			uint32_t tick = 100;
			const size_t n = rng() % 9;
			for (size_t k = 0; k < n; ++k) {
				tick += 1 + rng() % 4;
				v.push_back({ tick, rec(2, float(rng() % 100) * 0.1f, 3.0f) });
			}
			const double r = 95.0 + double(rng() % 6000) / 100.0;
			const net::SampleSelection sel = net::select_sample(v, r, kMaxExtraTicks);
			const net::OtherPose q = net::sample_other(v, r, kEyes, kMaxExtraTicks);
			CHECK(sel.valid == !v.empty());
			CHECK(q.tick_a == sel.tick_a);
			CHECK(q.tick_b == sel.tick_b);
			CHECK(q.kind == uint8_t(sel.kind));
			if (sel.valid) {
				CHECK(q.held == sel.held);
				CHECK(q.extrapolated == sel.extrapolated);
			} else {
				CHECK(q.held); // an empty list: a default pose, held
			}
		}
	}

	TEST_CASE("time rule: rewind_time_q8 = sim_time_q8(t) - clamp(view_back - 256, 0, max x 256)") {
		// sim_time_q8: q8 ticks of the record scale, half up.
		CHECK(net::sim_time_q8(0) == 0);
		CHECK(net::sim_time_q8(sw::tick_start(5)) == 4 * 256);
		CHECK(net::sim_time_q8(sw::tick_end(5)) == 5 * 256);
		CHECK(net::sim_time_q8(U / 512) == 1); // 0.5 q8 rounds up
		CHECK(net::sim_time_q8(U / 512 - 1) == 0);
		CHECK(net::sim_time_q8(-U) == -256);
		for (uint32_t T = 50; T < 60; ++T) {
			for (int64_t R = int64_t(T) * 256 - 30 * 256; R <= int64_t(T) * 256 + 256; R += 37) {
				const int32_t vb = int32_t((int64_t(T) << 8) - R);
				const int64_t lag = int64_t(net::view_lag_q8(vb));
				const int64_t got = net::rewind_time_q8(sw::tick_start(T), vb, host::kDefaultMaxRewindTicks);
				if (lag < 0) {
					CHECK(got == int64_t(T - 1) * 256); // never in the future: the present
				} else if (lag <= int64_t(host::kDefaultMaxRewindTicks) * 256) {
					CHECK(got == R); // a shot at the start of command tick T tests at exactly the drawn render time
				} else {
					CHECK(got == int64_t(T - 1) * 256 - int64_t(host::kDefaultMaxRewindTicks) * 256); // clamped
				}
				// A later segment of the same bullet (k ticks and a fraction later) tests as much later.
				const sw::SimTime later = sw::tick_start(T) + 3 * U + U / 4;
				CHECK(net::rewind_time_q8(later, vb, host::kDefaultMaxRewindTicks) == got + 3 * 256 + 64);
				// max_rewind_ticks 0 (E12's control run): the present.
				CHECK(net::rewind_time_q8(sw::tick_start(T), vb, 0) == int64_t(T - 1) * 256);
			}
		}
	}

	TEST_CASE("rebuild_track: sorted, duplicates once, the 8 newest, the newest's generation, the 60-tick timeout") {
		std::array<net::TimedOther, net::kTrackSamples> out;
		std::vector<net::TimedOther> c;
		for (uint32_t t : { 30u, 10u, 20u, 12u, 14u, 16u, 18u, 22u, 24u, 26u, 28u, 20u }) {
			c.push_back({ t, rec(3, float(t), 0) });
		}
		size_t n = net::rebuild_track(c, 40, out);
		REQUIRE(n == 8);
		const uint32_t want[8] = { 16, 18, 20, 22, 24, 26, 28, 30 };
		for (size_t i = 0; i < 8; ++i) {
			CHECK(out[i].server_tick == want[i]);
		}
		// Generation: the newest record's generation only.
		c.push_back({ 32, rec(3, 32.0f, 0, 0, 2) });
		c.push_back({ 31, rec(3, 31.0f, 0, 0, 2) });
		n = net::rebuild_track(c, 40, out);
		REQUIRE(n == 2);
		CHECK(out[0].server_tick == 31);
		CHECK(out[1].server_tick == 32);
		// Timeout: the newest record more than 60 ticks before the view tick: empty.
		CHECK(net::rebuild_track(c, 32 + net::kTrackTimeoutTicks, out) == 2);
		CHECK(net::rebuild_track(c, 33 + net::kTrackTimeoutTicks, out) == 0);
		CHECK(net::rebuild_track({}, 10, out) == 0);
	}

	TEST_CASE("LagHistory: the ack conventions of §7.2, ignored acks counted, the shot view's refusal rules") {
		host::LagCompConfig cfg;
		host::LagHistory h(cfg, 4);
		for (uint32_t t = 2; t <= 40; t += 2) {
			h.record_tick(t, std::vector<net::OtherSoldier>{ rec(1, float(t), 0), rec(2, -float(t), 0) });
			const uint16_t seq = uint16_t(65'530u + t / 2); // wraps at t = 12
			h.sent(0, seq, t, 1u << 1, (t / 2) % 2 == 0 ? (1u << 2) : 0u); // SnapshotOthers on even seqs only
		}
		// seq of tick t = 65530 + t / 2; newest acked = tick 30 -> seq 9 (wrapped).
		const uint16_t newest = uint16_t((65'530u + 15u) & 0xFFFFu);
		// snapshot_ack_bits: bit 0 = newest - 1, bit 2 = newest - 3; others_ack_bits: bit 0 = newest (odd seq 9: none
		// sent, ignored), bit 1 = newest - 1 (seq 8, sent).
		h.ack(0, newest, 0b101u, 0b11u);
		CHECK(h.stats().acks_ignored == 1);
		host::AckState a = h.ack_state(0);
		CHECK(a.newest == newest);
		CHECK(a.main_bits == (1u << 0 | 1u << 1 | 1u << 3));
		CHECK(a.others_bits == (1u << 1));
		// Unknown seqs (never sent) and an out-of-range slot are ignored and counted; acks are never cleared.
		h.ack(0, uint16_t(newest + 40), 0, 0);
		CHECK(h.stats().acks_ignored == 2);
		h.ack(9, newest, 0, 0);
		CHECK(h.stats().acks_ignored == 3);
		h.ack(0, uint16_t(newest - 2), 0, 0);
		a = h.ack_state(0);
		CHECK(a.newest == newest);
		CHECK(a.main_bits == (1u << 0 | 1u << 1 | 1u << 2 | 1u << 3));
		// Shot views.
		host::ShotView v;
		const int32_t vb = 10 * 256;
		CHECK(h.shot_view(0, a, uint8_t(newest & 0xFF), vb, uint8_t(sim::ViewQuality::Interpolated), v));
		CHECK(v.view_seq == newest);
		CHECK(v.main_bits == 0b1111u);
		CHECK(v.others_bits == 0b10u);
		CHECK(h.shot_view(0, a, uint8_t((newest - 1) & 0xFF), vb, uint8_t(sim::ViewQuality::Extrapolated), v));
		CHECK(v.view_seq == uint16_t(newest - 1));
		CHECK(v.main_bits == 0b111u);
		CHECK(v.others_bits == 0b1u);
		// None: tested at the present (true, no seq).
		CHECK(h.shot_view(0, a, 0, 0, uint8_t(sim::ViewQuality::None), v));
		CHECK(v.quality == uint8_t(sim::ViewQuality::None));
		// Refused: Held, an unknown quality, a negative lag, beyond the 32-seq window, a seq not acked as a Snapshot.
		CHECK_FALSE(h.shot_view(0, a, uint8_t(newest & 0xFF), vb, uint8_t(sim::ViewQuality::Held), v));
		CHECK_FALSE(h.shot_view(0, a, uint8_t(newest & 0xFF), vb, 7, v));
		CHECK_FALSE(h.shot_view(0, a, uint8_t(newest & 0xFF), 255, uint8_t(sim::ViewQuality::Interpolated), v));
		CHECK_FALSE(h.shot_view(0, a, uint8_t((newest - 40) & 0xFF), vb, uint8_t(sim::ViewQuality::Interpolated), v));
		CHECK_FALSE(h.shot_view(0, a, uint8_t((newest - 5) & 0xFF), vb, uint8_t(sim::ViewQuality::Interpolated), v)); // seq 4 not acked
		CHECK_FALSE(h.shot_view(1, h.ack_state(1), 0, vb, uint8_t(sim::ViewQuality::Interpolated), v)); // nothing acked
		// track under the view: target 1 (main part) from the acked seqs; target 2 (others) from seq 8 only.
		REQUIRE(h.shot_view(0, a, uint8_t(newest & 0xFF), vb, uint8_t(sim::ViewQuality::Interpolated), v));
		std::array<net::TimedOther, net::kTrackSamples> l;
		CHECK(h.track(0, v, 1, l) == 4);
		CHECK(l[3].server_tick == 30);
		REQUIRE(h.track(0, v, 2, l) == 1);
		CHECK(l[0].server_tick == 28);
		CHECK(h.track(0, v, 3, l) == 0);
		CHECK(h.track(0, v, 200, l) == 0);
		// client_reset forgets the slot's ring and acks.
		h.client_reset(0);
		CHECK(h.ack_state(0).main_bits == 0);
		// Hostile records: a slot beyond the history's slots or a non-finite position is not stored.
		net::OtherSoldier bad = rec(1, std::numeric_limits<float>::quiet_NaN(), 0);
		h.record_tick(42, std::vector<net::OtherSoldier>{ bad, rec(77, 1.0f, 0) });
		CHECK_FALSE(h.bounds(1).max_speed < 0.0f);
	}

	TEST_CASE("E12 (a): server rebuild from LagHistory == client rebuild from ReceivedRecords, bit for bit") {
		host::LagCompConfig cfg;
		cfg.pose_eye_heights = kEyes;
		host::LagHistory history(cfg, kPlayers);
		size_t clamped = 0;
		size_t compared = 0, rewound = 0, nonempty = 0, interp = 0, extra = 0, hold = 0, list_diff = 0, pose_diff = 0, gen2 = 0;
		E12Run run = run_e12(history, two_clients(), 900, [&](const Fired &f, const std::vector<sim::SoldierState> &) {
			REQUIRE(f.view_ok);
			const int64_t time_q8 = net::rewind_time_q8(sw::tick_start(f.cmd_tick), f.view.view_back_q8, cfg.max_rewind_ticks);
			if (int64_t(net::view_lag_q8(f.view.view_back_q8)) > int64_t(cfg.max_rewind_ticks) * 256) {
				++clamped; // a long Snapshot loss streak: tested at the clamped time, which the client never drew
				return;
			}
			++rewound;
			CHECK(time_q8 == f.render_q8);
			for (uint8_t s = 0; s < kPlayers; ++s) {
				if (s == two_clients()[f.client].slot) {
					continue;
				}
				std::array<net::TimedOther, net::kTrackSamples> list;
				const size_t n = history.track(f.client, f.view, s, list);
				bool same = n == f.list_n[s];
				for (size_t i = 0; same && i < n; ++i) {
					same = net::same_bits(list[i], f.list[s][i]);
				}
				list_diff += same ? 0 : 1;
				CHECK_MESSAGE(same, "client " << int(f.client) << " cmd " << f.cmd_tick << " target " << int(s) << " server n " << n << " client n " << f.list_n[s]);
				const net::OtherPose pose = history.sample(std::span<const net::TimedOther>(list.data(), n), time_q8);
				const bool same_pose = net::same_bits(pose, f.pose[s]);
				pose_diff += same_pose ? 0 : 1;
				CHECK(same_pose);
				++compared;
				nonempty += n > 0 ? 1 : 0;
				interp += n > 0 && pose.kind == uint8_t(net::SampleKind::Interpolate) ? 1 : 0;
				extra += n > 0 && pose.kind == uint8_t(net::SampleKind::Extrapolate) ? 1 : 0;
				hold += n > 0 && pose.kind == uint8_t(net::SampleKind::Hold) ? 1 : 0;
				gen2 += s == kRespawnSlot && n > 0 && pose.generation == 2 ? 1 : 0;
			}
		});
		std::printf("sim.lagcomp.unit E12 rebuild: %zu commands rewound (%zu clamped, not compared), %zu (command, target) rows compared (%zu non-empty: %zu interpolated, %zu "
					"extrapolated, %zu held/at-sample; %zu after the respawn), %zu list and %zu pose differences; %" PRIu64 " datagrams, %" PRIu64 " lost, %" PRIu64
					" reordered; history misses %" PRIu64 ", acks ignored %" PRIu64 "\n",
				rewound, clamped, compared, nonempty, interp, extra, hold, gen2, list_diff, pose_diff, run.datagrams, run.lost, run.reordered, run.history_stats.history_misses,
				run.history_stats.acks_ignored);
		CHECK(list_diff == 0);
		CHECK(pose_diff == 0);
		CHECK(clamped < rewound / 50);
		CHECK(rewound > 1500);
		CHECK(interp > 1000);
		CHECK(extra > 50);
		CHECK(gen2 > 100);
		CHECK(run.lost > 50);
		CHECK(run.reordered > 10);
	}

	TEST_CASE("E12 (a): the provider's rewound answers equal the client's samples; pose cache on == off") {
		const gp::CapsulePoseTable table = synth_table();
		REQUIRE(gp::capsule_pose_table_valid(table));
		host::LagCompConfig on;
		on.pose_eye_heights = kEyes;
		host::LagCompConfig off = on;
		off.pose_cache = false;
		host::LagHistory history(on, kPlayers);
		host::RewindProvider p_on(history, table, on), p_off(history, table, off);
		std::vector<host::RewindTraceRow> trace_on, trace_off;
		p_on.set_trace(&trace_on);
		p_off.set_trace(&trace_off);
		size_t rows = 0, answers = 0, diffs = 0, pellets = 0;
		const std::array<ClientSetup, 2> clients = two_clients();
		run_e12(history, clients, 900, [&](const Fired &f, const std::vector<sim::SoldierState> &p_states) {
			const std::vector<uint8_t> teams{ 0, 1, 0, 1, 0, 1 };
			p_on.begin_tick(f.cmd_tick, p_states, teams);
			p_off.begin_tick(f.cmd_tick, p_states, teams);
			p_on.note_shot(f.client, f.shot_index, f.view);
			p_off.note_shot(f.client, f.shot_index, f.view);
			trace_on.clear();
			trace_off.clear();
			// Two pellets and two segments of the shot: the second pellet's key repeats (cache hits with the cache on).
			for (int seg = 0; seg < 2; ++seg) {
				for (uint8_t pel = 0; pel < 2; ++pel) {
					sw::TargetQuery q;
					q.time_start = sw::tick_start(f.cmd_tick) + seg * U;
					q.time_end = q.time_start + U;
					q.player_slot = f.client;
					q.shot_index = f.shot_index;
					q.pellet = pel;
					const std::span<const sw::HitTarget> a_span = p_on.targets_for(q);
					const std::vector<sw::HitTarget> a(a_span.begin(), a_span.end());
					const std::span<const sw::HitTarget> b = p_off.targets_for(q);
					CHECK(a.size() == b.size());
					for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
						const bool same = same_target(a[i], b[i]);
						diffs += same ? 0 : 1;
						CHECK(same);
					}
					answers += a.size();
					++pellets;
				}
			}
			// The trace rows of the first segment are the client's samples (the generation check may skip a target the
			// client still draws at its old body right after the respawn).
			CHECK(trace_on.size() == trace_off.size()); // the cache-on run traces its cached rows too
			for (const host::RewindTraceRow &r : trace_off) {
				CHECK(r.pose.generation == uint8_t(p_states[r.target].life & 0xFF));
				if (r.time_q8 != f.render_q8) {
					continue; // the second segment, one tick later
				}
				CHECK(net::same_bits(r.pose, f.pose[r.target]));
				++rows;
			}
			p_on.end_tick();
			p_off.end_tick();
		});
		const host::LagCompStats &s_on = p_on.stats();
		const host::LagCompStats &s_off = p_off.stats();
		std::printf("sim.lagcomp.unit E12 provider: %zu queries, %zu answers, %zu trace rows equal to the client's samples, %zu cache on/off differences; cache on: "
					"%" PRIu64 " candidates, %" PRIu64 " hits, %" PRIu64 " full; generation skips %" PRIu64 "; verdicts rewound %" PRIu64 "\n",
				pellets, answers, rows, diffs, s_on.candidates, s_on.pose_cache_hits, s_on.pose_cache_full, s_on.generation_skips, s_on.rewound);
		CHECK(diffs == 0);
		CHECK(rows > 3000);
		CHECK(s_on.pose_cache_hits > 0);
		CHECK(s_off.pose_cache_hits == 0);
		CHECK(s_on.candidates == s_off.candidates);
		CHECK(s_on.answered == s_off.answered);
	}

	TEST_CASE("E12 (a): window (rebuild_track over the received set) vs the client's incremental track") {
		host::LagCompConfig cfg;
		host::LagHistory history(cfg, kPlayers);
		size_t rows = 0, equal = 0, reset_older = 0, newer_others = 0, outside_window = 0, timeout = 0, other = 0;
		const std::array<ClientSetup, 2> clients = two_clients();
		const E12Run run = run_e12(history, clients, 900, [](const Fired &, const std::vector<sim::SoldierState> &) {});
		for (const Fired &f : run.fired) {
			for (uint8_t s = 0; s < kPlayers; ++s) {
				if (s == clients[f.client].slot) {
					continue;
				}
				++rows;
				const std::vector<net::TimedOther> &tr = f.track[s];
				const size_t n = f.list_n[s];
				bool same = (f.track_live[s] ? tr.size() : 0) == n;
				for (size_t i = 0; same && i < n; ++i) {
					same = net::same_bits(tr[i], f.list[s][i]);
				}
				if (same) {
					++equal;
					continue;
				}
				// Classify: a reordered older record of the other generation reset the track (add_others' rule, the
				// window keeps the newest generation); a track sample older than the 32-seq window; the timeout
				// measured from the arrival (client) vs the record tick (window).
				const bool newer = std::any_of(tr.begin(), tr.end(), [&](const net::TimedOther &x) { return x.server_tick > f.view_tick; });
				const bool older = std::any_of(tr.begin(), tr.end(), [&](const net::TimedOther &x) { return x.server_tick + 62 < f.view_tick; });
				if (f.track_reset_by_older[s]) {
					++reset_older;
				} else if (newer) {
					++newer_others; // a SnapshotOthers record newer than the newest Snapshot (m2a.md §16 Q22: left out)
				} else if (older) {
					++outside_window;
				} else if (f.track_live[s] != (n > 0)) {
					++timeout;
				} else {
					++other;
					MESSAGE("unexplained window/track difference: client " << int(f.client) << " cmd " << f.cmd_tick << " target " << int(s) << " track " << tr.size()
																		   << " window " << n);
				}
			}
		}
		std::printf("sim.lagcomp.unit E12 window vs track: %zu rows, %zu equal, %zu reset by a reordered older generation, %zu with a SnapshotOthers record "
					"newer than the newest Snapshot (Q22), %zu track samples beyond the 32-seq window, %zu timeout-basis differences, %zu unexplained\n",
				rows, equal, reset_older, newer_others, outside_window, timeout, other);
		CHECK(other == 0);
		CHECK(equal + reset_older + newer_others + outside_window + timeout == rows);
		CHECK(equal > rows * 3 / 4);
	}

	TEST_CASE("capsule pose table: hash, checks, blob round trip, hostile blobs") {
		gp::CapsulePoseTable t = synth_table();
		REQUIRE(gp::capsule_pose_table_valid(t));
		const uint64_t h = t.hash;
		CHECK(h == gp::capsule_pose_table_hash(t));
		gp::CapsulePoseTable u = t;
		u.steady[5].a[1] = std::nextafter(u.steady[5].a[1], 9.0f);
		CHECK(gp::capsule_pose_table_hash(u) != h);
		u = t;
		u.row_source[4] = 1;
		CHECK(gp::capsule_pose_table_hash(u) != h);
		// Structural checks.
		u = t;
		u.steady[0].radius = 0.0f;
		CHECK_FALSE(gp::capsule_pose_table_valid(u));
		u = t;
		u.transition[3].b[2] = std::numeric_limits<float>::infinity();
		CHECK_FALSE(gp::capsule_pose_table_valid(u));
		u = t;
		std::swap(u.pitch_deg[3], u.pitch_deg[4]);
		CHECK_FALSE(gp::capsule_pose_table_valid(u));
		u = t;
		u.steady.pop_back();
		CHECK_FALSE(gp::capsule_pose_table_valid(u));
		u = t;
		u.version = 99;
		CHECK_FALSE(gp::capsule_pose_table_valid(u));
		u = t;
		u.row_source[0] = 7;
		CHECK_FALSE(gp::capsule_pose_table_valid(u));
		// Blob.
		const std::vector<uint8_t> blob = gp::encode_capsule_pose_table(t);
		gp::CapsulePoseTable back;
		std::string err;
		REQUIRE_MESSAGE(gp::decode_capsule_pose_table(blob, back, &err), err);
		CHECK(back.hash == h);
		CHECK(gp::encode_capsule_pose_table(back) == blob);
		// Every truncation fails.
		size_t truncations = 0;
		for (size_t n = 0; n < blob.size(); n += (n < 256 ? 1 : 97)) {
			gp::CapsulePoseTable x;
			CHECK_FALSE(gp::decode_capsule_pose_table(std::span<const uint8_t>(blob.data(), n), x));
			++truncations;
		}
		// Trailing bytes fail.
		std::vector<uint8_t> longer = blob;
		longer.push_back(0);
		CHECK_FALSE(gp::decode_capsule_pose_table(longer, back));
		// Seeded byte mutations: a decode either fails or gives a valid table whose hash checks.
		std::mt19937 rng(11);
		size_t accepted = 0;
		for (int i = 0; i < 3000; ++i) {
			std::vector<uint8_t> m = blob;
			const size_t at = rng() % m.size();
			m[at] = uint8_t(m[at] ^ (1u + rng() % 255));
			gp::CapsulePoseTable x;
			if (gp::decode_capsule_pose_table(m, x)) {
				++accepted;
				CHECK(gp::capsule_pose_table_valid(x));
				CHECK(x.hash == gp::capsule_pose_table_hash(x));
			}
		}
		// A huge count is refused before any allocation of that size (the blob is short).
		std::vector<uint8_t> big = blob;
		for (size_t k = 8; k < 32 && k < big.size(); ++k) {
			big[k] = 0xFF;
		}
		gp::CapsulePoseTable x;
		CHECK_FALSE(gp::decode_capsule_pose_table(big, x));
		std::printf("sim.lagcomp.unit capsule blob: %zu bytes, %zu truncations refused, %zu of 3000 mutations accepted (all valid with their hash)\n", blob.size(),
				truncations, accepted);
		CHECK(accepted == 0); // the stored hash covers every field
	}

	TEST_CASE("posed_target: pitch and fraction blends, yaw, feet, clamps, the bound sphere") {
		const gp::CapsulePoseTable t = synth_table();
		sw::TargetPose p;
		p.id = 4;
		p.team = 1;
		p.spawn_protected = true;
		sw::HitTarget h = sw::posed_target(t, p);
		CHECK(h.id == 4);
		CHECK(h.team == 1);
		CHECK(h.spawn_protected);
		REQUIRE(h.capsule_count == 3);
		CHECK(h.capsules[0].a[1] == doctest::Approx(1.65));
		CHECK(h.capsules[0].a[2] == doctest::Approx(0.05 + 0.004 * 14));
		CHECK(h.capsules[0].zone == sw::HitZone::Head);
		CHECK(h.capsules[2].flags == sw::zone_capsule_flags::kDeactivateIfBehindWall);
		// Mid-pitch: the average of the two buckets.
		const double mid = 0.5 * (double(t.pitch_deg[20]) + double(t.pitch_deg[21]));
		p.pitch = float(mid * 3.14159265358979323846 / 180.0);
		h = sw::posed_target(t, p);
		CHECK(h.capsules[0].a[2] == doctest::Approx(0.05 + 0.004 * 20.5).epsilon(1e-4));
		// Beyond the table: clamped; NaN: pitch 0.
		p.pitch = 3.0f;
		CHECK(sw::posed_target(t, p).capsules[0].a[2] == doctest::Approx(0.05 + 0.004 * 32));
		p.pitch = std::numeric_limits<float>::quiet_NaN();
		CHECK(sw::posed_target(t, p).capsules[0].a[2] == doctest::Approx(0.05 + 0.004 * 14));
		p.pitch = 0;
		// Yaw 90 deg: forward +Z turns to +X (sim convention), x-left turns to -Z; the feet translate.
		p.yaw = float(sim::kPi / 2);
		p.feet = { 10.0f, 2.0f, -3.0f };
		h = sw::posed_target(t, p);
		CHECK(h.capsules[0].a[0] == doctest::Approx(10.0 + 0.05 + 0.004 * 14).epsilon(1e-5));
		CHECK(h.capsules[0].a[1] == doctest::Approx(3.65));
		CHECK(h.capsules[2].a[2] == doctest::Approx(-3.1).epsilon(1e-5));
		// The bound sphere holds every capsule.
		for (size_t c = 0; c < h.capsule_count; ++c) {
			for (const sim::Vec3 &e : { h.capsules[c].a, h.capsules[c].b }) {
				const double d = std::sqrt(std::pow(e[0] - h.bound_center[0], 2) + std::pow(e[1] - h.bound_center[1], 2) + std::pow(e[2] - h.bound_center[2], 2));
				CHECK(d + h.capsules[c].radius <= h.bound_radius);
			}
		}
		// A transition: stand -> crouch at 0.5 lies between the rows (fraction steps 3 and 4 of 7).
		p = sw::TargetPose();
		p.pose = sim::Pose::Stand;
		p.target_pose = sim::Pose::Crouch;
		p.transition = 0.5f;
		h = sw::posed_target(t, p);
		CHECK(h.capsules[1].a[1] < 1.3f);
		CHECK(h.capsules[1].a[1] > 0.8f);
		p.transition = 0.0f;
		CHECK(sw::posed_target(t, p).capsules[1].a[1] == doctest::Approx(1.3));
		p.transition = 1.0f;
		CHECK(sw::posed_target(t, p).capsules[1].a[1] == doctest::Approx(0.8));
		p.transition = std::numeric_limits<float>::infinity();
		CHECK(sw::posed_target(t, p).capsules[1].a[1] == doctest::Approx(1.3)); // non-finite: 0
		// Same inputs, same bits.
		CHECK(same_target(sw::posed_target(t, p), sw::posed_target(t, p)));
		// An empty table: no capsules.
		CHECK(sw::posed_target(gp::CapsulePoseTable(), p).capsule_count == 0);
	}

	TEST_CASE("RewindProvider: verdicts, the present path, the swept-bound broad phase never removes a hit") {
		const gp::CapsulePoseTable table = synth_table();
		host::LagCompConfig cfg;
		cfg.pose_eye_heights = kEyes;
		host::LagHistory history(cfg, kPlayers);
		host::RewindProvider prov(history, table, cfg);
		// History: 30 snapshots of six moving soldiers, client 0 acks everything.
		uint16_t seq = 1000;
		std::vector<sim::SoldierState> states(kPlayers);
		for (uint32_t T = 2; T <= 60; T += 2) {
			std::vector<net::OtherSoldier> recs;
			for (uint32_t s = 0; s < kPlayers; ++s) {
				states[s] = state_of(s, T);
				states[s].life = 1;
				states[s].mode = sim::MoveMode::OnGround;
				recs.push_back(net::quantize_other(uint8_t(s), states[s]));
			}
			history.record_tick(T, recs);
			history.sent(0, seq, T, 0x3Eu, 0);
			history.ack(0, seq, 0xFFFFFFFFu, 0);
			++seq;
		}
		const host::AckState at = history.ack_state(0);
		host::ShotView v;
		REQUIRE(history.shot_view(0, at, uint8_t(at.newest & 0xFF), 6 * 256, uint8_t(sim::ViewQuality::Interpolated), v));
		prov.begin_tick(61, states);
		prov.note_shot(0, 1, v);
		host::ShotView far = v;
		far.view_back_q8 = 40 * 256;
		prov.note_shot(0, 2, far);
		host::ShotView none;
		prov.note_shot(0, 3, none);
		prov.note_refused(0, 4);
		CHECK(prov.stats().rewound == 1);
		CHECK(prov.stats().clamped == 1);
		CHECK(prov.stats().present == 1);
		CHECK(prov.stats().refused == 1);
		host::ShotView back;
		CHECK(prov.shot_view_of(0, 1, back));
		CHECK_FALSE(prov.shot_view_of(0, 4, back));
		CHECK_FALSE(prov.shot_view_of(0, 9, back));
		// The present path (None, refused, no view): the present set without the shooter.
		sw::TargetQuery q;
		q.time_start = sw::tick_start(61);
		q.time_end = sw::tick_end(61);
		q.player_slot = 0;
		for (const uint32_t idx : { 3u, 4u, 99u }) {
			q.shot_index = idx;
			const std::span<const sw::HitTarget> a = prov.targets_for(q);
			CHECK(a.size() == kPlayers - 1);
			for (const sw::HitTarget &x : a) {
				CHECK(x.id != 0);
				REQUIRE(prov.present_target(uint8_t(x.id)) != nullptr);
				CHECK(same_target(x, *prov.present_target(uint8_t(x.id))));
			}
		}
		CHECK(prov.stats().no_view == 1);
		// Broad phase: random segments near the soldiers; every target the uncull'd answer is hit by is in the
		// culled answer, posed identically.
		std::mt19937 rng(5);
		size_t hits = 0, culled_hits_lost = 0, segments = 0;
		for (int i = 0; i < 3000; ++i) {
			sw::TargetQuery a = q;
			a.shot_index = 1 + (rng() % 2); // rewound and clamped
			// Aimed near a soldier (its recorded track, +-1.5 m), from up to 8 m away; the segment is 0..8 m long, so it
			// may stop short, pass, or go through.
			const uint32_t s = 1 + rng() % (kPlayers - 1);
			const sim::SoldierState aim_at = state_of(s, 40 + rng() % 20);
			const sim::Vec3 target{ aim_at.position[0] + float(int(rng() % 301) - 150) / 100.0f, float(rng() % 200) / 100.0f,
				aim_at.position[2] + float(int(rng() % 301) - 150) / 100.0f };
			const sim::Vec3 from{ target[0] + float(int(rng() % 1601) - 800) / 100.0f, float(rng() % 250) / 100.0f, target[2] + float(int(rng() % 1601) - 800) / 100.0f };
			const double dx = target[0] - from[0], dy = target[1] - from[1], dz = target[2] - from[2];
			const double dl = std::max(1e-3, std::sqrt(dx * dx + dy * dy + dz * dz));
			const double len = double(rng() % 800) / 100.0;
			a.from = from;
			a.to = { float(from[0] + dx / dl * len), float(from[1] + dy / dl * len), float(from[2] + dz / dl * len) };
			a.has_segment = false;
			const std::span<const sw::HitTarget> all_span = prov.targets_for(a);
			const std::vector<sw::HitTarget> all(all_span.begin(), all_span.end());
			a.has_segment = true;
			const std::span<const sw::HitTarget> some_span = prov.targets_for(a);
			const std::vector<sw::HitTarget> some(some_span.begin(), some_span.end());
			++segments;
			for (const sw::HitTarget &x : all) {
				sw::TargetHit th;
				if (!sw::segment_target(a.from, a.to, x, th)) {
					continue;
				}
				++hits;
				bool found = false;
				for (const sw::HitTarget &y : some) {
					found = found || (y.id == x.id && same_target(x, y));
				}
				culled_hits_lost += found ? 0 : 1;
			}
		}
		std::printf("sim.lagcomp.unit broad phase: %zu segments, %zu target hits, %zu lost to culling; culled %" PRIu64 " of %" PRIu64 " candidates tested\n", segments,
				hits, culled_hits_lost, prov.stats().culled, prov.stats().culled + prov.stats().candidates);
		CHECK(hits > 100);
		CHECK(culled_hits_lost == 0);
		CHECK(prov.stats().culled > 0);
		// enabled false: every shot at the present.
		host::LagCompConfig offc = cfg;
		offc.enabled = false;
		host::RewindProvider pres(history, table, offc);
		pres.begin_tick(61, states);
		pres.note_shot(0, 1, v);
		CHECK(pres.stats().present == 1);
		q.shot_index = 1;
		CHECK(pres.targets_for(q).size() == kPlayers - 1);
		CHECK(pres.stats().rewound_queries == 0);
		// A dead or never-spawned soldier is not a target; a respawned one is not hit at its old body.
		std::vector<sim::SoldierState> st2 = states;
		st2[2].mode = sim::MoveMode::Dead;
		st2[3].life = 2;
		prov.begin_tick(62, st2);
		q.shot_index = 1;
		const std::span<const sw::HitTarget> r = prov.targets_for(q);
		for (const sw::HitTarget &x : r) {
			CHECK(x.id != 2);
			CHECK(x.id != 3);
		}
		CHECK(prov.stats().generation_skips >= 1);
		prov.end_tick();
		CHECK(prov.stats().ticks == 2);
	}

	TEST_CASE("R4: ProjectileWorld fills the TargetQuery segment at its query sites") {
		struct Recorder final : sw::HitTargetProvider {
			mutable std::vector<sw::TargetQuery> queries;
			std::span<const sw::HitTarget> targets_for(const sw::TargetQuery &p_q) const override {
				queries.push_back(p_q);
				return {};
			}
		};
		const std::shared_ptr<const sw::WeaponTable> t = tw::synthetic_table();
		const uint16_t m16 = tw::id_of(*t, "weapons/m16a4/u_m16a4");
		const uint16_t rpg = tw::id_of(*t, "weapons/rpg7/u_rpg7");
		const std::unique_ptr<sim::CollisionWorld> w = tw::wall_world({ 60.0f });
		Recorder rec_;
		sw::ProjectileWorld pw(t);
		sw::ShotEvent s;
		s.def = m16;
		s.origin = { 0, 1.6f, 0 };
		s.aim = { 0, 0, 1 };
		s.direction = { 0, 0, 1 };
		s.seed = 3;
		REQUIRE(pw.spawn(s).ok());
		sw::ShotEvent r = s;
		r.def = rpg;
		r.shot_index = 1;
		r.spawn_time = t->defs[rpg].spawn_delay;
		REQUIRE(pw.spawn(r).ok());
		sw::ProjectileStepOutput out;
		size_t detonations = 0, blasts = 0, flights = 0, m16_segments = 0;
		sim::Vec3 last_to{};
		bool chained = true;
		for (sim::Tick k = 1; k <= 120; ++k) {
			out.clear();
			const size_t before = rec_.queries.size();
			pw.step(k, *w, rec_, out);
			for (size_t i = before; i < rec_.queries.size(); ++i) {
				const sw::TargetQuery &q = rec_.queries[i];
				CHECK(q.has_segment);
				if (q.kind == sw::TargetQueryKind::Flight) {
					++flights;
					CHECK(q.radius == 0.0f);
					if (q.shot_index == 0 && q.def == m16) {
						if (++m16_segments > 1) {
							chained = chained && q.from == last_to; // the segments of one bullet chain
						}
						last_to = q.to;
					}
				} else if (q.kind == sw::TargetQueryKind::Blast) {
					++blasts;
					CHECK(q.from == q.to);
					CHECK(q.radius > 0.0f);
					REQUIRE_FALSE(out.detonations.empty());
					CHECK(q.from == out.detonations.back().position);
				}
			}
			detonations += out.detonations.size();
		}
		CHECK(flights > 2);
		CHECK(blasts == 1);
		CHECK(detonations == 1);
		CHECK(chained);
	}
}
