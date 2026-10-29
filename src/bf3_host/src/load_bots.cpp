#include "bf3/host/load_bots.h"

#include "bf3/core/math.h"
#include "bf3/core/util/splitmix64.h"
#include "bf3/host/client_session.h"
#include "bf3/net/loopback_ping.h"
#include "bf3/net/protocol.h"
#include "client_clock.h"
#include "client_link.h"
#include "client_transport.h"
#include "server_slots.h"
#include "tick_timer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace bf3::host {

sim::Command LegWalker::command(sim::Tick p_tick, const sim::SoldierState &p_state) {
	if (!started_) {
		started_ = true;
		base_yaw_ = p_state.yaw;
		start_ = p_tick;
	}
	sim::Command c;
	c.tick = p_tick;
	c.move_forward = sim::kAxisMax;
	const uint32_t leg = ((p_tick - start_) / kLegTicks + phase_) & 3u;
	c.yaw = uint16_t(base_yaw_ + leg * 16384u);
	c.pose = sim::Pose::Stand;
	return c;
}

namespace {

constexpr uint64_t kPollNs = 2'000'000ull;
constexpr uint64_t kPublishNs = 10'000'000ull;

class CountingTransport final : public ClientTransport {
public:
	CountingTransport(std::unique_ptr<ClientTransport> p_inner, LoadBotStats &r_stats) :
			inner_(std::move(p_inner)), stats_(r_stats) {}

	int service(uint32_t p_wait_ms, net::Host::Event &r_event) override {
		const int r = inner_->service(p_wait_ms, r_event);
		if (r > 0 && r_event.type == net::Host::EventType::Receive) {
			++stats_.datagrams_in;
			stats_.bytes_in += r_event.data.size();
		}
		return r;
	}
	void send(uint8_t p_channel, std::span<const uint8_t> p_bytes, net::Host::SendMode p_mode) override {
		++stats_.datagrams_out;
		stats_.bytes_out += p_bytes.size();
		inner_->send(p_channel, p_bytes, p_mode);
	}
	void flush() override { inner_->flush(); }
	void disconnect_all() override { inner_->disconnect_all(); }
	uint32_t rtt_ms() const override { return inner_->rtt_ms(); }

private:
	std::unique_ptr<ClientTransport> inner_;
	LoadBotStats &stats_;
};

bool decodes(net::MessageType p_type, std::span<const uint8_t> p_bytes) {
	using net::MessageType;
	const net::ShotQuantization frame{};
	switch (p_type) {
		case MessageType::MatchState:
			return net::decode_match_state(p_bytes).ok();
		case MessageType::PlayerList:
			return net::decode_player_list(p_bytes).ok();
		case MessageType::SoldierGear:
			return net::decode_soldier_gear(p_bytes).ok();
		case MessageType::DebugReport:
			return net::decode_debug_report(p_bytes).ok();
		case MessageType::RoundOverInfo:
			return net::decode_round_over_info(p_bytes).ok();
		case MessageType::Prepare:
			return net::decode_prepare(p_bytes).ok();
		case MessageType::LevelChange:
			return net::decode_level_change(p_bytes).ok();
		case MessageType::LogicEvents:
			return net::decode_logic_events(p_bytes).ok();
		case MessageType::FullSync:
			return net::decode_full_sync(p_bytes).ok();
		case MessageType::LogicDelta:
			return net::decode_logic_delta(p_bytes).ok();
		case MessageType::SquadOrder:
			return net::decode_squad_order(p_bytes).ok();
		case MessageType::ShotEvents:
			return net::decode_shot_events(p_bytes, frame).ok();
		case MessageType::CombatEvents:
			return net::decode_combat_events(p_bytes, frame).ok();
		default:
			return false;
	}
}

}

struct LoadBotFleet::Impl {
	struct Bot {
		Bot(const MonotonicClock &p_clock, const LoadBotConfig &p_config, uint32_t p_index) :
				config(p_config), clock(session), link(p_clock), index(p_index) {
			config.name = p_config.name + std::to_string(p_index);
			net::NetSimConfig netsim = p_config.netsim;
			netsim.seed = core::splitmix64(netsim.seed + p_index);
			link.configure(netsim);
		}

		LoadBotConfig config;
		ClientSessionConfig session;
		ClientClock clock;
		ClientLink link;
		uint32_t index = 0;
		LoadBotStats stats;
		bool deploy_screen = false;
		uint32_t next_sequence = 1;
		std::optional<sim::SoldierState> own;
		uint32_t own_life = 0;
		sim::Vec3 spawn_at{};
		std::unique_ptr<LegWalker> walker;
		uint64_t last_spawn_request_ns = 0;
		bool ended = false;

		void begin(std::unique_ptr<ClientTransport> p_transport, uint64_t p_now) {
			link.attach(std::make_unique<CountingTransport>(std::move(p_transport), stats));
			stats.connected = true;
			stats.connected_ns = p_now;
			net::Hello hello;
			hello.game_build = config.game_build.value_or(net::game_build());
			hello.name = config.name;
			clock.hello_sent(p_now);
			link.submit(net::kChannelReliable, net::encode_hello(hello), p_now);
			link.release(p_now);
		}

		void match_info(std::span<const uint8_t> p_bytes, uint64_t p_now) {
			core::Result<net::MatchInfo> m = net::decode_match_info(p_bytes);
			if (!m) {
				++stats.decode_errors;
				return;
			}
			++stats.reliable_messages;
			if (stats.matched || stats.refused) {
				return;
			}
			stats.slot = m->slot;
			if (!config.level.empty()) {
				if (const std::string refusal = level_refusal(*m, config.level, config.mode); !refusal.empty()) {
					link.refuse(refusal.c_str());
					stats.refused = true;
					return;
				}
			}
			stats.matched = true;
			deploy_screen = (m->rules_flags & net::rules_flags::kDeployScreen) != 0;
			clock.start(m->server_tick, p_now, next_sequence);
			link.submit(net::kChannelReliable, net::encode_ready({}), p_now);
		}

		void snapshot(std::span<const uint8_t> p_bytes, uint64_t p_now) {
			core::Result<net::Snapshot> s = net::decode_snapshot(p_bytes);
			if (!s) {
				++stats.decode_errors;
				return;
			}
			++stats.snapshots;
			if (!stats.matched) {
				return;
			}
			clock.rtt_sample(*s, p_now, next_sequence);
			if (link.arrive(s->seq) != SnapshotOrder::Newest) {
				return;
			}
			clock.anchor_snapshot(s->server_tick, p_now);
			if (!s->own.has_value()) {
				return;
			}
			clock.buffer_report(*s, p_now, next_sequence);
			own = *s->own;
			if (soldier_alive(*own) && own->life != own_life) {
				own_life = own->life;
				spawn_at = own->position;
				walker = std::make_unique<LegWalker>(index);
				++stats.lives;
			}
			if (soldier_alive(*own) && own->life == own_life) {
				stats.max_walk_m =
						std::max(stats.max_walk_m, core::vec::length(core::vec::sub(spawn_at, own->position)));
			}
		}

		void handle(const net::NetSim::Datagram &p_d, uint64_t p_now) {
			const std::optional<net::MessageType> type = net::peek_type(p_d.bytes);
			if (!type) {
				++stats.decode_errors;
				return;
			}
			switch (*type) {
				case net::MessageType::MatchInfo:
					match_info(p_d.bytes, p_now);
					return;
				case net::MessageType::Snapshot:
					snapshot(p_d.bytes, p_now);
					return;
				case net::MessageType::SnapshotOthers:
					if (net::decode_snapshot_others(p_d.bytes)) {
						++stats.snapshot_others;
					} else {
						++stats.decode_errors;
					}
					return;
				case net::MessageType::Spawned: {
					core::Result<net::Spawned> s = net::decode_spawned(p_d.bytes);
					if (!s) {
						++stats.decode_errors;
						return;
					}
					++stats.reliable_messages;
					if (stats.matched && s->slot == stats.slot) {
						clock.anchor_spawned(s->state.tick, p_now);
					}
					return;
				}
				case net::MessageType::Kick: {
					core::Result<net::Kick> k = net::decode_kick(p_d.bytes);
					stats.decode_errors += k ? 0u : 1u;
					link.drop("kicked: " + (k ? k->reason : std::string("(bad kick)")));
					return;
				}
				case net::MessageType::Bye:
					link.drop("the server said bye");
					return;
				default:
					if (!decodes(*type, p_d.bytes)) {
						++stats.decode_errors;
					} else if (*type == net::MessageType::ShotEvents) {
						++stats.shot_events;
					} else if (*type == net::MessageType::CombatEvents) {
						++stats.combat_events;
					} else {
						++stats.reliable_messages;
					}
					return;
			}
		}

		void command(uint64_t p_now) {
			if (!stats.matched || !link.connected() || !clock.running() || !clock.due(p_now)) {
				return;
			}
			const sim::Tick c = clock.next_tick();
			if (c > 0 && own.has_value() && soldier_alive(*own) && walker != nullptr) {
				sim::Command cmd = walker->command(c, *own);
				cmd.sequence = next_sequence++;
				clock.command_sent(cmd.sequence, p_now);
				link.send_commands(cmd, 0, p_now);
			}
			clock.set_next_tick(c + 1u);
		}

		void request_spawn(uint64_t p_now) {
			if (!stats.matched || !deploy_screen || !link.connected() || (own.has_value() && soldier_alive(*own))) {
				return;
			}
			if (last_spawn_request_ns != 0 &&
					p_now - last_spawn_request_ns < uint64_t(config.spawn_retry_ms) * 1'000'000ull) {
				return;
			}
			last_spawn_request_ns = p_now;
			link.queue_spawn_request(config.kit);
		}

		void iterate(uint64_t p_now) {
			if (ended) {
				return;
			}
			link.receive(0);
			net::NetSim::Datagram d;
			while (link.poll(p_now, d)) {
				handle(d, p_now);
			}
			request_spawn(p_now);
			link.send_requests(p_now);
			link.release(p_now);
			command(p_now);
			if (!link.connected()) {
				ended = true;
				stats.disconnected = !stats.refused;
				stats.ended_ns = p_now;
			}
		}

		uint64_t deadline(uint64_t p_now) const {
			uint64_t at = p_now + kPollNs;
			if (!ended && clock.running()) {
				at = std::min(at, clock.tick_deadline_ns(clock.next_tick()));
			}
			return ended ? at : link.wait_deadline(at);
		}

		LoadBotStats published() const {
			LoadBotStats s = stats;
			ClientStats c;
			link.stats(c);
			s.connected = stats.connected && !ended;
			s.snapshots_lost = c.snapshots_lost;
			s.commands_sent = c.commands_sent;
			s.spawn_requests = c.spawn_requests;
			s.last_error = c.last_error;
			s.refused = stats.refused || c.refused;
			return s;
		}
	};

	MonotonicClock clock;
	std::vector<std::unique_ptr<Bot>> bots;
	std::thread thread;
	std::atomic<bool> stop_requested{ false };
	std::atomic<bool> running{ false };
	uint64_t start_ns = 0;

	mutable std::mutex stats_mutex;
	std::vector<LoadBotStats> published;
	uint64_t published_ns = 0;

	void publish(uint64_t p_now) {
		std::vector<LoadBotStats> out;
		out.reserve(bots.size());
		for (const std::unique_ptr<Bot> &b : bots) {
			out.push_back(b->published());
		}
		std::lock_guard lock(stats_mutex);
		published = std::move(out);
		published_ns = p_now;
	}

	void run() {
		detail::TickTimer timer;
		uint64_t last_publish = 0;
		while (!stop_requested.load(std::memory_order_acquire)) {
			const uint64_t now = clock();
			uint64_t next = now + kPollNs;
			for (const std::unique_ptr<Bot> &b : bots) {
				b->iterate(now);
				next = std::min(next, b->deadline(now));
			}
			if (now - last_publish >= kPublishNs) {
				publish(now);
				last_publish = now;
			}
			const uint64_t after = clock();
			if (next > after) {
				timer.sleep_until(std::chrono::steady_clock::now() + std::chrono::nanoseconds(next - after));
			}
		}
		const uint64_t end = clock();
		for (const std::unique_ptr<Bot> &b : bots) {
			if (!b->ended) {
				b->stats.ended_ns = end;
			}
			b->link.shutdown();
			b->ended = true;
		}
		publish(end);
		for (const std::unique_ptr<Bot> &b : bots) {
			b->link.close();
		}
		running.store(false, std::memory_order_release);
	}
};

LoadBotFleet::LoadBotFleet() : impl_(std::make_unique<Impl>()) {}

LoadBotFleet::~LoadBotFleet() {
	stop();
}

core::Status LoadBotFleet::start(const LoadBotFleetConfig &p_config) {
	if (impl_->thread.joinable()) {
		return core::make_error("LoadBotFleet: already started");
	}
	std::vector<std::unique_ptr<ClientTransport>> transports;
	for (uint32_t i = 0; i < p_config.count; ++i) {
		core::Result<std::unique_ptr<ClientTransport>> t =
				connect_enet_transport(EnetConnect{ p_config.host, p_config.port, p_config.connect_timeout_ms, 0, 0 });
		if (!t) {
			for (std::unique_ptr<ClientTransport> &done : transports) {
				done->disconnect_all();
			}
			return core::make_error("LoadBotFleet: bot " + std::to_string(i) + ": " + t.error().message);
		}
		transports.push_back(std::move(*t));
	}
	return start(p_config, std::move(transports), net::monotonic_ns);
}

core::Status LoadBotFleet::start(const LoadBotFleetConfig &p_config,
		std::vector<std::unique_ptr<ClientTransport>> p_transports, std::function<uint64_t()> p_clock) {
	Impl &m = *impl_;
	if (m.thread.joinable()) {
		return core::make_error("LoadBotFleet: already started");
	}
	if (p_transports.size() != p_config.count || p_config.count == 0 || p_config.count > net::kMaxSlots) {
		return core::make_error("LoadBotFleet: " + std::to_string(p_transports.size()) + " transports for " +
				std::to_string(p_config.count) + " bots (1.." + std::to_string(net::kMaxSlots) + ")");
	}
	if (!p_clock) {
		return core::make_error("LoadBotFleet: no clock");
	}
	m.clock = std::move(p_clock);
	m.bots.clear();
	m.start_ns = m.clock();
	for (uint32_t i = 0; i < p_config.count; ++i) {
		m.bots.push_back(std::make_unique<Impl::Bot>(m.clock, p_config.bot, i));
		m.bots.back()->begin(std::move(p_transports[i]), m.start_ns);
	}
	m.publish(m.start_ns);
	m.stop_requested = false;
	m.running = true;
	m.thread = std::thread([&m] { m.run(); });
	return core::Status::success();
}

void LoadBotFleet::stop() {
	Impl &m = *impl_;
	if (!m.thread.joinable()) {
		return;
	}
	m.stop_requested = true;
	m.thread.join();
}

bool LoadBotFleet::running() const {
	return impl_->running.load(std::memory_order_acquire);
}

LoadBotFleetStats LoadBotFleet::stats() const {
	const Impl &m = *impl_;
	LoadBotFleetStats f;
	uint64_t at = 0;
	{
		std::lock_guard lock(m.stats_mutex);
		f.bots = m.published;
		at = m.published_ns;
	}
	f.seconds = at > m.start_ns ? double(at - m.start_ns) * 1e-9 : 0.0;
	for (const LoadBotStats &b : f.bots) {
		f.matched += b.matched ? 1u : 0u;
		f.spawned += b.lives > 0 ? 1u : 0u;
		f.disconnected += b.disconnected ? 1u : 0u;
		f.refused += b.refused ? 1u : 0u;
		f.snapshots += b.snapshots;
		f.decode_errors += b.decode_errors;
		f.bytes_in += b.bytes_in;
		f.bytes_out += b.bytes_out;
	}
	if (f.seconds > 0.0) {
		f.bytes_in_per_s = double(f.bytes_in) / f.seconds;
		f.bytes_out_per_s = double(f.bytes_out) / f.seconds;
	}
	return f;
}

}
