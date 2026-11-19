#include "bf3/render/program_database.h"

#include "bf3/core/res/res_loader.h"
#include "bf3/core/superbundle_mount.h"

#include <cstring>

namespace bf3::render {

namespace {

constexpr uint8_t kMagic[4] = { 'D', 'X', 'B', 'C' };
constexpr size_t kSizeAt = 24;
constexpr size_t kHeaderBytes = 32;

uint32_t rd32(const uint8_t *p_at) {
	uint32_t v = 0;
	std::memcpy(&v, p_at, 4);
	return v;
}

bool printable(uint8_t p_byte) {
	return p_byte >= 0x20 && p_byte < 0x7F;
}

}

core::Result<std::vector<DatabaseProgram>> parse_program_database(std::span<const uint8_t> p_payload) {
	std::vector<DatabaseProgram> out;
	const size_t n = p_payload.size();
	size_t at = 5;
	while (at + kHeaderBytes <= n) {
		if (std::memcmp(p_payload.data() + at, kMagic, 4) != 0) {
			++at;
			continue;
		}
		const uint32_t size = rd32(p_payload.data() + at - 4);
		if (rd32(p_payload.data() + at + kSizeAt) != size || size < kHeaderBytes || size > n - at ||
				p_payload[at - 5] != 0) {
			return core::make_error("program at byte " + std::to_string(at) +
					": size prefix, header size and name terminator disagree");
		}
		const size_t end = at - 5;
		size_t begin = end;
		while (begin > 0 && end - begin < kProgramNameMax && printable(p_payload[begin - 1])) {
			--begin;
		}
		const std::string name(reinterpret_cast<const char *>(p_payload.data() + begin), end - begin);
		if (name.find('/') == std::string::npos) {
			return core::make_error("program at byte " + std::to_string(at) + ": no platform/name");
		}
		out.push_back({ name, p_payload.subspan(at, size) });
		at += size;
	}
	if (out.empty()) {
		return core::make_error("no programs");
	}
	return out;
}

core::Result<dxbc::Program> ProgramDatabase::program(std::string_view p_name, uint16_t p_kind) const {
	for (const DatabaseProgram &p : programs) {
		if (p.name != p_name) {
			continue;
		}
		core::Result<dxbc::Program> parsed = dxbc::parse(p.bytes);
		if (parsed && parsed->kind == p_kind) {
			return parsed;
		}
	}
	return core::make_error("no program " + std::string(p_name) + " of kind " + std::to_string(p_kind));
}

core::Result<ProgramDatabase> read_program_database(const core::Install &p_install) {
	core::Result<core::LevelMount> mount = p_install.mount_superbundle(kProgramDatabaseSuperbundle);
	if (!mount) {
		return mount.error();
	}
	if (const core::Status st = mount->load_bundles({ std::string(kProgramDatabaseBundle) }); !st) {
		return st.error();
	}
	core::Result<core::res::RawRes> raw = core::res::read_raw_res(*mount, kProgramDatabaseRes);
	if (!raw) {
		return raw.error();
	}
	ProgramDatabase db;
	db.payload = std::move(raw->payload);
	core::Result<std::vector<DatabaseProgram>> programs = parse_program_database(db.payload);
	if (!programs) {
		return programs.error();
	}
	db.programs = std::move(*programs);
	return db;
}

}
