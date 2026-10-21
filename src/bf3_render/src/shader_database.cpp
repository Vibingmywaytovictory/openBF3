#include "bf3/render/shader_database.h"

#include "bf3/core/ebx.h"
#include "bf3/core/util/text.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <set>

namespace bf3::render {

namespace {

constexpr size_t kSolutionBytes = 64;
constexpr size_t kSolutionStateBytes = 32;
constexpr size_t kDeclarationBytes = 80;
constexpr size_t kInputElementBytes = 24;
constexpr size_t kTextureConstantBytes = 0x98;
constexpr size_t kTextureNameBytes = 0x80;
constexpr size_t kValueConstantBytes = 0x3C;
constexpr size_t kValueNameBytes = 0x20;
constexpr size_t kValueDefaultAt = 0x2C;
constexpr size_t kSolutionPixelShaderAt = 32;
constexpr size_t kSolutionConstantsAt = 56;
constexpr size_t kTexturesAt = 0x10;
constexpr size_t kValuesAt = 0x18;
constexpr size_t kTextureCountAt = 0x35;
constexpr size_t kValueCountAt = 0x36;

constexpr size_t kMinShaderBytes = 4 + 4 + 1 + 35 + 4 + 4 + 4;

using bf3::core::iequals_ascii;

uint16_t rd16(const uint8_t *p) {
	return uint16_t(p[0] | (p[1] << 8));
}
uint32_t rd32(const uint8_t *p) {
	uint32_t v;
	std::memcpy(&v, p, 4);
	return v;
}
uint64_t rd64(const uint8_t *p) {
	uint64_t v;
	std::memcpy(&v, p, 8);
	return v;
}
float rdf(const uint8_t *p) {
	float v;
	std::memcpy(&v, p, 4);
	return v;
}

class Cursor {
public:
	explicit Cursor(std::span<const uint8_t> p_data) :
			data_(p_data) {}
	size_t at() const { return at_; }
	size_t left() const { return data_.size() - at_; }

	core::Error fail(const char *p_what) const { return core::make_error(std::string(p_what) + " at " + std::to_string(at_)); }

	core::Result<std::span<const uint8_t>> take(uint64_t p_size, const char *p_what) {
		if (p_size > left()) {
			return fail(p_what);
		}
		std::span<const uint8_t> out = data_.subspan(at_, size_t(p_size));
		at_ += size_t(p_size);
		return out;
	}
	core::Result<uint32_t> u32(const char *p_what) {
		core::Result<std::span<const uint8_t>> b = take(4, p_what);
		if (!b) {
			return b.error();
		}
		return rd32(b->data());
	}
	core::Result<uint8_t> u8(const char *p_what) {
		core::Result<std::span<const uint8_t>> b = take(1, p_what);
		if (!b) {
			return b.error();
		}
		return (*b)[0];
	}
	core::Result<std::string> cstr(const char *p_what) {
		const uint8_t *begin = data_.data() + at_;
		const void *nul = std::memchr(begin, 0, left());
		if (nul == nullptr) {
			return fail(p_what);
		}
		const size_t length = size_t(static_cast<const uint8_t *>(nul) - begin);
		std::string out(reinterpret_cast<const char *>(begin), length);
		at_ += length + 1;
		return out;
	}

	core::Result<uint32_t> count(size_t p_min_bytes, const char *p_what) {
		core::Result<uint32_t> n = u32(p_what);
		if (!n) {
			return n;
		}
		if (p_min_bytes > 0 && uint64_t(*n) * p_min_bytes > left()) {
			return fail(p_what);
		}
		return n;
	}

private:
	std::span<const uint8_t> data_;
	size_t at_ = 0;
};

#define SDB_TRY_ASSIGN(var, expr) \
	auto var##_r = (expr); \
	if (!var##_r) { \
		return var##_r.error(); \
	} \
	auto var = *var##_r

std::string fixed_str(const uint8_t *p, size_t p_size) {
	const void *nul = std::memchr(p, 0, p_size);
	const size_t n = nul == nullptr ? p_size : size_t(static_cast<const uint8_t *>(nul) - p);
	return std::string(reinterpret_cast<const char *>(p), n);
}

struct ConstantBlock {
	std::vector<std::string> textures;
	std::vector<ShaderValue> values;
};

core::Result<ConstantBlock> parse_block(std::span<const uint8_t> p_block, size_t p_at) {
	const auto bad = [&](const char *p_what) { return core::make_error(std::string(p_what) + " in constant block at " + std::to_string(p_at)); };
	if (p_block.size() < kValueCountAt + 1) {
		return bad("short constant block");
	}
	ConstantBlock out;
	const auto entries = [&](size_t p_offset_at, size_t p_count_at, size_t p_stride, size_t p_need, auto &&p_each) -> core::Status {
		const uint64_t offset = rd64(p_block.data() + p_offset_at);
		const size_t n = p_block[p_count_at];
		for (size_t e = 0; e < n; ++e) {
			const uint64_t at = offset + uint64_t(p_stride) * e;
			if (offset > p_block.size() || at > p_block.size() || p_need > p_block.size() - at) {
				return bad("constant entry out of range");
			}
			p_each(p_block.data() + at);
		}
		return core::Status::success();
	};
	BF3_TRY(entries(kTexturesAt, kTextureCountAt, kTextureConstantBytes, 8 + kTextureNameBytes,
			[&](const uint8_t *p) { out.textures.push_back(fixed_str(p + 8, kTextureNameBytes)); }));
	BF3_TRY(entries(kValuesAt, kValueCountAt, kValueConstantBytes, kValueDefaultAt + 16, [&](const uint8_t *p) {
		ShaderValue v;
		v.name = fixed_str(p, kValueNameBytes);
		for (size_t c = 0; c < 4; ++c) {
			v.value[c] = rdf(p + kValueDefaultAt + 4 * c);
		}
		out.values.push_back(std::move(v));
	}));
	return out;
}

core::Status skip_blob(Cursor &r_r, const char *p_what) {
	BF3_TRY(r_r.take(16, p_what));
	SDB_TRY_ASSIGN(size, r_r.u32(p_what));
	BF3_TRY(r_r.take(size, p_what));
	return core::Status::success();
}

core::Result<ShaderDatabase> parse_path(std::span<const uint8_t> p_body, uint32_t p_render_path, uint64_t &r_work, uint64_t p_limit) {
	Cursor r(p_body);
	ShaderDatabase out;
	out.render_path = p_render_path;
	SDB_TRY_ASSIGN(version, r.u32("shader database version"));
	if (version != kShaderDatabaseVersion) {
		return core::make_error("shader database version " + std::to_string(version) + " (expected " + std::to_string(kShaderDatabaseVersion) + ")");
	}
	BF3_TRY(r.u32("shader database header"));
	SDB_TRY_ASSIGN(constant_count, r.count(4, "shader constant count"));
	std::vector<ConstantBlock> constants;
	constants.reserve(constant_count);
	for (uint32_t i = 0; i < constant_count; ++i) {
		const size_t start = r.at();
		SDB_TRY_ASSIGN(size, r.u32("shader constant size"));
		if (size < 4) {
			return r.fail("shader constant size");
		}
		BF3_TRY(r.take(size - 4, "shader constant block"));
		SDB_TRY_ASSIGN(block, parse_block(p_body.subspan(start, size), start));
		constants.push_back(std::move(block));
	}
	SDB_TRY_ASSIGN(n1, r.count(4, "constant list count"));
	for (uint32_t i = 0; i < n1; ++i) {
		SDB_TRY_ASSIGN(head, r.take(4, "constant list"));
		BF3_TRY(r.take(8ull * rd16(head.data()), "constant list"));
	}
	SDB_TRY_ASSIGN(n2, r.count(4, "texture list count"));
	for (uint32_t i = 0; i < n2; ++i) {
		SDB_TRY_ASSIGN(textures, r.u32("texture list"));
		BF3_TRY(r.take(4ull * textures, "texture list"));
	}
	SDB_TRY_ASSIGN(vertex_shaders, r.count(4, "vertex shader count"));
	for (uint32_t i = 0; i < vertex_shaders; ++i) {
		BF3_TRY(skip_blob(r, "vertex shader"));
		BF3_TRY(r.take(12, "vertex shader"));
		SDB_TRY_ASSIGN(signature, r.u32("vertex shader signature"));
		BF3_TRY(r.take(signature, "vertex shader signature"));
		SDB_TRY_ASSIGN(elements, r.u32("vertex shader elements"));
		BF3_TRY(r.take(uint64_t(kInputElementBytes) * elements, "vertex shader elements"));
		SDB_TRY_ASSIGN(names, r.u32("vertex shader element names"));
		if (names != elements) {
			return r.fail("vertex shader element names");
		}
		for (uint32_t n = 0; n < names; ++n) {
			BF3_TRY(r.cstr("vertex shader element name"));
		}
		BF3_TRY(r.u32("vertex shader"));
	}
	SDB_TRY_ASSIGN(pixel_shaders, r.count(36, "pixel shader count"));
	for (uint32_t i = 0; i < pixel_shaders; ++i) {
		BF3_TRY(r.take(16, "pixel shader"));
		SDB_TRY_ASSIGN(size, r.u32("pixel shader size"));
		BF3_TRY(r.take(size, "pixel shader"));
		BF3_TRY(r.take(16, "pixel shader"));
	}
	SDB_TRY_ASSIGN(n3, r.count(24, "shader blob count"));
	for (uint32_t i = 0; i < n3; ++i) {
		BF3_TRY(skip_blob(r, "shader blob"));
		BF3_TRY(r.u32("shader blob"));
	}
	SDB_TRY_ASSIGN(solution_count, r.count(kSolutionBytes, "shader solution count"));
	std::vector<ShaderSolution> solutions;
	std::vector<std::optional<size_t>> pixel_constants;
	solutions.reserve(solution_count);
	pixel_constants.reserve(solution_count);
	for (uint32_t i = 0; i < solution_count; ++i) {
		SDB_TRY_ASSIGN(b, r.take(kSolutionBytes, "shader solution"));
		ShaderSolution s;
		s.state_hash = rd64(b.data());
		s.flags = b[8];
		s.surface_type = surface_shader_type(b[9]);
		s.blend_mode = b[10];
		solutions.push_back(s);
		const int64_t index = int64_t(rd64(b.data() + kSolutionConstantsAt));
		pixel_constants.push_back(index >= 0 && uint64_t(index) < constants.size() ? std::optional<size_t>(size_t(index)) : std::nullopt);
		(void)kSolutionPixelShaderAt;
	}
	SDB_TRY_ASSIGN(states, r.u32("shader solution state count"));
	if (states != solution_count) {
		return r.fail("shader solution state count");
	}
	BF3_TRY(r.take(uint64_t(kSolutionStateBytes) * states, "shader solution states"));
	SDB_TRY_ASSIGN(declarations, r.u32("declaration count"));
	BF3_TRY(r.take(uint64_t(kDeclarationBytes) * declarations, "declarations"));

	SDB_TRY_ASSIGN(shader_count, r.count(kMinShaderBytes, "shader count"));
	out.shaders.reserve(shader_count);
	for (uint32_t i = 0; i < shader_count; ++i) {
		SurfaceShader s;
		SDB_TRY_ASSIGN(hash, r.u32("shader"));
		SDB_TRY_ASSIGN(type, r.u32("shader"));
		SDB_TRY_ASSIGN(flags, r.u8("shader"));
		s.name_hash = hash;
		s.surface_type = surface_shader_type(type);
		s.flags = flags;
		BF3_TRY(r.take(3 + 4 * 8, "shader"));
		SDB_TRY_ASSIGN(texture_count, r.count(13, "shader texture count"));
		s.textures.reserve(texture_count);
		for (uint32_t t = 0; t < texture_count; ++t) {
			ShaderTexture tex;
			SDB_TRY_ASSIGN(name, r.cstr("shader texture name"));
			SDB_TRY_ASSIGN(b, r.take(12, "shader texture"));
			tex.name = std::move(name);
			tex.coord_type = rd32(b.data());
			tex.vertex_usage = rd32(b.data() + 4);
			tex.factor = rdf(b.data() + 8);
			s.textures.push_back(std::move(tex));
		}
		SDB_TRY_ASSIGN(parameter_count, r.count(17, "shader texture parameter count"));
		s.texture_parameters.reserve(parameter_count);
		for (uint32_t t = 0; t < parameter_count; ++t) {
			ShaderTextureParameter p;
			SDB_TRY_ASSIGN(name, r.cstr("shader texture parameter name"));
			SDB_TRY_ASSIGN(b, r.take(16, "shader texture parameter"));
			p.name = std::move(name);
			p.id = rd32(b.data());
			p.coord_type = rd32(b.data() + 4);
			p.vertex_usage = rd32(b.data() + 8);
			p.factor = rdf(b.data() + 12);
			s.texture_parameters.push_back(std::move(p));
		}
		SDB_TRY_ASSIGN(used, r.count(2, "shader solution list"));
		std::set<std::string> seen_textures, seen_values;
		s.solutions.reserve(used);
		for (uint32_t u = 0; u < used; ++u) {
			SDB_TRY_ASSIGN(b, r.take(2, "shader solution index"));
			const size_t index = rd16(b.data());
			if (index >= solutions.size()) {
				return r.fail("shader solution index");
			}
			s.solutions.push_back(solutions[index]);
			r_work += 1;
			if (!pixel_constants[index]) {
				continue;
			}
			const ConstantBlock &block = constants[*pixel_constants[index]];
			r_work += block.textures.size() + block.values.size();
			if (r_work > p_limit) {
				return core::make_error("shader database work budget of " + std::to_string(p_limit) + " units exhausted at " + std::to_string(r.at()));
			}
			for (const std::string &name : block.textures) {
				if (seen_textures.insert(name).second) {
					s.sampled_textures.push_back(name);
				}
			}
			for (const ShaderValue &v : block.values) {
				if (seen_values.insert(v.name).second) {
					s.values.push_back(v);
				}
			}
		}
		out.shaders.push_back(std::move(s));
	}
	return out;
}

}

SurfaceShaderType surface_shader_type(uint32_t p_value) {
	return p_value <= 4 ? SurfaceShaderType(p_value) : SurfaceShaderType::Other;
}

bool SurfaceShader::double_sided() const {
	return std::any_of(solutions.begin(), solutions.end(), [](const ShaderSolution &s) { return (s.flags & 1u) != 0; });
}

const ShaderTextureParameter *SurfaceShader::parameter(std::string_view p_name) const {
	for (const ShaderTextureParameter &p : texture_parameters) {
		if (iequals_ascii(p.name, p_name)) {
			return &p;
		}
	}
	return nullptr;
}

const ShaderValue *SurfaceShader::value(std::string_view p_name) const {
	for (const ShaderValue &v : values) {
		if (iequals_ascii(v.name, p_name)) {
			return &v;
		}
	}
	return nullptr;
}

core::Result<std::vector<ShaderDatabase>> parse_shader_database(std::span<const uint8_t> p_res, uint64_t *r_work) {
	Cursor r(p_res);
	const uint64_t limit = kShaderWorkBase + kShaderWorkPerByte * uint64_t(p_res.size());
	uint64_t work = 0;
	std::vector<ShaderDatabase> out;
	SDB_TRY_ASSIGN(paths, r.count(8, "shader database render path count"));
	for (uint32_t i = 0; i < paths; ++i) {
		SDB_TRY_ASSIGN(render_path, r.u32("render path"));
		SDB_TRY_ASSIGN(size, r.u32("render path size"));
		SDB_TRY_ASSIGN(body, r.take(size, "render path body"));
		core::Result<ShaderDatabase> db = parse_path(body, render_path, work, limit);
		if (!db) {
			if (r_work != nullptr) {
				*r_work = work;
			}
			return core::make_error("shader database render path " + std::to_string(i) + ": " + db.error().message);
		}
		out.push_back(std::move(*db));
	}
	if (r_work != nullptr) {
		*r_work = work;
	}
	return out;
}

ShaderDatabase merge_first_wins(std::vector<std::vector<SurfaceShader>> p_lists) {
	ShaderDatabase out;
	std::set<uint32_t> seen;
	for (std::vector<SurfaceShader> &list : p_lists) {
		for (SurfaceShader &s : list) {
			if (seen.insert(s.name_hash).second) {
				out.shaders.push_back(std::move(s));
			}
		}
	}
	return out;
}

ShaderLookup::ShaderLookup(ShaderDatabase p_database) :
		database_(std::move(p_database)) {
	by_hash_.reserve(database_.shaders.size());
	for (size_t i = 0; i < database_.shaders.size(); ++i) {
		by_hash_.emplace_back(database_.shaders[i].name_hash, i);
	}
	std::sort(by_hash_.begin(), by_hash_.end());
}

const SurfaceShader *ShaderLookup::by_name(std::string_view p_partition) const {
	const std::string lower = core::to_lower_ascii(p_partition);
	const uint32_t hash = core::ebx_name_hash(lower);
	const auto found = std::lower_bound(by_hash_.begin(), by_hash_.end(), std::pair<uint32_t, size_t>(hash, 0));
	return found == by_hash_.end() || found->first != hash ? nullptr : &database_.shaders[found->second];
}

}
