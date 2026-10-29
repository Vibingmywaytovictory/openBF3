#include "client_logic_sync.h"

#include <utility>

namespace bf3::host {

void ClientLogicSync::datagram(net::MessageType p_type, std::span<const uint8_t> p_bytes) {
	std::lock_guard lock(mutex_);
	switch (p_type) {
		case net::MessageType::LogicEvents: {
			core::Result<net::LogicEvents> m = net::decode_logic_events(p_bytes);
			if (!m) {
				++bad_;
				return;
			}
			events(*m);
			return;
		}
		case net::MessageType::FullSync: {
			core::Result<net::FullSync> m = net::decode_full_sync(p_bytes);
			if (!m) {
				++bad_;
				return;
			}
			full_sync(std::move(*m));
			return;
		}
		case net::MessageType::LogicDelta: {
			core::Result<net::LogicDelta> m = net::decode_logic_delta(p_bytes);
			if (!m) {
				++bad_;
				return;
			}
			delta(*m);
			return;
		}
		default:
			++bad_;
			return;
	}
}

void ClientLogicSync::events(const net::LogicEvents &p_message) {
	for (const sim::logic::LogicNetworkedEvent &e : p_message.events) {
		++events_;
		if (have_ghost_ && e.tick <= sync_tick_) {
			++early_events_;
		}
		if (!any_event_) {
			any_event_ = true;
			first_event_tick_ = e.tick;
		}
		if (queued_.size() >= kClientLogicMaxQueuedEvents) {
			++dropped_events_;
			continue;
		}
		queued_.push_back(e);
	}
}

void ClientLogicSync::full_sync(net::FullSync p_message) {
	++full_syncs_;
	if (have_ghost_ && p_message.ghost.tick < ghost_.tick) {
		++stale_;
		return;
	}
	if (!have_ghost_ && any_event_ && first_event_tick_ <= p_message.ghost.tick) {
		++early_events_;
	}
	if (!have_ghost_) {
		sync_tick_ = p_message.ghost.tick;
	}
	ghost_ = std::move(p_message.ghost);
	have_ghost_ = true;
	for (const net::LogicDelta &d : held_) {
		if (d.tick > ghost_.tick) {
			apply(d);
		}
	}
	held_.clear();
	++revision_;
}

void ClientLogicSync::delta(const net::LogicDelta &p_message) {
	++deltas_;
	if (!have_ghost_) {
		if (held_.size() >= kClientLogicMaxHeldDeltas) {
			++bad_;
			return;
		}
		held_.push_back(p_message);
		return;
	}
	if (p_message.tick <= ghost_.tick) {
		++stale_;
		return;
	}
	apply(p_message);
	++revision_;
}

void ClientLogicSync::apply(const net::LogicDelta &p_message) {
	for (const sim::logic::LogicGhost::BoolDelta &row : p_message.rows) {
		if (row.index >= ghost_.bool_count) {
			++bad_;
			continue;
		}
		uint64_t &word = ghost_.bool_bits[row.index / 64];
		const uint64_t bit = uint64_t(1) << (row.index % 64);
		word = row.value ? (word | bit) : (word & ~bit);
	}
	ghost_.tick = p_message.tick;
}

void ClientLogicSync::take(ClientLogicFeed &r_feed) {
	std::lock_guard lock(mutex_);
	r_feed.events.insert(r_feed.events.end(), queued_.begin(), queued_.end());
	queued_.clear();
	if (have_ghost_ && revision_ != r_feed.ghost_revision) {
		r_feed.ghost = ghost_;
		r_feed.ghost_revision = revision_;
	}
}

void ClientLogicSync::stats(ClientStats &r_stats) const {
	std::lock_guard lock(mutex_);
	r_stats.logic_full_syncs = full_syncs_;
	r_stats.logic_deltas = deltas_;
	r_stats.logic_events = events_;
	r_stats.logic_stale = stale_;
	r_stats.logic_early_events = early_events_;
	r_stats.logic_dropped_events = dropped_events_;
	r_stats.bad_logic_datagrams = bad_;
}

}
