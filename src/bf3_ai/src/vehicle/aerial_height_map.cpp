#include "bf3/ai/vehicle/aerial_height_map.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bf3::ai {

namespace {

constexpr size_t kHeaderBytes = sizeof(float) + 2 * sizeof(int32_t) + 2 * sizeof(float);

template <class T> void put(std::vector<uint8_t> &r_out, const T &p_value) {
	const size_t at = r_out.size();
	r_out.resize(at + sizeof(T));
	std::memcpy(r_out.data() + at, &p_value, sizeof(T));
}

template <class T> T take(std::span<const uint8_t> p_bytes, size_t &r_at) {
	T value{};
	std::memcpy(&value, p_bytes.data() + r_at, sizeof(T));
	r_at += sizeof(T);
	return value;
}

}

float AerialHeightMap::altitude(const Vec3 &p_position) const {
	return p_position.y - ground_height(p_position);
}

float AerialHeightMap::ground_height(const Vec3 &p_position) const {
	return ground_height(p_position.x, p_position.z);
}

float AerialHeightMap::ground_height(float p_x, float p_z) const {
	if (heights_.empty()) {
		return kMissingGroundHeight;
	}
	const int column = int(std::floor((p_x - origin_.x) / cell_size_));
	const int row = int(std::floor((p_z - origin_.y) / cell_size_));
	if (column < 0 || row < 0 || column >= width_ || row >= height_) {
		return kMissingGroundHeight;
	}
	return float(heights_[size_t(column) + size_t(width_) * size_t(row)]);
}

void AerialHeightMap::create_height_map(
		const AerialBounds &p_bounds, const AerialHeightSource &p_source, int p_samples_per_axis) {
	width_ = std::max(0, int(p_bounds.size_x / cell_size_));
	height_ = std::max(0, int(p_bounds.size_z / cell_size_));
	origin_ = { p_bounds.min_x, p_bounds.min_z };
	heights_.assign(size_t(width_) * size_t(height_), 0);
	for (int z = 0; z < height_; ++z) {
		for (int x = 0; x < width_; ++x) {
			heights_[size_t(x) + size_t(width_) * size_t(z)] = compute_height_at(p_source, x, z, p_samples_per_axis);
		}
	}
}

uint16_t AerialHeightMap::compute_height_at(
		const AerialHeightSource &p_source, int p_cell_x, int p_cell_z, int p_samples_per_axis) const {
	const int samples = std::max(p_samples_per_axis, 1);
	const float x0 = origin_.x + float(p_cell_x) * cell_size_;
	const float z0 = origin_.y + float(p_cell_z) * cell_size_;
	const float step = samples > 1 ? cell_size_ / float(samples - 1) : 0.0f;
	const float start = samples > 1 ? 0.0f : 0.5f * cell_size_;
	float highest = 0.0f;
	for (int i = 0; i < samples; ++i) {
		for (int j = 0; j < samples; ++j) {
			highest = std::max(highest, p_source.height(x0 + start + step * float(i), z0 + start + step * float(j)));
		}
	}
	return uint16_t(std::clamp(std::ceil(highest), 0.0f, 65535.0f));
}

std::vector<uint8_t> AerialHeightMap::serialize() const {
	std::vector<uint8_t> out;
	out.reserve(kHeaderBytes + heights_.size() * sizeof(uint16_t));
	put(out, cell_size_);
	put(out, int32_t(width_));
	put(out, int32_t(height_));
	put(out, origin_.x);
	put(out, origin_.y);
	for (const uint16_t value : heights_) {
		put(out, value);
	}
	return out;
}

core::Status AerialHeightMap::deserialize(std::span<const uint8_t> p_bytes) {
	if (p_bytes.size() < kHeaderBytes) {
		return core::make_error("aerial height map: truncated header");
	}
	size_t at = 0;
	const float cell_size = take<float>(p_bytes, at);
	const int32_t width = take<int32_t>(p_bytes, at);
	const int32_t height = take<int32_t>(p_bytes, at);
	const float origin_x = take<float>(p_bytes, at);
	const float origin_z = take<float>(p_bytes, at);
	if (!(cell_size > 0.0f) || width < 0 || height < 0 ||
			p_bytes.size() - kHeaderBytes != size_t(width) * size_t(height) * sizeof(uint16_t)) {
		return core::make_error("aerial height map: header does not match the data");
	}
	cell_size_ = cell_size;
	width_ = width;
	height_ = height;
	origin_ = { origin_x, origin_z };
	heights_.resize(size_t(width) * size_t(height));
	for (uint16_t &value : heights_) {
		value = take<uint16_t>(p_bytes, at);
	}
	return core::Status::success();
}

}
