#pragma once
// A shared, read-only install file with bounds-checked positional reads (several threads may
// read one file at once). Ported from IW4L asset_transport::bf3 (file.rs DiskFile). Created only
// from InstallReader::open, so it never names a path outside the install.

#include "bf3/core/delta_bundle.h"
#include "bf3/core/read_only_file.h"
#include "bf3/core/result.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bf3::core {

class InstallFile final : public ByteSource {
public:
	InstallFile(std::string p_name, ReadOnlyFile p_file) :
			name_(std::move(p_name)), file_(std::move(p_file)) {}

	const std::string &name() const { return name_; } // install-relative
	uint64_t size() const { return file_.size(); }

	Status read_into(uint64_t p_offset, std::span<uint8_t> p_dst) const;
	Result<std::vector<uint8_t>> read_vec(uint64_t p_offset, uint64_t p_len) const;
	Status read_exact_at(uint64_t p_offset, std::span<uint8_t> p_dst) const override { return read_into(p_offset, p_dst); }

private:
	std::string name_;
	ReadOnlyFile file_;
};

} // namespace bf3::core
