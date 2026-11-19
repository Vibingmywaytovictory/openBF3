#pragma once

#include "bf3/core/install.h"
#include "bf3/core/result.h"
#include "bf3/render/dxbc.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bf3::render {

inline constexpr std::string_view kProgramDatabaseSuperbundle = "Win32/Globals";
inline constexpr std::string_view kProgramDatabaseBundle = "systems/frostbitestartupdata";
inline constexpr std::string_view kProgramDatabaseRes = "systems/shaderprogramdb";
inline constexpr size_t kProgramNameMax = 512;

struct DatabaseProgram {
	std::string name;
	std::span<const uint8_t> bytes;
};

struct ProgramDatabase {
	std::vector<uint8_t> payload;
	std::vector<DatabaseProgram> programs;

	core::Result<dxbc::Program> program(std::string_view p_name, uint16_t p_kind) const;
};

core::Result<std::vector<DatabaseProgram>> parse_program_database(std::span<const uint8_t> p_payload);

core::Result<ProgramDatabase> read_program_database(const core::Install &p_install);

}
