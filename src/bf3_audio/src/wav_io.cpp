#include "bf3/audio/wav_io.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace bf3::audio {

namespace {

void put_u16(std::vector<uint8_t> &r, uint16_t v) {
	r.push_back(uint8_t(v));
	r.push_back(uint8_t(v >> 8));
}
void put_u32(std::vector<uint8_t> &r, uint32_t v) {
	for (int i = 0; i < 4; ++i) {
		r.push_back(uint8_t(v >> (8 * i)));
	}
}
uint16_t get_u16(const uint8_t *p) {
	return uint16_t(p[0] | (p[1] << 8));
}
uint32_t get_u32(const uint8_t *p) {
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

}

core::Status write_wav(const core::PathGuard &p_guard, const std::filesystem::path &p_path, std::span<const float> p_interleaved, uint32_t p_channels,
		uint32_t p_rate, WavSampleFormat p_format) {
	if (p_channels == 0 || p_channels > 8 || p_rate == 0 || p_interleaved.size() % p_channels != 0) {
		return core::make_error("write_wav: bad format");
	}
	const core::PathGuard::Verdict verdict = p_guard.check_write_target(p_path);
	if (verdict != core::PathGuard::Verdict::Allowed) {
		return core::make_error("write_wav: " + p_path.string() + " refused: " + core::PathGuard::verdict_name(verdict));
	}
	const uint32_t bytes_per_sample = p_format == WavSampleFormat::Float32 ? 4 : 2;
	const uint64_t data_bytes = uint64_t(p_interleaved.size()) * bytes_per_sample;
	if (data_bytes > 0xffffff00ull - 64) {
		return core::make_error("write_wav: too long for RIFF");
	}
	std::vector<uint8_t> out;
	out.reserve(size_t(44 + data_bytes));
	out.insert(out.end(), { 'R', 'I', 'F', 'F' });
	put_u32(out, uint32_t(36 + data_bytes));
	out.insert(out.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
	put_u32(out, 16);
	put_u16(out, p_format == WavSampleFormat::Float32 ? 3 : 1);
	put_u16(out, uint16_t(p_channels));
	put_u32(out, p_rate);
	put_u32(out, p_rate * p_channels * bytes_per_sample);
	put_u16(out, uint16_t(p_channels * bytes_per_sample));
	put_u16(out, uint16_t(bytes_per_sample * 8));
	out.insert(out.end(), { 'd', 'a', 't', 'a' });
	put_u32(out, uint32_t(data_bytes));
	if (p_format == WavSampleFormat::Float32) {
		const size_t at = out.size();
		out.resize(at + size_t(data_bytes));
		std::memcpy(out.data() + at, p_interleaved.data(), size_t(data_bytes));
	} else {
		for (float f : p_interleaved) {
			const float c = std::isfinite(f) ? std::clamp(f, -1.0f, 1.0f) : 0.0f;
			put_u16(out, uint16_t(int16_t(std::lrint(c * 32767.0f))));
		}
	}
	std::error_code ec;
	if (p_path.has_parent_path()) {
		std::filesystem::create_directories(p_path.parent_path(), ec);
	}
	std::ofstream file(p_path, std::ios::binary | std::ios::trunc);
	if (!file) {
		return core::make_error("write_wav: cannot open " + p_path.string());
	}
	file.write(reinterpret_cast<const char *>(out.data()), std::streamsize(out.size()));
	if (!file) {
		return core::make_error("write_wav: write failed " + p_path.string());
	}
	return core::Status::success();
}

core::Result<WavData> read_wav(const std::filesystem::path &p_path, uint64_t p_max_bytes) {
	std::error_code ec;
	const uint64_t size = std::filesystem::file_size(p_path, ec);
	if (ec) {
		return core::make_error("read_wav: " + p_path.string() + ": " + ec.message());
	}
	if (size > p_max_bytes || size < 44) {
		return core::make_error("read_wav: " + p_path.string() + ": size " + std::to_string(size));
	}
	std::vector<uint8_t> bytes(static_cast<size_t>(size), uint8_t(0));
	std::ifstream file(p_path, std::ios::binary);
	if (!file.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(size))) {
		return core::make_error("read_wav: read failed");
	}
	if (std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
		return core::make_error("read_wav: not RIFF/WAVE");
	}
	WavData out;
	uint16_t format = 0, bits = 0;
	bool have_fmt = false;
	size_t at = 12;
	while (at + 8 <= bytes.size()) {
		const uint32_t len = get_u32(bytes.data() + at + 4);
		const size_t body = at + 8;
		if (len > bytes.size() - body) {
			return core::make_error("read_wav: chunk past the end");
		}
		if (std::memcmp(bytes.data() + at, "fmt ", 4) == 0 && len >= 16) {
			format = get_u16(bytes.data() + body);
			out.channels = get_u16(bytes.data() + body + 2);
			out.rate = get_u32(bytes.data() + body + 4);
			bits = get_u16(bytes.data() + body + 14);
			have_fmt = true;
		} else if (std::memcmp(bytes.data() + at, "data", 4) == 0) {
			if (!have_fmt || out.channels == 0 || out.channels > 8) {
				return core::make_error("read_wav: data before a valid fmt");
			}
			if (format == 3 && bits == 32) {
				out.samples.resize(len / 4);
				std::memcpy(out.samples.data(), bytes.data() + body, out.samples.size() * 4);
			} else if (format == 1 && bits == 16) {
				out.samples.resize(len / 2);
				for (size_t i = 0; i < out.samples.size(); ++i) {
					out.samples[i] = float(int16_t(get_u16(bytes.data() + body + i * 2))) / 32768.0f;
				}
			} else {
				return core::make_error("read_wav: unsupported format");
			}
			out.samples.resize(out.samples.size() - out.samples.size() % out.channels);
			return out;
		}
		at = body + len + (len & 1);
	}
	return core::make_error("read_wav: no data chunk");
}

WavRecorder::WavRecorder(uint32_t p_rate, double p_max_seconds) :
		rate_(p_rate) {
	const double frames = std::clamp(p_max_seconds, 0.0, 3600.0) * double(p_rate);
	buffer_.assign(size_t(frames) * 2, 0.0f);
}

void WavRecorder::on_block(const float *p_interleaved_stereo, uint32_t p_frames, uint64_t) {
	const uint64_t at = written_.load(std::memory_order_relaxed);
	const uint64_t capacity = buffer_.size() / 2;
	const uint64_t take = at >= capacity ? 0 : std::min<uint64_t>(p_frames, capacity - at);
	if (take > 0) {
		std::memcpy(buffer_.data() + at * 2, p_interleaved_stereo, size_t(take) * 2 * sizeof(float));
	}
	written_.store(at + take, std::memory_order_release);
	if (take < p_frames) {
		dropped_.fetch_add(p_frames - take, std::memory_order_relaxed);
	}
}

core::Status WavRecorder::save(const core::PathGuard &p_guard, const std::filesystem::path &p_path) const {
	return write_wav(p_guard, p_path, samples(), 2, rate_);
}

std::span<const float> WavRecorder::samples() const {
	return std::span<const float>(buffer_.data(), size_t(written_.load(std::memory_order_acquire)) * 2);
}

uint64_t WavRecorder::dropped_frames() const {
	return dropped_.load(std::memory_order_relaxed);
}

}
