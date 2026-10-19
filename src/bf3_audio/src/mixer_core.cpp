// MixerCore (mixer_core.h, design §6.5, §7): the fixed 512-frame render grid, the three SPSC rings, command
// application by frame, and the bus fold to stereo. The audio thread never allocates, frees, locks or waits.

#include "bf3/audio/mixer_core.h"

#include "bf3/audio/dsp.h"
#include "bf3/audio/hdr_window.h" // M2b au [a3]
#include "bf3/audio/voice_cull.h" // M2b au [a3]

#include <algorithm>
#include <array> // PT1 snd [limit]
#include <chrono>
#include <cmath>
#include <cstdio> // M2b au
#include <cstring>
#include <limits> // M2b au
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
	bool starting = false; // M2b au [a3]: its Start landed this block (the cull's pre-cull and DuplicateSpawn)
	bool pending = false; // M2b au [a3]: a loop over a limit: kept, not rendered, until the cull keeps it
	uint64_t start_frame = 0; // M2b au [a3]
	uint16_t scope = core::audio::kNoScope; // M2b au [a3]
	bool pt1_rendered = false; // PT1 snd [cuts]: rendered in the last block (a stop now is a cut)
	bool pt1_suspended = false; // PT1 snd [cuts]
	uint64_t pt1_abrupt_seen = 0; // PT1 snd [cuts]
	uint8_t pt1_fade_mode = 0; // PT1 snd [fade]: 0 none, kPt1Release (give back at the fade's end), kPt1Park (pending then)
	MixerCutKind pt1_fade_kind = MixerCutKind::Drop; // PT1 snd [fade]: the cut the fade replaces
	uint32_t pt1_voices_held = 0; // PT1 snd [hold]: the most busy sampler voices over the last pt1_voice_hold_blocks
	uint32_t pt1_hold_left = 0; // PT1 snd [hold]
};
constexpr uint8_t kPt1Release = 1; // PT1 snd [fade]
constexpr uint8_t kPt1Park = 2; // PT1 snd [fade]

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
	// Per block (audio thread): the Starts kept in `pending` because they are not due yet, as (handle, at_frame). A
	// later command for the same handle due before that frame waits for its Start (design §16 item 5). Bounded by
	// max_instances, the same order as find_slot's scan.
	std::vector<std::pair<InstanceHandle, uint64_t>> held_starts;
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
	// M2b au [a3] (docs/design/m2b.md §6, docs/design/m2b-au.md): the HDR window, the cull's buffers (sized here to the
	// slot count, never grown) and whether the A3 step runs at all (an HdrSetting, a ScopeTable, or more slots than
	// max_voices; otherwise the block is M2a's exactly).
	HdrWindow window; // M2b au [a3]
	bool a3_on = false; // M2b au [a3]
	std::vector<CullVoice> cull_voices; // M2b au [a3]
	std::vector<CullVerdict> cull_verdicts; // M2b au [a3]
	std::vector<uint32_t> cull_slots; // M2b au [a3]
	void a3_init(); // M2b au [a3]
	void a3_block(MixerStats &stats, uint64_t p_first); // M2b au [a3]
	MixerCutStats cut_stats; // PT1 snd [cuts]
	SpscRing<MixerCutEvent> cut_ring{ 4096 }; // PT1 snd [cuts]
	std::atomic<bool> cut_record{ false }; // PT1 snd [cuts]
	uint64_t block_first = 0; // PT1 snd [cuts]: the frame of the block being rendered
	void pt1_cut(MixerCutKind p_kind, const InstanceSlot &p_slot, float p_step); // PT1 snd [cuts]
	void pt1_event(MixerCutKind p_kind, InstanceHandle p_handle, float p_step, const PatchInstance *p_instance); // PT1 snd [cuts]
	// PT1 snd [fade]: true when the slot now fades out instead of being cut (it sounded in the last block and fades are on).
	bool pt1_begin_fade(InstanceSlot &r_slot, uint8_t p_mode, MixerCutKind p_kind); // PT1 snd [fade]
	float pt1_limit_gain = 1.0f; // PT1 snd [limit]: the gain at the last frame of the last block
	std::array<float, kBlockFrames> pt1_limit_g{}; // PT1 snd [limit]
	void pt1_limit(); // PT1 snd [limit]: on `block`, before the tap

	explicit Impl(const MixerConfig &p_config) :
			config(p_config), commands(p_config.command_capacity), returns(std::max<uint32_t>(p_config.max_instances, 1) * 2),
			feedback(std::max<uint32_t>(p_config.max_instances, 1) * 8) {
		owner = std::this_thread::get_id();
		pending.resize(std::max<uint32_t>(p_config.command_capacity, 2));
		instances.resize(std::max<uint32_t>(p_config.max_instances, 1));
		held_starts.resize(instances.size());
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
		a3_init(); // M2b au [a3]
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
	m.block_first = first; // PT1 snd [cuts]
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
	// Apply every command due in this block, in push order. A command for a handle whose Start is still held (stamped
	// later) is moved to that Start's frame, so it is applied right after the Start instead of finding no instance.
	size_t write = 0, held = 0;
	for (size_t i = 0; i < m.pending_count; ++i) {
		MixerCommand &cmd = m.pending[i];
		if (cmd.type != MixerCommandType::Start && cmd.handle != kNoInstance) {
			for (size_t k = 0; k < held; ++k) {
				if (m.held_starts[k].first == cmd.handle && cmd.at_frame < m.held_starts[k].second) {
					if (cmd.at_frame != 0 && cmd.at_frame < first) {
						stats.late_commands.fetch_add(1, std::memory_order_relaxed); // its own frame was already mixed
					}
					cmd.at_frame = m.held_starts[k].second;
				}
			}
		}
		if (cmd.at_frame >= end) {
			if (cmd.type == MixerCommandType::Start && held < m.held_starts.size()) {
				m.held_starts[held++] = { cmd.handle, cmd.at_frame };
			}
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
				free->starting = true; // M2b au [a3]
				free->pending = false; // M2b au [a3]
				free->start_frame = first + offset; // M2b au [a3]
				free->scope = m.config.scopes ? m.config.scopes->scope_of(cmd.instance->program().partition) : core::audio::kNoScope; // M2b au [a3]
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
					if (s->pt1_fade_mode != 0 || m.pt1_begin_fade(*s, kPt1Release, MixerCutKind::Kill)) { // PT1 snd [fade]
						s->pt1_fade_mode = kPt1Release; // PT1 snd [fade]: a parking loop is released at its fade's end
						s->pt1_fade_kind = MixerCutKind::Kill; // PT1 snd [fade]
						break; // PT1 snd [fade]
					} // PT1 snd [fade]
					if (s->pt1_rendered) { // PT1 snd [cuts]
						m.pt1_cut(MixerCutKind::Kill, *s, s->instance->pt1_edge().edge); // PT1 snd [cuts]
					} // PT1 snd [cuts]
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
	m.a3_block(stats, first); // M2b au [a3]: cull, window, gains (docs/design/m2b-au.md)

	// Render every instance into the buses.
	std::fill(m.bus_storage.begin(), m.bus_storage.end(), 0.0f);
	m.buses.frames = kBlockFrames;
	m.buses.first_frame = first;
	uint32_t active = 0;
	for (InstanceSlot &s : m.instances) {
		if (s.instance == nullptr) {
			continue;
		}
		if (s.pending) { // M2b au [a3]: a pending loop is not rendered
			s.pt1_rendered = false; // PT1 snd [cuts]
			continue; // M2b au [a3]
		} // M2b au [a3]
		const PatchInstanceStats before = s.instance->stats();
		s.instance->process_block(m.buses);
		const PatchInstanceStats &after = s.instance->stats();
		{ // PT1 snd [cuts]: a start (or a resumed loop) and the abrupt gain ramps of this block
			const InstanceEdge e = s.instance->pt1_edge(); // PT1 snd [cuts]
			if (!s.pt1_rendered) { // PT1 snd [cuts]
				m.pt1_cut(s.start_frame + kBlockFrames > first ? MixerCutKind::Start : MixerCutKind::Resume, s, e.head); // PT1 snd [cuts]
				s.pt1_rendered = true; // PT1 snd [cuts]
			} // PT1 snd [cuts]
			if (e.abrupt_gain != s.pt1_abrupt_seen) { // PT1 snd [cuts]
				m.cut_stats.abrupt_gain.fetch_add(e.abrupt_gain - s.pt1_abrupt_seen, std::memory_order_relaxed); // PT1 snd [cuts]
				s.pt1_abrupt_seen = e.abrupt_gain; // PT1 snd [cuts]
				if (e.abrupt_gain_max > m.cut_stats.abrupt_gain_max.load(std::memory_order_relaxed)) { // PT1 snd [cuts]
					m.cut_stats.abrupt_gain_max.store(e.abrupt_gain_max, std::memory_order_relaxed); // PT1 snd [cuts]
				} // PT1 snd [cuts]
			} // PT1 snd [cuts]
		} // PT1 snd [cuts]
		stats.starved_voices.fetch_add(after.starved_slots - before.starved_slots, std::memory_order_relaxed);
		stats.decode_errors.fetch_add(after.decode_errors - before.decode_errors, std::memory_order_relaxed);
		stats.deferred_events.fetch_add(after.deferred_events - before.deferred_events, std::memory_order_relaxed);
		if (s.pt1_fade_mode != 0 && s.instance->pt1_fade_done()) { // PT1 snd [fade]: the fade replaced a cut; it ends now
			m.pt1_cut(s.pt1_fade_kind, s, s.instance->pt1_edge().edge); // PT1 snd [fade]: the cut itself, at the faded level
			if (s.pt1_fade_mode == kPt1Park) { // PT1 snd [fade]
				s.pending = true; // PT1 snd [fade]
				s.pt1_rendered = false; // PT1 snd [fade]
				s.pt1_fade_mode = 0; // PT1 snd [fade]
				++active; // PT1 snd [fade]
				continue; // PT1 snd [fade]
			} // PT1 snd [fade]
			m.give_back(s.instance); // PT1 snd [fade]: a drop's finished feedback went at the drop, a Kill needs none
			s = InstanceSlot{}; // PT1 snd [fade]
			continue; // PT1 snd [fade]
		} // PT1 snd [fade]
		const bool done = s.instance->finished() && (s.stop_requested || s.instance->stats().sampler_triggers > 0);
		const PatchHaptics h = s.instance->haptics();
		const bool haptic = h.shake_pitch != 0.0f || h.shake_roll != 0.0f || h.shake_yaw != 0.0f || h.rumble_low != 0.0f || h.rumble_high != 0.0f;
		if ((done || haptic) && s.pt1_fade_mode != kPt1Release) { // PT1 snd [fade]: `&& s.pt1_fade_mode != kPt1Release` added (no news from a released voice)
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
			m.pt1_cut(MixerCutKind::Finish, s, s.instance->pt1_edge().edge); // PT1 snd [cuts]
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
	m.pt1_limit(); // PT1 snd [limit]
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
		m.pt1_event(MixerCutKind::LateBlock, kNoInstance, float(double(ns) / 1e6), nullptr); // PT1 snd [cuts]
	}
	record_mix_ns(stats, ns, budget_ns); // M2b au [profile] (mixer_core.h)
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
		const double gap_ms = std::chrono::duration<double, std::milli>(now - m.last_call).count(); // PT1 snd [cuts]
		size_t gb = kPt1GapEdgesMs.size(); // PT1 snd [cuts]
		for (size_t i = 0; i < kPt1GapEdgesMs.size(); ++i) { // PT1 snd [cuts]
			if (gap_ms < kPt1GapEdgesMs[i]) { // PT1 snd [cuts]
				gb = i; // PT1 snd [cuts]
				break; // PT1 snd [cuts]
			} // PT1 snd [cuts]
		} // PT1 snd [cuts]
		m.cut_stats.gap_hist[gb].fetch_add(1, std::memory_order_relaxed); // PT1 snd [cuts]
		if (gap_ms > kPt1LateCallMs) { // PT1 snd [cuts]
			m.block_first = m.rendered.load(std::memory_order_relaxed); // PT1 snd [cuts]
			m.pt1_event(MixerCutKind::CallGap, kNoInstance, float(gap_ms), nullptr); // PT1 snd [cuts]
		} // PT1 snd [cuts]
	}
	m.cut_stats.calls.fetch_add(1, std::memory_order_relaxed); // PT1 snd [cuts]
	atomic_max(m.cut_stats.call_frames_max, p_frames); // PT1 snd [cuts]
	if (p_frames < m.cut_stats.call_frames_min.load(std::memory_order_relaxed)) { // PT1 snd [cuts]
		m.cut_stats.call_frames_min.store(p_frames, std::memory_order_relaxed); // PT1 snd [cuts]
	} // PT1 snd [cuts]
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

// M2b au (docs/design/m2b.md §6, docs/design/m2b-au.md): the A3 step of render_block and the profiler.
//   cull (voice_cull.h) over every occupied slot -> drop the culled one-shots (back to the game thread with a
//   finished feedback, so the router and the one-shot pool free them), park the culled loops (pending: not rendered)
//   -> the window steps on the loudest kept HDR voice -> each kept HDR voice gets the window's gain, or is suspended
//   below the bottom. Without an HdrSetting the gains stay M2a's fixed window and pre-cull is off; without a
//   ScopeTable the scope step is skipped; with neither and no more slots than max_voices the step does not run.

void MixerCore::Impl::a3_init() {
	a3_on = config.hdr.has_value() || config.scopes != nullptr || instances.size() > config.max_voices;
	if (config.hdr) {
		window.configure(*config.hdr, config.mix_rate);
	}
	if (a3_on) {
		cull_voices.resize(instances.size());
		cull_verdicts.resize(instances.size());
		cull_slots.resize(instances.size());
	}
}

void MixerCore::Impl::a3_block(MixerStats &stats, uint64_t p_first) {
	if (!a3_on) {
		return;
	}
	const bool hdr_on = config.hdr.has_value();
	const auto drop = [&](InstanceSlot &r_slot) {
		const bool pt1_fades = pt1_begin_fade(r_slot, kPt1Release, MixerCutKind::Drop); // PT1 snd [fade]
		if (r_slot.pt1_rendered && !pt1_fades) { // PT1 snd [cuts]; [fade] `&& !pt1_fades` added
			pt1_cut(MixerCutKind::Drop, r_slot, r_slot.instance->pt1_edge().edge); // PT1 snd [cuts]
		} // PT1 snd [cuts]
		MixerFeedback fb;
		fb.handle = r_slot.handle;
		fb.frame = p_first;
		fb.finished = true;
		if (!feedback.push(fb)) {
			stats.feedback_drops.fetch_add(1, std::memory_order_relaxed);
		}
		if (pt1_fades) { // PT1 snd [fade]: the game frees it now; the slot fades out and is given back at the fade's end
			return; // PT1 snd [fade]
		} // PT1 snd [fade]
		give_back(r_slot.instance);
		r_slot = InstanceSlot{};
	};
	size_t n = 0;
	uint32_t pt1_reserved = 0; // PT1 snd [fade]: the voices of the fading slots, kept out of the cull and inside the cap
	for (size_t i = 0; i < instances.size(); ++i) {
		InstanceSlot &s = instances[i];
		if (s.instance == nullptr) {
			continue;
		}
		if (s.pt1_fade_mode != 0) { // PT1 snd [fade]
			pt1_reserved += std::max<uint32_t>(s.instance->cull_info().voices, 1); // PT1 snd [fade]
			continue; // PT1 snd [fade]
		} // PT1 snd [fade]
		if (s.pending && s.stop_requested) {
			drop(s); // a stopped pending loop will never play
			continue;
		}
		const VoiceCullInfo info = s.instance->cull_info();
		CullVoice &v = cull_voices[n];
		v = CullVoice{};
		v.handle = s.handle;
		v.loudness_db = info.loudness_db;
		v.priority = 1.0f; // the data has no per-patch priority (m2b-au.md)
		v.scope = s.scope;
		v.position[0] = info.position.x;
		v.position[1] = info.position.y;
		v.position[2] = info.position.z;
		v.distance_m = info.distance_m;
		v.start_frame = s.start_frame;
		v.looping = s.instance->program().is_looping;
		v.starting = s.starting || s.pending;
		v.hdr = hdr_on && info.hdr;
		v.voices = std::max<uint32_t>(info.voices, 1); // the cap counts sampler voices (voice_cull.h)
		if (!s.pending) { // PT1 snd [hold]: a parked instance's slots are frozen; it keeps its held count
			if (v.voices >= s.pt1_voices_held || s.pt1_hold_left == 0) { // PT1 snd [hold]
				s.pt1_hold_left = v.voices >= s.pt1_voices_held ? config.pt1_voice_hold_blocks : 0u; // PT1 snd [hold]
				s.pt1_voices_held = v.voices; // PT1 snd [hold]
			} else { // PT1 snd [hold]
				--s.pt1_hold_left; // PT1 snd [hold]
			} // PT1 snd [hold]
		} // PT1 snd [hold]
		v.voices = std::max(v.voices, s.pt1_voices_held); // PT1 snd [hold]
		v.hold_db = !s.starting && !s.pending && s.pt1_rendered ? config.pt1_cap_hold_db : 0.0f; // PT1 snd [hold]
		v.resume_margin = s.pending ? std::min(config.pt1_resume_margin, config.max_voices / 16) : 0u; // PT1 snd [hold]: at most 1/16 of the cap
		cull_slots[n] = uint32_t(i);
		++n;
	}
	CullCounts counts;
	const float precull = hdr_on ? window.precull_db() : -std::numeric_limits<float>::infinity();
	const uint32_t pt1_cap = config.max_voices > pt1_reserved ? config.max_voices - pt1_reserved : 0u; // PT1 snd [fade]
	audio::cull_voices(std::span<const CullVoice>(cull_voices.data(), n), config.scopes.get(), precull, pt1_cap, p_first, config.mix_rate, // PT1 snd [fade]: pt1_cap for config.max_voices
			std::span<CullVerdict>(cull_verdicts.data(), n), counts);
	float loudest = -std::numeric_limits<float>::infinity();
	uint32_t live = pt1_reserved; // PT1 snd [fade]: was 0
	for (size_t k = 0; k < n; ++k) {
		InstanceSlot &s = instances[cull_slots[k]];
		switch (cull_verdicts[k]) {
			case CullVerdict::Keep:
				if (s.pending && config.pt1_cut_fade_frames > 0) { // PT1 snd [fade]: a parked loop resumes with a fade in
					s.instance->pt1_set_fade(0.0f); // PT1 snd [fade]
					s.instance->pt1_fade_to(1.0f, config.pt1_cut_fade_frames); // PT1 snd [fade]
				} // PT1 snd [fade]
				s.pending = false;
				s.starting = false;
				live += cull_voices[k].voices;
				if (cull_voices[k].hdr) {
					loudest = std::max(loudest, cull_voices[k].loudness_db);
				}
				break;
			case CullVerdict::Pending:
				if (!s.pending && pt1_begin_fade(s, kPt1Park, MixerCutKind::Pend)) { // PT1 snd [fade]: parked at the fade's end
					break; // PT1 snd [fade]
				} // PT1 snd [fade]
				if (!s.pending && s.pt1_rendered) { // PT1 snd [cuts]
					pt1_cut(MixerCutKind::Pend, s, s.instance->pt1_edge().edge); // PT1 snd [cuts]
				} // PT1 snd [cuts]
				s.pending = true;
				break;
			default:
				drop(s);
				break;
		}
	}
	uint32_t suspended = 0;
	if (hdr_on) {
		window.step(loudest, kBlockFrames);
		const core::audio::HdrSetting &h = window.setting();
		for (size_t k = 0; k < n; ++k) {
			if (cull_verdicts[k] != CullVerdict::Keep) {
				continue;
			}
			InstanceSlot &s = instances[cull_slots[k]];
			HdrVoiceWindow w;
			w.active = cull_voices[k].hdr;
			w.top_db = window.top_db();
			w.headroom_db = h.headroom_db;
			w.overshoot_db = h.allowed_overshoot_db;
			w.suspended = w.active && window.suspended(cull_voices[k].loudness_db);
			suspended += w.suspended ? 1 : 0;
			if (w.suspended != s.pt1_suspended) { // PT1 snd [cuts]
				if (s.pt1_rendered) { // PT1 snd [cuts]
					pt1_cut(w.suspended ? MixerCutKind::Suspend : MixerCutKind::Unsuspend, s, s.instance->pt1_edge().edge); // PT1 snd [cuts]
				} // PT1 snd [cuts]
				s.pt1_suspended = w.suspended; // PT1 snd [cuts]
			} // PT1 snd [cuts]
			s.instance->set_hdr_window(w);
		}
		stats.window_top_db.store(window.top_db(), std::memory_order_relaxed);
		stats.window_bottom_db.store(window.bottom_db(), std::memory_order_relaxed);
	}
	stats.suspended.store(suspended, std::memory_order_relaxed);
	if (live > stats.live_voices_max.load(std::memory_order_relaxed)) {
		stats.live_voices_max.store(live, std::memory_order_relaxed);
	}
	stats.culled_precull.fetch_add(counts.precull, std::memory_order_relaxed);
	stats.culled_scope.fetch_add(counts.scope, std::memory_order_relaxed);
	stats.culled_cap.fetch_add(counts.cap, std::memory_order_relaxed);
}

void record_mix_ns(MixerStats &r_stats, uint64_t p_ns, uint64_t p_block_ns) {
	r_stats.mix_ns_last.store(p_ns, std::memory_order_relaxed);
	atomic_max(r_stats.mix_ns_max, p_ns);
	const double share = p_block_ns > 0 ? double(p_ns) / double(p_block_ns) : 1.0e9;
	size_t bucket = kMixHistEdges.size(); // the last bucket: at or above the last edge
	for (size_t i = 0; i < kMixHistEdges.size(); ++i) {
		if (share < double(kMixHistEdges[i])) {
			bucket = i;
			break;
		}
	}
	r_stats.mix_ns_hist[bucket].fetch_add(1, std::memory_order_relaxed);
}

double mix_share_quantile(const MixerStats &p_stats, double p_q) {
	uint64_t counts[kMixHistBuckets] = {};
	uint64_t total = 0;
	for (size_t i = 0; i < kMixHistBuckets; ++i) {
		counts[i] = p_stats.mix_ns_hist[i].load(std::memory_order_relaxed);
		total += counts[i];
	}
	if (total == 0) {
		return 0.0;
	}
	const double q = std::clamp(std::isfinite(p_q) ? p_q : 1.0, 0.0, 1.0);
	const uint64_t need = std::max<uint64_t>(1, uint64_t(std::ceil(q * double(total))));
	uint64_t seen = 0;
	for (size_t i = 0; i < kMixHistBuckets; ++i) {
		seen += counts[i];
		if (seen >= need) {
			return i < kMixHistEdges.size() ? double(kMixHistEdges[i]) : std::numeric_limits<double>::infinity();
		}
	}
	return std::numeric_limits<double>::infinity();
}

MixerProfile read_mixer_profile(const MixerStats &p_stats, uint32_t p_mix_rate) {
	MixerProfile p;
	p.blocks = p_stats.blocks.load(std::memory_order_relaxed);
	p.block_ns = uint64_t(kBlockFrames) * 1000000000ull / std::max<uint32_t>(p_mix_rate, 1);
	p.mix_ns_last = p_stats.mix_ns_last.load(std::memory_order_relaxed);
	p.mix_ns_max = p_stats.mix_ns_max.load(std::memory_order_relaxed);
	p.share_p50 = mix_share_quantile(p_stats, 0.50);
	p.share_p99 = mix_share_quantile(p_stats, 0.99);
	p.live_instances = p_stats.active_instances.load(std::memory_order_relaxed);
	p.live_voices_max = p_stats.live_voices_max.load(std::memory_order_relaxed);
	p.culled_precull = p_stats.culled_precull.load(std::memory_order_relaxed);
	p.culled_scope = p_stats.culled_scope.load(std::memory_order_relaxed);
	p.culled_cap = p_stats.culled_cap.load(std::memory_order_relaxed);
	p.suspended = p_stats.suspended.load(std::memory_order_relaxed);
	p.window_top_db = p_stats.window_top_db.load(std::memory_order_relaxed);
	p.window_bottom_db = p_stats.window_bottom_db.load(std::memory_order_relaxed);
	return p;
}

std::string format_mixer_profile(const MixerProfile &p_profile) {
	char line[400];
	const auto pct = [](double p_v) { return std::isfinite(p_v) ? p_v * 100.0 : 999.0; };
	std::snprintf(line, sizeof(line),
			"audio A3: mix %.3f ms last, %.3f ms max of %.3f ms (p50 < %.0f %%, p99 < %.0f %% of a core); instances %u live, voices %u max; "
			"culled %llu pre / %llu scope / %llu cap; %u suspended; window %.1f .. %.1f dB",
			double(p_profile.mix_ns_last) / 1e6, double(p_profile.mix_ns_max) / 1e6, double(p_profile.block_ns) / 1e6, pct(p_profile.share_p50),
			pct(p_profile.share_p99), p_profile.live_instances, p_profile.live_voices_max, (unsigned long long)p_profile.culled_precull,
			(unsigned long long)p_profile.culled_scope, (unsigned long long)p_profile.culled_cap, p_profile.suspended, double(p_profile.window_bottom_db),
			double(p_profile.window_top_db));
	return line;
}

// PT1 snd (docs/design/pt1-snd.md): voice cut diagnostics (mixer_core.h MixerCutStats).

void MixerCore::Impl::pt1_cut(MixerCutKind p_kind, const InstanceSlot &p_slot, float p_step) {
	pt1_event(p_kind, p_slot.handle, p_step, p_slot.instance);
}

void MixerCore::Impl::pt1_event(MixerCutKind p_kind, InstanceHandle p_handle, float p_step, const PatchInstance *p_instance) {
	const size_t k = std::min(size_t(p_kind), kMixerCutKinds - 1);
	const float step = std::isfinite(p_step) ? std::fabs(p_step) : 0.0f;
	cut_stats.events[k].fetch_add(1, std::memory_order_relaxed);
	if (p_kind != MixerCutKind::CallGap && p_kind != MixerCutKind::LateBlock) {
		if (step > kPt1AudibleStep) {
			cut_stats.audible[k].fetch_add(1, std::memory_order_relaxed);
		}
		if (step > kPt1LoudStep) {
			cut_stats.loud[k].fetch_add(1, std::memory_order_relaxed);
		}
	}
	if (step > cut_stats.step_max[k].load(std::memory_order_relaxed)) {
		cut_stats.step_max[k].store(step, std::memory_order_relaxed);
	}
	if (!cut_record.load(std::memory_order_relaxed)) {
		return;
	}
	MixerCutEvent e;
	e.frame = block_first;
	e.handle = p_handle;
	e.kind = p_kind;
	e.step = step;
	if (p_instance != nullptr) {
		const VoiceCullInfo info = p_instance->cull_info();
		e.loudness_db = info.loudness_db;
		e.distance_m = info.distance_m;
		const std::string &name = p_instance->program().name;
		const size_t n = std::min(name.size(), sizeof(e.program) - 1);
		std::memcpy(e.program, name.data(), n); // no allocation: a copy into the event
		e.program[n] = 0;
	}
	if (!cut_ring.push(e)) {
		cut_stats.event_drops.fetch_add(1, std::memory_order_relaxed);
	}
}

bool MixerCore::Impl::pt1_begin_fade(InstanceSlot &r_slot, uint8_t p_mode, MixerCutKind p_kind) {
	if (config.pt1_cut_fade_frames == 0 || !r_slot.pt1_rendered || r_slot.instance == nullptr || r_slot.pt1_fade_mode != 0) {
		return false;
	}
	pt1_cut(MixerCutKind::Fade, r_slot, r_slot.instance->pt1_edge().edge);
	r_slot.instance->pt1_fade_to(0.0f, config.pt1_cut_fade_frames);
	r_slot.pt1_fade_mode = p_mode;
	r_slot.pt1_fade_kind = p_kind;
	return true;
}

const MixerCutStats &MixerCore::pt1_cut_stats() const {
	return impl_->cut_stats;
}

void MixerCore::pt1_record_cuts(bool p_on) {
	impl_->cut_record.store(p_on, std::memory_order_relaxed);
}

size_t MixerCore::pt1_poll_cuts(std::span<MixerCutEvent> r_out) {
	size_t n = 0;
	while (n < r_out.size() && impl_->cut_ring.pop(r_out[n])) {
		++n;
	}
	return n;
}

void MixerCore::Impl::pt1_limit() {
	float *x = block.data();
	const float c = config.pt1_limit_ceiling;
	const bool on = std::isfinite(c) && c > 0.0f;
	uint64_t over_in = 0;
	bool any = false;
	for (uint32_t i = 0; i < kBlockFrames; ++i) {
		const float p = std::max(std::fabs(x[size_t(i) * 2]), std::fabs(x[size_t(i) * 2 + 1])); // NaN: compares false
		over_in += p > 1.0f ? 1u : 0u;
		any = any || (on && p > c);
		pt1_limit_g[i] = on && p > c ? c / p : 1.0f;
	}
	if (over_in > 0) {
		cut_stats.overs_in.fetch_add(over_in, std::memory_order_relaxed);
	}
	if (!on || (!any && pt1_limit_gain >= 1.0f)) {
		if (!on) {
			cut_stats.overs_out.fetch_add(over_in, std::memory_order_relaxed);
		}
		return; // unity gain: the samples as they were
	}
	const float attack = 1.0f / float(std::clamp<uint32_t>(config.pt1_limit_attack_frames, 1, kBlockFrames));
	const double release_frames = std::clamp(std::isfinite(config.pt1_limit_release_ms) ? double(config.pt1_limit_release_ms) : 80.0, 1.0, 10000.0) *
			double(config.mix_rate) / 1000.0;
	const float k = float(1.0 - std::exp(-1.0 / release_frames));
	// Backward: the gain falls at most `attack` per frame towards a peak, so it is already down when the peak comes.
	for (uint32_t i = kBlockFrames - 1; i-- > 0;) {
		pt1_limit_g[i] = std::min(pt1_limit_g[i], pt1_limit_g[i + 1] + attack);
	}
	// Forward: never above the backward gain, rising back with the release time constant.
	float h = pt1_limit_gain, lowest = 1.0f;
	uint64_t over_out = 0;
	if (pt1_limit_g[0] < h - attack) {
		cut_stats.limit_edge_steps.fetch_add(1, std::memory_order_relaxed); // the peak came too early in the block
	}
	for (uint32_t i = 0; i < kBlockFrames; ++i) {
		h = std::min(pt1_limit_g[i], h + (1.0f - h) * k);
		h = h > 1.0f - 1.0e-6f ? 1.0f : h;
		lowest = std::min(lowest, h);
		x[size_t(i) * 2] *= h;
		x[size_t(i) * 2 + 1] *= h;
		over_out += std::max(std::fabs(x[size_t(i) * 2]), std::fabs(x[size_t(i) * 2 + 1])) > 1.0f ? 1u : 0u;
	}
	pt1_limit_gain = h;
	if (lowest < 1.0f) {
		cut_stats.limit_blocks.fetch_add(1, std::memory_order_relaxed);
	}
	if (lowest < cut_stats.limit_min_gain.load(std::memory_order_relaxed)) {
		cut_stats.limit_min_gain.store(lowest, std::memory_order_relaxed);
	}
	if (over_out > 0) {
		cut_stats.overs_out.fetch_add(over_out, std::memory_order_relaxed);
	}
}

const char *mixer_cut_kind_name(MixerCutKind p_kind) {
	static const char *const kNames[kMixerCutKinds] = { "drop", "pend", "kill", "finish", "start", "resume", "suspend", "unsuspend", "fade", "callgap", "lateblock" };
	return size_t(p_kind) < kMixerCutKinds ? kNames[size_t(p_kind)] : "?";
}

std::string format_mixer_cuts(const MixerCutStats &p_stats) {
	std::string out = "audio cuts (events / > -60 dBFS / > -40 dBFS / max step):";
	char part[200];
	for (size_t k = 0; k < kMixerCutKinds; ++k) {
		if (MixerCutKind(k) == MixerCutKind::CallGap || MixerCutKind(k) == MixerCutKind::LateBlock) {
			continue;
		}
		std::snprintf(part, sizeof(part), " %s %llu/%llu/%llu/%.4f", mixer_cut_kind_name(MixerCutKind(k)), (unsigned long long)p_stats.events[k].load(),
				(unsigned long long)p_stats.audible[k].load(), (unsigned long long)p_stats.loud[k].load(), double(p_stats.step_max[k].load()));
		out += part;
	}
	const uint64_t calls = p_stats.calls.load();
	std::snprintf(part, sizeof(part), "; abrupt gain ramps %llu (max step %.4f); mix() calls %llu, %llu..%llu frames, gaps ms",
			(unsigned long long)p_stats.abrupt_gain.load(), double(p_stats.abrupt_gain_max.load()), (unsigned long long)calls,
			(unsigned long long)(calls > 0 ? p_stats.call_frames_min.load() : 0), (unsigned long long)p_stats.call_frames_max.load());
	out += part;
	static const char *const kGap[8] = { "<5", "<10", "<15", "<20", "<30", "<50", "<100", ">=100" };
	for (size_t i = 0; i < 8; ++i) {
		std::snprintf(part, sizeof(part), " %s:%llu", kGap[i], (unsigned long long)p_stats.gap_hist[i].load());
		out += part;
	}
	const size_t g = size_t(MixerCutKind::CallGap);
	const size_t lb = size_t(MixerCutKind::LateBlock);
	std::snprintf(part, sizeof(part), "; late calls (> %.0f ms) %llu, max %.1f ms; late blocks %llu, max %.1f ms; event drops %llu", kPt1LateCallMs,
			(unsigned long long)p_stats.events[g].load(), double(p_stats.step_max[g].load()), (unsigned long long)p_stats.events[lb].load(),
			double(p_stats.step_max[lb].load()), (unsigned long long)p_stats.event_drops.load());
	out += part;
	const float lg = p_stats.limit_min_gain.load();
	std::snprintf(part, sizeof(part), "; limiter: %llu blocks, down to %.1f dB, %llu edge steps; frames over 0 dBFS %llu before it, %llu after",
			(unsigned long long)p_stats.limit_blocks.load(), lg > 0.0f ? 20.0 * std::log10(double(lg)) : -999.0, (unsigned long long)p_stats.limit_edge_steps.load(),
			(unsigned long long)p_stats.overs_in.load(), (unsigned long long)p_stats.overs_out.load());
	out += part;
	return out;
}

} // namespace bf3::audio
