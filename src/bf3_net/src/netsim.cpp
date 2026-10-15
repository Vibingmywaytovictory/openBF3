// Network-conditions simulator (netsim.h; docs/design/m1.md §7.6; topics/05 §1.1 Game.cfg knobs). Deterministic:
// one splitmix64 stream per instance, seeded from the config, drawn in a fixed order per submit, so the same seed
// and the same submit/poll times give the same deliveries.
#include "bf3/net/netsim.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <system_error>

namespace bf3::net {

namespace {

// Pending::order's top bit marks a datagram held back by the reorder knob until the next unreliable datagram to
// the same peer that is not held itself is scheduled (the held ones are then released 1 ns after it, in their own
// order), or until its hold time runs out.
constexpr uint64_t kHeldBit = uint64_t(1) << 63;
constexpr double kHoldTimeoutMs = 100.0; // plus the jitter span: a held datagram never waits forever
constexpr double kMaxLatencyMs = 10'000.0;
constexpr double kMaxSpikeMs = 60'000.0;
constexpr double kMaxRttMs = 2 * kMaxLatencyMs;

uint64_t splitmix64(uint64_t &r_state) {
	uint64_t z = (r_state += 0x9E3779B97F4A7C15ull);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

double uniform01(uint64_t &r_state) {
	return double(splitmix64(r_state) >> 11) * (1.0 / 9007199254740992.0); // [0, 1), 53 bits
}

uint64_t ms_to_ns(double p_ms) {
	return p_ms <= 0.0 ? 0 : uint64_t(p_ms * 1.0e6 + 0.5);
}

std::string_view trim(std::string_view p_s) {
	while (!p_s.empty() && (p_s.front() == ' ' || p_s.front() == '\t')) {
		p_s.remove_prefix(1);
	}
	while (!p_s.empty() && (p_s.back() == ' ' || p_s.back() == '\t')) {
		p_s.remove_suffix(1);
	}
	return p_s;
}

bool parse_number(std::string_view p_s, double &r_value) {
	p_s = trim(p_s);
	if (p_s.empty()) {
		return false;
	}
	const char *end = p_s.data() + p_s.size();
	const std::from_chars_result r = std::from_chars(p_s.data(), end, r_value);
	return r.ec == std::errc() && r.ptr == end && std::isfinite(r_value) && r_value >= 0.0;
}

// "a" or "a-b" (no negative values, so the first '-' separates).
bool parse_range(std::string_view p_s, double &r_min, double &r_max) {
	const size_t dash = p_s.find('-');
	if (dash == std::string_view::npos) {
		if (!parse_number(p_s, r_min)) {
			return false;
		}
		r_max = r_min;
		return true;
	}
	return parse_number(p_s.substr(0, dash), r_min) && parse_number(p_s.substr(dash + 1), r_max) && r_min <= r_max;
}

core::Error bad(std::string_view p_profile, const char *p_why) {
	return core::make_error("netsim profile \"" + std::string(p_profile) + "\": " + p_why);
}

void append_number(std::string &r_out, double p_value) {
	char buf[64];
	const std::to_chars_result r = std::to_chars(buf, buf + sizeof(buf), float(p_value)); // shortest round trip
	r_out.append(buf, r.ptr);
}

} // namespace

core::Result<NetSimConfig> parse_netsim(std::string_view p_profile) {
	const std::string_view profile = trim(p_profile);
	NetSimConfig c;
	if (profile.empty() || profile == "off") {
		return c;
	}
	size_t start = 0;
	bool first = true;
	while (start <= profile.size()) {
		const size_t comma = std::min(profile.find(',', start), profile.size());
		const std::string_view token = trim(profile.substr(start, comma - start));
		start = comma + 1;
		if (token.empty()) {
			return bad(p_profile, "empty item");
		}
		const size_t eq = token.find('=');
		if (eq == std::string_view::npos) {
			if (!first) {
				return bad(p_profile, "a base profile (off, bf3, <rtt_ms>/<loss_percent>) must come first");
			}
			if (token == "off") {
				c = NetSimConfig{};
			} else if (token == "bf3") {
				// Game.cfg's IsInternetSimulation values (topics/05 §1.1): MinLatency 0.1, MaxLatency 0.2,
				// PacketDrops 0.01, DropSpikeChance 0.03, Min/MaxDropDuration 0.3/1.0, ReorderingChance 0.01,
				// DuplicationChance 0.02 (CorruptionChance is not modelled: ENet checks its own framing).
				c.latency_min_ms = 100;
				c.latency_max_ms = 200;
				c.loss = 0.01f;
				c.spike_chance = 0.03f;
				c.spike_min_ms = 300;
				c.spike_max_ms = 1000;
				c.reorder = 0.01f;
				c.duplicate = 0.02f;
			} else {
				const size_t slash = token.find('/');
				double rtt = 0, loss = 0;
				if (slash == std::string_view::npos || !parse_number(token.substr(0, slash), rtt) ||
						!parse_number(token.substr(slash + 1), loss)) {
					return bad(p_profile, "expected off, bf3, <rtt_ms>/<loss_percent> or key=value items");
				}
				if (rtt > kMaxRttMs || loss > 100.0) {
					return bad(p_profile, "round trip above 20,000 ms or loss above 100 %");
				}
				// The M1 reading of "150 ms / 2 %" (docs §7.6): half the round trip each way, +-10 % jitter, the loss
				// in each direction.
				const double one_way = rtt * 0.5;
				c.latency_min_ms = float(one_way * 0.9);
				c.latency_max_ms = float(one_way * 1.1);
				c.loss = float(loss / 100.0);
			}
			first = false;
			continue;
		}
		first = false;
		const std::string_view key = trim(token.substr(0, eq));
		const std::string_view value = trim(token.substr(eq + 1));
		double a = 0, b = 0;
		if (key == "lat") {
			if (!parse_range(value, a, b) || b > kMaxLatencyMs) {
				return bad(p_profile, "lat wants <ms> or <min_ms>-<max_ms>, at most 10,000 ms");
			}
			c.latency_min_ms = float(a);
			c.latency_max_ms = float(b);
		} else if (key == "spike_ms") {
			if (!parse_range(value, a, b) || b > kMaxSpikeMs) {
				return bad(p_profile, "spike_ms wants <ms> or <min_ms>-<max_ms>, at most 60,000 ms");
			}
			c.spike_min_ms = float(a);
			c.spike_max_ms = float(b);
		} else if (key == "loss" || key == "spike" || key == "reorder" || key == "dup") {
			if (!parse_number(value, a) || a > 100.0) {
				return bad(p_profile, "percentages are 0..100");
			}
			const float p = float(a / 100.0);
			(key == "loss" ? c.loss : key == "spike" ? c.spike_chance : key == "reorder" ? c.reorder : c.duplicate) = p;
		} else if (key == "seed") {
			uint64_t seed = 0;
			const char *end = value.data() + value.size();
			const std::from_chars_result r = std::from_chars(value.data(), end, seed);
			if (value.empty() || r.ec != std::errc() || r.ptr != end) {
				return bad(p_profile, "seed wants an unsigned 64-bit integer");
			}
			c.seed = seed;
		} else {
			return bad(p_profile, "unknown key (lat, loss, spike, spike_ms, reorder, dup, seed)");
		}
	}
	return c;
}

std::string format_netsim(const NetSimConfig &p_config) {
	if (!p_config.enabled()) {
		return "off";
	}
	std::string out = "lat=";
	append_number(out, p_config.latency_min_ms);
	out += '-';
	append_number(out, p_config.latency_max_ms);
	out += ",loss=";
	append_number(out, double(p_config.loss) * 100.0);
	out += ",spike=";
	append_number(out, double(p_config.spike_chance) * 100.0);
	out += ",spike_ms=";
	append_number(out, p_config.spike_min_ms);
	out += '-';
	append_number(out, p_config.spike_max_ms);
	out += ",reorder=";
	append_number(out, double(p_config.reorder) * 100.0);
	out += ",dup=";
	append_number(out, double(p_config.duplicate) * 100.0);
	out += ",seed=" + std::to_string(p_config.seed);
	return out;
}

NetSim::NetSim(const NetSimConfig &p_config) :
		config_(p_config), rng_(p_config.seed) {}

void NetSim::submit(Datagram p_datagram, uint64_t p_now_ns) {
	stats_.submitted++;
	const auto before = [](const Pending &p_a, const Pending &p_b) {
		return p_a.release_ns != p_b.release_ns ? p_a.release_ns < p_b.release_ns
												: (p_a.order & ~kHeldBit) < (p_b.order & ~kHeldBit);
	};
	const auto insert = [&](Pending p_pending) {
		queue_.insert(std::upper_bound(queue_.begin(), queue_.end(), p_pending, before), std::move(p_pending));
	};
	if (!config_.enabled()) {
		insert(Pending{ p_now_ns, next_order_++, std::move(p_datagram) });
		return;
	}
	const bool unreliable = !p_datagram.reliable;

	// Drop spikes: a Poisson-like start chance per second of elapsed time, checked on every submit.
	if (config_.spike_chance > 0.0f) {
		if (stats_.submitted > 1 && p_now_ns > last_spike_check_ns_ && p_now_ns >= spike_until_ns_) {
			const double dt = double(p_now_ns - last_spike_check_ns_) * 1.0e-9;
			const double chance = std::clamp(double(config_.spike_chance), 0.0, 1.0);
			const double p = chance >= 1.0 ? 1.0 : 1.0 - std::pow(1.0 - chance, dt);
			if (uniform01(rng_) < p) {
				const double ms = config_.spike_min_ms + (config_.spike_max_ms - config_.spike_min_ms) * uniform01(rng_);
				spike_until_ns_ = p_now_ns + ms_to_ns(ms);
			}
		}
		last_spike_check_ns_ = std::max(last_spike_check_ns_, p_now_ns);
	}
	if (unreliable && p_now_ns < spike_until_ns_) {
		stats_.dropped++;
		stats_.spike_dropped++;
		return;
	}
	if (unreliable && config_.loss > 0.0f && uniform01(rng_) < double(config_.loss)) {
		stats_.dropped++;
		return;
	}
	const bool duplicate = unreliable && config_.duplicate > 0.0f && uniform01(rng_) < double(config_.duplicate);
	const int copies = duplicate ? 2 : 1;
	stats_.duplicated += duplicate ? 1 : 0;
	const double span_ms = std::max(0.0, double(config_.latency_max_ms) - double(config_.latency_min_ms));
	for (int copy = 0; copy < copies; ++copy) {
		const double delay_ms = double(config_.latency_min_ms) + (span_ms > 0.0 ? span_ms * uniform01(rng_) : 0.0);
		uint64_t release = p_now_ns + ms_to_ns(delay_ms);
		const uint64_t scheduled = stats_.delivered + queue_.size(); // copies scheduled before this one
		stats_.mean_delay_ms += (delay_ms - stats_.mean_delay_ms) / double(scheduled + 1);
		bool held = false;
		if (!unreliable) {
			// In order per peer and channel, as ENet delivers reliable data: never before an earlier one.
			for (const Pending &p : queue_) {
				if (p.datagram.reliable && p.datagram.peer == p_datagram.peer && p.datagram.channel == p_datagram.channel) {
					release = std::max(release, p.release_ns);
				}
			}
		} else {
			held = config_.reorder > 0.0f && uniform01(rng_) < double(config_.reorder);
			if (!held) {
				// The datagrams held back for this peer go right behind this one (in their own order).
				bool moved = false;
				for (Pending &p : queue_) {
					if ((p.order & kHeldBit) != 0 && p.datagram.peer == p_datagram.peer) {
						p.order &= ~kHeldBit;
						p.release_ns = std::min(p.release_ns, release + 1);
						moved = true;
					}
				}
				if (moved) {
					std::stable_sort(queue_.begin(), queue_.end(), before);
				}
			}
		}
		uint64_t order = next_order_++;
		if (held) {
			order |= kHeldBit;
			release += ms_to_ns(kHoldTimeoutMs + span_ms);
			stats_.reordered++;
		}
		Pending pending{ release, order, copy + 1 < copies ? p_datagram : std::move(p_datagram) };
		insert(std::move(pending));
	}
}

bool NetSim::poll(uint64_t p_now_ns, Datagram &r_datagram) {
	if (queue_.empty() || queue_.front().release_ns > p_now_ns) {
		return false;
	}
	r_datagram = std::move(queue_.front().datagram);
	queue_.pop_front();
	stats_.delivered++;
	return true;
}

uint64_t NetSim::next_release_ns() const {
	return queue_.empty() ? 0 : std::max<uint64_t>(queue_.front().release_ns, 1);
}

size_t NetSim::pending() const {
	return queue_.size();
}

} // namespace bf3::net
