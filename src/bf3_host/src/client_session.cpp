#include "bf3/host/client_session.h"

#include "client_loop.h"
#include "client_ticks.h"
#include "client_transport.h"
#include "instance_registry.h"

#include "bf3/net/enet_host.h"
#include "bf3/net/loopback_ping.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <span>
#include <string>
#include <thread>

namespace bf3::host {

namespace {

detail::InstanceRegistry<ClientSession> g_registry;

class EnetTransport final : public ClientTransport {
public:
	EnetTransport(std::unique_ptr<net::Host> p_host, uint32_t p_peer) :
			host_(std::move(p_host)), peer_(p_peer) {}

	int service(uint32_t p_wait_ms, net::Host::Event &r_event) override { return host_->service(p_wait_ms, r_event); }
	void send(uint8_t p_channel, std::span<const uint8_t> p_bytes, net::Host::SendMode p_mode) override {
		host_->send(peer_, p_channel, p_bytes.data(), p_bytes.size(), p_mode);
	}
	void flush() override { host_->flush(); }
	void disconnect_all() override { host_->disconnect_all(); }
	uint32_t rtt_ms() const override { return host_->peer_rtt_ms(peer_); }

private:
	std::unique_ptr<net::Host> host_;
	uint32_t peer_ = 0;
};

}

size_t present_others(const PresentationFrame &p_frame, uint64_t p_now_ns, std::span<PresentedSoldier> r_out, uint64_t *r_extrapolated) {

	PresentOthersCounts counts;
	const size_t n = present_others(p_frame, p_now_ns, r_out, counts);
	if (r_extrapolated != nullptr) {
		*r_extrapolated += counts.extrapolated;
	}
	return n;
}

double present_own_fraction(const PresentationFrame &p_frame, uint64_t p_now_ns) {
	const double dt_ns = p_now_ns >= p_frame.publish_ns ? double(p_now_ns - p_frame.publish_ns) : 0.0;
	return std::clamp(p_frame.tick_fraction + dt_ns * p_frame.own_ticks_per_ns, 0.0, 1.0);
}

struct ClientSession::Impl {
	std::thread thread;
	std::mutex lifecycle_mutex;
	bool started = false;
	std::atomic<bool> stop_requested{ false };
	std::atomic<bool> running{ false };
	std::atomic<uint32_t> stall_request_ms{ 0 };
	ClientLoop loop;

	void run() {
		while (!stop_requested.load(std::memory_order_acquire)) {
			if (const uint32_t stall = stall_request_ms.exchange(0); stall > 0) {
				std::this_thread::sleep_for(std::chrono::milliseconds(stall));
			}
			loop.iterate();
		}
		loop.shutdown();
	}
};

ClientSession::ClientSession() :
		impl_(std::make_unique<Impl>()) {}

ClientSession::~ClientSession() {
	g_registry.retire(this);
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

	Impl &m = *impl_;
	BF3_TRY(m.loop.init(p_config, p_level));
	std::string err;
	std::unique_ptr<net::Host> host = net::Host::create_client(net::kChannelCount, &err);
	if (host == nullptr) {
		return core::make_error("ClientSession: " + err);
	}
	const int peer = host->connect(p_config.host.c_str(), p_config.port);
	if (peer < 0) {
		return core::make_error("ClientSession: cannot connect to " + p_config.host);
	}
	const uint64_t t0 = net::monotonic_ns();
	bool connected = false;
	while (!connected && net::monotonic_ns() - t0 < uint64_t(p_config.connect_timeout_ms) * 1'000'000ull) {
		net::Host::Event ev;
		const int r = host->service(10, ev);
		if (r < 0) {
			break;
		}
		connected = r > 0 && ev.type == net::Host::EventType::Connect;
	}
	if (!connected) {
		return core::make_error("ClientSession: no connection to " + p_config.host + ":" + std::to_string(p_config.port) + " within " +
				std::to_string(p_config.connect_timeout_ms) + " ms");
	}
	m.stop_requested = false;
	m.loop.begin(std::make_unique<EnetTransport>(std::move(host), uint32_t(peer)), net::monotonic_ns);
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
	impl_->loop.close();
	impl_->running = false;
	g_registry.remove(this);
}

bool ClientSession::running() const {
	return impl_->running.load(std::memory_order_acquire);
}

void ClientSession::set_input(const InputSample &p_input) {
	impl_->loop.set_input(p_input);
}

bool ClientSession::read_presentation(PresentationFrame &r_frame) const {
	return impl_->loop.read_presentation(r_frame);
}

ClientStats ClientSession::stats() const {
	return impl_->loop.stats();
}

void ClientSession::send_debug(const net::DebugRequest &p_request) {
	impl_->loop.send_debug(p_request);
}

bool ClientSession::take_debug_report(net::DebugReport &r_report) {
	return impl_->loop.take_debug_report(r_report);
}

void ClientSession::debug_shift_clock(int32_t p_ticks) {
	impl_->loop.shift_clock(p_ticks);
}

void ClientSession::debug_stall(uint32_t p_ms) {
	impl_->stall_request_ms.store(p_ms, std::memory_order_release);
}

PresentationEvents &ClientSession::events() {
	return impl_->loop.events();
}

void ClientSession::take_predicted(std::vector<PredictedShot> &r_shots, std::vector<PredictedTimed> &r_timed) {
	impl_->loop.take_predicted(r_shots, r_timed);
}

void ClientSession::take_own_test_samples(std::vector<OwnTestSample> &r_samples) {
	impl_->loop.take_own_test_samples(r_samples);
}

void ClientSession::send_kit_select(const net::KitSelect &p_select) {
	impl_->loop.send_kit_select(p_select);
}

void ClientSession::send_spawn_request(uint8_t p_kit) {
	impl_->loop.send_spawn_request(p_kit);
}

size_t ClientSession::stop_all() {
	return g_registry.for_each_pinned([](ClientSession *p_session) { p_session->stop(); });
}

}

#include "bf3/host/gunplay.h"
#include "bf3/net/other_sample.h"
#include "bf3/sim/weapons/weapon_rng.h"

namespace bf3::host {

namespace {

PresentedSoldier presented_of(const net::OtherPose &p_pose) {
	PresentedSoldier s;
	s.slot = p_pose.slot;
	s.generation = p_pose.generation;
	s.position = p_pose.position;
	s.velocity = p_pose.velocity;
	s.yaw = p_pose.yaw;
	s.pitch = p_pose.pitch;
	s.pose = sim::Pose(std::min<uint8_t>(p_pose.pose, uint8_t(sim::kPoseCount - 1)));
	s.eye_height = p_pose.eye_height;
	s.mode = sim::MoveMode(p_pose.mode);
	s.flags = p_pose.flags;
	s.extrapolated = p_pose.extrapolated;
	s.target_pose = sim::Pose(std::min<uint8_t>(p_pose.target_pose, uint8_t(sim::kPoseCount - 1)));
	s.transition = float(p_pose.transition) / 255.0f;
	return s;
}

size_t window_of(const OtherTrack &p_track, std::array<net::TimedOther, kOtherSamples> &r_window) {
	const size_t used = std::min<size_t>(p_track.count, kOtherSamples);
	for (size_t k = 0; k < used; ++k) {
		r_window[k].server_tick = p_track.samples[k].server_tick;
		r_window[k].soldier = p_track.samples[k].soldier;
	}
	return used;
}

}

double present_render_ticks(const PresentationFrame &p_frame, uint64_t p_now_ns) {
	const double dt_ns = p_now_ns >= p_frame.publish_ns ? double(p_now_ns - p_frame.publish_ns) : -double(p_frame.publish_ns - p_now_ns);
	return p_frame.render_ticks_at_publish + dt_ns * p_frame.ticks_per_ns;
}

size_t present_others(const PresentationFrame &p_frame, uint64_t p_now_ns, std::span<PresentedSoldier> r_out, PresentOthersCounts &r_counts) {
	const double render = present_render_ticks(p_frame, p_now_ns);
	const double max_extra_ticks = std::max(0.0, double(p_frame.extrapolate_max_ms)) / kMsPerTick;
	size_t n = 0;
	const uint32_t count = std::min<uint32_t>(p_frame.other_count, uint32_t(p_frame.others.size()));
	std::array<net::TimedOther, kOtherSamples> window;
	for (uint32_t i = 0; i < count && n < r_out.size(); ++i) {
		const OtherTrack &t = p_frame.others[i];
		const size_t used = window_of(t, window);
		if (used == 0) {
			continue;
		}
		const net::OtherPose pose = net::sample_other(std::span<const net::TimedOther>(window.data(), used), render, p_frame.pose_eye_heights, max_extra_ticks);
		if (pose.extrapolated) {
			++r_counts.extrapolated;
			if (t.track_newest_tick > window[used - 1].server_tick) {
				++r_counts.window_extrapolated;
			}
		}
		if (pose.held) {
			++r_counts.held;
		}
		r_out[n++] = presented_of(pose);
	}
	return n;
}

sim::ViewQuality present_view_quality(const PresentationFrame &p_frame, double p_render_ticks) {
	if (p_frame.serial == 0) {
		return sim::ViewQuality::None;
	}
	const double max_extra_ticks = std::max(0.0, double(p_frame.extrapolate_max_ms)) / kMsPerTick;
	sim::ViewQuality q = sim::ViewQuality::Interpolated;
	const uint32_t count = std::min<uint32_t>(p_frame.other_count, uint32_t(p_frame.others.size()));
	for (uint32_t i = 0; i < count; ++i) {
		const OtherTrack &t = p_frame.others[i];
		const size_t used = std::min<size_t>(t.count, kOtherSamples);
		if (used == 0) {
			continue;
		}
		const double ahead = p_render_ticks - double(t.samples[used - 1].server_tick);
		if (ahead > max_extra_ticks) {
			return sim::ViewQuality::Held;
		}
		if (ahead > 0.0) {
			q = sim::ViewQuality::Extrapolated;
		}
	}
	return q;
}

void set_view_sample(InputSample &r_input, const PresentationFrame &p_frame, uint64_t p_now_ns) {
	r_input.view_valid = p_frame.serial != 0;
	r_input.view_render_ticks = present_render_ticks(p_frame, p_now_ns);
	r_input.view_quality = uint8_t(present_view_quality(p_frame, r_input.view_render_ticks));
	r_input.view_seq = p_frame.newest_seq;
}

core::Result<std::shared_ptr<const sim::weapons::WeaponTable>> build_client_weapon_table(const ClientGunplayInputs &p_inputs,
		const std::string &p_mechanics_cfg, uint8_t p_weapon_flags) {
	if (p_inputs.table == nullptr) {
		return core::make_error("client weapon table: no level weapon table");
	}
	auto t = std::make_shared<sim::weapons::WeaponTable>(*p_inputs.table);
	if (p_inputs.load_mechanics) {
		core::Result<sim::weapons::Mechanics> m = p_inputs.load_mechanics(p_mechanics_cfg);
		if (!m) {
			return core::error_at("client weapon table: " + p_mechanics_cfg, m.error());
		}
		t->mechanics = *m;
	}
	t->exact_aim = (p_weapon_flags & net::weapon_flags::kExactAim) != 0;
	t->infinite_magazine = (p_weapon_flags & net::weapon_flags::kInfiniteMagazine) != 0;
	t->hash = sim::weapons::weapon_table_hash(*t);
	return std::shared_ptr<const sim::weapons::WeaponTable>(std::move(t));
}

uint64_t client_weapon_hash(const sim::weapons::WeaponTable &p_table, const core::gameplay::CapsulePoseTable &p_capsules) {
	return sim::weapons::weapon_table_hash(p_table) ^ p_capsules.hash;
}

core::Result<std::shared_ptr<const ClientGunplayInputs>> client_gunplay_inputs(const SharedLevel &p_level) {
	if (p_level.gunplay == nullptr || p_level.static_world == nullptr) {
		return core::make_error("client gunplay inputs: the level has no gunplay tables");
	}
	const GunplayLevel &g = *p_level.gunplay;
	core::Result<std::shared_ptr<const sim::weapons::WeaponTable>> table = make_match_table(g, sim::weapons::Mechanics{}, false, false);
	if (!table) {
		return core::error_at("client gunplay inputs", table.error());
	}
	auto in = std::make_shared<ClientGunplayInputs>();
	in->table = *table;
	in->capsules = std::make_shared<const core::gameplay::CapsulePoseTable>(g.capsule_table);
	in->kits = g.kits;
	in->shot_frame = shot_frame(*p_level.static_world);
	in->load_mechanics = load_mechanics_cfg;
	return std::shared_ptr<const ClientGunplayInputs>(std::move(in));
}

}

namespace bf3::host {

uint32_t OwnShotIds::emit(uint32_t p_shot_index) {
	if (!any_ || p_shot_index <= last_real_) {

		if (any_) {
			++resets_;
		}
		const uint64_t offset = next_presented_ > p_shot_index ? next_presented_ - p_shot_index : 0u;
		if (segment_count_ == kSegments) {
			std::copy(segments_.begin() + 1, segments_.end(), segments_.begin());
			--segment_count_;
		}
		segments_[segment_count_++] = { p_shot_index, uint32_t(offset) };
		any_ = true;
	}
	last_real_ = p_shot_index;
	const uint32_t presented = uint32_t(uint64_t(p_shot_index) + segments_[segment_count_ - 1].offset);
	next_presented_ = std::max<uint64_t>(next_presented_, uint64_t(presented) + 1u);
	recent_[recent_next_] = { p_shot_index, presented };
	recent_next_ = (recent_next_ + 1) % kRecent;
	recent_count_ = std::min(recent_count_ + 1, kRecent);
	return presented;
}

uint32_t OwnShotIds::lookup(uint32_t p_shot_index) const {
	for (size_t k = 1; k <= recent_count_; ++k) {
		const Pair &p = recent_[(recent_next_ + kRecent - k) % kRecent];
		if (p.real == p_shot_index) {
			return p.presented;
		}
	}
	for (size_t k = segment_count_; k > 0; --k) {
		const Segment &s = segments_[k - 1];
		if (s.from <= p_shot_index) {
			return uint32_t(uint64_t(p_shot_index) + s.offset);
		}
	}
	return p_shot_index;
}

bool present_own_shot_id(OwnShotIds &r_ids, uint8_t p_own_slot, PresentEvent &r_event) {
	if (r_event.slot != p_own_slot) {
		return false;
	}
	uint32_t id = r_event.id;
	switch (r_event.kind) {
		case PresentKind::OwnShot:
			id = r_ids.emit(r_event.id);
			break;
		case PresentKind::Impact:
		case PresentKind::SoldierContact:
			if (r_event.timeline != PresentTimeline::Predicted) {
				return false;
			}
			id = r_ids.lookup(r_event.id);
			break;
		case PresentKind::Hit:
			if ((r_event.flags & present_flags::kOwnShooter) == 0) {
				return false;
			}
			id = r_ids.lookup(r_event.id);
			break;
		case PresentKind::Death:
			if ((r_event.flags & present_flags::kOwnShooter) == 0 || r_event.def == sim::weapons::kNoWeapon) {
				return false;
			}
			id = r_ids.lookup(r_event.id);
			break;
		case PresentKind::Detonation:
			id = r_ids.lookup(r_event.id);
			break;
		default:
			return false;
	}
	const bool changed = id != r_event.id;
	r_event.id = id;
	return changed;
}

void apply_player_list(PlayerTable &r_table, const net::PlayerList &p_list) {
	if (p_list.full) {
		r_table.present.fill(false);
	}
	for (const net::PlayerRow &row : p_list.rows) {
		if (row.slot < net::kMaxPlayerRows) {
			r_table.rows[row.slot] = row;
			r_table.present[row.slot] = true;
		}
	}
	if (!p_list.full) {
		for (const uint8_t slot : p_list.removed) {
			if (slot < net::kMaxPlayerRows) {
				r_table.present[slot] = false;
			}
		}
	}
}

uint8_t player_rows(const PlayerTable &p_table, std::array<net::PlayerRow, net::kMaxPlayerRows> &r_rows) {
	uint8_t n = 0;
	for (size_t s = 0; s < net::kMaxPlayerRows; ++s) {
		if (p_table.present[s]) {
			r_rows[n++] = p_table.rows[s];
		}
	}
	for (size_t k = n; k < r_rows.size(); ++k) {
		r_rows[k] = net::PlayerRow{};
	}
	return n;
}

const char *rules_refusal(const net::MatchInfo &p_match, bool p_has_logic, uint64_t p_logic_digest) {
	if ((p_match.rules_flags & net::rules_flags::kTdm) == 0) {
		return nullptr;
	}
	if (!p_has_logic) {
		return "rules mismatch (the server runs TDM rules; this client's load has no logic graph)";
	}
	if (p_match.logic_digest != 0 && p_match.logic_digest != p_logic_digest) {
		return "rules mismatch (logic digest)";
	}
	return nullptr;
}

}
