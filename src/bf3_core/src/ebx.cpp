// EBX partition parser, validator and value decoder.
// Ported from IW4L ebx_bf3 (partition.rs, value.rs, error.rs, lib.rs), Apache-2.0.
#include "bf3/core/ebx.h"

#include "format_errors.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>

namespace bf3::core {

using namespace ebx_code;

namespace {

constexpr uint32_t kMagic = 0x0FB2D1CE;
constexpr uint32_t kMagicBigEndian = 0xCED1B20F;
constexpr size_t kHeaderLen = 80;
constexpr uint32_t kNullString = 0xFFFFFFFFu;
constexpr uint32_t kExternal = 0x80000000u;
constexpr uint16_t kNoType = 0xFFFF;
constexpr size_t kNestDepth = 64;

// Section names as the Rust `Section` Debug output.
enum class Section { Header, Imports, Names, Fields, Complexes, InstanceRepeaters, ArrayRepeaters, Strings, Instances, Arrays };

const char *section_name(Section p_section) {
	switch (p_section) {
		case Section::Header:
			return "Header";
		case Section::Imports:
			return "Imports";
		case Section::Names:
			return "Names";
		case Section::Fields:
			return "Fields";
		case Section::Complexes:
			return "Complexes";
		case Section::InstanceRepeaters:
			return "InstanceRepeaters";
		case Section::ArrayRepeaters:
			return "ArrayRepeaters";
		case Section::Strings:
			return "Strings";
		case Section::Instances:
			return "Instances";
		case Section::Arrays:
			return "Arrays";
	}
	return "?";
}

// --- errors (error.rs Display wording) ---
Error truncated(Section p_section) {
	return make_error(std::string(section_name(p_section)) + " runs past the end of the partition");
}
Error not_utf8(Section p_section) {
	return make_error(std::string(section_name(p_section)) + " is not UTF-8");
}
Error unknown_name(uint32_t p_hash) {
	return make_error("name hash " + detail::hex_prefixed(p_hash, 8) + " is not in the name table");
}
Error bad_complex(uint32_t p_index) {
	return make_error("type descriptor index " + std::to_string(p_index) + " is out of range");
}
Error unexpected_kind(std::string_view p_complex, uint8_t p_code) {
	return make_error("type `" + std::string(p_complex) + "` has type code " + std::to_string(p_code) + " where it is used");
}
Error bad_reference(uint32_t p_value) {
	return make_error("reference " + detail::hex_prefixed(p_value, 8) + " names no instance or import");
}
Error bad_string(uint32_t p_offset) {
	char text[32];
	std::snprintf(text, sizeof(text), "0x%x", p_offset);
	return make_error(std::string("string offset ") + text + " is outside the string table");
}
Error bad_array(uint32_t p_index) {
	return make_error("array " + std::to_string(p_index) + " is out of range");
}
Error too_deep() {
	return make_error("types or arrays nest deeper than the reader allows");
}
Error overlapping() {
	return make_error("fields or arrays overlap so much that the partition cannot be checked");
}

bool get16(std::span<const uint8_t> p_bytes, size_t p_at, uint16_t &r_out) {
	if (p_at > p_bytes.size() || p_bytes.size() - p_at < 2) {
		return false;
	}
	std::memcpy(&r_out, p_bytes.data() + p_at, 2);
	return true;
}

bool get32(std::span<const uint8_t> p_bytes, size_t p_at, uint32_t &r_out) {
	if (p_at > p_bytes.size() || p_bytes.size() - p_at < 4) {
		return false;
	}
	std::memcpy(&r_out, p_bytes.data() + p_at, 4);
	return true;
}

bool get64(std::span<const uint8_t> p_bytes, size_t p_at, uint64_t &r_out) {
	if (p_at > p_bytes.size() || p_bytes.size() - p_at < 8) {
		return false;
	}
	std::memcpy(&r_out, p_bytes.data() + p_at, 8);
	return true;
}

bool get_guid(std::span<const uint8_t> p_bytes, size_t p_at, Guid &r_out) {
	if (p_at > p_bytes.size() || p_bytes.size() - p_at < 16) {
		return false;
	}
	r_out = Guid::from_bytes(p_bytes.data() + p_at);
	return true;
}

std::optional<uint32_t> scalar_size(uint8_t p_code) {
	switch (p_code) {
		case kBoolean:
		case kInt8:
		case kUInt8:
			return 1;
		case kInt16:
		case kUInt16:
			return 2;
		case kClass:
		case kArray:
		case kCString:
		case kEnum:
		case kFileRef:
		case kInt32:
		case kUInt32:
		case kFloat32:
			return 4;
		case kInt64:
		case kUInt64:
		case kFloat64:
			return 8;
		case kGuid:
			return 16;
		case kSha1:
			return 20;
		default:
			return std::nullopt;
	}
}

struct Tables {
	size_t imports, names, fields, complexes, instance_repeaters, array_repeaters, end;
};

std::optional<Tables> table_layout(const std::array<uint32_t, 12> &p_header) {
	const uint64_t imports = p_header[3], instance_repeaters = p_header[5], complexes = p_header[6], fields = p_header[7],
				   names_len = p_header[8], arrays = p_header[10];
	// u32 counts times small record sizes cannot overflow u64.
	auto align = [](uint64_t p_at) { return (p_at + 15) & ~uint64_t(15); };
	Tables t{};
	const uint64_t imports_at = kHeaderLen;
	const uint64_t names_at = imports_at + imports * 32;
	const uint64_t fields_at = names_at + names_len;
	const uint64_t complexes_at = align(fields_at + fields * 16);
	const uint64_t instance_repeaters_at = complexes_at + complexes * 16;
	const uint64_t array_repeaters_at = align(instance_repeaters_at + instance_repeaters * 12);
	const uint64_t end = align(array_repeaters_at + arrays * 12);
	t.imports = static_cast<size_t>(imports_at);
	t.names = static_cast<size_t>(names_at);
	t.fields = static_cast<size_t>(fields_at);
	t.complexes = static_cast<size_t>(complexes_at);
	t.instance_repeaters = static_cast<size_t>(instance_repeaters_at);
	t.array_repeaters = static_cast<size_t>(array_repeaters_at);
	t.end = static_cast<size_t>(end);
	return t;
}

// Rust str::get(offset..) succeeds only on a char boundary.
bool char_boundary(std::span<const uint8_t> p_text, size_t p_at) {
	if (p_at == p_text.size()) {
		return true;
	}
	if (p_at > p_text.size()) {
		return false;
	}
	return (p_text[p_at] & 0xC0) != 0x80;
}

} // namespace

uint32_t ebx_name_hash(std::string_view p_name) {
	uint32_t hash = 5381;
	for (char c : p_name) {
		hash = (hash * 33u) ^ static_cast<uint8_t>(c);
	}
	return hash;
}

// ------------------------------------------------------------------------------------------------
// Parsing
// ------------------------------------------------------------------------------------------------

Result<EbxPartition> EbxPartition::parse(std::vector<uint8_t> p_bytes) {
	// The bytes move in first (a vector move keeps its buffer), so names resolve in error messages.
	EbxPartition p;
	p.bytes_ = std::move(p_bytes);
	const std::span<const uint8_t> bytes(p.bytes_);
	std::array<uint32_t, 12> header{};
	for (size_t i = 0; i < header.size(); ++i) {
		if (!get32(bytes, i * 4, header[i])) {
			return truncated(Section::Header);
		}
	}
	Guid guid, primary_guid;
	if (!get_guid(bytes, 0x30, guid) || !get_guid(bytes, 0x40, primary_guid)) {
		return truncated(Section::Header);
	}
	if (header[0] == kMagicBigEndian) {
		return make_error("big-endian (console) EBX partitions are not supported");
	}
	if (header[0] != kMagic) {
		return make_error("not an EBX partition (magic " + detail::hex_prefixed(header[0], 8) + ")");
	}
	const uint32_t meta_size = header[1], payload_size = header[2], import_count = header[3], instance_repeater_count = header[5],
				   complex_count = header[6], field_count = header[7], names_len = header[8], strings_len = header[9], array_count = header[10],
				   instance_payload_len = header[11];
	if (uint64_t(meta_size) + payload_size != bytes.size() || bytes.size() > std::numeric_limits<uint32_t>::max()) {
		return make_error("header sizes " + std::to_string(meta_size) + " + " + std::to_string(payload_size) +
				" do not add up to the partition length " + std::to_string(bytes.size()));
	}
	const std::optional<Tables> tables = table_layout(header);
	if (!tables) {
		return truncated(Section::Header);
	}
	if (tables->end != meta_size) {
		return make_error("descriptor tables end at " + std::to_string(tables->end) + ", header says the metadata ends at " + std::to_string(meta_size));
	}

	p.guid_ = guid;
	p.imports_.reserve(import_count);
	for (size_t i = 0; i < import_count; ++i) {
		const size_t at = tables->imports + i * 32;
		EbxImport imp;
		if (!get_guid(bytes, at, imp.partition) || !get_guid(bytes, at + 16, imp.instance)) {
			return truncated(Section::Imports);
		}
		p.imports_.push_back(imp);
	}

	if (tables->names + uint64_t(names_len) > bytes.size()) {
		return truncated(Section::Names);
	}
	const std::span<const uint8_t> names_region = bytes.subspan(tables->names, names_len);
	if (!detail::valid_utf8(names_region)) {
		return not_utf8(Section::Names);
	}
	p.names_at_ = tables->names;
	p.names_len_ = names_len;
	struct NameEntry {
		uint32_t hash, start, end;
	};
	std::vector<NameEntry> lookup;
	{
		size_t start = 0;
		for (size_t i = 0; i <= names_region.size(); ++i) {
			if (i == names_region.size() || names_region[i] == 0) {
				if (i > start) {
					const std::string_view piece(reinterpret_cast<const char *>(names_region.data()) + start, i - start);
					lookup.push_back({ ebx_name_hash(piece), static_cast<uint32_t>(start), static_cast<uint32_t>(i) });
				}
				start = i + 1;
			}
		}
	}
	std::sort(lookup.begin(), lookup.end(), [](const NameEntry &a, const NameEntry &b) { return a.hash < b.hash; });
	auto resolve_name = [&](uint32_t p_hash) -> const NameEntry * {
		const auto it = std::lower_bound(lookup.begin(), lookup.end(), p_hash, [](const NameEntry &e, uint32_t h) { return e.hash < h; });
		return (it != lookup.end() && it->hash == p_hash) ? &*it : nullptr;
	};

	p.fields_.reserve(field_count);
	for (size_t i = 0; i < field_count; ++i) {
		const size_t at = tables->fields + i * 16;
		uint32_t hash, offset;
		uint16_t flags, reference;
		if (!get32(bytes, at, hash) || !get16(bytes, at + 4, flags) || !get16(bytes, at + 6, reference) || !get32(bytes, at + 8, offset)) {
			return truncated(Section::Fields);
		}
		const NameEntry *name = resolve_name(hash);
		if (name == nullptr) {
			return unknown_name(hash);
		}
		p.fields_.push_back(Field{ name->start, name->end, hash, static_cast<uint8_t>((flags >> 4) & 0x1F), reference, static_cast<int32_t>(offset) });
	}

	p.complexes_.reserve(complex_count);
	for (size_t i = 0; i < complex_count; ++i) {
		const size_t at = tables->complexes + i * 16;
		uint32_t hash, field_start;
		uint16_t flags, size;
		if (!get32(bytes, at, hash) || !get32(bytes, at + 4, field_start) || at + 8 >= bytes.size() || !get16(bytes, at + 10, flags) ||
				!get16(bytes, at + 12, size)) {
			return truncated(Section::Complexes);
		}
		const uint8_t count = bytes[at + 8];
		const uint64_t field_end = uint64_t(field_start) + count;
		if (field_end > p.fields_.size()) {
			return bad_complex(static_cast<uint32_t>(i));
		}
		const NameEntry *name = resolve_name(hash);
		if (name == nullptr) {
			return unknown_name(hash);
		}
		p.complexes_.push_back(Complex{ name->start, name->end, hash, field_start, static_cast<uint32_t>(field_end),
				static_cast<uint8_t>((flags >> 4) & 0x1F), size, std::nullopt });
	}

	auto read_triples = [&](size_t p_at, uint32_t p_count, Section p_section, std::vector<std::array<uint32_t, 3>> &r_out) -> Status {
		r_out.reserve(p_count);
		for (size_t i = 0; i < p_count; ++i) {
			const size_t at = p_at + i * 12;
			std::array<uint32_t, 3> t{};
			if (!get32(bytes, at, t[0]) || !get32(bytes, at + 4, t[1]) || !get32(bytes, at + 8, t[2])) {
				return truncated(p_section);
			}
			r_out.push_back(t);
		}
		return Status::success();
	};
	std::vector<std::array<uint32_t, 3>> instance_repeaters, array_repeaters;
	BF3_TRY(read_triples(tables->instance_repeaters, instance_repeater_count, Section::InstanceRepeaters, instance_repeaters));
	BF3_TRY(read_triples(tables->array_repeaters, array_count, Section::ArrayRepeaters, array_repeaters));

	const size_t strings_at = meta_size;
	if (strings_at + uint64_t(strings_len) > bytes.size()) {
		return truncated(Section::Strings);
	}
	if (!detail::valid_utf8(bytes.subspan(strings_at, strings_len))) {
		return not_utf8(Section::Strings);
	}
	p.strings_at_ = strings_at;
	p.strings_len_ = strings_len;
	// Every NUL of the string table, so a string decodes in O(log n) whatever its length: many
	// fields may name one long string, and a scan per decode would cost fields x length.
	for (size_t at = 0; at < strings_len;) {
		const void *nul = std::memchr(bytes.data() + strings_at + at, 0, strings_len - at);
		if (nul == nullptr) {
			break;
		}
		const size_t found = static_cast<size_t>(static_cast<const uint8_t *>(nul) - (bytes.data() + strings_at));
		p.string_ends_.push_back(static_cast<uint32_t>(found));
		at = found + 1;
	}

	const size_t payload_at = strings_at + strings_len;
	const uint64_t payload_end = uint64_t(payload_at) + instance_payload_len;
	if (payload_end > bytes.size()) {
		return truncated(Section::Instances);
	}

	p.arrays_.reserve(array_repeaters.size());
	BF3_TRY(p.link_complexes());

	size_t at = payload_at;
	for (const auto &[internal, count, complex] : instance_repeaters) {
		Result<const Complex *> desc = p.expect_kind(complex, kClass);
		if (!desc) {
			return desc.error();
		}
		const size_t size = (*desc)->size;
		if (internal != 0) {
			return make_error("instances of `" + std::string(p.complex_name(complex)) + "` carry internal data");
		}
		for (uint32_t n = 0; n < count; ++n) {
			Guid g;
			if (!get_guid(bytes, at, g)) {
				return truncated(Section::Instances);
			}
			const size_t data = at + 16;
			at = data + size;
			if (at > payload_end) {
				return truncated(Section::Instances);
			}
			p.instances_.push_back(InstanceEntry{ g, complex, static_cast<uint32_t>(data) });
		}
	}

	const size_t arrays_at = static_cast<size_t>(payload_end);
	for (size_t index = 0; index < array_repeaters.size(); ++index) {
		const auto &[offset, len, complex] = array_repeaters[index];
		Result<uint32_t> member = p.array_member(complex);
		if (!member) {
			return member.error();
		}
		const uint64_t stride = p.stride(*member);
		const uint64_t start = uint64_t(arrays_at) + offset;
		const uint64_t end = start + uint64_t(len) * stride;
		if (end > bytes.size()) {
			return truncated(Section::Arrays);
		}
		if (len != 0) {
			uint32_t stored = 0;
			if (start < 4 || !get32(bytes, static_cast<size_t>(start - 4), stored)) {
				return truncated(Section::Arrays);
			}
			if (stored != len) {
				return make_error("array " + std::to_string(index) + " has " + std::to_string(len) + " elements but " + std::to_string(stored) +
						" is stored before its data");
			}
		}
		p.arrays_.push_back(ArrayEntry{ static_cast<uint32_t>(start), len, complex });
	}

	const auto primary = std::find_if(p.instances_.begin(), p.instances_.end(), [&](const InstanceEntry &e) { return e.guid == primary_guid; });
	if (primary == p.instances_.end()) {
		return make_error("primary instance " + primary_guid.to_string() + " is not in the partition");
	}
	p.primary_ = static_cast<uint32_t>(primary - p.instances_.begin());
	p.by_guid_.resize(p.instances_.size());
	for (uint32_t i = 0; i < p.by_guid_.size(); ++i) {
		p.by_guid_[i] = i;
	}
	std::sort(p.by_guid_.begin(), p.by_guid_.end(), [&](uint32_t a, uint32_t b) { return p.instances_[a].guid < p.instances_[b].guid; });
	BF3_TRY(p.validate());
	return p;
}

Result<const EbxPartition::Complex *> EbxPartition::expect_kind(uint32_t p_complex, uint8_t p_code) const {
	if (p_complex >= complexes_.size()) {
		return bad_complex(p_complex);
	}
	const Complex &desc = complexes_[p_complex];
	if (desc.code != p_code) {
		return unexpected_kind(name(desc.name_start, desc.name_end), desc.code);
	}
	return &desc;
}

Result<uint32_t> EbxPartition::array_member(uint32_t p_complex) const {
	Result<const Complex *> desc = expect_kind(p_complex, kArray);
	if (!desc) {
		return desc.error();
	}
	if ((*desc)->field_start == (*desc)->field_end) {
		return unexpected_kind(name((*desc)->name_start, (*desc)->name_end), kArray);
	}
	return (*desc)->field_start;
}

Error EbxPartition::unsupported(uint32_t p_complex, uint32_t p_field) const {
	return make_error("field `" + std::string(complex_name(p_complex)) + "." + std::string(field_name(p_field)) + "` has unsupported type code " +
			std::to_string(fields_[p_field].code));
}

Error EbxPartition::out_of_bounds(uint32_t p_complex, uint32_t p_field) const {
	return make_error("field `" + std::string(complex_name(p_complex)) + "." + std::string(field_name(p_field)) + "` lies outside its type");
}

Status EbxPartition::link_complexes() {
	for (uint32_t complex = 0; complex < complexes_.size(); ++complex) {
		switch (complexes_[complex].code) {
			case kClass:
			case kValueType: {
				Result<std::optional<uint32_t>> base = check_members(complex);
				if (!base) {
					return base.error();
				}
				complexes_[complex].base = *base;
				break;
			}
			case kArray: {
				Result<uint32_t> member = array_member(complex);
				if (!member) {
					return member.error();
				}
				BF3_TRY(check_element(complex, *member));
				break;
			}
			default:
				break;
		}
	}
	for (size_t complex = 0; complex < complexes_.size(); ++complex) {
		std::optional<uint32_t> current = complexes_[complex].base;
		size_t depth = 0;
		while (current) {
			++depth;
			if (depth >= kBaseDepth) {
				return too_deep();
			}
			current = complexes_[*current].base;
		}
	}
	return Status::success();
}

Result<std::optional<uint32_t>> EbxPartition::check_members(uint32_t p_complex) const {
	const Complex &desc = complexes_[p_complex];
	std::optional<uint32_t> base;
	for (uint32_t field = desc.field_start; field < desc.field_end; ++field) {
		const Field &member = fields_[field];
		uint32_t size = 0;
		switch (member.code) {
			case kVoid: {
				if (base) {
					return make_error("type `" + std::string(name(desc.name_start, desc.name_end)) + "` has more than one base");
				}
				base = member.reference;
				Result<const Complex *> b = expect_kind(member.reference, desc.code);
				if (!b) {
					return b.error();
				}
				size = (*b)->size;
				break;
			}
			case kValueType: {
				Result<const Complex *> v = expect_kind(member.reference, kValueType);
				if (!v) {
					return v.error();
				}
				size = (*v)->size;
				break;
			}
			case kEnum: {
				Result<const Complex *> e = expect_kind(member.reference, kEnum);
				if (!e) {
					return e.error();
				}
				size = 4;
				break;
			}
			default: {
				const std::optional<uint32_t> s = scalar_size(member.code);
				if (!s) {
					return unsupported(p_complex, field);
				}
				size = *s;
				break;
			}
		}
		if (member.offset < 0 || uint64_t(uint32_t(member.offset)) + size > desc.size) {
			return out_of_bounds(p_complex, field);
		}
	}
	return base;
}

Status EbxPartition::check_element(uint32_t p_complex, uint32_t p_member) {
	const Field &desc = fields_[p_member];
	switch (desc.code) {
		case kValueType: {
			Result<const Complex *> v = expect_kind(desc.reference, kValueType);
			if (!v) {
				return v.error();
			}
			return Status::success();
		}
		case kEnum:
			// An enum element names no enum type (its reference is 0), so it has no names.
			fields_[p_member].reference = kNoType;
			return Status::success();
		default:
			if (scalar_size(desc.code)) {
				return Status::success();
			}
			return unsupported(p_complex, p_member);
	}
}

// Checks every reference, array index and string offset once, so decoding never meets a bad one.
// Arrays are visited once each; the budget bounds descriptors that overlap on purpose.
class EbxValidator {
public:
	explicit EbxValidator(const EbxPartition &p_partition) :
			p_(p_partition), arrays_(p_partition.arrays_.size(), Visit::New) {
		const size_t len = p_partition.bytes_.size();
		budget_ = (len > (std::numeric_limits<size_t>::max() - 4096) / 16) ? std::numeric_limits<size_t>::max() : len * 16 + 4096;
	}

	Status object(uint32_t p_complex, size_t p_at, size_t p_depth) {
		BF3_TRY(spend(p_depth));
		const EbxPartition::Complex &desc = p_.complexes_[p_complex];
		for (uint32_t field = desc.field_start; field < desc.field_end; ++field) {
			const EbxPartition::Field &member = p_.fields_[field];
			const size_t field_at = p_at + static_cast<size_t>(member.offset);
			if (member.code == kVoid) {
				BF3_TRY(object(member.reference, field_at, p_depth + 1));
			} else {
				BF3_TRY(value(field, field_at, p_depth + 1));
			}
		}
		return Status::success();
	}

	Status array(uint32_t p_index, size_t p_depth) {
		if (p_index >= arrays_.size()) {
			return bad_array(p_index);
		}
		if (arrays_[p_index] == Visit::Done) {
			return Status::success();
		}
		if (arrays_[p_index] == Visit::Open) {
			return make_error("array " + std::to_string(p_index) + " contains itself");
		}
		BF3_TRY(spend(p_depth));
		const EbxPartition::ArrayEntry &entry = p_.arrays_[p_index];
		const uint32_t member = p_.complexes_[entry.complex].field_start;
		const uint8_t code = p_.fields_[member].code;
		if (code == kValueType || code == kClass || code == kArray || code == kCString || code == kFileRef) {
			arrays_[p_index] = Visit::Open;
			const size_t stride = p_.stride(member);
			for (size_t i = 0; i < entry.len; ++i) {
				BF3_TRY(value(member, entry.start + i * stride, p_depth + 1));
			}
		}
		arrays_[p_index] = Visit::Done;
		return Status::success();
	}

private:
	enum class Visit : uint8_t { New, Open, Done };

	Status spend(size_t p_depth) {
		if (p_depth > kNestDepth) {
			return too_deep();
		}
		if (budget_ == 0) {
			return overlapping();
		}
		--budget_;
		return Status::success();
	}

	Status value(uint32_t p_field, size_t p_at, size_t p_depth) {
		const EbxPartition::Field &member = p_.fields_[p_field];
		auto word = [&](uint32_t &r_out) { return get32(p_.bytes_, p_at, r_out); };
		switch (member.code) {
			case kValueType:
				return object(member.reference, p_at, p_depth);
			case kClass: {
				BF3_TRY(spend(p_depth));
				uint32_t raw;
				if (!word(raw)) {
					return truncated(Section::Instances);
				}
				return p_.reference(raw) ? Status::success() : Status(bad_reference(raw));
			}
			case kArray: {
				uint32_t index;
				if (!word(index)) {
					return truncated(Section::Instances);
				}
				return array(index, p_depth);
			}
			case kCString:
			case kFileRef: {
				BF3_TRY(spend(p_depth));
				uint32_t offset;
				if (!word(offset)) {
					return truncated(Section::Instances);
				}
				return p_.string(offset) ? Status::success() : Status(bad_string(offset));
			}
			default:
				return Status::success();
		}
	}

	const EbxPartition &p_;
	std::vector<Visit> arrays_;
	size_t budget_ = 0;
};

Status EbxPartition::validate() const {
	EbxValidator validator(*this);
	for (const InstanceEntry &entry : instances_) {
		BF3_TRY(validator.object(entry.complex, entry.offset, 0));
	}
	for (uint32_t index = 0; index < arrays_.size(); ++index) {
		BF3_TRY(validator.array(index, 0));
	}
	return Status::success();
}

// ------------------------------------------------------------------------------------------------
// Partition accessors
// ------------------------------------------------------------------------------------------------

std::optional<EbxInstance> EbxPartition::instance(size_t p_index) const {
	if (p_index >= instances_.size()) {
		return std::nullopt;
	}
	return EbxInstance(this, static_cast<uint32_t>(p_index));
}

std::vector<EbxInstance> EbxPartition::instances_of(std::string_view p_type_name) const {
	const uint32_t hash = ebx_name_hash(p_type_name);
	std::vector<EbxInstance> out;
	for (uint32_t i = 0; i < instances_.size(); ++i) {
		if (derives_from(instances_[i].complex, hash, p_type_name)) {
			out.emplace_back(this, i);
		}
	}
	return out;
}

std::optional<EbxInstance> EbxPartition::instance_by_guid(const Guid &p_guid) const {
	const auto it = std::lower_bound(by_guid_.begin(), by_guid_.end(), p_guid, [&](uint32_t i, const Guid &g) { return instances_[i].guid < g; });
	if (it == by_guid_.end() || instances_[*it].guid != p_guid) {
		return std::nullopt;
	}
	return EbxInstance(this, *it);
}

size_t EbxPartition::duplicate_instance_guids() const {
	size_t n = 0;
	for (size_t i = 1; i < by_guid_.size(); ++i) {
		n += instances_[by_guid_[i]].guid == instances_[by_guid_[i - 1]].guid ? 1 : 0;
	}
	return n;
}

std::optional<EbxInstance> EbxPartition::resolve(const EbxRef &p_ref) const {
	switch (p_ref.kind) {
		case EbxRef::Kind::Internal:
			return instance(p_ref.index);
		case EbxRef::Kind::External:
			return p_ref.partition == guid_ ? instance_by_guid(p_ref.instance) : std::nullopt;
		default:
			return std::nullopt;
	}
}

std::string_view EbxPartition::name(uint32_t p_start, uint32_t p_end) const {
	if (p_start > p_end || p_end > names_len_) {
		return {};
	}
	return std::string_view(reinterpret_cast<const char *>(bytes_.data()) + names_at_ + p_start, p_end - p_start);
}

std::string_view EbxPartition::complex_name(uint32_t p_complex) const {
	return p_complex < complexes_.size() ? name(complexes_[p_complex].name_start, complexes_[p_complex].name_end) : std::string_view();
}

std::string_view EbxPartition::field_name(uint32_t p_field) const {
	return p_field < fields_.size() ? name(fields_[p_field].name_start, fields_[p_field].name_end) : std::string_view();
}

bool EbxPartition::derives_from(uint32_t p_complex, uint32_t p_hash, std::string_view p_type_name) const {
	std::optional<uint32_t> current = p_complex;
	for (size_t i = 0; i < kBaseDepth; ++i) {
		if (!current || *current >= complexes_.size()) {
			return false;
		}
		const Complex &desc = complexes_[*current];
		if (desc.hash == p_hash && name(desc.name_start, desc.name_end) == p_type_name) {
			return true;
		}
		current = desc.base;
	}
	return false;
}

std::optional<std::optional<std::string_view>> EbxPartition::string(uint32_t p_offset) const {
	if (p_offset == kNullString) {
		return std::optional<std::string_view>();
	}
	const std::span<const uint8_t> table = std::span<const uint8_t>(bytes_).subspan(strings_at_, strings_len_);
	if (!char_boundary(table, p_offset)) {
		return std::nullopt;
	}
	const auto nul = std::lower_bound(string_ends_.begin(), string_ends_.end(), p_offset);
	if (nul == string_ends_.end()) {
		return std::nullopt;
	}
	const size_t len = *nul - p_offset;
	return std::optional<std::string_view>(std::string_view(reinterpret_cast<const char *>(table.data()) + p_offset, len));
}

std::optional<EbxRef> EbxPartition::reference(uint32_t p_raw) const {
	if (p_raw == 0) {
		return EbxRef::null();
	}
	if (p_raw & kExternal) {
		const uint32_t index = p_raw & ~kExternal;
		if (index >= imports_.size()) {
			return std::nullopt;
		}
		return EbxRef::external(imports_[index].partition, imports_[index].instance);
	}
	if (p_raw > instances_.size()) {
		return std::nullopt;
	}
	return EbxRef::internal(p_raw - 1);
}

std::optional<std::string_view> EbxPartition::enumerator(uint32_t p_complex, int32_t p_value) const {
	if (p_complex >= complexes_.size() || complexes_[p_complex].code != kEnum) {
		return std::nullopt;
	}
	const Complex &desc = complexes_[p_complex];
	for (uint32_t f = desc.field_start; f < desc.field_end; ++f) {
		if (fields_[f].offset == p_value) {
			return name(fields_[f].name_start, fields_[f].name_end);
		}
	}
	return std::nullopt;
}

uint32_t EbxPartition::stride(uint32_t p_field) const {
	const Field &desc = fields_[p_field];
	if (desc.code == kValueType) {
		return complexes_[desc.reference].size;
	}
	return scalar_size(desc.code).value_or(0);
}

std::optional<EbxValue> EbxPartition::decode(uint32_t p_field, size_t p_at) const {
	if (p_field >= fields_.size()) {
		return std::nullopt;
	}
	const Field &member = fields_[p_field];
	const std::span<const uint8_t> b(bytes_);
	uint32_t w = 0;
	uint16_t h = 0;
	uint64_t q = 0;
	switch (member.code) {
		case kValueType:
			return EbxValue::make_struct(EbxStruct(this, member.reference, static_cast<uint32_t>(p_at)));
		case kClass: {
			if (!get32(b, p_at, w)) {
				return std::nullopt;
			}
			const std::optional<EbxRef> r = reference(w);
			if (!r) {
				return std::nullopt;
			}
			return EbxValue::make_ref(*r);
		}
		case kArray: {
			if (!get32(b, p_at, w) || w >= arrays_.size()) {
				return std::nullopt;
			}
			const ArrayEntry &entry = arrays_[w];
			if (entry.complex >= complexes_.size()) {
				return std::nullopt;
			}
			const uint32_t element = complexes_[entry.complex].field_start;
			return EbxValue::make_array(EbxArray(this, element, entry.start, entry.len, stride(element)));
		}
		case kCString:
		case kFileRef: {
			if (!get32(b, p_at, w)) {
				return std::nullopt;
			}
			const std::optional<std::optional<std::string_view>> s = string(w);
			if (!s) {
				return std::nullopt;
			}
			return EbxValue::make_string(*s);
		}
		case kEnum: {
			if (!get32(b, p_at, w)) {
				return std::nullopt;
			}
			const int32_t value = static_cast<int32_t>(w);
			return EbxValue::make_enum(value, enumerator(member.reference, value));
		}
		case kBoolean:
			return p_at < b.size() ? std::optional<EbxValue>(EbxValue::make_bool(b[p_at] != 0)) : std::nullopt;
		case kInt8:
			return p_at < b.size() ? std::optional<EbxValue>(EbxValue::make_int(EbxValue::Kind::I8, static_cast<int8_t>(b[p_at]))) : std::nullopt;
		case kUInt8:
			return p_at < b.size() ? std::optional<EbxValue>(EbxValue::make_u64(EbxValue::Kind::U8, b[p_at])) : std::nullopt;
		case kInt16:
			return get16(b, p_at, h) ? std::optional<EbxValue>(EbxValue::make_int(EbxValue::Kind::I16, static_cast<int16_t>(h))) : std::nullopt;
		case kUInt16:
			return get16(b, p_at, h) ? std::optional<EbxValue>(EbxValue::make_u64(EbxValue::Kind::U16, h)) : std::nullopt;
		case kInt32:
			return get32(b, p_at, w) ? std::optional<EbxValue>(EbxValue::make_int(EbxValue::Kind::I32, static_cast<int32_t>(w))) : std::nullopt;
		case kUInt32:
			return get32(b, p_at, w) ? std::optional<EbxValue>(EbxValue::make_u64(EbxValue::Kind::U32, w)) : std::nullopt;
		case kInt64:
			return get64(b, p_at, q) ? std::optional<EbxValue>(EbxValue::make_int(EbxValue::Kind::I64, static_cast<int64_t>(q))) : std::nullopt;
		case kUInt64:
			return get64(b, p_at, q) ? std::optional<EbxValue>(EbxValue::make_u64(EbxValue::Kind::U64, q)) : std::nullopt;
		case kFloat32: {
			if (!get32(b, p_at, w)) {
				return std::nullopt;
			}
			float f;
			std::memcpy(&f, &w, 4);
			return EbxValue::make_f32(f);
		}
		case kFloat64: {
			if (!get64(b, p_at, q)) {
				return std::nullopt;
			}
			double d;
			std::memcpy(&d, &q, 8);
			return EbxValue::make_f64(d);
		}
		case kGuid: {
			Guid g;
			return get_guid(b, p_at, g) ? std::optional<EbxValue>(EbxValue::make_guid(g)) : std::nullopt;
		}
		case kSha1:
			if (p_at > b.size() || b.size() - p_at < 20) {
				return std::nullopt;
			}
			return EbxValue::make_sha1(Sha1::from_bytes(b.data() + p_at));
		default:
			return std::nullopt;
	}
}

// ------------------------------------------------------------------------------------------------
// Values
// ------------------------------------------------------------------------------------------------

EbxValue EbxValue::make_bool(bool p_v) {
	EbxValue v;
	v.kind_ = Kind::Bool;
	v.num_.u = p_v ? 1 : 0;
	return v;
}
EbxValue EbxValue::make_int(Kind p_kind, int64_t p_v) {
	EbxValue v;
	v.kind_ = p_kind;
	v.num_.i = p_v;
	return v;
}
EbxValue EbxValue::make_u64(Kind p_kind, uint64_t p_v) {
	EbxValue v;
	v.kind_ = p_kind;
	v.num_.u = p_v;
	return v;
}
EbxValue EbxValue::make_f32(float p_v) {
	EbxValue v;
	v.kind_ = Kind::F32;
	v.num_.f32 = p_v;
	return v;
}
EbxValue EbxValue::make_f64(double p_v) {
	EbxValue v;
	v.kind_ = Kind::F64;
	v.num_.f64 = p_v;
	return v;
}
EbxValue EbxValue::make_guid(const Guid &p_v) {
	EbxValue v;
	v.kind_ = Kind::Guid;
	v.guid_ = p_v;
	return v;
}
EbxValue EbxValue::make_sha1(const Sha1 &p_v) {
	EbxValue v;
	v.kind_ = Kind::Sha1;
	v.sha1_ = p_v;
	return v;
}
EbxValue EbxValue::make_string(std::optional<std::string_view> p_v) {
	EbxValue v;
	v.kind_ = Kind::String;
	v.has_text_ = p_v.has_value();
	v.text_ = p_v.value_or(std::string_view());
	return v;
}
EbxValue EbxValue::make_enum(int32_t p_value, std::optional<std::string_view> p_name) {
	EbxValue v;
	v.kind_ = Kind::Enum;
	v.num_.i = p_value;
	v.has_text_ = p_name.has_value();
	v.text_ = p_name.value_or(std::string_view());
	return v;
}
EbxValue EbxValue::make_ref(const EbxRef &p_v) {
	EbxValue v;
	v.kind_ = Kind::Ref;
	v.ref_ = p_v;
	return v;
}
EbxValue EbxValue::make_struct(const EbxStruct &p_v) {
	EbxValue v;
	v.kind_ = Kind::Struct;
	v.struct_ = p_v;
	return v;
}
EbxValue EbxValue::make_array(const EbxArray &p_v) {
	EbxValue v;
	v.kind_ = Kind::Array;
	v.array_ = p_v;
	return v;
}

std::optional<bool> EbxValue::as_bool() const {
	return kind_ == Kind::Bool ? std::optional<bool>(num_.u != 0) : std::nullopt;
}

std::optional<int64_t> EbxValue::as_i64() const {
	switch (kind_) {
		case Kind::I8:
		case Kind::I16:
		case Kind::I32:
		case Kind::I64:
		case Kind::Enum:
			return num_.i;
		case Kind::U8:
		case Kind::U16:
		case Kind::U32:
			return static_cast<int64_t>(num_.u);
		case Kind::U64:
			return num_.u <= uint64_t(std::numeric_limits<int64_t>::max()) ? std::optional<int64_t>(static_cast<int64_t>(num_.u)) : std::nullopt;
		default:
			return std::nullopt;
	}
}

std::optional<uint64_t> EbxValue::as_u64() const {
	if (kind_ == Kind::U64) {
		return num_.u;
	}
	const std::optional<int64_t> v = as_i64();
	return (v && *v >= 0) ? std::optional<uint64_t>(static_cast<uint64_t>(*v)) : std::nullopt;
}

std::optional<int32_t> EbxValue::as_i32() const {
	const std::optional<int64_t> v = as_i64();
	return (v && *v >= std::numeric_limits<int32_t>::min() && *v <= std::numeric_limits<int32_t>::max()) ? std::optional<int32_t>(static_cast<int32_t>(*v))
																										   : std::nullopt;
}

std::optional<uint32_t> EbxValue::as_u32() const {
	const std::optional<int64_t> v = as_i64();
	return (v && *v >= 0 && *v <= std::numeric_limits<uint32_t>::max()) ? std::optional<uint32_t>(static_cast<uint32_t>(*v)) : std::nullopt;
}

std::optional<float> EbxValue::as_f32() const {
	if (kind_ == Kind::F32) {
		return num_.f32;
	}
	if (kind_ == Kind::F64) {
		return static_cast<float>(num_.f64);
	}
	return std::nullopt;
}

std::optional<double> EbxValue::as_f64() const {
	if (kind_ == Kind::F32) {
		return static_cast<double>(num_.f32);
	}
	if (kind_ == Kind::F64) {
		return num_.f64;
	}
	return std::nullopt;
}

std::optional<std::string_view> EbxValue::as_str() const {
	return kind_ == Kind::String ? std::optional<std::string_view>(text_) : std::nullopt;
}

std::optional<std::string_view> EbxValue::enum_name() const {
	return (kind_ == Kind::Enum && has_text_) ? std::optional<std::string_view>(text_) : std::nullopt;
}

std::optional<int32_t> EbxValue::enum_value() const {
	return kind_ == Kind::Enum ? std::optional<int32_t>(static_cast<int32_t>(num_.i)) : std::nullopt;
}

std::optional<Guid> EbxValue::as_guid() const {
	return kind_ == Kind::Guid ? std::optional<Guid>(guid_) : std::nullopt;
}

std::optional<Sha1> EbxValue::as_sha1() const {
	return kind_ == Kind::Sha1 ? std::optional<Sha1>(sha1_) : std::nullopt;
}

std::optional<EbxRef> EbxValue::as_ref() const {
	return kind_ == Kind::Ref ? std::optional<EbxRef>(ref_) : std::nullopt;
}

std::optional<EbxStruct> EbxValue::as_struct() const {
	return kind_ == Kind::Struct ? std::optional<EbxStruct>(struct_) : std::nullopt;
}

OptionalEbxArray EbxValue::as_array() const {
	return kind_ == Kind::Array ? OptionalEbxArray(array_) : OptionalEbxArray();
}

bool EbxValue::is_null() const {
	return (kind_ == Kind::Ref && ref_.is_null()) || (kind_ == Kind::String && !has_text_);
}

std::optional<EbxValue> EbxValue::get(std::string_view p_field) const {
	const std::optional<EbxStruct> s = as_struct();
	return s ? s->get(p_field) : std::nullopt;
}

std::optional<EbxValue> EbxValue::get_path(std::string_view p_path) const {
	EbxValue value = *this;
	size_t pos = 0;
	while (true) {
		const size_t dot = std::min(p_path.find('.', pos), p_path.size());
		std::string_view segment = p_path.substr(pos, dot - pos);
		const size_t split = std::min(segment.find('['), segment.size());
		const std::string_view name = segment.substr(0, split);
		std::string_view rest = segment.substr(split);
		if (!name.empty()) {
			std::optional<EbxValue> next = value.get(name);
			if (!next) {
				return std::nullopt;
			}
			value = *next;
		}
		while (!rest.empty() && rest.front() == '[') {
			const std::string_view inner = rest.substr(1);
			const size_t close = inner.find(']');
			if (close == std::string_view::npos) {
				return std::nullopt;
			}
			size_t index = 0;
			const std::string_view digits = inner.substr(0, close);
			const auto res = std::from_chars(digits.data(), digits.data() + digits.size(), index);
			if (digits.empty() || res.ec != std::errc() || res.ptr != digits.data() + digits.size()) {
				return std::nullopt;
			}
			const OptionalEbxArray array = value.as_array();
			if (!array) {
				return std::nullopt;
			}
			std::optional<EbxValue> element = array->get(index);
			if (!element) {
				return std::nullopt;
			}
			value = *element;
			rest = inner.substr(close + 1);
		}
		if (!rest.empty()) {
			return std::nullopt;
		}
		if (dot >= p_path.size()) {
			return value;
		}
		pos = dot + 1;
	}
}

std::optional<std::array<float, 3>> EbxValue::as_vec3() const {
	const std::optional<EbxStruct> s = as_struct();
	return s ? s->vec3() : std::nullopt;
}

std::optional<std::array<std::array<float, 3>, 4>> EbxValue::as_transform() const {
	const std::optional<EbxStruct> s = as_struct();
	return s ? s->transform() : std::nullopt;
}

// --- EbxStruct -----------------------------------------------------------------------------------

std::string_view EbxStruct::type_name() const {
	return partition_->complex_name(complex_);
}

std::vector<std::string_view> EbxStruct::type_chain() const {
	std::vector<std::string_view> out;
	std::optional<uint32_t> next = complex_;
	for (size_t remaining = EbxPartition::kBaseDepth; next && remaining > 0; --remaining) {
		if (*next >= partition_->complex_descs().size()) {
			break;
		}
		const EbxPartition::Complex &desc = partition_->complex_descs()[*next];
		out.push_back(partition_->name(desc.name_start, desc.name_end));
		next = desc.base;
	}
	return out;
}

bool EbxStruct::is_a(std::string_view p_type_name) const {
	return partition_->derives_from(complex_, ebx_name_hash(p_type_name), p_type_name);
}

std::optional<EbxValue> EbxStruct::get(std::string_view p_field) const {
	const uint32_t hash = ebx_name_hash(p_field);
	const EbxPartition &p = *partition_;
	uint32_t complex = complex_;
	size_t at = offset_;
	for (size_t depth = 0; depth < EbxPartition::kBaseDepth; ++depth) {
		if (complex >= p.complex_descs().size()) {
			return std::nullopt;
		}
		const EbxPartition::Complex &desc = p.complex_descs()[complex];
		std::optional<std::pair<uint32_t, size_t>> base;
		for (uint32_t index = desc.field_start; index < desc.field_end; ++index) {
			const EbxPartition::Field &member = p.field_descs()[index];
			const size_t member_at = at + static_cast<size_t>(static_cast<int64_t>(member.offset));
			if (member.code == kVoid) {
				base = std::make_pair(static_cast<uint32_t>(member.reference), member_at);
			} else if (member.hash == hash && p.name(member.name_start, member.name_end) == p_field) {
				return p.decode(index, member_at);
			}
		}
		if (!base) {
			return std::nullopt;
		}
		complex = base->first;
		at = base->second;
	}
	return std::nullopt;
}

std::optional<EbxValue> EbxStruct::get_path(std::string_view p_path) const {
	return EbxValue::make_struct(*this).get_path(p_path);
}

EbxStruct::FieldRange EbxStruct::fields() const {
	return FieldRange{ EbxFieldIterator(partition_, complex_, offset_) };
}

std::optional<std::array<float, 3>> EbxStruct::vec3() const {
	const std::optional<EbxValue> x = get("x"), y = get("y"), z = get("z");
	if (!x || !y || !z) {
		return std::nullopt;
	}
	const std::optional<float> fx = x->as_f32(), fy = y->as_f32(), fz = z->as_f32();
	if (!fx || !fy || !fz) {
		return std::nullopt;
	}
	return std::array<float, 3>{ *fx, *fy, *fz };
}

std::optional<std::array<std::array<float, 3>, 4>> EbxStruct::transform() const {
	std::array<std::array<float, 3>, 4> out{};
	const char *rows[4] = { "right", "up", "forward", "trans" };
	for (size_t i = 0; i < 4; ++i) {
		const std::optional<EbxValue> row = get(rows[i]);
		if (!row) {
			return std::nullopt;
		}
		const std::optional<std::array<float, 3>> v = row->as_vec3();
		if (!v) {
			return std::nullopt;
		}
		out[i] = *v;
	}
	return out;
}

// --- EbxFieldIterator ----------------------------------------------------------------------------

EbxFieldIterator::EbxFieldIterator(const EbxPartition *p_partition, uint32_t p_complex, uint32_t p_offset) :
		partition_(p_partition) {
	std::optional<std::pair<uint32_t, uint32_t>> next = std::make_pair(p_complex, p_offset);
	while (next) {
		if (depth_ == kChain) {
			break;
		}
		chain_[depth_++] = *next;
		const EbxPartition::Complex &desc = partition_->complex_descs()[next->first];
		std::optional<std::pair<uint32_t, uint32_t>> base;
		for (uint32_t f = desc.field_start; f < desc.field_end; ++f) {
			const EbxPartition::Field &member = partition_->field_descs()[f];
			if (member.code == kVoid) {
				base = std::make_pair(static_cast<uint32_t>(member.reference), next->second + static_cast<uint32_t>(member.offset));
				break;
			}
		}
		next = base;
	}
	done_ = false;
	field_ = field_end_ = 0;
	advance();
}

void EbxFieldIterator::advance() {
	for (;;) {
		if (field_ < field_end_) {
			const uint32_t index = field_++;
			const EbxPartition::Field &member = partition_->field_descs()[index];
			if (member.code == kVoid) {
				continue;
			}
			const size_t at = size_t(at_) + static_cast<size_t>(static_cast<int64_t>(member.offset));
			std::optional<EbxValue> value = partition_->decode(index, at);
			if (value) {
				current_ = EbxField{ partition_->name(member.name_start, member.name_end), *value };
				return;
			}
			continue;
		}
		if (depth_ == 0) {
			done_ = true;
			return;
		}
		--depth_;
		const auto [complex, at] = chain_[depth_];
		const EbxPartition::Complex &desc = partition_->complex_descs()[complex];
		field_ = desc.field_start;
		field_end_ = desc.field_end;
		at_ = at;
	}
}

// --- EbxArray / EbxInstance -----------------------------------------------------------------------

std::optional<EbxValue> EbxArray::get(size_t p_index) const {
	if (p_index >= len_) {
		return std::nullopt;
	}
	return partition_->decode(member_, size_t(start_) + p_index * stride_);
}

EbxArrayIterator EbxArray::begin() const {
	return EbxArrayIterator(*this, 0);
}

EbxArrayIterator EbxArray::end() const {
	return EbxArrayIterator(*this, len_);
}

Guid EbxInstance::guid() const {
	return partition_->instance_entries()[index_].guid;
}

bool EbxInstance::is_primary() const {
	return guid() == partition_->primary_guid();
}

EbxStruct EbxInstance::data() const {
	const EbxPartition::InstanceEntry &entry = partition_->instance_entries()[index_];
	return EbxStruct(partition_, entry.complex, entry.offset);
}

std::string_view EbxInstance::type_name() const {
	return partition_->complex_name(partition_->instance_entries()[index_].complex);
}

EbxStruct::FieldRange EbxInstance::fields() const {
	return data().fields();
}

} // namespace bf3::core
