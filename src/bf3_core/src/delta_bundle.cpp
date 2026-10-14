// Ported from IW4L superbundle_bf3 (delta.rs), Apache-2.0.
#include "bf3/core/delta_bundle.h"

#include "format_errors.h"

#include <algorithm>

namespace bf3::core {

namespace {

constexpr size_t kDeltaRunLen = 16;

uint32_t be32(const uint8_t *p) {
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint64_t be64(const uint8_t *p) {
	return (uint64_t(be32(p)) << 32) | be32(p + 4);
}

} // namespace

Result<uint64_t> delta_table_len(std::span<const uint8_t> p_header) {
	BF3_TRY(detail::need("delta bundle header", p_header.size(), kDeltaHeaderLen));
	const uint32_t magic = be32(p_header.data() + 4);
	if (magic != kDeltaMagic) {
		return detail::bad_magic("delta bundle", magic);
	}
	const uint32_t runs_len = be32(p_header.data());
	if (runs_len % kDeltaRunLen != 0) {
		return detail::layout_error("delta bundle", "run table is not a whole number of runs");
	}
	return uint64_t(kDeltaHeaderLen) + runs_len;
}

Result<DeltaBundle> DeltaBundle::parse(std::span<const uint8_t> p_table, uint64_t p_patch_offset, uint64_t p_patch_len) {
	Result<uint64_t> table_len_r = delta_table_len(p_table);
	if (!table_len_r) {
		return table_len_r.error();
	}
	const uint64_t table_len = *table_len_r;
	BF3_TRY(detail::need("delta run table", p_table.size(), static_cast<size_t>(table_len)));
	DeltaBundle out;
	out.runs_.reserve(static_cast<size_t>((table_len - kDeltaHeaderLen) / kDeltaRunLen));
	uint64_t patch_at = table_len;
	uint64_t start = 0;
	for (size_t at = kDeltaHeaderLen; at < table_len; at += kDeltaRunLen) {
		const uint8_t *run = p_table.data() + at;
		const uint64_t len = be32(run);
		const uint32_t file = be32(run + 4);
		const uint64_t offset = be64(run + 8);
		DeltaRun r;
		r.start = start;
		r.len = len;
		if (file == 0) {
			r.from_patch = true;
			r.offset = p_patch_offset + patch_at;
			patch_at += len;
		} else {
			if (offset + len < offset) {
				return detail::layout_error("delta bundle", "base run overflows");
			}
			r.file = file;
			r.offset = offset;
		}
		if (len != 0) {
			out.runs_.push_back(r);
		}
		start += len;
	}
	if (patch_at > p_patch_len) {
		return detail::truncated("delta bundle patch data", patch_at, p_patch_len);
	}
	out.len_ = start;
	return out;
}

Status DeltaBundle::read_at(const ByteSource &p_patch, const ByteSource &p_base, uint64_t p_offset, std::span<uint8_t> p_dst) const {
	const uint64_t end = p_offset + p_dst.size();
	if (end < p_offset || end > len_) {
		const uint64_t need = end < p_offset ? UINT64_MAX : end;
		return detail::truncated("delta bundle read", need, len_);
	}
	size_t first = static_cast<size_t>(
			std::partition_point(runs_.begin(), runs_.end(), [p_offset](const DeltaRun &r) { return r.start + r.len <= p_offset; }) -
			runs_.begin());
	uint64_t at = p_offset;
	size_t filled = 0;
	while (at < end) {
		if (first >= runs_.size()) {
			return detail::layout_error("delta bundle", "runs do not cover the read");
		}
		const DeltaRun &run = runs_[first];
		const uint64_t within = at - run.start;
		const size_t take = static_cast<size_t>(std::min(run.len - within, end - at));
		const std::span<uint8_t> out = p_dst.subspan(filled, take);
		BF3_TRY((run.from_patch ? p_patch : p_base).read_exact_at(run.offset + within, out));
		filled += take;
		at += take;
		++first;
	}
	return {};
}

} // namespace bf3::core
