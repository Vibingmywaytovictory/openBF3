#include "bf3/audio/mixer_core.h"

#include "bf3/audio/dsp.h"
#include "bf3/audio/hdr_window.h"
#include "bf3/audio/voice_cull.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>
#define OPENBF3_HAVE_MXCSR 1
#endif

namespace bf3::audio {

namespace {

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
	char pad_[60] = {};
	std::atomic<uint32_t> tail_{ 0 };
};

struct InstanceSlot {
	InstanceHandle handle = kNoInstance;
	PatchInstance *instance = nullptr;
	bool stop_requested = false;
	bool starting = false;
	bool pending = false;
	uint64_t start_frame = 0;
	uint16_t scope = core::audio::kNoScope;
	bool has_rendered = false;
	bool suspended_seen = false;
	uint64_t abrupt_seen = 0;
	uint8_t fade_mode = 0;
	MixerCutKind fade_kind = MixerCutKind::Drop;
	uint32_t voices_held = 0;
	uint32_t hold_left = 0;
	bool awaiting_event = false;
};
constexpr uint8_t kFadeRelease = 1;
constexpr uint8_t kFadePark = 2;

void atomic_max(std::atomic<uint64_t> &r_value, uint64_t p_candidate) {
	uint64_t cur = r_value.load(std::memory_order_relaxed);
	while (p_candidate > cur && !r_value.compare_exchange_weak(cur, p_candidate, std::memory_order_relaxed)) {
	}
}

}

struct MixerCore::Impl {
	MixerConfig config;
	std::thread::id owner;
	std::atomic<InstanceHandle> next_handle{ 1 };
	SpscRing<MixerCommand> commands;
	SpscRing<PatchInstance *> returns;
	SpscRing<MixerFeedback> feedback;
	std::vector<MixerCommand> pending;
	size_t pending_count = 0;

	std::vector<std::pair<InstanceHandle, uint64_t>> held_starts;
	std::vector<PatchInstance *> return_overflow;
	size_t return_overflow_count = 0;
	std::vector<InstanceSlot> instances;
	std::vector<float> bus_storage;
	BusBuffers buses;
	std::vector<float> block;
	uint32_t block_pos = kBlockFrames;
	ListenerState listener;
	std::vector<std::pair<uint32_t, float>> mixer_values;
	size_t mixer_value_count = 0;
	MixTap *tap = nullptr;
	std::atomic<MixTap *> sum_tap{ nullptr };
	std::vector<float> sum_block;
	float output_gain = 1.0f;
	std::array<float, size_t(core::audio::MasterBus::Unknown)> bus_gain{};
	std::array<std::atomic<float>, size_t(core::audio::MasterBus::Unknown)> bus_gain_seen{};
	std::array<std::atomic<uint32_t>, kMixerValueSlots> mixer_value_hash_seen{};
	std::array<std::atomic<float>, kMixerValueSlots> mixer_value_seen{};
	std::atomic<size_t> mixer_values_seen{ 0 };
	std::atomic<uint64_t> rendered{ 0 };
	std::chrono::steady_clock::time_point last_call{};
	bool have_last_call = false;

	HdrWindow window;
	bool a3_on = false;
	std::vector<CullVoice> cull_voices;
	std::vector<CullVerdict> cull_verdicts;
	std::vector<uint32_t> cull_slots;
	std::vector<CullVerdict> cull_park;
	void a3_init();
	void a3_block(MixerStats &stats, uint64_t p_first);
	MixerCutStats cut_stats;
	SpscRing<MixerCutEvent> cut_ring{ 4096 };
	std::atomic<bool> cut_record{ false };
	std::atomic<bool> metering{ false };
	std::atomic<uint64_t> level_blocks{ 0 };
	std::array<std::atomic<float>, kMeteredBuses> level_bus_peak{};
	std::array<std::atomic<double>, kMeteredBuses> level_bus_energy{};
	std::atomic<float> level_pre_peak{ 0.0f };
	std::atomic<double> level_pre_energy{ 0.0 };
	std::array<std::atomic<uint64_t>, kLimitEdgesDb.size() + 1> level_limit_blocks{};
	std::atomic<uint64_t> level_master_clipped{ 0 };
	std::atomic<float> level_master_compressor{ 0.0f };
	MasterMixer master;
	std::atomic<bool> master_stats_reset{ false };
	std::atomic<int32_t> master_preset{ -1 };
	std::atomic<uint32_t> master_speakers{ 0 };
	void publish_master();
	void meter_block(const BusBuffers &p_buses);
	uint64_t block_first = 0;
	void note_cut(MixerCutKind p_kind, const InstanceSlot &p_slot, float p_step);
	void note_event(MixerCutKind p_kind, InstanceHandle p_handle, float p_step, const PatchInstance *p_instance,
			uint8_t p_reason = 0, uint16_t p_scope = core::audio::kNoScope, uint32_t p_count = 0);

	bool begin_fade(InstanceSlot &r_slot, uint8_t p_mode, MixerCutKind p_kind);
	bool join(const MixerCommand &p_start, uint64_t p_frame, MixerStats &stats);
	float limit_gain = 1.0f;
	std::array<float, 2 * kBlockFrames> limit_g{};
	std::array<float, 4 * kBlockFrames> limit_ext{};
	uint32_t limit_lookahead_frames() const {
		return std::clamp<uint32_t>(config.limit_attack_frames, 1, kBlockFrames);
	}
	void apply_limiter();

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
		sum_block.assign(size_t(kBlockFrames) * 2, 0.0f);
		mixer_values.resize(kMixerValueSlots);
		output_gain = db_to_gain(p_config.output_gain_db);
		master.configure(p_config.master, p_config.mix_rate, p_config.master_iir2_q);
		publish_master();
		bus_gain.fill(1.0f);
		for (std::atomic<float> &g : bus_gain_seen) {
			g.store(1.0f, std::memory_order_relaxed);
		}
		a3_init();
	}

	void render_block(MixerStats &stats);

	void give_back(PatchInstance *p_instance) {
		if (p_instance == nullptr) {
			return;
		}
		if (!returns.push(p_instance)) {
			if (return_overflow_count < return_overflow.size()) {
				return_overflow[return_overflow_count++] = p_instance;
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

uint32_t MixerCore::output_delay_frames() const {
	return impl_->limit_lookahead_frames();
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

void MixerCore::set_sum_tap(MixTap *p_tap) {
	impl_->sum_tap.store(p_tap, std::memory_order_release);
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

}

bool MixerCore::Impl::join(const MixerCommand &p_start, uint64_t p_frame, MixerStats &stats) {
	InstanceSlot *s = find_slot(instances, p_start.join);
	if (s == nullptr || s->pending || s->fade_mode != 0 || &s->instance->program() != &p_start.instance->program()) {
		return false;
	}
	const InstanceHandle previous = s->handle;
	note_event(MixerCutKind::Request, p_start.handle, 0.0f, p_start.instance, 0, s->scope);
	note_event(MixerCutKind::Join, previous, 0.0f, s->instance, 0, s->scope, p_start.handle);
	s->instance->reset_out_peak();
	MixerFeedback fb;
	fb.handle = previous;
	fb.frame = block_first;
	fb.finished = true;
	if (!feedback.push(fb)) {
		stats.feedback_drops.fetch_add(1, std::memory_order_relaxed);
	}
	s->handle = p_start.handle;
	s->stop_requested = false;
	s->starting = true;
	s->awaiting_event = true;
	s->start_frame = p_frame;
	give_back(p_start.instance);
	return true;
}

void MixerCore::Impl::render_block(MixerStats &stats) {
	Impl &m = *this;
	const auto t0 = std::chrono::steady_clock::now();
	const uint64_t first = m.rendered.load(std::memory_order_relaxed);
	const uint64_t end = first + kBlockFrames;
	m.block_first = first;

	size_t kept = 0;
	for (size_t i = 0; i < m.return_overflow_count; ++i) {
		if (!m.returns.push(m.return_overflow[i])) {
			m.return_overflow[kept++] = m.return_overflow[i];
		}
	}
	m.return_overflow_count = kept;

	MixerCommand c;
	while (m.pending_count < m.pending.size() && m.commands.pop(c)) {
		m.pending[m.pending_count++] = c;
	}

	size_t write = 0, held = 0;
	for (size_t i = 0; i < m.pending_count; ++i) {
		MixerCommand &cmd = m.pending[i];
		if (cmd.type != MixerCommandType::Start && cmd.handle != kNoInstance) {
			for (size_t k = 0; k < held; ++k) {
				if (m.held_starts[k].first == cmd.handle && cmd.at_frame < m.held_starts[k].second) {
					if (cmd.at_frame != 0 && cmd.at_frame < first) {
						stats.late_commands.fetch_add(1, std::memory_order_relaxed);
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
				if (cmd.instance != nullptr && m.join(cmd, first + offset, stats)) {
					break;
				}
				if (free == nullptr || cmd.instance == nullptr) {
					if (cmd.instance != nullptr) {
						m.note_event(MixerCutKind::Request, cmd.handle, 0.0f, cmd.instance, 0,
								m.config.scopes ? m.config.scopes->scope_of(cmd.instance->program().partition)
												: core::audio::kNoScope);
						m.note_event(MixerCutKind::Refused, cmd.handle, 0.0f, cmd.instance);
						MixerFeedback fb;
						fb.handle = cmd.handle;
						fb.frame = first;
						fb.finished = true;
						if (!m.feedback.push(fb)) {
							stats.feedback_drops.fetch_add(1, std::memory_order_relaxed);
						}
					}
					stats.starved_voices.fetch_add(cmd.instance ? 1 : 0, std::memory_order_relaxed);
					m.give_back(cmd.instance);
					break;
				}
				*free = InstanceSlot{};
				free->handle = cmd.handle;
				free->instance = cmd.instance;
				free->stop_requested = false;
				free->starting = true;
				free->pending = false;
				free->start_frame = first + offset;
				free->scope = m.config.scopes ? m.config.scopes->scope_of(cmd.instance->program().partition) : core::audio::kNoScope;
				m.note_event(MixerCutKind::Request, cmd.handle, 0.0f, cmd.instance, 0, free->scope);
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
					if (s->fade_mode != 0 || m.begin_fade(*s, kFadeRelease, MixerCutKind::Kill)) {
						s->fade_mode = kFadeRelease;
						s->fade_kind = MixerCutKind::Kill;
						break;
					}
					if (s->has_rendered) {
						m.note_cut(MixerCutKind::Kill, *s, s->instance->edge().edge);
					}
					m.give_back(s->instance);
					*s = InstanceSlot{};
				}
				break;
			case MixerCommandType::SetParameter:
				if (InstanceSlot *s = find_slot(m.instances, cmd.handle)) {
					s->instance->set_parameter(cmd.name_hash, cmd.value);
				}
				break;
			case MixerCommandType::PostEvent:
				if (InstanceSlot *s = find_slot(m.instances, cmd.handle)) {
					s->awaiting_event = false;
					s->instance->post_event(cmd.name_hash, offset);
				}
				break;
			case MixerCommandType::SetMixerValue: {
				bool found = false;
				for (size_t k = 0; k < m.mixer_value_count; ++k) {
					if (m.mixer_values[k].first == cmd.name_hash) {
						m.mixer_values[k].second = cmd.value;
						m.mixer_value_seen[k].store(cmd.value, std::memory_order_relaxed);
						found = true;
					}
				}
				if (!found && m.mixer_value_count < m.mixer_values.size()) {
					m.mixer_value_seen[m.mixer_value_count].store(cmd.value, std::memory_order_relaxed);
					m.mixer_value_hash_seen[m.mixer_value_count].store(cmd.name_hash, std::memory_order_relaxed);
					m.mixer_values[m.mixer_value_count++] = { cmd.name_hash, cmd.value };
					m.mixer_values_seen.store(m.mixer_value_count, std::memory_order_release);
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
			case MixerCommandType::SetMasterPreset:
				if (m.master.select(uint32_t(std::max(0.0f, cmd.value)), cmd.name_hash)) {
					m.publish_master();
				}
				break;
			case MixerCommandType::SetBusGain:
				if (cmd.name_hash < m.bus_gain.size() && std::isfinite(cmd.value) && cmd.value >= 0.0f) {
					m.bus_gain[cmd.name_hash] = cmd.value;
					m.bus_gain_seen[cmd.name_hash].store(cmd.value, std::memory_order_relaxed);
				}
				break;
		}
	}
	m.pending_count = write;
	m.a3_block(stats, first);

	std::fill(m.bus_storage.begin(), m.bus_storage.end(), 0.0f);
	m.buses.frames = kBlockFrames;
	m.buses.first_frame = first;
	uint32_t active = 0;
	for (InstanceSlot &s : m.instances) {
		if (s.instance == nullptr) {
			continue;
		}
		if (s.pending) {
			s.has_rendered = false;
			continue;
		}
		const PatchInstanceStats before = s.instance->stats();
		s.instance->set_metering(m.metering.load(std::memory_order_relaxed));
		s.instance->process_block(m.buses);
		const PatchInstanceStats &after = s.instance->stats();
		{
			const InstanceEdge e = s.instance->edge();
			if (!s.has_rendered) {
				m.note_cut(s.start_frame + kBlockFrames > first ? MixerCutKind::Start : MixerCutKind::Resume, s, e.head);
				s.has_rendered = true;
			}
			if (e.abrupt_gain != s.abrupt_seen) {
				m.cut_stats.abrupt_gain.fetch_add(e.abrupt_gain - s.abrupt_seen, std::memory_order_relaxed);
				s.abrupt_seen = e.abrupt_gain;
				if (e.abrupt_gain_max > m.cut_stats.abrupt_gain_max.load(std::memory_order_relaxed)) {
					m.cut_stats.abrupt_gain_max.store(e.abrupt_gain_max, std::memory_order_relaxed);
				}
			}
		}
		stats.starved_voices.fetch_add(after.starved_slots - before.starved_slots, std::memory_order_relaxed);
		stats.decode_errors.fetch_add(after.decode_errors - before.decode_errors, std::memory_order_relaxed);
		stats.deferred_events.fetch_add(after.deferred_events - before.deferred_events, std::memory_order_relaxed);
		m.cut_stats.slot_takeovers.fetch_add(after.slot_takeovers - before.slot_takeovers, std::memory_order_relaxed);
		m.cut_stats.takeover_cuts.fetch_add(after.takeover_cuts - before.takeover_cuts, std::memory_order_relaxed);
		m.cut_stats.held_triggers.fetch_add(after.held_triggers - before.held_triggers, std::memory_order_relaxed);
		m.cut_stats.pan_steps.fetch_add(after.pan_steps - before.pan_steps, std::memory_order_relaxed);
		if (after.pan_step_max > m.cut_stats.pan_step_max.load(std::memory_order_relaxed)) {
			m.cut_stats.pan_step_max.store(after.pan_step_max, std::memory_order_relaxed);
		}
		if (after.takeover_cut_max > m.cut_stats.takeover_cut_max.load(std::memory_order_relaxed)) {
			m.cut_stats.takeover_cut_max.store(after.takeover_cut_max, std::memory_order_relaxed);
		}
		if (s.fade_mode != 0 && s.instance->fade_done()) {
			m.note_cut(s.fade_kind, s, s.instance->edge().edge);
			if (s.fade_mode == kFadePark) {
				s.pending = true;
				s.has_rendered = false;
				s.fade_mode = 0;
				++active;
				continue;
			}
			m.give_back(s.instance);
			s = InstanceSlot{};
			continue;
		}
		const bool done = s.instance->finished() &&
				(s.stop_requested || (s.instance->stats().sampler_triggers > 0 && !s.awaiting_event));
		const PatchHaptics h = s.instance->haptics();
		const bool haptic = h.shake_pitch != 0.0f || h.shake_roll != 0.0f || h.shake_yaw != 0.0f || h.rumble_low != 0.0f || h.rumble_high != 0.0f;
		if ((done || haptic) && s.fade_mode != kFadeRelease) {
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
			m.note_cut(MixerCutKind::Finish, s, s.instance->edge().edge);
			m.give_back(s.instance);
			s = InstanceSlot{};
		} else {
			++active;
		}
	}
	stats.active_instances.store(active, std::memory_order_relaxed);
	if (active > stats.peak_instances.load(std::memory_order_relaxed)) {
		stats.peak_instances.store(active, std::memory_order_relaxed);
	}

	if (MixTap *st = m.sum_tap.load(std::memory_order_acquire)) {
		for (uint32_t i = 0; i < kBlockFrames; ++i) {
			float l = 0.0f, r = 0.0f;
			for (size_t b = 0; b < size_t(core::audio::MasterBus::Unknown); ++b) {
				l += m.buses.channels[b][0][i] * m.bus_gain[b];
				r += m.buses.channels[b][1][i] * m.bus_gain[b];
			}
			m.sum_block[size_t(i) * 2] = l;
			m.sum_block[size_t(i) * 2 + 1] = r;
		}
		st->on_block(m.sum_block.data(), kBlockFrames, first);
	}
	if (m.master_stats_reset.exchange(false, std::memory_order_relaxed)) {
		m.master.reset_stats();
	}
	if (m.master.active()) {
		m.master.process(m.buses, m.bus_gain.data(), m.block.data(), kBlockFrames);
		for (float &x : m.block) {
			x *= m.output_gain;
		}
	} else {
		for (uint32_t i = 0; i < kBlockFrames; ++i) {
			float l = 0.0f, r = 0.0f;
			for (size_t b = 0; b < size_t(core::audio::MasterBus::Unknown); ++b) {
				l += m.buses.channels[b][0][i] * m.bus_gain[b];
				r += m.buses.channels[b][1][i] * m.bus_gain[b];
			}
			m.block[size_t(i) * 2] = l * m.output_gain;
			m.block[size_t(i) * 2 + 1] = r * m.output_gain;
		}
	}
	if (m.metering.load(std::memory_order_relaxed)) {
		m.meter_block(m.buses);
	}
	m.apply_limiter();
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
		m.note_event(MixerCutKind::LateBlock, kNoInstance, float(double(ns) / 1e6), nullptr);
	}
	record_mix_ns(stats, ns, budget_ns);
}

float MixerCore::bus_gain(core::audio::MasterBus p_bus) const {
	const size_t b = size_t(p_bus);
	return b < impl_->bus_gain_seen.size() ? impl_->bus_gain_seen[b].load(std::memory_order_relaxed) : 0.0f;
}

std::optional<float> MixerCore::mixer_value(uint32_t p_name_hash) const {
	const size_t n = impl_->mixer_values_seen.load(std::memory_order_acquire);
	for (size_t k = 0; k < n; ++k) {
		if (impl_->mixer_value_hash_seen[k].load(std::memory_order_relaxed) == p_name_hash) {
			return impl_->mixer_value_seen[k].load(std::memory_order_relaxed);
		}
	}
	return std::nullopt;
}

uint32_t MixerCore::mix(float *r_interleaved_stereo, uint32_t p_frames) {
	Impl &m = *impl_;
#if OPENBF3_HAVE_MXCSR
	const unsigned int saved_csr = _mm_getcsr();
	_mm_setcsr(saved_csr | 0x8040);
#endif
	const auto now = std::chrono::steady_clock::now();
	if (m.have_last_call) {
		atomic_max(stats_.max_call_gap_ns, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - m.last_call).count()));
		const double gap_ms = std::chrono::duration<double, std::milli>(now - m.last_call).count();
		size_t gb = kGapEdgesMs.size();
		for (size_t i = 0; i < kGapEdgesMs.size(); ++i) {
			if (gap_ms < kGapEdgesMs[i]) {
				gb = i;
				break;
			}
		}
		m.cut_stats.gap_hist[gb].fetch_add(1, std::memory_order_relaxed);
		if (gap_ms > kLateCallMs) {
			m.block_first = m.rendered.load(std::memory_order_relaxed);
			m.note_event(MixerCutKind::CallGap, kNoInstance, float(gap_ms), nullptr);
		}
	}
	m.cut_stats.calls.fetch_add(1, std::memory_order_relaxed);
	atomic_max(m.cut_stats.call_frames_max, p_frames);
	if (p_frames < m.cut_stats.call_frames_min.load(std::memory_order_relaxed)) {
		m.cut_stats.call_frames_min.store(p_frames, std::memory_order_relaxed);
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

void MixerCore::Impl::a3_init() {
	a3_on = config.hdr.has_value() || config.scopes != nullptr || instances.size() > config.max_voices;
	if (config.hdr) {
		window.configure(*config.hdr, config.mix_rate);
	}
	if (a3_on) {
		cull_voices.resize(instances.size());
		cull_verdicts.resize(instances.size());
		cull_slots.resize(instances.size());
		cull_park.resize(instances.size());
	}
}

void MixerCore::Impl::a3_block(MixerStats &stats, uint64_t p_first) {
	if (!a3_on) {
		return;
	}
	const bool hdr_on = config.hdr.has_value();
	const auto drop = [&](InstanceSlot &r_slot) {
		const bool fades = begin_fade(r_slot, kFadeRelease, MixerCutKind::Drop);
		if (r_slot.has_rendered && !fades) {
			note_cut(MixerCutKind::Drop, r_slot, r_slot.instance->edge().edge);
		}
		MixerFeedback fb;
		fb.handle = r_slot.handle;
		fb.frame = p_first;
		fb.finished = true;
		if (!feedback.push(fb)) {
			stats.feedback_drops.fetch_add(1, std::memory_order_relaxed);
		}
		if (fades) {
			return;
		}
		give_back(r_slot.instance);
		r_slot = InstanceSlot{};
	};
	size_t n = 0;
	uint32_t reserved = 0;
	for (size_t i = 0; i < instances.size(); ++i) {
		InstanceSlot &s = instances[i];
		if (s.instance == nullptr) {
			continue;
		}
		if (s.fade_mode != 0) {
			reserved += std::max<uint32_t>(s.instance->cull_info().voices, 1);
			continue;
		}
		if (s.pending && s.stop_requested) {
			note_event(MixerCutKind::Cull, s.handle, 0.0f, s.instance, uint8_t(CullVerdict::Pending), s.scope);
			drop(s);
			continue;
		}
		const VoiceCullInfo info = s.instance->cull_info();
		CullVoice &v = cull_voices[n];
		v = CullVoice{};
		v.handle = s.handle;
		v.loudness_db = info.loudness_db;
		v.priority = 1.0f;
		v.scope = s.scope;
		v.position[0] = info.position.x;
		v.position[1] = info.position.y;
		v.position[2] = info.position.z;
		v.distance_m = info.distance_m;
		v.start_frame = s.start_frame;
		v.looping = s.instance->program().is_looping;
		v.starting = s.starting || s.pending;
		v.hdr = hdr_on && info.hdr;
		v.voices = std::max<uint32_t>(info.voices, 1);
		if (!s.pending) {
			if (v.voices >= s.voices_held || s.hold_left == 0) {
				s.hold_left = v.voices >= s.voices_held ? config.voice_hold_blocks : 0u;
				s.voices_held = v.voices;
			} else {
				--s.hold_left;
			}
		}
		v.held_voices = std::max(v.voices, s.voices_held);
		v.parked = s.pending;
		v.hold_db = !s.starting && !s.pending && s.has_rendered ? config.cap_hold_db : 0.0f;
		v.resume_margin = s.pending ? std::min(config.resume_margin, config.max_voices / 16) : 0u;
		cull_slots[n] = uint32_t(i);
		++n;
	}
	CullCounts counts;
	const float precull = hdr_on ? window.precull_db() : -std::numeric_limits<float>::infinity();
	const uint32_t voice_cap = config.max_voices > reserved ? config.max_voices - reserved : 0u;
	audio::cull_voices(std::span<const CullVoice>(cull_voices.data(), n), config.scopes.get(), precull, voice_cap,
			p_first, config.mix_rate, std::span<CullVerdict>(cull_verdicts.data(), n), counts,
			std::span<CullVerdict>(cull_park.data(), n));
	float loudest = -std::numeric_limits<float>::infinity();
	uint32_t live = reserved;
	for (size_t k = 0; k < n; ++k) {
		InstanceSlot &s = instances[cull_slots[k]];
		switch (cull_verdicts[k]) {
			case CullVerdict::Keep:
				if (s.pending) {
					const uint32_t stale = s.instance->coalesce_events();
					cut_stats.stale_events.fetch_add(stale, std::memory_order_relaxed);
					if (stale > 0) {
						note_event(MixerCutKind::Coalesce, s.handle, 0.0f, s.instance, 0, s.scope, stale);
					}
				}
				if (s.pending && config.cut_fade_frames > 0) {
					s.instance->set_fade(0.0f);
					s.instance->fade_to(1.0f, config.cut_fade_frames);
				}
				s.pending = false;
				s.starting = false;
				live += cull_voices[k].voices;
				if (cull_voices[k].hdr) {
					loudest = std::max(loudest, cull_voices[k].loudness_db);
				}
				break;
			case CullVerdict::Pending:
				if (!s.pending) {
					note_event(MixerCutKind::Park, s.handle, 0.0f, s.instance, uint8_t(cull_park[k]), s.scope);
				}
				if (!s.pending && begin_fade(s, kFadePark, MixerCutKind::Pend)) {
					break;
				}
				if (!s.pending && s.has_rendered) {
					note_cut(MixerCutKind::Pend, s, s.instance->edge().edge);
				}
				s.pending = true;
				break;
			default:
				note_event(MixerCutKind::Cull, s.handle, 0.0f, s.instance, uint8_t(cull_verdicts[k]), s.scope);
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
			w.alone_top_db = std::max(h.window_min_top_db, cull_voices[k].loudness_db);
			w.suspended = w.active && window.suspended(cull_voices[k].loudness_db);
			suspended += w.suspended ? 1 : 0;
			if (w.suspended != s.suspended_seen) {
				if (s.has_rendered) {
					note_cut(w.suspended ? MixerCutKind::Suspend : MixerCutKind::Unsuspend, s, s.instance->edge().edge);
				}
				s.suspended_seen = w.suspended;
			}
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
	size_t bucket = kMixHistEdges.size();
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

void MixerCore::Impl::note_cut(MixerCutKind p_kind, const InstanceSlot &p_slot, float p_step) {
	note_event(p_kind, p_slot.handle, p_step, p_slot.instance);
}

void MixerCore::Impl::note_event(MixerCutKind p_kind, InstanceHandle p_handle, float p_step,
		const PatchInstance *p_instance, uint8_t p_reason, uint16_t p_scope, uint32_t p_count) {
	const size_t k = std::min(size_t(p_kind), kMixerCutKinds - 1);
	const float step = std::isfinite(p_step) ? std::fabs(p_step) : 0.0f;
	cut_stats.events[k].fetch_add(1, std::memory_order_relaxed);
	if (p_kind != MixerCutKind::CallGap && p_kind != MixerCutKind::LateBlock) {
		if (step > kAudibleStep) {
			cut_stats.audible[k].fetch_add(1, std::memory_order_relaxed);
		}
		if (step > kLoudStep) {
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
	e.reason = p_reason;
	e.scope = p_scope;
	e.count = p_count;
	if (p_instance != nullptr) {
		const VoiceCullInfo info = p_instance->cull_info();
		e.loudness_db = info.loudness_db;
		e.distance_m = info.distance_m;
		e.peak = p_instance->stats().out_peak;
		const std::string &name = p_instance->program().name;
		const size_t n = std::min(name.size(), sizeof(e.program) - 1);
		std::memcpy(e.program, name.data(), n);
		e.program[n] = 0;
	}
	if (!cut_ring.push(e)) {
		cut_stats.event_drops.fetch_add(1, std::memory_order_relaxed);
	}
}

bool MixerCore::Impl::begin_fade(InstanceSlot &r_slot, uint8_t p_mode, MixerCutKind p_kind) {
	if (config.cut_fade_frames == 0 || !r_slot.has_rendered || r_slot.instance == nullptr || r_slot.fade_mode != 0) {
		return false;
	}
	note_cut(MixerCutKind::Fade, r_slot, r_slot.instance->edge().edge);
	r_slot.instance->fade_to(0.0f, config.cut_fade_frames);
	r_slot.fade_mode = p_mode;
	r_slot.fade_kind = p_kind;
	return true;
}

const MixerCutStats &MixerCore::cut_stats() const {
	return impl_->cut_stats;
}

void MixerCore::record_cuts(bool p_on) {
	impl_->cut_record.store(p_on, std::memory_order_relaxed);
}

size_t MixerCore::poll_cuts(std::span<MixerCutEvent> r_out) {
	size_t n = 0;
	while (n < r_out.size() && impl_->cut_ring.pop(r_out[n])) {
		++n;
	}
	return n;
}

void MixerCore::Impl::apply_limiter() {
	float *x = block.data();
	const float c = config.limit_ceiling;
	const bool on = std::isfinite(c) && c > 0.0f;
	const uint32_t look = limit_lookahead_frames();
	const uint32_t n = kBlockFrames + look;
	float *e = limit_ext.data();
	std::memcpy(e + size_t(look) * 2, x, size_t(kBlockFrames) * 2 * sizeof(float));
	uint64_t over_in = 0;
	bool any = false;
	for (uint32_t i = 0; i < n; ++i) {
		const float p = std::max(std::fabs(e[size_t(i) * 2]), std::fabs(e[size_t(i) * 2 + 1]));
		over_in += i >= look && p > 1.0f ? 1u : 0u;
		any = any || (on && p > c);
		limit_g[i] = on && p > c ? c / p : 1.0f;
	}
	if (over_in > 0) {
		cut_stats.overs_in.fetch_add(over_in, std::memory_order_relaxed);
	}
	std::memcpy(x, e, size_t(kBlockFrames) * 2 * sizeof(float));
	std::memmove(e, e + size_t(kBlockFrames) * 2, size_t(look) * 2 * sizeof(float));
	if (!on || (!any && limit_gain >= 1.0f)) {
		if (!on) {
			cut_stats.overs_out.fetch_add(over_in, std::memory_order_relaxed);
		}
		return;
	}
	const float attack = 1.0f / float(look);
	const double release_frames = std::clamp(std::isfinite(config.limit_release_ms) ? double(config.limit_release_ms) : 80.0, 1.0, 10000.0) *
			double(config.mix_rate) / 1000.0;
	const float k = float(1.0 - std::exp(-1.0 / release_frames));

	for (uint32_t i = n - 1; i-- > 0;) {
		limit_g[i] = std::min(limit_g[i], limit_g[i + 1] + attack);
	}

	float h = limit_gain, lowest = 1.0f;
	uint64_t over_out = 0;
	if (limit_g[0] < h - attack) {
		cut_stats.limit_edge_steps.fetch_add(1, std::memory_order_relaxed);
		const float jump = (h - limit_g[0]) * std::max(std::fabs(x[0]), std::fabs(x[1]));
		if (jump > cut_stats.limit_edge_step_max.load(std::memory_order_relaxed)) {
			cut_stats.limit_edge_step_max.store(jump, std::memory_order_relaxed);
		}
	}
	for (uint32_t i = 0; i < kBlockFrames; ++i) {
		h = std::min(limit_g[i], h + (1.0f - h) * k);
		h = h > 1.0f - 1.0e-6f ? 1.0f : h;
		lowest = std::min(lowest, h);
		x[size_t(i) * 2] *= h;
		x[size_t(i) * 2 + 1] *= h;
		over_out += std::max(std::fabs(x[size_t(i) * 2]), std::fabs(x[size_t(i) * 2 + 1])) > 1.0f ? 1u : 0u;
	}
	limit_gain = h;
	if (lowest < 0.98855f) {
		cut_stats.limit_blocks.fetch_add(1, std::memory_order_relaxed);
	}
	if (metering.load(std::memory_order_relaxed)) {
		const float db = lowest > 0.0f ? 20.0f * std::log10(lowest) : -200.0f;
		size_t bucket = 0;
		while (bucket < kLimitEdgesDb.size() && db < kLimitEdgesDb[bucket]) {
			++bucket;
		}
		if (bucket > 0) {
			level_limit_blocks[bucket].fetch_add(1, std::memory_order_relaxed);
		}
	}
	if (lowest < cut_stats.limit_min_gain.load(std::memory_order_relaxed)) {
		cut_stats.limit_min_gain.store(lowest, std::memory_order_relaxed);
	}
	if (over_out > 0) {
		cut_stats.overs_out.fetch_add(over_out, std::memory_order_relaxed);
	}
}

void MixerCore::Impl::meter_block(const BusBuffers &p_buses) {
	for (size_t b = 0; b < kMeteredBuses; ++b) {
		float peak = 0.0f;
		double energy = 0.0;
		for (size_t c = 0; c < 2; ++c) {
			const float *x = p_buses.channels[b][c];
			for (uint32_t i = 0; i < kBlockFrames; ++i) {
				const float v = x[i] * bus_gain[b];
				peak = std::max(peak, std::fabs(v));
				energy += double(v) * double(v);
			}
		}
		if (peak > level_bus_peak[b].load(std::memory_order_relaxed)) {
			level_bus_peak[b].store(peak, std::memory_order_relaxed);
		}
		level_bus_energy[b].store(
				level_bus_energy[b].load(std::memory_order_relaxed) + energy, std::memory_order_relaxed);
	}
	float peak = 0.0f;
	double energy = 0.0;
	for (size_t i = 0; i < size_t(kBlockFrames) * 2; ++i) {
		peak = std::max(peak, std::fabs(block[i]));
		energy += double(block[i]) * double(block[i]);
	}
	if (peak > level_pre_peak.load(std::memory_order_relaxed)) {
		level_pre_peak.store(peak, std::memory_order_relaxed);
	}
	level_pre_energy.store(level_pre_energy.load(std::memory_order_relaxed) + energy, std::memory_order_relaxed);
	level_master_clipped.store(master.stats().ceiling_clipped_samples, std::memory_order_relaxed);
	level_master_compressor.store(master.stats().compressor_min_db, std::memory_order_relaxed);
	level_blocks.fetch_add(1, std::memory_order_release);
}

void MixerCore::Impl::publish_master() {
	const core::audio::MasterMixPlan *p = master.plan();
	int32_t index = -1;
	if (p != nullptr && config.master) {
		for (const std::vector<core::audio::MasterMixPlan> &plans : config.master->plans) {
			for (size_t k = 0; k < plans.size(); ++k) {
				index = &plans[k] == p ? int32_t(k) : index;
			}
		}
	}
	master_preset.store(index, std::memory_order_relaxed);
	master_speakers.store(p != nullptr ? p->speakers : 0u, std::memory_order_relaxed);
}

void MixerCore::meter(bool p_on) {
	impl_->master_stats_reset.store(p_on, std::memory_order_relaxed);
	impl_->metering.store(p_on, std::memory_order_relaxed);
}

MixerLevels MixerCore::levels() const {
	const Impl &m = *impl_;
	MixerLevels l;
	l.blocks = m.level_blocks.load(std::memory_order_acquire);
	for (size_t b = 0; b < kMeteredBuses; ++b) {
		l.bus_peak[b] = m.level_bus_peak[b].load(std::memory_order_relaxed);
		l.bus_energy[b] = m.level_bus_energy[b].load(std::memory_order_relaxed);
	}
	l.pre_limit_peak = m.level_pre_peak.load(std::memory_order_relaxed);
	l.pre_limit_energy = m.level_pre_energy.load(std::memory_order_relaxed);
	uint64_t limited = 0;
	for (size_t k = 1; k < l.limit_blocks.size(); ++k) {
		l.limit_blocks[k] = m.level_limit_blocks[k].load(std::memory_order_relaxed);
		limited += l.limit_blocks[k];
	}
	l.limit_blocks[0] = l.blocks > limited ? l.blocks - limited : 0;
	l.master_ceiling_clipped_samples = m.level_master_clipped.load(std::memory_order_relaxed);
	l.master_compressor_min_db = m.level_master_compressor.load(std::memory_order_relaxed);
	l.master_preset = m.master_preset.load(std::memory_order_relaxed);
	l.master_speakers = m.master_speakers.load(std::memory_order_relaxed);
	return l;
}

const char *mixer_cut_kind_name(MixerCutKind p_kind) {
	static const char *const kNames[kMixerCutKinds] = { "drop", "pend", "kill", "finish", "start", "resume", "suspend",
		"unsuspend", "fade", "callgap", "lateblock", "request", "refused", "cull", "park", "coalesce", "join" };
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
	std::snprintf(part, sizeof(part), "; late calls (> %.0f ms) %llu, max %.1f ms; late blocks %llu, max %.1f ms; event drops %llu", kLateCallMs,
			(unsigned long long)p_stats.events[g].load(), double(p_stats.step_max[g].load()), (unsigned long long)p_stats.events[lb].load(),
			double(p_stats.step_max[lb].load()), (unsigned long long)p_stats.event_drops.load());
	out += part;
	const float lg = p_stats.limit_min_gain.load();
	std::snprintf(part, sizeof(part),
			"; limiter: %llu blocks below -0.1 dB, down to %.1f dB, %llu edge steps (max %.4f); frames over 0 dBFS "
			"%llu before it, %llu after",
			(unsigned long long)p_stats.limit_blocks.load(), lg > 0.0f ? 20.0 * std::log10(double(lg)) : -999.0,
			(unsigned long long)p_stats.limit_edge_steps.load(), double(p_stats.limit_edge_step_max.load()),
			(unsigned long long)p_stats.overs_in.load(), (unsigned long long)p_stats.overs_out.load());
	out += part;
	std::snprintf(part, sizeof(part),
			"; sampler slot takeovers %llu, %llu cut a sound above -60 dBFS, max step %.4f; %llu stale events dropped "
			"at resume",
			(unsigned long long)p_stats.slot_takeovers.load(), (unsigned long long)p_stats.takeover_cuts.load(),
			double(p_stats.takeover_cut_max.load()), (unsigned long long)p_stats.stale_events.load());
	out += part;
	std::snprintf(part, sizeof(part),
			"; %llu sampler triggers from a trigger input already on at the previous evaluation; %llu pan gain steps "
			"above "
			"-60 dBFS, max %.4f",
			(unsigned long long)p_stats.held_triggers.load(), (unsigned long long)p_stats.pan_steps.load(),
			double(p_stats.pan_step_max.load()));
	out += part;
	return out;
}

}
