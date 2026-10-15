// MixerCore (mixer_core.h, design §6.5, §7): the fixed 512-frame render grid, the three SPSC rings, command
// application by frame, and the bus fold to stereo. The audio thread never allocates, frees, locks or waits.

#include "bf3/audio/mixer_core.h"

#include "bf3/audio/dsp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>
#define OPENBF3_HAVE_MXCSR 1
#endif

namespace bf3::audio {

namespace {

// Single-producer single-consumer ring over a preallocated power-of-two array.
template <class T>
class SpscRing {
public:
	explicit SpscRing(uint32_t p_capacity) {
		uint32_t c = 2;
		while (c < p_capacity) {
			c <<= 1;
		}
		items_.resize(c);
		mask_ = c - 1;
	}
	bool push(const T &p_item) {
		const uint32_t head = head_.load(std::memory_order_relaxed);
		const uint32_t tail = tail_.load(std::memory_order_acquire);
		if (head - tail > mask_) {
			return false;
		}
		items_[head & mask_] = p_item;
		head_.store(head + 1, std::memory_order_release);
		return true;
	}
	bool pop(T &r_item) {
		const uint32_t tail = tail_.load(std::memory_order_relaxed);
		const uint32_t head = head_.load(std::memory_order_acquire);
		if (tail == head) {
			return false;
		}
		r_item = items_[tail & mask_];
		tail_.store(tail + 1, std::memory_order_release);
		return true;
	}

private:
	std::vector<T> items_;
	uint32_t mask_ = 0;
	std::atomic<uint32_t> head_{ 0 };
	char pad_[60] = {}; // head and tail on separate cache lines
	std::atomic<uint32_t> tail_{ 0 };
};

struct InstanceSlot {
	InstanceHandle handle = kNoInstance;
	PatchInstance *instance = nullptr;
	bool stop_requested = false;
};

void atomic_max(std::atomic<uint64_t> &r_value, uint64_t p_candidate) {
	uint64_t cur = r_value.load(std::memory_order_relaxed);
	while (p_candidate > cur && !r_value.compare_exchange_weak(cur, p_candidate, std::memory_order_relaxed)) {
	}
}

} // namespace

struct MixerCore::Impl {
	MixerConfig config;
	std::thread::id owner;
	std::atomic<InstanceHandle> next_handle{ 1 };
	SpscRing<MixerCommand> commands;
	SpscRing<PatchInstance *> returns;
	SpscRing<MixerFeedback> feedback;
	std::vector<MixerCommand> pending; // drained, not yet due (audio thread)
	size_t pending_count = 0;
	std::vector<PatchInstance *> return_overflow; // audio thread: instances the return ring refused (capacity reserved)
	size_t return_overflow_count = 0;
	std::vector<InstanceSlot> instances;
	std::vector<float> bus_storage; // [bus][channel][kBlockFrames]
	BusBuffers buses;
	std::vector<float> block; // interleaved stereo, kBlockFrames
	uint32_t block_pos = kBlockFrames; // next unread frame of `block`
	ListenerState listener;
	std::vector<std::pair<uint32_t, float>> mixer_values; // name hash -> value, applied to every new instance
	size_t mixer_value_count = 0;
	MixTap *tap = nullptr;
	float output_gain = 1.0f;
	std::atomic<uint64_t> rendered{ 0 }; // MixerClock: frames rendered into the grid
	std::chrono::steady_clock::time_point last_call{};
	bool have_last_call = false;

	explicit Impl(const MixerConfig &p_config) :
			config(p_config), commands(p_config.command_capacity), returns(std::max<uint32_t>(p_config.max_instances, 1) * 2),
			feedback(std::max<uint32_t>(p_config.max_instances, 1) * 8) {
		owner = std::this_thread::get_id();
		pending.resize(std::max<uint32_t>(p_config.command_capacity, 2));
		instances.resize(std::max<uint32_t>(p_config.max_instances, 1));
		return_overflow.resize(instances.size() + pending.size());
		bus_storage.assign(size_t(core::audio::MasterBus::Unknown) * kMaxBusChannels * kBlockFrames, 0.0f);
		for (size_t b = 0; b < size_t(core::audio::MasterBus::Unknown); ++b) {
			for (size_t c = 0; c < kMaxBusChannels; ++c) {
				buses.channels[b][c] = bus_storage.data() + (b * kMaxBusChannels + c) * kBlockFrames;
			}
		}
		block.assign(size_t(kBlockFrames) * 2, 0.0f);
		mixer_values.resize(64);
		output_gain = db_to_gain(p_config.output_gain_db);
	}

	void render_block(MixerStats &stats);

	void give_back(PatchInstance *p_instance) {
		if (p_instance == nullptr) {
			return;
		}
		if (!returns.push(p_instance)) {
			if (return_overflow_count < return_overflow.size()) {
				return_overflow[return_overflow_count++] = p_instance; // retried every block
			}
		}
	}
};

MixerCore::MixerCore(const MixerConfig &p_config) :
		impl_(std::make_unique<Impl>(p_config)) {}

MixerCore::~MixerCore() {
	Impl &m = *impl_;
	for (InstanceSlot &s : m.instances) {
		delete s.instance;
		s.instance = nullptr;
	}
	MixerCommand c;
	while (m.commands.pop(c)) {
		if (c.type == MixerCommandType::Start) {
			delete c.instance;
		}
	}
	for (size_t i = 0; i < m.pending_count; ++i) {
		if (m.pending[i].type == MixerCommandType::Start) {
			delete m.pending[i].instance;
		}
	}
	PatchInstance *p = nullptr;
	while (m.returns.pop(p)) {
		delete p;
	}
	for (size_t i = 0; i < m.return_overflow_count; ++i) {
		delete m.return_overflow[i];
	}
}

InstanceHandle MixerCore::next_handle() {
	return impl_->next_handle.fetch_add(1, std::memory_order_relaxed);
}

bool MixerCore::push(const MixerCommand &p_command) {
	if (!impl_->commands.push(p_command)) {
		stats_.ring_overflows.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	return true;
}

uint64_t MixerCore::clock() const {
	return impl_->rendered.load(std::memory_order_acquire);
}

uint64_t MixerCore::earliest_frame() const {
	return clock() + uint64_t(impl_->config.schedule_lead_blocks) * kBlockFrames;
}

void MixerCore::reclaim() {
	PatchInstance *p = nullptr;
	while (impl_->returns.pop(p)) {
		delete p;
	}
}

size_t MixerCore::poll_feedback(std::span<MixerFeedback> r_out) {
	size_t n = 0;
	while (n < r_out.size() && impl_->feedback.pop(r_out[n])) {
		++n;
	}
	return n;
}

void MixerCore::set_tap(MixTap *p_tap) {
	impl_->tap = p_tap;
}

namespace {

InstanceSlot *find_slot(std::vector<InstanceSlot> &r_slots, InstanceHandle p_handle) {
	if (p_handle == kNoInstance) {
		return nullptr;
	}
	for (InstanceSlot &s : r_slots) {
		if (s.handle == p_handle && s.instance != nullptr) {
			return &s;
		}
	}
	return nullptr;
}

} // namespace

// Renders one grid block into `block`.
void MixerCore::Impl::render_block(MixerStats &stats) {
	Impl &m = *this;
	const auto t0 = std::chrono::steady_clock::now();
	const uint64_t first = m.rendered.load(std::memory_order_relaxed);
	const uint64_t end = first + kBlockFrames;
	// Retry instances the return ring refused earlier.
	size_t kept = 0;
	for (size_t i = 0; i < m.return_overflow_count; ++i) {
		if (!m.returns.push(m.return_overflow[i])) {
			m.return_overflow[kept++] = m.return_overflow[i];
		}
	}
	m.return_overflow_count = kept;
	// Drain the command ring into the pending list (preallocated; a full list leaves commands in the ring).
	MixerCommand c;
	while (m.pending_count < m.pending.size() && m.commands.pop(c)) {
		m.pending[m.pending_count++] = c;
	}
	// Apply every command due in this block, in push order.
	size_t write = 0;
	for (size_t i = 0; i < m.pending_count; ++i) {
		MixerCommand &cmd = m.pending[i];
		if (cmd.at_frame >= end) {
			m.pending[write++] = cmd;
			continue;
		}
		uint32_t offset = 0;
		if (cmd.at_frame != 0) {
			if (cmd.at_frame < first) {
				stats.late_commands.fetch_add(1, std::memory_order_relaxed);
			} else {
				offset = uint32_t(cmd.at_frame - first);
			}
		}
		switch (cmd.type) {
			case MixerCommandType::Start: {
				InstanceSlot *free = nullptr;
				for (InstanceSlot &s : m.instances) {
					if (s.instance == nullptr) {
						free = &s;
						break;
					}
				}
				if (free == nullptr || cmd.instance == nullptr) {
					stats.starved_voices.fetch_add(cmd.instance ? 1 : 0, std::memory_order_relaxed);
					m.give_back(cmd.instance); // no room: straight back to the game thread
					break;
				}
				free->handle = cmd.handle;
				free->instance = cmd.instance;
				free->stop_requested = false;
				for (size_t k = 0; k < m.mixer_value_count; ++k) {
					cmd.instance->set_mixer_value(m.mixer_values[k].first, m.mixer_values[k].second);
				}
				break;
			}
			case MixerCommandType::Stop:
				if (InstanceSlot *s = find_slot(m.instances, cmd.handle)) {
					if (!s->stop_requested) {
						const uint32_t stop = s->instance->program().stop_event;
						if (stop != 0 && !s->instance->stop_seen()) {
							s->instance->post_event(stop, offset);
						}
						s->stop_requested = true;
					}
				}
				break;
			case MixerCommandType::Kill:
				if (InstanceSlot *s = find_slot(m.instances, cmd.handle)) {
					m.give_back(s->instance);
					s->instance = nullptr;
					s->handle = kNoInstance;
				}
				break;
			case MixerCommandType::SetParameter:
				if (InstanceSlot *s = find_slot(m.instances, cmd.handle)) {
					s->instance->set_parameter(cmd.name_hash, cmd.value);
				}
				break;
			case MixerCommandType::PostEvent:
				if (InstanceSlot *s = find_slot(m.instances, cmd.handle)) {
					s->instance->post_event(cmd.name_hash, offset);
				}
				break;
			case MixerCommandType::SetMixerValue: {
				bool found = false;
				for (size_t k = 0; k < m.mixer_value_count; ++k) {
					if (m.mixer_values[k].first == cmd.name_hash) {
						m.mixer_values[k].second = cmd.value;
						found = true;
					}
				}
				if (!found && m.mixer_value_count < m.mixer_values.size()) {
					m.mixer_values[m.mixer_value_count++] = { cmd.name_hash, cmd.value };
				}
				for (InstanceSlot &s : m.instances) {
					if (s.instance != nullptr) {
						s.instance->set_mixer_value(cmd.name_hash, cmd.value);
					}
				}
				break;
			}
			case MixerCommandType::SetEmitter:
				if (InstanceSlot *s = find_slot(m.instances, cmd.handle)) {
					s->instance->set_geometry(cmd.emitter, m.listener);
				}
				break;
			case MixerCommandType::SetListener:
				m.listener = cmd.listener;
				break;
			case MixerCommandType::SetBank:
				stats.acked_epoch.store(cmd.epoch, std::memory_order_release);
				break;
		}
	}
	m.pending_count = write;

	// Render every instance into the buses.
	std::fill(m.bus_storage.begin(), m.bus_storage.end(), 0.0f);
	m.buses.frames = kBlockFrames;
	m.buses.first_frame = first;
	uint32_t active = 0;
	for (InstanceSlot &s : m.instances) {
		if (s.instance == nullptr) {
			continue;
		}
		const PatchInstanceStats before = s.instance->stats();
		s.instance->process_block(m.buses);
		const PatchInstanceStats &after = s.instance->stats();
		stats.starved_voices.fetch_add(after.starved_slots - before.starved_slots, std::memory_order_relaxed);
		stats.decode_errors.fetch_add(after.decode_errors - before.decode_errors, std::memory_order_relaxed);
		stats.deferred_events.fetch_add(after.deferred_events - before.deferred_events, std::memory_order_relaxed);
		const bool done = s.instance->finished() && (s.stop_requested || s.instance->stats().sampler_triggers > 0);
		const PatchHaptics h = s.instance->haptics();
		const bool haptic = h.shake_pitch != 0.0f || h.shake_roll != 0.0f || h.shake_yaw != 0.0f || h.rumble_low != 0.0f || h.rumble_high != 0.0f;
		if (done || haptic) {
			MixerFeedback fb;
			fb.handle = s.handle;
			fb.frame = first;
			fb.haptics = h;
			fb.finished = done;
			if (!m.feedback.push(fb)) {
				stats.feedback_drops.fetch_add(1, std::memory_order_relaxed);
			}
		}
		if (done) {
			m.give_back(s.instance);
			s.instance = nullptr;
			s.handle = kNoInstance;
		} else {
			++active;
		}
	}
	stats.active_instances.store(active, std::memory_order_relaxed);
	if (active > stats.peak_instances.load(std::memory_order_relaxed)) {
		stats.peak_instances.store(active, std::memory_order_relaxed);
	}
	// Fold the buses to stereo: every bus at 0 dB (the instance applied the LFE fold and dropped the Reverb sends).
	for (uint32_t i = 0; i < kBlockFrames; ++i) {
		float l = 0.0f, r = 0.0f;
		for (size_t b = 0; b < size_t(core::audio::MasterBus::Unknown); ++b) {
			l += m.buses.channels[b][0][i];
			r += m.buses.channels[b][1][i];
		}
		m.block[size_t(i) * 2] = l * m.output_gain;
		m.block[size_t(i) * 2 + 1] = r * m.output_gain;
	}
	if (m.tap != nullptr) {
		m.tap->on_block(m.block.data(), kBlockFrames, first);
	}
	m.block_pos = 0;
	m.rendered.store(end, std::memory_order_release);
	stats.blocks.fetch_add(1, std::memory_order_relaxed);
	const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
	atomic_max(stats.max_block_ns, ns);
	const uint64_t budget_ns = uint64_t(kBlockFrames) * 1000000000ull / std::max<uint32_t>(m.config.mix_rate, 1);
	if (ns > budget_ns) {
		stats.late_blocks.fetch_add(1, std::memory_order_relaxed);
	}
}

uint32_t MixerCore::mix(float *r_interleaved_stereo, uint32_t p_frames) {
	Impl &m = *impl_;
#if OPENBF3_HAVE_MXCSR
	const unsigned int saved_csr = _mm_getcsr();
	_mm_setcsr(saved_csr | 0x8040); // FTZ (bit 15) | DAZ (bit 6)
#endif
	const auto now = std::chrono::steady_clock::now();
	if (m.have_last_call) {
		atomic_max(stats_.max_call_gap_ns, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - m.last_call).count()));
	}
	m.last_call = now;
	m.have_last_call = true;
	uint32_t done = 0;
	while (done < p_frames) {
		if (m.block_pos >= kBlockFrames) {
			m.render_block(stats_);
		}
		const uint32_t take = std::min(p_frames - done, kBlockFrames - m.block_pos);
		std::memcpy(r_interleaved_stereo + size_t(done) * 2, m.block.data() + size_t(m.block_pos) * 2, size_t(take) * 2 * sizeof(float));
		m.block_pos += take;
		done += take;
	}
	stats_.frames.fetch_add(p_frames, std::memory_order_relaxed);
#if OPENBF3_HAVE_MXCSR
	_mm_setcsr(saved_csr);
#endif
	return p_frames;
}

} // namespace bf3::audio
