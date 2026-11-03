#include "bf3/audio/ir_reverb.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bf3::audio {

namespace {

void split_packed(const float *p_re, const float *p_im, float *r_l_re, float *r_l_im, float *r_r_re, float *r_r_im) {
	for (uint32_t k = 0; k < kIrBins; ++k) {
		const uint32_t m = (kIrFftSize - k) & (kIrFftSize - 1);
		const float a = p_re[k], b = p_im[k], c = p_re[m], d = p_im[m];
		r_l_re[k] = 0.5f * (a + c);
		r_l_im[k] = 0.5f * (b - d);
		r_r_re[k] = 0.5f * (b + d);
		r_r_im[k] = 0.5f * (c - a);
	}
}

void join_packed(
		const float *p_l_re, const float *p_l_im, const float *p_r_re, const float *p_r_im, float *r_re, float *r_im) {
	for (uint32_t k = 0; k < kIrBins; ++k) {
		r_re[k] = p_l_re[k] - p_r_im[k];
		r_im[k] = p_l_im[k] + p_r_re[k];
	}
	for (uint32_t k = kIrBins; k < kIrFftSize; ++k) {
		const uint32_t m = kIrFftSize - k;
		r_re[k] = p_l_re[m] + p_r_im[m];
		r_im[k] = p_r_re[m] - p_l_im[m];
	}
}

}

const float *PreparedImpulse::bins_re(uint32_t p_channel, uint32_t p_partition) const {
	return re.data() + (size_t(p_channel) * partitions + p_partition) * kIrBins;
}

const float *PreparedImpulse::bins_im(uint32_t p_channel, uint32_t p_partition) const {
	return im.data() + (size_t(p_channel) * partitions + p_partition) * kIrBins;
}

uint32_t max_reverb_partitions(float p_max_seconds, uint32_t p_rate) {
	if (!std::isfinite(p_max_seconds) || p_max_seconds <= 0.0f || p_rate == 0) {
		return 0;
	}
	const double frames = std::ceil(double(p_max_seconds) * double(p_rate));
	return uint32_t((uint64_t(frames) + kIrPartitionFrames - 1) / kIrPartitionFrames);
}

core::Result<PreparedImpulse> prepare_impulse(
		const core::audio::ImpulseResponse &p_ir, uint32_t p_max_partitions, const Fft &p_fft) {
	if (p_fft.size() != kIrFftSize) {
		return core::make_error("prepare_impulse: the FFT is not " + std::to_string(kIrFftSize) + " points");
	}
	if (p_ir.channels == 0 || p_ir.channels > kIrChannels || p_ir.frames == 0 ||
			p_ir.samples.size() != size_t(p_ir.channels) * p_ir.frames) {
		return core::make_error("prepare_impulse: " + p_ir.name + " has " + std::to_string(p_ir.channels) +
				" channels of " + std::to_string(p_ir.frames) + " frames in " + std::to_string(p_ir.samples.size()) +
				" samples");
	}
	PreparedImpulse out;
	out.partition = p_ir.partition;
	out.name = p_ir.name;
	out.source_frames = p_ir.frames;
	out.partitions = std::min<uint32_t>(
			p_max_partitions, uint32_t((uint64_t(p_ir.frames) + kIrPartitionFrames - 1) / kIrPartitionFrames));
	out.frames = std::min<uint32_t>(p_ir.frames, out.partitions * kIrPartitionFrames);
	out.re.assign(size_t(kIrChannels) * out.partitions * kIrBins, 0.0f);
	out.im.assign(out.re.size(), 0.0f);
	std::array<float, kIrFftSize> re{}, im{};
	const std::span<const float> left = p_ir.channel(0);
	const std::span<const float> right = p_ir.channel(p_ir.channels > 1 ? 1 : 0);
	for (uint32_t p = 0; p < out.partitions; ++p) {
		re.fill(0.0f);
		im.fill(0.0f);
		const uint32_t first = p * kIrPartitionFrames;
		const uint32_t count = std::min<uint32_t>(kIrPartitionFrames, out.frames - first);
		for (uint32_t i = 0; i < count; ++i) {
			re[i] = left[first + i];
			im[i] = right[first + i];
		}
		p_fft.forward(re.data(), im.data());
		split_packed(re.data(), im.data(), out.re.data() + size_t(p) * kIrBins, out.im.data() + size_t(p) * kIrBins,
				out.re.data() + (size_t(out.partitions) + p) * kIrBins,
				out.im.data() + (size_t(out.partitions) + p) * kIrBins);
	}
	return out;
}

std::optional<uint32_t> ImpulseBank::find(const core::Guid &p_partition) const {
	for (uint32_t i = 0; i < impulses.size(); ++i) {
		if (impulses[i].partition == p_partition) {
			return i;
		}
	}
	return std::nullopt;
}

core::Result<ImpulseBank> load_impulse_bank(const core::EbxDb &p_db, const core::LevelMount &p_mount,
		float p_max_seconds, uint32_t p_rate, ImpulseBankLoad *r_load) {
	ImpulseBankLoad load;
	ImpulseBank bank;
	bank.max_partitions = max_reverb_partitions(p_max_seconds, p_rate);
	if (bank.max_partitions == 0) {
		return core::make_error("load_impulse_bank: MaxReverbLength " + std::to_string(p_max_seconds) + " s");
	}
	const Fft fft(kIrFftSize);
	for (size_t i = 0; i < p_db.size(); ++i) {
		const core::EbxInstance primary = p_db.partition_at(i).primary();
		if (!primary.is_a("ImpulseResponseAsset")) {
			continue;
		}
		++load.assets;
		core::Result<core::audio::ImpulseResponse> ir = core::audio::read_impulse_response(primary, p_mount);
		if (ir && ir->rate != p_rate) {
			ir = core::make_error(ir->name + " is at " + std::to_string(ir->rate) + " Hz");
		}
		core::Result<PreparedImpulse> prepared =
				ir ? prepare_impulse(*ir, bank.max_partitions, fft) : core::Result<PreparedImpulse>(ir.error());
		if (!prepared) {
			++load.failed;
			load.notes += p_db.name_at(i) + ": " + prepared.error().message + "\n";
			continue;
		}
		bank.impulses.push_back(std::move(*prepared));
	}
	if (r_load != nullptr) {
		*r_load = std::move(load);
	}
	return bank;
}

std::array<IrSlot, kIrSlots> IrReverbController::choose(std::span<const IrZone> p_zones) {
	std::array<IrSlot, kIrSlots> best{};
	for (uint32_t b = 0; b < kIrSlots; ++b) {
		for (const IrZone &z : p_zones) {
			if (z.impulse == kNoImpulse || !std::isfinite(z.amplitude) || !(z.amplitude > 0.0f)) {
				continue;
			}
			const bool taken = std::any_of(
					best.begin(), best.begin() + b, [&](const IrSlot &p_s) { return p_s.impulse == z.impulse; });
			const bool louder = best[b].impulse == kNoImpulse || z.amplitude > best[b].amplitude ||
					(z.amplitude == best[b].amplitude && z.impulse < best[b].impulse);
			if (!taken && louder) {
				best[b] = IrSlot{ z.impulse, z.amplitude };
			}
		}
	}
	std::array<IrSlot, kIrSlots> next{};
	std::array<bool, kIrSlots> placed{};
	for (uint32_t s = 0; s < kIrSlots; ++s) {
		for (uint32_t b = 0; b < kIrSlots; ++b) {
			if (!placed[b] && best[b].impulse != kNoImpulse && best[b].impulse == slots_[s].impulse) {
				next[s] = best[b];
				placed[b] = true;
			}
		}
	}
	for (uint32_t b = 0; b < kIrSlots; ++b) {
		for (uint32_t s = 0; s < kIrSlots && !placed[b] && best[b].impulse != kNoImpulse; ++s) {
			if (next[s].impulse == kNoImpulse) {
				next[s] = best[b];
				placed[b] = true;
			}
		}
	}
	slots_ = next;
	return slots_;
}

IrReverb::IrReverb() : fft_(kIrFftSize) {}

void IrReverb::configure(std::shared_ptr<const ImpulseBank> p_bank) {
	bank_ = std::move(p_bank);
	partitions_ = bank_ != nullptr ? bank_->max_partitions : 0;
	fdl_re_.assign(size_t(kIrChannels) * partitions_ * kIrBins, 0.0f);
	fdl_im_.assign(fdl_re_.size(), 0.0f);
	reset();
}

void IrReverb::reset() {
	std::fill(fdl_re_.begin(), fdl_re_.end(), 0.0f);
	std::fill(fdl_im_.begin(), fdl_im_.end(), 0.0f);
	for (auto &p : previous_) {
		p.fill(0.0f);
	}
	lanes_ = {};
	head_ = 0;
	silent_blocks_ = partitions_;
	previous_silent_ = true;
	stats_ = IrReverbStats{};
}

void IrReverb::retarget(Lane &r_lane, float p_target) {
	if (r_lane.target == p_target && r_lane.ramp_left > 0) {
		return;
	}
	r_lane.target = p_target;
	r_lane.ramp_left = kIrCrossfadeBlocks;
	r_lane.step = (p_target - r_lane.gain) / float(kIrCrossfadeBlocks);
}

bool IrReverb::set_slot(uint32_t p_slot, uint32_t p_impulse, float p_amplitude) {
	if (p_slot >= kIrSlots || !active()) {
		++stats_.refused;
		return false;
	}
	const float amplitude = std::isfinite(p_amplitude) ? std::clamp(p_amplitude, 0.0f, 16.0f) : 0.0f;
	const PreparedImpulse *impulse =
			p_impulse < bank_->impulses.size() && amplitude > 0.0f ? &bank_->impulses[p_impulse] : nullptr;
	Lane *current = nullptr;
	for (Lane &l : lanes_) {
		if (l.slot == int32_t(p_slot)) {
			current = &l;
		}
	}
	if (current != nullptr && current->impulse == impulse) {
		retarget(*current, amplitude);
		return true;
	}
	if (current != nullptr) {
		current->slot = -1;
		retarget(*current, 0.0f);
	}
	if (impulse == nullptr) {
		return true;
	}
	++stats_.switches;
	Lane *lane = nullptr;
	for (Lane &l : lanes_) {
		if (l.slot < 0 && l.impulse == impulse) {
			lane = &l;
		}
	}
	for (Lane &l : lanes_) {
		if (lane == nullptr && !l.live()) {
			lane = &l;
		}
	}
	if (lane == nullptr) {
		for (Lane &l : lanes_) {
			if (l.slot < 0 && (lane == nullptr || l.gain < lane->gain)) {
				lane = &l;
			}
		}
		++stats_.steals;
		*lane = Lane{};
	}
	if (lane->impulse != impulse) {
		lane->impulse = impulse;
		lane->gain = 0.0f;
		lane->ramp_left = 0;
	}
	lane->index = p_impulse;
	lane->slot = int32_t(p_slot);
	retarget(*lane, amplitude);
	return true;
}

IrSlot IrReverb::slot(uint32_t p_slot) const {
	for (const Lane &l : lanes_) {
		if (l.slot == int32_t(p_slot) && l.live()) {
			return IrSlot{ l.index, l.target };
		}
	}
	return IrSlot{};
}

void IrReverb::push_input(const float *p_left, const float *p_right, bool p_silent) {
	head_ = head_ + 1 < partitions_ ? head_ + 1 : 0;
	float *l_re = fdl_re_.data() + size_t(head_) * kIrBins;
	float *l_im = fdl_im_.data() + size_t(head_) * kIrBins;
	float *r_re = fdl_re_.data() + (size_t(partitions_) + head_) * kIrBins;
	float *r_im = fdl_im_.data() + (size_t(partitions_) + head_) * kIrBins;
	if (p_silent) {
		std::fill_n(l_re, kIrBins, 0.0f);
		std::fill_n(l_im, kIrBins, 0.0f);
		std::fill_n(r_re, kIrBins, 0.0f);
		std::fill_n(r_im, kIrBins, 0.0f);
		previous_[0].fill(0.0f);
		previous_[1].fill(0.0f);
		return;
	}
	for (uint32_t i = 0; i < kIrPartitionFrames; ++i) {
		work_re_[i] = previous_[0][i];
		work_im_[i] = previous_[1][i];
		work_re_[kIrPartitionFrames + i] = p_left[i];
		work_im_[kIrPartitionFrames + i] = p_right[i];
	}
	std::memcpy(previous_[0].data(), p_left, sizeof(float) * kIrPartitionFrames);
	std::memcpy(previous_[1].data(), p_right, sizeof(float) * kIrPartitionFrames);
	fft_.forward(work_re_.data(), work_im_.data());
	split_packed(work_re_.data(), work_im_.data(), l_re, l_im, r_re, r_im);
}

void IrReverb::render_lane(const Lane &p_lane, float *r_left, float *r_right) {
	const PreparedImpulse &h = *p_lane.impulse;
	const uint32_t count = std::min(h.partitions, partitions_);
	for (uint32_t c = 0; c < kIrChannels; ++c) {
		float *are = acc_re_[c].data(), *aim = acc_im_[c].data();
		std::fill_n(are, kIrBins, 0.0f);
		std::fill_n(aim, kIrBins, 0.0f);
		for (uint32_t p = 0; p < count; ++p) {
			const uint32_t slot = head_ >= p ? head_ - p : head_ + partitions_ - p;
			const float *xr = fdl_re_.data() + (size_t(c) * partitions_ + slot) * kIrBins;
			const float *xi = fdl_im_.data() + (size_t(c) * partitions_ + slot) * kIrBins;
			const float *hr = h.bins_re(c, p);
			const float *hi = h.bins_im(c, p);
			for (uint32_t k = 0; k < kIrBins; ++k) {
				are[k] += xr[k] * hr[k] - xi[k] * hi[k];
				aim[k] += xr[k] * hi[k] + xi[k] * hr[k];
			}
		}
	}
	join_packed(acc_re_[0].data(), acc_im_[0].data(), acc_re_[1].data(), acc_im_[1].data(), work_re_.data(),
			work_im_.data());
	fft_.inverse(work_re_.data(), work_im_.data());
	const float scale = 1.0f / float(kIrFftSize);
	for (uint32_t i = 0; i < kIrPartitionFrames; ++i) {
		r_left[i] = work_re_[kIrPartitionFrames + i] * scale;
		r_right[i] = work_im_[kIrPartitionFrames + i] * scale;
	}
}

void IrReverb::process(float *r_left, float *r_right, uint32_t p_frames) {
	++stats_.blocks;
	if (!active() || p_frames != kIrPartitionFrames) {
		std::fill_n(r_left, p_frames, 0.0f);
		std::fill_n(r_right, p_frames, 0.0f);
		return;
	}
	bool silent = previous_silent_;
	for (uint32_t i = 0; i < kIrPartitionFrames && silent; ++i) {
		silent = r_left[i] == 0.0f && r_right[i] == 0.0f;
	}
	previous_silent_ = std::all_of(r_left, r_left + kIrPartitionFrames, [](float x) { return x == 0.0f; }) &&
			std::all_of(r_right, r_right + kIrPartitionFrames, [](float x) { return x == 0.0f; });
	silent_blocks_ = silent ? std::min(silent_blocks_ + 1, partitions_) : 0;
	push_input(r_left, r_right, silent);
	std::fill_n(r_left, kIrPartitionFrames, 0.0f);
	std::fill_n(r_right, kIrPartitionFrames, 0.0f);
	bool convolved = false;
	for (Lane &l : lanes_) {
		if (!l.live()) {
			continue;
		}
		const float g0 = l.gain;
		const float g1 = l.ramp_left > 1 ? l.gain + l.step : l.target;
		l.ramp_left = l.ramp_left > 0 ? l.ramp_left - 1 : 0;
		l.gain = g1;
		const bool tail = silent_blocks_ < std::min(l.impulse->partitions, partitions_);
		if (tail && (g0 > 0.0f || g1 > 0.0f)) {
			render_lane(l, wet_[0].data(), wet_[1].data());
			const float d = (g1 - g0) / float(kIrPartitionFrames);
			for (uint32_t i = 0; i < kIrPartitionFrames; ++i) {
				const float g = g0 + d * float(i + 1);
				r_left[i] += wet_[0][i] * g;
				r_right[i] += wet_[1][i] * g;
			}
			++stats_.lane_blocks;
			convolved = true;
		}
		if (l.slot < 0 && l.gain == 0.0f && l.ramp_left == 0) {
			l = Lane{};
		}
	}
	stats_.convolved_blocks += convolved ? 1 : 0;
}

}
